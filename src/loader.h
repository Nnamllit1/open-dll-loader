#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace odl {
class Handle {
    HANDLE value_ = nullptr;
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
};
struct Process {
    DWORD pid{};
    std::uint64_t created{};
    USHORT machine{};
    std::wstring name, path;
};
struct Dll {
    std::wstring path, sha256;
    USHORT machine{};
    std::vector<std::wstring> imports;
};
enum class State { loaded, already_loaded, failed, pending };
struct Result {
    State state = State::failed;
    std::wstring message;
    bool ok() const { return state == State::loaded || state == State::already_loaded; }
};
USHORT own_machine();
std::wstring machine_name(USHORT machine);
std::wstring error(DWORD code);
std::vector<Process> processes();
Process process(DWORD pid);
Dll inspect(const std::wstring& path);
std::wstring diagnose(const Process& target, const std::wstring& path);
// expected.created binds the request to the process selected by the user.
// dry_run validates access and inputs without allocating or starting a thread.
Result load(const Process& expected, const std::wstring& path, bool dry_run = false);
}
