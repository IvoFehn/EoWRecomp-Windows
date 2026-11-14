# Contributing to EoWRecomp

Windows bug reports, testing on different hardware and code contributions are welcome. Include your Windows version, CPU, GPU and the shortest sequence that reproduces the issue.

## The one hard rule: no Nintendo files

Keep game dumps, keys, extracted assets, generated game code, screenshots and recordings with game content out of the GitHub repository, issues and pull requests. Use logs with personal paths and secrets removed.

## Project layout

- `tools/nca_extract.py` - ExeFS / RomFS from the program NCA of the user's own dump (handles compressed RomFS).
- `tools/gen.py` - the recompiler: the game's executables to C (`generated/`, never committed). `tools/patches.py`
  holds code patches applied during generation; every patch site checks the original instruction word.
- `runtime/` - the host side: `kernel.c`, `loader.c`, `ipc.c`, `services/` (the Switch OS replacement), `hle/`
  (host replacements of SDK functions), `nvn/` (the Vulkan backend and Maxwell shader translator),
  `ui/` (ImGui overlay menu and start screen).

## Testing your change

- Build from a clean Windows checkout and report the result of your verification.
- Use synthetic input for checks that run on GitHub. Keep tests that require your game local.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT License](LICENSE).
