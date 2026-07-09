# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.3.0] - 2026-07-09

### Fixed
- **AspectRatioUIFix now scales the in-game HUD and 3D map markers on 16:10 displays.** Previous releases only patched the menu UI code path; the HUD lived in a separate function and stayed oversized — health bars, prompts, minimap overlays, and world markers were unaffected. This release adds a dedicated HUD hook that multiplies the render value before it is stored, alongside the existing menu hook. Both sites share the same `Multiplier` from `[AspectRatioUIFix]`; no new INI keys.

### Added
- **Overlay slider steps.** Runtime sliders now snap to a fixed grid while you drag them, not only after you release the mouse. Multiplier sliders (GameplayFOV, CameraDistance, AspectRatioUIFix), **Sharpening Strength**, and **Pool Size (GB)** step by **0.1**; pool minimum lowered to **0.5 GB** (was 1.0).

### Changed
- **Internals only — no user-facing INI or behaviour changes beyond the fixes above.** Hook engine rebuilt on Zydis 4.1.1 (replacing HDE64) with transactional group install/rollback, gateway allocation within `rel32` range, and a `slots.def` registry that generates MASM slot constants. `AspectRatioUIFix` is now a two-site hook group (Hud + Menu); `StreamingPoolFix` slot index moved accordingly. Unit tests added for the hook engine and slider utilities.

## [1.2.0] - 2026-06-24
### Added
- **StreamingPoolFix** — locks the streaming pool size to a configured number of gigabytes (`PoolSizeGB`, 1.0–12.0; default 2.0) to prevent the unbounded VRAM growth the game can exhibit. Opt-in (disabled by default).

## [1.1.4] - 2026-06-20
### Fixed
- **AspectRatioUIFix now works on every 16:10 resolution.** The hooked value is a height-based UI scale (~ height/1440), not a fixed aspect constant, so forcing it to an absolute number only matched one resolution and zoomed the rest. The hook now multiplies that scale by `Multiplier`: a single `0.9` (= 9/10) maps any 16:10 mode onto its 16:9 equivalent (1920x1200, 2560x1600, ...), and `1.0` is an exact no-op.

### Changed
- `AspectRatioUIFix` default `Multiplier` is now `0.9` (the 16:10 fix); range `0.5-1.5`.
- Resolution-independent: the same `0.9` works at every 16:10 resolution. Opt-in (disabled by default); on 16:9 leave it off or at `1.0`.

## [1.1.3] - 2026-06-19
### Fixed
- **AspectRatioUIFix now reliably affects the UI on 16:10 displays.** Previously the hook only kicked in when the game emitted the exact 16:10 aspect constant (10/9 ~= 1.111); on setups where the game passed a slightly different value the tweak silently did nothing. While enabled, the configured `Multiplier` is now applied unconditionally to the UI-scale numerator.

### Changed
- **Wider `Multiplier` range** for AspectRatioUIFix: `0.9-1.2` (was `1.0-1.111`), so UI scale can be tuned both ways. Default stays `1.0`.
- **Overlay feedback:** moving a multiplier slider (GameplayFOV / CameraDistance / AspectRatioUIFix) now logs the new value to `JediSurvivorTweaks.log`.

## [1.1.2] - 2026-05-31
### Fixed
- **CVar resolution: more variables now found reliably.** Variables that previously failed to resolve (especially those registered via `FAutoConsoleVariableRef` static initializers) are now found correctly:
  - **Wider scan coverage**: scanner now also walks static registration tables in addition to the existing `.rdata`/`.text` reference pass — catches CVar patterns the old approach missed.
  - **Cross-DLL vtable validation**: `ValidateCVarObject` previously rejected objects whose vtable lived outside the main game module. Engine DLLs (renderer, audio, etc.) are loaded at separate addresses, so their vtables were incorrectly invalidated — fixed by checking committed memory instead of module bounds.
  - **`respawn.InterpolatedRendering` override**: this CVar uses a non-standard memory layout (value at offset `0x50` instead of the usual `0x48`); it now has a dedicated override entry so it is written correctly every time.
  - **Idle CPU fix**: pump thread now waits indefinitely when no CVars are pending, instead of polling on a fixed timer.
  - **Fast rejection**: CVars not present in the binary are dropped immediately on the first scan pass (no 30 s wait).

### Changed
- **Internals refactored.** CVarSystem split into focused subsystems (`cvar_scanner`, `cvar_resolver`, `cvar_layout`, `cvar_overrides`) — no user-visible behaviour change beyond the fixes above.

## [1.1.1] - 2026-05-26
### Fixed
- **UI Overlay**: fixed ImGui ID collisions that prevented multiple checkboxes within the same tweak section (e.g. Chromatic Aberration and Vignette in Graphical Tweaks) from being toggled interactively.

## [1.1.0] - 2026-05-26
### Added
- **ReShade Addon variant.** Loads as `JediSurvivorTweaks.addon64`. Registers an **Add-ons → JediSurvivorTweaks** panel inside the ReShade overlay (default key: **Home**).
  - Per tweak, the panel shows:
    - A **status dot** (green = loaded, red = disabled in INI or failed to install).
    - A **"Load on launch"** checkbox that writes `[Section] Enabled` to the INI for the next launch.
    - **Live runtime controls** that take effect instantly:
      - Multiplier sliders (GameplayFOV, CameraDistance, AspectRatioUIFix).
      - Sharpening on/off + strength slider; chromatic aberration / vignette / interpolated rendering toggles.
    - A per-tweak **Reset** button; a global **Reset to Defaults** at the bottom.
  - Changes autosave to `JediSurvivorTweaks.ini` after a 500 ms debounce (no Save button needed).
- New loader-agnostic core (`src/main_app.{hpp,cpp}`). ASI and ReShade entry points are minimal `DllMain` wrappers over `BootstrapAsync`.

### Changed
- `Config` now has two save policies (`PreserveComments` for the ASI variant; `Deterministic` for the ReShade autosave hot path).
- Various reliability fixes: bootstrap latch hardened to `std::atomic_flag`; `.tmp` orphan cleanup on Save failure; case-insensitive bool parsing in `ini_helpers`.
- The ASI variant is fully backwards-compatible — no INI key changes, same exports, same behaviour. Both variants now share the same `JediSurvivorTweaks.ini` schema.

## [1.0.1] - 2026-05-25
### Fixed
- **LetterboxPillarboxFix**: corrected hook offset; aspect ratio constraint flag is now cleared in game memory for all code paths; fixed resume address alignment.

## [1.0.0] - 2026-05-25
### Added
- Initial release of Jedi Survivor Tweaks.
  - `JediSurvivorTweaks.asi`
  - `JediSurvivorTweaks.ini`
