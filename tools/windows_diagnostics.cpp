#include "windows_diagnostics.hpp"

#if defined(_WIN32)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <string.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <winternl.h>

namespace diagnostics {
namespace {

struct Logger {
  std::mutex mutex;
  FILE *file = nullptr;
  bool initialized = false;
};

Logger g_logger;

struct WindowInfo {
  std::wstring class_name;
  std::wstring title;
  DWORD style = 0;
  DWORD ex_style = 0;
  HWND parent = nullptr;
  std::wstring tag;
};

struct DcInfo {
  HWND owner = nullptr;
  bool is_window_dc = false;
  bool is_memory_dc = false;
  HBITMAP selected_bitmap = nullptr;
};

struct DibInfo {
  void *bits = nullptr;
  LONG width = 0;
  LONG height = 0;
  WORD bit_count = 0;
  DWORD compression = 0;
  LONG pitch = 0;
  bool top_down = false;
};

struct FrameCaptureState {
  bool active = false;
  int width = 0;
  int height = 0;
  uint64_t generation = 0;
  uint64_t last_consumed_generation = 0;
  std::vector<uint8_t> rgba;
};

std::mutex g_state_mutex;
std::mutex g_hook_mutex;

std::unordered_map<HWND, WindowInfo> g_windows;
std::unordered_map<HDC, DcInfo> g_dcs;
std::unordered_map<HBITMAP, DibInfo> g_dibs;
std::unordered_set<HMODULE> g_hooked_modules;
FrameCaptureState g_capture_state;

std::wstring SafeWideString(LPCWSTR text) {
  if (text == nullptr) {
    return L"(null)";
  }
  return std::wstring(text);
}

std::wstring ConvertAnsiToWide(const char *text) {
  if (text == nullptr) {
    return L"(null)";
  }
  const int length = static_cast<int>(std::strlen(text));
  if (length == 0) {
    return std::wstring();
  }
  const int chars_needed =
      MultiByteToWideChar(CP_ACP, 0, text, length, nullptr, 0);
  if (chars_needed <= 0) {
    return L"(conversion failed)";
  }
  std::wstring wide(static_cast<size_t>(chars_needed), L'\0');
  const int converted = MultiByteToWideChar(CP_ACP, 0, text, length,
                                            wide.data(), chars_needed);
  if (converted <= 0) {
    return L"(conversion failed)";
  }
  return wide;
}

void LogTimestampPrefix(FILE *file) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fwprintf(file, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear,
                st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
}

FARPROC ResolveOriginal(const char *module_name, const char *function_name) {
  HMODULE module = GetModuleHandleA(module_name);
  if (module == nullptr) {
    module = LoadLibraryA(module_name);
  }
  if (module == nullptr) {
    return nullptr;
  }
  return GetProcAddress(module, function_name);
}

void LogLocked(const wchar_t *format, va_list args) {
  if (!g_logger.initialized || g_logger.file == nullptr) {
    return;
  }
  LogTimestampPrefix(g_logger.file);
  std::vfwprintf(g_logger.file, format, args);
  std::fputwc(L'\n', g_logger.file);
  std::fflush(g_logger.file);
}

void LogUnlocked(const wchar_t *format, ...) {
  std::lock_guard<std::mutex> lock(g_logger.mutex);
  va_list args;
  va_start(args, format);
  LogLocked(format, args);
  va_end(args);
}

void RememberWindow(HWND hwnd, const WindowInfo &info) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  g_windows[hwnd] = info;
}

void ForgetWindow(HWND hwnd) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  g_windows.erase(hwnd);
}

void RememberDc(HDC dc, const DcInfo &info) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  g_dcs[dc] = info;
}

void ForgetDc(HDC dc) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  g_dcs.erase(dc);
}

void UpdateDcSelection(HDC dc, HBITMAP bitmap) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  auto it = g_dcs.find(dc);
  if (it != g_dcs.end()) {
    it->second.selected_bitmap = bitmap;
  }
}

void RememberDib(HBITMAP bitmap, const DibInfo &info) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  g_dibs[bitmap] = info;
}

void ForgetDib(HBITMAP bitmap) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  g_dibs.erase(bitmap);
}

bool CaptureFromDcLocked(HDC dc, int width, int height) {
  if (!g_capture_state.active || width != g_capture_state.width ||
      height != g_capture_state.height) {
    return false;
  }
  auto dc_it = g_dcs.find(dc);
  if (dc_it == g_dcs.end()) {
    return false;
  }
  const HBITMAP selected = dc_it->second.selected_bitmap;
  if (selected == nullptr) {
    return false;
  }
  auto dib_it = g_dibs.find(selected);
  if (dib_it == g_dibs.end()) {
    return false;
  }
  const DibInfo &dib = dib_it->second;
  if (dib.bit_count != 32 || dib.width != width || dib.height != height ||
      dib.bits == nullptr) {
    return false;
  }

  const uint8_t *src_bytes =
      reinterpret_cast<const uint8_t *>(dib.bits);
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  const size_t total_bytes = row_bytes * static_cast<size_t>(height);
  g_capture_state.rgba.resize(total_bytes);
  for (int y = 0; y < height; ++y) {
    const int src_y = dib.top_down ? y : (height - 1 - y);
    const uint8_t *src_row = src_bytes + static_cast<size_t>(src_y) *
                                           static_cast<size_t>(dib.pitch);
    uint8_t *dst_row = g_capture_state.rgba.data() +
                       static_cast<size_t>(y) * row_bytes;
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      dst_row[offset + 0] = src_row[offset + 2];
      dst_row[offset + 1] = src_row[offset + 1];
      dst_row[offset + 2] = src_row[offset + 0];
      dst_row[offset + 3] = src_row[offset + 3];
    }
  }

  g_capture_state.generation++;
  return true;
}

bool TryCaptureFromBitBlt(HDC dest, HDC src, int width, int height) {
  std::lock_guard<std::mutex> state_lock(g_state_mutex);
  const bool from_src = CaptureFromDcLocked(src, width, height);
  const bool from_dest = from_src ? false : CaptureFromDcLocked(dest, width, height);
  return from_src || from_dest;
}

using CreateWindowExWFunc =
    HWND(WINAPI *)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND,
                   HMENU, HINSTANCE, LPVOID);

HWND WINAPI HookCreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName,
                                LPCWSTR lpWindowName, DWORD dwStyle, int X,
                                int Y, int nWidth, int nHeight, HWND hWndParent,
                                HMENU hMenu, HINSTANCE hInstance,
                                LPVOID lpParam) {
  auto original = reinterpret_cast<CreateWindowExWFunc>(
      ResolveOriginal("USER32.dll", "CreateWindowExW"));
  LogUnlocked(L"CreateWindowExW class='%ls' title='%ls' ex=0x%08X style=0x%08X parent=%p",
              lpClassName ? lpClassName : L"(null)",
              lpWindowName ? lpWindowName : L"(null)", dwExStyle, dwStyle,
              hWndParent);
  HWND hwnd = nullptr;
  if (original != nullptr) {
    hwnd = original(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth,
                    nHeight, hWndParent, hMenu, hInstance, lpParam);
  }
  LogUnlocked(L"CreateWindowExW => %p", hwnd);
  if (hwnd != nullptr) {
    WindowInfo info;
    info.class_name = SafeWideString(lpClassName);
    info.title = SafeWideString(lpWindowName);
    info.style = dwStyle;
    info.ex_style = dwExStyle;
    info.parent = hWndParent;
    RememberWindow(hwnd, info);
  }
  return hwnd;
}

using CreateWindowExAFunc =
    HWND(WINAPI *)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND,
                   HMENU, HINSTANCE, LPVOID);

HWND WINAPI HookCreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName,
                                LPCSTR lpWindowName, DWORD dwStyle, int X,
                                int Y, int nWidth, int nHeight, HWND hWndParent,
                                HMENU hMenu, HINSTANCE hInstance,
                                LPVOID lpParam) {
  auto original = reinterpret_cast<CreateWindowExAFunc>(
      ResolveOriginal("USER32.dll", "CreateWindowExA"));
  std::wstring class_name = ConvertAnsiToWide(lpClassName);
  std::wstring window_name = ConvertAnsiToWide(lpWindowName);
  LogUnlocked(L"CreateWindowExA class='%ls' title='%ls' ex=0x%08X style=0x%08X parent=%p",
              class_name.c_str(), window_name.c_str(), dwExStyle, dwStyle,
              hWndParent);
  HWND hwnd = nullptr;
  if (original != nullptr) {
    hwnd = original(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth,
                    nHeight, hWndParent, hMenu, hInstance, lpParam);
  }
  LogUnlocked(L"CreateWindowExA => %p", hwnd);
  if (hwnd != nullptr) {
    WindowInfo info;
    info.class_name = class_name;
    info.title = window_name;
    info.style = dwStyle;
    info.ex_style = dwExStyle;
    info.parent = hWndParent;
    RememberWindow(hwnd, info);
  }
  return hwnd;
}

using DestroyWindowFunc = BOOL(WINAPI *)(HWND);

BOOL WINAPI HookDestroyWindow(HWND hwnd) {
  auto original =
      reinterpret_cast<DestroyWindowFunc>(ResolveOriginal("USER32.dll", "DestroyWindow"));
  LogUnlocked(L"DestroyWindow hwnd=%p", hwnd);
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(hwnd);
  }
  if (result) {
    ForgetWindow(hwnd);
  }
  return result;
}

using GetDCFunc = HDC(WINAPI *)(HWND);

HDC WINAPI HookGetDC(HWND hwnd) {
  auto original = reinterpret_cast<GetDCFunc>(ResolveOriginal("USER32.dll", "GetDC"));
  HDC dc = nullptr;
  if (original != nullptr) {
    dc = original(hwnd);
  }
  LogUnlocked(L"GetDC hwnd=%p => %p", hwnd, dc);
  if (dc != nullptr) {
    DcInfo info;
    info.owner = hwnd;
    info.is_window_dc = true;
    RememberDc(dc, info);
  }
  return dc;
}

using GetWindowDCFunc = HDC(WINAPI *)(HWND);

HDC WINAPI HookGetWindowDC(HWND hwnd) {
  auto original = reinterpret_cast<GetWindowDCFunc>(
      ResolveOriginal("USER32.dll", "GetWindowDC"));
  HDC dc = nullptr;
  if (original != nullptr) {
    dc = original(hwnd);
  }
  LogUnlocked(L"GetWindowDC hwnd=%p => %p", hwnd, dc);
  if (dc != nullptr) {
    DcInfo info;
    info.owner = hwnd;
    info.is_window_dc = true;
    RememberDc(dc, info);
  }
  return dc;
}

using ReleaseDCFunc = int(WINAPI *)(HWND, HDC);

int WINAPI HookReleaseDC(HWND hwnd, HDC dc) {
  auto original = reinterpret_cast<ReleaseDCFunc>(
      ResolveOriginal("USER32.dll", "ReleaseDC"));
  int result = 0;
  if (original != nullptr) {
    result = original(hwnd, dc);
  }
  LogUnlocked(L"ReleaseDC hwnd=%p dc=%p => %d", hwnd, dc, result);
  if (result == 1) {
    ForgetDc(dc);
  }
  return result;
}

using CreateCompatibleDCFunc = HDC(WINAPI *)(HDC);

HDC WINAPI HookCreateCompatibleDC(HDC dc) {
  auto original = reinterpret_cast<CreateCompatibleDCFunc>(
      ResolveOriginal("GDI32.dll", "CreateCompatibleDC"));
  HDC new_dc = nullptr;
  if (original != nullptr) {
    new_dc = original(dc);
  }
  LogUnlocked(L"CreateCompatibleDC source=%p => %p", dc, new_dc);
  if (new_dc != nullptr) {
    DcInfo info;
    info.owner = nullptr;
    info.is_memory_dc = true;
    RememberDc(new_dc, info);
  }
  return new_dc;
}

using DeleteDCFunc = BOOL(WINAPI *)(HDC);

BOOL WINAPI HookDeleteDC(HDC dc) {
  auto original =
      reinterpret_cast<DeleteDCFunc>(ResolveOriginal("GDI32.dll", "DeleteDC"));
  LogUnlocked(L"DeleteDC %p", dc);
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(dc);
  }
  if (result) {
    ForgetDc(dc);
  }
  return result;
}

using CreateDIBSectionFunc = HBITMAP(WINAPI *)(HDC, const BITMAPINFO *, UINT,
                                               void **, HANDLE, DWORD);

HBITMAP WINAPI HookCreateDIBSection(HDC dc, const BITMAPINFO *bmi, UINT usage,
                                    void **bits, HANDLE section, DWORD offset) {
  auto original = reinterpret_cast<CreateDIBSectionFunc>(
      ResolveOriginal("GDI32.dll", "CreateDIBSection"));
  HBITMAP bitmap = nullptr;
  if (original != nullptr) {
    bitmap = original(dc, bmi, usage, bits, section, offset);
  }
  if (bmi != nullptr) {
    const BITMAPINFOHEADER &hdr = bmi->bmiHeader;
    LogUnlocked(L"CreateDIBSection dc=%p %ldx%ld bitCount=%d compression=%u => %p bits=%p",
                dc, hdr.biWidth, hdr.biHeight, hdr.biBitCount, hdr.biCompression,
                bitmap, bits ? *bits : nullptr);
    if (bitmap != nullptr && bits != nullptr && *bits != nullptr) {
      DibInfo info;
      info.bits = *bits;
      info.width = std::abs(hdr.biWidth);
      info.height = std::abs(hdr.biHeight);
      info.bit_count = hdr.biBitCount;
      info.compression = hdr.biCompression;
      info.top_down = hdr.biHeight < 0;
      const int bytes_per_pixel = hdr.biBitCount / 8;
      const int stride_bits = hdr.biWidth * hdr.biBitCount;
      const int stride_bytes = ((stride_bits + 31) / 32) * 4;
      info.pitch = stride_bytes;
      RememberDib(bitmap, info);
    }
  } else {
    LogUnlocked(L"CreateDIBSection dc=%p => %p (no BMI)", dc, bitmap);
  }
  return bitmap;
}

using DeleteObjectFunc = BOOL(WINAPI *)(HGDIOBJ);

BOOL WINAPI HookDeleteObject(HGDIOBJ object) {
  auto original = reinterpret_cast<DeleteObjectFunc>(
      ResolveOriginal("GDI32.dll", "DeleteObject"));
  LogUnlocked(L"DeleteObject %p", object);
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(object);
  }
  if (result) {
    ForgetDib(static_cast<HBITMAP>(object));
  }
  return result;
}

using SelectObjectFunc = HGDIOBJ(WINAPI *)(HDC, HGDIOBJ);

HGDIOBJ WINAPI HookSelectObject(HDC dc, HGDIOBJ object) {
  auto original = reinterpret_cast<SelectObjectFunc>(
      ResolveOriginal("GDI32.dll", "SelectObject"));
  HGDIOBJ previous = nullptr;
  if (original != nullptr) {
    previous = original(dc, object);
  }
  LogUnlocked(L"SelectObject dc=%p object=%p => %p", dc, object, previous);
  if (object != nullptr) {
    UpdateDcSelection(dc, static_cast<HBITMAP>(object));
  }
  return previous;
}

using BitBltFunc = BOOL(WINAPI *)(HDC, int, int, int, int, HDC, int, int, DWORD);

BOOL WINAPI HookBitBlt(HDC dest, int x_dest, int y_dest, int width, int height,
                       HDC src, int x_src, int y_src, DWORD rop) {
  auto original = reinterpret_cast<BitBltFunc>(
      ResolveOriginal("GDI32.dll", "BitBlt"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(dest, x_dest, y_dest, width, height, src, x_src, y_src,
                      rop);
  }
  const bool captured = TryCaptureFromBitBlt(dest, src, width, height);
  LogUnlocked(L"BitBlt dest=%p src=%p size=%dx%d rop=0x%08lX => %d%s",
              dest, src, width, height, rop, result,
              captured ? L" [captured]" : L"");
  return result;
}

using StretchBltFunc = BOOL(WINAPI *)(HDC, int, int, int, int, HDC, int, int,
                                      int, int, DWORD);

BOOL WINAPI HookStretchBlt(HDC dest, int x_dest, int y_dest, int width,
                           int height, HDC src, int x_src, int y_src,
                           int src_width, int src_height, DWORD rop) {
  auto original = reinterpret_cast<StretchBltFunc>(
      ResolveOriginal("GDI32.dll", "StretchBlt"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(dest, x_dest, y_dest, width, height, src, x_src, y_src,
                      src_width, src_height, rop);
  }
  const bool captured = TryCaptureFromBitBlt(dest, src, width, height);
  LogUnlocked(L"StretchBlt dest=%p src=%p size=%dx%d rop=0x%08lX => %d%s",
              dest, src, width, height, rop, result,
              captured ? L" [captured]" : L"");
  return result;
}

using SwapBuffersFunc = BOOL(WINAPI *)(HDC);

BOOL WINAPI HookSwapBuffers(HDC dc) {
  auto original = reinterpret_cast<SwapBuffersFunc>(
      ResolveOriginal("GDI32.dll", "SwapBuffers"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(dc);
  }
  LogUnlocked(L"SwapBuffers dc=%p => %d", dc, result);
  return result;
}

using WglCreateContextFunc = HGLRC(WINAPI *)(HDC);

HGLRC WINAPI HookWglCreateContext(HDC dc) {
  auto original = reinterpret_cast<WglCreateContextFunc>(
      ResolveOriginal("OPENGL32.dll", "wglCreateContext"));
  HGLRC ctx = nullptr;
  if (original != nullptr) {
    ctx = original(dc);
  }
  LogUnlocked(L"wglCreateContext dc=%p => %p", dc, ctx);
  return ctx;
}

using WglMakeCurrentFunc = BOOL(WINAPI *)(HDC, HGLRC);

BOOL WINAPI HookWglMakeCurrent(HDC dc, HGLRC ctx) {
  auto original = reinterpret_cast<WglMakeCurrentFunc>(
      ResolveOriginal("OPENGL32.dll", "wglMakeCurrent"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(dc, ctx);
  }
  LogUnlocked(L"wglMakeCurrent dc=%p ctx=%p => %d", dc, ctx, result);
  return result;
}

using WglDeleteContextFunc = BOOL(WINAPI *)(HGLRC);

BOOL WINAPI HookWglDeleteContext(HGLRC ctx) {
  auto original = reinterpret_cast<WglDeleteContextFunc>(
      ResolveOriginal("OPENGL32.dll", "wglDeleteContext"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(ctx);
  }
  LogUnlocked(L"wglDeleteContext ctx=%p => %d", ctx, result);
  return result;
}

using WglSwapLayerBuffersFunc = BOOL(WINAPI *)(HDC, UINT);

BOOL WINAPI HookWglSwapLayerBuffers(HDC dc, UINT planes) {
  auto original = reinterpret_cast<WglSwapLayerBuffersFunc>(
      ResolveOriginal("OPENGL32.dll", "wglSwapLayerBuffers"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(dc, planes);
  }
  LogUnlocked(L"wglSwapLayerBuffers dc=%p planes=0x%X => %d", dc, planes,
              result);
  return result;
}

using BeginPaintFunc = HDC(WINAPI *)(HWND, LPPAINTSTRUCT);

HDC WINAPI HookBeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
  auto original = reinterpret_cast<BeginPaintFunc>(
      ResolveOriginal("USER32.dll", "BeginPaint"));
  HDC dc = nullptr;
  if (original != nullptr) {
    dc = original(hwnd, ps);
  }
  LogUnlocked(L"BeginPaint hwnd=%p => %p", hwnd, dc);
  if (dc != nullptr) {
    DcInfo info;
    info.owner = hwnd;
    info.is_window_dc = true;
    RememberDc(dc, info);
  }
  return dc;
}

using EndPaintFunc = BOOL(WINAPI *)(HWND, const PAINTSTRUCT *);

BOOL WINAPI HookEndPaint(HWND hwnd, const PAINTSTRUCT *ps) {
  auto original = reinterpret_cast<EndPaintFunc>(
      ResolveOriginal("USER32.dll", "EndPaint"));
  BOOL result = FALSE;
  if (original != nullptr) {
    result = original(hwnd, ps);
  }
  LogUnlocked(L"EndPaint hwnd=%p => %d", hwnd, result);
  return result;
}

struct ApiHookSpec {
  const char *dll_name;
  const char *function_name;
  FARPROC replacement;
};

const ApiHookSpec kHookSpecs[] = {
    {"USER32.dll", "CreateWindowExW",
     reinterpret_cast<FARPROC>(&HookCreateWindowExW)},
    {"USER32.dll", "CreateWindowExA",
     reinterpret_cast<FARPROC>(&HookCreateWindowExA)},
    {"USER32.dll", "DestroyWindow",
     reinterpret_cast<FARPROC>(&HookDestroyWindow)},
    {"USER32.dll", "GetDC", reinterpret_cast<FARPROC>(&HookGetDC)},
    {"USER32.dll", "GetWindowDC", reinterpret_cast<FARPROC>(&HookGetWindowDC)},
    {"USER32.dll", "ReleaseDC", reinterpret_cast<FARPROC>(&HookReleaseDC)},
    {"USER32.dll", "BeginPaint", reinterpret_cast<FARPROC>(&HookBeginPaint)},
    {"USER32.dll", "EndPaint", reinterpret_cast<FARPROC>(&HookEndPaint)},
    {"GDI32.dll", "CreateCompatibleDC",
     reinterpret_cast<FARPROC>(&HookCreateCompatibleDC)},
    {"GDI32.dll", "DeleteDC", reinterpret_cast<FARPROC>(&HookDeleteDC)},
    {"GDI32.dll", "CreateDIBSection",
     reinterpret_cast<FARPROC>(&HookCreateDIBSection)},
    {"GDI32.dll", "DeleteObject",
     reinterpret_cast<FARPROC>(&HookDeleteObject)},
    {"GDI32.dll", "SelectObject",
     reinterpret_cast<FARPROC>(&HookSelectObject)},
    {"GDI32.dll", "BitBlt", reinterpret_cast<FARPROC>(&HookBitBlt)},
    {"GDI32.dll", "StretchBlt", reinterpret_cast<FARPROC>(&HookStretchBlt)},
    {"GDI32.dll", "SwapBuffers", reinterpret_cast<FARPROC>(&HookSwapBuffers)},
    {"OPENGL32.dll", "wglCreateContext",
     reinterpret_cast<FARPROC>(&HookWglCreateContext)},
    {"OPENGL32.dll", "wglMakeCurrent",
     reinterpret_cast<FARPROC>(&HookWglMakeCurrent)},
    {"OPENGL32.dll", "wglDeleteContext",
     reinterpret_cast<FARPROC>(&HookWglDeleteContext)},
    {"OPENGL32.dll", "wglSwapLayerBuffers",
     reinterpret_cast<FARPROC>(&HookWglSwapLayerBuffers)},
};

bool EqualsIgnoreCase(const char *a, const char *b) {
  return _stricmp(a, b) == 0;
}

void PatchImport(HMODULE module, const ApiHookSpec &spec) {
  BYTE *base = reinterpret_cast<BYTE *>(module);
  const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return;
  }
  const IMAGE_NT_HEADERS *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY &dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0) {
    return;
  }
  auto import_desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
      base + dir.VirtualAddress);
  for (; import_desc->Name != 0; ++import_desc) {
    const char *dll_name =
        reinterpret_cast<const char *>(base + import_desc->Name);
    if (dll_name == nullptr || !EqualsIgnoreCase(dll_name, spec.dll_name)) {
      continue;
    }
    auto thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(
        base + import_desc->FirstThunk);
    auto orig_thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(
        base + (import_desc->OriginalFirstThunk != 0
                    ? import_desc->OriginalFirstThunk
                    : import_desc->FirstThunk));
    for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) {
        continue;
      }
      auto import_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
          base + orig_thunk->u1.AddressOfData);
      if (import_name == nullptr || import_name->Name[0] == '\0') {
        continue;
      }
      if (!EqualsIgnoreCase(reinterpret_cast<char *>(import_name->Name),
                            spec.function_name)) {
        continue;
      }
      DWORD old_protect = 0;
      if (VirtualProtect(&thunk->u1.Function, sizeof(void *),
                         PAGE_EXECUTE_READWRITE, &old_protect)) {
        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(spec.replacement);
        VirtualProtect(&thunk->u1.Function, sizeof(void *), old_protect,
                       &old_protect);
        LogUnlocked(L"Patched %hs!%hs in module %p",
                    spec.dll_name, spec.function_name, module);
      }
    }
  }
}

} // namespace

void Initialize(const std::wstring &log_path) {
  std::lock_guard<std::mutex> lock(g_logger.mutex);
  if (g_logger.file != nullptr) {
    std::fclose(g_logger.file);
    g_logger.file = nullptr;
  }
  g_logger.file = _wfopen(log_path.c_str(), L"w, ccs=UTF-8");
  if (g_logger.file == nullptr) {
    g_logger.initialized = false;
    return;
  }
  g_logger.initialized = true;
  LogTimestampPrefix(g_logger.file);
  std::fwprintf(g_logger.file, L"Diagnostics log initialized at %ls\n",
                log_path.c_str());
  std::fflush(g_logger.file);
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(g_logger.mutex);
  if (g_logger.file != nullptr) {
    std::fclose(g_logger.file);
    g_logger.file = nullptr;
  }
  g_logger.initialized = false;
}

void InstallForCurrentProcess() {
  InstallForModule(GetModuleHandleW(nullptr));
}

void InstallForModule(HMODULE module) {
  if (module == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (!g_hooked_modules.insert(module).second) {
      return;
    }
  }
  for (const auto &spec : kHookSpecs) {
    PatchImport(module, spec);
  }
}

void RegisterWindowTag(HWND hwnd, const std::wstring &tag) {
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    WindowInfo &info = g_windows[hwnd];
    info.tag = tag;
  }
  LogUnlocked(L"Registered tag '%ls' for hwnd=%p", tag.c_str(), hwnd);
}

void SetCaptureBounds(int width, int height) {
  bool active = (width > 0 && height > 0);
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_capture_state.active = active;
    g_capture_state.width = width;
    g_capture_state.height = height;
    g_capture_state.generation = 0;
    g_capture_state.last_consumed_generation = 0;
    g_capture_state.rgba.clear();
  }
  LogUnlocked(L"Configured capture bounds %dx%d (active=%d)", width, height,
              active ? 1 : 0);
}

bool FetchLastFrame(std::vector<uint8_t> &rgba, uint64_t &frame_index) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!g_capture_state.active ||
      g_capture_state.generation == g_capture_state.last_consumed_generation ||
      g_capture_state.rgba.empty()) {
    return false;
  }
  rgba = g_capture_state.rgba;
  frame_index = g_capture_state.generation;
  g_capture_state.last_consumed_generation = g_capture_state.generation;
  return true;
}

void Logf(const wchar_t *format, ...) {
  std::lock_guard<std::mutex> lock(g_logger.mutex);
  if (!g_logger.initialized || g_logger.file == nullptr) {
    return;
  }
  va_list args;
  va_start(args, format);
  LogLocked(format, args);
  va_end(args);
}

} // namespace diagnostics

#endif // defined(_WIN32)

