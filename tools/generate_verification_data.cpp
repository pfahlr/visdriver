#include <algorithm>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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
             << L"  --runtime-dir <dir>     Runtime directory (defaults to \"
             << L"directory of vis DLL\")\n"
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

  return 0;
}
