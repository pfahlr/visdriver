#include "debug_trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <wingdi.h>
#include <winuser.h>

namespace {

struct HandleInfo {
  std::wstring type;
  std::wstring detail;
};

std::mutex g_log_mutex;
HANDLE g_log_file = INVALID_HANDLE_VALUE;
std::atomic<bool> g_log_initialized{false};

std::mutex g_handle_mutex;
std::unordered_map<UINT_PTR, HandleInfo> g_handle_info;

std::mutex g_target_mutex;
std::unordered_set<HWND> g_target_windows;

struct OffscreenSurface {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HBITMAP old_bitmap = nullptr;
  void *bits = nullptr;
  int width = 0;
  int height = 0;
};

std::mutex g_surface_mutex;
OffscreenSurface g_offscreen_surface;

std::mutex g_hook_mutex;
std::unordered_set<HMODULE> g_hooked_modules;

std::wstring FormatPointer(UINT_PTR value) {
  wchar_t buffer[32];
  std::swprintf(buffer, std::size(buffer), L"0x%p", reinterpret_cast<void *>(value));
  return buffer;
}

std::wstring FormatHwnd(HWND hwnd) {
  return FormatPointer(reinterpret_cast<UINT_PTR>(hwnd));
}

std::wstring FormatHdc(HDC dc) {
  return FormatPointer(reinterpret_cast<UINT_PTR>(dc));
}

std::wstring Widen(const char *text) {
  if (text == nullptr) {
    return L"(null)";
  }
  const int required = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
  if (required <= 0) {
    return L"(ansi conversion error)";
  }
  std::wstring result(static_cast<size_t>(required - 1), L'\0');
  if (!result.empty()) {
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), required);
  }
  return result;
}

std::wstring FormatClassName(LPCTSTR class_name) {
  if (class_name == nullptr) {
    return L"(null)";
  }
  if (!IS_INTRESOURCE(class_name)) {
#if defined(UNICODE)
    return std::wstring(class_name);
#else
    return Widen(class_name);
#endif
  }
  const WORD atom = LOWORD(reinterpret_cast<ULONG_PTR>(class_name));
  wchar_t buffer[32];
  std::swprintf(buffer, std::size(buffer), L"#%u", static_cast<unsigned>(atom));
  return buffer;
}

std::wstring FormatWide(const wchar_t *text) {
  if (text == nullptr) {
    return L"(null)";
  }
  return std::wstring(text);
}

std::wstring GetTimestamp() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t buffer[64];
  std::swprintf(buffer, std::size(buffer), L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
  return buffer;
}

void WriteLogLineUnlocked(const std::wstring &line) {
  if (g_log_file == INVALID_HANDLE_VALUE) {
    return;
  }
  std::wstring full = line;
  full.push_back(L'\n');

  const int required = WideCharToMultiByte(CP_UTF8, 0, full.c_str(),
                                           static_cast<int>(full.size()), nullptr, 0,
                                           nullptr, nullptr);
  if (required <= 0) {
    return;
  }
  std::string utf8(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()),
                      utf8.data(), required, nullptr, nullptr);

  DWORD written = 0;
  WriteFile(g_log_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
            nullptr);
}

void TrackHandle(UINT_PTR value, const std::wstring &type,
                 const std::wstring &detail) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  g_handle_info[value] = HandleInfo{type, detail};
}

void UntrackHandle(UINT_PTR value) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  g_handle_info.erase(value);
}

std::wstring DescribeHandle(UINT_PTR value) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  auto it = g_handle_info.find(value);
  if (it == g_handle_info.end()) {
    return FormatPointer(value);
  }
  std::wstring result = it->second.type;
  result.push_back(L' ');
  result += FormatPointer(value);
  if (!it->second.detail.empty()) {
    result += L" (";
    result += it->second.detail;
    result += L")";
  }
  return result;
}

bool IsTargetWindow(HWND hwnd) {
  if (hwnd == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_target_mutex);
  return g_target_windows.find(hwnd) != g_target_windows.end();
}

bool HasOffscreenSurfaceUnlocked() {
  return g_offscreen_surface.dc != nullptr && g_offscreen_surface.bits != nullptr;
}

bool HasOffscreenSurface() {
  std::lock_guard<std::mutex> lock(g_surface_mutex);
  return HasOffscreenSurfaceUnlocked();
}

// Forward declarations of original functions.
decltype(&CreateWindowExW) g_orig_CreateWindowExW = CreateWindowExW;
decltype(&CreateWindowExA) g_orig_CreateWindowExA = CreateWindowExA;
decltype(&DestroyWindow) g_orig_DestroyWindow = DestroyWindow;
decltype(&GetDC) g_orig_GetDC = GetDC;
decltype(&GetWindowDC) g_orig_GetWindowDC = GetWindowDC;
decltype(&ReleaseDC) g_orig_ReleaseDC = ReleaseDC;
decltype(&BeginPaint) g_orig_BeginPaint = BeginPaint;
decltype(&EndPaint) g_orig_EndPaint = EndPaint;
decltype(&CreateCompatibleDC) g_orig_CreateCompatibleDC = CreateCompatibleDC;
decltype(&DeleteDC) g_orig_DeleteDC = DeleteDC;
decltype(&CreateDIBSection) g_orig_CreateDIBSection = CreateDIBSection;
decltype(&DeleteObject) g_orig_DeleteObject = DeleteObject;
decltype(&BitBlt) g_orig_BitBlt = BitBlt;
decltype(&StretchBlt) g_orig_StretchBlt = StretchBlt;
decltype(&SwapBuffers) g_orig_SwapBuffers = SwapBuffers;
decltype(&ChoosePixelFormat) g_orig_ChoosePixelFormat = ChoosePixelFormat;
decltype(&SetPixelFormat) g_orig_SetPixelFormat = SetPixelFormat;
decltype(&wglCreateContext) g_orig_wglCreateContext = wglCreateContext;
decltype(&wglMakeCurrent) g_orig_wglMakeCurrent = wglMakeCurrent;
decltype(&wglDeleteContext) g_orig_wglDeleteContext = wglDeleteContext;

template <typename... Args>
void Logf(const wchar_t *format, Args... args) {
  if (!DebugTraceIsActive()) {
    return;
  }
  const int needed = std::swprintf(nullptr, 0, format, args...);
  if (needed <= 0) {
    return;
  }
  std::wstring buffer(static_cast<size_t>(needed), L'\0');
  std::swprintf(buffer.data(), buffer.size() + 1, format, args...);
  DebugTraceLog(buffer);
}

HWND WINAPI Hooked_CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName,
                                   LPCWSTR lpWindowName, DWORD dwStyle, int X,
                                   int Y, int nWidth, int nHeight, HWND hWndParent,
                                   HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {
  DebugTraceLog(L"CreateWindowExW class=%s name=%s ex=0x%08X style=0x%08X parent=%s",
                FormatClassName(lpClassName).c_str(),
                FormatWide(lpWindowName).c_str(), dwExStyle, dwStyle,
                FormatHwnd(hWndParent).c_str());
  HWND hwnd = g_orig_CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                     X, Y, nWidth, nHeight, hWndParent, hMenu,
                                     hInstance, lpParam);
  if (hwnd != nullptr) {
    std::wstring detail = L"class=" + FormatClassName(lpClassName);
    detail += L" name=";
    detail += FormatWide(lpWindowName);
    detail += L" parent=";
    detail += FormatHwnd(hWndParent);
    TrackHandle(reinterpret_cast<UINT_PTR>(hwnd), L"HWND", detail);
  }
  DebugTraceLog(L"CreateWindowExW -> %s", FormatHwnd(hwnd).c_str());
  return hwnd;
}

HWND WINAPI Hooked_CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName,
                                   LPCSTR lpWindowName, DWORD dwStyle, int X,
                                   int Y, int nWidth, int nHeight, HWND hWndParent,
                                   HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam) {
  DebugTraceLog(L"CreateWindowExA class=%s name=%s ex=0x%08X style=0x%08X parent=%s",
                FormatClassName(reinterpret_cast<LPCTSTR>(lpClassName)).c_str(),
                Widen(lpWindowName).c_str(), dwExStyle, dwStyle,
                FormatHwnd(hWndParent).c_str());
  HWND hwnd = g_orig_CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                     X, Y, nWidth, nHeight, hWndParent, hMenu,
                                     hInstance, lpParam);
  if (hwnd != nullptr) {
    std::wstring detail = L"class=" +
                          FormatClassName(reinterpret_cast<LPCTSTR>(lpClassName));
    detail += L" name=";
    detail += Widen(lpWindowName);
    detail += L" parent=";
    detail += FormatHwnd(hWndParent);
    TrackHandle(reinterpret_cast<UINT_PTR>(hwnd), L"HWND", detail);
  }
  DebugTraceLog(L"CreateWindowExA -> %s", FormatHwnd(hwnd).c_str());
  return hwnd;
}

BOOL WINAPI Hooked_DestroyWindow(HWND hwnd) {
  DebugTraceLog(L"DestroyWindow(%s)", FormatHwnd(hwnd).c_str());
  DebugTraceUnregisterTargetWindow(hwnd);
  UntrackHandle(reinterpret_cast<UINT_PTR>(hwnd));
  return g_orig_DestroyWindow(hwnd);
}

HDC WINAPI Hooked_GetDC(HWND hwnd) {
  if (IsTargetWindow(hwnd) && HasOffscreenSurface()) {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    DebugTraceLog(L"GetDC(%s) -> offscreen %s", FormatHwnd(hwnd).c_str(),
                  FormatHdc(g_offscreen_surface.dc).c_str());
    return g_offscreen_surface.dc;
  }
  HDC dc = g_orig_GetDC(hwnd);
  if (dc != nullptr) {
    std::wstring detail = L"GetDC hwnd=";
    detail += FormatHwnd(hwnd);
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail);
  }
  DebugTraceLog(L"GetDC(%s) -> %s", FormatHwnd(hwnd).c_str(),
                FormatHdc(dc).c_str());
  return dc;
}

HDC WINAPI Hooked_GetWindowDC(HWND hwnd) {
  if (IsTargetWindow(hwnd) && HasOffscreenSurface()) {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    DebugTraceLog(L"GetWindowDC(%s) -> offscreen %s", FormatHwnd(hwnd).c_str(),
                  FormatHdc(g_offscreen_surface.dc).c_str());
    return g_offscreen_surface.dc;
  }
  HDC dc = g_orig_GetWindowDC(hwnd);
  if (dc != nullptr) {
    std::wstring detail = L"GetWindowDC hwnd=";
    detail += FormatHwnd(hwnd);
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail);
  }
  DebugTraceLog(L"GetWindowDC(%s) -> %s", FormatHwnd(hwnd).c_str(),
                FormatHdc(dc).c_str());
  return dc;
}

int WINAPI Hooked_ReleaseDC(HWND hwnd, HDC dc) {
  if (IsTargetWindow(hwnd)) {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    if (dc == g_offscreen_surface.dc && HasOffscreenSurfaceUnlocked()) {
      DebugTraceLog(L"ReleaseDC(%s, offscreen %s) [ignored]",
                    FormatHwnd(hwnd).c_str(), FormatHdc(dc).c_str());
      return 1;
    }
  }
  const int result = g_orig_ReleaseDC(hwnd, dc);
  UntrackHandle(reinterpret_cast<UINT_PTR>(dc));
  DebugTraceLog(L"ReleaseDC(%s, %s) -> %d", FormatHwnd(hwnd).c_str(),
                FormatHdc(dc).c_str(), result);
  return result;
}

HDC WINAPI Hooked_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
  if (IsTargetWindow(hwnd) && HasOffscreenSurface()) {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    if (ps != nullptr) {
      ps->hdc = g_offscreen_surface.dc;
      ps->fErase = FALSE;
      ps->rcPaint.left = 0;
      ps->rcPaint.top = 0;
      ps->rcPaint.right = g_offscreen_surface.width;
      ps->rcPaint.bottom = g_offscreen_surface.height;
    }
    DebugTraceLog(L"BeginPaint(%s) -> offscreen %s",
                  FormatHwnd(hwnd).c_str(),
                  FormatHdc(g_offscreen_surface.dc).c_str());
    return g_offscreen_surface.dc;
  }
  HDC dc = g_orig_BeginPaint(hwnd, ps);
  if (dc != nullptr) {
    std::wstring detail = L"BeginPaint hwnd=";
    detail += FormatHwnd(hwnd);
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail);
  }
  DebugTraceLog(L"BeginPaint(%s) -> %s", FormatHwnd(hwnd).c_str(),
                FormatHdc(dc).c_str());
  return dc;
}

BOOL WINAPI Hooked_EndPaint(HWND hwnd, const PAINTSTRUCT *ps) {
  if (IsTargetWindow(hwnd) && HasOffscreenSurface()) {
    DebugTraceLog(L"EndPaint(%s) [offscreen]", FormatHwnd(hwnd).c_str());
    return TRUE;
  }
  DebugTraceLog(L"EndPaint(%s)", FormatHwnd(hwnd).c_str());
  return g_orig_EndPaint(hwnd, ps);
}

HDC WINAPI Hooked_CreateCompatibleDC(HDC dc) {
  HDC result = g_orig_CreateCompatibleDC(dc);
  if (result != nullptr) {
    std::wstring detail = L"CreateCompatibleDC from=";
    detail += FormatHdc(dc);
    TrackHandle(reinterpret_cast<UINT_PTR>(result), L"HDC", detail);
  }
  DebugTraceLog(L"CreateCompatibleDC(%s) -> %s", FormatHdc(dc).c_str(),
                FormatHdc(result).c_str());
  return result;
}

BOOL WINAPI Hooked_DeleteDC(HDC dc) {
  {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    if (dc == g_offscreen_surface.dc && HasOffscreenSurfaceUnlocked()) {
      DebugTraceLog(L"DeleteDC(offscreen %s) [ignored]", FormatHdc(dc).c_str());
      return TRUE;
    }
  }
  DebugTraceLog(L"DeleteDC(%s)", FormatHdc(dc).c_str());
  UntrackHandle(reinterpret_cast<UINT_PTR>(dc));
  return g_orig_DeleteDC(dc);
}

HBITMAP WINAPI Hooked_CreateDIBSection(HDC dc, const BITMAPINFO *bmi,
                                       UINT usage, VOID **bits, HANDLE section,
                                       DWORD offset) {
  HBITMAP bitmap = g_orig_CreateDIBSection(dc, bmi, usage, bits, section, offset);
  if (bitmap != nullptr) {
    std::wstring detail = L"CreateDIBSection dc=";
    detail += FormatHdc(dc);
    if (bmi != nullptr) {
      detail += L" size=";
      detail += std::to_wstring(bmi->bmiHeader.biWidth);
      detail += L"x";
      detail += std::to_wstring(std::abs(bmi->bmiHeader.biHeight));
    }
    TrackHandle(reinterpret_cast<UINT_PTR>(bitmap), L"HBITMAP", detail);
  }
  DebugTraceLog(L"CreateDIBSection(dc=%s, usage=%u) -> %s", FormatHdc(dc).c_str(),
                usage, FormatPointer(reinterpret_cast<UINT_PTR>(bitmap)).c_str());
  return bitmap;
}

BOOL WINAPI Hooked_DeleteObject(HGDIOBJ obj) {
  DebugTraceLog(L"DeleteObject(%s)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(obj)).c_str());
  UntrackHandle(reinterpret_cast<UINT_PTR>(obj));
  return g_orig_DeleteObject(obj);
}

BOOL WINAPI Hooked_BitBlt(HDC dest, int xDest, int yDest, int width, int height,
                          HDC src, int xSrc, int ySrc, DWORD rop) {
  DebugTraceLog(L"BitBlt(dest=%s, src=%s, size=%dx%d, rop=0x%08X)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dest)).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(src)).c_str(), width,
                height, rop);
  return g_orig_BitBlt(dest, xDest, yDest, width, height, src, xSrc, ySrc, rop);
}

BOOL WINAPI Hooked_StretchBlt(HDC dest, int xDest, int yDest, int widthDest,
                              int heightDest, HDC src, int xSrc, int ySrc,
                              int widthSrc, int heightSrc, DWORD rop) {
  DebugTraceLog(L"StretchBlt(dest=%s, src=%s, size=%dx%d -> %dx%d, rop=0x%08X)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dest)).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(src)).c_str(),
                widthSrc, heightSrc, widthDest, heightDest, rop);
  return g_orig_StretchBlt(dest, xDest, yDest, widthDest, heightDest, src, xSrc,
                           ySrc, widthSrc, heightSrc, rop);
}

BOOL WINAPI Hooked_SwapBuffers(HDC dc) {
  DebugTraceLog(L"SwapBuffers(%s)", DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str());
  return g_orig_SwapBuffers(dc);
}

int WINAPI Hooked_ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) {
  DebugTraceLog(L"ChoosePixelFormat(%s)", DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str());
  return g_orig_ChoosePixelFormat(dc, pfd);
}

BOOL WINAPI Hooked_SetPixelFormat(HDC dc, int format,
                                  const PIXELFORMATDESCRIPTOR *pfd) {
  DebugTraceLog(L"SetPixelFormat(%s, %d)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str(), format);
  return g_orig_SetPixelFormat(dc, format, pfd);
}

HGLRC WINAPI Hooked_wglCreateContext(HDC dc) {
  DebugTraceLog(L"wglCreateContext(%s)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str());
  HGLRC ctx = g_orig_wglCreateContext(dc);
  TrackHandle(reinterpret_cast<UINT_PTR>(ctx), L"HGLRC",
              DescribeHandle(reinterpret_cast<UINT_PTR>(dc)));
  DebugTraceLog(L"wglCreateContext -> %s",
                DescribeHandle(reinterpret_cast<UINT_PTR>(ctx)).c_str());
  return ctx;
}

BOOL WINAPI Hooked_wglMakeCurrent(HDC dc, HGLRC ctx) {
  DebugTraceLog(L"wglMakeCurrent(dc=%s, ctx=%s)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(ctx)).c_str());
  return g_orig_wglMakeCurrent(dc, ctx);
}

BOOL WINAPI Hooked_wglDeleteContext(HGLRC ctx) {
  DebugTraceLog(L"wglDeleteContext(%s)", DescribeHandle(reinterpret_cast<UINT_PTR>(ctx)).c_str());
  UntrackHandle(reinterpret_cast<UINT_PTR>(ctx));
  return g_orig_wglDeleteContext(ctx);
}

struct HookSpec {
  const char *dll_name;
  const char *function_name;
  void *replacement;
};

const std::array<HookSpec, 20> kHookSpecs = {{{"user32.dll", "CreateWindowExW",
                                              reinterpret_cast<void *>(Hooked_CreateWindowExW)},
                                             {"user32.dll", "CreateWindowExA",
                                              reinterpret_cast<void *>(Hooked_CreateWindowExA)},
                                             {"user32.dll", "DestroyWindow",
                                              reinterpret_cast<void *>(Hooked_DestroyWindow)},
                                             {"user32.dll", "GetDC",
                                              reinterpret_cast<void *>(Hooked_GetDC)},
                                             {"user32.dll", "GetWindowDC",
                                              reinterpret_cast<void *>(Hooked_GetWindowDC)},
                                             {"user32.dll", "ReleaseDC",
                                              reinterpret_cast<void *>(Hooked_ReleaseDC)},
                                             {"user32.dll", "BeginPaint",
                                              reinterpret_cast<void *>(Hooked_BeginPaint)},
                                             {"user32.dll", "EndPaint",
                                              reinterpret_cast<void *>(Hooked_EndPaint)},
                                             {"gdi32.dll", "CreateCompatibleDC",
                                              reinterpret_cast<void *>(Hooked_CreateCompatibleDC)},
                                             {"gdi32.dll", "DeleteDC",
                                              reinterpret_cast<void *>(Hooked_DeleteDC)},
                                             {"gdi32.dll", "CreateDIBSection",
                                              reinterpret_cast<void *>(Hooked_CreateDIBSection)},
                                             {"gdi32.dll", "DeleteObject",
                                              reinterpret_cast<void *>(Hooked_DeleteObject)},
                                             {"gdi32.dll", "BitBlt",
                                              reinterpret_cast<void *>(Hooked_BitBlt)},
                                             {"gdi32.dll", "StretchBlt",
                                              reinterpret_cast<void *>(Hooked_StretchBlt)},
                                             {"gdi32.dll", "SwapBuffers",
                                              reinterpret_cast<void *>(Hooked_SwapBuffers)},
                                             {"gdi32.dll", "ChoosePixelFormat",
                                              reinterpret_cast<void *>(Hooked_ChoosePixelFormat)},
                                             {"gdi32.dll", "SetPixelFormat",
                                              reinterpret_cast<void *>(Hooked_SetPixelFormat)},
                                             {"gdi32.dll", "wglCreateContext",
                                              reinterpret_cast<void *>(Hooked_wglCreateContext)},
                                             {"gdi32.dll", "wglMakeCurrent",
                                              reinterpret_cast<void *>(Hooked_wglMakeCurrent)},
                                             {"gdi32.dll", "wglDeleteContext",
                                              reinterpret_cast<void *>(Hooked_wglDeleteContext)}}};

bool ApplyHookToImport(HMODULE module, const HookSpec &spec) {
  if (module == nullptr) {
    return false;
  }

  auto *dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }

  auto *nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(
      reinterpret_cast<BYTE *>(module) + dos_header->e_lfanew);
  if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
    return false;
  }

  const IMAGE_DATA_DIRECTORY &import_directory =
      nt_headers->OptionalHeader
          .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_directory.VirtualAddress == 0 || import_directory.Size == 0) {
    return false;
  }

  auto *import_descriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
      reinterpret_cast<BYTE *>(module) + import_directory.VirtualAddress);
  for (; import_descriptor->Name != 0; ++import_descriptor) {
    const char *import_name = reinterpret_cast<const char *>(
        reinterpret_cast<BYTE *>(module) + import_descriptor->Name);
    if (_stricmp(import_name, spec.dll_name) != 0) {
      continue;
    }

    auto *thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
        reinterpret_cast<BYTE *>(module) + import_descriptor->FirstThunk);
    auto *original_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
        reinterpret_cast<BYTE *>(module) +
        (import_descriptor->OriginalFirstThunk
             ? import_descriptor->OriginalFirstThunk
             : import_descriptor->FirstThunk));

    for (; original_thunk->u1.AddressOfData != 0; ++original_thunk, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) {
        continue;
      }
      auto *import_by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
          reinterpret_cast<BYTE *>(module) + original_thunk->u1.AddressOfData);
      if (import_by_name == nullptr) {
        continue;
      }
      if (_stricmp(reinterpret_cast<const char *>(import_by_name->Name),
                   spec.function_name) != 0) {
        continue;
      }

      void **target = reinterpret_cast<void **>(&thunk->u1.Function);
      DWORD old_protect = 0;
      if (!VirtualProtect(target, sizeof(void *), PAGE_EXECUTE_READWRITE,
                          &old_protect)) {
        continue;
      }
      *target = spec.replacement;
      VirtualProtect(target, sizeof(void *), old_protect, &old_protect);
      FlushInstructionCache(GetCurrentProcess(), target, sizeof(void *));
      return true;
    }
  }

  return false;
}

bool ApplyHooksToModule(HMODULE module) {
  bool hooked_any = false;
  for (const HookSpec &spec : kHookSpecs) {
    const bool hooked = ApplyHookToImport(module, spec);
    if (hooked) {
      hooked_any = true;
    }
  }
  return hooked_any;
}

void DestroyOffscreenSurfaceUnlocked() {
  if (g_offscreen_surface.dc != nullptr) {
    if (g_offscreen_surface.old_bitmap != nullptr) {
      SelectObject(g_offscreen_surface.dc, g_offscreen_surface.old_bitmap);
      g_offscreen_surface.old_bitmap = nullptr;
    }
    g_orig_DeleteDC(g_offscreen_surface.dc);
    g_offscreen_surface.dc = nullptr;
  }
  if (g_offscreen_surface.bitmap != nullptr) {
    g_orig_DeleteObject(g_offscreen_surface.bitmap);
    g_offscreen_surface.bitmap = nullptr;
  }
  g_offscreen_surface.bits = nullptr;
  g_offscreen_surface.width = 0;
  g_offscreen_surface.height = 0;
}

} // namespace

bool DebugTraceInitialize(const std::wstring &log_path) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_initialized.load()) {
    return true;
  }
  g_log_file = CreateFileW(log_path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_log_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  g_log_initialized.store(true);
  WriteLogLineUnlocked(L"== Debug trace initialized ==");
  return true;
}

void DebugTraceShutdown() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log_initialized.load()) {
    return;
  }
  WriteLogLineUnlocked(L"== Debug trace shutdown ==");
  if (g_log_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_log_file);
    g_log_file = INVALID_HANDLE_VALUE;
  }
  g_log_initialized.store(false);
}

bool DebugTraceIsActive() { return g_log_initialized.load(); }

void DebugTraceLog(const wchar_t *format, ...) {
  if (!DebugTraceIsActive()) {
    return;
  }
  va_list args;
  va_start(args, format);
  const int needed = _vscwprintf(format, args);
  va_end(args);
  if (needed <= 0) {
    return;
  }
  std::wstring buffer(static_cast<size_t>(needed), L'\0');
  va_start(args, format);
  vswprintf(buffer.data(), buffer.size() + 1, format, args);
  va_end(args);
  DebugTraceLog(buffer);
}

void DebugTraceLog(const std::wstring &message) {
  if (!DebugTraceIsActive()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::wstring line = GetTimestamp();
  line += L" | ";
  line += message;
  WriteLogLineUnlocked(line);
}

bool DebugTraceInstallHooksForModule(HMODULE module,
                                     const std::wstring &module_name) {
  if (module == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_hook_mutex);
  if (g_hooked_modules.find(module) != g_hooked_modules.end()) {
    return true;
  }
  const bool hooked = ApplyHooksToModule(module);
  if (hooked) {
    g_hooked_modules.insert(module);
    DebugTraceLog(L"Installed debug hooks for module %s", module_name.c_str());
  } else {
    DebugTraceLog(L"No debug hooks applied to module %s", module_name.c_str());
  }
  return hooked;
}

void DebugTraceRegisterTargetWindow(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_target_mutex);
    g_target_windows.insert(hwnd);
  }
  DebugTraceLog(L"Registered target window %s", FormatHwnd(hwnd).c_str());
}

void DebugTraceUnregisterTargetWindow(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_target_mutex);
    g_target_windows.erase(hwnd);
  }
  DebugTraceLog(L"Unregistered target window %s", FormatHwnd(hwnd).c_str());
}

bool DebugTraceConfigureOffscreenSurface(int width, int height) {
  std::lock_guard<std::mutex> lock(g_surface_mutex);
  DestroyOffscreenSurfaceUnlocked();
  if (width <= 0 || height <= 0) {
    DebugTraceLog(L"Offscreen surface disabled (invalid size)");
    return false;
  }

  HDC dc = g_orig_CreateCompatibleDC(nullptr);
  if (dc == nullptr) {
    DebugTraceLog(L"ERROR: CreateCompatibleDC(nullptr) failed for offscreen surface");
    return false;
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
  HBITMAP bitmap = g_orig_CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap == nullptr || bits == nullptr) {
    DebugTraceLog(L"ERROR: CreateDIBSection failed for offscreen surface");
    if (dc != nullptr) {
      g_orig_DeleteDC(dc);
    }
    return false;
  }

  HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, bitmap));
  g_offscreen_surface.dc = dc;
  g_offscreen_surface.bitmap = bitmap;
  g_offscreen_surface.old_bitmap = old;
  g_offscreen_surface.bits = bits;
  g_offscreen_surface.width = width;
  g_offscreen_surface.height = height;

  DebugTraceLog(L"Initialized offscreen surface %dx%d dc=%s bitmap=%s", width, height,
                FormatHdc(dc).c_str(),
                FormatPointer(reinterpret_cast<UINT_PTR>(bitmap)).c_str());
  return true;
}

void DebugTraceResetOffscreenSurface() {
  std::lock_guard<std::mutex> lock(g_surface_mutex);
  DestroyOffscreenSurfaceUnlocked();
  DebugTraceLog(L"Offscreen surface reset");
}

bool DebugTraceCaptureOffscreenSurface(std::vector<uint8_t> &out_rgba) {
  std::lock_guard<std::mutex> lock(g_surface_mutex);
  if (!HasOffscreenSurfaceUnlocked()) {
    return false;
  }
  const size_t pixel_count =
      static_cast<size_t>(g_offscreen_surface.width) *
      static_cast<size_t>(g_offscreen_surface.height);
  out_rgba.resize(pixel_count * 4);
  const uint8_t *src = static_cast<const uint8_t *>(g_offscreen_surface.bits);
  uint8_t *dst = out_rgba.data();
  const size_t bytes = pixel_count * 4;
  for (size_t offset = 0; offset < bytes; offset += 4) {
    dst[offset + 0] = src[offset + 2];
    dst[offset + 1] = src[offset + 1];
    dst[offset + 2] = src[offset + 0];
    dst[offset + 3] = src[offset + 3];
  }
  return true;
}

} // namespace
