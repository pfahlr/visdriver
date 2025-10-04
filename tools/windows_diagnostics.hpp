#pragma once

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

namespace diagnostics {

void Initialize(const std::wstring &log_path);
void Shutdown();

void InstallForCurrentProcess();
void InstallForModule(HMODULE module);

void RegisterWindowTag(HWND hwnd, const std::wstring &tag);
void SetCaptureBounds(int width, int height);

bool FetchLastFrame(std::vector<uint8_t> &rgba, uint64_t &frame_index);

void Logf(const wchar_t *format, ...);

} // namespace diagnostics

#else // !defined(_WIN32)

namespace diagnostics {

inline void Initialize(const std::wstring &) {}
inline void Shutdown() {}
inline void InstallForCurrentProcess() {}
inline void InstallForModule(void *) {}
inline void RegisterWindowTag(void *, const std::wstring &) {}
inline void SetCaptureBounds(int, int) {}
inline bool FetchLastFrame(std::vector<uint8_t> &, uint64_t &) { return false; }
inline void Logf(const wchar_t *, ...) {}

} // namespace diagnostics

#endif // defined(_WIN32)

