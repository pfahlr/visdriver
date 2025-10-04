#pragma once

#include <cstdarg>
#include <string>

namespace debug_log {

bool Initialize(const std::wstring &path);

void Shutdown();

void Write(const wchar_t *format, ...);

void WriteV(const wchar_t *format, va_list args);

void Flush();

} // namespace debug_log

