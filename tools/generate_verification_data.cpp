#include <algorithm>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <xmmintrin.h>

#include <windows.h>

#include "vis_host.hpp"
#include "wav_reader.hpp"

namespace {

constexpr wchar_t kParentWindowClassName[] = L"visdriver.avs.parent";
constexpr wchar_t kChildWindowClassName[] = L"visdriver.avs.child";

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

  if (options.runtime_dir.empty()) {
    const std::filesystem::path vis_path(options.vis_dll);
    const std::filesystem::path parent = vis_path.parent_path();
    options.runtime_dir = parent.empty() ? std::wstring(L".") : parent.wstring();
  }

  PrintSummary(options);

  std::vector<int16_t> audio_pcm;
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

  for (int frame = 0; frame < options.frames; ++frame) {
    for (int channel = 0; channel < 2; ++channel) {
      std::fill_n(host.mod->waveformData[channel], 576, 128);
      std::fill_n(host.mod->spectrumData[channel], 576, 0);
    }

    const int render_result = host.mod->Render(host.mod);
    if (render_result != 0) {
      break;
    }
  }

  end_vis(host);

  unload_vis(host);

  return 0;
}
