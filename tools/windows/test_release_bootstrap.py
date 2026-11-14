"""Run headless extraction checks for the Windows single-file release."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from zipfile import ZipFile

from build_single_exe import FOOTER, MAGIC
from validate_package import manifest_files


def run_verify(exe, expect_success, timeout):
    result = subprocess.run([exe, "--bootstrap-verify"], cwd=exe.parent,
                            timeout=timeout, check=False)
    if (result.returncode == 0) != expect_success:
        raise RuntimeError(f"Unexpected bootstrap verification exit code {result.returncode}")


def verify_files(exe, manifest):
    expected, _ = manifest_files(manifest)
    with exe.open("rb") as stream:
        stream.seek(-FOOTER.size, 2)
        magic, _, _, count, digest = FOOTER.unpack(stream.read(FOOTER.size))
    if magic != MAGIC or count != len(expected):
        raise RuntimeError("The bootstrap footer does not identify the manifest inventory")
    payload = exe.parent / "EoWRecomp-runtime" / digest.hex()
    observed = {"EoWRecomp/" + entry.relative_to(payload).as_posix()
                for entry in payload.rglob("*") if entry.is_file()}
    if observed != expected.keys():
        raise RuntimeError("Extracted file inventory differs from the manifest")
    for name, (size, sha256) in expected.items():
        file = payload / Path(name).relative_to("EoWRecomp")
        with file.open("rb") as stream:
            actual_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        if file.stat().st_size != size or actual_hash != sha256:
            raise RuntimeError(f"Extracted file differs from manifest: {name}")
    print(f"Verified {len(expected)} extracted files against the manifest.")
    return payload


def verify_tools(payload):
    system_root = Path(os.environ["SystemRoot"])
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join((str(system_root / "System32"), str(system_root)))
    for relative, argument in (("eowgen/eowgen.exe", "--help"), ("zig/zig.exe", "version")):
        tool = payload / relative
        result = subprocess.run([tool, argument], cwd=tool.parent, env=env,
                                timeout=60, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not result.stdout.strip():
            raise RuntimeError(f"{relative} produced no output for {argument}")
        print(f"Verified startup of {relative} with the system PATH.")


def test_fixture(bootstrap):
    with TemporaryDirectory(prefix="EoWRecomp Prüfung ") as folder:
        root = Path(folder)
        fixture = b"Synthetic EoWRecomp bootstrap verification, no game files.\n"
        files = {"EoWRecomp/eowrecomp.exe": bootstrap.read_bytes(),
                 "EoWRecomp/nested path/fixture.txt": fixture}
        zip_path = root / "fixture.zip"
        manifest = root / "manifest.json"
        with ZipFile(zip_path, "w") as zip_file:
            for name, data in files.items():
                zip_file.writestr(name, data)
        manifest.write_text(json.dumps({"entries": [
            {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in files.items()
        ]}), encoding="utf-8")

        exe = root / "bundle.exe"
        subprocess.run([sys.executable, str(Path(__file__).with_name("build_single_exe.py")),
                        "--bootstrap", bootstrap, "--zip", zip_path, "--manifest", manifest,
                        "--output", exe], timeout=90, check=True)
        run_verify(exe, expect_success=True, timeout=30)
        payload = verify_files(exe, manifest)

        user = root / "EoWRecomp-data"
        user.mkdir(exist_ok=True)
        sentinel = user / "local-only.txt"
        sentinel.write_bytes(fixture)
        extracted = payload / "nested path" / "fixture.txt"
        extracted.write_bytes(b"damaged")
        run_verify(exe, expect_success=True, timeout=30)
        verify_files(exe, manifest)
        if sentinel.read_bytes() != fixture:
            raise RuntimeError("Persistent data was changed during a repair")

        damaged_root = root / "damaged"
        damaged_root.mkdir()
        damaged_exe = damaged_root / "bundle.exe"
        contents = exe.read_bytes()
        if contents.count(fixture) != 1:
            raise RuntimeError("Could not find the exact packed fixture")
        damaged_exe.write_bytes(contents.replace(fixture, b"X" + fixture[1:], 1))
        run_verify(damaged_exe, expect_success=False, timeout=30)
        print("Verified payload repair, retained local data, and rejected a modified payload.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--bootstrap", type=Path)
    mode.add_argument("--single-exe", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    if args.bootstrap is not None:
        test_fixture(args.bootstrap.resolve())
    else:
        if args.manifest is None:
            parser.error("--single-exe requires --manifest")
        exe = args.single_exe.resolve()
        run_verify(exe, expect_success=True, timeout=240)
        verify_tools(verify_files(exe, args.manifest))


if __name__ == "__main__":
    main()
