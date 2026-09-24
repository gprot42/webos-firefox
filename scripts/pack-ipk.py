#!/usr/bin/env python3
"""Pack app/ into a webOS IPK with real timestamps and executable bits.

Each run raises the patch number in app/appinfo.json first (0.1.4 -> 0.1.5);
pass --no-bump to repack at the current version.

Homebrew ares-package 2.4.0 on Node 22+ writes member dates of 1970-01-01.
webOS 5 has rejected those packages. This packer follows the same layout
ares-package writes for a native app.
"""

import io
import json
import sys
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "app"
DIST = ROOT / "dist"
SKIP_DIRS = {"profile"}
SKIP_SUFFIXES = {".log"}
# Marker files and backups that must never ship.
SKIP_NAMES = {".DS_Store", "wayland-debug", "env", "marionette"}


def ar_header(name: str, size: int) -> bytes:
    fields = [
        name.encode().ljust(16),
        str(int(time.time())).encode().ljust(12),
        b"0".ljust(6),
        b"0".ljust(6),
        b"100644".ljust(8),
        str(size).encode().ljust(10),
        b"`\n",
    ]
    header = b"".join(fields)
    if len(header) != 60:
        raise SystemExit(f"ar header for {name} is {len(header)} bytes")
    return header


def add_tree(tar: tarfile.TarFile, src: Path, arcname: str) -> int:
    total = 0
    info = tarfile.TarInfo(arcname)
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = int(src.stat().st_mtime)
    tar.addfile(info)
    for path in sorted(src.rglob("*")):
        rel = path.relative_to(src).as_posix()
        if any(part in SKIP_DIRS for part in path.relative_to(src).parts):
            continue
        if path.suffix in SKIP_SUFFIXES:
            continue
        if path.name in SKIP_NAMES or ".bak" in path.name:
            continue
        if path.is_symlink() and not path.exists():
            continue
        info = tarfile.TarInfo(f"{arcname}/{rel}")
        info.mtime = int(path.stat().st_mtime)
        if path.is_dir():
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            tar.addfile(info)
            continue
        data = path.read_bytes()
        info.size = len(data)
        if path.name in {"geckotv.sh", "smoke", "firefox", "gtk-hello"}:
            info.mode = 0o755
        else:
            info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
        tar.addfile(info, io.BytesIO(data))
        total += len(data)
    return total


def gzip_tar(build) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        build(tar)
    return buf.getvalue()


def bump_version() -> str:
    """Raise the last number of app/appinfo.json's version and save it.

    Every package gets a new version: webOS may skip installing a package
    whose version is already installed, leaving the old files in place.
    """
    path = APP / "appinfo.json"
    appinfo = json.loads(path.read_text())
    parts = appinfo["version"].split(".")
    if not all(p.isdigit() for p in parts):
        raise SystemExit(f"cannot bump version {appinfo['version']!r}")
    parts[-1] = str(int(parts[-1]) + 1)
    appinfo["version"] = ".".join(parts)
    path.write_text(json.dumps(appinfo, indent=2) + "\n")
    return appinfo["version"]


def main() -> None:
    if "--no-bump" in sys.argv[1:]:
        print("version not bumped (--no-bump)")
    else:
        print(f"version bumped to {bump_version()}")
    appinfo = json.loads((APP / "appinfo.json").read_text())
    app_id = appinfo["id"]
    version = appinfo["version"]
    if not (APP / "smoke").is_file():
        raise SystemExit("app/smoke is missing. Run make first.")
    if not (APP / "geckotv.sh").is_file():
        raise SystemExit("app/geckotv.sh is missing.")

    def build_data(tar: tarfile.TarFile) -> None:
        for directory in (
            "usr",
            "usr/palm",
            "usr/palm/applications",
            "usr/palm/packages",
        ):
            info = tarfile.TarInfo(directory)
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            info.mtime = int(time.time())
            tar.addfile(info)
        add_tree(tar, APP, f"usr/palm/applications/{app_id}")
        pkg = {
            "id": app_id,
            "version": version,
            "app": app_id,
        }
        payload = (json.dumps(pkg, indent=2) + "\n").encode()
        info = tarfile.TarInfo(f"usr/palm/packages/{app_id}/packageinfo.json")
        info.size = len(payload)
        info.mode = 0o644
        info.mtime = int(time.time())
        # Parent dir of the package id.
        parent = tarfile.TarInfo(f"usr/palm/packages/{app_id}")
        parent.type = tarfile.DIRTYPE
        parent.mode = 0o755
        parent.mtime = int(time.time())
        tar.addfile(parent)
        tar.addfile(info, io.BytesIO(payload))

    data = gzip_tar(build_data)
    installed = sum(
        path.stat().st_size
        for path in APP.rglob("*")
        if path.is_file()
        and path.suffix not in SKIP_SUFFIXES
        and "profile" not in path.parts
    )

    control_text = "\n".join(
        [
            f"Package: {app_id}",
            f"Version: {version}",
            "Section: misc",
            "Priority: optional",
            "Architecture: arm",
            f"Installed-Size: {installed}",
            "Maintainer: N/A <nobody@example.com>",
            "Description: This is a webOS application.",
            "webOS-Package-Format-Version: 2",
            "webOS-Packager-Version: webos-firefox",
            "",
        ]
    ).encode()

    def build_control(tar: tarfile.TarFile) -> None:
        info = tarfile.TarInfo("control")
        info.size = len(control_text)
        info.mode = 0o644
        info.mtime = int(time.time())
        tar.addfile(info, io.BytesIO(control_text))

    control = gzip_tar(build_control)
    debian = b"2.0\n"
    DIST.mkdir(exist_ok=True)
    ipk = DIST / f"{app_id}_{version}_arm.ipk"
    with ipk.open("wb") as out:
        out.write(b"!<arch>\n")
        for name, blob in (
            ("debian-binary", debian),
            ("control.tar.gz", control),
            ("data.tar.gz", data),
        ):
            out.write(ar_header(name, len(blob)))
            out.write(blob)
            if len(blob) % 2:
                out.write(b"\n")
    print(ipk)


if __name__ == "__main__":
    main()
