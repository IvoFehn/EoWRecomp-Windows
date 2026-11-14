# EoWRecomp for Windows

EoWRecomp for Windows statically recompiles *The Legend of Zelda: Echoes of Wisdom* into native x86-64-v3 code on your PC and renders with Vulkan.

## Get started

1. Download **EoWRecomp.exe** from [Releases](../../releases) when a Windows build is available.
2. Put the EXE beside your own base game dump of *Echoes of Wisdom* **1.0.0** (`.nsp` or `.xci`) and your Switch's `prod.keys`.
3. Open the EXE and select **Set up game files**. The launcher builds the game library locally. Select **Play** when setup finishes.

You need 64-bit Windows 10 (1903 or later) or Windows 11, an x86-64-v3 CPU with AVX2, and a Vulkan-capable graphics driver. Use a writable folder with at least 12 GB free for first setup. The EXE unpacks its runtime beside itself and keeps generated files and saves in `EoWRecomp-data`. See the [Windows setup guide](docs/WINDOWS-START.md) for details.

## Build and verification

The Windows port is under test. GitHub Actions builds the native launcher with an empty generated-code directory and checks the single-file bootstrap with synthetic data. The [Windows build guide](docs/BUILD-windows.md) covers developer builds. Game setup and gameplay require a local copy of the game.

## Credits and license

This Windows project builds on [EoWRecomp](https://github.com/Nycolazs/EoWRecomp), [LAS Recomp](https://github.com/peiikai/LASRecomp) by Peiikai and its contributors, and [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus). The project code is licensed under [MIT](LICENSE). Third-party code, tools and fonts retain their own licenses. The release's corresponding source archive is provided separately for redistribution.

This is an independent fan project. Nintendo owns the game and its trademarks. The repository contains source and licensed dependencies. It includes no game files, game keys, game screenshots, or generated game binaries.
