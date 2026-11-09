import json
import os
from pathlib import Path, PurePosixPath
import tempfile
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

import odl_proton as odl


def prefix(library, appid):
    path = library / f"steamapps/compatdata/{appid}/pfx"
    (path / "drive_c").mkdir(parents=True)
    (path / "system.reg").write_text("WINE REGISTRY Version 2\n")
    (path.parent / "pfx.lock").touch()
    return path.resolve()


def manifest(library, appid, name):
    file = library / f"steamapps/appmanifest_{appid}.acf"
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(f'"AppState" {{ "appid" "{appid}" "name" {json.dumps(name, ensure_ascii=False)} }}', encoding="utf-8")


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.steam = self.home / ".local/share/Steam"
        (self.steam / "steamapps").mkdir(parents=True)

    def discover(self, sessions=()):
        return odl.discover(home=self.home, environ={}, sessions=sessions)

    def test_external_library_and_unlisted_prefix(self):
        external = self.home / ('Games "extra" $test' if os.name != "nt" else 'Games (extra) $test')
        first, second = prefix(self.steam, 101), prefix(external, 202)
        manifest(external, 202, "A Game (Deluxe)")
        folders = self.steam / "steamapps/libraryfolders.vdf"
        folders.write_text('"libraryfolders" { // Steam libraries\n"1" { "path" ' + json.dumps(str(external)) + ' } }')
        games = self.discover({odl.Session(second, 202, self.steam.resolve())})
        self.assertEqual([g.appid for g in games], [202, 101])
        self.assertEqual(games[0].name, "A Game (Deluxe)")
        self.assertTrue(games[0].running)
        self.assertTrue(games[0].launchable)
        self.assertEqual(games[1].prefix, first)
        self.assertEqual(games[1].name, "Steam app 101")

    def test_legacy_libraryfolders_and_broken_manifest(self):
        external = self.home / "legacy"
        prefix(external, 303)
        (self.steam / "steamapps/libraryfolders.vdf").write_text('"LibraryFolders" { "1" '+json.dumps(str(external))+' }')
        (external / "steamapps/appmanifest_303.acf").write_text('"AppState" { "name"')
        self.assertEqual(self.discover()[0].appid, 303)

    def test_multiple_prefixes_prefer_latest_lock_and_reject_wrong_running_prefix(self):
        old = prefix(self.steam, 404)
        external = self.home / "external"
        new = prefix(external, 404)
        os.utime(old.parent / "pfx.lock", (100, 100))
        os.utime(new.parent / "pfx.lock", (200, 200))
        (self.steam / "steamapps/libraryfolders.vdf").write_text('"libraryfolders" { "1" '+json.dumps(str(external))+' }')
        game = self.discover()[0]
        self.assertEqual(game.prefix, new)
        self.assertTrue(game.launchable)
        game = self.discover({odl.Session(old, 404, self.steam.resolve())})[0]
        self.assertTrue(game.running)
        self.assertFalse(game.launchable)

    def test_flatpak_and_native_installations_remain_distinct(self):
        flatpak = self.home / ".var/app/com.valvesoftware.Steam/data/Steam"
        prefix(self.steam, 505)
        prefix(flatpak, 505)
        games = self.discover()
        self.assertEqual(len(games), 2)
        self.assertNotEqual(games[0].key, games[1].key)
        scoped = odl.discover([flatpak], home=self.home, environ={}, sessions=())
        self.assertEqual(len(scoped), 1)
        self.assertEqual(scoped[0].steam, flatpak.resolve())

    def test_custom_running_prefix_is_visible_but_cannot_use_wrong_appid_prefix(self):
        custom = prefix(self.home / "custom", 606)
        game = self.discover({odl.Session(custom, 606, self.steam.resolve())})[0]
        self.assertTrue(game.running)
        self.assertFalse(game.launchable)
        with self.assertRaisesRegex(ValueError, "Custom"):
            odl.launch(game)

    def test_uninitialized_prefix_is_ignored(self):
        (self.steam / "steamapps/compatdata/707/pfx").mkdir(parents=True)
        self.assertEqual(self.discover(), [])

    @unittest.skipUnless(sys.platform == "linux", "Uses the Linux proc filesystem")
    def test_live_process_environment_marks_its_existing_prefix(self):
        pfx = prefix(self.steam, 777)
        env = {**os.environ, "WINEPREFIX": str(pfx), "STEAM_COMPAT_APP_ID": "777",
               "STEAM_COMPAT_CLIENT_INSTALL_PATH": str(self.steam.resolve())}
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"], env=env)
        try:
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                games = odl.discover([self.steam])
                if games and games[0].running:
                    break
                time.sleep(0.02)
            self.assertTrue(games[0].running)
            self.assertEqual(games[0].prefix, pfx)
        finally:
            child.terminate()
            child.wait(timeout=5)

    def test_running_scan_reads_only_matching_steam_environments(self):
        pfx = prefix(self.steam, 808)
        proc = self.home / "proc"
        (proc / "123").mkdir(parents=True)
        env = {"WINEPREFIX": str(pfx), "SteamAppId": "808", "STEAM_COMPAT_CLIENT_INSTALL_PATH": str(self.steam.resolve()), "SECRET": "do-not-retain"}
        (proc / "123/environ").write_bytes(b"\0".join(os.fsencode(k+"="+v) for k, v in env.items()))
        (proc / "124").mkdir()
        sessions = odl.running_sessions(proc)
        self.assertEqual(sessions, {odl.Session(pfx, 808, self.steam.resolve())})
        self.assertNotIn("do-not-retain", repr(sessions))

    def test_metadata_handles_quoted_braces_and_rejects_unclosed_data(self):
        file = self.home / "test.vdf"
        file.write_text('"x" { "name" "{hello}" "other" "\\\\folder" }')
        self.assertEqual(odl.read_vdf(file)["x"]["name"], "{hello}")
        file.write_text('"x" {')
        with self.assertRaises(ValueError):
            odl.read_vdf(file)

    def test_native_launch_keeps_paths_literal_and_clears_foreign_wine_overrides(self):
        pfx = prefix(self.steam, 909)
        payload = self.home / ('payload $test "quoted"' if os.name != "nt" else 'payload $test (quoted)')
        (payload / "x64").mkdir(parents=True)
        (payload / "x64/OpenDllLoader.exe").write_bytes(b"fixture, not executable")
        game = odl.Game(909, "Fixture", self.steam.resolve(), pfx, True)
        with patch.dict(os.environ, {"WINEPREFIX": "/wrong", "PROTON_VERSION": "wrong"}), patch.object(odl.subprocess, "Popen") as spawn:
            odl.launch(game, payload_root=payload, backend=odl.Backend("protontricks-launch"))
        args, kwargs = spawn.call_args
        self.assertEqual(args[0], ["protontricks-launch", "--no-term", "--appid", "909", str(payload / "x64/OpenDllLoader.exe")])
        self.assertNotIn("WINEPREFIX", kwargs["env"])
        self.assertNotIn("PROTON_VERSION", kwargs["env"])
        self.assertEqual(kwargs["env"]["STEAM_DIR"], str(self.steam.resolve()))
        self.assertEqual(kwargs["cwd"], payload / "x64")
        self.assertNotIn("shell", kwargs)

    def test_flatpak_backend_has_scoped_payload_access_and_explicit_steam_binding(self):
        game = odl.Game(10, "Fixture", self.steam.resolve(), Path("/prefix"), True)
        payload = PurePosixPath("/a folder/x64/OpenDllLoader.exe")
        command = odl.Backend("flatpak", True).command(game, payload)
        self.assertIn("--filesystem=/a folder/x64:ro", command)
        self.assertIn("--env=STEAM_DIR="+str(self.steam.resolve()), command)
        self.assertEqual(command[-3:], ["--appid", "10", str(payload)])

    def test_missing_backend_and_payload_report_clear_errors(self):
        game = odl.Game(1, "Fixture", self.steam.resolve(), prefix(self.steam, 1), True)
        with patch.object(odl, "find_backend", return_value=None):
            with self.assertRaisesRegex(ValueError, "Install Protontricks"):
                odl.launch(game)
        with self.assertRaisesRegex(ValueError, "Windows loader missing"):
            odl.launch(game, payload_root=self.home, backend=odl.Backend("fixture"))


if __name__ == "__main__":
    unittest.main()
