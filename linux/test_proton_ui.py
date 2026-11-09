"""Game-picker checks with sample data; no Steam installation or target is used."""
import json
from pathlib import Path
import sys
import tkinter as tk
from unittest.mock import patch

import odl_proton as odl


def main(output):
    root = tk.Tk()
    checks = 0

    def require(condition, label):
        nonlocal checks
        if not condition:
            raise AssertionError(label)
        checks += 1
        print("PASS:", label)

    try:
        with patch.object(odl, "find_backend", return_value=odl.Backend("fixture-only")):
            app = odl.Application(root, auto_refresh=False)
        running = odl.Game(10001, "Example Adventure", Path("/home/player/.local/share/Steam"),
                           Path("/games/SteamLibrary/steamapps/compatdata/10001/pfx"), True)
        stopped = odl.Game(10002, "Workshop Sandbox", running.steam,
                           Path("/games/SteamLibrary/steamapps/compatdata/10002/pfx"), False)
        conflict = odl.Game(10003, "Prefix Conflict Example", running.steam,
                            Path("/games/SteamLibrary/steamapps/compatdata/10003/pfx"), True, False,
                            "Multiple prefixes: the running game differs from Steam's newest prefix.")
        app.games = [running, stopped, conflict]
        app.render()
        root.update()
        require(len(app.visible) == 3, "show discovered games")
        app.table.selection_set(next(iter(app.visible)))
        app.selection_changed()
        require(str(app.open_button["state"]) == "normal", "enable opening a running game's loader")
        require(str(running.prefix) in app.details.get(), "show selected prefix")
        app.render()
        require(app.selected_game() == running, "preserve selection across discovery refresh")
        app.search.set("workshop")
        require(len(app.visible) == 1 and next(iter(app.visible.values())) == stopped, "filter by name")
        app.table.selection_set(next(iter(app.visible)))
        app.selection_changed()
        require(str(app.open_button["state"]) == "disabled", "require the game to be running")
        app.search.set("10003")
        app.table.selection_set(next(iter(app.visible)))
        app.selection_changed()
        require(str(app.open_button["state"]) == "disabled", "block a conflicting running prefix")
        app.search.set("")
        app.running_only.set(True)
        app.render()
        require(len(app.visible) == 2, "filter to running games")
        app.table.selection_set(next(iter(app.visible)))
        app.backend = None
        app.selection_changed()
        require(str(app.open_button["state"]) == "disabled", "require an installed launch backend")
        app.backend = odl.Backend("fixture-only")
        app.running_only.set(False)
        app.render()
        app.table.selection_set(next(iter(app.visible)))
        app.selection_changed()
        root.geometry("780x570")
        root.update()
        bottom = root.winfo_rooty() + root.winfo_height()
        require(app.open_button.winfo_rooty() + app.open_button.winfo_height() <= bottom, "keep launch action visible at minimum size")
        require(app.status_label.winfo_rooty() + app.status_label.winfo_height() <= bottom, "keep status visible at minimum size")
        root.geometry("980x720")
        root.update()
        from PIL import ImageGrab
        ImageGrab.grab().crop((root.winfo_rootx(), root.winfo_rooty(),
                              root.winfo_rootx() + root.winfo_width(), root.winfo_rooty() + root.winfo_height())).save(output)
        Path(str(output) + ".json").write_text(json.dumps({"checks_passed": checks, "sample_data_only": True,
                                                         "launched_games": False}) + "\n")
        print(f"{checks} game-picker UI checks passed.")
    finally:
        root.destroy()


if __name__ == "__main__":
    main(Path(sys.argv[1]))
