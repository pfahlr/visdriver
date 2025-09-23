#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <xmmintrin.h>

#include <windows.h>

#include "avi_writer.hpp"
#include "capture.hpp"
#include "manifest.hpp"
#include "spectrum.hpp"
#include "sha256.hpp"
#include "vis_host.hpp"
#include "wav_reader.hpp"

namespace {

constexpr wchar_t kParentWindowClassName[] = L"visdriver.avs.parent";
constexpr wchar_t kChildWindowClassName[] = L"visdriver.avs.child";
constexpr int kWaveformSamples = 576;
constexpr int kSpectrumFftSize = 1024;

int g_total_pcm_samples = 0;

struct HandleCloser {
  void operator()(HANDLE handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using ScopedHandle =
    std::unique_ptr<std::remove_pointer<HANDLE>::type, HandleCloser>;

bool WriteAllBytes(HANDLE handle, const std::string &text) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (text.empty()) {
    return true;
  }

  const char *data = text.data();
  size_t remaining = text.size();
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(handle, data, chunk, &written, nullptr)) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    data += written;
    remaining -= written;
  }
  return true;
}

std::wstring ConvertToWide(const std::string &text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.c_str(), -1, nullptr, 0);
  if (required <= 0) {
    return L"(conversion error)";
  }
  std::wstring result(static_cast<size_t>(required - 1), L'\0');
  if (!result.empty()) {
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1,
                        result.data(), required);
  }
  return result;
}

std::wstring FormatWindowsErrorMessage(DWORD error_code) {
  wchar_t *buffer = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD size =
      FormatMessageW(flags, nullptr, error_code,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
  std::wstring message;
  if (size != 0 && buffer != nullptr) {
    message.assign(buffer, size);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n')) {
      message.pop_back();
    }
  } else {
    message = L"Unknown error";
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return message;
}

bool EnsureDirectoryExists(const std::wstring &path) {
  if (path.empty()) {
    std::wcerr << L"ERROR: Directory path is empty.\n";
    return false;
  }

  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return true;
    }
    std::wcerr << L"ERROR: Path '" << path
               << L"' exists but is not a directory.\n";
    return false;
  }

  const DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    std::wcerr << L"ERROR: GetFileAttributesW failed for '" << path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    return false;
  }

  const std::filesystem::path dir_path(path);
  const std::filesystem::path parent = dir_path.parent_path();
  if (!parent.empty() && parent != dir_path) {
    if (!EnsureDirectoryExists(parent.wstring())) {
      return false;
    }
  }

  if (CreateDirectoryW(path.c_str(), nullptr)) {
    return true;
  }

  const DWORD create_error = GetLastError();
  if (create_error == ERROR_ALREADY_EXISTS) {
    return true;
  }

  std::wcerr << L"ERROR: Failed to create directory '" << path
             << L"': " << FormatWindowsErrorMessage(create_error) << L"\n";
  return false;
}

std::wstring FormatFrameFilename(int frame_index) {
  std::wstringstream stream;
  stream << L"frame" << std::setw(4) << std::setfill(L'0') << frame_index
         << L".png";
  return stream.str();
}

LRESULT CALLBACK DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterWindowClass(const wchar_t *class_name) {
  HINSTANCE instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = DummyWindowProc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  cls.lpszClassName = class_name;

  const ATOM atom = RegisterClassExW(&cls);
  if (atom != 0) {
    return true;
  }

  const DWORD error = GetLastError();
  if (error == ERROR_CLASS_ALREADY_EXISTS) {
    return true;
  }

  std::wcerr << L"ERROR: Failed to register window class '" << class_name
             << L"': " << FormatWindowsErrorMessage(error) << L"\n";
  return false;
}

uint8_t SampleToWaveformValue(int16_t sample) {
  const float scaled =
      (static_cast<float>(sample) / 32768.0f) * 127.0f + 128.0f;
  const float clamped = std::clamp(scaled, 0.0f, 255.0f);
  return static_cast<uint8_t>(clamped);
}

void fill_waveform(winampVisModule *mod, const int16_t *pcm, int start_sample,
                   int hop) {
  if (mod == nullptr) {
    return;
  }

  if (pcm == nullptr || g_total_pcm_samples <= 0 || hop <= 0) {
    for (int channel = 0; channel < 2; ++channel) {
      std::fill_n(mod->waveformData[channel], kWaveformSamples, 128);
    }
    return;
  }

  const int last_valid_sample = g_total_pcm_samples - 1;

  for (int channel = 0; channel < 2; ++channel) {
    for (int i = 0; i < kWaveformSamples; ++i) {
      int sample_index = start_sample + i * hop;
      if (sample_index > last_valid_sample) {
        sample_index = last_valid_sample;
      }
      if (sample_index < 0) {
        sample_index = 0;
      }

      const int16_t sample = pcm[sample_index * 2 + channel];
      mod->waveformData[channel][i] = SampleToWaveformValue(sample);
    }
  }
}

HWND CreateHiddenParentWindow(int width, int height) {
  if (!RegisterWindowClass(kParentWindowClassName)) {
    return nullptr;
  }

  HINSTANCE instance = GetModuleHandleW(nullptr);

  RECT rect{0, 0, width, height};
  if (!AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW)) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: AdjustWindowRectEx failed: "
               << FormatWindowsErrorMessage(error) << L"\n";
    return nullptr;
  }

  const int window_width = rect.right - rect.left;
  const int window_height = rect.bottom - rect.top;

  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kParentWindowClassName,
                              L"visdriver avs parent", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, window_width,
                              window_height, nullptr, nullptr, instance,
                              nullptr);
  if (hwnd == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to create parent window: "
               << FormatWindowsErrorMessage(error) << L"\n";
  }
  return hwnd;
}

HWND CreateChildWindow(HWND parent, int width, int height) {
  if (!RegisterWindowClass(kChildWindowClassName)) {
    return nullptr;
  }

  HINSTANCE instance = GetModuleHandleW(nullptr);
  HWND hwnd = CreateWindowExW(0, kChildWindowClassName, L"visdriver avs child",
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
                                  WS_VISIBLE,
                              0, 0, width, height, parent, nullptr, instance,
                              nullptr);
  if (hwnd == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to create child window: "
               << FormatWindowsErrorMessage(error) << L"\n";
  }
  return hwnd;
}

enum class HashMode {
  kPixels,
  kRolling,
};

struct Options {
  std::wstring vis_dll;
  std::wstring runtime_dir;
  std::wstring vis_avs_dat;
  std::wstring preset;
  std::wstring wav;
  int width = 640;
  int height = 480;
  int fps = 60;
  int frames = 121;
  std::wstring out_dir;
  std::wstring avi_out;
  int png_step = 1;
  HashMode hash_mode = HashMode::kPixels;
};

void PrintUsage(const std::wstring &command_name) {
  std::wcout << L"Usage: " << command_name
             << L" [OPTIONS]\n\n"
             << L"Options:\n"
             << L"  --vis-dll <path>        Path to vis DLL (required)\n"
             << L"  --runtime-dir <dir>     Runtime directory (defaults to directory of vis DLL\")\n"
             << L"  --vis-avs-dat <path>    Optional path to vis_avs.dat\n"
             << L"  --preset <path>         Optional path to preset file\n"
             << L"  --wav <path>            Path to WAV input (required)\n"
             << L"  --width <pixels>        Output width (default: 640)\n"
             << L"  --height <pixels>       Output height (default: 480)\n"
             << L"  --fps <value>           Frames per second (default: 60)\n"
             << L"  --frames <count>        Number of frames to render (default: 121)\n"
             << L"  --out-dir <dir>         Output directory (required)\n"
             << L"  --avi-out <filename>    Optional AVI output filename\n"
             << L"  --png-step <value>      Interval between PNG dumps (default: 1)\n"
             << L"  --hash-mode <mode>      Hashing mode: pixels|rolling (default: pixels)\n"
             << L"  --help                  Show this help message\n";
}

std::wstring ToLower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return value;
}

bool ParseIntegerOption(const std::wstring &option_name, const std::wstring &value,
                        int *target) {
  try {
    size_t index = 0;
    const int parsed = std::stoi(value, &index, 10);
    if (index != value.size()) {
      std::wcerr << L"ERROR: Option '" << option_name
                 << L"' expects an integer value.\n";
      return false;
    }
    *target = parsed;
    return true;
  } catch (const std::exception &) {
    std::wcerr << L"ERROR: Option '" << option_name
               << L"' expects an integer value.\n";
    return false;
  }
}

void PrintSummary(const Options &options) {
  std::wcout << L"generate-verification-data options:\n";
  std::wcout << L"  Visualizer DLL:        " << options.vis_dll << L"\n";
  std::wcout << L"  Runtime directory:     " << options.runtime_dir << L"\n";
  std::wcout << L"  vis_avs.dat:           "
             << (options.vis_avs_dat.empty() ? L"(not set)" : options.vis_avs_dat)
             << L"\n";
  std::wcout << L"  Preset:                "
             << (options.preset.empty() ? L"(not set)" : options.preset) << L"\n";
  std::wcout << L"  WAV input:             " << options.wav << L"\n";
  std::wcout << L"  Output size (WxH):     " << options.width << L"x" << options.height
             << L"\n";
  std::wcout << L"  Frame rate:            " << options.fps << L" fps\n";
  std::wcout << L"  Frame count:           " << options.frames << L"\n";
  std::wcout << L"  Output directory:      " << options.out_dir << L"\n";
  std::wcout << L"  AVI output filename:   "
             << (options.avi_out.empty() ? L"(not set)" : options.avi_out) << L"\n";
  std::wcout << L"  PNG step:              " << options.png_step << L"\n";
  std::wcout << L"  Hash mode:             "
             << (options.hash_mode == HashMode::kPixels ? L"pixels" : L"rolling")
             << L"\n";
}

} // namespace

extern "C" int cmd_generate_verification_data(int argc, wchar_t **argv) {
  const std::wstring command_name =
      (argc > 0 && argv != nullptr) ? argv[0] : L"generate-verification-data";

  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::wstring current = argv[i];
    if (current == L"--help" || current == L"-h") {
      PrintUsage(command_name);
      return 0;
    }

    auto require_value = [&](const std::wstring &option) -> std::wstring {
      if (i + 1 >= argc) {
        std::wcerr << L"ERROR: Option '" << option << L"' expects a value.\n";
        throw std::runtime_error("missing option value");
      }
      ++i;
      return argv[i];
    };

    try {
      if (current == L"--vis-dll") {
        options.vis_dll = require_value(current);
      } else if (current == L"--runtime-dir") {
        options.runtime_dir = require_value(current);
      } else if (current == L"--vis-avs-dat") {
        options.vis_avs_dat = require_value(current);
      } else if (current == L"--preset") {
        options.preset = require_value(current);
      } else if (current == L"--wav") {
        options.wav = require_value(current);
      } else if (current == L"--width") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.width)) {
          return 1;
        }
      } else if (current == L"--height") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.height)) {
          return 1;
        }
      } else if (current == L"--fps") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.fps)) {
          return 1;
        }
      } else if (current == L"--frames") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.frames)) {
          return 1;
        }
      } else if (current == L"--out-dir") {
        options.out_dir = require_value(current);
      } else if (current == L"--avi-out") {
        options.avi_out = require_value(current);
      } else if (current == L"--png-step") {
        const std::wstring value = require_value(current);
        if (!ParseIntegerOption(current, value, &options.png_step)) {
          return 1;
        }
      } else if (current == L"--hash-mode") {
        const std::wstring value = ToLower(require_value(current));
        if (value == L"pixels") {
          options.hash_mode = HashMode::kPixels;
        } else if (value == L"rolling") {
          options.hash_mode = HashMode::kRolling;
        } else {
          std::wcerr << L"ERROR: Option '--hash-mode' expects 'pixels' or 'rolling'.\n";
          return 1;
        }
      } else {
        std::wcerr << L"ERROR: Unknown option '" << current << L"'.\n";
        return 1;
      }
    } catch (const std::runtime_error &) {
      return 1;
    }
  }

  std::vector<std::wstring> missing;
  if (options.vis_dll.empty()) {
    missing.push_back(L"--vis-dll");
  }
  if (options.wav.empty()) {
    missing.push_back(L"--wav");
  }
  if (options.out_dir.empty()) {
    missing.push_back(L"--out-dir");
  }

  if (!missing.empty()) {
    for (const std::wstring &option : missing) {
      std::wcerr << L"ERROR: Required option '" << option << L"' is missing.\n";
    }
    std::wcerr << L"\n";
    PrintUsage(command_name);
    return 1;
  }

  if (options.png_step <= 0) {
    std::wcerr << L"ERROR: --png-step must be greater than zero.\n";
    return 1;
  }

  if (options.runtime_dir.empty()) {
    const std::filesystem::path vis_path(options.vis_dll);
    const std::filesystem::path parent = vis_path.parent_path();
    options.runtime_dir = parent.empty() ? std::wstring(L".") : parent.wstring();
  }

  PrintSummary(options);

  std::error_code path_error;
  const std::filesystem::path out_dir_path =
      std::filesystem::absolute(options.out_dir, path_error);
  if (path_error) {
    std::wcerr << L"ERROR: Failed to resolve output directory '" << options.out_dir
               << L"': "
               << ConvertToWide(path_error.message()) << L"\n";
    return 1;
  }
  const std::filesystem::path frames_dir_path = out_dir_path / L"frames";
  const std::filesystem::path hashes_dir_path = out_dir_path / L"hashes";

  if (!EnsureDirectoryExists(out_dir_path.wstring())) {
    return 1;
  }
  if (!EnsureDirectoryExists(frames_dir_path.wstring())) {
    return 1;
  }
  if (!EnsureDirectoryExists(hashes_dir_path.wstring())) {
    return 1;
  }

  const std::filesystem::path per_frame_csv_path =
      hashes_dir_path / L"per_frame.csv";
  const std::filesystem::path rolling_hash_path =
      hashes_dir_path / L"rolling_sha256.txt";

  ScopedHandle per_frame_file(CreateFileW(
      per_frame_csv_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!per_frame_file || per_frame_file.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to open per-frame hash file '"
               << per_frame_csv_path.wstring() << L"': "
               << FormatWindowsErrorMessage(error) << L"\n";
    return 1;
  }

  std::vector<int16_t> audio_pcm;
  int wav_sample_rate = 0;
  int wav_channels = 0;
  int64_t wav_sample_count = 0;
  try {
    WavData wav = load_wav_16le_stereo(options.wav);
    if (wav.channels != 2) {
      throw std::runtime_error("WAV file must have 2 channels");
    }

    if (wav.sample_rate != 44100) {
      audio_pcm = resample_linear_2ch_16bit(wav.pcm, wav.sample_rate, 44100);
    } else {
      audio_pcm = wav.pcm;
    }

    const size_t total_samples = audio_pcm.size() / 2;
    std::wcout << L"WAV ok: 44100 Hz, 2 ch, " << total_samples << L" samples\n";
    g_total_pcm_samples = static_cast<int>(total_samples);
    wav_sample_rate = 44100;
    wav_channels = 2;
    wav_sample_count = static_cast<int64_t>(total_samples);
  } catch (const std::exception &ex) {
    std::wcerr << L"ERROR: Failed to load WAV: "
               << ConvertToWide(std::string(ex.what())) << L"\n";
    return 1;
  }

  if (!SetCurrentDirectoryW(options.runtime_dir.c_str())) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to set current directory to '"
               << options.runtime_dir << L"': "
               << FormatWindowsErrorMessage(error) << L"\n";
    return 1;
  }

  HWND parent_window = CreateHiddenParentWindow(options.width, options.height);
  if (parent_window == nullptr) {
    return 1;
  }

  VisHost host = load_vis(options.vis_dll, parent_window);
  if (host.dll == nullptr || host.hdr == nullptr || host.mod == nullptr) {
    unload_vis(host);
    return 1;
  }

  host.child = CreateChildWindow(parent_window, options.width, options.height);
  if (host.child == nullptr) {
    unload_vis(host);
    return 1;
  }

  host.mod->delayMs = (options.fps > 0) ? (1000 / options.fps) : 0;
  if (!begin_vis(host, options.width, options.height)) {
    unload_vis(host);
    return 1;
  }

  if (host.mod->Render == nullptr) {
    std::wcerr << L"ERROR: Visualization module is missing Render().\n";
    end_vis(host);
    unload_vis(host);
    return 1;
  }

  std::wcout << L"loaded vis module\n";

#if defined(_MM_SET_FLUSH_ZERO_MODE) && defined(_MM_SET_DENORMALS_ZERO_MODE)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

  std::vector<int16_t> spectrum_window(static_cast<size_t>(kSpectrumFftSize) * 2,
                                      0);
  bool logged_spectrum_ok = false;

  std::vector<uint8_t> frame_rgba;
  std::vector<std::filesystem::path> png_outputs;
  bool capture_failed = false;
  bool hash_failed = false;
  bool avi_failed = false;
  const int effective_fps = (options.fps > 0) ? options.fps : 1;
  AviWriter avi_writer;
  const bool avi_enabled = !options.avi_out.empty();
  std::filesystem::path avi_output_path;
  if (avi_enabled) {
    avi_output_path = out_dir_path / options.avi_out;
  }
  bool avi_opened = false;
  if (avi_enabled && options.frames == 0) {
    if (!avi_writer.Open(avi_output_path.wstring(), options.width,
                         options.height, effective_fps)) {
      avi_failed = true;
    } else {
      avi_opened = true;
    }
  }
  Sha256 rolling_hash;

  for (int frame = 0; frame < options.frames; ++frame) {
    const int samples_per_frame =
        static_cast<int>(std::lround(44100.0 / effective_fps));
    const int hop = std::max(1, samples_per_frame / kWaveformSamples);
    const int start_sample = frame * samples_per_frame;

    fill_waveform(host.mod, audio_pcm.data(), start_sample, hop);

    std::fill(spectrum_window.begin(), spectrum_window.end(), 0);
    const int total_samples = g_total_pcm_samples;
    if (!audio_pcm.empty() && start_sample < total_samples) {
      const int available = total_samples - start_sample;
      const int samples_to_copy =
          std::min(kSpectrumFftSize, std::max(0, available));
      if (samples_to_copy > 0) {
        const int16_t *src =
            audio_pcm.data() + static_cast<size_t>(start_sample) * 2;
        std::copy_n(src, static_cast<size_t>(samples_to_copy) * 2,
                    spectrum_window.begin());
      }
    }

    fill_spectrum(host.mod, spectrum_window.data(), 0, kSpectrumFftSize);

    if (!logged_spectrum_ok) {
      std::wcout << L"spectrum ok\n";
      logged_spectrum_ok = true;
    }

    std::wcout << L"Frame " << (frame + 1) << L"/" << options.frames
               << L": start=" << start_sample << L", hop=" << hop << L"\n";

    const int render_result = host.mod->Render(host.mod);
    if (render_result != 0) {
      break;
    }

    if (!capture_child_to_rgba(host.child, options.width, options.height,
                               frame_rgba)) {
      std::wcerr << L"ERROR: Failed to capture frame " << frame << L".\n";
      capture_failed = true;
      break;
    }

    if (avi_enabled && !avi_failed) {
      if (!avi_opened) {
        if (!avi_writer.Open(avi_output_path.wstring(), options.width,
                             options.height, effective_fps)) {
          avi_failed = true;
          break;
        }
        avi_opened = true;
      }
      if (!avi_writer.WriteFrame(frame_rgba.data(), frame_rgba.size())) {
        avi_failed = true;
        break;
      }
    }

    const std::array<uint8_t, 32> frame_digest = Sha256Hash(frame_rgba);
    const std::string frame_hex = Sha256ToHex(frame_digest);
    const std::string csv_line = std::to_string(frame) + "," + frame_hex + "\n";
    if (!WriteAllBytes(per_frame_file.get(), csv_line)) {
      std::wcerr << L"ERROR: Failed to write per-frame hash for frame " << frame
                 << L".\n";
      hash_failed = true;
      break;
    }
    rolling_hash.Update(frame_rgba);

    if ((frame % options.png_step) == 0) {
      const std::filesystem::path png_path =
          frames_dir_path / FormatFrameFilename(frame);
      if (!write_png(png_path.wstring(), options.width, options.height,
                     frame_rgba.data())) {
        capture_failed = true;
        break;
      }
      png_outputs.push_back(png_path);
      std::wcout << L"Wrote " << png_path << L"\n";
    }
  }

  if (avi_writer.IsOpen()) {
    avi_writer.Close();
  }

  if (!capture_failed && !hash_failed && !avi_failed) {
    if (!FlushFileBuffers(per_frame_file.get())) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to flush per-frame hash file: "
                 << FormatWindowsErrorMessage(error) << L"\n";
      hash_failed = true;
    }
  }

  end_vis(host);

  unload_vis(host);

  if (!capture_failed && !hash_failed && !avi_failed) {
    const std::array<uint8_t, 32> rolling_digest = rolling_hash.Final();
    const std::string rolling_hex = Sha256ToHex(rolling_digest);
    const std::string rolling_line = rolling_hex + "\n";

    ScopedHandle rolling_file(CreateFileW(
        rolling_hash_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!rolling_file || rolling_file.get() == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to open rolling hash file '"
                 << rolling_hash_path.wstring() << L"': "
                 << FormatWindowsErrorMessage(error) << L"\n";
      hash_failed = true;
    } else {
      if (!WriteAllBytes(rolling_file.get(), rolling_line)) {
        std::wcerr << L"ERROR: Failed to write rolling hash.\n";
        hash_failed = true;
      } else if (!FlushFileBuffers(rolling_file.get())) {
        const DWORD error = GetLastError();
        std::wcerr << L"ERROR: Failed to flush rolling hash file: "
                   << FormatWindowsErrorMessage(error) << L"\n";
        hash_failed = true;
      }
    }
  }

  if (capture_failed || hash_failed || avi_failed) {
    return 1;
  }

  ManifestInfo manifest;
  manifest.out_dir = out_dir_path;
  manifest.vis_dll_path = options.vis_dll;
  manifest.runtime_dir = options.runtime_dir;
  manifest.has_vis_avs_dat = !options.vis_avs_dat.empty();
  if (manifest.has_vis_avs_dat) {
    manifest.vis_avs_dat_path = options.vis_avs_dat;
  }
  manifest.has_preset = !options.preset.empty();
  if (manifest.has_preset) {
    manifest.preset_path = options.preset;
  }
  manifest.wav_path = options.wav;
  manifest.wav_sample_rate = wav_sample_rate;
  manifest.wav_channels = wav_channels;
  manifest.wav_sample_count = wav_sample_count;
  manifest.width = options.width;
  manifest.height = options.height;
  manifest.fps = options.fps;
  manifest.frames = options.frames;
  manifest.png_paths = png_outputs;
  manifest.per_frame_hash_path = per_frame_csv_path;
  manifest.rolling_hash_path = rolling_hash_path;
  manifest.has_avi_output = avi_enabled && avi_opened && !avi_failed;
  if (manifest.has_avi_output) {
    manifest.avi_output_path = avi_output_path;
  }

  if (!WriteManifest(manifest)) {
    return 1;
  }

  return 0;
}
