#include "debug_log.hpp"

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>

#include <windows.h>

namespace debug_log {

namespace {

std::mutex g_mutex;
std::FILE *g_file = nullptr;
bool g_initialized = false;

void write_timestamp_locked(std::FILE *file) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  const DWORD thread_id = GetCurrentThreadId();
  std::fwprintf(file, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u][T%lu] ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, static_cast<unsigned long>(thread_id));
}

void ensure_bom(std::FILE *file) {
  if (file == nullptr) {
    return;
  }
  // UTF-8 BOM so the log is easy to inspect in Notepad.
  std::fputc(0xEF, file);
  std::fputc(0xBB, file);
  std::fputc(0xBF, file);
}

} // namespace

bool Initialize(const std::wstring &path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != nullptr) {
    std::fclose(g_file);
    g_file = nullptr;
  }

  g_file = _wfopen(path.c_str(), L"wb");
  if (g_file == nullptr) {
    g_initialized = false;
    return false;
  }

  ensure_bom(g_file);
  g_initialized = true;

  write_timestamp_locked(g_file);
  std::fwprintf(g_file, L"Diagnostics log initialized at '%ls'\n", path.c_str());
  std::fflush(g_file);
  return true;
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized) {
    return;
  }
  if (g_file != nullptr) {
    write_timestamp_locked(g_file);
    std::fputws(L"Diagnostics log shutting down\n", g_file);
    std::fflush(g_file);
    std::fclose(g_file);
    g_file = nullptr;
  }
  g_initialized = false;
}

void WriteV(const wchar_t *format, va_list args) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized || g_file == nullptr) {
    return;
  }

  write_timestamp_locked(g_file);
  std::vfwprintf(g_file, format, args);
  if (std::wcslen(format) == 0 || format[std::wcslen(format) - 1] != L'\n') {
    std::fputws(L"\n", g_file);
  }
  std::fflush(g_file);
}

void Write(const wchar_t *format, ...) {
  va_list args;
  va_start(args, format);
  WriteV(format, args);
  va_end(args);
}

void Flush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_initialized && g_file != nullptr) {
    std::fflush(g_file);
  }
}

} // namespace debug_log

