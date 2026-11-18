// usdcc.exe at the top of a deploy: what the installer's shortcuts start, and
// what Velopack runs for its install and update hooks (docs/UPDATES.md §3).
//
// Velopack wants the program it starts at the top of the package, and usdcc's
// is bin\usdcc.exe, beside its DLLs. So this small one goes on top: it lets
// Velopack do its startup work first (run a hook and exit, or apply an update
// downloaded earlier and restart), then starts bin\usdcc.exe with the same
// arguments and exits. In the 7z, which isn't installed, Velopack finds no
// install and does nothing, so it is just a launcher there.
//
// Win32 only, no Qt: it has to start fast and run before the app's DLLs load.

#include <windows.h>

#include <string>

#include "Velopack.hpp"

namespace {

// Set by the hooks below, while VelopackApp runs.
bool g_restartedAfterUpdate = false;

void onRestarted(void*, const char*) { g_restartedAfterUpdate = true; }

// This program's folder, with a trailing backslash.
std::wstring ownFolder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L"\\/") + 1);
}

// The command line after this program's own name, as Windows passed it, so the
// arguments reach the app exactly as they were given (quotes and all).
const wchar_t* argumentsAfterProgram(const wchar_t* commandLine) {
    const wchar_t* at = commandLine;
    if (*at == L'"') {
        ++at;
        while (*at && *at != L'"') ++at;
        if (*at == L'"') ++at;
    } else {
        while (*at && *at != L' ' && *at != L'\t') ++at;
    }
    while (*at == L' ' || *at == L'\t') ++at;
    return at;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // First, as Velopack asks: with one of its hook arguments it runs the hook
    // and exits here; with an update downloaded and not yet applied, it applies
    // it and starts this program again.
    try {
        Velopack::VelopackApp::Build().OnRestarted(&onRestarted).Run();
    } catch (...) {
        // Never in the way of starting the app.
    }

    const std::wstring app = ownFolder() + L"bin\\usdcc.exe";
    if (GetFileAttributesW(app.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring message = L"usdcc can't start: " + app + L" is missing.\n\nInstall usdcc again.";
        MessageBoxW(nullptr, message.c_str(), L"usdcc", MB_OK | MB_ICONERROR);
        return 1;
    }

    // The app shows "Updated to <version>" when this is set (docs/UPDATES.md §4).
    if (g_restartedAfterUpdate) SetEnvironmentVariableW(L"USDCC_UPDATED", L"1");

    std::wstring commandLine = L"\"" + app + L"\"";
    if (const wchar_t* arguments = argumentsAfterProgram(GetCommandLineW()); *arguments) {
        commandLine += L" ";
        commandLine += arguments;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(app.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        const std::wstring message = L"usdcc can't start " + app + L" (error " + std::to_wstring(GetLastError()) + L").";
        MessageBoxW(nullptr, message.c_str(), L"usdcc", MB_OK | MB_ICONERROR);
        return 1;
    }
    // This process was started by the user, so it may hand the foreground on.
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
