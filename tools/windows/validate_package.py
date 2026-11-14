"""Validate a public EoWRecomp ZIP and its fresh extraction against an exact file manifest."""
import argparse
from collections import Counter
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tarfile
import tempfile
import unicodedata
import zipfile


ROOT = "EoWRecomp"
CHUNK = 1024 * 1024
NESTED_ARCHIVE_LIMIT = 32 * CHUNK
NESTED_CONTENT_LIMIT = 64 * CHUNK
NESTED_ENTRY_LIMIT = 10_000
ARCHIVE_SUFFIX = (".zip", ".whl", ".jar", ".tar", ".gz", ".tgz", ".xz", ".bz2", ".7z", ".rar")
ARCHIVE_MAGIC = (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08", b"\x1f\x8b", b"\xfd7zXZ\x00",
                 b"BZh", b"7z\xbc\xaf\x27\x1c", b"Rar!\x1a\x07")
INVALID = re.compile(r'[\x00-\x1f\x7f<>:"|?*\\]')
DOS_DEVICE = re.compile(r"(?i)^(?:con|prn|aux|nul|conin\$|conout\$|(?:com|lpt)[1-9¹²³])$")
GAME_SUFFIX = (".nsp", ".xci", ".nca", ".nsz", ".xcz", ".nso", ".nro", ".nrr", ".bntx",
               ".bffnt", ".bfttf", ".msbt", ".bfsha", ".bfres", ".bars", ".bfsar", ".sarc",
               ".spv", ".spvcache", ".sav")
GAME_PART = {"exefs", "romfs", "extracted", "generated", "savedata", "shadercache",
             "shader-cache", ".gamebuild", ".extract", "xci_secure", "nso_raw", "update_pfs",
             "vk_pipelines.bin", "vk_pipeline_replay.bin", "romfs.bin", "romfs.src", "eowgame.dll", "eowgame.dll.id",
             "prod.keys", "title.keys", "save.meta", "eowrecomp.ini", "eowrecomp.log"}
SIGNATURES = {b"PFS0": "PFS0 game package", b"HFS0": "HFS0 game container",
              b"NSO0": "NSO game module", b"NRO0": "NRO module", b"NCA0": "NCA game content",
              b"NCA1": "NCA game content", b"NCA2": "NCA game content", b"NCA3": "NCA game content",
              b"BNTX": "BNTX texture archive", b"FFNT": "Nintendo bitmap font", b"FRES": "Nintendo model archive",
              b"FSHA": "Nintendo shader archive", b"\x03\x02\x23\x07": "raw SPIR-V"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse_path(name, directory=False):
    require(isinstance(name, str) and name and len(name) <= 4096, "invalid ZIP/manifest path")
    require(directory == name.endswith("/"), f"unexpected directory separator in {name!r}")
    trimmed = name[:-1] if directory else name
    parts = trimmed.split("/")
    require(parts[0] == ROOT and (directory or len(parts) > 1), f"path is outside {ROOT}/: {name!r}")
    for part in parts:
        require(part and part not in (".", "..") and not INVALID.search(part)
                and not part.endswith((" ", ".")) and len(part.encode("utf-16-le"))//2 <= 255,
                f"unsafe Windows path component in {name!r}")
        stem = part.split(".", 1)[0].rstrip(" ")
        require(not DOS_DEVICE.fullmatch(stem), f"Windows device name in {name!r}")
    return trimmed, parts


def folded(path):
    return unicodedata.normalize("NFKC", path).casefold()


def reject_game_path(name, parts):
    lower = [part.casefold() for part in parts[1:]]
    for part in lower:
        require(part not in GAME_PART and not part.endswith(GAME_SUFFIX + (".keys", ".log")),
                f"game material path {name!r}")
        require(not re.fullmatch(r"save_\d+|slot\d{2,}|file\d{2,}", part),
                f"game save material path {name!r}")
    if lower:
        require(lower[0] not in {"build", "cache", "keys", "saves", "seeds", "logs", "abnahme"},
                f"private runtime/development path {name!r}")


class Namespace:
    """Track explicit entries and the spelling of all implied directories."""
    def __init__(self):
        self.paths = {}
        self.explicit = set()

    def add(self, name, directory):
        key, parts = parse_path(name, directory)
        reject_game_path(name, parts)
        require(key not in self.explicit, f"duplicate entry {name!r}")
        self.explicit.add(key)
        for i in range(1, len(parts) + 1):
            prefix = "/".join(parts[:i])
            norm = folded(prefix)
            kind = i < len(parts) or directory
            old = self.paths.setdefault(norm, (prefix, kind))
            require(old == (prefix, kind), f"Windows path collision: {old[0]!r} versus {prefix!r}")
        return key


def no_duplicate_json_members(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON member {key!r}")
        result[key] = value
    return result


def manifest_files(path):
    with path.open(encoding="utf-8-sig") as stream:
        obj = json.load(stream, object_pairs_hook=no_duplicate_json_members)
    rows = obj.get("entries") if isinstance(obj, dict) else obj
    require(isinstance(rows, list) and rows, "manifest must be a nonempty file array or an object with entries")
    namespace = Namespace()
    files = {}
    for row in rows:
        require(isinstance(row, dict) and {"path", "bytes", "sha256"} <= row.keys(), "invalid manifest row")
        name = namespace.add(row["path"], False)
        length = row["bytes"]
        sha = row["sha256"]
        require(type(length) is int and length >= 0, f"invalid size for {name!r}")
        require(isinstance(sha, str) and re.fullmatch(r"[0-9a-fA-F]{64}", sha), f"invalid SHA-256 for {name!r}")
        files[name] = (length, sha.lower())
    ancestors = {name for name, directory in namespace.paths.values() if directory}
    return files, ancestors


def game_signature(prefix):
    signature = SIGNATURES.get(prefix[:4])
    if signature:
        return signature
    if prefix[:8] == b"MsgStdBn":
        return "Nintendo message data"
    if prefix[:8] == b"EOWVKC1\x00":
        return "local Vulkan pipeline cache"
    if prefix[:8] == b"EOWVKRP\x00":
        return "local Vulkan pipeline recipes"
    if prefix[:8] == b"OWSPIRV2":
        return "local SPIR-V cache"
    if prefix[0x10:0x14] == b"NRO0":
        return "NRO module"
    if prefix[0x200:0x204] in {b"NCA0", b"NCA1", b"NCA2", b"NCA3"}:
        return "NCA game content"
    if prefix[0x100:0x104] == b"HEAD":
        return "XCI game cartridge header"
    return None


def archive_material(name, prefix):
    return (name.casefold().endswith(ARCHIVE_SUFFIX) or prefix.startswith(ARCHIVE_MAGIC)
            or prefix[257:263] in (b"ustar\x00", b"ustar "))


def read_stream(stream, size, name, *, sha=None, mirror=None, forbid_archives=False):
    digest = hashlib.sha256() if sha is not None else None
    read = 0
    while data := stream.read(min(CHUNK, size-read+1)):
        if not read:
            signature = game_signature(data[:0x204])
            require(not signature, f"game material signature {signature} in {name!r}")
            require(not forbid_archives or not archive_material(name, data), f"unexpected archive {name!r}")
        read += len(data)
        require(read <= size, f"file exceeds manifest size {name!r}")
        if digest is not None:
            digest.update(data)
        if mirror is not None:
            mirror.write(data)
    require(read == size, f"file size differs from manifest {name!r}")
    if digest is not None:
        require(digest.hexdigest() == sha, f"SHA-256 differs from manifest {name!r}")


def verify_zip_entry(item, directory):
    name = item.filename
    require(item.orig_filename == name, f"noncanonical ZIP entry name {item.orig_filename!r}")
    require(not item.flag_bits & 1, f"encrypted ZIP entry {name!r}")
    win_flags = item.external_attr & 0xFFFF
    require(not win_flags & 0x440, f"device or reparse-point ZIP entry {name!r}")
    unix_type = stat.S_IFMT(item.external_attr >> 16)
    require(unix_type in (0, stat.S_IFDIR if directory else stat.S_IFREG),
            f"symlink or nonregular ZIP entry {name!r}")
    require(not (win_flags & 0x10) or directory, f"directory attribute on ZIP file {name!r}")
    require(item.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED),
            f"unsupported ZIP compression method {name!r}")
    if directory:
        require(item.file_size == 0 and item.CRC == 0, f"nonempty ZIP directory {name!r}")


def verify_zip_layout(archive):
    stream = archive.fp
    stream.seek(-22, os.SEEK_END)
    require(stream.read(4) == b"PK\x05\x06" and not archive.comment,
            "ZIP has trailing data or an archive comment")
    require(archive.infolist() and min(item.header_offset for item in archive.infolist()) == 0,
            "ZIP has leading data or no local entries")


def verify_zip_directory(archive, item):
    with archive.open(item) as stream:
        require(not stream.read(1), f"nonempty ZIP directory {item.filename!r}")


def verify_nested_members(archive_name, entries):
    namespace = Namespace()
    count = total = files = 0
    for name, directory, size, open_entry in entries:
        count += 1
        total += size
        require(count <= NESTED_ENTRY_LIMIT and 0 <= size and total <= NESTED_CONTENT_LIMIT,
                f"nested archive exceeds inspection limits: {archive_name!r}")
        namespace.add(f"{ROOT}/{name}", directory)
        if directory:
            continue
        require(not archive_material(name, b""), f"unexpected archive {archive_name}!{name}")
        with open_entry() as member:
            read_stream(member, size, f"{archive_name}!{name}", forbid_archives=True)
        files += 1
    require(files > 0, f"empty nested archive {archive_name!r}")
    return files


def nested_zip_entries(archive):
    for item in archive.infolist():
        directory = item.is_dir()
        verify_zip_entry(item, directory)
        if directory:
            verify_zip_directory(archive, item)
        yield item.filename, directory, item.file_size, lambda: archive.open(item)


def nested_tar_entries(archive):
    for item in archive:
        directory = item.isdir()
        require((directory and item.size == 0) or item.type in (tarfile.REGTYPE, tarfile.AREGTYPE),
                f"link, device or nonregular TAR entry {item.name!r}")
        name = item.name.rstrip("/") + "/" if directory else item.name
        yield name, directory, item.size, lambda: archive.extractfile(item)
        end = item.offset_data + item.size
        padding_size = -end % tarfile.BLOCKSIZE
        archive.fileobj.seek(end)
        padding = archive.fileobj.read(padding_size)
        require(len(padding) == padding_size and not padding.strip(b"\x00"),
                f"invalid TAR padding after {item.name!r}")


def verify_nested(archive_name, kind, stream):
    if kind == "zip":
        with zipfile.ZipFile(stream) as archive:
            verify_zip_layout(archive)
            return verify_nested_members(archive_name, nested_zip_entries(archive))

    with gzip.GzipFile(fileobj=stream, mode="rb") as compressed, tempfile.SpooledTemporaryFile(max_size=CHUNK) as tar:
        while data := compressed.read(CHUNK):
            require(tar.tell() + len(data) <= NESTED_CONTENT_LIMIT,
                    f"nested archive exceeds inspection limits: {archive_name!r}")
            tar.write(data)
        tar.seek(0)
        with tarfile.open(fileobj=tar, mode="r:") as archive:
            files = verify_nested_members(archive_name, nested_tar_entries(archive))
            tail_size = tar.seek(0, os.SEEK_END) - archive.offset
            require(tail_size >= 2 * tarfile.BLOCKSIZE and tail_size % tarfile.BLOCKSIZE == 0,
                    f"missing or incomplete end blocks in TAR {archive_name!r}")
            tar.seek(archive.offset)
            while data := tar.read(CHUNK):
                require(not data.strip(b"\x00"), f"data after end of TAR {archive_name!r}")
            return files


def nested_kind(name):
    if name == f"{ROOT}/eowgen/_internal/base_library.zip":
        return "zip"
    if re.fullmatch(rf"{ROOT}/licenses/generator/unicorn-[\d.]+\.tar\.gz", name):
        return "tar.gz"
    return None


def zip_files(path, expected, ancestors):
    namespace = Namespace()
    files = {}
    directories = set()
    methods = Counter()
    nested = {}
    with zipfile.ZipFile(path, "r") as archive:
        verify_zip_layout(archive)
        for item in archive.infolist():
            directory = item.is_dir()
            verify_zip_entry(item, directory)
            name = namespace.add(item.filename, directory)
            if directory:
                require(name in ancestors, f"unexpected ZIP directory {name!r}")
                verify_zip_directory(archive, item)
                directories.add(name)
            else:
                require(name in expected, f"unexpected ZIP file {name!r}")
                require(item.file_size == expected[name][0], f"ZIP entry size differs from manifest {name!r}")
                files[name] = item
                methods[str(item.compress_type)] += 1
        require(files.keys() == expected.keys(),
                f"missing ZIP manifest files: {sorted(expected.keys()-files.keys())[:12]}")
        for name, item in files.items():
            size, sha = expected[name]
            kind = nested_kind(name)
            with archive.open(item) as stream:
                if kind is None:
                    require(not archive_material(name, b""), f"unexpected archive {name!r}")
                    read_stream(stream, size, name, sha=sha, forbid_archives=True)
                else:
                    require(size <= NESTED_ARCHIVE_LIMIT, f"nested archive exceeds inspection limits: {name!r}")
                    with tempfile.SpooledTemporaryFile(max_size=CHUNK) as inner:
                        read_stream(stream, size, name, sha=sha, mirror=inner)
                        inner.seek(0)
                        nested[name] = verify_nested(name, kind, inner)
    return len(directories), dict(methods), nested


def verify_extraction(parent, expected, ancestors):
    root_info = parent.lstat()
    require(stat.S_ISDIR(root_info.st_mode), f"invalid extraction parent {parent}")
    require(not getattr(root_info, "st_file_attributes", 0) & 0x400,
            f"extraction parent is a reparse point {parent}")
    ns = Namespace()
    visited = set()
    dirs = set()
    stack = [(parent, "")]
    while stack:
        current, prefix = stack.pop()
        with os.scandir(current) as listing:
            for entry in listing:
                name = prefix + entry.name
                info = entry.stat(follow_symlinks=False)
                attributes = getattr(info, "st_file_attributes", 0)
                require(not attributes & 0x400 and not stat.S_ISLNK(info.st_mode),
                        f"extraction contains a link or reparse point {name!r}")
                if stat.S_ISDIR(info.st_mode):
                    ns.add(name + "/", True)
                    require(name in ancestors, f"unexpected extraction directory {name!r}")
                    dirs.add(name)
                    stack.append((Path(entry.path), name + "/"))
                else:
                    require(stat.S_ISREG(info.st_mode), f"nonregular extracted file {name!r}")
                    ns.add(name, False)
                    require(name in expected, f"unexpected extracted file {name!r}")
                    require(info.st_size == expected[name][0], f"extracted size differs from manifest {name!r}")
                    with open(entry.path, "rb") as stream:
                        # DirEntry.stat on Windows may report zero links; use
                        # the open handle's metadata for the file actually read.
                        require(os.fstat(stream.fileno()).st_nlink == 1, f"hardlinked extracted file {name!r}")
                        size, sha = expected[name]
                        read_stream(stream, size, name, sha=sha)
                    visited.add(name)
    require(visited == expected.keys(), f"missing extracted manifest files: {sorted(expected.keys()-visited)[:12]}")
    require(dirs == ancestors, f"missing extracted manifest directories: {sorted(ancestors-dirs)[:12]}")
    return len(dirs)


def sha_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate(archive, manifest, extracted=None):
    expected, ancestors = manifest_files(manifest)
    directory_count, methods, nested = zip_files(archive, expected, ancestors)
    extracted_dirs = verify_extraction(extracted, expected, ancestors) if extracted is not None else None
    return dict(result="pass", archive=str(archive.resolve()), archive_sha256=sha_file(archive),
                archive_bytes=archive.stat().st_size, manifest=str(manifest.resolve()),
                manifest_sha256=sha_file(manifest), files=len(expected), file_bytes=sum(size for size,_ in expected.values()),
                zip_explicit_directories=directory_count, compression_methods=methods,
                nested_archive_files=nested,
                extraction_verified=extracted is not None,
                extraction_parent=str(extracted.resolve()) if extracted is not None else None,
                extraction_directories=extracted_dirs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--extracted", type=Path, help=f"fresh parent containing only {ROOT}/")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = validate(args.zip, args.manifest, args.extracted)
    text = json.dumps(result, indent=2)
    if args.report is not None:
        with args.report.open("x", encoding="utf-8") as stream:
            stream.write(text + "\n")
    print(text)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, UnicodeError, RuntimeError, EOFError, tarfile.TarError, zipfile.BadZipFile) as error:
        raise SystemExit(f"ZIP validation failed: {error}") from error
