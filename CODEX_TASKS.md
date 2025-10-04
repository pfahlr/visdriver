
# Step 0 — Create the subcommand skeleton (no DLL, no WAV yet)

**Codex Prompt 0**

> **Goal:** Add a new CLI subcommand `generate-verification-data` (Windows only) that parses arguments and prints them back. No rendering yet.
>
> **Tasks**
>
> 1. Add a new source file `tools/generate_verification_data.cpp` that implements a function:
>
>    ```cpp
>    int cmd_generate_verification_data(int argc, wchar_t** argv);
>    ```
>
>    Parse these args (with defaults):
>    `--vis-dll <path>` (required), `--runtime-dir <dir>` (optional, default: directory of vis-dll), `--vis-avs-dat <path>` (optional), `--preset <path>` (optional),
>    `--wav <path>` (required), `--width 640`, `--height 480`, `--fps 60`, `--frames 121`,
>    `--out-dir <dir>` (required), `--avi-out <filename>` (optional), `--png-step 1`,
>    `--hash-mode pixels|rolling` (default pixels).
> 2. Wire `src/main.cpp` (or the main entry) to dispatch:
>
>    * `visdriver.exe generate-verification-data ...` → call `cmd_generate_verification_data`.
> 3. Print a neat summary of parsed options and return 0. Do not touch existing commands.
> 4. Update CMake to compile `tools/generate_verification_data.cpp`.
>
> **Acceptance**
>
> * `visdriver.exe generate-verification-data --help` shows options.
> * Running with required args prints them and exits 0.
> * CI/build stays green on Windows.

---

# Step 1 — Win32 window + vis DLL loading (no audio yet)

**Codex Prompt 1**

> **Goal:** Load `vis_avs.dll`, fetch `winampVisGetHeader`, get the first module, and create a parent/child window the plugin can render into. No rendering loop yet.
>
> **Tasks**
>
> 1. Add `tools/vis_host.hpp/.cpp` with:
>
>    * `struct VisHost { HMODULE dll; winampVisHeader* hdr; winampVisModule* mod; HWND parent; HWND child; };`
>    * `VisHost load_vis(const std::wstring& dll_path, HWND parent);`
>    * `void unload_vis(VisHost&);`
> 2. In `cmd_generate_verification_data`, after parsing, set `SetCurrentDirectoryW(runtime_dir)` and call `load_vis`. Create a 640×480 (or given size) parent HWND (hidden) and a child client area HWND for AVS to draw in.
> 3. Fill required fields on `mod` (hwndParent, hDllInstance, sRate=44100, nCh=2, latencyMs=0, delayMs=1000/fps, spectrumNch=2, waveformNch=2). **Do not call Init yet.**
> 4. Log clear errors if the symbol or header is missing.
>
> **Acceptance**
>
> * `generate-verification-data` with a real `--vis-dll` starts and prints “loaded vis module” then exits.
> * No crashes; parent/child window created and destroyed cleanly.

---

# Step 2 — WAV reader (no FFT yet)

**Codex Prompt 2**

> **Goal:** Decode a WAV (16-bit PCM stereo). If sample rate ≠ 44100, linearly resample to 44.1k.
>
> **Tasks**
>
> 1. Add `tools/wav_reader.hpp/.cpp` with:
>
>    ```cpp
>    struct WavData { int sample_rate; int channels; std::vector<int16_t> pcm; };
>    WavData load_wav_16le_stereo(const std::wstring& path); // throws on error
>    std::vector<int16_t> resample_linear_2ch_16bit(const std::vector<int16_t>& in, int in_sr, int out_sr);
>    ```
> 2. In `cmd_generate_verification_data`, load `--wav`, assert stereo, resample to 44100 if needed.
> 3. Print a line with total samples after resample.
>
> **Acceptance**
>
> * Running with a small WAV prints “WAV ok: 44100 Hz, 2 ch, N samples”.

---

# Step 3 — Init/Quit + dry render loop (no spectrum/waveform fill yet)

**Codex Prompt 3**

> **Goal:** Call `mod->Init(mod)`, then step a fixed number of frames calling `mod->Render(mod)`, then `mod->Quit(mod)`. For now, feed zeroed waveform/spectrum buffers.
>
> **Tasks**
>
> 1. Add helpers `begin_vis(VisHost&, int width, int height)` and `end_vis(VisHost&)`.
> 2. Before the loop, set SSE FTZ/DAZ:
>
>    ```cpp
>    #include <xmmintrin.h>
>    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
>    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
>    ```
> 3. For frames=F: set `mod->waveformData` and `mod->spectrumData` to mid/zero values (128 / 0), then call `Render(mod)`.
>
> **Acceptance**
>
> * Runs frames without crashing and exits.

---

# Step 4 — Waveform fill from WAV

**Codex Prompt 4**

> **Goal:** Populate `waveformData[2][576]` per frame from the WAV.
>
> **Tasks**
>
> 1. Implement `fill_waveform(winampVisModule* mod, const int16_t* pcm, int start_sample, int hop)`. Map \[-32768..32767] → \[0..255] via `uint8_t((s/32768.0f)*127+128)`.
> 2. In the loop: compute `samples_per_frame = round(44100.0 / fps)`, `hop = max(1, samples_per_frame / 576)`, and `start = f * samples_per_frame`.
> 3. Call `fill_waveform(mod, pcm, start, hop)`.
>
> **Acceptance**
>
> * No visual output yet, but the loop runs and logs per-frame progress.

---

# Step 5 — Spectrum via kissfft (add third\_party)

**Codex Prompt 5**

> **Goal:** Compute `spectrumData[2][576]` using a 1024-pt real FFT (kissfft), normalized to 0..255.
>
> **Tasks**
>
> 1. Vendor minimal kissfft under `third_party/kissfft/` and add to CMake.
> 2. Add `tools/spectrum.hpp/.cpp`:
>
>    ```cpp
>    void fill_spectrum(winampVisModule* mod, const int16_t* pcm, int start_sample, int fft_size /*1024*/);
>    ```
>
>    * Window (Hann), run FFT per channel on 1024 samples from `start_sample`.
>    * Magnitude → log/lin scale → resample/clip to 576 bins → 0..255 `uint8_t`.
> 3. Call `fill_spectrum` each frame before `Render`.
>
> **Acceptance**
>
> * Builds clean; logs “spectrum ok”.

---

# Step 6 — Frame capture to PNG sequence

**Codex Prompt 6**

> **Goal:** Capture the plugin child window each frame into a 32-bit buffer and write PNGs every `--png-step` frames.
>
> **Tasks**
>
> 1. Vendor `third_party/stb/stb_image_write.h`.
> 2. Add `tools/capture.hpp/.cpp` with:
>
>    ```cpp
>    bool capture_child_to_rgba(HWND child, int w, int h, std::vector<uint8_t>& out_rgba); // BGRA -> RGBA
>    bool write_png(const std::wstring& path, int w, int h, const uint8_t* rgba);
>    ```
>
>    Use `CreateDIBSection` + `BitBlt`, convert BGRA→RGBA.
> 3. In the loop: if `(f % png_step)==0` write `out_dir/frames/frame%04d.png`.
> 4. Create directories `out_dir/frames` if missing.
>
> **Acceptance**
>
> * Produces a handful of PNGs with the right dimensions.

---

# Step 7 — Per-frame + rolling SHA-256 hashes

**Codex Prompt 7**

> **Goal:** Compute SHA-256 of decoded pixels per frame and a rolling hash over all frames; write `per_frame.csv` and `rolling_sha256.txt`.
>
> **Tasks**
>
> 1. Add `tools/sha256.hpp/.cpp` (tiny self-contained SHA-256 or use Windows CNG/BCrypt).
> 2. In the loop: after capture, compute `sha256(rgba_bytes)` and append `frame_index,hex` to `out_dir/hashes/per_frame.csv`.
> 3. Maintain a running context updated with each frame’s RGBA bytes. After the loop, write hex digest to `out_dir/hashes/rolling_sha256.txt`.
> 4. Ensure `out_dir/hashes/` is created.
>
> **Acceptance**
>
> * CSV with N rows (one per frame) and a non-empty rolling hash file.

---

# Step 8 — Manifest JSON

**Codex Prompt 8**

> **Goal:** Emit a machine-readable `manifest.json` summarizing inputs/outputs.
>
> **Tasks**
>
> 1. Add `tools/manifest.hpp/.cpp` using a minimal JSON writer (no external dep).
> 2. Include: tool version (hardcode “visdriver+verify/0.1”), DLL path, runtime dir, wav sample rate/channels/sample count, width/height/fps/frames, paths of PNGs created, per-frame hashes (aggregate or reference CSV), rolling hash, and `avi_out` if present.
> 3. Write to `out_dir/manifest.json` (UTF-8, Windows newlines OK).
>
> **Acceptance**
>
> * Valid JSON with all the fields; paths are relative to `out_dir` where sensible.

---

# Step 9 — Optional AVI writer (uncompressed BI\_RGB)

**Codex Prompt 9**

> **Goal:** If `--avi-out` given, write an uncompressed 32-bit AVI alongside PNGs.
>
> **Tasks**
>
> 1. Add `tools/avi_writer.hpp/.cpp` using `Vfw.h` (AVIFile API). Link `Vfw32.lib`.
> 2. Open stream on first frame; write each frame’s RGBA as BI\_RGB 32-bit (no compression).
> 3. Close cleanly at the end; place file at `out_dir/<avi_out>`.
>
> **Acceptance**
>
> * AVI plays at the requested FPS; file size \~= frames × width × height × 4 (+ header).

---

## How to run after each step

* Install the Ubuntu packages listed in `.ci/ubuntu-packages.txt`:
  `sudo apt-get update && sudo apt-get install --yes build-essential cmake ccache mingw-w64 wine`
* Build (MSVC):
  `cmake -S . -B build -A x64 && cmake --build build -j`
* Smoke (with your real files):

  ```
  build\bin\visdriver.exe generate-verification-data ^
    --vis-dll assets\vis_avs.dll ^
    --runtime-dir assets ^
    --vis-avs-dat assets\vis_avs.dat ^
    --preset tests\data\phase1\fadeout_color.avs ^
    --wav tests\audio\fixture.wav ^
    --width 640 --height 480 --fps 60 --frames 121 ^
    --out-dir out\phase1\fadeout_color ^
    --png-step 10
  ```

