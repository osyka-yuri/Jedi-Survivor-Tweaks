<div align="center">
  <h1>Jedi Survivor Tweaks</h1>
  <p><strong>Fix display issues, tune the camera and graphics, and keep performance settings under control in Star Wars Jedi: Survivor.</strong></p>
  <p>One lightweight Windows mod, available as an ASI plugin or a ReShade add-on with live controls.</p>
  <h3><a href="https://github.com/osyka-yuri/Jedi-Survivor-Tweaks/releases/latest">Download the latest release</a></h3>
  <p>
    <a href="docs/user/installation.md">Installation guide</a>
    · <a href="https://github.com/osyka-yuri/Jedi-Survivor-Tweaks/releases">All releases</a>
  </p>
  <p>
    <a href="https://github.com/osyka-yuri/Jedi-Survivor-Tweaks/releases"><img src="https://img.shields.io/github/v/release/osyka-yuri/Jedi-Survivor-Tweaks?display_name=tag&sort=semver&style=flat-square" alt="Latest release"></a>
    <img src="https://img.shields.io/badge/Windows-x64-0078d4?style=flat-square&logo=windows11&logoColor=white" alt="Windows x64">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-4a9eff?style=flat-square" alt="MIT license"></a>
  </p>
  <p>Official downloads are published only through GitHub Releases.</p>
</div>

## What it improves

- **Ultrawide and 16:10 presentation** — removes cutscene black bars and corrects oversized interface elements on 16:10 displays.
- **Camera control** — adjusts gameplay field of view and camera distance.
- **Cleaner image options** — controls sharpening, chromatic aberration, and vignette.
- **Performance tuning** — limits frame rate, offers opt-in interpolated rendering, and stabilizes the streaming pool to help prevent runaway VRAM usage.
- **Advanced customization** — applies additional numeric Unreal Engine settings from the configuration file.
- **Live tuning with ReShade** — changes supported settings in game and saves them automatically.

The included configuration provides conservative starting values and short explanations beside each setting.

## Choose a version

Both downloads provide the same fixes and use the same `JediSurvivorTweaks.ini` file. Install only one.

| Version | Best for | How it is controlled |
| --- | --- | --- |
| **ReShade Addon** | Most users who already use ReShade or want live tuning | ReShade's in-game Add-ons panel and the INI file |
| **ASI plugin** | A minimal setup without an in-game panel | The INI file |

The ReShade version requires a ReShade build with full add-on support. The ASI version requires Ultimate ASI Loader. See the [installation guide](docs/user/installation.md) for exact paths and loader setup.

## Get started

1. Download the ASI or ReShade archive from the [latest release](https://github.com/osyka-yuri/Jedi-Survivor-Tweaks/releases/latest).
2. Follow the matching loader instructions in the [installation guide](docs/user/installation.md).
3. Place the plugin and `JediSurvivorTweaks.ini` in the documented game or loader folder.
4. Start the game. ReShade users can open the overlay with the **Home** key and select **Add-ons → JediSurvivorTweaks**.

Start with the shipped defaults, then use the [configuration guide](docs/user/configuration.md) when you want to tune individual features.

## Compatibility and safety

Jedi Survivor Tweaks is built for Windows x64 and checks the supported game version before applying settings that depend on it. A game update can require a matching mod update; incompatible functionality stays disabled and writes an explanation to `JediSurvivorTweaks.log`.

Custom Unreal Engine settings are intended for experienced users and can affect visuals, performance, or stability. The mod runs locally and does not require an account, network connection, or telemetry.

See [Troubleshooting and removal](docs/user/troubleshooting.md) if the mod does not load, a status indicator turns red, or the game was recently updated.

## Documentation and project

- [User guide](docs/README.md#user-guide) — installation, configuration, overlay behavior, troubleshooting, updates, and removal.
- [Developer guide](docs/README.md#developer-guide) — build setup, architecture, compatibility checks, tests, and release quality gates.
- [Contributing](CONTRIBUTING.md) — the shortest path from a local checkout to a reviewable change.
- [Issues](https://github.com/osyka-yuri/Jedi-Survivor-Tweaks/issues) and [changelog](CHANGELOG.md) — report problems and review release history.
- [Support development on Boosty](https://boosty.to/osyka.yuri/donate) — an optional way to support the project.
- [MIT license](LICENSE) — terms for using, modifying, and distributing the project.

For installation and update safety, download Jedi Survivor Tweaks only from this repository's GitHub Releases page. ReShade, Ultimate ASI Loader, and the game remain independent third-party software subject to their own licenses and support policies.
