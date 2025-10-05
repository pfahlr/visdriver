#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

bool DebugTraceInitialize(const std::wstring &log_path);
void DebugTraceShutdown();

void DebugTraceLog(const wchar_t *format, ...);
void DebugTraceLog(const std::wstring &message);

bool DebugTraceInstallHooksForModule(HMODULE module,
                                     const std::wstring &module_name);

void DebugTraceRegisterTargetWindow(HWND hwnd);
void DebugTraceUnregisterTargetWindow(HWND hwnd);
bool DebugTraceActivateDiagnosticsFallback(HWND hwnd);
void DebugTraceDeactivateDiagnosticsFallback(HWND hwnd);
bool DebugTraceIsDiagnosticsFallbackActive(HWND hwnd);
bool DebugTraceWindowRequiresRealDc(HWND hwnd);

bool DebugTraceConfigureOffscreenSurface(int width, int height);
void DebugTraceResetOffscreenSurface();

bool DebugTraceCaptureOffscreenSurface(std::vector<uint8_t> &out_rgba);

bool DebugTraceConfigureDiagnosticsBuffer(int width, int height);
void DebugTraceResetDiagnosticsBuffer();
bool DebugTraceFetchLastFrame(uint64_t &in_out_generation,
                              std::vector<uint8_t> &out_rgba);
uint64_t DebugTracePeekDiagnosticsGeneration();

bool DebugTraceIsActive();

