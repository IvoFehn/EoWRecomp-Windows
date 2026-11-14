#!/usr/bin/env python3
"""Assemble the Windows launcher ZIP from explicitly selected build products."""

import argparse
import hashlib
import json
import os
import stat
from pathlib import Path, PurePosixPath
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[2]
GAMELIB_SOURCES = (
    "runtime/cpu.h",
    "runtime/simd.h",
    "runtime/simd_fp.h",
    "runtime/gamelib.h",
    "runtime/modules.h",
    "tools/gamelib_entry.c",
    "runtime/hle/hle_symbols.txt",
)


def check_path(path):
    mode = path.lstat()
    reparse = getattr(mode, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    if reparse or stat.S_ISLNK(mode.st_mode):
        raise ValueError(f"Linked package input: {path}")
    return mode.st_mode


def tree(root, destination):
    if not stat.S_ISDIR(check_path(root)):
        raise ValueError(f"Expected directory: {root}")
    files = []
    for folder, directories, names in os.walk(root, onerror=raise_walk_error):
        folder = Path(folder)
        for name in directories:
            check_path(folder / name)
        for name in names:
            source = folder / name
            files.append((source, PurePosixPath(destination, source.relative_to(root).as_posix())))
    if not files:
        raise ValueError(f"Empty package input: {root}")
    return files


def raise_walk_error(error):
    raise error


def inputs(args):
    for name in ("eowrecomp.exe", "eowrecomp.dll.a"):
        yield args.release / name, name
    for dll in args.dll:
        if dll.suffix.lower() != ".dll":
            raise ValueError(f"Expected runtime DLL: {dll}")
        yield dll, dll.name

    yield args.generator / "eowgen.exe", "eowgen/eowgen.exe"
    yield from tree(args.generator / "_internal", "eowgen/_internal")
    for name in ("zig.exe", "LICENSE", "README.md"):
        yield args.zig / name, f"zig/{name}"
    yield from tree(args.zig / "lib", "zig/lib")

    for name in GAMELIB_SOURCES:
        yield ROOT / name, f"gamelib-src/{Path(name).name}"
    yield ROOT / "LICENSE", "LICENSE"
    yield ROOT / "docs/WINDOWS-START.md", "README.md"
    icon = ROOT / "docs/icon/icon_1024.png"
    if icon.is_file():
        yield icon, "icon.png"
    yield ROOT / "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", "Baloo2-SemiBold.ttf"
    yield ROOT / "third_party/fonts/Baloo2/OFL.txt", "licenses/Baloo2-OFL.txt"
    yield ROOT / "third_party/fonts/DejaVu/DejaVuSans.ttf", "DejaVuSans.ttf"
    yield ROOT / "third_party/fonts/DejaVu/LICENSE", "licenses/DejaVu-LICENSE.txt"
    yield from tree(args.runtime_notices, "licenses/runtime")
    yield from tree(args.generator_notices, "licenses/generator")


def archive_inputs(args):
    entries = {}
    for source, relative in inputs(args):
        path = PurePosixPath("EoWRecomp", relative).as_posix()
        key = path.casefold()
        if key in entries:
            raise ValueError(f"Duplicate package entry: {path}")
        if not stat.S_ISREG(check_path(source)):
            raise ValueError(f"Expected regular file: {source}")
        entries[key] = (source, path)
    return sorted(entries.values(), key=lambda entry: entry[1])


def write_entry(archive, source, path):
    digest = hashlib.sha256()
    size = 0
    with source.open("rb") as data, archive.open(path, "w", force_zip64=True) as output:
        for chunk in iter(lambda: data.read(1024 * 1024), b""):
            output.write(chunk)
            digest.update(chunk)
            size += len(chunk)
    return {"path": path, "bytes": size, "sha256": digest.hexdigest()}


def package(args):
    entries = archive_inputs(args)
    # Exclusive creation preserves any existing deliverable on a repeated invocation.
    with args.output.open("xb") as destination:
        try:
            with ZipFile(destination, "w", ZIP_DEFLATED, compresslevel=5) as archive:
                manifest = [write_entry(archive, source, path) for source, path in entries]
            with args.manifest.open("x", encoding="utf-8", newline="\n") as output:
                json.dump({"entries": manifest}, output, ensure_ascii=False, indent=2)
                output.write("\n")
        except BaseException:
            destination.close()
            args.output.unlink()
            raise
    print(f"Wrote {len(entries)} files to {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--zig", type=Path, required=True)
    parser.add_argument("--dll", type=Path, action="append", required=True)
    parser.add_argument("--runtime-notices", type=Path, required=True)
    parser.add_argument("--generator-notices", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        package(args)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
