# Cooperative hot reload

This example is an opt-in plugin host for Windows and native Linux. It loads its own plugin and keeps running while you rebuild, reload, or unload that plugin. The Windows build also works as a Wine/Proton compatibility test candidate. It is independent of remote DLL injection.

## Windows quick start

Build the project with `build.cmd x64`, then start `dist/x64/odl-demo-launcher.exe`. The launcher creates `active-plugin.dll` from revision 1 only if the active file does not exist, starts the host in watch mode without requiring stdin, and opens the loader. Select `odl-plugin-host.exe` in the process sidebar. Its **Reload** and **Unload** buttons become available after host discovery. The launcher host displays logs; use the console-only command below when you want interactive commands. Close that host window or press Ctrl+C when finished.

To swap in the other sample without restarting the host:

```powershell
Copy-Item .\dist\x64\reloadable-plugin-v2.dll .\dist\x64\active-plugin.dll -Force
```

The host logs revision 2 and a new generation while retaining its tick counter. You can rebuild your own plugin directly to `active-plugin.dll` because the host loads a temporary copy, leaving the source file unlocked.

Console-only use:

```powershell
.\dist\x64\odl-plugin-host.exe --plugin .\dist\x64\active-plugin.dll --watch
```

Commands are `status`, `reload`, `unload`, `watch on`, `watch off`, and `quit`. Unload pauses watching, so it stays unloaded until you explicitly reload or enable watching again. `--no-console` keeps the host running without reading stdin and allows Windows GUI control. Closing the loader does not close the host.

The loader CLI can query or control a cooperating Windows host:

```powershell
.\dist\x64\open-dll-loader.exe --host-status 1234
.\dist\x64\open-dll-loader.exe --reload 1234
.\dist\x64\open-dll-loader.exe --unload 1234
```

These commands verify process identity and the local pipe's server PID. They do not call `FreeLibrary` remotely. A normal application that does not implement this protocol cannot be controlled this way. A response timeout can leave the host operation pending; inspect its console before retrying.

## Native Linux

From the source repository, with CMake, a C++20 compiler, and Python 3:

```bash
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel 4
ctest --test-dir build/linux --output-on-failure
cp build/linux/examples/reloadable-host/reloadable-plugin-v1.so active-plugin.so
build/linux/examples/reloadable-host/odl-plugin-host --plugin "$PWD/active-plugin.so" --watch
```

In another terminal:

```bash
cp build/linux/examples/reloadable-host/reloadable-plugin-v2.so active-plugin.so
```

The native Linux host uses `dlopen`/`dlsym`/`dlclose` and the same lifecycle ABI. Linux plugins are native `.so` files; they are separate builds from Windows DLLs. The Windows desktop GUI is not a native Linux GUI. The Linux example uses console commands and watch mode rather than the Windows control pipe.

## Integrate into your own host

`plugin_api.h` is the complete C ABI. Export `odl_plugin_v1` and return an `OdlPluginV1` descriptor. The sample in `sample_plugin.cpp` is intentionally small.

- `start(host, state)` receives a stable host logging callback and host-owned state. Do not retain pointers into an earlier plugin image in that state.
- `tick()` runs serially on the host loop. Your application can replace this with its normal safe dispatch point.
- `stop()` must unregister all hooks/callbacks, stop accepting work, and join every plugin worker before returning `1`. Return `0` when unloading must be refused. Returning success without completing cleanup violates the ABI and can crash the host.
- Static constructors and `DllMain` must not start workers or register persistent callbacks. Initialization belongs in `start`, not inside the OS loader lock.
- Callbacks must not throw across the C ABI. A failed `start` must allow `stop` to complete cleanup.

The host stages a fresh copy before stopping the old plugin. A malformed or incompatible replacement leaves the previous plugin active. If a new plugin fails to start and cleans up successfully, the host attempts to restart the old plugin. An incomplete cleanup leaves modules mapped and blocks further lifecycle operations. Quitting the example then terminates the host process; it does not forcibly unload those modules.

Watch mode waits for the source's timestamp and size to remain stable for 500 ms. This is build debouncing, not a transactional filesystem guarantee. If staging or loading fails, the previous plugin is kept where possible; another build or a manual reload retries. Only the main plugin is shadow-copied. Dependencies and resources are not copied, and loading from a temporary path changes `$ORIGIN`/module-directory resolution. The supplied samples use platform runtime dependencies only; real plugins need an explicit dependency/resource strategy.

## Tests

`test_reload.py` launches only newly built example hosts. It checks version swapping, persistent state, source replacement while loaded, invalid replacements, unload/reload, watch mode, and stop refusal. Windows tests also exercise the same local control protocol used by the GUI. CI runs the portable tests on native Linux and the Windows programs under Wine. See the workflow results for the exact commit; Proton needs separate manual testing through Steam.
