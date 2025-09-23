#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <vfw.h>

class AviWriter {
 public:
  AviWriter();
  ~AviWriter();

  bool Open(const std::wstring &path, int width, int height, int fps);
  bool WriteFrame(const uint8_t *rgba, size_t byte_count);
  void Close();
  bool IsOpen() const;

 private:
  bool SetStreamFormat();

  PAVIFILE avi_file_ = nullptr;
  PAVISTREAM stream_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 0;
  int frame_index_ = 0;
  std::vector<uint8_t> scratch_;
};
