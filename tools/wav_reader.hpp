#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct WavData {
  int sample_rate = 0;
  int channels = 0;
  std::vector<int16_t> pcm;
};

WavData load_wav_16le_stereo(const std::wstring &path);

std::vector<int16_t> resample_linear_2ch_16bit(const std::vector<int16_t> &in,
                                               int in_sr, int out_sr);
