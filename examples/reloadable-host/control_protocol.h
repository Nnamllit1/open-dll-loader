#pragma once
#include <cstdint>

namespace odl::control {
inline constexpr std::uint32_t magic = 0x4f444c48, version = 1;
enum class Command : std::uint32_t { status = 0, reload = 1, unload = 2 };
struct Request { std::uint32_t signature = magic, abi = version; Command command{}; std::uint32_t reserved{}; };
struct Reply {
    std::uint32_t signature = magic, abi = version, error{}, active{}, generation{};
    char message[448]{};
};
}
