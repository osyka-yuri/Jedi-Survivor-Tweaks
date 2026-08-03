# Troubleshooting and removal

The main diagnostic file is `JediSurvivorTweaks.log` beside the active INI. Start there after a game update, loader change, red overlay status, or setting that does not apply.

## The mod does not load

- Install only one plugin variant.
- Confirm the ASI loader is in `SwGame\Binaries\Win64` and the INI is beside the `.asi`, or that the `.addon64` and its INI are together in ReShade's active add-on path.
- For ReShade, use the build with full add-on support and check the **Add-ons** tab.
- Ensure the plugin and INI came from the same release archive.
- Check whether security software quarantined the loader or plugin.

## A feature is disabled or missing live controls

Hook-based features are selected at launch. Changing **Load on launch** in the ReShade panel saves the preference for the next game start; it cannot install or remove a code hook in the current session.

If a status is amber, wait for the pending game command. If it turns red, read the displayed message and the log. Repeatedly toggling the setting does not repair an incompatible executable or unavailable game setting.

## The game was updated

Game updates can change the parts of the game used by the mod. Version-dependent settings stay disabled when the executable does not match the supported build. Download a newer mod release if one is available; do not try to bypass the compatibility check.

Contributors and users with a source checkout can run its read-only check without starting or modifying the game:

```powershell
.\scripts\verify_supported_game.ps1 -ExePath "D:\path\to\JediSurvivor.exe"
```

## The INI lost comments

This is expected after the ReShade overlay saves a change. The overlay writes a clean deterministic file and does not preserve comments or custom ordering. The ASI version reads the file without rewriting it.

Keep the shipped INI or the [configuration guide](configuration.md) nearby if you prefer editing settings by hand.

## Clean removal

Close the game, then delete `JediSurvivorTweaks.asi` or `JediSurvivorTweaks.addon64`. Remove the ASI loader only if no other installed mod needs it. The INI and log can be kept for a future reinstall or deleted.

Most settings disappear with the plugin. A MaxFPS value already applied in the current process is not rewritten during shutdown, but it does not create a permanent game-file modification. Use live disable before removal if you want to set `t.MaxFPS` to its uncapped value immediately.
