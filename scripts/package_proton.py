"""Build and verify a Linux game-picker archive containing both Windows loaders."""
import argparse
import hashlib
import io
from pathlib import Path
import re
import tarfile
import zipfile

from checksums import verify_archive


def package(windows, output):
    root = Path(__file__).resolve().parents[1]
    files = {}
    for arch in ("x64", "x86"):
        archive = windows / f"OpenDllLoader-windows-{arch}.zip"
        verify_archive(archive, arch)
        with zipfile.ZipFile(archive) as source:
            for name in source.namelist():
                parts = name.split("/")
                if len(parts) == 2 and parts[0] == arch and Path(name).suffix in (".exe", ".dll"):
                    files["windows/" + name] = source.read(name)
    for dest, source in (("odl-proton", "linux/odl-proton"), ("odl_proton.py", "linux/odl_proton.py"),
                         ("README.md", "linux/README.md"), ("LICENSE", "LICENSE"),
                         ("docs/linux-wine-proton.md", "docs/linux-wine-proton.md"),
                         ("docs/proton-picker.png", "docs/proton-picker.png")):
        path = root / source
        if dest.endswith(".png") and not path.exists():
            continue
        files[dest] = path.read_bytes()
    version = re.search(r"project\(OpenDllLoader VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text()).group(1)
    files["VERSION"] = (version + "\n").encode()
    files["SHA256SUMS.txt"] = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(files.items())).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz") as archive:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o755 if name == "odl-proton" else 0o644
            archive.addfile(info, io.BytesIO(data))
    with tarfile.open(output, "r:gz") as archive:
        for line in archive.extractfile("SHA256SUMS.txt").read().decode().splitlines():
            expected, name = line.split("  ", 1)
            if hashlib.sha256(archive.extractfile(name).read()).hexdigest() != expected:
                raise ValueError(f"Packaged checksum mismatch: {name}")
        if not archive.getmember("odl-proton").mode & 0o111:
            raise ValueError("The Linux launcher must be executable")
    print(f"Verified {len(files)} files in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.windows, args.output)
