"""Check Git-tracked files before building the Windows source distribution."""

import re
from pathlib import Path, PurePosixPath
import subprocess


GAME_DIRS = {"gamefiles", "exefs", "romfs", "extracted", "generated", "savedata", "save", "cache", "build"}
GAME_ENDINGS = {".xci", ".xcz", ".nsp", ".nsz", ".nca", ".ncz", ".nso", ".nro", ".npdm", ".keys", ".tik",
                ".bfres", ".bntx", ".bfsha", ".bfstm", ".bfwav", ".bffnt", ".bfttf", ".msbt", ".sarc"}
KEY = re.compile(rb"(?im)^\s*(?:master_key_[0-9a-f]+|header_key|key_area_key_(?:application|ocean|system)_[0-9a-f]+)"
                 rb"\s*=\s*[0-9a-f]{32,}\s*$")
FONTS = {"third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf"}


def main():
    root = Path(__file__).resolve().parents[2]
    tracked = subprocess.check_output(["git", "ls-files", "--stage", "-z"], cwd=root)
    failures = []
    count = 0
    for row in tracked.split(b"\0"):
        if not row:
            continue
        info, name = row.split(b"\t", 1)
        relative = name.decode("utf-8")
        path = PurePosixPath(relative)
        folded = {part.casefold() for part in path.parts}
        item = root / relative
        count += 1
        if (info.split()[0] not in (b"100644", b"100755") or item.is_symlink() or
                GAME_DIRS & folded or any(part.startswith("generated-") for part in folded) or
                path.suffix.casefold() in GAME_ENDINGS or
                path.name.casefold() in {"keys.txt", "romfs.src", "save.meta", "eowgame.dll.id", "eowrecomp.ini"}):
            failures.append(relative)
            continue
        if relative in FONTS:
            continue
        data = item.read_bytes()
        if b"\0" in data or KEY.search(data):
            failures.append(relative)
    if failures:
        raise SystemExit("Source distribution rejected these paths:\n" + "\n".join(failures))
    print(f"Verified {count} tracked source and licensed font files.")


if __name__ == "__main__":
    main()
