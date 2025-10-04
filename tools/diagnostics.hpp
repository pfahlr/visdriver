#pragma once

#include <string>
#include <vector>

#include <windows.h>

namespace diagnostics {

bool Initialize(const std::wstring &log_path, int target_width,
                int target_height);
void Shutdown();

bool HookModule(HMODULE module);

void RegisterPrimaryWindow(HWND hwnd);
void RegisterParentWindow(HWND hwnd);
void RegisterModuleLoad(const std::wstring &path, HMODULE module);

void NoteVisStep(const char *step);
void NoteVisStep(const char *step, int frame_index);

bool CaptureFallbackIfActive(std::vector<uint8_t> &rgba_out);

HWND EnsureOffscreenFallbackWindow(int width, int height);
bool IsFallbackWindow(HWND hwnd);
bool ActivateFallbackForWindow(HWND hwnd, int width, int height);

class ScopedSuppressLogs {
public:
  ScopedSuppressLogs();
  ~ScopedSuppressLogs();
};

} // namespace diagnostics
