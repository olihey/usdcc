#pragma once

// The usdcc and usdcc_node programs running from an install: installing an update stops
// them (Windows) or takes their files away (macOS), docs/UPDATES.md §9. Without Qt, for the
// launcher too. An AppImage's programs keep the old file until they exit.

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <csignal>
#include <libproc.h>
#include <unistd.h>
#endif

namespace usdcc::app {

// The process ids of the program `name` ("usdcc_node", without .exe) in `folder`, but for
// this process, whoever started them, as far as the system tells.
inline std::vector<unsigned long> runningPrograms(const std::filesystem::path& folder, const std::string& name) {
    std::vector<unsigned long> found;
    std::error_code error;
#if defined(_WIN32)
    const std::filesystem::path node = folder / (name + ".exe");
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        if (_wcsicmp(entry.szExeFile, node.filename().c_str()) != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            path.resize(length);
            if (std::filesystem::equivalent(path, node, error)) found.push_back(entry.th32ProcessID);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
#elif defined(__APPLE__)
    const std::filesystem::path node = folder / name;
    const int count = proc_listallpids(nullptr, 0);
    if (count <= 0) return found;
    std::vector<pid_t> pids(static_cast<size_t>(count) + 64);
    const int filled = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < filled; ++i) {
        if (pids[static_cast<size_t>(i)] == ::getpid()) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pids[static_cast<size_t>(i)], path, sizeof(path)) <= 0) continue;
        if (std::filesystem::path(path).filename() != name) continue;
        if (std::filesystem::equivalent(path, node, error)) found.push_back(static_cast<unsigned long>(pids[static_cast<size_t>(i)]));
    }
#else
    (void)folder;
    (void)name;
    (void)error;
#endif
    return found;
}

// On macOS, asks them to stop, as Ctrl+C does. Elsewhere nothing: on Windows Velopack's
// updater stops what runs from the install.
inline void stopNodes([[maybe_unused]] const std::vector<unsigned long>& nodes) {
#if defined(__APPLE__)
    for (const unsigned long node : nodes) ::kill(static_cast<pid_t>(node), SIGTERM);
#endif
}

}  // namespace usdcc::app
