#include "vis_host.hpp"

#include <iostream>

#include "diagnostics.hpp"

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

  host.dll = LoadLibraryW(dll_path.c_str());
  if (host.dll == nullptr) {
    const DWORD error = GetLastError();
    std::wcerr << L"ERROR: Failed to load vis DLL '" << dll_path
               << L"': " << FormatWindowsErrorMessage(error) << L"\n";
    diagnostics::Log(L"load_vis: LoadLibraryW failed for '%ls' (error=%lu)",
                     dll_path.c_str(), error);
    return host;
  }
  diagnostics::Log(L"load_vis: loaded '%ls' at %p", dll_path.c_str(), host.dll);

  if (!diagnostics::InstallHooksForModule(host.dll, dll_path.c_str())) {
    diagnostics::Log(L"load_vis: failed to hook module '%ls'", dll_path.c_str());
  }

  FARPROC proc = GetProcAddress(host.dll, "winampVisGetHeader");
  if (proc == nullptr) {
    std::wcerr << L"ERROR: Symbol 'winampVisGetHeader' not found in '" << dll_path
               << L"'.\n";
    diagnostics::Log(L"load_vis: winampVisGetHeader not found in '%ls'",
                     dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  auto get_header = reinterpret_cast<winampVisGetHeaderType>(proc);
  winampVisHeader *const header = get_header();
  if (header == nullptr) {
    std::wcerr << L"ERROR: winampVisGetHeader returned null for '" << dll_path
               << L"'.\n";
    diagnostics::Log(L"load_vis: winampVisGetHeader returned null for '%ls'",
                     dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  if (header->getModule == nullptr) {
    std::wcerr << L"ERROR: winampVisHeader::getModule is null in '" << dll_path
               << L"'.\n";
    diagnostics::Log(
        L"load_vis: header->getModule null for '%ls'",
        dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  winampVisModule *const module = header->getModule(0);
  if (module == nullptr) {
    std::wcerr << L"ERROR: Failed to fetch first vis module from '" << dll_path
               << L"'.\n";
    diagnostics::Log(L"load_vis: getModule(0) returned null for '%ls'",
                     dll_path.c_str());
    FreeLibrary(host.dll);
    host.dll = nullptr;
    return host;
  }

  host.hdr = header;
  host.mod = module;

  diagnostics::Log(L"load_vis: module description='%hs'",
                   host.mod->description != nullptr ? host.mod->description
                                                     : "(null)");

  return host;
}

void unload_vis(VisHost &host) {
  if (host.child != nullptr) {
    diagnostics::Log(L"unload_vis: DestroyWindow child=%p", host.child);
    DestroyWindow(host.child);
    host.child = nullptr;
  }
  if (host.parent != nullptr) {
    diagnostics::Log(L"unload_vis: DestroyWindow parent=%p", host.parent);
    DestroyWindow(host.parent);
    host.parent = nullptr;
  }
  host.hdr = nullptr;
  host.mod = nullptr;
  if (host.dll != nullptr) {
    diagnostics::Log(L"unload_vis: FreeLibrary %p", host.dll);
    FreeLibrary(host.dll);
    host.dll = nullptr;
  }
}

bool begin_vis(VisHost &host, int width, int height) {
  if (host.mod == nullptr) {
    std::wcerr << L"ERROR: Visualization module handle is null.\n";
    diagnostics::Log(L"begin_vis: module handle null");
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
    diagnostics::Log(L"begin_vis: configured target_window=%p size=%dx%d",
                     target_window, width, height);
  }

  if (host.mod->Init == nullptr) {
    std::wcerr << L"ERROR: Visualization module is missing Init().\n";
    diagnostics::Log(L"begin_vis: module missing Init");
    return false;
  }

  diagnostics::Log(L"begin_vis: calling Init on module=%p", host.mod);
  const int init_result = host.mod->Init(host.mod);
  if (init_result != 0) {
    std::wcerr << L"ERROR: Visualization module Init() returned "
               << init_result << L".\n";
    diagnostics::Log(L"begin_vis: Init returned %d", init_result);
    return false;
  }

  diagnostics::Log(L"begin_vis: Init succeeded");
  return true;
}

void end_vis(VisHost &host) {
  if (host.mod != nullptr && host.mod->Quit != nullptr) {
    diagnostics::Log(L"end_vis: calling Quit on module=%p", host.mod);
    host.mod->Quit(host.mod);
  }

  if (host.child != nullptr) {
    diagnostics::Log(L"end_vis: DestroyWindow child=%p", host.child);
    DestroyWindow(host.child);
    host.child = nullptr;
  }
}
