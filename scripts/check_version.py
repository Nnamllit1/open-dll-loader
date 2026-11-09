import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
version = re.search(r"project\(OpenDllLoader VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text()).group(1)
ref = sys.argv[1]
if ref.startswith("v") and ref != "v" + version:
    raise SystemExit(f"Tag {ref} does not match CMake version {version}")
print("Project version:", version)
