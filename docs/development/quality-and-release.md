# Quality and release

A release candidate must pass automated checks for both loader configurations. Runtime behavior remains a separate smoke-test because build and PE compatibility checks cannot prove behavior inside the running game.

## Local quality gates

Run from a Visual Studio Developer PowerShell:

```powershell
git diff --check

npm ci
npm run docs:check

msbuild JediSurvivorTweaks.sln /p:Configuration=Debug /p:Platform=x64 /t:Rebuild
& .\x64\Debug\tests\JediSurvivorTweaks.Tests.exe

msbuild JediSurvivorTweaks.sln /p:Configuration=Release /p:Platform=x64 /t:Rebuild
& .\x64\Release\tests\JediSurvivorTweaks.Tests.exe

msbuild JediSurvivorTweaks.sln /p:Configuration=ReleaseAddon /p:Platform=x64 /t:Rebuild
& .\x64\ReleaseAddon\tests\JediSurvivorTweaks.Tests.exe
```

The documentation gate enforces the shared Markdown style and validates local paths, path casing, heading anchors, and image alternative text. All three rebuilds must finish with zero first-party warnings. Every matching test executable must pass. Concurrency tests use bounded synchronization and must never rely on unbounded busy-wait loops.

Review `git status`, `git diff --stat`, and `git diff --check` before committing. Do not include generated binaries, local logs, game files, or unrelated line-ending changes.

## Read-only game verification

When the supported executable is installed locally, run:

```powershell
.\scripts\verify_supported_game.ps1 -ExePath "D:\path\to\JediSurvivor.exe"
```

Record the timestamp, image size, Tick RVA, signature uniqueness, and executable section result. Do not deploy a newly built plugin into the game merely to complete this automated gate.

## Runtime smoke-test

When a release is explicitly approved for in-game testing, verify at minimum:

- The first post-Tick barrier opens before the main menu.
- Startup-disabled MaxFPS and InterpolatedRendering perform no CVar work.
- Startup-enabled MaxFPS applies on the first post-Tick pass; value changes during the following 15 seconds are detected and corrected, while `LastSetBy`-only changes are ignored.
- Live MaxFPS enable, target update, uncapped disable, and pending/error overlay states behave as documented.
- Streaming pool Auto remains unforced until its one-shot post-Tick read, then locks the exact `r.Streaming.PoolSize` value selected by the game. Verify a rejected, invalid, or nonpositive read reports `Auto fallback: 2.00 GB` without retrying, persisting, or changing to Manual.
- Hook-based display and camera features work at their supported resolutions.
- ReShade and ASI are tested independently when both loaders are available.

Document that runtime behavior is unconfirmed whenever this smoke-test was not performed.

## Documentation policy

- The root README explains the product, editions, shortest installation path, compatibility expectations, and documentation links.
- User guides explain observable behavior and decisions without implementation internals.
- Developer guides preserve architecture and verification contracts.
- `npm run docs:check` must pass for the root README, contributing guide, and every page under `docs/`.
- The changelog records user-visible additions, changes, fixes, compatibility notes, and only the maintenance details contributors need to act on.
- The shipped INI stays concise enough to edit in place; extended rationale belongs in the configuration guide.

## CI and packaging

CI validates the documentation once, then the Windows matrix rebuilds and tests `Release|x64` and `ReleaseAddon|x64`. The release workflow repeats the documentation gate before packaging. Release packaging keeps ASI and ReShade archives separate, each with its matching plugin and the shared default INI.

Release notes should call out loader requirements, game-build compatibility, new defaults, changed INI keys, and any behavior that cannot be reverted during the current process.
