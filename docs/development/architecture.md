# Architecture

Jedi Survivor Tweaks has two thin loader entry points over one process-lifetime application core. The ASI and ReShade builds differ only at loading and UI boundaries; tweak behavior, configuration, hooks, logging, and game integration are shared.

## Source ownership

```text
src/
├── core/       configuration, logging, hooks, dispatcher, CVar system, GPU services
├── hooks/      generated slot registry, shared hook context, MASM detours
├── tweaks/     feature configuration, hook bindings, runtime controls, policies
├── reshade/    ReShade entry point, overlay, and control rendering
├── external/   vendored Zydis and ReShade SDK sources
├── main_app.*  shared application bootstrap and shutdown orchestration
└── entry_asi.cpp
```

## Tweak lifecycle

`ITweak::Configure` returns only whether a tweak is enabled. Each concrete tweak retains its typed settings. `Prepare` registers required infrastructure, followed by resolution and installation finalization. `TweakManager` owns the public lifecycle stage and diagnostic.

Only successfully prepared tweaks receive shutdown, in reverse preparation order. This includes a tweak that later failed during resolution or installation. Hook tweaks are launch-gated; runtime-capable CVar tweaks can remain visible while their effect is off.

Runtime callbacks return `Unchanged`, `Applied`, `Queued`, or `Rejected`. The overlay persists `Applied` and `Queued`, rolls a rejected control back, and records its diagnostic in the log. Ticket-backed pending or failure state remains separate from the desired configuration. Immediate rejection never changes the persistent tweak status.

## Game-thread execution

Binary scanning and CVar object resolution run away from the game thread. Engine object reads, setters, and user callbacks run only from the post-`FEngineLoop::Tick` dispatcher.

The first completed Tick identifies the game thread and opens the only startup barrier. Queued commands and one-shot reads are then executed from post-Tick passes. Every unresolved name cohort accepted before that barrier receives the same immutable deadline at the barrier; a later cohort receives its own acceptance-time deadline, and attaching more work never extends either. Resolver terminal errors extract the cohort and schedule its callbacks exactly once for the next post-Tick pass; callbacks are never invoked by the resolver.

A bounded startup reconciler compares the stored value of each startup-written CVar every 100 ms for 15 seconds. It reapplies only values that changed, never retries a failed setter, and does not inspect `LastSetBy` when deciding whether to act. The target set is frozen at the first Tick; an explicit later command cancels startup monitoring for that CVar. Per-target permits make a live command win before a correction setter begins, or cause one current live command to run in the same post-Tick tail when it arrives during that setter.

The CVar cache, override table, reconciler, and managed-claim set use one ASCII case-insensitive name contract. `ResolvedCVar` stores the object, string setter, and declarative read layout. External references are dereferenced again before every read. Object, vtable setter identity, and read address are validated immediately before use.

Ordinary CVar commands are one-shot and latest-value-wins. Queue acceptance produces a ticket; the later Unreal call completes it as applied or failed. Public write requests are typed (`wstring`, `int32`, or finite `float`), then serialized atomically for the complete batch before the cache mutex is acquired. A failed setter is not retried. Setters and callbacks execute without the cache mutex, and generation checks protect reentrant replacement.

`Superseded` is the neutral completion of a stale command, not a failure. Runtime status follows only the latest ticket owned by a control. Related managed settings use an all-or-none admission batch; individual Unreal setters still complete independently and retain their own diagnostics.

Specialized controls claim the CVar names they manage for the current process. A managed command replaces an older pending custom command. A conflicting `[CVars]` item is skipped with a warning, while all non-conflicting custom items continue through the ordinary per-item queue. A specialized tweak that is disabled at startup and issues no command makes no claim.

## MaxFPS commands

MaxFPS uses the same one-shot, latest-value-wins command path as other CVar settings. Startup enable queues the selected target for the first post-Tick pass and participates in the bounded startup reconciliation window. Live target changes use the normal next-post-Tick path, and live disable queues `0` (uncapped). Startup disable and process shutdown perform no engine write.

`GraphicalTweaks.Enabled = false` is also neutral at startup: it queues no sharpening, chromatic-aberration, or tonemapper writes. ReShade may still issue an individual managed command when the user explicitly edits one of those live controls.

## Streaming pool startup

In Auto mode the streaming hook is installed with no forced size. A one-shot read captures `r.Streaming.PoolSize` on the first post-Tick pass after its CVar object is resolved and publishes that exact engine-selected value to the hook payload. A rejected, failed, invalid, or nonpositive one-shot read terminates Auto once at a runtime-only 2 GiB fallback; it is not persisted, retried, or converted to Manual. Runtime Manual-to-Auto changes retain the previous lock only until that generation completes. GPU-aware limits and adapter updates apply only to explicit Manual values. The C++/MASM payload contains only an aligned `forcedBytes` word; there is no watch, path sample, or ASM policy state.

## Dispatcher and shutdown

The CVar runtime coordinator owns registration, resolution, installation, rollback, and shutdown of the Tick dispatcher. A dispatcher failure removes its service hook and marks CVar access unavailable; independent tweak hooks continue through their own lifecycle.

The Tick dispatcher counts admitted callbacks independently from detour entry. Returning C++ detours use a process-lifetime gateway gate: one atomic state transition either admits and counts the call before it can enter DLL code, or bypasses the closed detour through the retained original trampoline. Shutdown closes admission, restores the splice through the suspended-thread patching path, and waits for every admitted call. Unregistration preserves the hook and propagates the error if the original bytes cannot be restored.

Application shutdown follows this order:

1. Stop publishing the application to UI consumers.
2. Close dispatcher callback admission.
3. Stop background graphics services.
4. Shut down tweaks in reverse order without issuing shutdown CVar writes.
5. Let the CVar coordinator remove the dispatcher hook and stop/join the CVar system.
6. Remove remaining independent hooks.
7. Close the logger.

## Configuration persistence

Parsing trims centrally, requires full numeric consumption, rejects non-finite values, and applies documented ranges. Custom CVar booleans normalize to `1` or `0`; other values remain numeric-only.

Saving creates a unique sibling temp file, flushes it through `FlushFileBuffers`, closes it, and publishes it atomically with `ReplaceFileW` or `MoveFileExW`. Replacing an existing file requests an explicit sibling backup because `ReplaceFileW` may move operands before reporting failure. A classified failure restores the original and removes disposable artifacts; an ambiguous failure preserves every possible recovery copy and logs its exact paths instead of deleting potentially unique data.

## Maintained boundaries

- No engine API call is allowed from the resolver thread.
- No cache mutex may be held across an engine setter or user callback.
- Startup CVar setters and one-shot reads must remain closed until the first completed Tick.
- Startup reconciliation is value-only, bounded to 15 seconds, and excludes specialized CVar owners.
- Startup-disabled MaxFPS and InterpolatedRendering must not create resolver work.
- Shutdown does not issue an extra MaxFPS command.
- Incompatible executable detection fails CVar access closed without disabling independent hook tweaks.
