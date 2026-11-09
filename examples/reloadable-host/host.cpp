#include "plugin_api.h"
#include "control_protocol.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <dlfcn.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted{};
void interrupt(int) { interrupted = 1; }
void ODL_PLUGIN_CALL log_line(const char* text) { std::cout << text << '\n' << std::flush; }
struct Stamp {
    fs::file_time_type time{}; std::uintmax_t size{}; bool valid{};
    bool operator==(const Stamp&) const = default;
};
Stamp stamp(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error); if (error) return {};
    const auto time = fs::last_write_time(path, error); if (error) return {};
    return {time, size, true};
}
std::uint64_t pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
std::string load_error() {
#ifdef _WIN32
    return "Windows loader error " + std::to_string(GetLastError());
#else
    const char* error = dlerror(); return error ? error : "dlopen/dlsym failed";
#endif
}
struct Module {
#ifdef _WIN32
    HMODULE handle{};
#else
    void* handle{};
#endif
    const OdlPluginV1* api{};
    fs::path shadow;
};
class Host {
    fs::path source_, cache_;
    Module current_;
    OdlHostV1 api_{sizeof(OdlHostV1), ODL_PLUGIN_ABI, log_line};
    OdlStateV1 state_{};
    std::uint64_t serial_{};
    std::uint32_t generation_{};
    bool blocked_{};
    std::vector<Module> retained_;
    std::vector<fs::path> stagedFiles_;
    bool close(Module& module) {
        if (module.handle) {
#ifdef _WIN32
            const bool closed = FreeLibrary(module.handle) != 0;
#else
            const bool closed = dlclose(module.handle) == 0;
#endif
            if (!closed) {
                retained_.push_back(module); module = {}; blocked_ = true;
                log_line("Operating-system unload failed. Module retained; restart this example host."); return false;
            }
            module.handle = nullptr;
        }
        // Only delete an exact file created inside this host's fresh cache.
        if (!module.shadow.empty() && module.shadow.parent_path() == cache_) {
            std::error_code error; fs::remove(module.shadow, error);
        }
        module.api = nullptr;
        return true;
    }
    Module stage() {
        const auto before = stamp(source_); if (!before.valid) throw std::runtime_error("Plugin file is not readable.");
        Module module; module.shadow = cache_ / ("module-" + std::to_string(++serial_) + source_.extension().string());
        stagedFiles_.push_back(module.shadow);
        fs::copy_file(source_, module.shadow);
        if (stamp(source_) != before) { close(module); throw std::runtime_error("Plugin changed during staging; try again after the build finishes."); }
#ifdef _WIN32
        module.handle = LoadLibraryExW(module.shadow.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module.handle) { auto error = load_error(); close(module); throw std::runtime_error(error); }
        auto entry = GetProcAddress(module.handle, "odl_plugin_v1");
        if (!entry) entry = GetProcAddress(module.handle, "_odl_plugin_v1");
        // FARPROC conversion is the documented Windows dynamic-linking ABI.
        OdlGetPluginV1 get{}; static_assert(sizeof(get) == sizeof(entry)); std::memcpy(&get, &entry, sizeof(get));
#else
        module.handle = dlopen(module.shadow.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module.handle) { auto error = load_error(); close(module); throw std::runtime_error(error); }
        auto entry = dlsym(module.handle, "odl_plugin_v1");
        OdlGetPluginV1 get{}; static_assert(sizeof(get) == sizeof(entry)); std::memcpy(&get, &entry, sizeof(get));
#endif
        if (!get) { close(module); throw std::runtime_error("Missing odl_plugin_v1 export."); }
        module.api = get();
        if (!module.api || module.api->size < sizeof(OdlPluginV1) || module.api->abi != ODL_PLUGIN_ABI ||
            !module.api->name || !module.api->start || !module.api->stop || !module.api->tick) {
            close(module); throw std::runtime_error("Plugin ABI mismatch.");
        }
        return module;
    }
public:
    explicit Host(fs::path source) : source_(fs::absolute(std::move(source))) {
        const auto unique = std::to_string(pid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < 32; ++attempt) {
            cache_ = fs::temp_directory_path() / ("odl-plugin-host-" + unique + "-" + std::to_string(attempt));
            if (fs::create_directory(cache_)) {
#ifndef _WIN32
                if (chmod(cache_.c_str(), 0700) != 0) throw std::runtime_error("Cannot restrict plugin cache permissions.");
#endif
                return;
            }
        }
        throw std::runtime_error("Cannot create a fresh plugin cache.");
    }
    ~Host() {
        if (current_.handle && !unload()) {
            // A refusing plugin is left mapped. Never force FreeLibrary/dlclose.
            return;
        }
        if (!retained_.empty()) return;
        for (const auto& file : stagedFiles_) if (file.parent_path() == cache_) { std::error_code error; fs::remove(file, error); }
        std::error_code error; fs::remove(cache_, error); // empty directory only
    }
    bool reload() {
        if (blocked_) { log_line("Reload refused: a plugin failed its lifecycle contract; restart this example host."); return false; }
        Module candidate;
        try { candidate = stage(); }
        catch (const std::exception& error) { log_line((std::string("Reload failed before stopping the active plugin: ") + error.what()).c_str()); return false; }
        if (current_.handle && !current_.api->stop()) { close(candidate); log_line("Unload refused; the current plugin remains active."); return false; }
        const OdlStateV1 saved = state_;
        if (!candidate.api->start(&api_, &state_)) {
            if (!candidate.api->stop()) {
                retained_.push_back(candidate); blocked_ = true;
                log_line("New plugin failed start and refused cleanup. Modules retained; restart this example host."); return false;
            }
            close(candidate); state_ = saved;
            if (current_.handle && !current_.api->start(&api_, &state_)) {
                blocked_ = true; log_line("Previous plugin could not restart. Module retained; restart this example host."); return false;
            }
            log_line("New plugin failed start; previous plugin restored."); return false;
        }
        if (!close(current_)) { current_ = candidate; return false; }
        current_ = candidate; ++generation_;
        log_line(("Reloaded generation=" + std::to_string(generation_) + " name=" + current_.api->name).c_str()); return true;
    }
    bool unload() {
        if (blocked_) { log_line("Unload refused: plugin cleanup is incomplete. Module retained."); return false; }
        if (!current_.handle) return true;
        if (!current_.api->stop()) { log_line("Unload refused; the current plugin remains active."); return false; }
        if (!close(current_)) return false;
        log_line("Plugin unloaded. Host is still running."); return true;
    }
    void tick() { if (current_.handle && !blocked_) current_.api->tick(); }
    bool active() const { return current_.handle && !blocked_; }
    std::uint32_t generation() const { return generation_; }
    const fs::path& source() const { return source_; }
    std::string status() const {
        return "STATUS active=" + std::to_string(active() ? 1 : 0) + " generation=" + std::to_string(generation_) + " ticks=" + std::to_string(state_.ticks) +
               " name=" + (current_.handle && !blocked_ ? current_.api->name : "none");
    }
};

#ifdef _WIN32
class ControlPipe {
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool connected_{};
    bool replied_{};
    ULONGLONG lastActivity_{};
    void disconnect() { DisconnectNamedPipe(pipe_); connected_ = replied_ = false; }
public:
    ControlPipe() {
        const auto name = L"\\\\.\\pipe\\OpenDllLoader.Host." + std::to_wstring(GetCurrentProcessId());
        pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 1024, 1024, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) log_line("GUI control pipe unavailable; console commands and watch mode still work.");
    }
    ~ControlPipe() { if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_); }
    void poll(Host& host, bool& watch) {
        using namespace odl::control;
        if (pipe_ == INVALID_HANDLE_VALUE) return;
        if (!connected_) {
            if (!ConnectNamedPipe(pipe_, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
            connected_ = true;
            lastActivity_ = GetTickCount64();
        }
        if (replied_) {
            std::uint32_t ack{}; DWORD bytes{};
            if (ReadFile(pipe_, &ack, sizeof(ack), &bytes, nullptr)) { disconnect(); return; }
            const auto error = GetLastError();
            if ((error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING) && GetTickCount64() - lastActivity_ < 1000) return;
            disconnect(); return;
        }
        Request request; DWORD bytes{};
        if (!ReadFile(pipe_, &request, sizeof(request), &bytes, nullptr)) {
            const auto error = GetLastError();
            if ((error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING) && GetTickCount64() - lastActivity_ < 1000) return;
            disconnect(); return;
        }
        Reply reply;
        if (bytes != sizeof(request) || request.signature != magic || request.abi != version || request.reserved ||
            static_cast<std::uint32_t>(request.command) > static_cast<std::uint32_t>(Command::unload)) reply.error = ERROR_INVALID_DATA;
        else if (request.command == Command::reload && !host.reload()) reply.error = ERROR_DLL_INIT_FAILED;
        else if (request.command == Command::unload && !host.unload()) reply.error = ERROR_BUSY;
        if (request.command == Command::unload && !reply.error) watch = false;
        reply.active = host.active() ? 1u : 0u; reply.generation = host.generation();
        const auto text = host.status(); std::memcpy(reply.message, text.data(), std::min(text.size(), sizeof(reply.message) - 1));
        if (!WriteFile(pipe_, &reply, sizeof(reply), &bytes, nullptr) || bytes != sizeof(reply)) { disconnect(); return; }
        // Wait for acknowledgement before discarding the response. A bounded
        // idle timeout also handles clients that close without an ACK; Wine
        // can report that case as NO_DATA rather than BROKEN_PIPE.
        replied_ = true; lastActivity_ = GetTickCount64();
    }
};
#else
struct ControlPipe { void poll(Host&, bool&) {} };
#endif

struct Input {
    std::string buffer;
    bool eof{};
#ifdef _WIN32
    struct PipeInput { std::mutex mutex; std::deque<std::string> lines; std::atomic<bool> done{}; };
    std::shared_ptr<PipeInput> pipeInput;
    std::thread reader;
    bool console{};
    Input() {
        const auto input = GetStdHandle(STD_INPUT_HANDLE); DWORD mode{}; console = GetConsoleMode(input, &mode) != 0;
        if (console) return;
        pipeInput = std::make_shared<PipeInput>(); const auto state = pipeInput;
        // Unix stdin forwarded by Wine is not necessarily a Windows named
        // pipe. Read it directly on a dedicated thread, leaving the host loop
        // free for watch mode and control requests.
        reader = std::thread([state, input] {
            std::string line;
            for (;;) {
                char ch{}; DWORD got{};
                if (!ReadFile(input, &ch, 1, &got, nullptr) || !got) break;
                if (ch == '\n') {
                    const bool quit = line == "quit";
                    { std::lock_guard lock(state->mutex); state->lines.push_back(std::move(line)); }
                    line.clear(); if (quit) break;
                } else if (ch != '\r' && line.size() < 4096) line.push_back(ch);
            }
            state->done = true;
        });
    }
    ~Input() {
        if (!reader.joinable()) return;
        if (!pipeInput->done) CancelSynchronousIo(reader.native_handle());
        if (WaitForSingleObject(reader.native_handle(), 250) == WAIT_OBJECT_0) reader.join();
        else reader.detach(); // reader owns its state until this process ends
    }
#endif
    bool line(std::string& out) {
#ifdef _WIN32
        if (console) {
            while (_kbhit()) {
                const int ch = _getch();
                if (ch == '\r' || ch == '\n') { std::cout << '\n'; out = buffer; buffer.clear(); return true; }
                if (ch == '\b') { if (!buffer.empty()) { buffer.pop_back(); std::cout << "\b \b"; } }
                else if (ch >= 32 && ch < 127) { buffer.push_back(static_cast<char>(ch)); std::cout << static_cast<char>(ch) << std::flush; }
            }
            return false;
        }
        std::lock_guard lock(pipeInput->mutex);
        if (!pipeInput->lines.empty()) { out = std::move(pipeInput->lines.front()); pipeInput->lines.pop_front(); return true; }
        eof = pipeInput->done;
#else
        for (;;) {
            fd_set ready; FD_ZERO(&ready); FD_SET(STDIN_FILENO, &ready); timeval timeout{};
            if (select(STDIN_FILENO + 1, &ready, nullptr, nullptr, &timeout) <= 0) break;
            char ch{}; const auto got = read(STDIN_FILENO, &ch, 1);
            if (got <= 0) { eof = true; break; }
            if (ch == '\n') { out = buffer; buffer.clear(); return true; }
            if (ch != '\r') buffer.push_back(ch);
        }
#endif
        return false;
    }
};
int run(const std::vector<fs::path>& args) {
    fs::path plugin; bool watch{}, noConsole{};
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--plugin" && i + 1 < args.size()) plugin = args[++i];
        else if (args[i] == "--watch") watch = true;
        else if (args[i] == "--no-console") noConsole = true;
        else { std::cerr << "Usage: odl-plugin-host --plugin LIBRARY [--watch] [--no-console]\n"; return 2; }
    }
    if (plugin.empty()) { std::cerr << "Specify --plugin DLL (Windows) or --plugin SO (Linux).\n"; return 2; }
    auto storage = std::make_unique<Host>(plugin); auto& host = *storage; ControlPipe control;
    std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
    const auto initialStamp = stamp(host.source()); host.reload();
    std::cout << "HOST_READY pid=" << pid() << "\nCommands: status, reload, unload, quit. Watch=" << (watch ? 1 : 0) << '\n' << std::flush;
    std::unique_ptr<Input> input;
    if (!noConsole) input = std::make_unique<Input>();
    auto attempted = initialStamp, observed = attempted;
    auto changed = std::chrono::steady_clock::now();
    while (!interrupted) {
        control.poll(host, watch);
        if (!noConsole) {
            std::string command;
            if (input->line(command)) {
                if (command == "quit") break;
                if (command == "reload") { host.reload(); attempted = stamp(host.source()); }
                else if (command == "unload") { host.unload(); watch = false; }
                else if (command == "status") log_line(host.status().c_str());
                else if (command == "watch on") { watch = true; log_line("Watch enabled."); }
                else if (command == "watch off") { watch = false; log_line("Watch paused."); }
                else if (!command.empty()) log_line("Commands: status, reload, unload, watch on, watch off, quit.");
            }
            if (input->eof) break;
        }
        host.tick();
        if (watch) {
            const auto current = stamp(host.source()); const auto now = std::chrono::steady_clock::now();
            if (current != observed) { observed = current; changed = now; }
            else if (current.valid && current != attempted && now - changed >= std::chrono::milliseconds(500)) {
                attempted = current; host.reload();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    log_line("Host exiting.");
    if (!host.unload()) {
        // Quit ends this example process. It does not forcibly unload a module
        // whose workers may still reference host-owned state.
        storage.release(); log_line("Plugin refused shutdown; retaining its state until process termination."); std::_Exit(0);
    }
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try { std::vector<fs::path> args; for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]); return run(args); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#else
int main(int argc, char** argv) {
    try { std::vector<fs::path> args; for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]); return run(args); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
