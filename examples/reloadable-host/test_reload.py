"""Lifecycle regression tests. Runs only new, source-built example hosts."""
import argparse
import pathlib
import queue
import re
import shutil
import subprocess
import tempfile
import threading
import time


class Session:
    def __init__(self, host, plugin, watch=False):
        self.lines = queue.Queue()
        self.seen = []
        args = [str(host), "--plugin", str(plugin)] + (["--watch"] if watch else [])
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, bufsize=1, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        ready = self.wait("HOST_READY")
        self.host_pid = int(re.search(r"pid=(\d+)", ready).group(1))

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.strip())

    def send(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def wait(self, text, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=0.1)
            except queue.Empty:
                continue
            self.seen.append(line)
            if text in line:
                return line
        raise AssertionError(f"Missing {text!r}. Output: {self.seen}")

    def status(self):
        self.send("status")
        line = self.wait("STATUS active=")
        return {k: int(v) for k, v in re.findall(r"(active|generation|ticks)=(\d+)", line)}

    def close(self):
        try:
            if self.process.poll() is None:
                self.send("quit")
            self.process.wait(timeout=5)
        except BrokenPipeError:
            self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.terminate()
                self.process.wait(timeout=5)
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass
            self.reader.join(timeout=1)
            self.process.stdout.close()


def main():
    parser = argparse.ArgumentParser()
    for name in ["host", "v1", "v2", "refuse"]:
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    parser.add_argument("--client", type=pathlib.Path)
    args = parser.parse_args()
    checks = 0

    def require(condition, label):
        nonlocal checks
        assert condition, label
        checks += 1
        print("PASS:", label, flush=True)

    with tempfile.TemporaryDirectory(prefix="odl-reload-test-") as folder:
        source = pathlib.Path(folder) / ("active" + args.v1.suffix)
        shutil.copyfile(args.v1, source)
        session = Session(args.host, source)
        try:
            initial = session.status()
            require(initial["active"] == 1 and initial["generation"] == 1, "initial plugin starts")
            shutil.copyfile(args.v2, source)
            require(source.read_bytes() == args.v2.read_bytes(), "source binary can be replaced while plugin is loaded")
            session.send("reload")
            session.wait("Reloaded generation=2 name=Example plugin revision 2")
            after = session.status()
            require(after["active"] == 1 and after["ticks"] >= initial["ticks"], "hot reload preserves host state")
            source.write_bytes(b"not a shared library")
            session.send("reload")
            session.wait("Reload failed before stopping the active plugin")
            failed = session.status()
            require(failed["active"] == 1 and failed["generation"] == 2, "invalid replacement leaves previous plugin running")
            session.send("unload")
            session.wait("Plugin unloaded. Host is still running.")
            unloaded = session.status()
            require(unloaded["active"] == 0 and session.process.poll() is None, "unload keeps the host alive")
            shutil.copyfile(args.v1, source)
            session.send("reload")
            session.wait("Reloaded generation=3")
            require(session.status()["active"] == 1, "reload works after unloading")
        finally:
            session.close()
        if args.client:
            shutil.copyfile(args.v1, source)
            session = Session(args.host, source, watch=True)
            try:
                def client(command):
                    result = subprocess.run([str(args.client), command, str(session.host_pid)], capture_output=True, text=True, timeout=8,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                    if result.returncode:
                        while True:
                            try:
                                session.seen.append(session.lines.get_nowait())
                            except queue.Empty:
                                break
                        raise AssertionError(result.stdout + result.stderr + f"\nHost alive={session.process.poll() is None}; output={session.seen[-30:]}")
                    return result.stdout
                require("active=1" in client("--host-status"), "GUI/CLI protocol discovers a cooperating host")
                shutil.copyfile(args.v2, source)
                require("name=Example plugin revision 2" in client("--reload"), "cooperative reload command works over the control pipe")
                require("active=0" in client("--unload"), "cooperative unload command works over the control pipe")
                shutil.copyfile(args.v1, source)
                time.sleep(0.8)
                require("active=0" in client("--host-status"), "control-pipe unload pauses watch mode")
            finally:
                session.close()
        shutil.copyfile(args.v1, source)
        session = Session(args.host, source, watch=True)
        try:
            shutil.copyfile(args.v2, source)
            session.wait("Reloaded generation=2 name=Example plugin revision 2")
            require(session.status()["generation"] == 2, "watch mode reloads a stable rebuilt binary")
            session.send("unload")
            session.wait("Plugin unloaded.")
            shutil.copyfile(args.v1, source)
            time.sleep(0.8)
            require(session.status()["active"] == 0, "unload pauses watch mode")
        finally:
            session.close()
        shutil.copyfile(args.refuse, source)
        session = Session(args.host, source)
        try:
            session.send("unload")
            session.wait("Unload refused; the current plugin remains active.")
            require(session.status()["active"] == 1, "stop refusal prevents unsafe unloading")
            shutil.copyfile(args.v2, source)
            session.send("reload")
            session.wait("Unload refused; the current plugin remains active.")
            require(session.status()["generation"] == 1, "stop refusal prevents replacing the active module")
        finally:
            session.close()
    print(f"{checks} cooperative lifecycle checks passed.")


if __name__ == "__main__":
    main()
