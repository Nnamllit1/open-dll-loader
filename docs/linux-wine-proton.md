# Linux, Wine, and Proton

The project has two platform paths:

| Component | Windows | Native Linux | Wine / Proton |
| --- | --- | --- | --- |
| Desktop loader and process browser | Native Win32 application | No native GUI build | Run the Windows build; compatibility is checked separately |
| Cooperative plugin host | Windows DLLs | Native `.so` plugins | Windows host and Windows DLLs |
| Automatic process refresh | Every two seconds | Not applicable to the Win32 UI | Depends on the Windows API implementation in that prefix |
| Reload/unload | Cooperating host only | Console/watch mode | Cooperating host; GUI pipe support is runtime-dependent |

The native Linux host archive contains the portable host and examples. The separate Linux Proton package contains a native game picker and the Windows loader, which runs through Proton. Windows DLLs and Linux shared objects are not interchangeable.

## Open the loader in a running Steam game's prefix

Use `OpenDllLoader-linux-proton.tar.gz` for this workflow. It contains a native Linux game picker and both Windows loader architectures. Install Python 3.10+, Tkinter (`python3-tk` on many distributions), and [Protontricks](https://github.com/Matoking/protontricks#installation), then run:

```bash
./odl-proton
```

Start your game normally in Steam. The picker discovers existing Proton prefixes from native and Flatpak Steam installations and their configured library folders. It marks running games using accessible process metadata and refreshes every five seconds. Select the running game, choose its target architecture, and click **Open loader for this game**. The Windows loader opens through `protontricks-launch --appid` with the selected Steam installation. Select the game's Windows process and your DLL inside that loader.

This uses the existing game prefix; it does not start the demo host or launch the game again. Protontricks resolves the game's configured Proton version and Steam runtime. Changing the compatibility tool while a game is running can make those differ; restart the game after such a change. An initialized prefix and a previous Steam launch are required. The launcher does not edit compatibility settings or persistent Flatpak permissions.

Native Protontricks is preferred when available; the installed Flatpak backend is also supported. Flatpak filesystem permissions still apply to external libraries and DLL paths. Custom prefixes outside Steam's `compatdata` layout, conflicting duplicate prefixes, and inaccessible process namespaces are reported instead of guessing. A recognized prefix does not establish that its game accepts normal DLL loading.

Use `./odl-proton --list` or `--json` for discovery without a desktop window. `--steam-dir /path/to/Steam` restricts discovery to that installation and its libraries; repeat the option for several roots. `--appid 123456 --arch x64` opens a loader for one unambiguously discovered running game. Source users can supply `--payload-root /path/to/extracted/windows-release`. See the [complete game-picker instructions](https://github.com/Nnamllit1/open-dll-loader/blob/main/linux/README.md).

CI validates fixture Steam layouts, Linux process detection, UI states, archive integrity, and the Windows loader under Wine. A real Steam game under Proton is not exercised by CI.

## Wine example in an isolated prefix

Extract the Windows x64 release to a writable directory. From its root:

```bash
export WINEPREFIX="$HOME/.local/share/open-dll-loader/demo-prefix"
export WINEARCH=win64
wine "$PWD/x64/odl-demo-launcher.exe"
```

Use a new prefix for this example. Wine fixes a prefix's architecture when it is created. Wine processes using different prefixes are isolated from one another; the prefix also identifies their wineserver connection. See the [Wine manual](https://manpages.debian.org/testing/wine/wine.1.en.html).

The launcher starts the example host and the UI as children with the same environment. Select `odl-plugin-host.exe` in the sidebar; the process list refreshes automatically. The host watches `x64/active-plugin.dll`. Replace that source file with revision 2:

```bash
cp x64/reloadable-plugin-v2.dll x64/active-plugin.dll
```

The host reloads after the file settles. If GUI host controls are unavailable in a particular Wine build, use the host's console commands and watch mode. The loader's normal process access, module enumeration, and Win32 UI APIs remain compatibility requirements; this guide does not imply that every Wine version supports them.

## Isolated Proton demo through Steam

Use Proton supplied by Steam, as recommended by [Valve's Proton project](https://github.com/ValveSoftware/Proton). Add `x64/odl-demo-launcher.exe` as a non-Steam shortcut and select your installed Proton version in that shortcut's Compatibility settings. Launch the shortcut through Steam.

The launcher keeps both example programs under the same Proton environment. Starting the loader with system Wine in a different prefix does not establish access to a Proton session. This example uses its own host; it does not alter another title's compatibility data or attempt to bypass that title's loading restrictions.

For a Proton diagnostic log, set the shortcut's launch options to:

```text
PROTON_LOG=1 %command%
```

Valve documents this option in the [Proton runtime configuration reference](https://github.com/ValveSoftware/Proton#runtime-config-options). Keep the tested Proton version, architecture, and logs when reporting a compatibility result. Wine CI is useful evidence for Wine, not a substitute for a Steam/Proton test.

## Build a native Linux host

```bash
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel 4
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux --prefix staging
```

The [hot-reload example](https://github.com/Nnamllit1/open-dll-loader/tree/main/examples/reloadable-host) explains the C ABI and stop/start contract. Porting the desktop application itself would additionally require a Linux UI and process-enumeration backend; the existing Win32 controls and Windows DLL-loading path are not compiled on Linux.
