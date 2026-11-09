#include <windows.h>
#include <filesystem>
#include <string>
namespace fs = std::filesystem;
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int) {
    wchar_t self[32768]{};
    if (!GetModuleFileNameW(nullptr, self, 32768)) return 1;
    const auto directory = fs::path(self).parent_path();
    const auto host = directory / L"odl-plugin-host.exe", gui = directory / L"OpenDllLoader.exe";
    const auto sample = directory / L"reloadable-plugin-v1.dll", active = directory / L"active-plugin.dll";
    if (!fs::is_regular_file(host) || !fs::is_regular_file(gui) || !fs::is_regular_file(sample)) {
        if (std::wstring(args) != L"--check") MessageBoxW(nullptr, L"Keep the launcher, host, sample DLLs, and loader together in the same directory.", L"Open DLL Loader demo", MB_ICONERROR);
        return 1;
    }
    if (std::wstring(args) == L"--check") return 0;
    try { fs::copy_file(sample, active, fs::copy_options::skip_existing); }
    catch (...) { MessageBoxW(nullptr, L"Cannot create active-plugin.dll. Put the demo in a writable directory.", L"Open DLL Loader demo", MB_ICONERROR); return 1; }
    auto launch = [&](const fs::path& exe, std::wstring command, DWORD flags) {
        STARTUPINFOW info{}; info.cb = sizeof(info); PROCESS_INFORMATION process{};
        const bool ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.c_str(), &info, &process) != 0;
        if (ok) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
        return ok;
    };
    // Both children inherit the exact same Wine/Proton environment and prefix.
    if (!launch(host, L"\"" + host.wstring() + L"\" --plugin \"" + active.wstring() + L"\" --watch", CREATE_NEW_CONSOLE)) return 1;
    if (!launch(gui, L"\"" + gui.wstring() + L"\"", 0)) {
        MessageBoxW(nullptr, L"The example host started, but the loader could not start. Close the host console when finished.", L"Open DLL Loader demo", MB_ICONERROR); return 1;
    }
    return 0;
}
