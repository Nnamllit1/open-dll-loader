#include "managed_host.h"
#include "../examples/reloadable-host/control_protocol.h"
#include <cstring>

namespace odl {
HostReply host_command(const Process& expected, HostCommand command, DWORD timeout_ms) {
    HostReply reply;
    auto fail = [&](const std::wstring& text) { reply.message = text; return reply; };
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, expected.pid));
    if (!process) return fail(L"Host query failed: " + error(GetLastError()));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) return fail(L"Cannot verify host identity.");
    const auto identity = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    if (!expected.created || identity != expected.created || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        return fail(L"Host identity changed; refresh and select the process again.");
    const auto name = L"\\\\.\\pipe\\OpenDllLoader.Host." + std::to_wstring(expected.pid);
    HANDLE raw = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(name.c_str(), timeout_ms))
        raw = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    Handle pipe(raw);
    if (!pipe) return fail(L"This process does not expose the cooperative reload protocol. Arbitrary DLLs cannot be unloaded through this control.");
    ULONG server{};
    if (!GetNamedPipeServerProcessId(pipe.get(), &server) || server != expected.pid)
        return fail(L"The control pipe's server could not be verified. No command was sent.");
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) return fail(L"The host control pipe is incompatible.");
    control::Request request; request.command = static_cast<control::Command>(command);
    control::Reply result;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return fail(L"Cannot create host request event.");
    OVERLAPPED io{}; io.hEvent = event.get(); DWORD bytes{};
    if (!TransactNamedPipe(pipe.get(), &request, sizeof(request), &result, sizeof(result), &bytes, &io)) {
        if (GetLastError() != ERROR_IO_PENDING) return fail(L"Host request failed: " + error(GetLastError()));
        if (WaitForSingleObject(event.get(), timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe.get(), &io); GetOverlappedResult(pipe.get(), &io, &bytes, TRUE);
            return fail(L"Host response timed out. A reload/unload may still be pending; check the host console before retrying. No unload was forced.");
        }
        if (!GetOverlappedResult(pipe.get(), &io, &bytes, FALSE)) return fail(L"Host response failed: " + error(GetLastError()));
    }
    if (bytes != sizeof(result) || result.signature != control::magic || result.abi != control::version)
        return fail(L"The host returned an incompatible control response.");
    const auto ack = control::acknowledgement;
    OVERLAPPED ackIo{}; ackIo.hEvent = event.get(); ResetEvent(event.get()); DWORD acknowledged{};
    if (!WriteFile(pipe.get(), &ack, sizeof(ack), &acknowledged, &ackIo) && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event.get(), timeout_ms) != WAIT_OBJECT_0) CancelIoEx(pipe.get(), &ackIo);
        GetOverlappedResult(pipe.get(), &ackIo, &acknowledged, TRUE);
    }
    result.message[sizeof(result.message) - 1] = '\0';
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result.message, -1, nullptr, 0);
    if (length > 0) {
        std::wstring message(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result.message, -1, message.data(), length); message.pop_back(); reply.message = std::move(message);
    }
    reply.available = true; reply.ok = result.error == ERROR_SUCCESS; reply.active = result.active != 0; reply.generation = result.generation;
    if (!reply.ok) reply.message = L"Host declined the operation: " + error(result.error) + L"\r\n" + reply.message;
    return reply;
}
}
