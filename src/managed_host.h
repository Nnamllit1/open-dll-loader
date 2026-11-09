#pragma once
#include "loader.h"
namespace odl {
enum class HostCommand { status, reload, unload };
struct HostReply {
    bool available{}, ok{}, active{};
    DWORD generation{};
    std::wstring message;
};
// Talks only to a cooperating, source-built host's local control pipe.
// It never opens process memory or remotely calls FreeLibrary.
HostReply host_command(const Process& expected, HostCommand command, DWORD timeout_ms = 2000);
}
