# AGENTS.md

## Setup

* Build with **CMake** and **MinGW**.
* Ubuntu/Debian environments must enable multi-arch *before* refreshing the
  package lists so that 32-bit Wine packages are available:

  ```bash
  sudo dpkg --add-architecture i386
  sudo apt-get update
  sudo apt-get install --yes --no-install-recommends \
      build-essential cmake ccache mingw-w64 \
      wine wine64 wine32:i386 winbind xvfb
  ```

  The package list mirrors `.ci/ubuntu-packages.txt`.

* If `wine --version` or `/usr/lib/i386-linux-gnu/glib-2.0/glib-compile-schemas`
  reports `Exec format error`, the host kernel is missing
  `CONFIG_IA32_EMULATION` and cannot execute 32-bit binaries. Use a kernel or
  VM that supports 32-bit userspace before attempting to run visdriver.
* Dependencies are vendored header-only or small C libraries; no additional
  downloads are required beyond the system packages above.

### Build

* Configure via CMake using the MinGW toolchain file and build with GNU make:

  ```bash
  cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -S . -B build
  make -C build -j"$(nproc)" VERBOSE=1
  ```

  Copy the helper DLLs and test assets next to the resulting executable when
  running integration tests:

  ```bash
  cp for_codex/dll/* build/
  cp -R for_codex/tests build/
  ```

### Run

* Initialise a dedicated 32-bit Wine prefix once per machine:

  ```bash
  export WINEARCH=win32
  export WINEPREFIX="$HOME/.wine32"
  wineboot -i
  # Optional: DISPLAY must be available for winecfg
  # winecfg
  ```

* Keep `WINEPREFIX` set whenever running the tools. For example:

  ```bash
  cd build
  WINEPREFIX="$HOME/.wine32" wine ./visdriver.exe generate-verification-data \
      --vis-dll ./vis_avs.dll \
      --vis-avs-dat ./vis_avs.dat \
      --runtime-dir ./ \
      --wav ./tests/data/test.wav \
      --preset ./tests/data/phase1/simple.avs \
      --out-dir ./tests/golden/phase1/simple
  ```

* `visdriver.exe generate-verification-data --help` describes all CLI options.
  Ensure `--vis-dll` and `--wav` point at valid Windows paths within the Wine
  prefix or the current directory mapped by Wine.

If any Wine command reports `Exec format error`, confirm the host kernel
supports 32-bit execution before retrying.

---

## Misc. Directives

* Use wide-char everywhere on Windows paths (`std::wstring`, `CreateFileW`, etc.).
* Always `CreateDirectoryW` (recursive helper) before writing.
* Keep the render loop **single-threaded**, and don’t sleep; advance by counter only.
* If `--runtime-dir` is set, call `SetCurrentDirectoryW` before `LoadLibraryW`.

## Code style

* Language: **C++17**, use RAII, `std::vector`, `std::wstring`.
* Keep functions small and modular (`tools/*.cpp` for helpers).
* Prefer explicit error messages, return nonzero on failure.
* Use `CreateDirectoryW` helpers to ensure output dirs exist.
* Use Windows APIs (`LoadLibraryW`, `AVIFile`, `BitBlt`) for system tasks.

---

## Testing

* Manual smoke tests for new subcommands (`generate-verification-data`).
* For each step, confirm the binary builds and runs without crashing.
* Check that deterministic output (hashes, PNGs) is stable across runs.

---

## Pull request rules

* **Do not include binary test artifacts** (`.png`, `.avi`, `.wav`) in Codex PRs.
* If code changes generate binary outputs, package them as a **ZIP** artifact and attach separately.
* PRs must be code-only, with clear instructions in the PR body.
* Maintainer will commit the corresponding binary files manually.

---

## Subcommands

### `generate-verification-data`

A new CLI function to produce deterministic goldens from `vis_avs.dll`.

**Arguments (long only):**

* `--vis-dll <path>` (required)
* `--runtime-dir <dir>` (default: directory of vis-dll)
* `--vis-avs-dat <path>` (optional)
* `--preset <path>` (optional)
* `--wav <path>` (required)
* `--width <int>` (default: 640)
* `--height <int>` (default: 480)
* `--fps <int>` (default: 60)
* `--frames <int>` (default: 121)
* `--out-dir <dir>` (required)
* `--avi-out <filename>` (optional)
* `--png-step <int>` (default: 1, write every Nth frame)
* `--hash-mode pixels|rolling` (default: pixels)

**Outputs:**

* `frames/frame%04d.png` (PNG sequence)
* `hashes/per_frame.csv` (frame index + sha256 of raw pixels)
* `hashes/rolling_sha256.txt` (aggregate hash across all frames)
* `manifest.json` (metadata of inputs/outputs, DLL, WAV, sizes, hashes)
* optional: `*.avi` (uncompressed 32-bit BI_RGB)

---

## Development phases

Codex must work incrementally. Each PR implements **one phase** only.

1. **CLI skeleton**: parse args, print summary.
2. **vis_avs.dll load**: load DLL, create parent/child window.
3. **WAV reader**: parse/resample 16-bit stereo PCM.
4. **Dry render loop**: call Init/Render/Quit with zeroed buffers.
5. **Waveform fill**: 576-sample window mapped to [0..255].
6. **Spectrum fill**: FFT 1024 samples → 576 bins.
7. **Frame capture**: BitBlt to DIB, write PNGs.
8. **Hashes**: per-frame + rolling SHA-256.
9. **Manifest**: JSON with all metadata.
10. **AVI writer**: optional, uncompressed 32-bit frames.

---

## Acceptance checklist (Codex must verify after each PR)

* Builds on Windows (MSVC).
* Builds on Linux (MINGW).
* Subcommand compiles and runs with `--help`.
* Produces logs/errors instead of crashing.
* Deterministic outputs (PNG/Hash) identical on repeat runs.
