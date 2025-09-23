#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

bool capture_child_to_rgba(HWND child, int width, int height,
                           std::vector<uint8_t> &out_rgba);
bool write_png(const std::wstring &path, int width, int height,
               const uint8_t *rgba);
