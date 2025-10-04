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
  HWND hwnd = nullptr;
  int width = 0;
  int height = 0;
  void *bits = nullptr;
};

std::mutex g_log_mutex;
HANDLE g_log_file = INVALID_HANDLE_VALUE;
std::atomic<bool> g_log_initialized{false};

std::mutex g_handle_mutex;
std::unordered_map<UINT_PTR, HandleInfo> g_handle_info;
std::unordered_map<HDC, HBITMAP> g_selected_bitmaps;

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

struct DiagnosticsBuffer {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HBITMAP old_bitmap = nullptr;
  void *bits = nullptr;
  int width = 0;
  int height = 0;
  uint64_t generation = 0;
};

std::mutex g_diagnostics_mutex;
DiagnosticsBuffer g_diagnostics_buffer;

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

std::wstring FormatResourceClassName(ULONG_PTR value) {
  const WORD atom = LOWORD(value);
  wchar_t buffer[32];
  std::swprintf(buffer, std::size(buffer), L"#%u", static_cast<unsigned>(atom));
  return buffer;
}

std::wstring FormatClassName(LPCWSTR class_name) {
  if (class_name == nullptr) {
    return L"(null)";
  }
  if (!IS_INTRESOURCE(class_name)) {
    return std::wstring(class_name);
  }
  return FormatResourceClassName(reinterpret_cast<ULONG_PTR>(class_name));
}

std::wstring FormatClassName(LPCSTR class_name) {
  if (class_name == nullptr) {
    return L"(null)";
  }
  if (!IS_INTRESOURCE(class_name)) {
    return Widen(class_name);
  }
  return FormatResourceClassName(reinterpret_cast<ULONG_PTR>(class_name));
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
  wchar_t buffer[96];
  const DWORD thread_id = GetCurrentThreadId();
  std::swprintf(buffer, std::size(buffer),
                L"%04d-%02d-%02d %02d:%02d:%02d.%03d [tid=%lu]", st.wYear,
                st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, static_cast<unsigned long>(thread_id));
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
                 const std::wstring &detail, HWND hwnd = nullptr,
                 int width = 0, int height = 0, void *bits = nullptr) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  g_handle_info[value] = HandleInfo{type, detail, hwnd, width, height, bits};
}

void UntrackHandle(UINT_PTR value) {
  std::lock_guard<std::mutex> lock(g_handle_mutex);
  auto it = g_handle_info.find(value);
  if (it != g_handle_info.end()) {
    if (it->second.type == L"HDC") {
      g_selected_bitmaps.erase(reinterpret_cast<HDC>(value));
    }
    g_handle_info.erase(it);
  } else {
    g_selected_bitmaps.erase(reinterpret_cast<HDC>(value));
  }
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
  if (it->second.hwnd != nullptr) {
    result += L" hwnd=";
    result += FormatHwnd(it->second.hwnd);
  }
  if (it->second.width > 0 && it->second.height > 0) {
    result += L" size=";
    result += std::to_wstring(it->second.width);
    result.push_back(L'x');
    result += std::to_wstring(it->second.height);
  }
  if (it->second.bits != nullptr) {
    result += L" bits=";
    result += FormatPointer(reinterpret_cast<UINT_PTR>(it->second.bits));
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

bool IsOffscreenDcUnlocked(HDC dc) {
  return HasOffscreenSurfaceUnlocked() && dc == g_offscreen_surface.dc;
}

bool IsOffscreenDc(HDC dc) {
  std::lock_guard<std::mutex> lock(g_surface_mutex);
  return IsOffscreenDcUnlocked(dc);
}

bool HasDiagnosticsBufferUnlocked() {
  return g_diagnostics_buffer.dc != nullptr && g_diagnostics_buffer.bits != nullptr &&
         g_diagnostics_buffer.width > 0 && g_diagnostics_buffer.height > 0;
}

bool HasDiagnosticsBuffer() {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  return HasDiagnosticsBufferUnlocked();
}

bool ShouldSnapshotDiagnostics(HDC dc);
bool SnapshotDiagnosticsFromDc(HDC dc, const wchar_t *reason);

// Forward declarations of original functions.
decltype(&CreateWindowExW) g_orig_CreateWindowExW = CreateWindowExW;
decltype(&CreateWindowExA) g_orig_CreateWindowExA = CreateWindowExA;
decltype(&DestroyWindow) g_orig_DestroyWindow = DestroyWindow;
decltype(&GetDC) g_orig_GetDC = GetDC;
decltype(&GetDCEx) g_orig_GetDCEx = GetDCEx;
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
decltype(&CreateCompatibleBitmap) g_orig_CreateCompatibleBitmap =
    CreateCompatibleBitmap;
decltype(&SelectObject) g_orig_SelectObject = SelectObject;
decltype(&PatBlt) g_orig_PatBlt = PatBlt;
decltype(&DescribePixelFormat) g_orig_DescribePixelFormat = DescribePixelFormat;

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
                FormatClassName(lpClassName).c_str(),
                Widen(lpWindowName).c_str(), dwExStyle, dwStyle,
                FormatHwnd(hWndParent).c_str());
  HWND hwnd = g_orig_CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                     X, Y, nWidth, nHeight, hWndParent, hMenu,
                                     hInstance, lpParam);
  if (hwnd != nullptr) {
    std::wstring detail = L"class=" + FormatClassName(lpClassName);
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
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail, hwnd);
  }
  DebugTraceLog(L"GetDC(%s) -> %s", FormatHwnd(hwnd).c_str(),
                FormatHdc(dc).c_str());
  return dc;
}

HDC WINAPI Hooked_GetDCEx(HWND hwnd, HRGN hrgnClip, DWORD flags) {
  if (IsTargetWindow(hwnd) && HasOffscreenSurface()) {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    DebugTraceLog(
        L"GetDCEx(%s, hrgn=%s, flags=0x%08X) -> offscreen %s",
        FormatHwnd(hwnd).c_str(),
        FormatPointer(reinterpret_cast<UINT_PTR>(hrgnClip)).c_str(), flags,
        FormatHdc(g_offscreen_surface.dc).c_str());
    return g_offscreen_surface.dc;
  }
  HDC dc = g_orig_GetDCEx(hwnd, hrgnClip, flags);
  if (dc != nullptr) {
    std::wstring detail = L"GetDCEx hwnd=";
    detail += FormatHwnd(hwnd);
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail, hwnd);
  }
  DebugTraceLog(L"GetDCEx(%s, hrgn=%s, flags=0x%08X) -> %s",
                FormatHwnd(hwnd).c_str(),
                FormatPointer(reinterpret_cast<UINT_PTR>(hrgnClip)).c_str(), flags,
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
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail, hwnd);
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
    TrackHandle(reinterpret_cast<UINT_PTR>(dc), L"HDC", detail, hwnd);
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
    int bitmap_width = 0;
    int bitmap_height = 0;
    if (bmi != nullptr) {
      bitmap_width = bmi->bmiHeader.biWidth;
      const LONG raw_height = bmi->bmiHeader.biHeight;
      bitmap_height = raw_height < 0 ? -raw_height : raw_height;
      detail += L" size=";
      detail += std::to_wstring(bitmap_width);
      detail += L"x";
      detail += std::to_wstring(bitmap_height);
    }
    TrackHandle(reinterpret_cast<UINT_PTR>(bitmap), L"HBITMAP", detail, nullptr,
                bitmap_width, bitmap_height, bits != nullptr ? *bits : nullptr);
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

HBITMAP WINAPI Hooked_CreateCompatibleBitmap(HDC dc, int width, int height) {
  HBITMAP bitmap = g_orig_CreateCompatibleBitmap(dc, width, height);
  if (bitmap != nullptr) {
    std::wstring detail = L"CreateCompatibleBitmap dc=";
    detail += FormatHdc(dc);
    detail += L" size=";
    detail += std::to_wstring(width);
    detail += L"x";
    detail += std::to_wstring(height);
    TrackHandle(reinterpret_cast<UINT_PTR>(bitmap), L"HBITMAP", detail, nullptr,
                width, height, nullptr);
  }
  DebugTraceLog(L"CreateCompatibleBitmap(dc=%s, %dx%d) -> %s",
                FormatHdc(dc).c_str(), width, height,
                FormatPointer(reinterpret_cast<UINT_PTR>(bitmap)).c_str());
  return bitmap;
}

HGDIOBJ WINAPI Hooked_SelectObject(HDC dc, HGDIOBJ obj) {
  HGDIOBJ previous = g_orig_SelectObject(dc, obj);
  const UINT object_type = (obj != nullptr) ? GetObjectType(obj) : 0U;
  {
    std::lock_guard<std::mutex> lock(g_handle_mutex);
    if (object_type == OBJ_BITMAP) {
      g_selected_bitmaps[dc] = static_cast<HBITMAP>(obj);
    } else {
      g_selected_bitmaps.erase(dc);
    }
  }
  DebugTraceLog(L"SelectObject(dc=%s, obj=%s) -> %s", FormatHdc(dc).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(obj)).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(previous)).c_str());
  return previous;
}

BOOL WINAPI Hooked_BitBlt(HDC dest, int xDest, int yDest, int width, int height,
                          HDC src, int xSrc, int ySrc, DWORD rop) {
  DebugTraceLog(L"BitBlt(dest=%s, src=%s, size=%dx%d, rop=0x%08X)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dest)).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(src)).c_str(), width,
                height, rop);
  const BOOL result =
      g_orig_BitBlt(dest, xDest, yDest, width, height, src, xSrc, ySrc, rop);
  if (result && HasDiagnosticsBuffer()) {
    bool captured = false;
    if (ShouldSnapshotDiagnostics(dest)) {
      captured = SnapshotDiagnosticsFromDc(dest, L"BitBlt dest");
    }
    if (!captured && ShouldSnapshotDiagnostics(src)) {
      SnapshotDiagnosticsFromDc(src, L"BitBlt src");
    }
  }
  return result;
}

BOOL WINAPI Hooked_StretchBlt(HDC dest, int xDest, int yDest, int widthDest,
                              int heightDest, HDC src, int xSrc, int ySrc,
                              int widthSrc, int heightSrc, DWORD rop) {
  DebugTraceLog(L"StretchBlt(dest=%s, src=%s, size=%dx%d -> %dx%d, rop=0x%08X)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dest)).c_str(),
                DescribeHandle(reinterpret_cast<UINT_PTR>(src)).c_str(),
                widthSrc, heightSrc, widthDest, heightDest, rop);
  const BOOL result = g_orig_StretchBlt(dest, xDest, yDest, widthDest, heightDest,
                                        src, xSrc, ySrc, widthSrc, heightSrc, rop);
  if (result && HasDiagnosticsBuffer()) {
    bool captured = false;
    if (ShouldSnapshotDiagnostics(dest)) {
      captured = SnapshotDiagnosticsFromDc(dest, L"StretchBlt dest");
    }
    if (!captured && ShouldSnapshotDiagnostics(src)) {
      SnapshotDiagnosticsFromDc(src, L"StretchBlt src");
    }
  }
  return result;
}

BOOL WINAPI Hooked_PatBlt(HDC dc, int x, int y, int width, int height, DWORD rop) {
  DebugTraceLog(L"PatBlt(dc=%s, x=%d, y=%d, size=%dx%d, rop=0x%08X)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str(), x, y,
                width, height, rop);
  const BOOL result = g_orig_PatBlt(dc, x, y, width, height, rop);
  if (result && HasDiagnosticsBuffer() && ShouldSnapshotDiagnostics(dc)) {
    SnapshotDiagnosticsFromDc(dc, L"PatBlt");
  }
  return result;
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

int WINAPI Hooked_DescribePixelFormat(HDC dc, int format, UINT bytes,
                                      PIXELFORMATDESCRIPTOR *pfd) {
  DebugTraceLog(L"DescribePixelFormat(dc=%s, format=%d, bytes=%u)",
                DescribeHandle(reinterpret_cast<UINT_PTR>(dc)).c_str(), format,
                bytes);
  const int result = g_orig_DescribePixelFormat(dc, format, bytes, pfd);
  DebugTraceLog(L"DescribePixelFormat -> %d", result);
  return result;
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

const std::array<HookSpec, 25> kHookSpecs = {{{"user32.dll", "CreateWindowExW",
                                              reinterpret_cast<void *>(Hooked_CreateWindowExW)},
                                             {"user32.dll", "CreateWindowExA",
                                              reinterpret_cast<void *>(Hooked_CreateWindowExA)},
                                             {"user32.dll", "DestroyWindow",
                                              reinterpret_cast<void *>(Hooked_DestroyWindow)},
                                             {"user32.dll", "GetDC",
                                              reinterpret_cast<void *>(Hooked_GetDC)},
                                             {"user32.dll", "GetDCEx",
                                              reinterpret_cast<void *>(Hooked_GetDCEx)},
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
                                             {"gdi32.dll", "CreateCompatibleBitmap",
                                              reinterpret_cast<void *>(Hooked_CreateCompatibleBitmap)},
                                             {"gdi32.dll", "CreateDIBSection",
                                              reinterpret_cast<void *>(Hooked_CreateDIBSection)},
                                             {"gdi32.dll", "DeleteObject",
                                              reinterpret_cast<void *>(Hooked_DeleteObject)},
                                             {"gdi32.dll", "SelectObject",
                                              reinterpret_cast<void *>(Hooked_SelectObject)},
                                             {"gdi32.dll", "BitBlt",
                                              reinterpret_cast<void *>(Hooked_BitBlt)},
                                             {"gdi32.dll", "StretchBlt",
                                              reinterpret_cast<void *>(Hooked_StretchBlt)},
                                             {"gdi32.dll", "PatBlt",
                                              reinterpret_cast<void *>(Hooked_PatBlt)},
                                             {"gdi32.dll", "SwapBuffers",
                                              reinterpret_cast<void *>(Hooked_SwapBuffers)},
                                             {"gdi32.dll", "ChoosePixelFormat",
                                              reinterpret_cast<void *>(Hooked_ChoosePixelFormat)},
                                             {"gdi32.dll", "SetPixelFormat",
                                              reinterpret_cast<void *>(Hooked_SetPixelFormat)},
                                             {"gdi32.dll", "DescribePixelFormat",
                                              reinterpret_cast<void *>(Hooked_DescribePixelFormat)},
                                             {"opengl32.dll", "wglCreateContext",
                                              reinterpret_cast<void *>(Hooked_wglCreateContext)},
                                             {"opengl32.dll", "wglMakeCurrent",
                                              reinterpret_cast<void *>(Hooked_wglMakeCurrent)},
                                             {"opengl32.dll", "wglDeleteContext",
                                              reinterpret_cast<void *>(Hooked_wglDeleteContext)}}};

bool ApplyHookToImport(HMODULE module, const HookSpec &spec,
                       std::vector<std::wstring> *applied) {
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
      if (applied != nullptr) {
        std::wstring entry = Widen(spec.dll_name);
        entry += L"!";
        entry += Widen(spec.function_name);
        applied->push_back(std::move(entry));
      }
      return true;
    }
  }

  return false;
}

size_t ApplyHooksToModule(HMODULE module, std::vector<std::wstring> *applied) {
  size_t hook_count = 0;
  for (const HookSpec &spec : kHookSpecs) {
    const bool hooked = ApplyHookToImport(module, spec, applied);
    if (hooked) {
      ++hook_count;
    }
  }
  return hook_count;
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

void DestroyDiagnosticsBufferUnlocked() {
  if (g_diagnostics_buffer.dc != nullptr) {
    if (g_diagnostics_buffer.old_bitmap != nullptr) {
      SelectObject(g_diagnostics_buffer.dc, g_diagnostics_buffer.old_bitmap);
      g_diagnostics_buffer.old_bitmap = nullptr;
    }
    g_orig_DeleteDC(g_diagnostics_buffer.dc);
    g_diagnostics_buffer.dc = nullptr;
  }
  if (g_diagnostics_buffer.bitmap != nullptr) {
    g_orig_DeleteObject(g_diagnostics_buffer.bitmap);
    g_diagnostics_buffer.bitmap = nullptr;
  }
  g_diagnostics_buffer.bits = nullptr;
  g_diagnostics_buffer.width = 0;
  g_diagnostics_buffer.height = 0;
  g_diagnostics_buffer.generation = 0;
}

bool EnsureDiagnosticsBufferUnlocked(int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (HasDiagnosticsBufferUnlocked() && g_diagnostics_buffer.width == width &&
      g_diagnostics_buffer.height == height) {
    return true;
  }

  DestroyDiagnosticsBufferUnlocked();

  HDC dc = g_orig_CreateCompatibleDC(nullptr);
  if (dc == nullptr) {
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
  HBITMAP bitmap =
      g_orig_CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap == nullptr || bits == nullptr) {
    g_orig_DeleteDC(dc);
    return false;
  }

  HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, bitmap));
  g_diagnostics_buffer.dc = dc;
  g_diagnostics_buffer.bitmap = bitmap;
  g_diagnostics_buffer.old_bitmap = old;
  g_diagnostics_buffer.bits = bits;
  g_diagnostics_buffer.width = width;
  g_diagnostics_buffer.height = height;
  g_diagnostics_buffer.generation = 0;
  return true;
}

bool ShouldSnapshotDiagnostics(HDC dc) {
  if (dc == nullptr) {
    return false;
  }
  if (IsOffscreenDc(dc)) {
    return true;
  }
  HWND hwnd = WindowFromDC(dc);
  if (hwnd != nullptr && IsTargetWindow(hwnd)) {
    return true;
  }
  return false;
}

bool SnapshotDiagnosticsFromDc(HDC dc, const wchar_t *reason) {
  if (dc == nullptr || reason == nullptr) {
    return false;
  }
  uint64_t new_generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    if (!HasDiagnosticsBufferUnlocked()) {
      return false;
    }
    const int width = g_diagnostics_buffer.width;
    const int height = g_diagnostics_buffer.height;
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (!g_orig_BitBlt(g_diagnostics_buffer.dc, 0, 0, width, height, dc, 0, 0,
                       SRCCOPY)) {
      return false;
    }
    new_generation = ++g_diagnostics_buffer.generation;
  }
  DebugTraceLog(L"Diagnostics buffer updated via %s (generation=%llu)", reason,
                static_cast<unsigned long long>(new_generation));
  return true;
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
    DebugTraceLog(L"Skipping hook install for %s (module=null)",
                  module_name.c_str());
    return false;
  }
  std::lock_guard<std::mutex> lock(g_hook_mutex);
  if (g_hooked_modules.find(module) != g_hooked_modules.end()) {
    DebugTraceLog(L"Debug hooks already installed for %s (module=%s)",
                  module_name.c_str(),
                  FormatPointer(reinterpret_cast<UINT_PTR>(module)).c_str());
    return true;
  }
  std::vector<std::wstring> applied;
  const size_t hook_count = ApplyHooksToModule(module, &applied);
  if (hook_count > 0) {
    g_hooked_modules.insert(module);
    DebugTraceLog(L"Installed %zu debug hooks for %s (module=%s)", hook_count,
                  module_name.c_str(),
                  FormatPointer(reinterpret_cast<UINT_PTR>(module)).c_str());
    for (const std::wstring &entry : applied) {
      DebugTraceLog(L"  hook: %s", entry.c_str());
    }
    return true;
  }
  DebugTraceLog(L"No debug hooks applied to %s (module=%s)", module_name.c_str(),
                FormatPointer(reinterpret_cast<UINT_PTR>(module)).c_str());
  return false;
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

bool DebugTraceConfigureDiagnosticsBuffer(int width, int height) {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  void *bits = nullptr;
  bool success = false;
  {
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    if (width <= 0 || height <= 0) {
      DestroyDiagnosticsBufferUnlocked();
    } else {
      success = EnsureDiagnosticsBufferUnlocked(width, height);
      if (success) {
        dc = g_diagnostics_buffer.dc;
        bitmap = g_diagnostics_buffer.bitmap;
        bits = g_diagnostics_buffer.bits;
      }
    }
  }
  if (width <= 0 || height <= 0) {
    DebugTraceLog(L"Diagnostics buffer disabled (invalid size)");
    return false;
  }
  if (!success) {
    DebugTraceLog(L"ERROR: Failed to initialize diagnostics buffer %dx%d", width,
                  height);
    return false;
  }
  DebugTraceLog(L"Initialized diagnostics buffer %dx%d dc=%s bitmap=%s bits=%s",
                width, height, FormatHdc(dc).c_str(),
                FormatPointer(reinterpret_cast<UINT_PTR>(bitmap)).c_str(),
                FormatPointer(reinterpret_cast<UINT_PTR>(bits)).c_str());
  return true;
}

void DebugTraceResetDiagnosticsBuffer() {
  bool had_buffer = false;
  {
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    had_buffer = HasDiagnosticsBufferUnlocked();
    DestroyDiagnosticsBufferUnlocked();
  }
  if (had_buffer) {
    DebugTraceLog(L"Diagnostics buffer reset");
  }
}

bool DebugTraceFetchLastFrame(uint64_t &in_out_generation,
                              std::vector<uint8_t> &out_rgba) {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  if (!HasDiagnosticsBufferUnlocked()) {
    return false;
  }
  if (g_diagnostics_buffer.generation == 0 ||
      g_diagnostics_buffer.generation == in_out_generation) {
    return false;
  }
  const size_t pixel_count =
      static_cast<size_t>(g_diagnostics_buffer.width) *
      static_cast<size_t>(g_diagnostics_buffer.height);
  if (pixel_count == 0) {
    return false;
  }
  out_rgba.resize(pixel_count * 4);
  const uint8_t *src =
      static_cast<const uint8_t *>(g_diagnostics_buffer.bits);
  uint8_t *dst = out_rgba.data();
  const size_t bytes = pixel_count * 4;
  for (size_t offset = 0; offset < bytes; offset += 4) {
    dst[offset + 0] = src[offset + 2];
    dst[offset + 1] = src[offset + 1];
    dst[offset + 2] = src[offset + 0];
    dst[offset + 3] = src[offset + 3];
  }
  in_out_generation = g_diagnostics_buffer.generation;
  return true;
}

uint64_t DebugTracePeekDiagnosticsGeneration() {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  return g_diagnostics_buffer.generation;
}
