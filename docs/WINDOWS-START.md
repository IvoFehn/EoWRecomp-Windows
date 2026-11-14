# EoWRecomp for Windows

This build targets Windows 10 (1903 or later) and Windows 11 on an x86-64-v3 CPU with operating system support for AVX. Install a graphics driver with Vulkan support. Use a writable folder with at least 12 GB free for first setup.

1. Put **EoWRecomp.exe** beside your own base game dump of **The Legend of Zelda: Echoes of Wisdom 1.0.0** (`.nsp` or `.xci`) and your Switch's `prod.keys`.
2. Open **EoWRecomp.exe**. It selects the folder if it finds a game package there. Use **Game files folder** to choose another location or a folder containing your extracted `exefs` and `romfs`.
3. Select **Set up game files**. The launcher builds the native game library on your PC. The first build can take a while.
4. Select **Play** after setup finishes. Keep a packaged game dump at the selected location while using it.

The EXE includes the launcher, local build tools and runtime dependencies. On first launch it creates a versioned `EoWRecomp-runtime` folder beside itself and stores generated files, settings and saves in `EoWRecomp-data`. Keep a backup of `EoWRecomp-data/savedata` and keep your game files and keys on your system.

**Console mode** is in the in-game Video menu and applies the next time you start the game. Choose playback output and speaker layout in the Audio menu. The output and speaker layout choices apply to the current session. A stereo device uses stereo playback when 5.1 is selected. Under Controls, you can select the icon family and optionally use left click for the game's A button.

The Windows display path uses 16:9 SDR. For wider windows, **Window fit** offers black bars or stretch. The Video menu also offers FSR 1 and bilinear scaling. **Vulkan presentation timing** is optional and takes effect after closing and reopening EoWRecomp. It can improve fullscreen frame pacing on supported systems. Native ultrawide rendering is experimental. Temporal upscaling, frame generation, and HDR output are pending.

Discord playing status is off by default. The Controls menu offers it when the build has a public project Discord Application ID or `EOWRECOMP_DISCORD_APP_ID` supplies one. When enabled, it sends EoWRecomp and the session start time to the Discord desktop app.

Third-party notices and source references are in the unpacked runtime's `licenses/` and `zig/` folders.
