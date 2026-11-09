#include "loader.h"
#include <tlhelp32.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <cstring>

namespace odl {
namespace {
[[noreturn]] void fail(const std::wstring& s) { throw s; }
void check(bool ok, const wchar_t* what) {
    if (!ok) fail(std::wstring(what) + L": " + error(GetLastError()));
}
std::uint64_t creation(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    check(GetProcessTimes(h, &c, &e, &k, &u) != 0, L"GetProcessTimes");
    return (static_cast<std::uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
USHORT architecture(HANDLE h) {
    USHORT processMachine{}, nativeMachine{};
    using Query = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto symbol = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    Query query{}; static_assert(sizeof(query) == sizeof(symbol)); std::memcpy(&query, &symbol, sizeof(query));
    if (query && query(h, &processMachine, &nativeMachine))
        return processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
    if (query && GetLastError() != ERROR_CALL_NOT_IMPLEMENTED && GetLastError() != ERROR_INVALID_FUNCTION)
        fail(L"IsWow64Process2: " + error(GetLastError()));
    // Older Wine implementations may lack the newer query. No security or
    // compatibility setting is changed; use the older architecture query.
    BOOL wow{}; check(IsWow64Process(h, &wow) != 0, L"IsWow64Process");
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    if (wow) return IMAGE_FILE_MACHINE_I386;
    if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) return IMAGE_FILE_MACHINE_AMD64;
    if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) return IMAGE_FILE_MACHINE_I386;
    return IMAGE_FILE_MACHINE_UNKNOWN;
}
std::wstring file_path(HANDLE h) {
    std::vector<wchar_t> b(32768);
    DWORD n = GetFinalPathNameByHandleW(h, b.data(), static_cast<DWORD>(b.size()), FILE_NAME_NORMALIZED);
    check(n != 0 && n < b.size(), L"Resolve DLL path");
    std::wstring p(b.data(), n);
    // Use ordinary DOS paths for Toolhelp comparisons and UI display.
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    return p;
}
Handle open_file(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    check(static_cast<bool>(file), L"Open DLL (file must be readable and not being modified)");
    return file;
}
template<class T> T read_at(HANDLE h, LONGLONG offset, LONGLONG length) {
    if (offset < 0 || offset > length || static_cast<LONGLONG>(sizeof(T)) > length - offset)
        fail(L"Truncated or malformed PE header.");
    LARGE_INTEGER pos{}; pos.QuadPart = offset;
    check(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0, L"Seek DLL");
    T t{}; DWORD got{};
    check(ReadFile(h, &t, sizeof(t), &got, nullptr) && got == sizeof(t), L"Read DLL");
    return t;
}
std::wstring hash(HANDLE h) {
    BCRYPT_ALG_HANDLE alg{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        fail(L"Cannot initialize SHA-256.");
    struct AlgGuard { BCRYPT_ALG_HANDLE h; ~AlgGuard() { BCryptCloseAlgorithmProvider(h, 0); } } ag{alg};
    BCRYPT_HASH_HANDLE ctx{};
    if (BCryptCreateHash(alg, &ctx, nullptr, 0, nullptr, 0, 0) < 0) fail(L"Cannot create SHA-256.");
    struct HashGuard { BCRYPT_HASH_HANDLE h; ~HashGuard() { BCryptDestroyHash(h); } } hg{ctx};
    LARGE_INTEGER zero{};
    check(SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) != 0, L"Rewind DLL");
    std::array<UCHAR, 65536> buf{};
    for (;;) {
        DWORD got{}; check(ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) != 0, L"Hash DLL");
        if (!got) break;
        if (BCryptHashData(ctx, buf.data(), got, 0) < 0) fail(L"SHA-256 update failed.");
    }
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(ctx, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        fail(L"SHA-256 finalization failed.");
    std::wostringstream out;
    for (auto b : digest) out << std::hex << std::setfill(L'0') << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}
Dll inspect_file(HANDLE file) {
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file, &size) != 0, L"Get DLL size");
    const auto dos = read_at<IMAGE_DOS_HEADER>(file, 0, size.QuadPart);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos)) fail(L"Not a valid PE DLL.");
    LONGLONG pos = dos.e_lfanew;
    if (read_at<DWORD>(file, pos, size.QuadPart) != IMAGE_NT_SIGNATURE) fail(L"Invalid PE signature.");
    const auto fh = read_at<IMAGE_FILE_HEADER>(file, pos + sizeof(DWORD), size.QuadPart);
    if (!(fh.Characteristics & IMAGE_FILE_DLL)) fail(L"The selected PE file is not a DLL.");
    if (fh.Machine != IMAGE_FILE_MACHINE_AMD64 && fh.Machine != IMAGE_FILE_MACHINE_I386)
        fail(L"Only native x64 and x86 DLLs are supported.");
    pos += sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    DWORD clr{}, headers{};
    IMAGE_DATA_DIRECTORY importDirectory{};
    if (fh.Machine == IMAGE_FILE_MACHINE_AMD64) {
        if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) fail(L"Invalid PE optional header.");
        const auto oh = read_at<IMAGE_OPTIONAL_HEADER64>(file, pos, size.QuadPart);
        if (oh.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) fail(L"PE architecture/header mismatch.");
        headers = oh.SizeOfHeaders;
        if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT)
            importDirectory = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
            clr = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress;
    } else {
        if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)) fail(L"Invalid PE optional header.");
        const auto oh = read_at<IMAGE_OPTIONAL_HEADER32>(file, pos, size.QuadPart);
        if (oh.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) fail(L"PE architecture/header mismatch.");
        headers = oh.SizeOfHeaders;
        if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT)
            importDirectory = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (oh.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)
            clr = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress;
    }
    if (clr) fail(L"Managed/mixed-mode DLLs require a dedicated host and are not supported.");
    if (!fh.NumberOfSections || fh.NumberOfSections > 96) fail(L"Invalid PE section count.");
    const auto sectionStart = pos + fh.SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections;
    for (unsigned i = 0; i < fh.NumberOfSections; ++i) {
        auto sh = read_at<IMAGE_SECTION_HEADER>(file, sectionStart + i * sizeof(IMAGE_SECTION_HEADER), size.QuadPart);
        if (static_cast<std::uint64_t>(sh.PointerToRawData) + sh.SizeOfRawData > static_cast<std::uint64_t>(size.QuadPart))
            fail(L"Truncated PE section data.");
        sections.push_back(sh);
    }
    auto offset_of = [&](std::uint64_t rva, std::uint64_t bytes) -> LONGLONG {
        if (rva < headers && bytes <= headers - rva && rva + bytes <= static_cast<std::uint64_t>(size.QuadPart))
            return static_cast<LONGLONG>(rva);
        for (const auto& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const auto delta = rva - section.VirtualAddress;
            if (delta <= section.SizeOfRawData && bytes <= section.SizeOfRawData - delta)
                return static_cast<LONGLONG>(section.PointerToRawData + delta);
        }
        fail(L"Invalid PE import address.");
    };
    std::vector<std::wstring> imports;
    if (importDirectory.VirtualAddress) {
        bool terminated{};
        const auto limit = std::min<DWORD>(4096, importDirectory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR));
        for (DWORD i = 0; i < limit; ++i) {
            const auto rva = static_cast<std::uint64_t>(importDirectory.VirtualAddress) + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            const auto descriptor = read_at<IMAGE_IMPORT_DESCRIPTOR>(file, offset_of(rva, sizeof(IMAGE_IMPORT_DESCRIPTOR)), size.QuadPart);
            if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) { terminated = true; break; }
            std::wstring name;
            bool ended{};
            for (unsigned n = 0; n < 256; ++n) {
                const auto ch = read_at<unsigned char>(file, offset_of(static_cast<std::uint64_t>(descriptor.Name) + n, 1), size.QuadPart);
                if (!ch) { ended = true; break; }
                if (ch < 32 || ch > 126) fail(L"Invalid character in PE import name.");
                name.push_back(static_cast<wchar_t>(ch));
            }
            if (!ended || name.empty()) fail(L"Invalid PE import name.");
            imports.push_back(std::move(name));
        }
        if (!terminated) fail(L"Unterminated PE import directory.");
    }
    return {file_path(file), hash(file), fh.Machine, std::move(imports)};
}
std::vector<MODULEENTRY32W> modules(DWORD pid) {
    // Toolhelp can transiently fail while the loader changes its module list.
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 8; ++i) {
        raw = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (raw != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(10);
    }
    Handle snapshot(raw);
    check(static_cast<bool>(snapshot), L"Read target modules");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::vector<MODULEENTRY32W> out;
    if (!Module32FirstW(snapshot.get(), &entry)) {
        if (GetLastError() == ERROR_NO_MORE_FILES) return out;
        fail(L"Module32First: " + error(GetLastError()));
    }
    do { out.push_back(entry); entry.dwSize = sizeof(entry); } while (Module32NextW(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) fail(L"Module32Next: " + error(GetLastError()));
    return out;
}
bool contains(const std::vector<MODULEENTRY32W>& mods, const std::wstring& path) {
    return std::any_of(mods.begin(), mods.end(), [&](const auto& m) { return _wcsicmp(m.szExePath, path.c_str()) == 0; });
}
LPTHREAD_START_ROUTINE remote_loader(const std::vector<MODULEENTRY32W>& mods) {
    // GetProcAddress may return a forwarded export inside KernelBase.dll.
    // Find its actual owner and rebase the RVA onto that module in the target.
    auto proc = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    check(proc != nullptr, L"Resolve LoadLibraryW");
    HMODULE owner{};
    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(proc), &owner) != 0, L"Resolve loader owner");
    wchar_t path[32768]{};
    check(GetModuleFileNameW(owner, path, 32768) != 0, L"Read loader owner");
    const auto name = wcsrchr(path, L'\\');
    const auto offset = reinterpret_cast<std::uintptr_t>(proc) - reinterpret_cast<std::uintptr_t>(owner);
    for (const auto& m : mods) {
        if (_wcsicmp(m.szModule, name ? name + 1 : path) == 0 && offset < m.modBaseSize)
            return reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<std::uintptr_t>(m.modBaseAddr) + offset);
    }
    fail(L"The Windows loader module is unavailable in the target.");
}
}
USHORT own_machine() {
#ifdef _WIN64
    return IMAGE_FILE_MACHINE_AMD64;
#else
    return IMAGE_FILE_MACHINE_I386;
#endif
}
std::wstring machine_name(USHORT m) {
    if (m == IMAGE_FILE_MACHINE_AMD64) return L"x64";
    if (m == IMAGE_FILE_MACHINE_I386) return L"x86";
    return L"unsupported";
}
std::wstring error(DWORD code) {
    wchar_t* buf{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"Windows error";
    if (buf) LocalFree(buf);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s + L" (" + std::to_wstring(code) + L")";
}
Process process(DWORD pid) {
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    check(static_cast<bool>(h), L"Open target");
    if (WaitForSingleObject(h.get(), 0) != WAIT_TIMEOUT) fail(L"Target has exited.");
    std::vector<wchar_t> buf(32768); DWORD n = static_cast<DWORD>(buf.size());
    check(QueryFullProcessImageNameW(h.get(), 0, buf.data(), &n) != 0, L"Read target path");
    std::wstring path(buf.data(), n);
    auto slash = path.find_last_of(L"\\/");
    return {pid, creation(h.get()), architecture(h.get()), path.substr(slash + 1), path};
}
std::vector<Process> processes() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    check(static_cast<bool>(snapshot), L"List processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::vector<Process> out;
    if (Process32FirstW(snapshot.get(), &entry)) do {
        if (entry.th32ProcessID <= 4 || entry.th32ProcessID == GetCurrentProcessId()) continue;
        try { out.push_back(process(entry.th32ProcessID)); } catch (const std::wstring&) { /* inaccessible/exited */ }
    } while (Process32NextW(snapshot.get(), &entry));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { const auto order = _wcsicmp(a.name.c_str(), b.name.c_str()); return order ? order < 0 : a.pid < b.pid; });
    return out;
}
Dll inspect(const std::wstring& path) { auto f = open_file(path); return inspect_file(f.get()); }
std::wstring diagnose(const Process& target, const std::wstring& path) {
    const auto dll = inspect(path);
    std::wostringstream out;
    out << L"READ-ONLY INSPECTION\r\nDLL: " << dll.path << L"\r\nArchitecture: " << machine_name(dll.machine)
        << L"\r\nSHA-256: " << dll.sha256 << L"\r\n";
    if (target.pid) {
        out << L"Target: " << target.path << L" [PID " << target.pid << L"]\r\n";
        Handle h(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, target.pid));
        if (!h) out << L"Process policy query unavailable: " << error(GetLastError()) << L"\r\n";
        else if (!target.created || creation(h.get()) != target.created)
            out << L"Process identity changed. Refresh and select the process again.\r\n";
        else {
            PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy{};
            if (GetProcessMitigationPolicy(h.get(), ProcessSignaturePolicy, &policy, sizeof(policy))) {
                out << L"Image signature restrictions: Microsoft-only=" << policy.MicrosoftSignedOnly
                    << L", Store-only=" << policy.StoreSignedOnly << L", Microsoft/Store/WHQL-only=" << policy.MitigationOptIn << L"\r\n";
                if (policy.MicrosoftSignedOnly || policy.StoreSignedOnly || policy.MitigationOptIn)
                    out << L"A signature restriction is enabled. This does not establish which check caused a previous failure.\r\n";
            } else out << L"Image signature policy unavailable: " << error(GetLastError()) << L"\r\n";
        }
        if (target.machine != dll.machine) out << L"Architecture mismatch: DLL and target cannot use this loading path.\r\n";
    }
    out << L"\r\nDirect imports (" << dll.imports.size() << L"):\r\n";
    const auto folder = std::filesystem::path(dll.path).parent_path();
    for (const auto& name : dll.imports) {
        out << L"  " << name;
        // Existence in the DLL folder is an observation, not a reconstruction
        // of the target's API-set, manifest, or transitive dependency resolution.
        if (std::filesystem::path(name).filename() == std::filesystem::path(name)) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(folder / name, ec)) out << L"  [present beside DLL]";
        }
        out << L"\r\n";
    }
    out << L"\r\nNo DLL code was executed and no target memory was modified. This lists direct imports only; "
           L"it does not prove that all dependencies or initialization will succeed. "
           L"Windows may search for dependencies separately from the selected DLL's full path.";
    return out.str();
}
Result load(const Process& expected, const std::wstring& path, bool dry_run) {
    try {
        if (expected.pid <= 4 || expected.pid == GetCurrentProcessId()) fail(L"Select a separate user process.");
        auto file = open_file(path);
        auto dll = inspect_file(file.get());
        if (dll.machine != own_machine() || expected.machine != own_machine())
            fail(L"Loader, target, and DLL must have the same architecture. Use the matching x86/x64 build.");
        const DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                             PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;
        Handle target(OpenProcess(rights, FALSE, expected.pid));
        check(static_cast<bool>(target), L"Open target for loading");
        if (!expected.created || creation(target.get()) != expected.created) fail(L"Process identity changed; select it again.");
        if (WaitForSingleObject(target.get(), 0) != WAIT_TIMEOUT) fail(L"Target has exited.");
        if (architecture(target.get()) != own_machine()) fail(L"Target architecture changed.");
        auto mods = modules(expected.pid);
        if (contains(mods, dll.path)) return {State::already_loaded, L"Already loaded; reference count unchanged: " + dll.path};
        const auto loader = remote_loader(mods);
        if (dry_run) return {State::loaded, L"Validation passed; no memory changed. SHA-256: " + dll.sha256};
        const SIZE_T bytes = (dll.path.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(target.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        check(remote != nullptr, L"Allocate DLL path");
        struct RemotePath {
            HANDLE process; void* address;
            ~RemotePath() { if (address) VirtualFreeEx(process, address, 0, MEM_RELEASE); }
        } allocation{target.get(), remote};
        SIZE_T written{};
        check(WriteProcessMemory(target.get(), remote, dll.path.c_str(), bytes, &written) && written == bytes,
              L"Write DLL path");
        Handle thread(CreateRemoteThread(target.get(), nullptr, 0, loader, remote, 0, nullptr));
        check(static_cast<bool>(thread), L"Start LoadLibraryW");
        const DWORD waited = WaitForSingleObject(thread.get(), 15000);
        if (waited != WAIT_OBJECT_0) {
            // Never free an argument a live remote thread may still be reading.
            // This small allocation remains until target exit. No thread is killed.
            allocation.address = nullptr;
            return {State::pending, L"Load is still pending or its status is unknown. DLL path memory retained until target exit. Do not retry until the target is inspected."};
        }
        if (WaitForSingleObject(target.get(), 0) != WAIT_TIMEOUT) fail(L"Target exited during loading.");
        // A remote thread's DWORD exit code truncates HMODULE on x64. Verify the
        // complete expected module path instead of treating that code as proof.
        if (!contains(modules(expected.pid), dll.path))
            fail(L"Windows did not load the expected DLL. Check dependencies, DllMain, and target policy; the remote GetLastError value is unavailable.");
        return {State::loaded, L"Loaded and verified: " + dll.path + L"\r\nSHA-256: " + dll.sha256};
    } catch (const std::wstring& s) { return {State::failed, s}; }
}
}
