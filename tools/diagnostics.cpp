#include "diagnostics.hpp"

#include "debug_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace diagnostics {

namespace {

class DummySurface {
public:
  bool Initialize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    ShutdownLocked();
    if (width <= 0 || height <= 0) {
      return false;
    }

    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
      debug_log::Write(L"DummySurface: GetDC(nullptr) failed (error=%lu)",
                       GetLastError());
      return false;
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);
    ReleaseDC(nullptr, screen_dc);
    if (memory_dc == nullptr) {
      debug_log::Write(L"DummySurface: CreateCompatibleDC failed (error=%lu)",
                       GetLastError());
      return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory_dc, &bmi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
      const DWORD error = GetLastError();
      DeleteDC(memory_dc);
      debug_log::Write(L"DummySurface: CreateDIBSection failed (error=%lu)",
                       error);
      return false;
    }

    HGDIOBJ previous = SelectObject(memory_dc, dib);
    if (previous == nullptr || previous == HGDI_ERROR) {
      const DWORD error = GetLastError();
      DeleteObject(dib);
      DeleteDC(memory_dc);
      debug_log::Write(L"DummySurface: SelectObject failed (error=%lu)", error);
      return false;
    }

    memory_dc_ = memory_dc;
    bitmap_ = dib;
    bits_ = bits;
    old_bitmap_ = previous;
    width_ = width;
    height_ = height;
    ref_count_ = 0;
    std::memset(bits_, 0, static_cast<size_t>(width_) * height_ * 4);
    debug_log::Write(L"DummySurface: initialized %dx%d", width_, height_);
    return true;
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    ShutdownLocked();
  }

  HDC Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (memory_dc_ == nullptr) {
      return nullptr;
    }
    ++ref_count_;
    return memory_dc_;
  }

  void Release(HDC dc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dc != nullptr && dc == memory_dc_ && ref_count_ > 0) {
      --ref_count_;
    }
  }

  bool Capture(int width, int height, std::vector<uint8_t> &out_rgba) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (memory_dc_ == nullptr || bits_ == nullptr) {
      return false;
    }
    if (width != width_ || height != height_) {
      debug_log::Write(
          L"DummySurface: Capture dimension mismatch (requested %dx%d, actual %dx%d)",
          width, height, width_, height_);
      return false;
    }
    const size_t pixels = static_cast<size_t>(width_) * height_;
    out_rgba.resize(pixels * 4);
    const uint8_t *src = static_cast<const uint8_t *>(bits_);
    uint8_t *dst = out_rgba.data();
    for (size_t i = 0; i < pixels; ++i) {
      const size_t offset = i * 4;
      dst[offset + 0] = src[offset + 2];
      dst[offset + 1] = src[offset + 1];
      dst[offset + 2] = src[offset + 0];
      dst[offset + 3] = src[offset + 3];
    }
    return true;
  }

  bool IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return memory_dc_ != nullptr && bits_ != nullptr;
  }

private:
  void ShutdownLocked() {
    if (memory_dc_ != nullptr && old_bitmap_ != nullptr) {
      SelectObject(memory_dc_, old_bitmap_);
    }
    if (bitmap_ != nullptr) {
      DeleteObject(bitmap_);
    }
    if (memory_dc_ != nullptr) {
      DeleteDC(memory_dc_);
    }
    memory_dc_ = nullptr;
    bitmap_ = nullptr;
    bits_ = nullptr;
    old_bitmap_ = nullptr;
    width_ = 0;
    height_ = 0;
    ref_count_ = 0;
  }

  mutable std::mutex mutex_;
  HDC memory_dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  void *bits_ = nullptr;
  HGDIOBJ old_bitmap_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int ref_count_ = 0;
};

struct WindowInfo {
  DWORD ex_style = 0;
  DWORD style = 0;
  HWND parent = nullptr;
  HINSTANCE instance = nullptr;
  std::wstring class_name;
  std::wstring window_name;
};

struct DcInfo {
  HWND owner = nullptr;
  bool is_dummy = false;
  std::wstring label;
};

std::unordered_map<HWND, WindowInfo> g_windows;
std::unordered_map<HDC, DcInfo> g_dcs;
std::mutex g_window_mutex;
std::mutex g_dc_mutex;

DummySurface g_dummy_surface;
HWND g_expected_parent = nullptr;
HWND g_expected_child = nullptr;

struct HookDefinition {
  const char *module_name;
  const char *function_name;
  FARPROC replacement;
  void **original;
};

// Forward declarations of hook replacements.
using CreateWindowExWPtr = HWND(WINAPI *)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                          int, int, int, HWND, HMENU, HINSTANCE,
                                          LPVOID);
using CreateWindowExAPtr = HWND(WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                          int, int, HWND, HMENU, HINSTANCE,
                                          LPVOID);
using DestroyWindowPtr = BOOL(WINAPI *)(HWND);
using GetDCPtr = HDC(WINAPI *)(HWND);
using GetDCExPtr = HDC(WINAPI *)(HWND, HRGN, DWORD);
using GetWindowDCPtr = HDC(WINAPI *)(HWND);
using ReleaseDCPtr = int(WINAPI *)(HWND, HDC);
using BeginPaintPtr = HDC(WINAPI *)(HWND, LPPAINTSTRUCT);
using EndPaintPtr = BOOL(WINAPI *)(HWND, const PAINTSTRUCT *);
using CreateCompatibleDCPtr = HDC(WINAPI *)(HDC);
using DeleteDCPtr = BOOL(WINAPI *)(HDC);
using CreateDIBSectionPtr = HBITMAP(WINAPI *)(HDC, const BITMAPINFO *, UINT,
                                              void **, HANDLE, DWORD);
using CreateCompatibleBitmapPtr = HBITMAP(WINAPI *)(HDC, int, int);
using SelectObjectPtr = HGDIOBJ(WINAPI *)(HDC, HGDIOBJ);
using DeleteObjectPtr = BOOL(WINAPI *)(HGDIOBJ);
using BitBltPtr = BOOL(WINAPI *)(HDC, int, int, int, int, HDC, int, int, DWORD);
using StretchBltPtr = BOOL(WINAPI *)(HDC, int, int, int, int, HDC, int, int, int,
                                     int, DWORD);
using PatBltPtr = BOOL(WINAPI *)(HDC, int, int, int, int, DWORD);
using SwapBuffersPtr = BOOL(WINAPI *)(HDC);
using ChoosePixelFormatPtr = int(WINAPI *)(HDC, const PIXELFORMATDESCRIPTOR *);
using DescribePixelFormatPtr = int(WINAPI *)(HDC, int, UINT,
                                             LPPIXELFORMATDESCRIPTOR);
using SetPixelFormatPtr = BOOL(WINAPI *)(HDC, int, const PIXELFORMATDESCRIPTOR *);
using WglCreateContextPtr = HGLRC(WINAPI *)(HDC);
using WglMakeCurrentPtr = BOOL(WINAPI *)(HDC, HGLRC);
using WglDeleteContextPtr = BOOL(WINAPI *)(HGLRC);

CreateWindowExWPtr g_orig_CreateWindowExW = nullptr;
CreateWindowExAPtr g_orig_CreateWindowExA = nullptr;
DestroyWindowPtr g_orig_DestroyWindow = nullptr;
GetDCPtr g_orig_GetDC = nullptr;
GetDCExPtr g_orig_GetDCEx = nullptr;
GetWindowDCPtr g_orig_GetWindowDC = nullptr;
ReleaseDCPtr g_orig_ReleaseDC = nullptr;
BeginPaintPtr g_orig_BeginPaint = nullptr;
EndPaintPtr g_orig_EndPaint = nullptr;
CreateCompatibleDCPtr g_orig_CreateCompatibleDC = nullptr;
DeleteDCPtr g_orig_DeleteDC = nullptr;
CreateDIBSectionPtr g_orig_CreateDIBSection = nullptr;
CreateCompatibleBitmapPtr g_orig_CreateCompatibleBitmap = nullptr;
SelectObjectPtr g_orig_SelectObject = nullptr;
DeleteObjectPtr g_orig_DeleteObject = nullptr;
BitBltPtr g_orig_BitBlt = nullptr;
StretchBltPtr g_orig_StretchBlt = nullptr;
PatBltPtr g_orig_PatBlt = nullptr;
SwapBuffersPtr g_orig_SwapBuffers = nullptr;
ChoosePixelFormatPtr g_orig_ChoosePixelFormat = nullptr;
DescribePixelFormatPtr g_orig_DescribePixelFormat = nullptr;
SetPixelFormatPtr g_orig_SetPixelFormat = nullptr;
WglCreateContextPtr g_orig_wglCreateContext = nullptr;
WglMakeCurrentPtr g_orig_wglMakeCurrent = nullptr;
WglDeleteContextPtr g_orig_wglDeleteContext = nullptr;

std::wstring AnsiToWide(const char *text) {
  if (text == nullptr) {
    return L"(null)";
  }
  const int required = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
  if (required <= 0) {
    return L"(conversion error)";
  }
  std::wstring result(static_cast<size_t>(required), L'\0');
  MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), required);
  if (!result.empty() && result.back() == L'\0') {
    result.pop_back();
  }
  return result;
}

void RegisterWindow(HWND hwnd, DWORD ex_style, DWORD style, HWND parent,
                    HINSTANCE instance, const std::wstring &class_name,
                    const std::wstring &window_name) {
  if (hwnd == nullptr) {
    return;
  }
  WindowInfo info;
  info.ex_style = ex_style;
  info.style = style;
  info.parent = parent;
  info.instance = instance;
  info.class_name = class_name;
  info.window_name = window_name;
  std::lock_guard<std::mutex> lock(g_window_mutex);
  g_windows[hwnd] = info;
}

void UnregisterWindow(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_window_mutex);
  g_windows.erase(hwnd);
}

std::wstring DescribeWindow(HWND hwnd) {
  if (hwnd == nullptr) {
    return L"HWND=null";
  }
  std::lock_guard<std::mutex> lock(g_window_mutex);
  auto it = g_windows.find(hwnd);
  if (it == g_windows.end()) {
    std::wstringstream stream;
    stream << L"HWND=" << hwnd << L" (untracked)";
    return stream.str();
  }
  const WindowInfo &info = it->second;
  std::wstringstream stream;
  stream << L"HWND=" << hwnd << L" class='" << info.class_name << L"'"
         << L" name='" << info.window_name << L"' parent=" << info.parent
         << L" style=0x" << std::hex << info.style << L" ex=0x" << info.ex_style
         << std::dec;
  return stream.str();
}

void RegisterDc(HDC dc, HWND owner, bool is_dummy, const std::wstring &label) {
  if (dc == nullptr) {
    return;
  }
  DcInfo info;
  info.owner = owner;
  info.is_dummy = is_dummy;
  info.label = label;
  std::lock_guard<std::mutex> lock(g_dc_mutex);
  g_dcs[dc] = info;
}

void UnregisterDc(HDC dc) {
  if (dc == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_dc_mutex);
  g_dcs.erase(dc);
}

std::wstring DescribeDc(HDC dc) {
  std::lock_guard<std::mutex> lock(g_dc_mutex);
  auto it = g_dcs.find(dc);
  std::wstringstream stream;
  stream << L"HDC=" << dc;
  if (it != g_dcs.end()) {
    stream << L" owner=" << it->second.owner << L" dummy="
           << (it->second.is_dummy ? L"yes" : L"no");
    if (!it->second.label.empty()) {
      stream << L" label='" << it->second.label << L"'";
    }
  } else {
    stream << L" (untracked)";
  }
  return stream.str();
}

const wchar_t *MessageToName(UINT message) {
  switch (message) {
  case WM_CREATE:
    return L"WM_CREATE";
  case WM_DESTROY:
    return L"WM_DESTROY";
  case WM_SIZE:
    return L"WM_SIZE";
  case WM_PAINT:
    return L"WM_PAINT";
  case WM_ERASEBKGND:
    return L"WM_ERASEBKGND";
  case WM_SHOWWINDOW:
    return L"WM_SHOWWINDOW";
  case WM_TIMER:
    return L"WM_TIMER";
  case WM_DROPFILES:
    return L"WM_DROPFILES";
  default:
    break;
  }
  return nullptr;
}

std::wstring FormatMessageLog(UINT message) {
  const wchar_t *name = MessageToName(message);
  if (name != nullptr) {
    return std::wstring(name);
  }
  std::wstringstream stream;
  stream << L"0x" << std::hex << message;
  return stream.str();
}

// Hook implementations

HWND WINAPI Hook_CreateWindowExW(DWORD ex_style, LPCWSTR class_name,
                                 LPCWSTR window_name, DWORD style, int x, int y,
                                 int width, int height, HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID param) {
  debug_log::Write(
      L"CreateWindowExW ex=0x%08X class='%ls' name='%ls' style=0x%08X parent=%p",
      ex_style, (class_name != nullptr) ? class_name : L"(null)",
      (window_name != nullptr) ? window_name : L"(null)", style, parent);
  HWND hwnd = g_orig_CreateWindowExW(ex_style, class_name, window_name, style, x,
                                     y, width, height, parent, menu, instance,
                                     param);
  if (hwnd != nullptr) {
    RegisterWindow(hwnd, ex_style, style, parent, instance,
                   (class_name != nullptr) ? class_name : L"(null)",
                   (window_name != nullptr) ? window_name : L"(null)");
    debug_log::Write(L"CreateWindowExW -> %ls", DescribeWindow(hwnd).c_str());
  } else {
    debug_log::Write(L"CreateWindowExW failed (error=%lu)", GetLastError());
  }
  return hwnd;
}

HWND WINAPI Hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                 LPCSTR window_name, DWORD style, int x, int y,
                                 int width, int height, HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID param) {
  const std::wstring class_w = AnsiToWide(class_name);
  const std::wstring name_w = AnsiToWide(window_name);
  debug_log::Write(L"CreateWindowExA ex=0x%08X class='%ls' name='%ls' style=0x%08X"
                   L" parent=%p",
                   ex_style, class_w.c_str(), name_w.c_str(), style, parent);
  HWND hwnd = g_orig_CreateWindowExA(ex_style, class_name, window_name, style, x,
                                     y, width, height, parent, menu, instance,
                                     param);
  if (hwnd != nullptr) {
    RegisterWindow(hwnd, ex_style, style, parent, instance, class_w, name_w);
    debug_log::Write(L"CreateWindowExA -> %ls", DescribeWindow(hwnd).c_str());
  } else {
    debug_log::Write(L"CreateWindowExA failed (error=%lu)", GetLastError());
  }
  return hwnd;
}

BOOL WINAPI Hook_DestroyWindow(HWND hwnd) {
  debug_log::Write(L"DestroyWindow %ls", DescribeWindow(hwnd).c_str());
  BOOL result = g_orig_DestroyWindow(hwnd);
  if (result) {
    UnregisterWindow(hwnd);
  } else {
    debug_log::Write(L"DestroyWindow failed (error=%lu)", GetLastError());
  }
  return result;
}

HDC AcquireDummyIfNeeded(HWND hwnd) {
  if (hwnd != nullptr && hwnd == g_expected_child && g_dummy_surface.IsActive()) {
    HDC dc = g_dummy_surface.Acquire();
    if (dc != nullptr) {
      RegisterDc(dc, hwnd, true, L"dummy-surface");
      debug_log::Write(L"Redirected GetDC to dummy surface for hwnd=%p", hwnd);
    }
    return dc;
  }
  return nullptr;
}

HDC WINAPI Hook_GetDC(HWND hwnd) {
  debug_log::Write(L"GetDC request hwnd=%p", hwnd);
  if (HDC dc = AcquireDummyIfNeeded(hwnd)) {
    return dc;
  }
  HDC dc = g_orig_GetDC(hwnd);
  if (dc != nullptr) {
    RegisterDc(dc, hwnd, false, L"GetDC");
    debug_log::Write(L"GetDC -> %ls", DescribeDc(dc).c_str());
  } else {
    debug_log::Write(L"GetDC failed (error=%lu)", GetLastError());
  }
  return dc;
}

HDC WINAPI Hook_GetDCEx(HWND hwnd, HRGN region, DWORD flags) {
  debug_log::Write(L"GetDCEx hwnd=%p flags=0x%08X", hwnd, flags);
  if (HDC dc = AcquireDummyIfNeeded(hwnd)) {
    return dc;
  }
  HDC dc = g_orig_GetDCEx(hwnd, region, flags);
  if (dc != nullptr) {
    RegisterDc(dc, hwnd, false, L"GetDCEx");
    debug_log::Write(L"GetDCEx -> %ls", DescribeDc(dc).c_str());
  } else {
    debug_log::Write(L"GetDCEx failed (error=%lu)", GetLastError());
  }
  return dc;
}

HDC WINAPI Hook_GetWindowDC(HWND hwnd) {
  debug_log::Write(L"GetWindowDC request hwnd=%p", hwnd);
  if (HDC dc = AcquireDummyIfNeeded(hwnd)) {
    return dc;
  }
  HDC dc = g_orig_GetWindowDC(hwnd);
  if (dc != nullptr) {
    RegisterDc(dc, hwnd, false, L"GetWindowDC");
    debug_log::Write(L"GetWindowDC -> %ls", DescribeDc(dc).c_str());
  } else {
    debug_log::Write(L"GetWindowDC failed (error=%lu)", GetLastError());
  }
  return dc;
}

int WINAPI Hook_ReleaseDC(HWND hwnd, HDC dc) {
  debug_log::Write(L"ReleaseDC hwnd=%p dc=%p", hwnd, dc);
  {
    std::lock_guard<std::mutex> lock(g_dc_mutex);
    auto it = g_dcs.find(dc);
    if (it != g_dcs.end() && it->second.is_dummy) {
      g_dummy_surface.Release(dc);
      g_dcs.erase(it);
      debug_log::Write(L"ReleaseDC consumed by dummy surface for dc=%p", dc);
      return 1;
    }
  }
  int result = g_orig_ReleaseDC(hwnd, dc);
  if (result != 0) {
    UnregisterDc(dc);
  } else {
    debug_log::Write(L"ReleaseDC failed (error=%lu)", GetLastError());
  }
  return result;
}

HDC WINAPI Hook_BeginPaint(HWND hwnd, LPPAINTSTRUCT paint) {
  debug_log::Write(L"BeginPaint hwnd=%p", hwnd);
  HDC dc = g_orig_BeginPaint(hwnd, paint);
  if (dc != nullptr) {
    RegisterDc(dc, hwnd, false, L"BeginPaint");
    debug_log::Write(L"BeginPaint -> %ls", DescribeDc(dc).c_str());
  } else {
    debug_log::Write(L"BeginPaint failed (error=%lu)", GetLastError());
  }
  return dc;
}

BOOL WINAPI Hook_EndPaint(HWND hwnd, const PAINTSTRUCT *paint) {
  debug_log::Write(L"EndPaint hwnd=%p dc=%p", hwnd,
                   paint != nullptr ? paint->hdc : nullptr);
  if (paint != nullptr && paint->hdc != nullptr) {
    UnregisterDc(paint->hdc);
  }
  return g_orig_EndPaint(hwnd, paint);
}

HDC WINAPI Hook_CreateCompatibleDC(HDC dc) {
  debug_log::Write(L"CreateCompatibleDC base=%p", dc);
  HDC result = g_orig_CreateCompatibleDC(dc);
  if (result != nullptr) {
    RegisterDc(result, nullptr, false, L"CreateCompatibleDC");
    debug_log::Write(L"CreateCompatibleDC -> %ls", DescribeDc(result).c_str());
  } else {
    debug_log::Write(L"CreateCompatibleDC failed (error=%lu)", GetLastError());
  }
  return result;
}

BOOL WINAPI Hook_DeleteDC(HDC dc) {
  debug_log::Write(L"DeleteDC dc=%p", dc);
  {
    std::lock_guard<std::mutex> lock(g_dc_mutex);
    auto it = g_dcs.find(dc);
    if (it != g_dcs.end() && it->second.is_dummy) {
      g_dummy_surface.Release(dc);
      g_dcs.erase(it);
      debug_log::Write(L"DeleteDC consumed by dummy surface for dc=%p", dc);
      return TRUE;
    }
  }
  BOOL result = g_orig_DeleteDC(dc);
  if (result) {
    UnregisterDc(dc);
  } else {
    debug_log::Write(L"DeleteDC failed (error=%lu)", GetLastError());
  }
  return result;
}

HBITMAP WINAPI Hook_CreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage,
                                     void **bits, HANDLE section, DWORD offset) {
  debug_log::Write(L"CreateDIBSection dc=%p usage=%u", dc, usage);
  HBITMAP bmp = g_orig_CreateDIBSection(dc, info, usage, bits, section, offset);
  if (bmp != nullptr) {
    debug_log::Write(L"CreateDIBSection -> HBITMAP=%p bits=%p", bmp,
                     bits != nullptr ? *bits : nullptr);
  } else {
    debug_log::Write(L"CreateDIBSection failed (error=%lu)", GetLastError());
  }
  return bmp;
}

HBITMAP WINAPI Hook_CreateCompatibleBitmap(HDC dc, int width, int height) {
  debug_log::Write(L"CreateCompatibleBitmap dc=%p %dx%d", dc, width, height);
  HBITMAP bmp = g_orig_CreateCompatibleBitmap(dc, width, height);
  if (bmp == nullptr) {
    debug_log::Write(L"CreateCompatibleBitmap failed (error=%lu)", GetLastError());
  }
  return bmp;
}

HGDIOBJ WINAPI Hook_SelectObject(HDC dc, HGDIOBJ obj) {
  debug_log::Write(L"SelectObject dc=%p obj=%p", dc, obj);
  HGDIOBJ previous = g_orig_SelectObject(dc, obj);
  if (previous == nullptr || previous == HGDI_ERROR) {
    debug_log::Write(L"SelectObject failed (error=%lu)", GetLastError());
  }
  return previous;
}

BOOL WINAPI Hook_DeleteObject(HGDIOBJ obj) {
  debug_log::Write(L"DeleteObject obj=%p", obj);
  BOOL result = g_orig_DeleteObject(obj);
  if (!result) {
    debug_log::Write(L"DeleteObject failed (error=%lu)", GetLastError());
  }
  return result;
}

BOOL WINAPI Hook_BitBlt(HDC dest, int x, int y, int width, int height, HDC src,
                        int src_x, int src_y, DWORD rop) {
  debug_log::Write(L"BitBlt dest=%ls src=%ls rect=(%d,%d %dx%d) rop=0x%08X",
                   DescribeDc(dest).c_str(), DescribeDc(src).c_str(), x, y,
                   width, height, rop);
  return g_orig_BitBlt(dest, x, y, width, height, src, src_x, src_y, rop);
}

BOOL WINAPI Hook_StretchBlt(HDC dest, int x, int y, int width, int height, HDC src,
                            int src_x, int src_y, int src_width, int src_height,
                            DWORD rop) {
  debug_log::Write(
      L"StretchBlt dest=%ls src=%ls dstRect=(%d,%d %dx%d) srcRect=(%d,%d %dx%d) rop=0x%08X",
      DescribeDc(dest).c_str(), DescribeDc(src).c_str(), x, y, width, height,
      src_x, src_y, src_width, src_height, rop);
  return g_orig_StretchBlt(dest, x, y, width, height, src, src_x, src_y,
                           src_width, src_height, rop);
}

BOOL WINAPI Hook_PatBlt(HDC dc, int x, int y, int width, int height, DWORD rop) {
  debug_log::Write(L"PatBlt dc=%ls rect=(%d,%d %dx%d) rop=0x%08X",
                   DescribeDc(dc).c_str(), x, y, width, height, rop);
  return g_orig_PatBlt(dc, x, y, width, height, rop);
}

BOOL WINAPI Hook_SwapBuffers(HDC dc) {
  debug_log::Write(L"SwapBuffers dc=%ls", DescribeDc(dc).c_str());
  {
    std::lock_guard<std::mutex> lock(g_dc_mutex);
    auto it = g_dcs.find(dc);
    if (it != g_dcs.end() && it->second.is_dummy) {
      debug_log::Write(L"SwapBuffers intercepted for dummy surface (noop)");
      return TRUE;
    }
  }
  return g_orig_SwapBuffers(dc);
}

int WINAPI Hook_ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *pfd) {
  debug_log::Write(L"ChoosePixelFormat dc=%ls", DescribeDc(dc).c_str());
  return g_orig_ChoosePixelFormat(dc, pfd);
}

int WINAPI Hook_DescribePixelFormat(HDC dc, int format, UINT bytes,
                                    LPPIXELFORMATDESCRIPTOR pfd) {
  debug_log::Write(L"DescribePixelFormat dc=%ls fmt=%d", DescribeDc(dc).c_str(),
                   format);
  return g_orig_DescribePixelFormat(dc, format, bytes, pfd);
}

BOOL WINAPI Hook_SetPixelFormat(HDC dc, int format,
                                const PIXELFORMATDESCRIPTOR *pfd) {
  debug_log::Write(L"SetPixelFormat dc=%ls fmt=%d", DescribeDc(dc).c_str(),
                   format);
  return g_orig_SetPixelFormat(dc, format, pfd);
}

HGLRC WINAPI Hook_wglCreateContext(HDC dc) {
  debug_log::Write(L"wglCreateContext dc=%ls", DescribeDc(dc).c_str());
  return g_orig_wglCreateContext(dc);
}

BOOL WINAPI Hook_wglMakeCurrent(HDC dc, HGLRC context) {
  debug_log::Write(L"wglMakeCurrent dc=%ls ctx=%p", DescribeDc(dc).c_str(),
                   context);
  return g_orig_wglMakeCurrent(dc, context);
}

BOOL WINAPI Hook_wglDeleteContext(HGLRC context) {
  debug_log::Write(L"wglDeleteContext ctx=%p", context);
  return g_orig_wglDeleteContext(context);
}

std::vector<HookDefinition> &HookList() {
  static std::vector<HookDefinition> hooks = {
      {"user32.dll", "CreateWindowExW", reinterpret_cast<FARPROC>(
                                           Hook_CreateWindowExW),
       reinterpret_cast<void **>(&g_orig_CreateWindowExW)},
      {"user32.dll", "CreateWindowExA", reinterpret_cast<FARPROC>(
                                           Hook_CreateWindowExA),
       reinterpret_cast<void **>(&g_orig_CreateWindowExA)},
      {"user32.dll", "DestroyWindow",
       reinterpret_cast<FARPROC>(Hook_DestroyWindow),
       reinterpret_cast<void **>(&g_orig_DestroyWindow)},
      {"user32.dll", "GetDC", reinterpret_cast<FARPROC>(Hook_GetDC),
       reinterpret_cast<void **>(&g_orig_GetDC)},
      {"user32.dll", "GetDCEx", reinterpret_cast<FARPROC>(Hook_GetDCEx),
       reinterpret_cast<void **>(&g_orig_GetDCEx)},
      {"user32.dll", "GetWindowDC",
       reinterpret_cast<FARPROC>(Hook_GetWindowDC),
       reinterpret_cast<void **>(&g_orig_GetWindowDC)},
      {"user32.dll", "ReleaseDC", reinterpret_cast<FARPROC>(Hook_ReleaseDC),
       reinterpret_cast<void **>(&g_orig_ReleaseDC)},
      {"user32.dll", "BeginPaint", reinterpret_cast<FARPROC>(Hook_BeginPaint),
       reinterpret_cast<void **>(&g_orig_BeginPaint)},
      {"user32.dll", "EndPaint", reinterpret_cast<FARPROC>(Hook_EndPaint),
       reinterpret_cast<void **>(&g_orig_EndPaint)},
      {"gdi32.dll", "CreateCompatibleDC",
       reinterpret_cast<FARPROC>(Hook_CreateCompatibleDC),
       reinterpret_cast<void **>(&g_orig_CreateCompatibleDC)},
      {"gdi32.dll", "DeleteDC", reinterpret_cast<FARPROC>(Hook_DeleteDC),
       reinterpret_cast<void **>(&g_orig_DeleteDC)},
      {"gdi32.dll", "CreateDIBSection",
       reinterpret_cast<FARPROC>(Hook_CreateDIBSection),
       reinterpret_cast<void **>(&g_orig_CreateDIBSection)},
      {"gdi32.dll", "CreateCompatibleBitmap",
       reinterpret_cast<FARPROC>(Hook_CreateCompatibleBitmap),
       reinterpret_cast<void **>(&g_orig_CreateCompatibleBitmap)},
      {"gdi32.dll", "SelectObject",
       reinterpret_cast<FARPROC>(Hook_SelectObject),
       reinterpret_cast<void **>(&g_orig_SelectObject)},
      {"gdi32.dll", "DeleteObject",
       reinterpret_cast<FARPROC>(Hook_DeleteObject),
       reinterpret_cast<void **>(&g_orig_DeleteObject)},
      {"gdi32.dll", "BitBlt", reinterpret_cast<FARPROC>(Hook_BitBlt),
       reinterpret_cast<void **>(&g_orig_BitBlt)},
      {"gdi32.dll", "StretchBlt", reinterpret_cast<FARPROC>(Hook_StretchBlt),
       reinterpret_cast<void **>(&g_orig_StretchBlt)},
      {"gdi32.dll", "PatBlt", reinterpret_cast<FARPROC>(Hook_PatBlt),
       reinterpret_cast<void **>(&g_orig_PatBlt)},
      {"gdi32.dll", "SwapBuffers",
       reinterpret_cast<FARPROC>(Hook_SwapBuffers),
       reinterpret_cast<void **>(&g_orig_SwapBuffers)},
      {"gdi32.dll", "ChoosePixelFormat",
       reinterpret_cast<FARPROC>(Hook_ChoosePixelFormat),
       reinterpret_cast<void **>(&g_orig_ChoosePixelFormat)},
      {"gdi32.dll", "DescribePixelFormat",
       reinterpret_cast<FARPROC>(Hook_DescribePixelFormat),
       reinterpret_cast<void **>(&g_orig_DescribePixelFormat)},
      {"gdi32.dll", "SetPixelFormat",
       reinterpret_cast<FARPROC>(Hook_SetPixelFormat),
       reinterpret_cast<void **>(&g_orig_SetPixelFormat)},
      {"opengl32.dll", "wglCreateContext",
       reinterpret_cast<FARPROC>(Hook_wglCreateContext),
       reinterpret_cast<void **>(&g_orig_wglCreateContext)},
      {"opengl32.dll", "wglMakeCurrent",
       reinterpret_cast<FARPROC>(Hook_wglMakeCurrent),
       reinterpret_cast<void **>(&g_orig_wglMakeCurrent)},
      {"opengl32.dll", "wglDeleteContext",
       reinterpret_cast<FARPROC>(Hook_wglDeleteContext),
       reinterpret_cast<void **>(&g_orig_wglDeleteContext)},
  };
  return hooks;
}

std::string NormalizeModuleName(const char *module_name) {
  std::string name = module_name != nullptr ? module_name : "";
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name;
}

bool PatchModule(HMODULE module, const wchar_t *tag) {
  if (module == nullptr) {
    debug_log::Write(L"PatchModule: module handle is null for %ls", tag);
    return false;
  }
  auto *dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    debug_log::Write(L"PatchModule: invalid DOS header for %ls", tag);
    return false;
  }
  auto *nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
      reinterpret_cast<BYTE *>(module) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    debug_log::Write(L"PatchModule: invalid NT header for %ls", tag);
    return false;
  }
  const IMAGE_DATA_DIRECTORY &import_dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (import_dir.VirtualAddress == 0) {
    debug_log::Write(L"PatchModule: no import table for %ls", tag);
    return true;
  }

  auto *imports = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
      reinterpret_cast<BYTE *>(module) + import_dir.VirtualAddress);
  std::vector<HookDefinition> &hooks = HookList();
  int applied = 0;
  for (; imports->Name != 0; ++imports) {
    const char *import_module_name =
        reinterpret_cast<char *>(reinterpret_cast<BYTE *>(module) + imports->Name);
    std::string import_lower = NormalizeModuleName(import_module_name);
    for (HookDefinition &hook : hooks) {
      if (import_lower != hook.module_name) {
        continue;
      }
      PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
          reinterpret_cast<BYTE *>(module) + imports->FirstThunk);
      PIMAGE_THUNK_DATA orig_thunk;
      if (imports->OriginalFirstThunk != 0) {
        orig_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<BYTE *>(module) + imports->OriginalFirstThunk);
      } else {
        orig_thunk = thunk;
      }
      for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++thunk) {
        if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) {
          continue;
        }
        auto *import = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
            reinterpret_cast<BYTE *>(module) + orig_thunk->u1.AddressOfData);
        if (std::strcmp(import->Name, hook.function_name) != 0) {
          continue;
        }
        auto **target = reinterpret_cast<FARPROC **>(thunk);
        DWORD old_protect = 0;
        if (!VirtualProtect(target, sizeof(FARPROC), PAGE_READWRITE,
                            &old_protect)) {
          debug_log::Write(
              L"PatchModule: VirtualProtect failed for %hs in %ls (error=%lu)",
              hook.function_name, tag, GetLastError());
          continue;
        }
        FARPROC original = *target;
        if (hook.original != nullptr && *hook.original == nullptr) {
          *hook.original = reinterpret_cast<void *>(original);
        }
        *target = hook.replacement;
        VirtualProtect(target, sizeof(FARPROC), old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(FARPROC));
        ++applied;
        debug_log::Write(L"Patched %hs from %ls", hook.function_name, tag);
      }
    }
  }

  debug_log::Write(L"PatchModule: applied %d hooks to %ls", applied, tag);
  return true;
}

} // namespace

bool Initialize(const std::wstring &log_path) {
  if (!debug_log::Initialize(log_path)) {
    return false;
  }
  debug_log::Write(L"Diagnostics initialized");
  return true;
}

void Shutdown() {
  g_dummy_surface.Shutdown();
  debug_log::Write(L"Diagnostics shutting down");
  debug_log::Flush();
  debug_log::Shutdown();
}

void Log(const wchar_t *format, ...) {
  va_list args;
  va_start(args, format);
  debug_log::WriteV(format, args);
  va_end(args);
}

bool InstallHooksForCurrentProcess() {
  HMODULE self = GetModuleHandleW(nullptr);
  return PatchModule(self, L"self");
}

bool InstallHooksForModule(HMODULE module, const wchar_t *tag) {
  return PatchModule(module, tag);
}

void SetExpectedWindows(HWND parent, HWND child) {
  g_expected_parent = parent;
  g_expected_child = child;
  debug_log::Write(L"SetExpectedWindows parent=%p child=%p", parent, child);
}

void NotifyWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  const std::wstring message_text = FormatMessageLog(message);
  debug_log::Write(L"WindowMessage hwnd=%p msg=%ls wParam=0x%p lParam=0x%p",
                   hwnd, message_text.c_str(), reinterpret_cast<void *>(wParam),
                   reinterpret_cast<void *>(lParam));
}

void EnableDummySurface(int width, int height) {
  if (g_dummy_surface.Initialize(width, height)) {
    debug_log::Write(L"Dummy surface enabled for %dx%d", width, height);
  } else {
    debug_log::Write(L"Failed to enable dummy surface for %dx%d", width, height);
  }
}

bool IsDummySurfaceActive() { return g_dummy_surface.IsActive(); }

bool CaptureDummySurfaceRgba(int width, int height,
                             std::vector<uint8_t> &out_rgba) {
  return g_dummy_surface.Capture(width, height, out_rgba);
}

void MarkFrameStart(int frame_index) {
  debug_log::Write(L"Frame %d start", frame_index);
}

} // namespace diagnostics

