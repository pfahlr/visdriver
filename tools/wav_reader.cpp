#include "wav_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <windows.h>

namespace {

struct HandleCloser {
  void operator()(HANDLE handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using ScopedHandle = std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleCloser>;

struct RiffChunkHeader {
  char id[4];
  uint32_t size;
};

uint32_t ReadUint32(const uint8_t *ptr) {
  uint32_t value = 0;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}

uint16_t ReadUint16(const uint8_t *ptr) {
  uint16_t value = 0;
  std::memcpy(&value, ptr, sizeof(value));
  return value;
}

bool MatchFourCC(const char *id, const char (&expected)[5]) {
  return std::memcmp(id, expected, 4) == 0;
}

}  // namespace

WavData load_wav_16le_stereo(const std::wstring &path) {
  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file || file.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    throw std::runtime_error("Failed to open WAV file, error " +
                             std::to_string(static_cast<unsigned long>(error)));
  }

  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file.get(), &file_size) || file_size.QuadPart < 0) {
    throw std::runtime_error("Failed to query WAV file size");
  }
  if (file_size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<size_t>::max())) {
    throw std::runtime_error("WAV file too large to load into memory");
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(file_size.QuadPart));
  if (!buffer.empty()) {
    DWORD total_read = 0;
    while (total_read < buffer.size()) {
      DWORD chunk = 0;
      const DWORD to_read = static_cast<DWORD>(std::min<size_t>(buffer.size() - total_read, 1 << 20));
      if (!ReadFile(file.get(), buffer.data() + total_read, to_read, &chunk, nullptr)) {
        throw std::runtime_error("Failed to read WAV file contents");
      }
      if (chunk == 0) {
        throw std::runtime_error("Unexpected EOF while reading WAV file");
      }
      total_read += chunk;
    }
  }

  if (buffer.size() < 12) {
    throw std::runtime_error("WAV file too small");
  }

  const uint8_t *cursor = buffer.data();
  const uint8_t *const end = buffer.data() + buffer.size();

  if (!MatchFourCC(reinterpret_cast<const char *>(cursor), "RIFF")) {
    throw std::runtime_error("WAV file missing RIFF header");
  }
  cursor += 4;

  cursor += 4;  // Skip RIFF chunk size.

  if (cursor + 4 > end ||
      !MatchFourCC(reinterpret_cast<const char *>(cursor), "WAVE")) {
    throw std::runtime_error("WAV file missing WAVE header");
  }
  cursor += 4;

  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  const uint8_t *data_ptr = nullptr;
  uint32_t data_size = 0;

  while (cursor + sizeof(RiffChunkHeader) <= end) {
    const RiffChunkHeader *header =
        reinterpret_cast<const RiffChunkHeader *>(cursor);
    cursor += sizeof(RiffChunkHeader);
    const uint32_t chunk_size = header->size;
    const uint8_t *chunk_data = cursor;
    if (cursor + chunk_size > end) {
      throw std::runtime_error("WAV chunk exceeds file size");
    }

    if (MatchFourCC(header->id, "fmt ")) {
      if (chunk_size < 16) {
        throw std::runtime_error("WAV fmt chunk too small");
      }
      const uint16_t audio_format = ReadUint16(chunk_data + 0);
      channels = ReadUint16(chunk_data + 2);
      sample_rate = ReadUint32(chunk_data + 4);
      const uint16_t block_align = ReadUint16(chunk_data + 12);
      bits_per_sample = ReadUint16(chunk_data + 14);

      if (audio_format != 1) {
        throw std::runtime_error("Only PCM WAV files are supported");
      }
      if (block_align != channels * (bits_per_sample / 8)) {
        throw std::runtime_error("Invalid WAV block alignment");
      }
    } else if (MatchFourCC(header->id, "data")) {
      data_ptr = chunk_data;
      data_size = chunk_size;
    }

    cursor += chunk_size;
    if (chunk_size % 2 == 1) {
      ++cursor;  // Skip padding byte.
    }
  }

  if (channels == 0 || sample_rate == 0 || bits_per_sample == 0) {
    throw std::runtime_error("WAV fmt chunk missing or incomplete");
  }
  if (bits_per_sample != 16) {
    throw std::runtime_error("WAV file must be 16 bits per sample");
  }
  if (channels != 2) {
    throw std::runtime_error("WAV file must be stereo");
  }
  if (data_ptr == nullptr || data_size == 0) {
    throw std::runtime_error("WAV data chunk missing");
  }
  if (data_size % (channels * (bits_per_sample / 8)) != 0) {
    throw std::runtime_error("WAV data chunk has invalid size");
  }

  const size_t sample_count = data_size / sizeof(int16_t);
  std::vector<int16_t> samples(sample_count);
  std::memcpy(samples.data(), data_ptr, data_size);

  WavData result;
  result.sample_rate = static_cast<int>(sample_rate);
  result.channels = static_cast<int>(channels);
  result.pcm = std::move(samples);
  return result;
}

std::vector<int16_t> resample_linear_2ch_16bit(const std::vector<int16_t> &in,
                                               int in_sr, int out_sr) {
  if (in_sr <= 0 || out_sr <= 0) {
    throw std::invalid_argument("Sample rates must be positive");
  }
  if (in.size() % 2 != 0) {
    throw std::invalid_argument("Input PCM must have an even number of samples");
  }
  if (out_sr == in_sr) {
    return in;
  }
  const size_t in_frames = in.size() / 2;
  if (in_frames == 0) {
    return {};
  }

  const uint64_t numerator = static_cast<uint64_t>(in_frames) * static_cast<uint64_t>(out_sr);
  const size_t out_frames = static_cast<size_t>((numerator + in_sr / 2) / static_cast<uint64_t>(in_sr));

  std::vector<int16_t> out(out_frames * 2);
  const double position_scale = static_cast<double>(in_sr) / static_cast<double>(out_sr);

  for (size_t frame = 0; frame < out_frames; ++frame) {
    const double pos = position_scale * static_cast<double>(frame);
    size_t index = static_cast<size_t>(pos);
    if (index >= in_frames) {
      index = in_frames - 1;
    }
    const double frac = std::clamp(pos - static_cast<double>(index), 0.0, 1.0);
    const size_t next_index = (index + 1 < in_frames) ? (index + 1) : index;

    for (int channel = 0; channel < 2; ++channel) {
      const int16_t s0 = in[index * 2 + channel];
      const int16_t s1 = in[next_index * 2 + channel];
      const double interpolated = static_cast<double>(s0) +
                                  (static_cast<double>(s1) - static_cast<double>(s0)) * frac;
      const long rounded = std::lround(interpolated);
      out[frame * 2 + channel] = static_cast<int16_t>(std::clamp<long>(
          rounded, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
    }
  }

  return out;
}
