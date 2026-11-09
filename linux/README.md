# Open DLL Loader for Proton

This package includes a native Linux game picker and the Windows x64/x86 loader builds. It discovers initialized Steam Proton prefixes across native Steam, Flatpak Steam, and external Steam libraries, and marks games with accessible running-process evidence.

## Start

Install Python 3.10 or later, Tkinter (often packaged as `python3-tk`), and [Protontricks](https://github.com/Matoking/protontricks#installation). Native and Flatpak Protontricks are supported as external launch backends; this package does not bundle them or install anything automatically.

1. Start your Windows game normally through Steam.
2. Run `./odl-proton` from this extracted directory.
3. Select the running game and click **Open loader for this game**. Choose x86 for a 32-bit target; x64 is the default.
4. In the Windows loader, select the game's process, add your DLL, and use **Check only** or **Load checked**.

The picker refreshes every five seconds. Its App ID identifies a Steam game; the Windows process picker supplies Windows PIDs within that game's prefix. Linux PIDs are not accepted as Windows target PIDs.

The loader uses the existing documented `LoadLibraryW` path. Discovering a prefix does not guarantee that its game accepts DLL loading. Reload/unload still requires a cooperating plugin host.

## Terminal use

```bash
./odl-proton --list
./odl-proton --json
./odl-proton --appid 123456 --arch x64
./odl-proton --steam-dir /path/to/Steam
```

`--steam-dir` restricts discovery to the supplied Steam installation; its configured library folders are included. Repeat it to search several installations. A source checkout can use `python3 linux/odl_proton.py --payload-root /path/to/extracted/windows-release`.

The picker delegates launching to `protontricks-launch --appid`, setting the selected Steam installation explicitly. It does not create a demo prefix or start the game again. Protontricks selects Steam's configured Proton version and runtime. Keep that version unchanged while the game is running; restart the game after changing its compatibility tool.

An existing prefix and a prior Steam launch are required. Custom paths outside Steam's `compatdata` layout are listed when visible in running-process metadata, but are not launched through an ambiguous App ID. Duplicate prefixes that disagree with the running game are blocked. Namespaces, inaccessible process metadata, and Flatpak filesystem permissions can limit discovery or access. The picker does not change persistent sandbox permissions, Steam settings, security policies, or game files.

Native Protontricks is preferred when installed. With Flatpak Protontricks, the selected Windows loader directory is exposed read-only for that invocation. Other external libraries or DLL locations must already be accessible to the backend. Failed launches report a log under `${XDG_STATE_HOME:-$HOME/.local/state}/open-dll-loader/`.

See [the platform guide](docs/linux-wine-proton.md) for Wine examples and validation boundaries. CI exercises discovery fixtures, UI behavior, package integrity, and the Windows loader under Wine; it does not run a real Steam game under Proton.
