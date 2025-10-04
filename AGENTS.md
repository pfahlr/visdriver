
# AGENTS.md

## Setup

* Build with **CMake** and **MinGW**.
* In Ubuntu the packages `build-essential`, `cmake`, `ccache`, `mingw-w64`, and `wine` are needed (same as `.ci/ubuntu-packages.txt`).
  Install them with:

  ```bash
  sudo apt-get update
  sudo apt-get install --yes build-essential cmake ccache mingw-w64 wine
  ```
* Dependencies are vendored header-only or tiny C libs (no big external deps).

### Build 

see `.github/workflows/linux-mingw.yml` for complete build instructions.

the shorter version is as follows, but the above is the best source of truth:

run 
```
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
make -C build -j$(nproc) VERBOSE=1
```

to build in a ubuntu environment with the following packages

```
build-essential
cmake
ccache
mingw-w64
wine
```

copy the necessary dlls into the directory with the executable 
```
cp for_codex/dll/* build/
cp -R for_codex/tests build/
```

### Run


visdriver.exe generate-verification-data --help
Options:
  --vis-dll <path>      Path to vis DLL (required)
  --runtime-dir <dir>   Runtime directory (default: directory of vis DLL)
  --vis-avs-dat <path>  Optional path to vis_avs.dat
  --out-dll <path>      Optional output plug-in DLL
  --preset <path>       Optional path to preset file
  --wav <path>          Path to WAV input (required)
  --width <pixels>      Output width (default: 640)
  --height <pixels>     Output height (default: 480)
  --fps <value>         Frames per second (default: 60)
  --frames <count>      Number of frames to render (default: 121)
  --out-dir <dir>       Output directory (required)
  --avi-out <filename>  Optional AVI output filename
  --png-step <value>    Interval between PNG dumps (default: 1)
  --hash-mode <mode>    Hashing mode: pixels|rolling (default: pixels)
  --help, -h            Show this help message


```
wine visdriver.exe generate-verification-data --vis-dll .\vis_avs.dll  --vis-avs-dat .\vis_avs.dat --runtime-dir .\  --wav .\tests\data\test.wav --preset  .\tests\data\phase1\simple.avs --out-dir .\tests\golden\phase1\simple
```


  
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
* optional: `*.avi` (uncompressed 32-bit BI\_RGB)

---

## Development phases

Codex must work incrementally. Each PR implements **one phase** only.

1. **CLI skeleton**: parse args, print summary.
2. **vis\_avs.dll load**: load DLL, create parent/child window.
3. **WAV reader**: parse/resample 16-bit stereo PCM.
4. **Dry render loop**: call Init/Render/Quit with zeroed buffers.
5. **Waveform fill**: 576-sample window mapped to \[0..255].
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

