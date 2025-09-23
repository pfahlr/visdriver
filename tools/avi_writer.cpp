#include "avi_writer.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

AviWriter::AviWriter() {
  AVIFileInit();
}

AviWriter::~AviWriter() {
  Close();
  AVIFileExit();
}

bool AviWriter::IsOpen() const {
  return stream_ != nullptr;
}

bool AviWriter::Open(const std::wstring &path, int width, int height, int fps) {
  Close();

  if (path.empty() || width <= 0 || height <= 0 || fps <= 0) {
    std::wcerr << L"ERROR: Invalid parameters when opening AVI writer.\n";
    return false;
  }

  width_ = width;
  height_ = height;
  fps_ = fps;
  frame_index_ = 0;

  HRESULT hr = AVIFileOpenW(&avi_file_, path.c_str(), OF_WRITE | OF_CREATE, nullptr);
  if (FAILED(hr) || avi_file_ == nullptr) {
    std::wcerr << L"ERROR: AVIFileOpenW failed for '" << path
               << L"' (HRESULT 0x" << std::hex << hr << std::dec << L").\n";
    avi_file_ = nullptr;
    return false;
  }

  AVISTREAMINFOW info;
  std::memset(&info, 0, sizeof(info));
  info.fccType = streamtypeVIDEO;
  info.dwScale = 1;
  info.dwRate = static_cast<DWORD>(fps_);
  info.dwSuggestedBufferSize = static_cast<DWORD>(width_ * height_ * 4);
  info.rcFrame.right = static_cast<LONG>(width_);
  info.rcFrame.bottom = static_cast<LONG>(height_);

  hr = AVIFileCreateStreamW(avi_file_, &stream_, &info);
  if (FAILED(hr) || stream_ == nullptr) {
    std::wcerr << L"ERROR: AVIFileCreateStreamW failed (HRESULT 0x" << std::hex << hr
               << std::dec << L").\n";
    Close();
    return false;
  }

  if (!SetStreamFormat()) {
    Close();
    return false;
  }

  return true;
}

bool AviWriter::SetStreamFormat() {
  if (stream_ == nullptr) {
    return false;
  }

  BITMAPINFOHEADER bih;
  std::memset(&bih, 0, sizeof(bih));
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = width_;
  bih.biHeight = -height_;
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;
  bih.biSizeImage = static_cast<DWORD>(width_ * height_ * 4);

  HRESULT hr = AVIStreamSetFormat(stream_, 0, &bih, sizeof(bih));
  if (FAILED(hr)) {
    std::wcerr << L"ERROR: AVIStreamSetFormat failed (HRESULT 0x" << std::hex << hr
               << std::dec << L").\n";
    return false;
  }

  return true;
}

bool AviWriter::WriteFrame(const uint8_t *rgba, size_t byte_count) {
  if (!IsOpen()) {
    std::wcerr << L"ERROR: AVI writer is not open.\n";
    return false;
  }

  const size_t expected_bytes = static_cast<size_t>(width_) *
                                static_cast<size_t>(height_) * 4;
  if (rgba == nullptr || byte_count != expected_bytes) {
    std::wcerr << L"ERROR: Invalid frame data for AVI writer.\n";
    return false;
  }

  scratch_.resize(expected_bytes);
  const uint8_t *src = rgba;
  uint8_t *dst = scratch_.data();
  for (size_t i = 0; i < expected_bytes; i += 4) {
    dst[i + 0] = src[i + 2];
    dst[i + 1] = src[i + 1];
    dst[i + 2] = src[i + 0];
    dst[i + 3] = src[i + 3];
  }

  HRESULT hr = AVIStreamWrite(stream_, frame_index_, 1, scratch_.data(),
                              static_cast<LONG>(expected_bytes), 0, nullptr,
                              nullptr);
  if (FAILED(hr)) {
    std::wcerr << L"ERROR: AVIStreamWrite failed on frame " << frame_index_
               << L" (HRESULT 0x" << std::hex << hr << std::dec << L").\n";
    return false;
  }

  ++frame_index_;
  return true;
}

void AviWriter::Close() {
  if (stream_ != nullptr) {
    AVIStreamRelease(stream_);
    stream_ = nullptr;
  }
  if (avi_file_ != nullptr) {
    AVIFileRelease(avi_file_);
    avi_file_ = nullptr;
  }
  scratch_.clear();
  width_ = 0;
  height_ = 0;
  fps_ = 0;
  frame_index_ = 0;
}
