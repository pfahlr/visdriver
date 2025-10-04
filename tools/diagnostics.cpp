#include "diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windowsx.h>
#include <wingdi.h>

namespace diagnostics {
namespace {

struct WindowInfo {
  DWORD ex_style = 0;
  DWORD style = 0;
  std::wstring class_name;
  std::wstring window_name;
  HWND parent = nullptr;
};

enum class DcKind { kWindow, kCompatible, kFallback };

struct DcInfo {
  DcKind kind = DcKind::kWindow;
  HWND owner = nullptr;
  std::wstring note;
};

struct OffscreenSurface {
  HWND window = nullptr;
  HDC dc = nullptr;
  HBITMAP dib = nullptr;
  void *bits = nullptr;
  int width = 0;
  int height = 0;
  bool owns_window = false;
};

FILE *g_log_file = nullptr;
std::mutex g_log_mutex;
int g_target_width = 0;
int g_target_height = 0;
HWND g_primary_window = nullptr;
HWND g_parent_window = nullptr;
std::unordered_map<HWND, WindowInfo> g_windows;
std::unordered_map<HDC, DcInfo> g_dcs;
OffscreenSurface g_offscreen;

thread_local int g_suppression_depth = 0;

using CreateWindowExWPtr = decltype(&CreateWindowExW);
using CreateWindowExAPtr = decltype(&CreateWindowExA);
using DestroyWindowPtr = decltype(&DestroyWindow);
using GetDCPtr = decltype(&GetDC);
using GetDCExPtr = decltype(&GetDCEx);
using ReleaseDCPtr = decltype(&ReleaseDC);
using CreateCompatibleDCPtr = decltype(&CreateCompatibleDC);
using DeleteDCPtr = decltype(&DeleteDC);
using BitBltPtr = decltype(&BitBlt);
using SwapBuffersPtr = BOOL(WINAPI *)(HDC);
using BeginPaintPtr = decltype(&BeginPaint);
using EndPaintPtr = decltype(&EndPaint);
using ChoosePixelFormatPtr = decltype(&ChoosePixelFormat);
using SetPixelFormatPtr = decltype(&SetPixelFormat);
using WglCreateContextPtr = HGLRC(WINAPI *)(HDC);
using WglMakeCurrentPtr = BOOL(WINAPI *)(HDC, HGLRC);
using WglDeleteContextPtr = BOOL(WINAPI *)(HGLRC);

CreateWindowExWPtr g_orig_CreateWindowExW = nullptr;
CreateWindowExAPtr g_orig_CreateWindowExA = nullptr;
DestroyWindowPtr g_orig_DestroyWindow = nullptr;
GetDCPtr g_orig_GetDC = nullptr;
GetDCExPtr g_orig_GetDCEx = nullptr;
ReleaseDCPtr g_orig_ReleaseDC = nullptr;
CreateCompatibleDCPtr g_orig_CreateCompatibleDC = nullptr;
DeleteDCPtr g_orig_DeleteDC = nullptr;
BitBltPtr g_orig_BitBlt = nullptr;
SwapBuffersPtr g_orig_SwapBuffers = nullptr;
BeginPaintPtr g_orig_BeginPaint = nullptr;
EndPaintPtr g_orig_EndPaint = nullptr;
ChoosePixelFormatPtr g_orig_ChoosePixelFormat = nullptr;
SetPixelFormatPtr g_orig_SetPixelFormat = nullptr;
WglCreateContextPtr g_orig_wglCreateContext = nullptr;
WglMakeCurrentPtr g_orig_wglMakeCurrent = nullptr;
WglDeleteContextPtr g_orig_wglDeleteContext = nullptr;

class ScopedLock {
public:
  explicit ScopedLock(std::mutex &m) : mutex_(m) { mutex_.lock(); }
  ~ScopedLock() { mutex_.unlock(); }

private:
  std::mutex &mutex_;
};

bool LoggingSuppressed() { return g_suppression_depth > 0; }

std::string ToUtf8(const wchar_t *text) {
  if (text == nullptr) {
    return std::string();
  }
  const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                          nullptr, nullptr);
  if (required <= 0) {
    return std::string();
  }
  std::string utf8;
  utf8.resize(static_cast<size_t>(required));
  WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), required, nullptr,
                      nullptr);
  if (!utf8.empty() && utf8.back() == '\0') {
    utf8.pop_back();
  }
  return utf8;
}

std::string ToUtf8FromAnsi(const char *text) {
  if (text == nullptr) {
    return std::string();
  }
  const int wide_required =
      MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (wide_required <= 0) {
    return std::string();
  }
  std::wstring wide;
  wide.resize(static_cast<size_t>(wide_required));
  MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, -1, wide.data(),
                      wide_required);
  if (!wide.empty() && wide.back() == L'\0') {
    wide.pop_back();
  }
  return ToUtf8(wide.c_str());
}

std::wstring WideFromAnsi(const char *text) {
  if (text == nullptr) {
    return std::wstring();
  }
  const int required =
      MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (required <= 0) {
    return std::wstring();
  }
  std::wstring wide;
  wide.resize(static_cast<size_t>(required));
  MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, -1, wide.data(),
                      required);
  if (!wide.empty() && wide.back() == L'\0') {
    wide.pop_back();
  }
  return wide;
}

void LogMessageV(const char *fmt, va_list args) {
  if (g_log_file == nullptr) {
    return;
  }
  va_list copy;
  va_copy(copy, args);
  const int required = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (required < 0) {
    return;
  }
  std::string line;
  line.resize(static_cast<size_t>(required));
  std::vsnprintf(line.data(), static_cast<size_t>(required) + 1, fmt, args);

  SYSTEMTIME st;
  GetLocalTime(&st);

  ScopedLock lock(g_log_mutex);
  std::fprintf(g_log_file,
               "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, line.c_str());
  std::fflush(g_log_file);
}

void LogMessage(const char *fmt, ...) {
  if (LoggingSuppressed()) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  LogMessageV(fmt, args);
  va_end(args);
}

void TrackWindow(HWND hwnd, DWORD ex_style, DWORD style,
                 const std::wstring &class_name,
                 const std::wstring &window_name, HWND parent) {
  if (hwnd == nullptr) {
    return;
  }
  WindowInfo info;
  info.ex_style = ex_style;
  info.style = style;
  info.class_name = class_name;
  info.window_name = window_name;
  info.parent = parent;
  g_windows[hwnd] = info;
}

void ForgetWindow(HWND hwnd) {
  g_windows.erase(hwnd);
}

void TrackDc(HDC dc, DcKind kind, HWND owner, const std::wstring &note) {
  if (dc == nullptr) {
    return;
  }
  DcInfo info;
  info.kind = kind;
  info.owner = owner;
  info.note = note;
  g_dcs[dc] = info;
}

void ForgetDc(HDC dc) { g_dcs.erase(dc); }

bool EnsureOffscreenClassRegistered() {
  static bool registered = false;
  if (registered) {
    return true;
  }
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpszClassName = L"visdriver.offscreen";
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  if (RegisterClassExW(&cls) == 0) {
    const DWORD err = GetLastError();
    LogMessage("RegisterClassExW(visdriver.offscreen) failed: %lu", err);
    return false;
  }
  registered = true;
  return true;
}

bool EnsureOffscreenBitmap(int width, int height) {
  if (g_offscreen.dc == nullptr) {
    g_offscreen.dc = CreateCompatibleDC(nullptr);
    if (g_offscreen.dc == nullptr) {
      LogMessage("CreateCompatibleDC(nullptr) for fallback failed (%lu)",
                 GetLastError());
      return false;
    }
    TrackDc(g_offscreen.dc, DcKind::kFallback, g_offscreen.window,
            L"fallback memory dc");
  }

  if (g_offscreen.dib != nullptr) {
    if (g_offscreen.width == width && g_offscreen.height == height) {
      return true;
    }
  }

  BITMAPINFO bmi;
  std::memset(&bmi, 0, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *bits = nullptr;
  HBITMAP dib = CreateDIBSection(g_offscreen.dc, &bmi, DIB_RGB_COLORS, &bits,
                                 nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    LogMessage("CreateDIBSection for fallback failed (%lu)", GetLastError());
    return false;
  }
  HGDIOBJ old = SelectObject(g_offscreen.dc, dib);
  if (old == nullptr || old == HGDI_ERROR) {
    LogMessage("SelectObject for fallback failed (%lu)", GetLastError());
    DeleteObject(dib);
    return false;
  }
  if (g_offscreen.dib != nullptr && old != nullptr && old != HGDI_ERROR) {
    DeleteObject(static_cast<HBITMAP>(old));
  }
  g_offscreen.dib = dib;
  g_offscreen.bits = bits;
  g_offscreen.width = width;
  g_offscreen.height = height;
  return true;
}

HWND EnsureOffscreenWindow(int width, int height) {
  if (!EnsureOffscreenClassRegistered()) {
    return nullptr;
  }
  if (g_offscreen.window != nullptr) {
    return g_offscreen.window;
  }
  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"visdriver.offscreen",
      L"visdriver fallback", WS_POPUP, 0, 0, width, height, nullptr, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (hwnd == nullptr) {
    LogMessage("CreateWindowExW for fallback failed (%lu)", GetLastError());
    return nullptr;
  }
  g_offscreen.window = hwnd;
  g_offscreen.owns_window = true;
  TrackWindow(hwnd, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP,
              L"visdriver.offscreen", L"visdriver fallback", nullptr);
  return hwnd;
}

bool IsFallbackActive() { return g_offscreen.window != nullptr; }

HDC AcquireFallbackDc(HWND hwnd) {
  if (!IsFallbackActive() || hwnd != g_offscreen.window) {
    return nullptr;
  }
  return g_offscreen.dc;
}

void DestroyFallback() {
  if (g_offscreen.dib != nullptr) {
    DeleteObject(g_offscreen.dib);
    g_offscreen.dib = nullptr;
  }
  if (g_offscreen.dc != nullptr) {
    ForgetDc(g_offscreen.dc);
    DeleteDC(g_offscreen.dc);
    g_offscreen.dc = nullptr;
  }
  if (g_offscreen.window != nullptr) {
    if (g_offscreen.owns_window) {
      ForgetWindow(g_offscreen.window);
      DestroyWindow(g_offscreen.window);
    }
    g_offscreen.window = nullptr;
  }
  g_offscreen.bits = nullptr;
  g_offscreen.width = 0;
  g_offscreen.height = 0;
  g_offscreen.owns_window = false;
}

// Hook implementations -----------------------------------------------------

HWND WINAPI Hooked_CreateWindowExW(DWORD ex_style, LPCWSTR class_name,
                                   LPCWSTR window_name, DWORD style, int x,
                                   int y, int width, int height, HWND parent,
                                   HMENU menu, HINSTANCE instance,
                                   LPVOID param) {
  if (g_orig_CreateWindowExW == nullptr) {
    return nullptr;
  }
  HWND hwnd = g_orig_CreateWindowExW(ex_style, class_name, window_name, style,
                                     x, y, width, height, parent, menu,
                                     instance, param);
  if (!LoggingSuppressed()) {
    std::wstring cls = class_name ? class_name : L"";
    std::wstring name = window_name ? window_name : L"";
    LogMessage("CreateWindowExW class='%s' name='%s' ex=0x%08lX style=0x%08lX -> %p",
               ToUtf8(cls.c_str()).c_str(), ToUtf8(name.c_str()).c_str(),
               ex_style, style, hwnd);
    TrackWindow(hwnd, ex_style, style, cls, name, parent);
  }
  return hwnd;
}

HWND WINAPI Hooked_CreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                   LPCSTR window_name, DWORD style, int x,
                                   int y, int width, int height, HWND parent,
                                   HMENU menu, HINSTANCE instance,
                                   LPVOID param) {
  if (g_orig_CreateWindowExA == nullptr) {
    return nullptr;
  }
  HWND hwnd = g_orig_CreateWindowExA(ex_style, class_name, window_name, style,
                                     x, y, width, height, parent, menu,
                                     instance, param);
  if (!LoggingSuppressed()) {
    LogMessage(
        "CreateWindowExA class='%s' name='%s' ex=0x%08lX style=0x%08lX -> %p",
        ToUtf8FromAnsi(class_name).c_str(), ToUtf8FromAnsi(window_name).c_str(),
        ex_style, style, hwnd);
    std::wstring cls = WideFromAnsi(class_name);
    std::wstring name = WideFromAnsi(window_name);
    TrackWindow(hwnd, ex_style, style, cls, name, parent);
  }
  return hwnd;
}

BOOL WINAPI Hooked_DestroyWindow(HWND hwnd) {
  if (g_orig_DestroyWindow == nullptr) {
    return FALSE;
  }
  BOOL result = g_orig_DestroyWindow(hwnd);
  if (!LoggingSuppressed()) {
    LogMessage("DestroyWindow(%p) -> %d", hwnd, result);
    ForgetWindow(hwnd);
  }
  return result;
}

HDC WINAPI Hooked_GetDC(HWND hwnd) {
  if (g_orig_GetDC == nullptr) {
    return nullptr;
  }
  if (HDC fallback = AcquireFallbackDc(hwnd)) {
    LogMessage("GetDC(%p) -> fallback %p", hwnd, fallback);
    return fallback;
  }
  HDC dc = g_orig_GetDC(hwnd);
  if (!LoggingSuppressed()) {
    LogMessage("GetDC(%p) -> %p", hwnd, dc);
    TrackDc(dc, DcKind::kWindow, hwnd, L"GetDC");
  }
  return dc;
}

HDC WINAPI Hooked_GetDCEx(HWND hwnd, HRGN hrgn, DWORD flags) {
  if (g_orig_GetDCEx == nullptr) {
    return nullptr;
  }
  if (HDC fallback = AcquireFallbackDc(hwnd)) {
    LogMessage("GetDCEx(%p) -> fallback %p", hwnd, fallback);
    return fallback;
  }
  HDC dc = g_orig_GetDCEx(hwnd, hrgn, flags);
  if (!LoggingSuppressed()) {
    LogMessage("GetDCEx(%p) flags=0x%08lX -> %p", hwnd, flags, dc);
    TrackDc(dc, DcKind::kWindow, hwnd, L"GetDCEx");
  }
  return dc;
}

int WINAPI Hooked_ReleaseDC(HWND hwnd, HDC dc) {
  if (AcquireFallbackDc(hwnd) == dc) {
    LogMessage("ReleaseDC(%p, fallback %p)", hwnd, dc);
    return 1;
  }
  if (g_orig_ReleaseDC == nullptr) {
    return 0;
  }
  int result = g_orig_ReleaseDC(hwnd, dc);
  if (!LoggingSuppressed()) {
    LogMessage("ReleaseDC(%p, %p) -> %d", hwnd, dc, result);
    ForgetDc(dc);
  }
  return result;
}

HDC WINAPI Hooked_CreateCompatibleDC(HDC existing) {
  if (g_orig_CreateCompatibleDC == nullptr) {
    return nullptr;
  }
  HDC dc = g_orig_CreateCompatibleDC(existing);
  if (!LoggingSuppressed()) {
    LogMessage("CreateCompatibleDC(%p) -> %p", existing, dc);
    TrackDc(dc, DcKind::kCompatible, nullptr, L"CreateCompatibleDC");
  }
  return dc;
}

BOOL WINAPI Hooked_DeleteDC(HDC dc) {
  if (dc != nullptr && dc == g_offscreen.dc) {
    LogMessage("DeleteDC(fallback %p)", dc);
    return TRUE;
  }
  if (g_orig_DeleteDC == nullptr) {
    return FALSE;
  }
  BOOL result = g_orig_DeleteDC(dc);
  if (!LoggingSuppressed()) {
    LogMessage("DeleteDC(%p) -> %d", dc, result);
    ForgetDc(dc);
  }
  return result;
}

BOOL WINAPI Hooked_BitBlt(HDC dest, int x, int y, int width, int height,
                          HDC src, int src_x, int src_y, DWORD rop) {
  if (g_orig_BitBlt == nullptr) {
    return FALSE;
  }
  BOOL result = g_orig_BitBlt(dest, x, y, width, height, src, src_x, src_y, rop);
  if (!LoggingSuppressed()) {
    LogMessage("BitBlt(dest=%p, src=%p, size=%dx%d, rop=0x%08lX) -> %d", dest,
               src, width, height, rop, result);
  }
  return result;
}

BOOL WINAPI Hooked_SwapBuffers(HDC dc) {
  if (g_orig_SwapBuffers == nullptr) {
    return FALSE;
  }
  BOOL result = g_orig_SwapBuffers(dc);
  if (!LoggingSuppressed()) {
    HWND hwnd = WindowFromDC(dc);
    LogMessage("SwapBuffers(%p -> hwnd %p) -> %d", dc, hwnd, result);
  }
  return result;
}

HDC WINAPI Hooked_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
  if (AcquireFallbackDc(hwnd) != nullptr) {
    if (ps != nullptr) {
      std::memset(ps, 0, sizeof(*ps));
      ps->hdc = g_offscreen.dc;
      ps->rcPaint.left = 0;
      ps->rcPaint.top = 0;
      ps->rcPaint.right = g_offscreen.width;
      ps->rcPaint.bottom = g_offscreen.height;
      ps->fErase = TRUE;
    }
    LogMessage("BeginPaint(%p) -> fallback %p", hwnd, g_offscreen.dc);
    return g_offscreen.dc;
  }
  if (g_orig_BeginPaint == nullptr) {
    return nullptr;
  }
  HDC dc = g_orig_BeginPaint(hwnd, ps);
  if (!LoggingSuppressed()) {
    LogMessage("BeginPaint(%p) -> %p", hwnd, dc);
    TrackDc(dc, DcKind::kWindow, hwnd, L"BeginPaint");
  }
  return dc;
}

BOOL WINAPI Hooked_EndPaint(HWND hwnd, const PAINTSTRUCT *ps) {
  if (AcquireFallbackDc(hwnd) != nullptr) {
    LogMessage("EndPaint(%p, fallback)", hwnd);
    return TRUE;
  }
  if (g_orig_EndPaint == nullptr) {
    return FALSE;
  }
  BOOL result = g_orig_EndPaint(hwnd, ps);
  if (!LoggingSuppressed()) {
    LogMessage("EndPaint(%p) -> %d", hwnd, result);
  }
  return result;
}

int WINAPI Hooked_ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) {
  if (g_orig_ChoosePixelFormat == nullptr) {
    return 0;
  }
  int idx = g_orig_ChoosePixelFormat(dc, pfd);
  if (!LoggingSuppressed()) {
    LogMessage("ChoosePixelFormat(%p) -> %d", dc, idx);
  }
  return idx;
}

BOOL WINAPI Hooked_SetPixelFormat(HDC dc, int format,
                                  const PIXELFORMATDESCRIPTOR *pfd) {
  if (g_orig_SetPixelFormat == nullptr) {
    return FALSE;
  }
  BOOL ok = g_orig_SetPixelFormat(dc, format, pfd);
  if (!LoggingSuppressed()) {
    LogMessage("SetPixelFormat(%p, %d) -> %d", dc, format, ok);
  }
  return ok;
}

HGLRC WINAPI Hooked_wglCreateContext(HDC dc) {
  if (g_orig_wglCreateContext == nullptr) {
    return nullptr;
  }
  HGLRC ctx = g_orig_wglCreateContext(dc);
  if (!LoggingSuppressed()) {
    LogMessage("wglCreateContext(%p) -> %p", dc, ctx);
  }
  return ctx;
}

BOOL WINAPI Hooked_wglMakeCurrent(HDC dc, HGLRC ctx) {
  if (g_orig_wglMakeCurrent == nullptr) {
    return FALSE;
  }
  BOOL ok = g_orig_wglMakeCurrent(dc, ctx);
  if (!LoggingSuppressed()) {
    LogMessage("wglMakeCurrent(dc=%p, ctx=%p) -> %d", dc, ctx, ok);
  }
  return ok;
}

BOOL WINAPI Hooked_wglDeleteContext(HGLRC ctx) {
  if (g_orig_wglDeleteContext == nullptr) {
    return FALSE;
  }
  BOOL ok = g_orig_wglDeleteContext(ctx);
  if (!LoggingSuppressed()) {
    LogMessage("wglDeleteContext(%p) -> %d", ctx, ok);
  }
  return ok;
}

struct HookEntry {
  const char *dll;
  const char *name;
  void *replacement;
};

bool PatchImport(HMODULE module, const HookEntry &entry) {
  if (module == nullptr) {
    return false;
  }
  BYTE *const base = reinterpret_cast<BYTE *>(module);
  const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }
  const IMAGE_NT_HEADERS *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  const DWORD import_rva =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .VirtualAddress;
  if (import_rva == 0) {
    return false;
  }
  auto *desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + import_rva);
  for (; desc->Name != 0; ++desc) {
    const char *dll_name = reinterpret_cast<const char *>(base + desc->Name);
    if (_stricmp(dll_name, entry.dll) != 0) {
      continue;
    }
    auto *thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);
    PIMAGE_THUNK_DATA orig_thunk = nullptr;
    if (desc->OriginalFirstThunk != 0) {
      orig_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base +
                                                       desc->OriginalFirstThunk);
    } else {
      orig_thunk = thunk;
    }
    for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) {
        continue;
      }
      auto *import = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
          base + orig_thunk->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char *>(import->Name),
                      entry.name) != 0) {
        continue;
      }
      DWORD old_protect = 0;
      if (!VirtualProtect(&thunk->u1.Function, sizeof(void *),
                          PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
      }
      thunk->u1.Function = reinterpret_cast<ULONG_PTR>(entry.replacement);
      VirtualProtect(&thunk->u1.Function, sizeof(void *), old_protect,
                     &old_protect);
      FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function,
                            sizeof(void *));
      return true;
    }
  }
  return false;
}

bool InstallHooks(HMODULE module) {
  const HookEntry entries[] = {
      {"USER32.dll", "CreateWindowExW", (void *)Hooked_CreateWindowExW},
      {"USER32.dll", "CreateWindowExA", (void *)Hooked_CreateWindowExA},
      {"USER32.dll", "DestroyWindow", (void *)Hooked_DestroyWindow},
      {"USER32.dll", "GetDC", (void *)Hooked_GetDC},
      {"USER32.dll", "GetDCEx", (void *)Hooked_GetDCEx},
      {"USER32.dll", "ReleaseDC", (void *)Hooked_ReleaseDC},
      {"USER32.dll", "BeginPaint", (void *)Hooked_BeginPaint},
      {"USER32.dll", "EndPaint", (void *)Hooked_EndPaint},
      {"GDI32.dll", "CreateCompatibleDC", (void *)Hooked_CreateCompatibleDC},
      {"GDI32.dll", "DeleteDC", (void *)Hooked_DeleteDC},
      {"GDI32.dll", "BitBlt", (void *)Hooked_BitBlt},
      {"GDI32.dll", "SwapBuffers", (void *)Hooked_SwapBuffers},
      {"GDI32.dll", "ChoosePixelFormat", (void *)Hooked_ChoosePixelFormat},
      {"GDI32.dll", "SetPixelFormat", (void *)Hooked_SetPixelFormat},
      {"OPENGL32.dll", "wglCreateContext", (void *)Hooked_wglCreateContext},
      {"OPENGL32.dll", "wglMakeCurrent", (void *)Hooked_wglMakeCurrent},
      {"OPENGL32.dll", "wglDeleteContext", (void *)Hooked_wglDeleteContext},
  };
  bool hooked_any = false;
  for (const HookEntry &entry : entries) {
    if (PatchImport(module, entry)) {
      hooked_any = true;
    }
  }
  return hooked_any;
}

void InitializeOriginalPointers() {
  if (g_orig_CreateWindowExW == nullptr) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
      user32 = LoadLibraryW(L"user32.dll");
    }
    if (user32 != nullptr) {
      g_orig_CreateWindowExW = reinterpret_cast<CreateWindowExWPtr>(
          GetProcAddress(user32, "CreateWindowExW"));
      g_orig_CreateWindowExA = reinterpret_cast<CreateWindowExAPtr>(
          GetProcAddress(user32, "CreateWindowExA"));
      g_orig_DestroyWindow = reinterpret_cast<DestroyWindowPtr>(
          GetProcAddress(user32, "DestroyWindow"));
      g_orig_GetDC = reinterpret_cast<GetDCPtr>(
          GetProcAddress(user32, "GetDC"));
      g_orig_GetDCEx = reinterpret_cast<GetDCExPtr>(
          GetProcAddress(user32, "GetDCEx"));
      g_orig_ReleaseDC = reinterpret_cast<ReleaseDCPtr>(
          GetProcAddress(user32, "ReleaseDC"));
      g_orig_BeginPaint = reinterpret_cast<BeginPaintPtr>(
          GetProcAddress(user32, "BeginPaint"));
      g_orig_EndPaint = reinterpret_cast<EndPaintPtr>(
          GetProcAddress(user32, "EndPaint"));
    }
  }
  if (g_orig_CreateCompatibleDC == nullptr) {
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (gdi32 == nullptr) {
      gdi32 = LoadLibraryW(L"gdi32.dll");
    }
    if (gdi32 != nullptr) {
      g_orig_CreateCompatibleDC = reinterpret_cast<CreateCompatibleDCPtr>(
          GetProcAddress(gdi32, "CreateCompatibleDC"));
      g_orig_DeleteDC = reinterpret_cast<DeleteDCPtr>(
          GetProcAddress(gdi32, "DeleteDC"));
      g_orig_BitBlt = reinterpret_cast<BitBltPtr>(
          GetProcAddress(gdi32, "BitBlt"));
      g_orig_SwapBuffers = reinterpret_cast<SwapBuffersPtr>(
          GetProcAddress(gdi32, "SwapBuffers"));
      g_orig_ChoosePixelFormat = reinterpret_cast<ChoosePixelFormatPtr>(
          GetProcAddress(gdi32, "ChoosePixelFormat"));
      g_orig_SetPixelFormat = reinterpret_cast<SetPixelFormatPtr>(
          GetProcAddress(gdi32, "SetPixelFormat"));
    }
  }
  if (g_orig_wglCreateContext == nullptr || g_orig_wglMakeCurrent == nullptr ||
      g_orig_wglDeleteContext == nullptr) {
    HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");
    if (opengl32 != nullptr || (opengl32 = LoadLibraryW(L"opengl32.dll")) != nullptr) {
      if (g_orig_wglCreateContext == nullptr) {
        g_orig_wglCreateContext = reinterpret_cast<WglCreateContextPtr>(
            GetProcAddress(opengl32, "wglCreateContext"));
      }
      if (g_orig_wglMakeCurrent == nullptr) {
        g_orig_wglMakeCurrent = reinterpret_cast<WglMakeCurrentPtr>(
            GetProcAddress(opengl32, "wglMakeCurrent"));
      }
      if (g_orig_wglDeleteContext == nullptr) {
        g_orig_wglDeleteContext = reinterpret_cast<WglDeleteContextPtr>(
            GetProcAddress(opengl32, "wglDeleteContext"));
      }
    }
  }
}

} // namespace

ScopedSuppressLogs::ScopedSuppressLogs() { ++g_suppression_depth; }
ScopedSuppressLogs::~ScopedSuppressLogs() { --g_suppression_depth; }

bool Initialize(const std::wstring &log_path, int target_width,
                int target_height) {
  ScopedLock lock(g_log_mutex);
  if (g_log_file != nullptr) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  g_log_file = _wfopen(log_path.c_str(), L"w, ccs=UTF-8");
  if (g_log_file == nullptr) {
    return false;
  }
  g_target_width = target_width;
  g_target_height = target_height;
  g_windows.clear();
  g_dcs.clear();
  g_primary_window = nullptr;
  g_parent_window = nullptr;
  DestroyFallback();
  InitializeOriginalPointers();
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(g_log_file,
               "# visdriver diagnostics log started %04u-%02u-%02u %02u:%02u:%02u.%03u\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);
  std::fprintf(g_log_file, "# target size: %dx%d\n", target_width,
               target_height);
  std::fflush(g_log_file);
  return true;
}

void Shutdown() {
  DestroyFallback();
  ScopedLock lock(g_log_mutex);
  if (g_log_file != nullptr) {
    std::fprintf(g_log_file, "# visdriver diagnostics log closed\n");
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  g_windows.clear();
  g_dcs.clear();
}

bool HookModule(HMODULE module) {
  InitializeOriginalPointers();
  bool result = InstallHooks(module);
  if (result) {
    LogMessage("Hooks installed for module %p", module);
  }
  return result;
}

void RegisterPrimaryWindow(HWND hwnd) {
  g_primary_window = hwnd;
  LogMessage("Registered primary window: %p", hwnd);
}

void RegisterParentWindow(HWND hwnd) {
  g_parent_window = hwnd;
  LogMessage("Registered parent window: %p", hwnd);
}

void RegisterModuleLoad(const std::wstring &path, HMODULE module) {
  LogMessage("Loaded module %p from %s", module, ToUtf8(path.c_str()).c_str());
}

void NoteVisStep(const char *step) { LogMessage("VIS: %s", step); }

void NoteVisStep(const char *step, int frame_index) {
  LogMessage("VIS: frame %d: %s", frame_index, step);
}

HWND EnsureOffscreenFallbackWindow(int width, int height) {
  HWND hwnd = EnsureOffscreenWindow(width, height);
  if (hwnd == nullptr) {
    return nullptr;
  }
  if (!EnsureOffscreenBitmap(width, height)) {
    DestroyFallback();
    return nullptr;
  }
  LogMessage("Activated fallback offscreen surface %p (%dx%d)", hwnd, width,
             height);
  return hwnd;
}

bool IsFallbackWindow(HWND hwnd) {
  return hwnd != nullptr && hwnd == g_offscreen.window;
}

bool CaptureFallbackIfActive(std::vector<uint8_t> &rgba_out) {
  if (!IsFallbackActive() || g_offscreen.bits == nullptr) {
    return false;
  }
  const size_t pixel_count = static_cast<size_t>(g_offscreen.width) *
                             static_cast<size_t>(g_offscreen.height);
  rgba_out.resize(pixel_count * 4);
  const uint8_t *src = static_cast<const uint8_t *>(g_offscreen.bits);
  uint8_t *dst = rgba_out.data();
  for (size_t i = 0; i < pixel_count; ++i) {
    const size_t offset = i * 4;
    dst[offset + 0] = src[offset + 2];
    dst[offset + 1] = src[offset + 1];
    dst[offset + 2] = src[offset + 0];
    dst[offset + 3] = src[offset + 3];
  }
  return true;
}

bool ActivateFallbackForWindow(HWND hwnd, int width, int height) {
  if (hwnd == nullptr) {
    return false;
  }
  g_offscreen.window = hwnd;
  g_offscreen.owns_window = false;
  if (!EnsureOffscreenBitmap(width, height)) {
    g_offscreen.window = nullptr;
    return false;
  }
  auto it = g_dcs.find(g_offscreen.dc);
  if (it != g_dcs.end()) {
    it->second.owner = hwnd;
  }
  LogMessage("Attached fallback surface to existing window %p (%dx%d)", hwnd,
             width, height);
  return true;
}

} // namespace diagnostics
