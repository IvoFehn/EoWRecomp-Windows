"""Freeze and audit the staged Windows runtime, helper, and release archives."""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath
from urllib.parse import quote
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

import package
import validate_package


ROOT = Path(__file__).resolve().parents[2]
FFMPEG = "ffmpeg-9.0.2.tar.xz"
FFMPEG_SHA = "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
FFMPEG_DLLS = (
    "avcodec-63.dll", "avformat-63.dll", "avutil-61.dll", "swresample-7.dll", "swscale-10.dll",
)
ZIG = "zig-x86_64-windows-0.17.0"
ZIG_SHA = "b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428"
MSYS_PREFIX = "mingw-w64-ucrt-x86_64-"
MSYS_DLLS = {
    "SDL3.dll": ("sdl3", "sdl3"),
    "libshaderc_shared.dll": ("shaderc", "shaderc"),
    "libgcc_s_seh-1.dll": ("libgcc", "gcc"),
    "libstdc++-6.dll": ("libstdc++", "gcc"),
    "libwinpthread-1.dll": ("libwinpthread", "winpthreads"),
    "libiconv-2.dll": ("libiconv", "libiconv"),
}
MSYS_SOURCES = {"spirv-tools": "spirv-tools", "glslang": "glslang", "headers": "headers"}
MSYS_LICENSES = (
    "SDL3/LICENSE.txt", "shaderc/LICENSE", "gcc/COPYING.RUNTIME", "gcc/COPYING3",
    "glslang/LICENSE.txt", "spirv-tools/LICENSE", "libwinpthread/COPYING",
    "libiconv/COPYING", "libiconv/COPYING.LIB", "vulkan-memory-allocator/LICENSE.txt",
)
SYSTEM_DLLS = set("""
    advapi32 bcrypt bcryptprimitives cfgmgr32 combase comctl32 comdlg32 crypt32 dwmapi dxgi
    gdi32 gdi32full imm32 iphlpapi kernel32 kernelbase msvcp_win msvcrt ntdll ole32 oleaut32
    powrprof propsys psapi rpcrt4 secur32 sechost setupapi shcore shell32 shlwapi ucrtbase user32
    userenv version vulkan-1 win32u winhttp winmm wintrust ws2_32
""".split())
PYTHON_DLLS = {
    "python312.dll": "python312.dll",
    "VCRUNTIME140.dll": "VCRUNTIME140.dll",
    "VCRUNTIME140_1.dll": "VCRUNTIME140_1.dll",
    "libffi-8.dll": "DLLs/libffi-8.dll",
}
HELPER_MODULES = (
    "analyze.py", "eowgen.spec", "gen.py", "lifter.py", "nso.py",
    "patches.py", "pe_no_cfg.py", "simd_c.py",
)
WHEEL_PINS = {
    "altgraph==0.17.5": "f3a22400bce1b0c701683820ac4f3b159cd301acab067c51c653e06961600597",
    "capstone==5.0.9": "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0",
    "lz4==4.4.5": "713a777de88a73425cf08eb11f742cd2c98628e79a8673d6a52e3c5f0c116f33",
    "packaging==26.3": "d7193f7c8e4e93f444fde0262bf90af30e16fa0ad0ad44cb553c87339b23cd1c",
    "pefile==2024.8.26": "76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f",
    "pyinstaller-hooks-contrib==2026.10": "dfc3cde20a7ce6c92e803016e66eed4b7050467e2c5f886a3745d89b258c6d6d",
    "pyinstaller==6.22.3": "500bd58c7bf7e584a8435adccbd763a0b918d5c12b08d74ff50fd79b2915458b",
    "pywin32-ctypes==0.2.3": "8a1513379d709975552d202d942d9837758905c8d01eb82b8bcc30918929e7b8",
    "setuptools==84.2.1": "ca140b45d5325a9478f880e351484f62fef15f336462e449ca0c03c873d589c2",
    "unicorn==2.1.4": "d7107500c64ce5c168fbff6bef9485b5db1350050036f4cea568650cf8bdbdf5",
}
UNICORN = "unicorn-2.1.4.tar.gz"
UNICORN_SHA = "00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff"
UNICORN_URL = ("https://files.pythonhosted.org/packages/b2/1b/"
               "b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/" + UNICORN)
UNICORN_COPYING_SHA = "6f04ae8364d0079a192b14635f4b1da294ce18724c034c39a6a41d1b09df6100"
UNICORN_COPYING_URL = "https://raw.githubusercontent.com/unicorn-engine/unicorn/2.1.4/COPYING"


def need(condition, message):
    if not condition:
        raise ValueError(message)


def run(*args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def sha_file(path):
    return validate_package.sha_file(path)


def verify_sha(path, expected):
    need(sha_file(path) == expected, f"SHA-256 mismatch for {path.name}")


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def nonempty_copy(source, target):
    need(source.is_file() and source.stat().st_size > 0, f"Missing build input: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def fetch(curl, url, path, expected=None):
    run(curl, "--fail", "--silent", "--show-error", "--location", "--proto", "=https",
        "--proto-redir", "=https", "--max-time", "240", "--output", path, url)
    if expected is not None:
        verify_sha(path, expected)
    return {"url": url, "sha256": sha_file(path), "bytes": path.stat().st_size}


def verify_msys_source(msys, signature, artifact):
    # Reuse the trusted pacman keyring and leave its trust database unchanged.
    result = subprocess.run(
        [msys / "usr/bin/gpg.exe", "--no-options", "--homedir", "/etc/pacman.d/gnupg",
         "--batch", "--no-auto-key-retrieve", "--no-auto-check-trustdb", "--lock-never",
         "--status-fd", "1", "--verify", signature.as_posix(), artifact.as_posix()],
        text=True, encoding="utf-8", errors="replace", capture_output=True, cwd=ROOT,
    )
    valid = re.findall(r"(?m)^\[GNUPG:\] VALIDSIG ([A-F0-9]{40,64}) ", result.stdout)
    trusted = re.search(r"(?m)^\[GNUPG:\] TRUST_(FULLY|ULTIMATE) ", result.stdout)
    need(result.returncode == 0 and len(valid) == 1 and trusted,
         f"Invalid or untrusted MSYS2 source package signature: {artifact.name}")
    return valid[0]


def keep_zig_file(parts):
    if parts[0] in ("LICENSE", "README.md", "zig.exe"):
        return len(parts) == 1
    if parts[0] != "lib" or len(parts) < 2 or parts[1] in ("libcxx", "libcxxabi", "libtsan", "docs"):
        return False
    if parts[1] != "libc" or len(parts) < 3 or parts[2] == "mingw":
        return True
    return parts[2] == "include" and (len(parts) < 4 or re.search("windows|mingw", parts[3]))


def verify_zig(source, curated):
    verify_sha(source, ZIG_SHA)
    files = {}
    with ZipFile(source) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            prefix, *parts = PurePosixPath(entry.filename).parts
            need(prefix == ZIG and parts, "Unexpected upstream Zig archive root")
            if keep_zig_file(parts):
                with archive.open(entry) as stream:
                    files["/".join(parts)] = hashlib.file_digest(stream, "sha256").hexdigest()
    actual = {path: sha_file(item) for item, path in package.tree(curated / "lib", "lib")}
    actual = {path.as_posix(): value for path, value in actual.items()}
    actual.update({name: sha_file(curated / name) for name in ("zig.exe", "LICENSE", "README.md")})
    need(actual == files, "Curated Zig files differ from the checked upstream archive")


def build_helper(stage, notices, msys):
    venv = stage / "venv"
    wheelhouse = stage / "wheels"
    wheelhouse.mkdir()
    requirements = notices / "requirements.txt"
    requirements.write_text("".join(f"{name} --hash=sha256:{digest}\n"
                                    for name, digest in WHEEL_PINS.items()), encoding="ascii")
    run(sys.executable, "-m", "venv", venv)
    python = venv / "Scripts/python.exe"
    pip = (python, "-m", "pip", "--isolated", "--disable-pip-version-check", "--no-cache-dir")
    run(*pip, "download", "--index-url", "https://pypi.org/simple/", "--only-binary=:all:",
        "--no-deps", "--require-hashes", "-r", requirements, "-d", wheelhouse)
    run(*pip, "install", "--no-index", "--find-links", wheelhouse, "--no-deps", "--require-hashes",
        "-r", requirements)
    wheel_digests = {wheel.name: sha_file(wheel) for wheel in sorted(wheelhouse.glob("*.whl"))}
    need(sorted(wheel_digests.values()) == sorted(WHEEL_PINS.values()), "Wheel inventory mismatch")
    write_json(notices / "wheels.json", wheel_digests)

    system = Path(os.environ["SystemRoot"])
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("PYTHON", "PYINSTALLER"))}
    env.update(PATH=f"{system / 'System32'};{system}", PYINSTALLER_CONFIG_DIR=str(stage / "pyi-cache"))
    dist = stage / "dist"
    run(python, "-m", "PyInstaller", "--clean", "--noconfirm", "--distpath", dist,
        "--workpath", stage / "pyi-work", ROOT / "tools/eowgen.spec", env=env)
    helper = dist / "eowgen"
    run(python, "-B", ROOT / "tools/pe_no_cfg.py", helper / "eowgen.exe")
    original = Path(sys.base_prefix)
    cp_notice = original / "LICENSE.txt"
    need("Microsoft Distributable Code" in cp_notice.read_text(encoding="utf-8"),
         "CPython installation is missing its Windows redistribution notices")
    nonempty_copy(cp_notice, notices / "CPython-3.12.10-LICENSE.txt")
    for name, relative in PYTHON_DLLS.items():
        verify_sha(helper / "_internal" / name, sha_file(original / relative))
    for filename in HELPER_MODULES:
        nonempty_copy(ROOT / "tools" / filename, notices / "eowgen-source" / filename)
    nonempty_copy(ROOT / "LICENSE", notices / "eowgen-source/LICENSE")
    for filename in wheel_digests:
        with ZipFile(wheelhouse / filename) as archive:
            for entry in archive.infolist():
                parts = PurePosixPath(entry.filename).parts
                if (not entry.is_dir() and ".dist-info" in parts[0]
                        and re.match(r"(?i)^(license|copying|notice)([._-]|$)", parts[-1])):
                    target = notices / filename.removesuffix(".whl") / Path(*parts).name
                    target.parent.mkdir(exist_ok=True)
                    with target.open("xb") as stream:
                        stream.write(archive.read(entry))

    curl = msys / "usr/bin/curl.exe"
    source = notices / UNICORN
    upstream = fetch(curl, UNICORN_URL, source, UNICORN_SHA)
    fetch(curl, UNICORN_COPYING_URL, notices / "Unicorn-2.1.4-COPYING.txt", UNICORN_COPYING_SHA)
    with tarfile.open(source, "r:gz") as archive:
        for name in ("COPYING", "COPYING.LIB", "LICENSE"):
            with archive.extractfile(f"unicorn-2.1.4/src/qemu/{name}") as stream:
                (notices / f"Unicorn-QEMU-{name}.txt").write_bytes(stream.read())
    return helper, venv, upstream


def source_inputs(stage, msys, runtime_notices):
    ucrt = msys / "ucrt64"
    pacman = msys / "usr/bin/pacman.exe"
    packages = dict(line.split() for line in (stage / "packages.txt").read_text().splitlines())
    bits = {name: packages[MSYS_PREFIX + name] for name, _ in MSYS_DLLS.values()}
    for name, (owner, _) in MSYS_DLLS.items():
        observed = run(pacman, "-Qqo", f"/ucrt64/bin/{name}", text=True, capture_output=True).stdout.strip()
        need(observed == MSYS_PREFIX + owner, f"Unexpected MSYS2 DLL owner for {name}: {observed}")
    selected = dict(MSYS_SOURCES, **{package_name: base for package_name, base in MSYS_DLLS.values()})
    urls = {}
    for package_name, base in selected.items():
        version = packages[MSYS_PREFIX + package_name]
        need(re.fullmatch(r"[A-Za-z0-9.:+_~-]+", version), "Invalid MSYS2 version metadata")
        filename = f"mingw-w64-{base}-{version.replace(':', '~')}.src.tar.zst"
        urls[filename] = "https://repo.msys2.org/mingw/sources/" + quote(filename)
    downloads = stage / "source-downloads"
    downloads.mkdir()
    files = {}
    for filename, url in sorted(urls.items()):
        artifact = downloads / filename
        receipt = fetch(msys / "usr/bin/curl.exe", url, artifact)
        with artifact.open("rb") as stream:
            need(stream.read(4) == b"\x28\xb5\x2f\xfd", f"Expected MSYS2 zstd source package: {filename}")
        signature = downloads / (filename + ".sig")
        files[signature] = fetch(msys / "usr/bin/curl.exe", url + ".sig", signature)
        receipt.update(msys2_signature=signature.name, msys2_signer=verify_msys_source(msys, signature, artifact))
        files[artifact] = receipt

    for relative in MSYS_LICENSES:
        directory, name = relative.split("/")
        nonempty_copy(ucrt / "share/licenses" / relative, runtime_notices / f"{directory}-{name}.txt")
    for vendor in ("imgui", "smaa"):
        nonempty_copy(ROOT / f"third_party/{vendor}/LICENSE.txt", runtime_notices / f"{vendor}-LICENSE.txt")
    ff_src = stage / "deps/src"
    ff_archive = ff_src / FFMPEG
    verify_sha(ff_archive, FFMPEG_SHA)
    files[ff_archive] = {
        "url": f"https://ffmpeg.org/releases/{FFMPEG}", "sha256": FFMPEG_SHA,
        "bytes": ff_archive.stat().st_size,
    }
    for name in ("COPYING.LGPLv2.1", "LICENSE.md"):
        nonempty_copy(ff_src / "ffmpeg" / name, runtime_notices / f"FFmpeg-{name}.txt")
    config = (ff_src / "ffmpeg/config.h").read_text()
    need(all(f"#define CONFIG_{option} {value}" in config.splitlines()
             for option, value in (("GPL", 0), ("NONFREE", 0), ("VERSION3", 0), ("SWRESAMPLE", 1))),
         "FFmpeg build is not the expected LGPL-only profile")
    configure = next(line for line in config.splitlines() if line.startswith("#define FFMPEG_CONFIGURATION "))
    (runtime_notices / "FFmpeg-configuration.txt").write_text(configure + "\n", encoding="utf-8")
    write_json(runtime_notices / "sources.json", {path.name: receipt for path, receipt in files.items()})
    return files, bits


def pe_imports(pefile, path):
    with pefile.PE(str(path), fast_load=True) as image:
        need(image.FILE_HEADER.Machine == 0x8664, f"Expected x64 PE: {path.name}")
        image.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY[name] for name in (
            "IMAGE_DIRECTORY_ENTRY_IMPORT", "IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT")])
        return {entry.dll.decode("ascii").casefold()
                for field in ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT")
                for entry in getattr(image, field, ())}


def reject_unresolved(pefile, images):
    local = {path.name.casefold() for path in images}
    result = {}
    for path in images:
        imports = pe_imports(pefile, path)
        unresolved = {name for name in imports - local
                      if not name.startswith(("api-ms-win-", "ext-ms-win-"))
                      and name.removesuffix(".dll") not in SYSTEM_DLLS}
        need(not unresolved, f"Unresolved non-system imports in {path.name}: {sorted(unresolved)}")
        result[path.name] = sorted(imports)
    return result


def audit_binaries(venv, runtime, helper, stage, msys):
    sys.path.insert(0, str(venv / "Lib/site-packages"))
    import pefile

    dlls = [stage / "deps/bin" / name for name in FFMPEG_DLLS]
    dlls.extend(msys / "ucrt64/bin" / name for name in MSYS_DLLS)
    runtime_imports = reject_unresolved(pefile, [runtime / "eowrecomp.exe", *dlls])
    need(set(FFMPEG_DLLS) <= {name for imports in runtime_imports.values() for name in imports},
         "Runtime does not use the complete expected FFmpeg import set")
    native = sorted(path for path in helper.rglob("*") if path.suffix.casefold() in (".dll", ".pyd", ".exe"))
    helper_imports = reject_unresolved(pefile, native)
    known_dlls = {f"_internal/{name.casefold()}" for name in PYTHON_DLLS}
    known_dlls |= {"_internal/capstone/lib/capstone.dll", "_internal/unicorn/lib/unicorn.dll"}
    need({path.relative_to(helper).as_posix().casefold() for path in native if path.suffix.casefold() == ".dll"}
         == known_dlls, "Frozen generator native DLL inventory changed")
    return dlls, runtime_imports, helper_imports


def main(output, msys):
    stage = output / "work"
    runtime = stage / "runtime"
    zig = ROOT / "build/zig"
    verify_zig(ROOT / f"build/zig-dl/{ZIG}.zip", zig)
    runtime_notices = stage / "runtime-notices"
    generator_notices = stage / "generator-notices"
    runtime_notices.mkdir()
    generator_notices.mkdir()
    helper, venv, unicorn = build_helper(stage, generator_notices, msys)
    source, package_versions = source_inputs(stage, msys, runtime_notices)
    dlls, runtime_imports, helper_imports = audit_binaries(venv, runtime, helper, stage, msys)

    source[generator_notices / UNICORN] = unicorn
    source_manifest = stage / "source-manifest.json"
    write_json(source_manifest, {path.name: receipt for path, receipt in source.items()})
    corresponding = output / "corresponding-sources.zip"
    with ZipFile(corresponding, "x", ZIP_STORED) as archive:
        for path in sorted(source):
            archive.write(path, f"upstream/{path.name}")
        archive.write(source_manifest, "source-manifest.json", ZIP_DEFLATED)
        archive.write(ROOT / "tools/build_deps_windows.sh", "build/build_deps_windows.sh", ZIP_DEFLATED)
        archive.write(ROOT / "tools/windows/build_release_payload.sh", "build/build_release_payload.sh", ZIP_DEFLATED)
        archive.write(ROOT / "tools/windows/build_release_payload.py", "build/build_release_payload.py", ZIP_DEFLATED)
        for path, relative in package.tree(runtime_notices, "licenses/runtime"):
            archive.write(path, relative.as_posix(), ZIP_DEFLATED)
        for path, relative in package.tree(generator_notices / "eowgen-source", "build/eowgen-source"):
            archive.write(path, relative.as_posix(), ZIP_DEFLATED)
        for name in ("Unicorn-2.1.4-COPYING.txt", "Unicorn-QEMU-COPYING.txt",
                     "Unicorn-QEMU-COPYING.LIB.txt", "Unicorn-QEMU-LICENSE.txt", "requirements.txt"):
            archive.write(generator_notices / name, f"build/eowgen-source/{name}", ZIP_DEFLATED)

    zip_path = output / "EoWRecomp-Windows-x64-v3.zip"
    manifest = output / "manifest.json"
    run(sys.executable, "-B", ROOT / "tools/windows/package.py", "--release", runtime, "--generator", helper,
        "--zig", zig, *(arg for dll in dlls for arg in ("--dll", dll)),
        "--runtime-notices", runtime_notices, "--generator-notices", generator_notices,
        "--output", zip_path, "--manifest", manifest)
    validate_package.validate(zip_path, manifest)
    extracted = stage / "verified-extraction"
    with ZipFile(zip_path) as archive:
        extracted.mkdir()
        archive.extractall(extracted)
    validation = validate_package.validate(zip_path, manifest, extracted)
    write_json(output / "validation.json", validation)
    write_json(output / "provenance.json", {
        "archive_sha256": validation["archive_sha256"],
        "manifest_sha256": validation["manifest_sha256"],
        "corresponding_sources_sha256": sha_file(corresponding),
        "source_revision": (stage / "revision.txt").read_text().strip(),
        "compiler": (stage / "compiler.txt").read_text().splitlines(),
        "msys_runtime_packages": package_versions,
        "runtime_dll_sha256": {dll.name: sha_file(dll) for dll in dlls},
        "runtime_imports": runtime_imports,
        "helper_imports": helper_imports,
        "zig_upstream_sha256": ZIG_SHA,
    })
    (output / (zip_path.name + ".sha256")).write_text(
        f"{validation['archive_sha256']}  {zip_path.name}\n", encoding="ascii")
    print(f"Validated {validation['files']} Windows payload files and their fresh extraction.")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 3:
            raise ValueError("Expected release output and MSYS2 root directories")
        main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Windows payload preparation failed: {error}") from error
