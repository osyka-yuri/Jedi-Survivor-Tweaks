# Features and configuration

The shipped `JediSurvivorTweaks.ini` is ready to use. Each feature has its own section; `Enabled = true` turns it on and `Enabled = false` leaves it off. Decimal values use a dot.

The ASI version reads the file at startup. The ReShade version also exposes supported controls in game and saves accepted edits automatically.

## Settings overview

| Section | Shipped value | Setting | Purpose |
| --- | --- | --- | --- |
| `LetterboxPillarboxFix` | On | `Enabled` | Removes forced black bars in supported cutscenes. |
| `AspectRatioUIFix` | Off | `Multiplier = 0.9` | Corrects oversized UI on 16:10 displays; `1.0` leaves the original scale unchanged. |
| `GameplayFOV` | Off | `Multiplier = 1.0` | Changes gameplay field of view. |
| `CameraDistance` | Off | `Multiplier = 1.0` | Moves the gameplay camera closer or farther away. |
| `GraphicalTweaks` | On | `Enabled` | Applies the configured sharpening, chromatic aberration, and vignette values at startup. When off, startup leaves all three game values untouched. |
| `Sharpening` | On | `Strength = 1.0` | Controls sharpening from `0.0` to `10.0`; use `0.0` to remove it. |
| `ChromaticAberration` | Off | `Enabled` | Enables or disables chromatic aberration. |
| `Vignette` | On | `Enabled` | Enables or disables vignette. |
| `InterpolatedRendering` | Off | `Enabled` | Opt-in frame interpolation intended to reduce CPU stutter and camera jitter. |
| `StreamingPoolFix` | On | `PoolSizeGB = auto` | Stabilizes the texture streaming pool to help prevent runaway VRAM usage. |
| `MaxFPS` | Off | `TargetFPS = 60` | Applies a frame-rate limit from `0` to `300`; `0` means uncapped. |

Multiplier controls accept values from `0.0` to `10.0`, except `AspectRatioUIFix`, which accepts `0.5` to `1.5`. Values outside documented ranges fall back to safe defaults and are reported in the log.

## Streaming pool

`PoolSizeGB = auto` is recommended. After the game begins running, the mod reads and locks the exact pool size selected by the game. If that one-time read is unavailable, fails, or reports a nonpositive value, Auto settles once on a runtime-only **2.00 GiB** fallback. The saved value remains `auto`: this does not retry, write the INI, or switch to Manual mode.

You can instead enter a number such as `2.0` to request a manual size in GiB. On a detected dedicated GPU the mod limits manual locks to 70% of physical VRAM. If the active GPU cannot be identified, manual mode keeps the compatible 12 GiB ceiling rather than guessing another adapter. GPU limits never clamp normal Auto values or the exceptional Auto fallback.

## Frame-rate limit

When MaxFPS is disabled at startup, the mod leaves the game's limit untouched. When enabled, its limit is applied after the game begins running and kept stable while startup finishes. Live enable and target changes apply `TargetFPS`; live disable sets `t.MaxFPS` to `0`, which is Unreal's uncapped value.

The ReShade overlay treats an accepted edit as the desired setting immediately. An amber status means the game command is still pending; red means it failed later, and the explanation is shown in the panel. A late failure does not silently rewrite your saved choice.

## ReShade overlay

The panel uses four status colors:

- **Green** — active or successfully applied.
- **Gray** — disabled until the next launch.
- **Amber** — waiting for the game to apply the setting.
- **Red** — setup or runtime failure; read the message below the feature name.

If an individual edit cannot be accepted immediately, its control returns to the previous value and the reason is written to the log. This does not turn the whole feature red because no pending game command was created.

Hook-based features must be loaded at launch. Their **Load on launch** checkbox changes the next-launch preference, while the live controls below it affect an already loaded feature. **Reset** restores one feature's built-in values; **Reset to Defaults** resets all visible controls.

The Graphical Tweaks launch switch is intentionally neutral when off: it does not force any post-processing value. Its individual ReShade controls remain available, and an explicit live edit changes only the selected setting.

## Custom Unreal Engine settings

Add numeric settings under `[CVars]`:

```ini
[CVars]
r.VSync = 0
r.D3D12.UseAllowTearing = 1
```

Integer, finite decimal, exponent, and boolean values are accepted. Booleans are normalized to `1` and `0`. String commands are intentionally unsupported. A newer value for the same name replaces an older pending value, and failed game commands are not retried automatically.

When a specialized control such as MaxFPS, Interpolated Rendering, or a graphical control manages the same CVar, the specialized control wins. The conflicting custom line is skipped with a warning in the log and overlay; other `[CVars]` entries still apply. If the specialized tweak is disabled at startup and leaves the value untouched, the custom entry is allowed.

Custom settings can change rendering, performance, or stability. Add them one at a time and keep a copy of the last known-good configuration.

## Logging

`[Logger] MinLevel` accepts `Debug`, `Info`, `Warning`, or `Error`. Use `Debug` temporarily when diagnosing a problem; `Info` is the normal default.
