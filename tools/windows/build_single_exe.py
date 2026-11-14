#!/usr/bin/env python3
"""Append a validated Windows release to the native single-download launcher."""

import argparse
import hashlib
import stat
import struct
import sys
import tarfile
from pathlib import Path
from zipfile import BadZipFile, ZipFile

from validate_package import CHUNK, manifest_files, sha_file, validate


MAGIC = b"EOWBOOT1"
ENTRY = struct.Struct("<HHQ32s")
FOOTER = struct.Struct("<8sQII32s")
ROOT = "EoWRecomp/"
MAX_FILES = 20_000
MAX_INDEX = 8 * 1024**2
MAX_PAYLOAD = 1024**3
MAX_PATH = 1023


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_bootstrap(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and not (getattr(info, "st_file_attributes", 0) &
            stat.FILE_ATTRIBUTE_REPARSE_POINT), "Bootstrap must be an ordinary file")
    with path.open("rb") as stub:
        header = stub.read(64)
        require(len(header) == 64 and header[:2] == b"MZ", "Bootstrap must be an x64 Windows PE executable")
        position, = struct.unpack_from("<I", header, 60)
        require(position < 1024**2, "Invalid bootstrap PE header offset")
        stub.seek(position)
        require(stub.read(6) == b"PE\0\0\x64\x86", "Bootstrap must be an x64 Windows PE executable")
        if info.st_size >= FOOTER.size:
            stub.seek(-FOOTER.size, 2)
            require(stub.read(len(MAGIC)) != MAGIC, "Bootstrap already has an embedded runtime")
    require(info.st_size <= 16 * 1024**2, "Bootstrap stub is too large")


def index_for(files):
    names = sorted(files, key=lambda name: name.encode("utf-16-be"))
    require(0 < len(names) <= MAX_FILES and ROOT + "eowrecomp.exe" in files,
            "Payload must include the root eowrecomp.exe and fit the file limit")
    index = bytearray()
    content_size = 0
    for name in names:
        size, sha256 = files[name]
        relative = name[len(ROOT):]
        raw_name = relative.encode("utf-8")
        require(relative.count("/") < 64 and len(relative.encode("utf-16-le")) // 2 <= MAX_PATH and len(raw_name) <= 3 * MAX_PATH,
                f"Payload path is too long: {name}")
        index += ENTRY.pack(len(raw_name), 0, size, bytes.fromhex(sha256))
        index += raw_name
        content_size += size
    require(len(index) <= MAX_INDEX and content_size + len(index) <= MAX_PAYLOAD, "Payload is too large")
    return names, index, content_size


def assemble(args):
    check_bootstrap(args.bootstrap)
    report = validate(args.zip, args.manifest)
    if args.zip_sha256:
        require(report["archive_sha256"] == args.zip_sha256.lower(), "Release ZIP SHA-256 mismatch")
    if args.manifest_sha256:
        require(report["manifest_sha256"] == args.manifest_sha256.lower(), "Release manifest SHA-256 mismatch")
    files, _ = manifest_files(args.manifest)
    names, index, content_size = index_for(files)
    identity = hashlib.sha256(index).digest()
    with args.output.open("xb") as output:
        try:
            with args.bootstrap.open("rb") as stub:
                for chunk in iter(lambda: stub.read(CHUNK), b""):
                    output.write(chunk)
            output.write(index)
            with ZipFile(args.zip) as archive:
                for name in names:
                    size, hash_hex = files[name]
                    digest = hashlib.sha256()
                    copied = 0
                    with archive.open(name) as source:
                        for chunk in iter(lambda: source.read(CHUNK), b""):
                            copied += len(chunk)
                            require(copied <= size, f"Payload entry exceeded manifest size: {name}")
                            digest.update(chunk)
                            output.write(chunk)
                    require(copied == size and digest.hexdigest() == hash_hex, f"Payload changed: {name}")
            require(sha_file(args.zip) == report["archive_sha256"] and
                    sha_file(args.manifest) == report["manifest_sha256"],
                    "Validated release ZIP or manifest changed while embedding")
            output.write(FOOTER.pack(MAGIC, len(index) + content_size, len(index), len(names), identity))
        except BaseException:
            output.close()
            args.output.unlink()
            raise
    print(f"Embedded {len(names)} files, runtime ID {identity.hex()}, into {str(args.output)!a}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap", type=Path, required=True)
    parser.add_argument("--zip", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--zip-sha256")
    parser.add_argument("--manifest-sha256")
    args = parser.parse_args()
    try:
        assemble(args)
    except (OSError, ValueError, struct.error, BadZipFile, tarfile.TarError, RuntimeError, EOFError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
