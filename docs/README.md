# Jedi Survivor Tweaks documentation

This directory is the documentation hub for Jedi Survivor Tweaks. Start with the user guide if you installed a release or want to change settings. Use the developer guide when building from source, changing runtime contracts, or preparing a release.

## User guide

- [Installation and updates](user/installation.md) explains how to choose between the ASI and ReShade versions, where to place files, and how to update safely.
- [Features and configuration](user/configuration.md) documents every setting, the shipped values, live-control behavior, and the custom CVar section.
- [Troubleshooting and removal](user/troubleshooting.md) covers loader problems, overlay status indicators, logs, game updates, clean removal, and common configuration surprises.

## Developer guide

- [Development setup](development/setup.md) covers the Windows toolchain, both build configurations, outputs, and local tests.
- [Architecture](development/architecture.md) describes loader boundaries, tweak lifecycle, game-thread CVar execution, managed overrides, shutdown, and source ownership.
- [Quality and release](development/quality-and-release.md) lists required builds, tests, repository checks, executable compatibility validation, and runtime smoke-test boundaries.

Public behavior belongs in the user guide. Maintained engineering contracts belong in the developer guide. Private implementation details should remain close to the code instead of being duplicated here.

## Sources of truth

- [Default configuration](../config/JediSurvivorTweaks.ini)
- [CI workflow](../.github/workflows/ci.yml)
- [Project file](../JediSurvivorTweaks.vcxproj)
- [Changelog](../CHANGELOG.md)
