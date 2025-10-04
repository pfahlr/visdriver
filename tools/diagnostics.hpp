#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace diagnostics {

bool Initialize(const std::wstring &log_path);

void Shutdown();

void Log(const wchar_t *format, ...);

bool InstallHooksForCurrentProcess();

bool InstallHooksForModule(HMODULE module, const wchar_t *tag);

void SetExpectedWindows(HWND parent, HWND child);

void NotifyWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

void EnableDummySurface(int width, int height);

bool IsDummySurfaceActive();

bool CaptureDummySurfaceRgba(int width, int height, std::vector<uint8_t> &out_rgba);

void MarkFrameStart(int frame_index);

} // namespace diagnostics

