#include "vis_host.hpp"

#include <iostream>

#include "debug_trace.hpp"

namespace {

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

} // namespace

VisHost load_vis(const std::wstring &dll_path, HWND parent) {
  VisHost host;
  host.parent = parent;

  DebugTraceLog(L"load_vis: attempting to load %s", dll_path.c_str());
  host.dll = LoadLibraryW(dll_path.c_str());
  if (host.dll == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to load vis DLL '" << dll_path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    DebugTraceLog(L"load_vis: LoadLibraryW failed for %s error=%lu", dll_path.c_str(),
                  error);
    return host;
  }

  DebugTraceLog(L"load_vis: loaded %s module=%p", dll_path.c_str(), host.dll);

  FARPROC proc = GetProcAddress(host.dll, "winampVisGetHeader");
  if (proc == nullptr) {
    std::wcerr << L"ERROR: Symbol 'winampVisGetHeader' not found in '" << dll_path
               << L"'.\n";
    DebugTraceLog(L"load_vis: winampVisGetHeader missing in %s", dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  auto get_header = reinterpret_cast<winampVisGetHeaderType>(proc);
  winampVisHeader *const header = get_header();
  if (header == nullptr) {
    std::wcerr << L"ERROR: winampVisGetHeader returned null for '" << dll_path
               << L"'.\n";
    DebugTraceLog(L"load_vis: winampVisGetHeader returned null for %s",
                  dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  if (header->getModule == nullptr) {
    std::wcerr << L"ERROR: winampVisHeader::getModule is null in '" << dll_path
               << L"'.\n";
    DebugTraceLog(L"load_vis: getModule is null for %s", dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  winampVisModule *const module = header->getModule(0);
  if (module == nullptr) {
    std::wcerr << L"ERROR: Failed to fetch first vis module from '" << dll_path
               << L"'.\n";
    DebugTraceLog(L"load_vis: getModule returned null for %s", dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  host.hdr = header;
  host.mod = module;

  DebugTraceLog(L"load_vis: module acquired header=%p module=%p", host.hdr,
                host.mod);

  return host;
}

void unload_vis(VisHost &host) {
  DebugTraceLog(L"unload_vis: module=%p child=%p parent=%p", host.dll, host.child,
                host.parent);
  if (host.child != nullptr) {
    DestroyWindow(host.child);
    host.child = nullptr;
  }
  if (host.parent != nullptr) {
    DestroyWindow(host.parent);
    host.parent = nullptr;
  }
  host.hdr = nullptr;
  host.mod = nullptr;
  if (host.dll != nullptr) {
    FreeLibrary(host.dll);
    DebugTraceLog(L"unload_vis: freed module handle=%p", host.dll);
    host.dll = nullptr;
  }
}

bool begin_vis(VisHost &host, int width, int height) {
  if (host.mod == nullptr) {
    std::wcerr << L"ERROR: Visualization module handle is null.\n";
    DebugTraceLog(L"begin_vis: module handle null");
    return false;
  }

  HWND const target_window = (host.child != nullptr) ? host.child : host.parent;
  DebugTraceLog(L"begin_vis: target=%p width=%d height=%d", target_window, width,
                height);
  host.mod->hwndParent = target_window;
  host.mod->hDllInstance = host.dll;
  host.mod->sRate = 44100;
  host.mod->nCh = 2;
  host.mod->latencyMs = 0;
  host.mod->spectrumNch = 2;
  host.mod->waveformNch = 2;

  if (target_window != nullptr) {
    SetWindowPos(target_window, nullptr, 0, 0, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
  }

  if (host.mod->Init == nullptr) {
    std::wcerr << L"ERROR: Visualization module is missing Init().\n";
    DebugTraceLog(L"begin_vis: Init missing for module=%p", host.mod);
    return false;
  }

  const int init_result = host.mod->Init(host.mod);
  if (init_result != 0) {
    std::wcerr << L"ERROR: Visualization module Init() returned "
               << init_result << L".\n";
    DebugTraceLog(L"begin_vis: Init returned %d", init_result);
    return false;
  }

  DebugTraceLog(L"begin_vis: Init success");
  return true;
}

void end_vis(VisHost &host) {
  if (host.mod != nullptr && host.mod->Quit != nullptr) {
    DebugTraceLog(L"end_vis: calling Quit on module=%p", host.mod);
    host.mod->Quit(host.mod);
  }

  if (host.child != nullptr) {
    DebugTraceLog(L"end_vis: destroying child window=%p", host.child);
    DestroyWindow(host.child);
    host.child = nullptr;
  }
}
