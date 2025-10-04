#pragma once

#include <string>

#include <windows.h>

#include <winamp/vis.h>

struct VisHost {
  HMODULE dll = nullptr;
  winampVisHeader *hdr = nullptr;
  winampVisModule *mod = nullptr;
  HWND parent = nullptr;
  HWND child = nullptr;
  HWND vis_window = nullptr;
};

VisHost load_vis(const std::wstring &dll_path, HWND parent);

void unload_vis(VisHost &host);

bool begin_vis(VisHost &host, int width, int height);

void end_vis(VisHost &host);
