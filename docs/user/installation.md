# Installation and updates

Jedi Survivor Tweaks is distributed for Windows x64 in two alternative packages. Both contain the same features and use `JediSurvivorTweaks.ini`; install only one version at a time.

Official downloads are published on the project's [GitHub Releases](https://github.com/osyka-yuri/Jedi-Survivor-Tweaks/releases) page.

## ReShade Addon

Choose this version if you want an in-game control panel or already use ReShade.

1. Install the ReShade build with **full add-on support**. The standard signed build does not load third-party add-ons.
2. Copy `JediSurvivorTweaks.addon64` next to the ReShade DLL loaded by the game, or into the folder configured by `AddonPath` in `ReShade.ini`.
3. Copy `JediSurvivorTweaks.ini` to the same location as the add-on.
4. Start the game and open ReShade with the **Home** key.
5. Open **Add-ons → JediSurvivorTweaks**. The add-on should also appear in ReShade's installed add-ons list.

The overlay applies supported live controls immediately and saves accepted changes after a short delay. It rewrites the INI in a deterministic format, so comments and custom ordering are not preserved after an overlay save.

## ASI plugin

Choose this version for a smaller file-based setup without an in-game panel.

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). Its `xinput1_3.dll` variant is the recommended choice for this game.
2. Place the loader DLL in:

   ```text
   <Jedi Survivor>\SwGame\Binaries\Win64
   ```

3. Copy `JediSurvivorTweaks.asi` into that folder or its `plugins` subfolder.
4. Copy `JediSurvivorTweaks.ini` beside `JediSurvivorTweaks.asi`. If the plugin is in `plugins`, the INI belongs there too.
5. Start the game. Edit the INI before the next launch when you want to change a setting.

## Updating

Close the game, download the newer archive for the same loader, and replace the plugin file. Compare the new default INI with your existing settings before replacing it. Release notes call out changes to defaults or configuration keys.

Do not copy build outputs from a source checkout into the game unless you intentionally want to test an unreleased build. Published releases are the supported installation path.

## Switching versions

Remove the currently installed `.asi` or `.addon64`, then install the other package. Keep one copy of your INI and move it to the location used by the new loader. Do not leave both plugin variants active: they would attempt to apply the same tweaks twice.
