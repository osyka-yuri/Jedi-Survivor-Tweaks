# Contributing to Jedi Survivor Tweaks

Thank you for improving Jedi Survivor Tweaks. Keep changes focused, explain user-visible behavior, and preserve the compatibility and shutdown boundaries around in-process game modifications.

## Before coding

1. Read the [development setup](docs/development/setup.md) and use the configured Windows x64 toolchain.
2. Review the [architecture](docs/development/architecture.md) before changing hook ownership, game-thread execution, runtime state, or loader boundaries.
3. For configuration, compatibility, testing, or packaging changes, follow the relevant contract in the [developer guide](docs/README.md#developer-guide).
4. Prefer an issue before a large behavioral or architectural change. Do not include copyrighted game files, private symbols, local logs, or third-party binaries in a contribution.

## Quality gates

Run the checks that match your change. Before opening a pull request, the expected complete set is documented in [Quality and release](docs/development/quality-and-release.md#local-quality-gates). It covers documentation validation, all three build configurations, unit tests, and the optional read-only supported-game check.

Add focused tests for changed contracts and failure cases. Update user documentation when observable behavior changes and developer documentation when a maintained boundary changes. Private implementation details belong close to the code instead of being duplicated in guides.

## Pull requests

Describe the problem, the chosen behavior, compatibility or runtime-safety implications, and the verification performed. Keep generated hook-slot constants synchronized with their generator, and keep ASI- and ReShade-specific code at their loader boundaries when shared behavior does not need to differ.

## Sources of truth

- [Developer documentation](docs/README.md#developer-guide)
- [CI workflow](.github/workflows/ci.yml)
- [Visual Studio project](JediSurvivorTweaks.vcxproj)
