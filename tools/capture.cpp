#include "capture.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <type_traits>

#include "debug_trace.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace {

struct WindowDcDeleter {
  HWND hwnd = nullptr;
  void operator()(HDC dc) const {
    if (dc != nullptr && hwnd != nullptr) {
      ReleaseDC(hwnd, dc);
    }
  }
};

struct DcDeleter {
  void operator()(HDC dc) const {
    if (dc != nullptr) {
      DeleteDC(dc);
    }
  }
};

struct BitmapDeleter {
  void operator()(HBITMAP bitmap) const {
    if (bitmap != nullptr) {
      DeleteObject(bitmap);
    }
  }
};

using unique_window_dc =
    std::unique_ptr<std::remove_pointer<HDC>::type, WindowDcDeleter>;
using unique_dc = std::unique_ptr<std::remove_pointer<HDC>::type, DcDeleter>;
using unique_bitmap =
    std::unique_ptr<std::remove_pointer<HBITMAP>::type, BitmapDeleter>;

std::wstring FormatWindowsErrorMessage(DWORD error_code) {
  wchar_t *buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
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

void stb_write_to_file(void *context, void *data, int size) {
  if (context == nullptr || data == nullptr || size <= 0) {
    return;
  }
  std::FILE *file = static_cast<std::FILE *>(context);
  std::fwrite(data, 1, static_cast<size_t>(size), file);
}

} // namespace

bool capture_child_to_rgba(HWND child, int width, int height,
                           std::vector<uint8_t> &out_rgba) {
  if (child == nullptr || width <= 0 || height <= 0) {
    std::wcerr << L"ERROR: Invalid parameters for capture_child_to_rgba.\n";
    DebugTraceLog(L"capture_child_to_rgba invalid args hwnd=%p width=%d height=%d",
                  child, width, height);
    return false;
  }

  DebugTraceLog(L"capture_child_to_rgba(hwnd=%p, width=%d, height=%d)", child, width,
                height);

  unique_window_dc window_dc(GetDC(child), WindowDcDeleter{child});
  if (!window_dc) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: GetDC failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"capture_child_to_rgba: GetDC failed hwnd=%p error=%lu", child,
                  error);
    return false;
  }

  unique_dc memory_dc(CreateCompatibleDC(window_dc.get()));
  if (!memory_dc) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: CreateCompatibleDC failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"capture_child_to_rgba: CreateCompatibleDC failed hwnd=%p error=%lu",
                  child, error);
    return false;
  }

  BITMAPINFO bmi;
  std::memset(&bmi, 0, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height; // top-down DIB
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *bits = nullptr;
  unique_bitmap dib(CreateDIBSection(memory_dc.get(), &bmi, DIB_RGB_COLORS,
                                     &bits, nullptr, 0));
  if (!dib || bits == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: CreateDIBSection failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"capture_child_to_rgba: CreateDIBSection failed hwnd=%p error=%lu",
                  child, error);
    return false;
  }

  HGDIOBJ old_bitmap = SelectObject(memory_dc.get(), dib.get());
  if (old_bitmap == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: SelectObject failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"capture_child_to_rgba: SelectObject failed hwnd=%p error=%lu",
                  child, error);
    return false;
  }

  const BOOL blt_result = BitBlt(memory_dc.get(), 0, 0, width, height,
                                 window_dc.get(), 0, 0, SRCCOPY);
  if (!blt_result) {
    const DWORD error = GetLastError();
    SelectObject(memory_dc.get(), old_bitmap);
    std::wcerr << L"ERROR: BitBlt failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"capture_child_to_rgba: BitBlt failed hwnd=%p error=%lu", child,
                  error);
    return false;
  }

  const size_t pixel_count = static_cast<size_t>(width) *
                             static_cast<size_t>(height);
  out_rgba.resize(pixel_count * 4);
  const uint8_t *src = static_cast<const uint8_t *>(bits);
  uint8_t *dst = out_rgba.data();
  for (size_t i = 0; i < pixel_count; ++i) {
    const size_t offset = i * 4;
    dst[offset + 0] = src[offset + 2]; // R
    dst[offset + 1] = src[offset + 1]; // G
    dst[offset + 2] = src[offset + 0]; // B
    dst[offset + 3] = src[offset + 3]; // A
  }

  SelectObject(memory_dc.get(), old_bitmap);
  DebugTraceLog(L"capture_child_to_rgba: success hwnd=%p", child);
  return true;
}

bool write_png(const std::wstring &path, int width, int height,
               const uint8_t *rgba) {
  if (path.empty() || width <= 0 || height <= 0 || rgba == nullptr) {
    std::wcerr << L"ERROR: Invalid parameters for write_png.\n";
    return false;
  }

  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
      _wfopen(path.c_str(), L"wb"), &std::fclose);
  if (!file) {
    const int err_code = errno;
    std::wcerr << L"ERROR: Failed to open '" << path
               << L"' for writing (errno " << err_code << L").\n";
    return false;
  }

  const int stride = width * 4;
  const int result = stbi_write_png_to_func(stb_write_to_file, file.get(), width,
                                            height, 4, rgba, stride);
  if (result == 0) {
    std::wcerr << L"ERROR: stbi_write_png_to_func failed for '" << path
               << L"'.\n";
    return false;
  }

  return true;
}
