# Development setup

Jedi Survivor Tweaks is a Windows x64 C++23 project with MASM hook stubs. Visual Studio Build Tools provide the supported command-line workflow.

## Toolchain

- Visual Studio or Build Tools with MSVC, Windows SDK, and MASM
- Platform toolset v145 as configured by the project; v143 remains compatible when selected locally
- C++ latest language mode
- PowerShell for generated hook slots and compatibility scripts

Run builds from a Visual Studio Developer PowerShell, or invoke the installed `MSBuild.exe` directly.

## Build both loaders

```powershell
msbuild JediSurvivorTweaks.sln /p:Configuration=Release /p:Platform=x64 /t:Rebuild
msbuild JediSurvivorTweaks.sln /p:Configuration=ReleaseAddon /p:Platform=x64 /t:Rebuild
```

Outputs:

- `x64\Release\JediSurvivorTweaks.asi`
- `x64\ReleaseAddon\JediSurvivorTweaks.addon64`

The configurations share the same core and compile different loader entry points. Do not introduce loader conditionals into shared behavior when a boundary-specific translation unit is sufficient.

## Tests

Each release configuration produces its own test executable:

```powershell
& .\x64\Release\tests\JediSurvivorTweaks.Tests.exe
& .\x64\ReleaseAddon\tests\JediSurvivorTweaks.Tests.exe
```

First-party C++ builds with C++ latest, warning level 4, SDL checks, and warnings as errors. These settings and the shared include roots live in `JediSurvivorTweaks.Common.props`; vendored dependencies keep their own warning policy. The executable entry point is isolated in `tests/test_main.cpp`, while each test translation unit exposes one focused suite.

## Executable compatibility check

The compatibility script reads PE metadata and code-unwind records without launching or changing the game:

```powershell
.\scripts\verify_supported_game.ps1 -ExePath "D:\path\to\JediSurvivor.exe"
```

It validates the supported timestamp and image size, requires one matching Tick signature, checks the expected Tick RVA, and confirms that the target belongs to an executable section. This is not a runtime smoke-test.

## Repository line endings

`.editorconfig` and `.gitattributes` define LF for project text while preserving vendored and generated files that must remain byte-for-byte stable. Do not run a repository-wide renormalization as part of an unrelated change. Review the content diff and keep metadata-only paths out of commits.

## Sources of truth

- [Visual Studio project](../../JediSurvivorTweaks.vcxproj)
- [Test project](../../tests/JediSurvivorTweaks.Tests.vcxproj)
- [CI workflow](../../.github/workflows/ci.yml)
- [Hook slot generator](../../scripts/generate_hook_slots.ps1)
