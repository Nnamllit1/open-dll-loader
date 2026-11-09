#include "loader.h"
#include "managed_host.h"
#include <filesystem>
#include <iostream>
#include <functional>

namespace {
int test_host(const wchar_t* readyName, const wchar_t* stopName) {
    odl::Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName));
    odl::Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stopName));
    if (!ready || !stop) return 2;
    SetEvent(ready.get());
    return WaitForSingleObject(stop.get(), 60000) == WAIT_OBJECT_0 ? 0 : 3;
}
int self_test(const std::wstring& plugin) {
    unsigned passed{};
    auto require = [&](bool condition, const wchar_t* label) {
        if (!condition) throw std::wstring(L"FAIL: ") + label;
        std::wcout << L"PASS: " << label << L'\n'; ++passed;
    };
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr, exe, 32768)) throw std::wstring(L"Cannot resolve self-test executable.");
    const auto stem = L"Local\\OpenDllLoader.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const auto readyName = stem + L".Ready", stopName = stem + L".Stop";
    odl::Handle ready(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    odl::Handle stop(CreateEventW(nullptr, TRUE, FALSE, stopName.c_str()));
    require(ready && stop, L"create test synchronization events");
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --test-host \"" + readyName + L"\" \"" + stopName + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    require(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi) != 0, L"launch isolated test child");
    odl::Handle child(pi.hProcess), thread(pi.hThread);
    struct StopChild {
        HANDLE stop, child;
        ~StopChild() { SetEvent(stop); WaitForSingleObject(child, 5000); }
    } cleanup{stop.get(), child.get()};
    require(WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0, L"test child ready");
    auto target = odl::process(pi.dwProcessId);
    const auto eventName = L"Local\\OpenDllLoader.Plugin." + std::to_wstring(pi.dwProcessId);
    odl::Handle attached(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
    require(static_cast<bool>(attached), L"create plugin acknowledgment event");
    auto info = odl::inspect(plugin);
    require(info.machine == odl::own_machine() && info.sha256.size() == 64, L"inspect native DLL and SHA-256");
    require(!info.imports.empty(), L"read static PE imports");
    require(odl::diagnose(target, plugin).find(L"No DLL code was executed") != std::wstring::npos, L"produce read-only dependency and policy report");
    require(WaitForSingleObject(attached.get(), 0) == WAIT_TIMEOUT, L"inspection does not execute DLL code");
    require(odl::load(target, plugin, true).ok(), L"dry run validates target and DLL");
    require(WaitForSingleObject(attached.get(), 0) == WAIT_TIMEOUT, L"dry run does not execute plugin");
    auto stale = target; ++stale.created;
    require(!odl::load(stale, plugin).ok(), L"reject stale process identity");
    auto wrongArch = target;
    wrongArch.machine = odl::own_machine() == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    require(!odl::load(wrongArch, plugin).ok(), L"reject architecture mismatch");
    require(!odl::load(target, plugin + L".missing").ok(), L"reject missing DLL");
    require(!odl::load(target, exe).ok(), L"reject executable input");
    auto result = odl::load(target, plugin);
    if (!result.ok()) std::wcerr << result.message << L'\n';
    require(result.state == odl::State::loaded, L"load into isolated child and verify module path");
    require(WaitForSingleObject(attached.get(), 1000) == WAIT_OBJECT_0, L"plugin DllMain acknowledges execution");
    require(odl::load(target, plugin).state == odl::State::already_loaded, L"repeat load is skipped");
    SetEvent(stop.get());
    require(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0, L"test child exits normally");
    require(!odl::load(target, plugin).ok(), L"reject exited target");
    std::wcout << passed << L" checks passed (" << odl::machine_name(odl::own_machine()) << L").\n";
    return 0;
}
DWORD parse_pid(const wchar_t* text) {
    if (!*text || *text == L'-') throw std::wstring(L"Invalid PID.");
    wchar_t* end{};
    unsigned long long n = wcstoull(text, &end, 10);
    if (*end || n <= 4 || n > MAXDWORD) throw std::wstring(L"Invalid PID.");
    return static_cast<DWORD>(n);
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--test-host") return test_host(argv[2], argv[3]);
        if (argc == 3 && std::wstring(argv[1]) == L"--self-test") return self_test(argv[2]);
        if (argc == 3 && (std::wstring(argv[1]) == L"--host-status" || std::wstring(argv[1]) == L"--reload" || std::wstring(argv[1]) == L"--unload")) {
            const auto command = std::wstring(argv[1]) == L"--reload" ? odl::HostCommand::reload : std::wstring(argv[1]) == L"--unload" ? odl::HostCommand::unload : odl::HostCommand::status;
            const auto result = odl::host_command(odl::process(parse_pid(argv[2])), command, 5000);
            std::wcout << result.message << L'\n'; return result.available && result.ok ? 0 : 1;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--list") {
            for (const auto& p : odl::processes())
                std::wcout << p.pid << L"\t" << odl::machine_name(p.machine) << L"\t" << p.path << L'\n';
            return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--inspect") {
            const auto d = odl::inspect(argv[2]);
            std::wcout << d.path << L'\n' << odl::machine_name(d.machine) << L"\nSHA-256: " << d.sha256 << L'\n';
            return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--diagnose") {
            const auto target = std::wstring(argv[2]) == L"0" ? odl::Process{} : odl::process(parse_pid(argv[2]));
            std::wcout << odl::diagnose(target, argv[3]) << L'\n';
            return 0;
        }
        if (argc >= 4 && (std::wstring(argv[1]) == L"--load" || std::wstring(argv[1]) == L"--check")) {
            auto target = odl::process(parse_pid(argv[2]));
            std::wcout << L"Target: " << target.path << L" [PID " << target.pid << L"]\n";
            for (int i = 3; i < argc; ++i) {
                auto r = odl::load(target, argv[i], std::wstring(argv[1]) == L"--check");
                std::wcout << r.message << L'\n';
                if (!r.ok()) return r.state == odl::State::pending ? 3 : 1;
            }
            return 0;
        }
        std::wcout << L"Open DLL Loader 0.3.0 (" << odl::machine_name(odl::own_machine()) << L")\n"
            L"  --list                       List readable processes\n"
            L"  --inspect DLL                Validate native PE and print SHA-256\n"
            L"  --diagnose PID DLL           Read-only import/policy report (PID 0: file only)\n"
            L"  --check PID DLL [DLL ...]     Validate without loading\n"
            L"  --load PID DLL [DLL ...]      Load using the Windows loader\n"
            L"  --host-status PID            Query a cooperating plugin host\n"
            L"  --reload PID / --unload PID  Ask that host to reload/unload its own plugin\n"
            L"  --self-test test-plugin.dll  Test only a newly created child\n";
        return argc == 1 ? 0 : 2;
    } catch (const std::wstring& s) { std::wcerr << s << L'\n'; return 1; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
