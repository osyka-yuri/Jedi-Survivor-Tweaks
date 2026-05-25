# Jedi Survivor Tweaks

A collection of tweaks and fixes for **Star Wars Jedi: Survivor**, implemented as an ASI plugin.

## Features

All features can be toggled on or off individually via the `JediSurvivorTweaks.ini` configuration file.

- **LetterboxPillarboxFix** — removes pillarboxing / letterboxing (black bars) during cutscenes on 21:9 and wider displays. Enabled by default.
- **AspectRatioUIFix** — workaround for broken UI scaling on 16:10 displays. The game passes `1.111` (its 16:10 aspect constant) into a UI scale formula (`value / 1.5 = 0.740`); replacing it with `1.0` gives a better result (`1.0 / 1.5 = 0.667`). On 16:9 the game already uses `1.0`, so the hook is a no-op there. Configurable `Multiplier` (1.0–1.111; default 1.0).
- **GameplayFOV** — adjusts the gameplay field of view via a `Multiplier` (0.0–10.0).
- **CameraDistance** — adjusts the camera distance from the player via a `Multiplier` (0.0–10.0).
- **GraphicalTweaks**:
  - **Sharpening** — overrides `r.Tonemapper.Sharpen` strength (0.0–10.0; game default 0.0).
  - **ChromaticAberration** — toggles `r.SceneColorFringeQuality` on/off.
  - **Vignette** — toggles `r.Tonemapper.Quality` on/off.
- **InterpolatedRendering** — enables `respawn.InterpolatedRendering` to reduce CPU stutters and camera jitter.
- **CVars** — applies arbitrary Unreal Engine console variables via the `.ini` file. CVar resolution is performed asynchronously on a background pump thread, so unresolved CVars are retried until the engine has constructed them (with a 30s timeout).

## Installation

Due to EA AntiCheat, the plugin must be loaded using an ASI loader. Standard proxy DLLs are blocked.

1. Download and install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
   The `xinput1_3.dll` variant is recommended and known to work; other proxy DLL names may function but are not guaranteed.
   Place the chosen `.dll` into the game's `SwGame\Binaries\Win64` folder.
2. Copy `JediSurvivorTweaks.asi` into the game's `SwGame\Binaries\Win64` folder (or a `plugins` subfolder).
3. Copy `JediSurvivorTweaks.ini` into `SwGame\Binaries\Win64`.

## Configuration

Edit `JediSurvivorTweaks.ini` to enable, disable, or adjust specific tweaks.

```ini
; Example snippet
[LetterboxPillarboxFix]
Enabled = true

[GameplayFOV]
Enabled = true
Multiplier = 1.5

[Sharpening]
Enabled = true
Strength = 0.0

[ChromaticAberration]
Enabled = false

[CVars]
r.SceneColorFringeQuality = 0

[Logger]
MinLevel = Info  ; Debug | Info | Warning | Error
```

## Build Requirements

- Visual Studio 2022 (or Build Tools 2022+)
- Platform toolset **v145** (also supported: v143)
- C++23 Standard (`/std:c++latest`)
- MASM (`ml64`) — wired in via the project's `BuildCustomizations\masm.props/.targets`
- Target: x64 (Release recommended)

To build from the command line, run `build.bat`. The build produces
`JediSurvivorTweaks.asi` in `x64\Release\`.

## Project Layout

```
src/
├── core/      # Logger, Config, MemoryScanner, PEUtils, HookEngine, CVarSystem
├── hooks/     # ASM detours (tweak_hooks.asm) + shared C++ context (g_contexts)
├── tweaks/    # Each individual tweak + the unified HookTweak base + manager
└── external/  # Bundled HDE64 disassembler
tools/         # Helper scripts (DLL export dump, etc.)
```

## Support

<p align="center">
  <a href="https://boosty.to/osyka.yuri/donate">
    <img src="https://img.shields.io/badge/%E2%98%95%20Support%20on-Boosty-F15C22?style=for-the-badge&labelColor=1c1c1c" alt="Support on Boosty"/>
  </a>
</p>

## License

MIT License
