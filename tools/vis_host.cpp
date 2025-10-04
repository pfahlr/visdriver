#include "vis_host.hpp"

#include <iostream>

#include "windows_diagnostics.hpp"

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

  diagnostics::Logf(L"load_vis: loading %ls", dll_path.c_str());
  host.dll = LoadLibraryW(dll_path.c_str());
  if (host.dll == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to load vis DLL '" << dll_path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    diagnostics::Logf(L"load_vis: LoadLibraryW failed (%ls)",
                      FormatWindowsErrorMessage(error).c_str());
    return host;
  }

  FARPROC proc = GetProcAddress(host.dll, "winampVisGetHeader");
  if (proc == nullptr) {
    std::wcerr << L"ERROR: Symbol 'winampVisGetHeader' not found in '" << dll_path
               << L"'.\n";
    diagnostics::Logf(L"load_vis: winampVisGetHeader missing");
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  auto get_header = reinterpret_cast<winampVisGetHeaderType>(proc);
  winampVisHeader *const header = get_header();
  if (header == nullptr) {
    std::wcerr << L"ERROR: winampVisGetHeader returned null for '" << dll_path
               << L"'.\n";
    diagnostics::Logf(L"load_vis: header null");
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  if (header->getModule == nullptr) {
    std::wcerr << L"ERROR: winampVisHeader::getModule is null in '" << dll_path
               << L"'.\n";
    diagnostics::Logf(L"load_vis: getModule null");
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  winampVisModule *const module = header->getModule(0);
  if (module == nullptr) {
    std::wcerr << L"ERROR: Failed to fetch first vis module from '" << dll_path
               << L"'.\n";
    diagnostics::Logf(L"load_vis: getModule(0) returned null");
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  host.hdr = header;
  host.mod = module;
  diagnostics::Logf(L"load_vis: success module=%p", host.mod);

  return host;
}

void unload_vis(VisHost &host) {
  diagnostics::Logf(L"unload_vis: begin");
  if (host.child != nullptr) {
    diagnostics::Logf(L"unload_vis: destroying child %p", host.child);
    DestroyWindow(host.child);
    host.child = nullptr;
  }
  if (host.parent != nullptr) {
    diagnostics::Logf(L"unload_vis: destroying parent %p", host.parent);
    DestroyWindow(host.parent);
    host.parent = nullptr;
  }
  host.hdr = nullptr;
  host.mod = nullptr;
  if (host.dll != nullptr) {
    diagnostics::Logf(L"unload_vis: freeing dll %p", host.dll);
    FreeLibrary(host.dll);
    host.dll = nullptr;
  }
  diagnostics::Logf(L"unload_vis: done");
}

bool begin_vis(VisHost &host, int width, int height) {
  if (host.mod == nullptr) {
    std::wcerr << L"ERROR: Visualization module handle is null.\n";
    diagnostics::Logf(L"begin_vis: module null");
    return false;
  }

  HWND const target_window = (host.child != nullptr) ? host.child : host.parent;
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
    diagnostics::Logf(L"begin_vis: Init missing");
    return false;
  }

  diagnostics::Logf(L"begin_vis: calling Init (target=%p)", target_window);
  const int init_result = host.mod->Init(host.mod);
  if (init_result != 0) {
    std::wcerr << L"ERROR: Visualization module Init() returned "
               << init_result << L".\n";
    diagnostics::Logf(L"begin_vis: Init returned %d", init_result);
    return false;
  }

  diagnostics::Logf(L"begin_vis: Init succeeded");

  return true;
}

void end_vis(VisHost &host) {
  diagnostics::Logf(L"end_vis: begin");
  if (host.mod != nullptr && host.mod->Quit != nullptr) {
    host.mod->Quit(host.mod);
    diagnostics::Logf(L"end_vis: Quit called");
  }

  if (host.child != nullptr) {
    diagnostics::Logf(L"end_vis: destroying child %p", host.child);
    DestroyWindow(host.child);
    host.child = nullptr;
  }
  diagnostics::Logf(L"end_vis: done");
}
