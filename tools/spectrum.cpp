#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <kissfft/kiss_fftr.h>

#include <windows.h>

#include <winamp/vis.h>

namespace {

constexpr int kSpectrumBins = 576;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinDecibels = -80.0f;
constexpr float kAmplitudeEpsilon = 1e-6f;

class FftResources {
 public:
  FftResources() = default;
  ~FftResources() { reset(); }

  bool Ensure(int fft_size) {
    if (fft_size <= 0) {
      return false;
    }
    if (fft_size == size_) {
      return true;
    }
    reset();

    cfg_ = kiss_fftr_alloc(fft_size, 0, nullptr, nullptr);
    if (cfg_ == nullptr) {
      return false;
    }

    window_.resize(static_cast<size_t>(fft_size));
    for (int i = 0; i < fft_size; ++i) {
      window_[static_cast<size_t>(i)] = Hann(i, fft_size);
    }

    size_ = fft_size;
    return true;
  }

  kiss_fftr_cfg GetConfig() const { return cfg_; }

  const std::vector<kiss_fft_scalar> &Window() const { return window_; }

 private:
  static kiss_fft_scalar Hann(int index, int size) {
    if (size <= 1) {
      return 1.0f;
    }
    const float ratio = static_cast<float>(index) / static_cast<float>(size - 1);
    return static_cast<kiss_fft_scalar>(
        0.5f * (1.0f - std::cos(2.0f * kPi * ratio)));
  }

  void reset() {
    if (cfg_ != nullptr) {
      kiss_fftr_free(cfg_);
      cfg_ = nullptr;
    }
    window_.clear();
    size_ = 0;
  }

  int size_ = 0;
  kiss_fftr_cfg cfg_ = nullptr;
  std::vector<kiss_fft_scalar> window_;
};

FftResources &GetResources() {
  static FftResources resources;
  return resources;
}

void ClearSpectrum(winampVisModule *mod) {
  if (mod == nullptr) {
    return;
  }
  for (int channel = 0; channel < 2; ++channel) {
    std::fill_n(mod->spectrumData[channel], kSpectrumBins,
                static_cast<uint8_t>(0));
  }
}

float ToDecibels(float magnitude) {
  const float safe = std::max(magnitude, kAmplitudeEpsilon);
  return 20.0f * std::log10(safe);
}

}  // namespace

void fill_spectrum(winampVisModule *mod, const int16_t *pcm, int start_sample,
                   int fft_size) {
  if (mod == nullptr) {
    return;
  }

  if (pcm == nullptr || fft_size <= 0) {
    ClearSpectrum(mod);
    return;
  }

  FftResources &resources = GetResources();
  if (!resources.Ensure(fft_size) || resources.GetConfig() == nullptr) {
    ClearSpectrum(mod);
    return;
  }

  const int complex_bins = fft_size / 2;
  if (complex_bins <= 0) {
    ClearSpectrum(mod);
    return;
  }

  static std::vector<kiss_fft_scalar> input;
  static std::vector<kiss_fft_cpx> output;
  static std::vector<float> magnitudes;

  input.resize(static_cast<size_t>(fft_size));
  output.resize(static_cast<size_t>(complex_bins + 1));
  magnitudes.resize(static_cast<size_t>(complex_bins));

  const float amplitude_scale = 2.0f / static_cast<float>(fft_size);

  for (int channel = 0; channel < 2; ++channel) {
    for (int i = 0; i < fft_size; ++i) {
      const int sample_index = start_sample + i;
      float sample = 0.0f;
      if (sample_index >= 0) {
        const size_t offset = static_cast<size_t>(sample_index) * 2u +
                              static_cast<size_t>(channel);
        sample = static_cast<float>(pcm[offset]) / 32768.0f;
      }
      input[static_cast<size_t>(i)] =
          sample * resources.Window()[static_cast<size_t>(i)];
    }

    kiss_fftr(resources.GetConfig(), input.data(), output.data());

    for (int bin = 0; bin < complex_bins; ++bin) {
      const kiss_fft_cpx &c = output[static_cast<size_t>(bin + 1)];
      const float magnitude =
          std::sqrt(c.r * c.r + c.i * c.i) * amplitude_scale;
      const float db = ToDecibels(magnitude);
      const float normalized = std::clamp((db - kMinDecibels) / -kMinDecibels,
                                          0.0f, 1.0f);
      magnitudes[static_cast<size_t>(bin)] = normalized;
    }

    for (int out_index = 0; out_index < kSpectrumBins; ++out_index) {
      const float position = static_cast<float>(out_index) *
                             static_cast<float>(complex_bins - 1) /
                             static_cast<float>(kSpectrumBins - 1);
      const int left_index = static_cast<int>(position);
      const int right_index =
          std::min(left_index + 1, complex_bins - 1);
      const float right_weight = position - static_cast<float>(left_index);
      const float left_weight = 1.0f - right_weight;
      const float value =
          magnitudes[static_cast<size_t>(left_index)] * left_weight +
          magnitudes[static_cast<size_t>(right_index)] * right_weight;
      const float clamped = std::clamp(value, 0.0f, 1.0f);
      const auto byte_value = static_cast<uint8_t>(
          std::lround(clamped * 255.0f));
      mod->spectrumData[channel][out_index] = byte_value;
    }
  }
}
