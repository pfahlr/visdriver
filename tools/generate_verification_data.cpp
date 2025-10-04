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

#include <cstring>

#include <winamp/out.h>
#include <winamp/wa_ipc.h>

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

HWND g_parent_window_handle = nullptr;
HWND g_child_window_handle = nullptr;
HWND g_vis_window_handle = nullptr;

HWND GetEmbedTargetWindow() {
  if (g_child_window_handle != nullptr) {
    return g_child_window_handle;
  }
  return g_parent_window_handle;
}

void ResizeEmbeddedWindow(HWND embedded, HWND container) {
  if (embedded == nullptr || container == nullptr) {
    return;
  }

  RECT rect{};
  if (!GetClientRect(container, &rect)) {
    return;
  }

  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  SetWindowPos(embedded, nullptr, 0, 0, width, height,
               SWP_NOACTIVATE | SWP_NOZORDER);
}

HWND CALLBACK EmbedWindow(embedWindowState *state) {
  HWND const target = GetEmbedTargetWindow();
  if (state != nullptr) {
    state->me = target;
  }
  return target;
}

LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  switch (msg) {
  case WM_DESTROY:
    if (hwnd == g_child_window_handle) {
      g_child_window_handle = nullptr;
      g_vis_window_handle = nullptr;
    }
    if (hwnd == g_parent_window_handle) {
      g_parent_window_handle = nullptr;
    }
    break;

  case WM_SIZE:
    if (hwnd == g_child_window_handle && g_vis_window_handle != nullptr) {
      ResizeEmbeddedWindow(g_vis_window_handle, g_child_window_handle);
    } else if (hwnd == g_parent_window_handle &&
               g_child_window_handle == nullptr &&
               g_vis_window_handle != nullptr) {
      ResizeEmbeddedWindow(g_vis_window_handle, g_parent_window_handle);
    }
    break;

  case WM_WA_IPC:
    switch (lParam) {
    case IPC_GETVERSION:
      return 0x2900;

    case IPC_ISPLAYING:
      return 1;

    case IPC_GETSKIN:
      if (wParam != 0) {
        std::strcpy(reinterpret_cast<char *>(wParam), "/tmp");
        return static_cast<LRESULT>(wParam);
      }
      return 0;

    case IPC_GETINIFILE: {
      static char ini_path[1024] = "";
      if (ini_path[0] == '\0') {
        GetModuleFileNameA(nullptr, ini_path,
                           static_cast<DWORD>(sizeof(ini_path)));
        char *dot_position = std::strrchr(ini_path, '.');
        if (dot_position == nullptr) {
          ini_path[0] = '\0';
        } else {
          std::strcpy(dot_position, ".ini");
        }
      }
      return reinterpret_cast<LRESULT>(ini_path);
    }

    case IPC_GET_EMBEDIF: {
      HWND const container = GetEmbedTargetWindow();
      if (container != nullptr) {
        ShowWindow(container, SW_SHOW);
      }
      if (wParam == 0) {
        return reinterpret_cast<LRESULT>(&EmbedWindow);
      }
      return reinterpret_cast<LRESULT>(
          EmbedWindow(reinterpret_cast<embedWindowState *>(wParam)));
    }

    case IPC_SETVISWND: {
      g_vis_window_handle = reinterpret_cast<HWND>(wParam);
      HWND const container = GetEmbedTargetWindow();
      if (g_vis_window_handle != nullptr && container != nullptr) {
        SetParent(g_vis_window_handle, container);
        ResizeEmbeddedWindow(g_vis_window_handle, container);
      }
      break;
    }
    }
    break;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

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

bool ReadAllBytes(const std::wstring &path, std::vector<uint8_t> &output) {
  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file || file.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to open '" << path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size)) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to query size of '" << path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    return false;
  }

  if (size.QuadPart < 0) {
    std::wcerr << L"ERROR: File size overflow for '" << path << L"'.\n";
    return false;
  }

  if (static_cast<uint64_t>(size.QuadPart) >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    std::wcerr << L"ERROR: File '" << path << L"' is too large to load.\n";
    return false;
  }

  const size_t total_size = static_cast<size_t>(size.QuadPart);
  output.clear();
  output.resize(total_size);
  uint8_t *dest = output.data();
  size_t remaining = total_size;
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD read = 0;
    if (!ReadFile(file.get(), dest, chunk, &read, nullptr)) {
      const DWORD error = GetLastError();
      std::wcerr << L"ERROR: Failed to read from '" << path
                 << L"': " << FormatWindowsErrorMessage(error) << L"\n";
      return false;
    }
    if (read == 0) {
      std::wcerr << L"ERROR: Unexpected end of file while reading '" << path
                 << L"'.\n";
      return false;
    }
    dest += read;
    remaining -= read;
  }

  return true;
}

struct LoadedOutputModule {
  HMODULE dll = nullptr;
  Out_Module *module = nullptr;
};

bool LogDataFileInfo(const std::wstring &label, const std::wstring &path) {
  std::vector<uint8_t> bytes;
  if (!ReadAllBytes(path, bytes)) {
    return false;
  }

  std::error_code ec;
  const std::filesystem::path absolute_path =
      std::filesystem::absolute(path, ec);
  const std::wstring display_path =
      ec ? path : absolute_path.wstring();

  const std::array<uint8_t, 32> digest = Sha256Hash(bytes);
  const std::wstring hex = ConvertToWide(Sha256ToHex(digest));
  std::wcout << label << L": " << display_path << L" ("
             << static_cast<unsigned long long>(bytes.size())
             << L" bytes, sha256=" << hex << L")\n";
  return true;
}

LoadedOutputModule LoadOutputModule(const std::wstring &path) {
  LoadedOutputModule info;
  info.dll = LoadLibraryW(path.c_str());
  if (info.dll == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to load output DLL '" << path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    return info;
  }

  using GetOutModuleProc = Out_Module *(__cdecl *)();
  FARPROC proc = GetProcAddress(info.dll, "winampGetOutModule");
  if (proc == nullptr) {
    std::wcerr << L"ERROR: Symbol 'winampGetOutModule' not found in '" << path
               << L"'.\n";
    FreeLibrary(info.dll);
    info.dll = nullptr;
    return info;
  }

  auto get_module = reinterpret_cast<GetOutModuleProc>(proc);
  info.module = get_module();
  if (info.module == nullptr) {
    std::wcerr << L"ERROR: winampGetOutModule() returned null for '" << path
               << L"'.\n";
    FreeLibrary(info.dll);
    info.dll = nullptr;
  }

  return info;
}

void UnloadOutputModule(LoadedOutputModule &module) {
  if (module.dll != nullptr) {
    FreeLibrary(module.dll);
    module.dll = nullptr;
    module.module = nullptr;
  }
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

bool RegisterWindowClass(const wchar_t *class_name, WNDPROC proc) {
  HINSTANCE instance = GetModuleHandleW(nullptr);

  UnregisterClassW(class_name, instance);

  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = proc;
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
  if (!RegisterWindowClass(kParentWindowClassName, HostWindowProc)) {
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
  g_parent_window_handle = hwnd;
  if (hwnd != nullptr) {
    g_vis_window_handle = nullptr;
  }
  return hwnd;
}

HWND CreateChildWindow(HWND parent, int width, int height) {
  if (!RegisterWindowClass(kChildWindowClassName, HostWindowProc)) {
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
  g_child_window_handle = hwnd;
  if (hwnd != nullptr) {
    g_vis_window_handle = nullptr;
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
  std::wstring out_dll;
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
  struct OptionHelp {
    const wchar_t *flag;
    const wchar_t *description;
  };

  const std::array<OptionHelp, 15> kOptions = {
      OptionHelp{L"--vis-dll <path>", L"Path to vis DLL (required)"},
      OptionHelp{L"--runtime-dir <dir>",
                 L"Runtime directory (default: directory of vis DLL)"},
      OptionHelp{L"--vis-avs-dat <path>", L"Optional path to vis_avs.dat"},
      OptionHelp{L"--preset <path>", L"Optional path to preset file"},
      OptionHelp{L"--out-dll <path>",
                 L"Optional path to output module DLL (e.g. out_wave.dll)"},
      OptionHelp{L"--wav <path>", L"Path to WAV input (required)"},
      OptionHelp{L"--width <pixels>", L"Output width (default: 640)"},
      OptionHelp{L"--height <pixels>", L"Output height (default: 480)"},
      OptionHelp{L"--fps <value>", L"Frames per second (default: 60)"},
      OptionHelp{L"--frames <count>",
                 L"Number of frames to render (default: 121)"},
      OptionHelp{L"--out-dir <dir>", L"Output directory (required)"},
      OptionHelp{L"--avi-out <filename>", L"Optional AVI output filename"},
      OptionHelp{L"--png-step <value>",
                 L"Interval between PNG dumps (default: 1)"},
      OptionHelp{L"--hash-mode <mode>",
                 L"Hashing mode: pixels|rolling (default: pixels)"},
      OptionHelp{L"--help, -h", L"Show this help message"},
  };

  size_t max_flag_width = 0;
  for (const OptionHelp &option : kOptions) {
    max_flag_width =
        std::max(max_flag_width, static_cast<size_t>(std::wcslen(option.flag)));
  }

  std::wcout << L"Usage: " << command_name << L" [OPTIONS]\n\n";
  std::wcout << L"Options:\n";
  for (const OptionHelp &option : kOptions) {
    const size_t flag_width = static_cast<size_t>(std::wcslen(option.flag));
    std::wcout << L"  " << option.flag;
    if (flag_width < max_flag_width) {
      std::wcout << std::wstring(max_flag_width - flag_width + 2, L' ');
    } else {
      std::wcout << L"  ";
    }
    std::wcout << option.description << L"\n";
  }
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
  std::wcout << L"  Output DLL:            "
             << (options.out_dll.empty() ? L"(not set)" : options.out_dll)
             << L"\n";
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
      } else if (current == L"--out-dll") {
        options.out_dll = require_value(current);
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

  {
    std::error_code cwd_error;
    const std::filesystem::path cwd =
        std::filesystem::current_path(cwd_error);
    if (!cwd_error) {
      std::wcout << L"Runtime directory ready: " << cwd.wstring() << L"\n";
    } else {
      std::wcout << L"Runtime directory ready: " << options.runtime_dir << L"\n";
    }
  }

  if (!options.out_dll.empty()) {
    std::error_code out_ec;
    const std::filesystem::path out_abs =
        std::filesystem::absolute(options.out_dll, out_ec);
    const std::wstring out_path_to_load =
        out_ec ? options.out_dll : out_abs.wstring();
    std::wcout << L"Loading output DLL: " << out_path_to_load << L"\n";
    LoadedOutputModule output_module = LoadOutputModule(out_path_to_load);
    if (output_module.dll == nullptr || output_module.module == nullptr) {
      return 1;
    }
    const char *desc_ptr =
        (output_module.module->description != nullptr)
            ? output_module.module->description
            : "(no description)";
    std::wstring description = ConvertToWide(std::string(desc_ptr));
    std::wstringstream version_stream;
    version_stream << std::uppercase << std::hex
                   << output_module.module->version;
    std::wcout << L"Output module: " << description << L" (API 0x"
               << version_stream.str() << L")\n";
    UnloadOutputModule(output_module);
  }

  if (!options.vis_avs_dat.empty()) {
    if (!LogDataFileInfo(L"vis_avs.dat", options.vis_avs_dat)) {
      return 1;
    }
  }

  if (!options.preset.empty()) {
    if (!LogDataFileInfo(L"Preset", options.preset)) {
      return 1;
    }
  }

  HWND parent_window = CreateHiddenParentWindow(options.width, options.height);
  if (parent_window == nullptr) {
    return 1;
  }

  std::error_code vis_ec;
  const std::filesystem::path vis_abs =
      std::filesystem::absolute(options.vis_dll, vis_ec);
  const std::wstring vis_path_to_load =
      vis_ec ? options.vis_dll : vis_abs.wstring();
  std::wcout << L"Loading vis DLL: " << vis_path_to_load << L"\n";

  VisHost host = load_vis(vis_path_to_load, parent_window);
  if (host.dll == nullptr || host.hdr == nullptr || host.mod == nullptr) {
    unload_vis(host);
    return 1;
  }

  host.child = CreateChildWindow(parent_window, options.width, options.height);
  if (host.child == nullptr) {
    unload_vis(host);
    return 1;
  }

  const char *header_desc =
      (host.hdr != nullptr && host.hdr->description != nullptr)
          ? host.hdr->description
          : "(no description)";
  std::wcout << L"Vis header: " << ConvertToWide(std::string(header_desc))
             << L" (API 0x";
  {
    std::wstringstream version_stream;
    version_stream << std::uppercase << std::hex
                   << (host.hdr != nullptr ? host.hdr->version : 0);
    std::wcout << version_stream.str();
  }
  std::wcout << L")\n";

  if (host.mod != nullptr) {
    const char *module_desc =
        (host.mod->description != nullptr) ? host.mod->description
                                           : "(no description)";
    std::wstringstream mod_version_stream;
    mod_version_stream << std::uppercase << std::hex << host.mod->version;
    std::wcout << L"Vis module: " << ConvertToWide(std::string(module_desc))
               << L" (API 0x" << mod_version_stream.str() << L")\n";
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
