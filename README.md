# Jedi Survivor Tweaks

A collection of tweaks and fixes for **Star Wars Jedi: Survivor**. Ships as two
interchangeable build variants from a single source tree:

- **ASI plugin** (`JediSurvivorTweaks.asi`) — loaded via Ultimate ASI Loader.
- **ReShade Addon** (`JediSurvivorTweaks.addon64`) — loaded directly by ReShade; ships with an in-game ImGui overlay for live tuning.

The two variants are alternatives that share the same `JediSurvivorTweaks.ini`
schema and identical in-game behaviour. Pick whichever loader you already have
set up — for live runtime tuning without restarting the game, choose the
ReShade Addon variant.

## Features

All features can be toggled on or off individually via the
`JediSurvivorTweaks.ini` configuration file. Both build variants use the
same file name; the mod ships in two separate archives (ASI / ReShade) each
with its own preconfigured copy.

- **LetterboxPillarboxFix** — removes pillarboxing / letterboxing (black bars) during cutscenes on 21:9 and wider displays. Enabled by default.
- **AspectRatioUIFix** — workaround for broken UI scaling on 16:10 displays. The game passes `1.111` (its 16:10 aspect constant) into a UI scale formula (`value / 1.5 = 0.740`); replacing it with `1.0` gives a better result (`1.0 / 1.5 = 0.667`). On 16:9 the game already uses `1.0`, so the hook is a no-op there. Configurable `Multiplier` (1.0–1.111; default 1.0).
- **GameplayFOV** — adjusts the gameplay field of view via a `Multiplier` (0.0–10.0).
- **CameraDistance** — adjusts the camera distance from the player via a `Multiplier` (0.0–10.0).
- **GraphicalTweaks**:
  - **Sharpening** — overrides `r.Tonemapper.Sharpen` strength (0.0–10.0; game default 0.0).
  - **ChromaticAberration** — toggles `r.SceneColorFringeQuality` on/off.
  - **Vignette** — toggles `r.Tonemapper.Quality` on/off.
- **InterpolatedRendering** — enables `respawn.InterpolatedRendering` to reduce CPU stutters and camera jitter.
- **CVars** — applies arbitrary Unreal Engine console variables via the `.ini` file. Resolution scans the game binary's `.rdata` section for the UTF-16 CVar name, then `.text` for references to it, deriving either a global pointer or a direct variable address. If the CVar object isn't yet constructed at startup, the write is queued and retried by a background pump thread (100 ms interval) until success or a 30 s timeout; CVars absent from the binary are dropped immediately on the first scan pass.

## Installation

Pick **one** of the two variants below. Both read the same `JediSurvivorTweaks.ini`
and produce identical in-game behaviour.

### Variant A — ASI plugin

1. Download and install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
   The `xinput1_3.dll` variant is recommended and known to work; other proxy DLL names may function but are not guaranteed.
   Place the chosen `.dll` into the game's `SwGame\Binaries\Win64` folder.
2. Copy `JediSurvivorTweaks.asi` into the game's `SwGame\Binaries\Win64` folder (or a `plugins` subfolder).
3. Copy `JediSurvivorTweaks.ini` into `SwGame\Binaries\Win64`.

### Variant B — ReShade Addon

1. Download and install [ReShade](https://reshade.me/#download). When the
   installer asks which version, pick the **"with full add-on support"**
   build — the stock signed ReShade has add-ons disabled and won't load
   `.addon64` files.
2. Copy `JediSurvivorTweaks.addon64` next to the ReShade DLL the game loads,
   or into the directory pointed to by `AddonPath` in `ReShade.ini`.
3. Copy `JediSurvivorTweaks.ini` to the same directory.
4. Launch the game. Open the ReShade overlay (default: **Home** key); in the **Add-ons** tab you
   should see "JediSurvivorTweaks" listed as loaded.

#### In-game overlay panel

Once loaded, the **Add-ons → JediSurvivorTweaks** panel provides, per tweak:

- A green/red **status dot** next to the tweak name (green = loaded; red = disabled in `.ini` or failed to install).
- A **"Load on launch"** checkbox that persists the tweak's `[Section] Enabled` key. Hooks can't be installed or removed at runtime, so this takes effect on next launch — the live controls below it are the way to change behaviour right now.
- **Live runtime controls** that take effect immediately:
  - **Sliders** for the multiplier tweaks: GameplayFOV, CameraDistance, AspectRatioUIFix (range 0–10, or 1.0–1.111 for AspectRatioUIFix).
  - **Sharpening on/off** checkbox + **Sharpening Strength** slider (0–10).
  - **Chromatic Aberration**, **Vignette**, and **Interpolated Rendering** on/off checkboxes.
- Per-tweak **Reset** button (top-right of each section) snaps that tweak's controls back to defaults.
- Global **Reset to Defaults** button at the bottom resets every tweak's runtime controls.

Changes are autosaved to `JediSurvivorTweaks.ini` after a short debounce
(500 ms); there is no explicit Save button. The ReShade Addon rewrites
the file in a deterministic, comment-free format — manual comments and
custom key ordering won't survive an overlay save. The same tooltip text
you see in the overlay carries the documentation that the comments used
to.

## Configuration

Edit `JediSurvivorTweaks.ini` directly in any text editor. Each tweak
section has an `Enabled` flag plus tweak-specific values. The ASI variant
never rewrites this file from the runtime; the ReShade Addon rewrites it
whenever the overlay changes a value (see above).

```ini
[AspectRatioUIFix]
Enabled = false
Multiplier = 1.0        ; 1.0–1.111 (1.111 = revert to game default)

[LetterboxPillarboxFix]
Enabled = true

[GameplayFOV]
Enabled = false
Multiplier = 1.0        ; 0.0–10.0

[CameraDistance]
Enabled = false
Multiplier = 1.0        ; 0.0–10.0

[GraphicalTweaks]       ; master switch for the three tweaks below
Enabled = true

[Sharpening]
Enabled = true
Strength = 1.0          ; 0.0–10.0 (game default is 0.0)

[ChromaticAberration]
Enabled = false

[Vignette]
Enabled = true

[InterpolatedRendering]
Enabled = true

[CVars]
; r.VSync = 0
; r.D3D12.UseAllowTearing = 1

[Logger]
MinLevel = Info         ; Debug | Info | Warning | Error
```

## Build Requirements

- Visual Studio 2022 (or Build Tools 2022+)
- Platform toolset **v145** (also supported: v143)
- C++23 Standard (`/std:c++latest`)
- MASM (`ml64`) — wired in via the project's `BuildCustomizations\masm.props/.targets`
- Target: x64 (Release recommended)

To build from the command line, run `build.bat`. It builds both variants
sequentially:

- `x64\Release\JediSurvivorTweaks.asi` (ASI plugin)
- `x64\ReleaseAddon\JediSurvivorTweaks.addon64` (ReShade Addon)

Each variant is selected by an MSBuild configuration (`Release` vs `ReleaseAddon`)
and is compiled from disjoint entry-point translation units — there are no
preprocessor switches in the shared core.

## Project Layout

```
src/
├── core/         # Logger, Config, MemoryScanner, PEUtils, HookEngine,
│                 # CVarSystem (scanner, resolver, layout constants, per-CVar override table)
├── hooks/        # ASM detours (tweak_hooks.asm) + shared C++ hook context
├── tweaks/       # Each individual tweak + the unified HookTweak base + manager
├── reshade/      # ReShade-Addon-only TUs (entry.cpp, overlay.cpp)
├── external/     # Vendored libraries:
│   ├── hde64/    #   HDE64 disassembler (C)
│   └── reshade/  #   ReShade SDK headers (header-only, BSD-2-Clause)
├── main_app.{hpp,cpp}  # Loader-agnostic Application + bootstrap
└── entry_asi.cpp       # ASI loader entry-point
```

## Support

<p align="center">
  <a href="https://boosty.to/osyka.yuri/donate">
    <img src="https://img.shields.io/badge/%E2%98%95%20Support%20on-Boosty-F15C22?style=for-the-badge&labelColor=1c1c1c" alt="Support on Boosty"/>
  </a>
</p>

## License

MIT License
