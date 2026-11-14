# Building EoWRecomp on Windows (x86-64)

The Windows build uses Vulkan and runs with your own game files. See [the launcher instructions](WINDOWS-START.md) for the packaged build.

GitHub Actions builds the native launcher and its Windows import library with an empty generated-code directory. It checks the source tree, the package validator and single-file extraction with synthetic inputs. The release job rebuilds the Windows payload, audits its redistributable components and checks extraction from the finished `EoWRecomp.exe`. Run game setup and gameplay checks locally with your own game files.

## One-time setup
Run in PowerShell from the repo root:

    powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1

This installs MSYS2 (UCRT64: clang, lld, make, SDL3, FFmpeg, Vulkan loader + headers, shaderc, Vulkan Memory
Allocator, validation layers) and Python 3.12 for the tools (`tools\venv`: unicorn, capstone, lz4). It switches the clone to LF line endings.

## Build
From an MSYS2 UCRT64 shell, or Git Bash with MSYS2 first on PATH:

    export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
    make gen        # once: recompile the game from extracted/exefs (your own game files; see the start screen)
    make -j         # -> build/eowrecomp.exe and the game code library build/eowgame.dll

Without `generated/` only the app is built: the start screen builds the game code itself when you set up the game
files (as for players), given the generator and the compiler (`make gen-helper`, `tools/fetch_zig.sh`).
`make runtime-objs` compiles the runtime only (quick check).

Run from a shell that has `C:\msys64\ucrt64\bin` on PATH (SDL3, FFmpeg, Vulkan, shaderc DLLs).

## Build the single-file download

Run the **Windows build** workflow with **Build and test the complete release payload** to produce a candidate. The workflow uses the release script in `tools/windows/` to build LGPL FFmpeg and the launcher, collect the audited DLLs and local build tools, and verify the archive and the final EXE. The candidate carries the source archive required for redistribution. See the [workflow](../.github/workflows/windows.yml) for the compiler command and the intermediate checks.

Version tags build a GitHub release. A tag with a suffix such as `-rc.1` creates a prerelease. Players download `EoWRecomp.exe`.

Verify first-run game setup with a Windows system `PATH`. Keep game files, keys, generated libraries, saves and logs local. The Vulkan loader comes from the graphics driver.

## Tests
- `tools/test_translate.sh`: shader translator fuzz and GLSL validation.
- `tools/test_vk_pipelines.sh`: fuzzed shaders through the backend's compile/reflection into real pipelines.
- `EOWRECOMP_VK_VALIDATION=1` with `VK_ADD_LAYER_PATH` pointing at `C:\msys64\ucrt64\bin`: Khronos validation layer.
- Run test windows directly, not under MSYS `timeout` (it stalls the window).
