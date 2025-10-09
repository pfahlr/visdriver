#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <xmmintrin.h>

#include <windows.h>
#include <shellapi.h>

#ifndef __DROPFILES_DEFINED
typedef struct _DROPFILES {
  DWORD pFiles;
  POINT pt;
  BOOL fNC;
  BOOL fWide;
} DROPFILES, *LPDROPFILES;
#define __DROPFILES_DEFINED
#endif

#include <winamp/out.h>
#include <winamp/wa_ipc.h>

#include "avi_writer.hpp"
#include "capture.hpp"
#include "debug_trace.hpp"
#include "manifest.hpp"
#include "spectrum.hpp"
#include "sha256.hpp"
#include "vis_host.hpp"
#include "wav_reader.hpp"

namespace {

constexpr wchar_t kParentWindowClassName[] = L"visdriver.avs.parent";
constexpr wchar_t kChildWindowClassName[] = L"visdriver.avs.child";
constexpr int kWaveformSamples = 576;
constexpr int kSpectrumFftSize = 1024;

int g_total_pcm_samples = 0;

HWND g_parent_window = nullptr;
HWND g_child_container_window = nullptr;
HWND g_embedded_vis_window = nullptr;

bool g_force_diagnostics_fallback = false;

int g_target_width = 0;
int g_target_height = 0;

bool HasValidEmbeddedVisWindow();
void StopTrackingEmbeddedVisWindow();
HWND GetEmbeddedWindowContainer();
void ResizeEmbeddedWindow(HWND container);

std::wstring GetWindowClassName(HWND hwnd) {
  if (hwnd == nullptr) {
    return L"(null)";
  }
  wchar_t buffer[256];
  const int length = GetClassNameW(
      hwnd, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
  if (length <= 0) {
    return L"(unknown)";
  }
  return std::wstring(buffer, buffer + length);
}

bool ShouldReplaceTrackedWindow(HWND candidate) {
  if (candidate == nullptr) {
    return false;
  }
  if (HasValidEmbeddedVisWindow() && candidate == g_embedded_vis_window) {
    return false;
  }
  if (HasValidEmbeddedVisWindow() &&
      IsChild(g_embedded_vis_window, candidate)) {
    return true;
  }
  if (IsWindow(candidate) == FALSE) {
    return false;
  }

  const LONG_PTR candidate_style = GetWindowLongPtrW(candidate, GWL_STYLE);
  if ((candidate_style & WS_CHILD) == 0) {
    return false;
  }

  RECT candidate_rect{0, 0, 0, 0};
  GetClientRect(candidate, &candidate_rect);
  const int candidate_width = candidate_rect.right - candidate_rect.left;
  const int candidate_height = candidate_rect.bottom - candidate_rect.top;
  const int candidate_area = candidate_width * candidate_height;

  if (!HasValidEmbeddedVisWindow()) {
    return candidate_area >= 0;
  }

  RECT current_rect{0, 0, 0, 0};
  GetClientRect(g_embedded_vis_window, &current_rect);
  const int current_width = current_rect.right - current_rect.left;
  const int current_height = current_rect.bottom - current_rect.top;
  const int current_area = current_width * current_height;

  const LONG_PTR current_style =
      GetWindowLongPtrW(g_embedded_vis_window, GWL_STYLE);
  const bool current_visible = (current_style & WS_VISIBLE) != 0;
  const bool candidate_visible = (candidate_style & WS_VISIBLE) != 0;

  const LONG_PTR candidate_class_style =
      GetClassLongPtrW(candidate, GCL_STYLE);
  const LONG_PTR current_class_style =
      GetClassLongPtrW(g_embedded_vis_window, GCL_STYLE);
  const bool candidate_has_own_dc = (candidate_class_style & CS_OWNDC) != 0;
  const bool current_has_own_dc = (current_class_style & CS_OWNDC) != 0;

  if (candidate_has_own_dc && !current_has_own_dc) {
    return true;
  }
  if (candidate_visible && !current_visible) {
    return true;
  }
  if (candidate_area > current_area) {
    return true;
  }
  if (current_area <= 0 && candidate_area > 0) {
    return true;
  }
  return false;
}

bool FrameHasVisiblePixels(const std::vector<uint8_t> &buffer) {
  if (buffer.size() < 4) {
    return false;
  }
  const size_t pixel_count = buffer.size() / 4;
  const uint8_t *data = buffer.data();
  for (size_t i = 0; i < pixel_count; ++i) {
    const size_t offset = i * 4;
    if (data[offset + 0] != 0 || data[offset + 1] != 0 ||
        data[offset + 2] != 0) {
      return true;
    }
  }
  return false;
}

bool HasValidEmbeddedVisWindow() {
  return g_embedded_vis_window != nullptr &&
         IsWindow(g_embedded_vis_window) != FALSE;
}

void StopTrackingEmbeddedVisWindow() {
  if (g_embedded_vis_window == nullptr) {
    return;
  }
  DebugTraceLog(L"StopTrackingEmbeddedVisWindow: releasing window=%p",
                g_embedded_vis_window);
  DebugTraceDeactivateFallbackForWindow(g_embedded_vis_window);
  DebugTraceUnregisterTargetWindow(g_embedded_vis_window);
  g_embedded_vis_window = nullptr;
}

void SetTrackedEmbeddedVisWindow(HWND window) {
  if (window == nullptr) {
    StopTrackingEmbeddedVisWindow();
    return;
  }
  if (g_embedded_vis_window == window) {
    ResizeEmbeddedWindow(GetEmbeddedWindowContainer());
    return;
  }
  if (g_embedded_vis_window != nullptr) {
    DebugTraceLog(L"SetTrackedEmbeddedVisWindow: replacing %p with %p",
                  g_embedded_vis_window, window);
    DebugTraceUnregisterTargetWindow(g_embedded_vis_window);
    DebugTraceDeactivateFallbackForWindow(g_embedded_vis_window);
  } else {
    DebugTraceLog(L"SetTrackedEmbeddedVisWindow: tracking %p", window);
  }
  g_embedded_vis_window = window;
  DebugTraceRegisterTargetWindow(g_embedded_vis_window);
  if (g_force_diagnostics_fallback && g_embedded_vis_window != nullptr) {
    DebugTraceActivateFallbackForWindow(g_embedded_vis_window);
  }
  ResizeEmbeddedWindow(GetEmbeddedWindowContainer());
}

HWND GetEmbeddedWindowContainer() {
  if (g_child_container_window != nullptr) {
    return g_child_container_window;
  }
  return g_parent_window;
}

HWND ResolveCaptureWindow(const VisHost &host) {
  HWND capture_window = g_embedded_vis_window;
  if (capture_window != nullptr && IsWindow(capture_window) == FALSE) {
    DebugTraceLog(L"ResolveCaptureWindow: embedded window %p invalid", capture_window);
    StopTrackingEmbeddedVisWindow();
    capture_window = nullptr;
  }
  if (capture_window == nullptr) {
    if (host.child != nullptr && IsWindow(host.child)) {
      capture_window = host.child;
    } else if (host.child != nullptr) {
      DebugTraceLog(L"ResolveCaptureWindow: child window %p invalid", host.child);
    }
  }
  if (capture_window == nullptr) {
    if (host.parent != nullptr && IsWindow(host.parent)) {
      capture_window = host.parent;
    } else if (host.parent != nullptr) {
      DebugTraceLog(L"ResolveCaptureWindow: parent window %p invalid", host.parent);
    }
  }
  return capture_window;
}

bool ForceSynchronousPaint(HWND window, int frame_index) {
  if (window == nullptr) {
    return false;
  }
  const UINT redraw_flags = RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN;
  if (RedrawWindow(window, nullptr, nullptr, redraw_flags) == FALSE) {
    const DWORD error = GetLastError();
    DebugTraceLog(L"Frame %d: RedrawWindow(%p) failed error=%lu", frame_index,
                  window, static_cast<unsigned long>(error));
    return false;
  }
  DebugTraceLog(L"Frame %d: RedrawWindow(%p) succeeded", frame_index, window);
  return true;
}

bool CaptureFrameViaForcedOffscreen(const VisHost &host, int frame_index,
                                    std::vector<uint8_t> &frame_rgba) {
  if (!g_force_diagnostics_fallback) {
    return false;
  }

  HWND capture_window = ResolveCaptureWindow(host);
  if (capture_window == nullptr) {
    DebugTraceLog(L"Frame %d: no window available for forced offscreen capture",
                  frame_index);
    return false;
  }

  DebugTraceActivateFallbackForWindow(capture_window);
  if (host.child != nullptr) {
    DebugTraceActivateFallbackForWindow(host.child);
  }
  if (host.parent != nullptr) {
    DebugTraceActivateFallbackForWindow(host.parent);
  }

  if (!ForceSynchronousPaint(capture_window, frame_index)) {
    return false;
  }

  if (DebugTraceCaptureOffscreenSurface(frame_rgba)) {
    DebugTraceLog(L"Frame %d: captured via forced offscreen surface", frame_index);
    return true;
  }

  DebugTraceLog(L"Frame %d: forced offscreen capture unavailable", frame_index);
  return false;
}

bool PumpPendingWindowMessages() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      PostQuitMessage(static_cast<int>(msg.wParam));
      return false;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return true;
}

bool WaitWithMessagePump(DWORD total_wait_ms) {
  if (!PumpPendingWindowMessages()) {
    return false;
  }
  if (total_wait_ms == 0) {
    return true;
  }

  const ULONGLONG wait_limit = GetTickCount64() + total_wait_ms;
  while (true) {
    if (!PumpPendingWindowMessages()) {
      return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (now >= wait_limit) {
      break;
    }

    const ULONGLONG remaining64 = wait_limit - now;
    const DWORD remaining =
        static_cast<DWORD>(std::min<ULONGLONG>(remaining64, 50));
    if (remaining == 0) {
      Sleep(0);
      continue;
    }

    const DWORD wait_result = MsgWaitForMultipleObjectsEx(
        0, nullptr, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait_result == WAIT_FAILED) {
      const DWORD wait_error = GetLastError();
      if (wait_error != ERROR_INVALID_PARAMETER) {
        DebugTraceLog(L"WaitWithMessagePump: MsgWait failed error=%lu",
                      wait_error);
      }
      Sleep(remaining);
    }
  }

  return true;
}

bool RequestEmbeddedVisWindow(const VisHost &host) {
  HWND command_target = nullptr;
  if (host.child != nullptr) {
    command_target = host.child;
  } else if (host.parent != nullptr) {
    command_target = host.parent;
  }
  if (command_target == nullptr) {
    DebugTraceLog(L"RequestEmbeddedVisWindow: no command target available");
    return false;
  }

  DebugTraceLog(L"RequestEmbeddedVisWindow: sending IPC_GETVISWND to %p",
                command_target);
  DWORD_PTR response = 0;
  const LRESULT status = SendMessageTimeoutW(
      command_target, WM_WA_IPC, 0, IPC_GETVISWND, SMTO_NORMAL, 100,
      reinterpret_cast<PDWORD_PTR>(&response));
  if (status == 0) {
    DebugTraceLog(L"RequestEmbeddedVisWindow: SendMessageTimeoutW failed (error=%lu)",
                  GetLastError());
    return false;
  }

  HWND candidate = reinterpret_cast<HWND>(response);
  if (candidate != nullptr && IsWindow(candidate)) {
    DebugTraceLog(L"RequestEmbeddedVisWindow: located embedded window %p",
                  candidate);
    SetTrackedEmbeddedVisWindow(candidate);
    return true;
  }
  DebugTraceLog(L"RequestEmbeddedVisWindow: IPC returned %p (invalid)", candidate);
  return false;
}

bool WaitForEmbeddedVisWindow(const VisHost &host, DWORD timeout_ms) {
  if (HasValidEmbeddedVisWindow()) {
    DebugTraceLog(L"WaitForEmbeddedVisWindow: already have window %p",
                  g_embedded_vis_window);
    return true;
  }

  if (!PumpPendingWindowMessages()) {
    DebugTraceLog(L"WaitForEmbeddedVisWindow: message pump aborted");
    return false;
  }

  if (HasValidEmbeddedVisWindow()) {
    DebugTraceLog(L"WaitForEmbeddedVisWindow: window %p arrived after pump",
                  g_embedded_vis_window);
    return true;
  }

  RequestEmbeddedVisWindow(host);
  if (HasValidEmbeddedVisWindow()) {
    DebugTraceLog(L"WaitForEmbeddedVisWindow: window %p provided by IPC",
                  g_embedded_vis_window);
    return true;
  }

  const ULONGLONG timeout = static_cast<ULONGLONG>(timeout_ms);
  const ULONGLONG start = GetTickCount64();

  while (true) {
    if (!PumpPendingWindowMessages()) {
      DebugTraceLog(L"WaitForEmbeddedVisWindow: message pump failed during wait");
      return false;
    }

    if (HasValidEmbeddedVisWindow()) {
      DebugTraceLog(L"WaitForEmbeddedVisWindow: window %p discovered during wait",
                    g_embedded_vis_window);
      return true;
    }

    const ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed >= timeout) {
      break;
    }

    const ULONGLONG remaining = timeout - elapsed;
    const DWORD wait_duration =
        static_cast<DWORD>(std::min<ULONGLONG>(remaining, 50));
    const DWORD wait_result = MsgWaitForMultipleObjectsEx(
        0, nullptr, wait_duration, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait_result == WAIT_FAILED) {
      const DWORD wait_error = GetLastError();
      if (wait_error == ERROR_INVALID_PARAMETER) {
        if (wait_duration > 0) {
          Sleep(wait_duration);
        } else {
          Sleep(1);
        }
        RequestEmbeddedVisWindow(host);
        continue;
      }
      DebugTraceLog(L"WaitForEmbeddedVisWindow: MsgWait failed error=%lu",
                    wait_error);
      break;
    }

    RequestEmbeddedVisWindow(host);
  }

  const bool found = g_embedded_vis_window != nullptr &&
                     IsWindow(g_embedded_vis_window);
  if (!found) {
    DebugTraceLog(L"WaitForEmbeddedVisWindow: timeout after %u ms", timeout_ms);
  }
  return found;
}

struct HandleCloser {
  void operator()(HANDLE handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using ScopedHandle =
    std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleCloser>;

bool WriteAllBytes(HANDLE handle, const std::string &text) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (text.empty()) {
    return true;
  }

  const char *data = text.data();
  size_t remaining = text.size();
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(handle, data, chunk, &written, nullptr)) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    data += written;
    remaining -= written;
  }
  return true;
}

std::wstring ConvertToWide(const std::string &text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.c_str(), -1, nullptr, 0);
  if (required <= 0) {
    return L"(conversion error)";
  }
  std::wstring result(static_cast<size_t>(required - 1), L'\0');
  if (!result.empty()) {
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1,
                        result.data(), required);
  }
  return result;
}

std::wstring ConvertAnsiToWide(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return std::wstring();
  }
  const int required = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
  if (required <= 0) {
    return L"(conversion error)";
  }
  std::wstring result(static_cast<size_t>(required - 1), L'\0');
  if (!result.empty()) {
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), required);
  }
  return result;
}

std::wstring FormatWindowsErrorMessage(DWORD error_code) {
  wchar_t *buffer = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD size =
      FormatMessageW(flags, nullptr, error_code,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
  std::wstring message;
  if (size != 0 && buffer != nullptr) {
    message.assign(buffer, size);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
      message.pop_back();
    }
  } else {
    message = L"Unknown error";
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return message;
}

bool EnsureDirectoryExists(const std::wstring &path) {
  if (path.empty()) {
    std::wcerr << L"ERROR: Directory path is empty.\n";
    DebugTraceLog(L"EnsureDirectoryExists failed: empty path");
    return false;
  }

  DebugTraceLog(L"EnsureDirectoryExists request: %s", path.c_str());

  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      DebugTraceLog(L"EnsureDirectoryExists OK (exists): %s", path.c_str());
      return true;
    }
    std::wcerr << L"ERROR: Path '" << path
               << L"' exists but is not a directory.\n";
    DebugTraceLog(L"EnsureDirectoryExists failed: not a directory %s", path.c_str());
    return false;
  }

  const DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    std::wcerr << L"ERROR: GetFileAttributesW failed for '" << path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"EnsureDirectoryExists: GetFileAttributesW failed for %s error=%lu",
                  path.c_str(), error);
    return false;
  }

  const std::filesystem::path dir_path(path);
  const std::filesystem::path parent = dir_path.parent_path();
  if (!parent.empty() && parent != dir_path) {
    if (!EnsureDirectoryExists(parent.wstring())) {
      return false;
    }
  }

  if (CreateDirectoryW(path.c_str(), nullptr)) {
    DebugTraceLog(L"EnsureDirectoryExists created: %s", path.c_str());
    return true;
  }

  const DWORD create_error = GetLastError();
  if (create_error == ERROR_ALREADY_EXISTS) {
    DebugTraceLog(L"EnsureDirectoryExists OK (already exists): %s", path.c_str());
    return true;
  }

  std::wcerr << L"ERROR: Failed to create directory '" << path
             << L"': " << FormatWindowsErrorMessage(create_error) << L"\n";
  DebugTraceLog(L"EnsureDirectoryExists: CreateDirectory failed for %s error=%lu",
                path.c_str(), create_error);
  return false;
}

std::wstring FormatFrameFilename(int frame_index) {
  std::wstringstream stream;
  stream << L"frame" << std::setw(4) << std::setfill(L'0') << frame_index
         << L".png";
  return stream.str();
}

static HWND embed_window(embedWindowState *state) {
  (void)state;
  if (g_child_container_window != nullptr) {
    return g_child_container_window;
  }
  return g_parent_window;
}

void ResizeEmbeddedWindow(HWND container) {
  if (container == nullptr || g_embedded_vis_window == nullptr) {
    return;
  }
  RECT rect{0, 0, 0, 0};
  if (!GetClientRect(container, &rect)) {
    return;
  }
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  if (width <= 0 && g_target_width > 0) {
    width = g_target_width;
  }
  if (height <= 0 && g_target_height > 0) {
    height = g_target_height;
  }
  if (width <= 0 || height <= 0) {
    return;
  }
  DebugTraceLog(L"ResizeEmbeddedWindow: container=%p embed=%p size=%dx%d",
                container, g_embedded_vis_window, width, height);
  SetWindowPos(g_embedded_vis_window, nullptr, 0, 0, width, height,
               SWP_NOACTIVATE | SWP_NOZORDER);
  SendMessageW(g_embedded_vis_window, WM_SIZE, SIZE_RESTORED,
               MAKELPARAM(width, height));
}

LRESULT CALLBACK AvsWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  DebugTraceLog(L"AvsWindowProc hwnd=%p msg=0x%04X wParam=0x%p lParam=0x%p", hwnd,
                msg, reinterpret_cast<void *>(wParam),
                reinterpret_cast<void *>(lParam));
  switch (msg) {
    case WM_NCDESTROY:
      if (hwnd == g_child_container_window) {
        g_child_container_window = nullptr;
        DebugTraceUnregisterTargetWindow(hwnd);
      }
      if (hwnd == g_parent_window) {
        g_parent_window = nullptr;
        DebugTraceUnregisterTargetWindow(hwnd);
      }
      if (hwnd == g_embedded_vis_window) {
        StopTrackingEmbeddedVisWindow();
      }
      break;

    case WM_SIZE:
    case WM_SIZING:
      ResizeEmbeddedWindow(hwnd);
      break;

    case WM_PARENTNOTIFY: {
      const UINT event = LOWORD(wParam);
      if (g_embedded_vis_window != nullptr &&
          !IsWindow(g_embedded_vis_window)) {
        DebugTraceLog(
            L"WM_PARENTNOTIFY: dropping stale embedded window handle %p",
            g_embedded_vis_window);
        StopTrackingEmbeddedVisWindow();
      }

      if (event == WM_CREATE) {
        HWND child = reinterpret_cast<HWND>(lParam);
        if (child == nullptr) {
          break;
        }
        if (child == g_parent_window || child == g_child_container_window) {
          break;
        }
        if (!IsWindow(child)) {
          break;
        }
        RECT rect{0, 0, 0, 0};
        GetClientRect(child, &rect);
        const int child_width = rect.right - rect.left;
        const int child_height = rect.bottom - rect.top;
        const LONG_PTR child_style = GetWindowLongPtrW(child, GWL_STYLE);
        const std::wstring class_name = GetWindowClassName(child);
        DebugTraceLog(
            L"WM_PARENTNOTIFY: child create hwnd=%p class=%ls size=%dx%d "
            L"style=0x%016llX",
            child, class_name.c_str(), child_width, child_height,
            static_cast<unsigned long long>(child_style));

        if (!HasValidEmbeddedVisWindow() || ShouldReplaceTrackedWindow(child)) {
          DebugTraceLog(L"WM_PARENTNOTIFY: tracking embedded window candidate %p",
                        child);
          SetTrackedEmbeddedVisWindow(child);
        } else {
          DebugTraceLog(
              L"WM_PARENTNOTIFY: keeping existing embedded window %p over %p",
              g_embedded_vis_window, child);
        }
        ResizeEmbeddedWindow(GetEmbeddedWindowContainer());
      } else if (event == WM_DESTROY) {
        HWND child = reinterpret_cast<HWND>(lParam);
        if (child != nullptr && child == g_embedded_vis_window) {
          DebugTraceLog(L"WM_PARENTNOTIFY: tracked embedded window %p destroyed",
                        child);
          StopTrackingEmbeddedVisWindow();
        }
      }
      break;
    }

    case WM_WINDOWPOSCHANGING: {
      auto pos = reinterpret_cast<WINDOWPOS *>(lParam);
      if (pos != nullptr && (pos->flags & SWP_NOSIZE) == 0 &&
          (hwnd == g_child_container_window || hwnd == g_parent_window)) {
        if (g_target_width > 0 && g_target_height > 0) {
          pos->cx = g_target_width;
          pos->cy = g_target_height;
          DebugTraceLog(
              L"AvsWindowProc: enforcing %dx%d via WINDOWPOSCHANGING on %p",
              g_target_width, g_target_height, hwnd);
        }
      }
      break;
    }

    case WM_GETMINMAXINFO: {
      if (hwnd == g_child_container_window || hwnd == g_parent_window) {
        auto info = reinterpret_cast<MINMAXINFO *>(lParam);
        if (info != nullptr && g_target_width > 0 && g_target_height > 0) {
          info->ptMinTrackSize.x = g_target_width;
          info->ptMinTrackSize.y = g_target_height;
          info->ptMaxTrackSize.x = g_target_width;
          info->ptMaxTrackSize.y = g_target_height;
          return 0;
        }
      }
      break;
    }

    case WM_WA_IPC:
      if (hwnd != g_parent_window && hwnd != g_child_container_window) {
        break;
      }
      switch (lParam) {
        case IPC_GETVERSION:
          DebugTraceLog(L"AvsWindowProc: IPC_GETVERSION -> 0x2900");
          return 0x2900;
        case IPC_ISPLAYING:
          DebugTraceLog(L"AvsWindowProc: IPC_ISPLAYING -> 1");
          return 1;
        case IPC_GETSKIN:
          if (wParam != 0) {
            std::strcpy(reinterpret_cast<char *>(wParam), "/tmp");
          }
          DebugTraceLog(L"AvsWindowProc: IPC_GETSKIN -> %p", reinterpret_cast<void *>(wParam));
          return wParam;
        case IPC_GETINIFILE: {
          static char ini_path[1024] = "";
          if (ini_path[0] == '\0') {
            GetModuleFileNameA(nullptr, ini_path,
                               static_cast<DWORD>(sizeof(ini_path)));
            char *const dot_position = std::strrchr(ini_path, '.');
            if (dot_position == nullptr) {
              ini_path[0] = '\0';
            } else {
              std::strcpy(dot_position, ".ini");
            }
          }
          DebugTraceLog(L"AvsWindowProc: IPC_GETINIFILE -> %S", ini_path);
          return reinterpret_cast<LRESULT>(ini_path);
        }
        case IPC_GET_EMBEDIF: {
          if (g_parent_window != nullptr) {
            ShowWindow(g_parent_window, SW_SHOW);
          }
          if (wParam == 0) {
            DebugTraceLog(L"AvsWindowProc: IPC_GET_EMBEDIF -> embed_window callback");
            return reinterpret_cast<LRESULT>(embed_window);
          }
          auto state = reinterpret_cast<embedWindowState *>(wParam);
          HWND embed_target = embed_window(state);
          if (state != nullptr && g_target_width > 0 && g_target_height > 0) {
            state->r.left = 0;
            state->r.top = 0;
            state->r.right = g_target_width;
            state->r.bottom = g_target_height;
          }
          DebugTraceLog(L"AvsWindowProc: IPC_GET_EMBEDIF state=%p -> %p", state,
                        embed_target);
          ResizeEmbeddedWindow(embed_target);
          return reinterpret_cast<LRESULT>(embed_target);
        }
        case IPC_SETVISWND: {
          HWND vis_window = reinterpret_cast<HWND>(wParam);
          DebugTraceLog(L"IPC_SETVISWND received, window=%p", vis_window);
          if (vis_window == nullptr) {
            StopTrackingEmbeddedVisWindow();
          } else {
            SetTrackedEmbeddedVisWindow(vis_window);
          }
          HWND container = GetEmbeddedWindowContainer();
          ResizeEmbeddedWindow(container);
          DebugTraceLog(L"AvsWindowProc: IPC_SETVISWND configured window=%p", container);
          return 0;
        }
      }
      break;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterWindowClass(const wchar_t *class_name, WNDPROC window_proc) {
  HINSTANCE instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = window_proc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  cls.lpszClassName = class_name;

  const ATOM atom = RegisterClassExW(&cls);
  if (atom != 0) {
    return true;
  }

  const DWORD error = GetLastError();
  if (error == ERROR_CLASS_ALREADY_EXISTS) {
    return true;
  }

  std::wcerr << L"ERROR: Failed to register window class '" << class_name
             << L"': " << FormatWindowsErrorMessage(error) << L"\n";
  return false;
}

uint8_t SampleToWaveformValue(int16_t sample) {
  const float scaled =
      (static_cast<float>(sample) / 32768.0f) * 127.0f + 128.0f;
  const float clamped = std::clamp(scaled, 0.0f, 255.0f);
  return static_cast<uint8_t>(clamped);
}

void fill_waveform(winampVisModule *mod, const int16_t *pcm, int start_sample,
                   int hop) {
  if (mod == nullptr) {
    return;
  }

  if (pcm == nullptr || g_total_pcm_samples <= 0 || hop <= 0) {
    for (int channel = 0; channel < 2; ++channel) {
      std::fill_n(mod->waveformData[channel], kWaveformSamples, 128);
    }
    return;
  }

  const int last_valid_sample = g_total_pcm_samples - 1;

  for (int channel = 0; channel < 2; ++channel) {
    for (int i = 0; i < kWaveformSamples; ++i) {
      int sample_index = start_sample + i * hop;
      if (sample_index > last_valid_sample) {
        sample_index = last_valid_sample;
      }
      if (sample_index < 0) {
        sample_index = 0;
      }

      const int16_t sample = pcm[sample_index * 2 + channel];
      mod->waveformData[channel][i] = SampleToWaveformValue(sample);
    }
  }
}

HWND CreateHiddenParentWindow(int width, int height) {
  DebugTraceLog(L"CreateHiddenParentWindow width=%d height=%d", width, height);
  if (!RegisterWindowClass(kParentWindowClassName, AvsWindowProc)) {
    return nullptr;
  }

  HINSTANCE instance = GetModuleHandleW(nullptr);

  RECT rect{0, 0, width, height};
  if (!AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW)) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: AdjustWindowRectEx failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    return nullptr;
  }

  const int window_width = rect.right - rect.left;
  const int window_height = rect.bottom - rect.top;

  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kParentWindowClassName,
                              L"visdriver avs parent", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, window_width,
                              window_height, nullptr, nullptr, instance,
                              nullptr);
  if (hwnd == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to create parent window: "
               << FormatWindowsErrorMessage(error) << L"\n";
  }
  g_parent_window = hwnd;
  StopTrackingEmbeddedVisWindow();
  if (hwnd != nullptr) {
    DebugTraceRegisterTargetWindow(hwnd);
  }
  return hwnd;
}

HWND CreateChildWindow(HWND parent, int width, int height) {
  DebugTraceLog(L"CreateChildWindow parent=%p width=%d height=%d", parent, width,
                height);
  if (!RegisterWindowClass(kChildWindowClassName, AvsWindowProc)) {
    return nullptr;
  }

  HINSTANCE instance = GetModuleHandleW(nullptr);
  HWND hwnd = CreateWindowExW(0, kChildWindowClassName, L"visdriver avs child",
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
                                  WS_VISIBLE,
                              0, 0, width, height, parent, nullptr, instance,
                              nullptr);
  if (hwnd == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to create child window: "
               << FormatWindowsErrorMessage(error) << L"\n";
  }
  g_child_container_window = hwnd;
  if (hwnd != nullptr) {
    DebugTraceRegisterTargetWindow(hwnd);
  }
  return hwnd;
}

enum class HashMode {
  kPixels,
  kRolling,
};

struct Options {
  std::wstring vis_dll;
  std::wstring runtime_dir;
  std::wstring vis_avs_dat;
  std::wstring out_dll;
  std::wstring preset;
  std::wstring wav;
  int width = 640;
  int height = 480;
  int fps = 60;
  int frames = 121;
  int wait_time_seconds = 5;
  std::wstring out_dir;
  std::wstring avi_out;
  int png_step = 1;
  HashMode hash_mode = HashMode::kPixels;
  std::wstring trace_log = L"avs_debug_trace.log";
  bool diagnostics_fallback_enabled = true;
};

void PrintUsage(const std::wstring &command_name) {
  struct OptionHelp {
    const wchar_t *flag;
    const wchar_t *description;
  };

  const std::array<OptionHelp, 18> kOptions = {
      OptionHelp{L"--vis-dll <path>", L"Path to vis DLL (required)"},
      OptionHelp{L"--runtime-dir <dir>",
                 L"Runtime directory (default: directory of vis DLL)"},
      OptionHelp{L"--vis-avs-dat <path>", L"Optional path to vis_avs.dat"},
      OptionHelp{L"--out-dll <path>", L"Optional output plug-in DLL"},
      OptionHelp{L"--preset <path>", L"Optional path to preset file"},
      OptionHelp{L"--wav <path>", L"Path to WAV input (required)"},
      OptionHelp{L"--width <pixels>", L"Output width (default: 640)"},
      OptionHelp{L"--height <pixels>", L"Output height (default: 480)"},
      OptionHelp{L"--fps <value>", L"Frames per second (default: 60)"},
      OptionHelp{L"--frames <count>",
                 L"Number of frames to render (default: 121)"},
      OptionHelp{L"--wait-time <seconds>",
                 L"Delay before playback/capture (default: 5)"},
      OptionHelp{L"--out-dir <dir>", L"Output directory (required)"},
      OptionHelp{L"--avi-out <filename>", L"Optional AVI output filename"},
      OptionHelp{L"--png-step <value>",
                 L"Interval between PNG dumps (default: 1)"},
      OptionHelp{L"--hash-mode <mode>",
                 L"Hashing mode: pixels|rolling (default: pixels)"},
      OptionHelp{L"--trace-log <filename>",
                 L"Diagnostics log filename (default: avs_debug_trace.log)"},
      OptionHelp{L"--disable-diagnostics-fallback",
                 L"Disable diagnostics buffer capture fallback"},
      OptionHelp{L"--help, -h", L"Show this help message"},
  };

  size_t max_flag_width = 0;
  for (const OptionHelp &option : kOptions) {
    max_flag_width =
        std::max(max_flag_width, static_cast<size_t>(std::wcslen(option.flag)));
  }

  std::wcout << L"Usage: " << command_name << L" [OPTIONS]\n\n";
  std::wcout << L"Options:\n";
  for (const OptionHelp &option : kOptions) {
    const size_t flag_width = static_cast<size_t>(std::wcslen(option.flag));
    std::wcout << L"  " << option.flag;
    if (flag_width < max_flag_width) {
      std::wcout << std::wstring(max_flag_width - flag_width + 2, L' ');
    } else {
      std::wcout << L"  ";
    }
    std::wcout << option.description << L"\n";
  }
}

std::wstring ToLower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool ParseIntegerOption(const std::wstring &option_name, const std::wstring &value,
                        int *target) {
  try {
    size_t index = 0;
    const int parsed = std::stoi(value, &index, 10);
    if (index != value.size()) {
      std::wcerr << L"ERROR: Option '" << option_name
                 << L"' expects an integer value.\n";
      return false;
    }
    *target = parsed;
    return true;
  } catch (const std::exception &) {
    std::wcerr << L"ERROR: Option '" << option_name
               << L"' expects an integer value.\n";
    return false;
  }
}

void PrintSummary(const Options &options) {
  std::wcout << L"generate-verification-data options:\n";
  std::wcout << L"  Visualizer DLL:        " << options.vis_dll << L"\n";
  std::wcout << L"  Runtime directory:     " << options.runtime_dir << L"\n";
  std::wcout << L"  vis_avs.dat:           "
             << (options.vis_avs_dat.empty() ? L"(not set)" : options.vis_avs_dat)
             << L"\n";
  std::wcout << L"  Output plug-in DLL:    "
             << (options.out_dll.empty() ? L"(not set)" : options.out_dll)
             << L"\n";
  std::wcout << L"  Preset:                "
             << (options.preset.empty() ? L"(not set)" : options.preset) << L"\n";
  std::wcout << L"  WAV input:             " << options.wav << L"\n";
  std::wcout << L"  Output size (WxH):     " << options.width << L"x" << options.height
             << L"\n";
  std::wcout << L"  Frame rate:            " << options.fps << L" fps\n";
  std::wcout << L"  Frame count:           " << options.frames << L"\n";
  std::wcout << L"  Wait time (s):         " << options.wait_time_seconds << L"\n";
  std::wcout << L"  Output directory:      " << options.out_dir << L"\n";
  std::wcout << L"  AVI output filename:   "
             << (options.avi_out.empty() ? L"(not set)" : options.avi_out) << L"\n";
  std::wcout << L"  PNG step:              " << options.png_step << L"\n";
  std::wcout << L"  Hash mode:             "
             << (options.hash_mode == HashMode::kPixels ? L"pixels" : L"rolling")
             << L"\n";
  std::wcout << L"  Trace log filename:    " << options.trace_log << L"\n";
  std::wcout << L"  Diagnostics fallback:  "
             << (options.diagnostics_fallback_enabled ? L"enabled"
                                                      : L"disabled")
             << L"\n";
}

} // namespace

extern "C" int cmd_generate_verification_data(int argc, wchar_t **argv) {
  const std::wstring command_name =
      (argc > 0 && argv != nullptr) ? argv[0] : L"generate-verification-data";

  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::wstring current = argv[i];
    if (current == L"--help" || current == L"-h") {
      PrintUsage(command_name);
      return 0;
    }

    auto require_value = [&](const std::wstring &option) -> std::wstring {
      if (i + 1 >= argc) {
        std::wcerr << L"ERROR: Option '" << option << L"' expects a value.\n";
        throw std::runtime_error("missing option value");
      }
      ++i;
      return argv[i];
    };

    try {
      if (current == L"--vis-dll") {
        options.vis_dll = require_value(current);
      } else if (current == L"--runtime-dir") {
        options.runtime_dir = require_value(current);
      } else if (current == L"--vis-avs-dat") {
        options.vis_avs_dat = require_value(current);
      } else if (current == L"--out-dll") {
        options.out_dll = require_value(current);
      } else if (current == L"--preset") {
        options.preset = require_value(current);
      } else if (current == L"--wav") {
        options.wav = require_value(current);
      } else if (current == L"--width") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.width)) {
          return 1;
        }
      } else if (current == L"--height") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.height)) {
          return 1;
        }
      } else if (current == L"--fps") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.fps)) {
          return 1;
        }
      } else if (current == L"--frames") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.frames)) {
          return 1;
        }
      } else if (current == L"--wait-time") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.wait_time_seconds)) {
          return 1;
        }
      } else if (current == L"--out-dir") {
        options.out_dir = require_value(current);
      } else if (current == L"--avi-out") {
        options.avi_out = require_value(current);
      } else if (current == L"--png-step") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.png_step)) {
          return 1;
        }
      } else if (current == L"--hash-mode") {
        const std::wstring value = ToLower(require_value(current));
        if (value == L"pixels") {
          options.hash_mode = HashMode::kPixels;
        } else if (value == L"rolling") {
          options.hash_mode = HashMode::kRolling;
        } else {
          std::wcerr << L"ERROR: Option '--hash-mode' expects 'pixels' or 'rolling'.\n";
          return 1;
        }
      } else if (current == L"--trace-log") {
        options.trace_log = require_value(current);
      } else if (current == L"--disable-diagnostics-fallback") {
        options.diagnostics_fallback_enabled = false;
      } else {
        std::wcerr << L"ERROR: Unknown option '" << current << L"'.\n";
        return 1;
      }
    } catch (const std::runtime_error &) {
      return 1;
    }
  }

  std::vector<std::wstring> missing;
  if (options.vis_dll.empty()) {
    missing.push_back(L"--vis-dll");
  }
  if (options.wav.empty()) {
    missing.push_back(L"--wav");
  }
  if (options.out_dir.empty()) {
    missing.push_back(L"--out-dir");
  }

  if (!missing.empty()) {
    for (const std::wstring &option : missing) {
      std::wcerr << L"ERROR: Required option '" << option << L"' is missing.\n";
    }
    std::wcerr << L"\n";
    PrintUsage(command_name);
    return 1;
  }

  if (options.png_step <= 0) {
    std::wcerr << L"ERROR: --png-step must be greater than zero.\n";
    return 1;
  }

  if (options.wait_time_seconds < 0) {
    std::wcerr << L"ERROR: --wait-time must be zero or greater.\n";
    return 1;
  }

  if (options.runtime_dir.empty()) {
    const std::filesystem::path vis_path(options.vis_dll);
    const std::filesystem::path parent = vis_path.parent_path();
    options.runtime_dir = parent.empty() ? std::wstring(L".") : parent.wstring();
  }

  auto normalize_absolute = [](const std::wstring &value) {
    if (value.empty()) {
      return std::wstring();
    }
    std::filesystem::path target(value);
    std::error_code ec;
    std::filesystem::path abs_path = std::filesystem::absolute(target, ec);
    if (!ec) {
      abs_path = abs_path.lexically_normal();
      return abs_path.wstring();
    }
    return target.lexically_normal().wstring();
  };

  auto normalize_with_base = [&](const std::wstring &value,
                                 const std::wstring &base) {
    if (value.empty()) {
      return std::wstring();
    }
    std::filesystem::path target(value);
    if (!target.is_absolute()) {
      if (!base.empty()) {
        target = std::filesystem::path(base) / target;
      }
    }
    std::error_code ec;
    std::filesystem::path abs_path = std::filesystem::absolute(target, ec);
    if (!ec) {
      abs_path = abs_path.lexically_normal();
      return abs_path.wstring();
    }
    return target.lexically_normal().wstring();
  };

  options.runtime_dir = normalize_absolute(options.runtime_dir);
  options.vis_dll = normalize_with_base(options.vis_dll, options.runtime_dir);
  if (!options.vis_avs_dat.empty()) {
    options.vis_avs_dat =
        normalize_with_base(options.vis_avs_dat, options.runtime_dir);
  }
  if (!options.out_dll.empty()) {
    options.out_dll = normalize_with_base(options.out_dll, options.runtime_dir);
  }
  if (!options.preset.empty()) {
    options.preset = normalize_with_base(options.preset, options.runtime_dir);
  }
  options.wav = normalize_absolute(options.wav);
  options.out_dir = normalize_absolute(options.out_dir);
  if (!options.avi_out.empty()) {
    std::filesystem::path avi_path(options.avi_out);
    options.avi_out = avi_path.lexically_normal().wstring();
  }

  PrintSummary(options);

  auto log_runtime_directory = [](const std::wstring &runtime_dir) {
    if (runtime_dir.empty()) {
      std::wcout << L"Runtime directory is not set.\n";
      return;
    }
    const std::filesystem::path dir(runtime_dir);
    std::error_code status_error;
    const bool exists = std::filesystem::exists(dir, status_error);
    if (status_error || !exists) {
      std::wcerr << L"ERROR: Runtime directory '" << runtime_dir
                 << L"' is not accessible.\n";
      return;
    }
    std::wcout << L"Runtime directory ready: " << runtime_dir << L"\n";
    std::filesystem::directory_iterator iter(dir, status_error);
    if (status_error) {
      std::wcerr << L"ERROR: Failed to enumerate runtime directory '"
                 << runtime_dir << L"'.\n";
      return;
    }
    size_t shown = 0;
    bool has_more = false;
    for (; iter != std::filesystem::directory_iterator();
         iter.increment(status_error)) {
      if (status_error) {
        std::wcerr << L"ERROR: Failed during runtime directory enumeration.\n";
        return;
      }
      if (shown < 3) {
        std::wcout << L"  entry: "
                   << iter->path().filename().wstring() << L"\n";
      } else {
        has_more = true;
        break;
      }
      ++shown;
    }
    if (shown == 0) {
      std::wcout << L"  (directory is empty)\n";
    } else if (has_more) {
      std::wcout << L"  ...\n";
    }
  };

  auto log_file_snippet = [](const std::wstring &label,
                             const std::wstring &path) {
    if (path.empty()) {
      return;
    }
    std::wcout << label << L": " << path << L"\n";
    ScopedHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
    if (!handle || handle.get() == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to open '" << path << L"': "
                 << FormatWindowsErrorMessage(error) << L"\n";
      return;
    }
    std::array<uint8_t, 32> buffer{};
    DWORD bytes_read = 0;
    if (!ReadFile(handle.get(), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to read '" << path << L"': "
                 << FormatWindowsErrorMessage(error) << L"\n";
      return;
    }
    if (bytes_read == 0) {
      std::wcout << L"  (file is empty)\n";
      return;
    }
    std::wstringstream hex_stream;
    hex_stream << std::hex << std::setfill(L'0');
    const DWORD hex_limit = std::min<DWORD>(bytes_read, 16);
    for (DWORD i = 0; i < hex_limit; ++i) {
      if (i != 0) {
        hex_stream << L' ';
      }
      hex_stream << std::setw(2)
                 << static_cast<int>(static_cast<unsigned char>(buffer[i]));
    }
    if (bytes_read > hex_limit) {
      hex_stream << L" ...";
    }
    std::wstring text_sample;
    const DWORD text_limit = std::min<DWORD>(bytes_read, 32);
    text_sample.reserve(text_limit);
    for (DWORD i = 0; i < text_limit; ++i) {
      const unsigned char ch = buffer[i];
      if (ch >= 32 && ch < 127) {
        text_sample.push_back(static_cast<wchar_t>(ch));
      } else {
        text_sample.push_back(L'.');
      }
    }
    if (bytes_read > text_limit) {
      text_sample += L"...";
    }
    std::wcout << L"  first " << hex_limit << L" bytes hex: "
               << hex_stream.str() << L"\n";
    std::wcout << L"  text sample: " << text_sample << L"\n";
  };

  auto log_output_module = [](const std::wstring &path) {
    if (path.empty()) {
      return;
    }
    std::wcout << L"Output plug-in probe: " << path << L"\n";
    HMODULE module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to load output DLL '" << path << L"': "
                 << FormatWindowsErrorMessage(error) << L"\n";
      return;
    }
    auto cleanup = [&]() { FreeLibrary(module); };
    using WinampGetOutModule = Out_Module *(__cdecl *)();
    FARPROC proc = GetProcAddress(module, "winampGetOutModule");
    if (proc == nullptr) {
      std::wcerr << L"ERROR: winampGetOutModule not found in '" << path
                 << L"'.\n";
      cleanup();
      return;
    }
    auto getter = reinterpret_cast<WinampGetOutModule>(proc);
    Out_Module *out_module = getter();
    if (out_module == nullptr) {
      std::wcerr << L"ERROR: winampGetOutModule returned null for '" << path
                 << L"'.\n";
      cleanup();
      return;
    }
    std::wstring description = ConvertAnsiToWide(out_module->description);
    std::wcout << L"  description: "
               << (description.empty() ? L"(none)" : description) << L"\n";
    std::wcout << L"  version: 0x" << std::hex << out_module->version << std::dec
               << L"\n";
    cleanup();
  };

  log_runtime_directory(options.runtime_dir);
  DebugTraceLog(L"Runtime directory: %s", options.runtime_dir.c_str());
  if (!options.vis_avs_dat.empty()) {
    log_file_snippet(L"vis_avs.dat", options.vis_avs_dat);
    DebugTraceLog(L"vis_avs.dat path: %s", options.vis_avs_dat.c_str());
  }
  if (!options.preset.empty()) {
    log_file_snippet(L"Preset", options.preset);
    DebugTraceLog(L"Preset path: %s", options.preset.c_str());
  }
  if (!options.out_dll.empty()) {
    log_output_module(options.out_dll);
    DebugTraceLog(L"Output plug-in override: %s", options.out_dll.c_str());
  }

  std::error_code path_error;
  const std::filesystem::path out_dir_path =
      std::filesystem::absolute(options.out_dir, path_error);
  if (path_error) {
    std::wcerr << L"ERROR: Failed to resolve output directory '" << options.out_dir
               << L"': "
               << ConvertToWide(path_error.message()) << L"\n";
    return 1;
  }
  const std::filesystem::path frames_dir_path = out_dir_path / L"frames";
  const std::filesystem::path hashes_dir_path = out_dir_path / L"hashes";
  const std::filesystem::path logs_dir_path = out_dir_path / L"logs";

  if (!EnsureDirectoryExists(out_dir_path.wstring())) {
    return 1;
  }
  if (!EnsureDirectoryExists(frames_dir_path.wstring())) {
    return 1;
  }
  if (!EnsureDirectoryExists(hashes_dir_path.wstring())) {
    return 1;
  }
  if (!EnsureDirectoryExists(logs_dir_path.wstring())) {
    return 1;
  }

  std::filesystem::path trace_log_path(options.trace_log);
  if (!trace_log_path.is_absolute()) {
    trace_log_path = logs_dir_path / trace_log_path;
  }
  trace_log_path = trace_log_path.lexically_normal();
  struct DebugTraceGuard {
    bool active = false;
    ~DebugTraceGuard() {
      if (active) {
        DebugTraceResetDiagnosticsBuffer();
        DebugTraceResetOffscreenSurface();
        DebugTraceShutdown();
      }
    }
  } trace_guard;
  if (!DebugTraceInitialize(trace_log_path.wstring())) {
    std::wcerr << L"ERROR: Failed to initialize debug trace log at '"
               << trace_log_path.wstring() << L"'.\n";
    return 1;
  }
  trace_guard.active = true;
  DebugTraceInstallHooksForModule(GetModuleHandleW(nullptr),
                                  L"visdriver host process");
  DebugTraceLog(L"Debug trace logging to %s", trace_log_path.c_str());
  DebugTraceLog(L"Render options: %dx%d @ %d FPS for %d frames", options.width,
                options.height, options.fps, options.frames);

  g_target_width = options.width;
  g_target_height = options.height;

  const std::filesystem::path per_frame_csv_path =
      hashes_dir_path / L"per_frame.csv";
  const std::filesystem::path rolling_hash_path =
      hashes_dir_path / L"rolling_sha256.txt";

  ScopedHandle per_frame_file(CreateFileW(
      per_frame_csv_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!per_frame_file || per_frame_file.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to open per-frame hash file '"
               << per_frame_csv_path.wstring() << L"': "
               << FormatWindowsErrorMessage(error) << L"\n";
    return 1;
  }

  std::vector<int16_t> audio_pcm;
  int wav_sample_rate = 0;
  int wav_channels = 0;
  int64_t wav_sample_count = 0;
  try {
    WavData wav = load_wav_16le_stereo(options.wav);
    if (wav.channels != 2) {
      throw std::runtime_error("WAV file must have 2 channels");
    }

    if (wav.sample_rate != 44100) {
      audio_pcm = resample_linear_2ch_16bit(wav.pcm, wav.sample_rate, 44100);
    } else {
      audio_pcm = wav.pcm;
    }

    const size_t total_samples = audio_pcm.size() / 2;
    std::wcout << L"WAV ok: 44100 Hz, 2 ch, " << total_samples << L" samples\n";
    DebugTraceLog(L"Loaded WAV '%s' (%zu samples)", options.wav.c_str(),
                  total_samples);
    g_total_pcm_samples = static_cast<int>(total_samples);
    wav_sample_rate = 44100;
    wav_channels = 2;
    wav_sample_count = static_cast<int64_t>(total_samples);
  } catch (const std::exception &ex) {
    std::wcerr << L"ERROR: Failed to load WAV: "
               << ConvertToWide(std::string(ex.what())) << L"\n";
    return 1;
  }

  if (!SetCurrentDirectoryW(options.runtime_dir.c_str())) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to set current directory to '"
               << options.runtime_dir << L"': "
               << FormatWindowsErrorMessage(error) << L"\n";
    return 1;
  }
  std::wcout << L"Switched to runtime directory: " << options.runtime_dir
             << L"\n";
  DebugTraceLog(L"SetCurrentDirectory to %s", options.runtime_dir.c_str());

  HWND parent_window = CreateHiddenParentWindow(options.width, options.height);
  if (parent_window == nullptr) {
    return 1;
  }

  VisHost host = load_vis(options.vis_dll, parent_window);
  if (host.dll == nullptr || host.hdr == nullptr || host.mod == nullptr) {
    unload_vis(host);
    return 1;
  }
  DebugTraceLog(L"Loaded vis module from %s (module=%p)", options.vis_dll.c_str(),
                host.dll);
  DebugTraceInstallHooksForModule(host.dll, options.vis_dll);

  std::wstring header_description = ConvertAnsiToWide(host.hdr->description);
  if (header_description.empty()) {
    header_description = L"(no header description)";
  }
  std::wstring module_description = ConvertAnsiToWide(host.mod->description);
  if (module_description.empty()) {
    module_description = L"(no module description)";
  }
  std::wcout << L"Loaded vis module: " << header_description << L"\n";
  std::wcout << L"  module description: " << module_description << L"\n";

  host.child = CreateChildWindow(parent_window, options.width, options.height);
  if (host.child == nullptr) {
    unload_vis(host);
    return 1;
  }
  const bool offscreen_surface_ready =
      DebugTraceConfigureOffscreenSurface(options.width, options.height);
  if (!offscreen_surface_ready) {
    DebugTraceLog(L"WARNING: Offscreen surface setup failed");
  }
  bool diagnostics_buffer_ready = false;
  if (options.diagnostics_fallback_enabled) {
    diagnostics_buffer_ready =
        DebugTraceConfigureDiagnosticsBuffer(options.width, options.height);
    if (!diagnostics_buffer_ready) {
      DebugTraceLog(L"WARNING: Diagnostics buffer setup failed");
    }
  } else {
    DebugTraceLog(L"Diagnostics fallback disabled by option");
  }
  g_force_diagnostics_fallback =
      offscreen_surface_ready && options.diagnostics_fallback_enabled;
  if (g_force_diagnostics_fallback) {
    DebugTraceActivateFallbackForWindow(host.child);
    if (parent_window != nullptr) {
      DebugTraceActivateFallbackForWindow(parent_window);
    }
  }

  host.mod->delayMs = (options.fps > 0) ? (1000 / options.fps) : 0;
  if (!begin_vis(host, options.width, options.height)) {
    unload_vis(host);
    return 1;
  }
  DebugTraceLog(L"Visualization module initialized successfully");

  if (!options.preset.empty()) {
    DebugTraceLog(L"Loading preset from %s", options.preset.c_str());
    if (!std::filesystem::exists(options.preset)) {
      std::wcerr << L"ERROR: Preset file not found: " << options.preset << L"\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    if (!WaitForEmbeddedVisWindow(host, 5000)) {
      std::wcerr << L"ERROR: Failed to locate AVS window for preset load (timeout).\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    HWND preset_window = g_embedded_vis_window;
    if (preset_window == nullptr || !IsWindow(preset_window)) {
      std::wcerr << L"ERROR: Failed to locate AVS window for preset load.\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    const size_t preset_length = options.preset.size();
    const size_t buffer_size =
        sizeof(DROPFILES) + (preset_length + 2) * sizeof(wchar_t);
    HGLOBAL drop_handle = GlobalAlloc(GHND | GMEM_SHARE, buffer_size);
    if (drop_handle == nullptr) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to allocate DROPFILES buffer: "
                 << FormatWindowsErrorMessage(error) << L"\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    auto drop_info = reinterpret_cast<DROPFILES *>(GlobalLock(drop_handle));
    if (drop_info == nullptr) {
      const DWORD error = GetLastError();
      GlobalFree(drop_handle);
      std::wcerr << L"ERROR: Failed to lock DROPFILES buffer: "
                 << FormatWindowsErrorMessage(error) << L"\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    drop_info->pFiles = sizeof(DROPFILES);
    drop_info->pt.x = 0;
    drop_info->pt.y = 0;
    drop_info->fNC = FALSE;
    drop_info->fWide = TRUE;

    auto payload = reinterpret_cast<wchar_t *>(
        reinterpret_cast<BYTE *>(drop_info) + sizeof(DROPFILES));
    std::memcpy(payload, options.preset.c_str(),
                preset_length * sizeof(wchar_t));
    payload[preset_length] = L'\0';
    payload[preset_length + 1] = L'\0';

    GlobalUnlock(drop_handle);

    DWORD_PTR send_result = 0;
    const LRESULT send_status = SendMessageTimeoutW(
        preset_window, WM_DROPFILES, reinterpret_cast<WPARAM>(drop_handle), 0,
        SMTO_BLOCK | SMTO_ABORTIFHUNG, 5000, &send_result);
    if (send_status == 0) {
      const DWORD error = GetLastError();
      DragFinish(reinterpret_cast<HDROP>(drop_handle));
      std::wcerr << L"ERROR: Failed to deliver preset to AVS window: "
                 << FormatWindowsErrorMessage(error) << L"\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    if (!IsWindow(preset_window)) {
      std::wcerr << L"ERROR: AVS window destroyed during preset load.\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }

    std::wcout << L"Loaded preset: " << options.preset << L"\n";
    DebugTraceLog(L"Preset load delivered via WM_DROPFILES");
  }

  const int wait_seconds = options.wait_time_seconds;
  if (wait_seconds > 0) {
    const uint64_t wait_ms_64 = static_cast<uint64_t>(wait_seconds) * 1000ull;
    const DWORD wait_ms =
        wait_ms_64 > std::numeric_limits<DWORD>::max()
            ? std::numeric_limits<DWORD>::max()
            : static_cast<DWORD>(wait_ms_64);
    std::wcout << L"Waiting " << wait_seconds
               << (wait_seconds == 1 ? L" second" : L" seconds")
               << L" before starting playback and capture.\n";
    DebugTraceLog(L"Initialization wait: %d seconds (%lu ms)", wait_seconds,
                  static_cast<unsigned long>(wait_ms));
    if (!WaitWithMessagePump(wait_ms)) {
      std::wcerr << L"ERROR: Message loop aborted during wait period.\n";
      end_vis(host);
      unload_vis(host);
      return 1;
    }
    DebugTraceLog(L"Initialization wait completed");
  }

  if (host.mod->Render == nullptr) {
    std::wcerr << L"ERROR: Visualization module is missing Render().\n";
    end_vis(host);
    unload_vis(host);
    return 1;
  }

#if defined(_MM_SET_FLUSH_ZERO_MODE) && defined(_MM_SET_DENORMALS_ZERO_MODE)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

  std::vector<int16_t> spectrum_window(static_cast<size_t>(kSpectrumFftSize) * 2,
                                      0);
  bool logged_spectrum_ok = false;

  std::vector<uint8_t> frame_rgba;
  std::vector<std::filesystem::path> png_outputs;
  bool capture_failed = false;
  bool hash_failed = false;
  bool avi_failed = false;
  const int effective_fps = (options.fps > 0) ? options.fps : 1;
  AviWriter avi_writer;
  const bool avi_enabled = !options.avi_out.empty();
  std::filesystem::path avi_output_path;
  if (avi_enabled) {
    avi_output_path = out_dir_path / options.avi_out;
  }
  bool avi_opened = false;
  if (avi_enabled && options.frames == 0) {
    if (!avi_writer.Open(avi_output_path.wstring(), options.width,
                         options.height, effective_fps)) {
      avi_failed = true;
    } else {
      avi_opened = true;
    }
  }
  Sha256 rolling_hash;
  uint64_t diagnostics_generation = 0;

  for (int frame = 0; frame < options.frames; ++frame) {
    if (!PumpPendingWindowMessages()) {
      break;
    }

    const DWORD pump_delay =
        (host.mod != nullptr && host.mod->delayMs > 0)
            ? static_cast<DWORD>(host.mod->delayMs)
            : 0;
    if (pump_delay > 0) {
      Sleep(pump_delay);
    } else {
      Sleep(0);
    }

    const int samples_per_frame =
        static_cast<int>(std::lround(44100.0 / effective_fps));
    const int hop = std::max(1, samples_per_frame / kWaveformSamples);
    const int start_sample = frame * samples_per_frame;

    fill_waveform(host.mod, audio_pcm.data(), start_sample, hop);

    std::fill(spectrum_window.begin(), spectrum_window.end(), 0);
    const int total_samples = g_total_pcm_samples;
    if (!audio_pcm.empty() && start_sample < total_samples) {
      const int available = total_samples - start_sample;
      const int samples_to_copy =
          std::min(kSpectrumFftSize, std::max(0, available));
      if (samples_to_copy > 0) {
        const int16_t *src =
            audio_pcm.data() + static_cast<size_t>(start_sample) * 2;
        std::copy_n(src, static_cast<size_t>(samples_to_copy) * 2,
                    spectrum_window.begin());
      }
    }

    fill_spectrum(host.mod, spectrum_window.data(), 0, kSpectrumFftSize);

    if (!logged_spectrum_ok) {
      std::wcout << L"spectrum ok\n";
      logged_spectrum_ok = true;
    }

    std::wcout << L"Frame " << (frame + 1) << L"/" << options.frames
               << L": start=" << start_sample << L", hop=" << hop << L"\n";
    DebugTraceLog(L"Frame %d: begin", frame);
    DebugTraceLog(L"Frame %d: waveform start=%d hop=%d", frame, start_sample, hop);

    const int render_result = host.mod->Render(host.mod);
    DebugTraceLog(L"Frame %d: Render returned %d", frame, render_result);
    if (render_result != 0) {
      DebugTraceLog(L"Frame %d: Render aborted", frame);
      DebugTraceLog(L"Frame %d: end (render aborted)", frame);
      break;
    }

    bool captured = false;
    bool used_trace_capture = false;
    if (g_force_diagnostics_fallback) {
      captured = CaptureFrameViaForcedOffscreen(host, frame, frame_rgba);
      used_trace_capture = captured;
    }
    if (!captured && options.diagnostics_fallback_enabled) {
      const uint64_t previous_generation = diagnostics_generation;
      uint64_t next_generation = diagnostics_generation;
      if (DebugTraceFetchLastFrame(next_generation, frame_rgba)) {
        diagnostics_generation = next_generation;
        DebugTraceLog(L"Frame %d: captured from diagnostics buffer (generation=%llu)",
                      frame, static_cast<unsigned long long>(diagnostics_generation));
        captured = true;
        used_trace_capture = true;
      } else {
        const uint64_t peek_generation = DebugTracePeekDiagnosticsGeneration();
        if (peek_generation == 0) {
          DebugTraceLog(L"Frame %d: diagnostics buffer empty", frame);
        } else if (peek_generation == previous_generation) {
          DebugTraceLog(L"Frame %d: diagnostics buffer unchanged (generation=%llu)",
                        frame, static_cast<unsigned long long>(peek_generation));
        } else {
          DebugTraceLog(L"Frame %d: diagnostics buffer stale (prev=%llu current=%llu)",
                        frame, static_cast<unsigned long long>(previous_generation),
                        static_cast<unsigned long long>(peek_generation));
        }
      }
    }
    if (!captured) {
      const bool fallback_active =
          DebugTraceIsFallbackActiveForWindow(host.child) ||
          DebugTraceIsFallbackActiveForWindow(host.parent);
      if (fallback_active) {
        captured = DebugTraceCaptureOffscreenSurface(frame_rgba);
        if (captured) {
          DebugTraceLog(L"Frame %d: captured from offscreen surface", frame);
          used_trace_capture = true;
        } else {
          DebugTraceLog(L"Frame %d: offscreen capture unavailable", frame);
        }
      } else {
        DebugTraceLog(L"Frame %d: offscreen capture skipped (fallback inactive)",
                      frame);
      }
    }
    if (!captured) {
      DebugTraceLog(L"Frame %d: attempting window capture fallback", frame);
      HWND capture_window = g_embedded_vis_window;
      if (capture_window != nullptr &&
          IsWindow(capture_window) == FALSE) {
        DebugTraceLog(
            L"Frame %d: embedded window %p is no longer valid, clearing", frame,
            capture_window);
        StopTrackingEmbeddedVisWindow();
        capture_window = nullptr;
      }
      if (capture_window == nullptr) {
        capture_window = host.child;
      }
      if (capture_window == nullptr) {
        capture_window = host.parent;
      }
      if (capture_window != nullptr && capture_window == g_embedded_vis_window) {
        ResizeEmbeddedWindow(GetEmbeddedWindowContainer());
      }
      if (capture_window == nullptr) {
        std::wcerr << L"ERROR: No valid window available for frame capture.\n";
        DebugTraceLog(L"Frame %d: no capture window available", frame);
        capture_failed = true;
        DebugTraceLog(L"Frame %d: end (capture failure)", frame);
        break;
      }
      DebugTraceLog(L"Frame %d: capture target window=%p", frame,
                    capture_window);
      if (!capture_child_to_rgba(capture_window, options.width, options.height,
                                 frame_rgba)) {
        if (options.diagnostics_fallback_enabled &&
            !DebugTraceIsFallbackActiveForWindow(host.child)) {
          DebugTraceLog(
              L"Frame %d: enabling diagnostics fallback after window capture"
              L" failure",
              frame);
          DebugTraceActivateFallbackForWindow(host.child);
          DebugTraceResetDiagnosticsBuffer();
          diagnostics_generation = 0;
          DebugTraceLog(
              L"Frame %d: retrying with diagnostics fallback enabled", frame);
          --frame;
          continue;
        }
        std::wcerr << L"ERROR: Failed to capture frame " << frame << L".\n";
        DebugTraceLog(L"Frame %d: window capture failed", frame);
        capture_failed = true;
        DebugTraceLog(L"Frame %d: end (capture failure)", frame);
        break;
      }
      DebugTraceLog(L"Frame %d: captured from window DC", frame);
    }

    if (options.diagnostics_fallback_enabled) {
      const bool has_content = captured && FrameHasVisiblePixels(frame_rgba);
      const uint64_t peek_generation = DebugTracePeekDiagnosticsGeneration();
      if ((!captured || !has_content || !used_trace_capture) && peek_generation != 0 &&
          peek_generation != diagnostics_generation) {
        std::vector<uint8_t> diagnostics_rgba;
        uint64_t next_generation = diagnostics_generation;
        if (DebugTraceFetchLastFrame(next_generation, diagnostics_rgba)) {
          if (!diagnostics_rgba.empty()) {
            frame_rgba.swap(diagnostics_rgba);
            captured = true;
            DebugTraceLog(
                L"Frame %d: diagnostics buffer used after fallback capture (generation=%llu)",
                frame, static_cast<unsigned long long>(next_generation));
          }
          diagnostics_generation = next_generation;
          used_trace_capture = true;
        }
      }
    }

    if (avi_enabled && !avi_failed) {
      if (!avi_opened) {
        if (!avi_writer.Open(avi_output_path.wstring(), options.width,
                             options.height, effective_fps)) {
          avi_failed = true;
          DebugTraceLog(L"Frame %d: failed to open AVI output", frame);
          DebugTraceLog(L"Frame %d: end (AVI open failure)", frame);
          break;
        }
        avi_opened = true;
        DebugTraceLog(L"Opened AVI output %s", avi_output_path.wstring().c_str());
      }
      if (!avi_writer.WriteFrame(frame_rgba.data(), frame_rgba.size())) {
        avi_failed = true;
        DebugTraceLog(L"Frame %d: failed to append frame to AVI", frame);
        DebugTraceLog(L"Frame %d: end (AVI write failure)", frame);
        break;
      }
    }

    const std::array<uint8_t, 32> frame_digest = Sha256Hash(frame_rgba);
    const std::string frame_hex = Sha256ToHex(frame_digest);
    const std::string csv_line = std::to_string(frame) + "," + frame_hex + "\n";
    if (!WriteAllBytes(per_frame_file.get(), csv_line)) {
      std::wcerr << L"ERROR: Failed to write per-frame hash for frame " << frame
                 << L".\n";
      DebugTraceLog(L"Frame %d: failed to append per-frame hash", frame);
      hash_failed = true;
      DebugTraceLog(L"Frame %d: end (hash failure)", frame);
      break;
    }
    rolling_hash.Update(frame_rgba);

    if ((frame % options.png_step) == 0) {
      const std::filesystem::path png_path =
          frames_dir_path / FormatFrameFilename(frame);
      if (!write_png(png_path.wstring(), options.width, options.height,
                     frame_rgba.data())) {
        capture_failed = true;
        DebugTraceLog(L"Frame %d: failed to write PNG %s", frame,
                      png_path.wstring().c_str());
        DebugTraceLog(L"Frame %d: end (PNG failure)", frame);
        break;
      }
      png_outputs.push_back(png_path);
      std::wcout << L"Wrote " << png_path << L"\n";
      DebugTraceLog(L"Frame %d: wrote PNG %s", frame,
                    png_path.wstring().c_str());
    }

    DebugTraceLog(L"Frame %d: end", frame);
  }

  if (avi_writer.IsOpen()) {
    avi_writer.Close();
  }

  if (!capture_failed && !hash_failed && !avi_failed) {
    if (!FlushFileBuffers(per_frame_file.get())) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to flush per-frame hash file: "
                 << FormatWindowsErrorMessage(error) << L"\n";
      DebugTraceLog(L"Failed to flush per-frame hash file (error=%lu)", error);
      hash_failed = true;
    }
  }

  end_vis(host);

  unload_vis(host);

  DebugTraceResetOffscreenSurface();
  g_force_diagnostics_fallback = false;

  if (!capture_failed && !hash_failed && !avi_failed) {
    const std::array<uint8_t, 32> rolling_digest = rolling_hash.Final();
    const std::string rolling_hex = Sha256ToHex(rolling_digest);
    const std::string rolling_line = rolling_hex + "\n";

    ScopedHandle rolling_file(CreateFileW(
        rolling_hash_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!rolling_file || rolling_file.get() == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to open rolling hash file '"
                 << rolling_hash_path.wstring() << L"': "
                 << FormatWindowsErrorMessage(error) << L"\n";
      hash_failed = true;
    } else {
      if (!WriteAllBytes(rolling_file.get(), rolling_line)) {
        std::wcerr << L"ERROR: Failed to write rolling hash.\n";
        DebugTraceLog(L"Failed to write rolling hash to %s",
                      rolling_hash_path.wstring().c_str());
        hash_failed = true;
      } else if (!FlushFileBuffers(rolling_file.get())) {
        const DWORD error = GetLastError();
        std::wcerr << L"ERROR: Failed to flush rolling hash file: "
                   << FormatWindowsErrorMessage(error) << L"\n";
        DebugTraceLog(L"Failed to flush rolling hash file (error=%lu)", error);
        hash_failed = true;
      }
    }
  }

  if (capture_failed || hash_failed || avi_failed) {
    return 1;
  }

  ManifestInfo manifest;
  manifest.out_dir = out_dir_path;
  manifest.vis_dll_path = options.vis_dll;
  manifest.runtime_dir = options.runtime_dir;
  manifest.has_vis_avs_dat = !options.vis_avs_dat.empty();
  if (manifest.has_vis_avs_dat) {
    manifest.vis_avs_dat_path = options.vis_avs_dat;
  }
  manifest.has_out_dll = !options.out_dll.empty();
  if (manifest.has_out_dll) {
    manifest.out_dll_path = options.out_dll;
  }
  manifest.has_preset = !options.preset.empty();
  if (manifest.has_preset) {
    manifest.preset_path = options.preset;
  }
  manifest.wav_path = options.wav;
  manifest.wav_sample_rate = wav_sample_rate;
  manifest.wav_channels = wav_channels;
  manifest.wav_sample_count = wav_sample_count;
  manifest.width = options.width;
  manifest.height = options.height;
  manifest.fps = options.fps;
  manifest.frames = options.frames;
  manifest.png_paths = png_outputs;
  manifest.per_frame_hash_path = per_frame_csv_path;
  manifest.rolling_hash_path = rolling_hash_path;
  manifest.has_avi_output = avi_enabled && avi_opened && !avi_failed;
  if (manifest.has_avi_output) {
    manifest.avi_output_path = avi_output_path;
  }

  if (!WriteManifest(manifest)) {
    return 1;
  }

  DebugTraceLog(L"Render completed successfully");
  return 0;
}
