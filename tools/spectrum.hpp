#pragma once

#include <cstdint>

struct winampVisModule;

void fill_spectrum(winampVisModule *mod, const int16_t *pcm, int start_sample,
                   int fft_size);
