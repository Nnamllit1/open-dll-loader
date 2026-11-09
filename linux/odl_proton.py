#!/usr/bin/env python3
"""Discover Steam Proton games and open the Windows loader in a selected prefix."""

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading

APP = "com.github.Matoking.protontricks"
HERE = Path(__file__).resolve().parent
OVERRIDES = (
    "WINEPREFIX", "WINE", "WINESERVER", "PROTON_VERSION", "STEAM_COMPAT_DATA_PATH",
    "STEAM_COMPAT_CLIENT_INSTALL_PATH", "STEAM_COMPAT_APP_ID", "SteamAppId", "SteamGameId",
)


def read_vdf(path):
    """Read Steam's text KeyValues files; never execute their contents."""
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    if len(text) > 4 * 1024 * 1024:
        raise ValueError("Steam metadata is too large")
    pattern = re.compile(r'\s+|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+')
    tokens, offset = [], 0
    while offset < len(text):
        match = pattern.match(text, offset)
        if not match:
            raise ValueError("Invalid Steam text metadata")
        token = match.group()
        offset = match.end()
        if token.isspace() or token.startswith("//"):
            continue
        if token.startswith('"'):
            token = re.sub(r'\\(["\\])', r'\1', token[1:-1])
            tokens.append((token, False))
        else:
            tokens.append((token, token in ("{", "}")))
    position = 0

    def obj(nested=False, depth=0):
        nonlocal position
        if depth > 32:
            raise ValueError("Steam metadata nesting is too deep")
        result = {}
        while position < len(tokens):
            key, structural = tokens[position]
            position += 1
            if structural:
                if key == "}" and nested:
                    return result
                raise ValueError("Unexpected brace in Steam metadata")
            if position == len(tokens):
                raise ValueError("Missing Steam metadata value")
            value, structural = tokens[position]
            position += 1
            if structural:
                if value != "{":
                    raise ValueError("Unexpected closing brace")
                value = obj(True, depth + 1)
            result[key.lower()] = value
        if nested:
            raise ValueError("Unclosed Steam metadata block")
        return result

    return obj()


def metadata(path):
    try:
        return read_vdf(path)
    except (OSError, ValueError):
        return {}


def steam_roots(extra=(), home=None, environ=None):
    home, env = Path(home or Path.home()), os.environ if environ is None else environ
    candidates = list(map(Path, extra))
    if not extra and env.get("STEAM_DIR"):
        candidates.append(Path(env["STEAM_DIR"]))
    if not extra:
        candidates += [home / ".steam/steam", home / ".steam/root",
                   Path(env.get("XDG_DATA_HOME", home / ".local/share")) / "Steam",
                   home / ".var/app/com.valvesoftware.Steam/data/Steam"]
    roots = []
    for candidate in candidates:
        try:
            root = candidate.expanduser().resolve()
            if (root / "steamapps").is_dir() and root not in roots:
                roots.append(root)
        except OSError:
            continue
    return roots


def steam_libraries(root):
    libraries = [root]
    for file in (root / "steamapps/libraryfolders.vdf", root / "config/libraryfolders.vdf"):
        folders = metadata(file).get("libraryfolders", {})
        if not isinstance(folders, dict):
            continue
        for index, value in folders.items():
            if not index.isdecimal():
                continue
            raw = value.get("path") if isinstance(value, dict) else value
            if isinstance(raw, str) and raw:
                try:
                    path = Path(raw).expanduser().resolve()
                    if (path / "steamapps").is_dir() and path not in libraries:
                        libraries.append(path)
                except OSError:
                    continue
    return libraries


@dataclass(frozen=True)
class Session:
    prefix: Path
    appid: int
    steam: Path | None


def running_sessions(proc=Path("/proc"), uid=None):
    """Read only prefix/AppID fields of this user's accessible process environments."""
    uid = os.getuid() if uid is None and hasattr(os, "getuid") else uid
    sessions = set()
    try:
        entries = list(proc.iterdir())
    except OSError:
        return sessions
    fields = {b"WINEPREFIX", b"STEAM_COMPAT_DATA_PATH", b"STEAM_COMPAT_CLIENT_INSTALL_PATH",
              b"SteamAppId", b"SteamGameId", b"STEAM_COMPAT_APP_ID"}
    for entry in entries:
        if not entry.name.isdecimal():
            continue
        try:
            if uid is not None and entry.stat().st_uid != uid:
                continue
            with (entry / "environ").open("rb") as stream:
                blob = stream.read(1024 * 1024 + 1)
            if len(blob) > 1024 * 1024:
                continue
            env = {}
            for item in blob.split(b"\0"):
                key, sep, value = item.partition(b"=")
                if sep and key in fields:
                    env[key.decode()] = os.fsdecode(value)
            raw = env.get("WINEPREFIX")
            if not raw and env.get("STEAM_COMPAT_DATA_PATH"):
                raw = str(Path(env["STEAM_COMPAT_DATA_PATH"]) / "pfx")
            appid = next((int(env[key]) for key in ("STEAM_COMPAT_APP_ID", "SteamAppId", "SteamGameId")
                          if env.get(key, "").isdecimal() and int(env[key]) > 0), 0)
            if raw and Path(raw).is_absolute() and appid:
                steam = env.get("STEAM_COMPAT_CLIENT_INSTALL_PATH")
                sessions.add(Session(Path(raw).resolve(), appid, Path(steam).resolve() if steam else None))
        except (OSError, ValueError):
            continue
    return sessions


@dataclass(frozen=True)
class Game:
    appid: int
    name: str
    steam: Path
    prefix: Path
    running: bool
    launchable: bool = True
    note: str = ""

    @property
    def key(self):
        return f"{self.steam}\n{self.appid}\n{self.prefix}"


def prefix_ready(prefix):
    try:
        return ((prefix / "system.reg").is_file() and (prefix / "drive_c").is_dir()
                and (prefix.parent / "pfx.lock").is_file())
    except OSError:
        return False


def prefix_time(prefix):
    try:
        return (prefix.parent / "pfx.lock").stat().st_mtime_ns
    except OSError:
        return 0


def discover(extra=(), home=None, environ=None, sessions=None):
    sessions = running_sessions() if sessions is None else sessions
    roots = steam_roots(extra, home, environ)
    for session in sessions:
        if not extra and session.steam and (session.steam / "steamapps").is_dir() and session.steam not in roots:
            roots.append(session.steam)
    games = []
    for root in roots:
        names, candidates = {}, {}
        libraries = steam_libraries(root)
        for library in libraries:
            apps = library / "steamapps"
            for manifest in apps.glob("appmanifest_*.acf"):
                state = metadata(manifest).get("appstate", {})
                if not isinstance(state, dict):
                    continue
                appid, name = state.get("appid", ""), state.get("name")
                if isinstance(appid, str) and appid.isdecimal() and isinstance(name, str):
                    names[int(appid)] = name
            try:
                children = list((apps / "compatdata").iterdir())
            except OSError:
                children = []
            for child in children:
                prefix = (child / "pfx").resolve()
                if child.name.isdecimal() and int(child.name) > 0 and prefix_ready(prefix):
                    group = candidates.setdefault(int(child.name), [])
                    if prefix not in group:
                        group.append(prefix)
        for session in sessions:
            if session.steam == root and prefix_ready(session.prefix):
                group = candidates.setdefault(session.appid, [])
                if session.prefix not in group:
                    group.append(session.prefix)
        for appid, prefixes in candidates.items():
            # Match Protontricks' newest pfx.lock choice across libraries.
            selected = max(prefixes, key=prefix_time)
            running = {s.prefix for s in sessions if s.appid == appid and s.prefix in prefixes}
            custom = selected not in [(library / f"steamapps/compatdata/{appid}/pfx").resolve()
                                      for library in libraries]
            ready = not custom and (not running or running == {selected})
            note = ""
            if custom:
                note = "Custom running prefix: Protontricks cannot select this path by AppID."
            elif not ready:
                note = "Multiple prefixes: the running game differs from Steam's newest prefix."
            elif len(prefixes) > 1:
                note = "Multiple prefixes found; using the most recently launched prefix."
            games.append(Game(appid, names.get(appid, f"Steam app {appid}"), root, selected,
                              bool(running), ready, note))
    return sorted(games, key=lambda g: (not g.running, g.name.casefold(), str(g.steam), g.appid))


@dataclass(frozen=True)
class Backend:
    executable: str
    flatpak: bool = False

    def command(self, game, payload):
        if self.flatpak:
            return [self.executable, "run", f"--filesystem={payload.parent}:ro",
                    *[f"--unset-env={name}" for name in OVERRIDES],
                    f"--env=STEAM_DIR={game.steam}", "--command=protontricks-launch", APP,
                    "--no-term", "--appid", str(game.appid), str(payload)]
        return [self.executable, "--no-term", "--appid", str(game.appid), str(payload)]


def find_backend():
    native = shutil.which("protontricks-launch")
    if native:
        return Backend(native)
    flatpak = shutil.which("flatpak")
    if flatpak:
        try:
            if subprocess.run([flatpak, "info", APP], capture_output=True, timeout=8).returncode == 0:
                return Backend(flatpak, True)
        except (OSError, subprocess.TimeoutExpired):
            pass
    return None


def launch(game, arch="x64", payload_root=None, backend=None, log_path=None):
    if not game.launchable:
        raise ValueError(game.note)
    if not prefix_ready(game.prefix):
        raise ValueError("This Proton prefix is no longer available. Refresh and start the game first.")
    backend = backend or find_backend()
    if not backend:
        raise ValueError("Install Protontricks (native or Flatpak), then reopen this launcher. See docs/linux-wine-proton.md.")
    root = Path(payload_root or HERE / "windows").expanduser().resolve()
    payload = root / arch / "OpenDllLoader.exe"
    if not payload.is_file():
        raise ValueError(f"Windows loader missing: {payload}. Extract the complete Linux Proton package.")
    env = os.environ.copy()
    for key in OVERRIDES:
        env.pop(key, None)
    env["STEAM_DIR"] = str(game.steam)
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as log:
            return subprocess.Popen(backend.command(game, payload), env=env, cwd=root,
                                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    return subprocess.Popen(backend.command(game, payload), env=env, cwd=root, stdin=subprocess.DEVNULL)


class Application:
    def __init__(self, root, extra=(), payload_root=None, auto_refresh=True):
        import tkinter as tk
        from tkinter import ttk
        self.root, self.extra, self.payload_root = root, extra, payload_root
        self.games, self.visible, self.results, self.children = [], {}, queue.Queue(), []
        self.launch_results, self.launching = queue.Queue(), False
        self.scanning, self.backend = False, find_backend()
        root.title("Open DLL Loader · Proton")
        root.geometry("980x680")
        root.minsize(780, 570)
        root.configure(bg="#eef2f7")
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background="#eef2f7")
        style.configure("TLabel", background="#eef2f7", foreground="#19283b", font=("Sans", 10))
        style.configure("Treeview", rowheight=34, font=("Sans", 10), background="white", fieldbackground="white")
        style.configure("Treeview.Heading", font=("Sans", 10, "bold"), padding=8)
        style.configure("TButton", padding=(14, 9), font=("Sans", 10))
        style.configure("Accent.TButton", background="#1967c7", foreground="white")
        top = tk.Frame(root, bg="#17283e", padx=26, pady=20)
        top.pack(fill="x")
        tk.Label(top, text="Open DLL Loader", font=("Sans", 22, "bold"), bg="#17283e", fg="white").pack(anchor="w")
        tk.Label(top, text="PROTON GAMES   /   Open the loader in your game's Steam environment",
                 font=("Sans", 10), bg="#17283e", fg="#bed1e8").pack(anchor="w", pady=(6, 0))
        content = ttk.Frame(root, padding=(24, 16))
        content.pack(fill="both", expand=True)
        controls = ttk.Frame(content)
        controls.pack(fill="x", pady=(0, 12))
        self.search, self.running_only, self.arch = tk.StringVar(), tk.BooleanVar(), tk.StringVar(value="x64")
        entry = ttk.Entry(controls, textvariable=self.search, font=("Sans", 11))
        entry.pack(side="left", fill="x", expand=True, padx=(0, 12))
        entry.insert(0, "")
        ttk.Checkbutton(controls, text="Running only", variable=self.running_only, command=self.render).pack(side="left", padx=(0, 12))
        ttk.Button(controls, text="Refresh", command=self.refresh).pack(side="left")
        self.search.trace_add("write", lambda *_: self.render())
        table_frame = ttk.Frame(content)
        table_frame.pack(fill="both", expand=True)
        self.table = ttk.Treeview(table_frame, columns=("state", "name", "appid", "steam"), show="headings", selectmode="browse")
        for key, label, width in (("state", "Status", 110), ("name", "Game", 330), ("appid", "App ID", 100), ("steam", "Steam", 160)):
            self.table.heading(key, text=label)
            self.table.column(key, width=width, minwidth=75, stretch=key in ("name", "steam"))
        self.table.tag_configure("running", foreground="#08704c")
        self.table.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(table_frame, orient="vertical", command=self.table.yview)
        scrollbar.pack(side="right", fill="y")
        self.table.configure(yscrollcommand=scrollbar.set)
        self.table.bind("<<TreeviewSelect>>", lambda _: self.selection_changed())
        self.table.bind("<Double-1>", lambda _: self.open_loader())
        self.details = tk.StringVar(value="Select a game. Start it through Steam before opening the loader.")
        ttk.Label(content, textvariable=self.details, wraplength=875).pack(fill="x", pady=(14, 12))
        actions = ttk.Frame(content)
        actions.pack(fill="x")
        ttk.Label(actions, text="Target architecture").pack(side="left", padx=(0, 10))
        ttk.Combobox(actions, textvariable=self.arch, values=("x64", "x86"), width=7, state="readonly").pack(side="left")
        self.open_button = ttk.Button(actions, text="Open loader for this game", style="Accent.TButton", command=self.open_loader, state="disabled")
        self.open_button.pack(side="right")
        self.status = tk.StringVar(value="Discovering Steam libraries…")
        ttk.Label(content, textvariable=self.status, wraplength=875).pack(fill="x", pady=(14, 0))
        root.bind("<F5>", lambda _: self.refresh())
        if auto_refresh:
            self.refresh()
            self.root.after(150, self.poll)

    def refresh(self):
        if self.scanning:
            return
        self.scanning = True
        def worker():
            try:
                self.results.put((discover(self.extra), None))
            except Exception as exc:
                self.results.put((None, str(exc)))
        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        try:
            games, error = self.results.get_nowait()
            self.scanning = False
            if error:
                self.status.set(f"Discovery failed: {error}")
            else:
                self.games = games
                self.render()
            self.root.after(5000, self.refresh)
        except queue.Empty:
            pass
        try:
            child, log_path, error = self.launch_results.get_nowait()
            self.launching = False
            if error:
                from tkinter import messagebox
                messagebox.showerror("Cannot open loader", error, parent=self.root)
            else:
                self.children.append((child, log_path))
                self.status.set(f"Loader started. Log: {log_path}")
            self.selection_changed()
        except queue.Empty:
            pass
        alive = []
        for child, log_path in self.children:
            result = child.poll()
            if result is None:
                alive.append((child, log_path))
            elif result:
                from tkinter import messagebox
                messagebox.showerror("Protontricks could not open the loader",
                                     f"Protontricks exited with code {result}.\nLog: {log_path}", parent=self.root)
        self.children = alive
        self.root.after(150, self.poll)

    def render(self):
        previous = self.table.selection()
        selected = self.visible.get(previous[0]) if previous else None
        selected_key = selected.key if selected else None
        scroll = self.table.yview()[0]
        self.table.delete(*self.table.get_children())
        self.visible = {}
        query = self.search.get().casefold().strip()
        for game in self.games:
            if self.running_only.get() and not game.running:
                continue
            if query and query not in f"{game.name} {game.appid}".casefold():
                continue
            item = self.table.insert("", "end", values=("Running" if game.running else "Not running", game.name, game.appid,
                                     "Flatpak" if "/.var/app/" in str(game.steam) else "Native / custom"), tags=("running",) if game.running else ())
            self.visible[item] = game
            if game.key == selected_key:
                self.table.selection_set(item)
        self.table.yview_moveto(scroll)
        available = "Protontricks ready" if self.backend else "Install Protontricks to open a loader"
        self.status.set(f"{len(self.games)} games with existing prefixes · {sum(g.running for g in self.games)} running · Refresh: 5 seconds · {available}"
                        if self.games else f"No initialized Proton games found. Launch a Windows game once through Steam. {available}.")
        self.selection_changed()

    def selected_game(self):
        selected = self.table.selection()
        return self.visible.get(selected[0]) if selected else None

    def selection_changed(self):
        game = self.selected_game()
        if not game:
            self.details.set("Select a game. Start it through Steam before opening the loader.")
            self.open_button.configure(state="disabled")
            return
        self.details.set(f"{game.name}\nPrefix: {game.prefix}\n{game.note or 'Select the target process and DLL in the Windows loader after it opens.'}")
        self.open_button.configure(state="normal" if self.backend and game.launchable and game.running and not self.launching else "disabled")
        if not game.running and game.launchable:
            self.details.set(self.details.get() + "\nStart this game through Steam first.")

    def open_loader(self):
        game = self.selected_game()
        if not game or not game.launchable or not game.running or self.launching:
            return
        self.launching = True
        self.selection_changed()
        arch = self.arch.get()
        self.status.set(f"Checking {game.name}'s running prefix…")

        def worker():
            try:
                current = next((g for g in discover(self.extra) if g.key == game.key and g.running), None)
                if not current:
                    raise ValueError("The selected game stopped or changed prefix. Refresh and select it again.")
                log_path = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / f"open-dll-loader/proton-{game.appid}.log"
                child = launch(current, arch, self.payload_root, self.backend, log_path)
                self.launch_results.put((child, log_path, None))
            except Exception as exc:
                self.launch_results.put((None, None, str(exc)))

        threading.Thread(target=worker, daemon=True).start()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--steam-dir", action="append", default=[], help="Search only these Steam installations (libraries are read automatically)")
    parser.add_argument("--list", action="store_true", help="List discovered games without opening a window")
    parser.add_argument("--json", action="store_true", help="Print the list as JSON")
    parser.add_argument("--appid", type=int, help="Open the loader for one running game")
    parser.add_argument("--arch", choices=("x64", "x86"), default="x64")
    parser.add_argument("--payload-root", type=Path, help="Directory containing x64/ and x86/ Windows builds")
    args = parser.parse_args(argv)
    if args.list or args.json or args.appid is not None:
        games = discover(args.steam_dir)
        if args.appid is not None:
            matches = [game for game in games if game.appid == args.appid and game.running]
            if len(matches) != 1:
                parser.error("Select exactly one running game; use --steam-dir to resolve multiple Steam installations.")
            try:
                return launch(matches[0], args.arch, args.payload_root).wait()
            except (OSError, ValueError) as exc:
                parser.error(str(exc))
        if args.json:
            print(json.dumps([asdict(game) for game in games], default=str, indent=2))
        else:
            for game in games:
                print(f"{'Running' if game.running else 'Stopped'}\t{game.appid}\t{game.name}\t{game.prefix}")
        return 0
    try:
        import tkinter as tk
        root = tk.Tk()
        Application(root, args.steam_dir, args.payload_root)
        root.mainloop()
    except ImportError:
        parser.error("The game picker requires Tkinter (usually the python3-tk package). --list works without it.")
    except tk.TclError as exc:
        parser.error(f"Cannot open the game picker: {exc}. Run it from a Linux desktop, or use --list.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
