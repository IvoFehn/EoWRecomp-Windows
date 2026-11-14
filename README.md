# EoWRecomp for Windows

Play your own copy of *The Legend of Zelda: Echoes of Wisdom* on Windows. EoWRecomp converts the game code into native x86-64 code on your PC and renders it with Vulkan.

[![Windows build](https://github.com/IvoFehn/EoWRecomp-Windows/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/IvoFehn/EoWRecomp-Windows/actions/workflows/windows.yml)

**[⬇️ Download EoWRecomp.exe](https://github.com/IvoFehn/EoWRecomp-Windows/releases)** · [Windows setup guide](docs/WINDOWS-START.md)

> [!NOTE]
> This is a release candidate. The Windows features below passed targeted tests on earlier builds. Full game setup and frame pacing for this candidate are still being checked.

## 🚀 Quick start

> [!IMPORTANT]
> Dump the **1.0.0 base game** yourself from your own cartridge or your own legally purchased digital copy. Extract `prod.keys` from your own Nintendo Switch. Keep your dump and keys private.

1. Download **EoWRecomp.exe** from Releases and put it in a writable folder with your game dump (`.nsp` or `.xci`) and `prod.keys`.
2. Open the EXE and click **Set up game files**. It detects the adjacent dump and builds the game library locally. The first build can take a while.
3. Click **Play** when setup finishes. Keep your dump in its selected location while you play.

```text
Your folder/
  EoWRecomp.exe
  Echoes of Wisdom.nsp
  prod.keys
```

The EXE unpacks its runtime beside itself and saves your settings and progress in `EoWRecomp-data`. Game data, keys and generated game code stay on your PC.

**Requirements** · 64-bit Windows 10 (1903+) or Windows 11 · x86-64-v3 CPU with AVX2 and FMA · Vulkan 1.3 graphics driver · 12 GB free for first setup

## 🎮 Tested Windows features

| Feature | What you can use |
| :-- | :-- |
| Frame rate | Auto with a 60 FPS target or the game's 30 FPS mode |
| Display | Borderless fullscreen on a chosen monitor, 16:9 output with black bars or stretch |
| Graphics | 1080p, 1440p, 1800p, 4K, 5K or window-size rendering, AMD FSR 1 or bilinear scaling, and SMAA |
| Controls | Remappable keyboard and gamepad controls, optional left-click action, and Xbox, PlayStation, Steam Deck and keyboard button prompts |
| Audio and console mode | Stereo with a selectable output device, plus Docked and Handheld modes applied on game start |
| Shader cache | Setup prepares known shaders and the runtime reuses its local cache |

Press **F1** for settings or **F10** for the performance overlay.

## 🛠️ Build and credits

GitHub Actions builds the launcher and checks the release EXE from a clean Windows checkout. The corresponding source and license notices for bundled dependencies are available on the release page. [Windows build guide](docs/BUILD-windows.md)

This independent fan project builds on [EoWRecomp](https://github.com/Nycolazs/EoWRecomp), [LAS Recomp](https://github.com/peiikai/LASRecomp) and [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus). Project code is [MIT-licensed](LICENSE). Nintendo owns the game and its trademarks.
