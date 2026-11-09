"""Write binary checksums and verify the actual Windows release archive."""

import argparse
import hashlib
from pathlib import Path
import zipfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_checksums(root):
    binaries = sorted(
        (p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in {".exe", ".dll"}),
        key=lambda p: p.relative_to(root).as_posix(),
    )
    if not binaries:
        raise ValueError("No binaries found to checksum")
    manifest = "".join(f"{digest(p.read_bytes())}  {p.relative_to(root).as_posix()}\n" for p in binaries)
    (root / "SHA256SUMS.txt").write_text(manifest, encoding="ascii", newline="\n")
    print(f"Wrote checksums for {len(binaries)} binaries")


def verify_archive(path, arch):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate archive entries")
        required = {
            f"{arch}/{name}"
            for name in (
                "OpenDllLoader.exe", "open-dll-loader.exe", "odl-demo-launcher.exe",
                "odl-plugin-host.exe", "reloadable-plugin-v1.dll", "reloadable-plugin-v2.dll",
            )
        } | {"README.md", "LICENSE", "SHA256SUMS.txt", "docs/hot-reload.md", "docs/linux-wine-proton.md"}
        missing = required - set(names)
        if missing:
            raise ValueError(f"Missing release files: {sorted(missing)}")
        binaries = {name for name in names if Path(name).suffix.lower() in {".exe", ".dll"}}
        verified = set()
        for line in archive.read("SHA256SUMS.txt").decode("ascii").splitlines():
            expected, name = line.split("  ", 1)
            if name in verified or name not in binaries:
                raise ValueError(f"Unexpected checksum entry: {name}")
            if digest(archive.read(name)) != expected:
                raise ValueError(f"Checksum mismatch: {name}")
            verified.add(name)
        if verified != binaries:
            raise ValueError("Checksums do not cover every packaged binary")
    print(f"Verified {len(verified)} packaged binaries in {path.name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    writer = commands.add_parser("write")
    writer.add_argument("root", type=Path)
    verifier = commands.add_parser("verify-zip")
    verifier.add_argument("archive", type=Path)
    verifier.add_argument("architecture", choices=("x64", "x86"))
    args = parser.parse_args()
    if args.command == "write":
        write_checksums(args.root)
    else:
        verify_archive(args.archive, args.architecture)
