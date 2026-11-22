#include "process_probe.h"

#include <iterator>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace jpt {

#ifdef _WIN32
std::filesystem::path CurrentExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path);
}

bool OtherProcessRunning(const std::filesystem::path& executable) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const std::wstring name = executable.filename().wstring();
    const DWORD self = GetCurrentProcessId();
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self || _wcsicmp(entry.szExeFile, name.c_str()) != 0) continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t image[32768];
        DWORD length = static_cast<DWORD>(std::size(image));
        if (QueryFullProcessImageNameW(process, 0, image, &length)) {
            std::error_code ec;
            found = std::filesystem::equivalent(std::filesystem::path(std::wstring(image, length)), executable, ec);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}
#else
std::filesystem::path CurrentExecutablePath() {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : path;
}

bool OtherProcessRunning(const std::filesystem::path&) { return false; }
#endif

} // namespace jpt
