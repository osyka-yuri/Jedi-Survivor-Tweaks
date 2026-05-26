#pragma once

// Loader-agnostic core of the JediSurvivorTweaks plugin.
//
// Two loaders are supported (selected at build time via separate translation
// units, not preprocessor switches):
//   - ASI plugin (entry_asi.cpp)        -> `.asi` produced by `Release|x64`
//   - ReShade addon (reshade/entry.cpp) -> `.addon64` produced by `ReleaseAddon|x64`
//
// Both loaders end up calling `jst::BootstrapAsync(hModule)` from their DllMain
// attach handler. Everything below this point is loader-agnostic and shared.

#include <windows.h>
#include <filesystem>

#include "core/config.hpp"
#include "core/hook_engine.hpp"
#include "tweaks/tweak_manager.hpp"

namespace jst {

enum class LoaderVariant { Asi, ReShadeAddon };

/// Top-level orchestrator. Owns the lifetime of every subsystem (logger,
/// config, cvar pump, hook engine, tweak manager). Constructed exactly once
/// on a worker thread launched by `BootstrapAsync`.
class Application final {
public:
    explicit Application(const std::filesystem::path& baseDir, LoaderVariant variant);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] bool IsOk() const noexcept { return m_ok; }
    [[nodiscard]] LoaderVariant GetVariant() const noexcept { return m_variant; }

    // Extension points for loader-specific UI code (e.g. ReShade overlay).
    // Thread-safety note: the Application is "write-once during bootstrap,
    // read-many afterwards". Loader-specific code that calls these from the
    // render thread is safe once `GetRunningApplication()` has returned a
    // non-null pointer.
    [[nodiscard]] const jst::tweaks::TweakManager& GetTweakManager() const noexcept { return m_tweakManager; }
    [[nodiscard]] const jst::core::Config&         GetConfig()       const noexcept { return m_config; }

    // Non-const accessor for loader-specific code that needs to mutate config
    // values at runtime (e.g. the ReShade overlay writing slider/checkbox
    // changes back to JediSurvivorTweaks.ini via Config::Set*+Save).
    [[nodiscard]] jst::core::Config& GetConfigMutable() noexcept { return m_config; }

private:
    bool                          m_ok = false;
    LoaderVariant                 m_variant;
    jst::core::Config             m_config;
    jst::core::HookEngine         m_hookEngine;
    jst::tweaks::TweakManager     m_tweakManager;
};

/// Spawn a worker thread that constructs the Application exactly once. Safe
/// to call repeatedly -- subsequent calls are no-ops thanks to an internal
/// static latch.
///
/// `hModule` is the loading DLL's own handle (passed through from DllMain);
/// it is used only to derive the directory in which to look for the config
/// and write the log file.
///
/// `variant` selects which save policy the Application uses for the
/// shared `JediSurvivorTweaks.ini`:
///   - Asi          -> SaveMode::PreserveComments (Save() not called at runtime)
///   - ReShadeAddon -> SaveMode::Deterministic    (overlay autosaves on change)
void BootstrapAsync(HMODULE hModule, LoaderVariant variant);

/// Resolve the directory of the given module. Used by `BootstrapAsync` to
/// place the log / config alongside the loaded DLL/ASI/addon file. Exposed
/// in the header so loader-specific code (or future telemetry) can reuse it.
[[nodiscard]] std::filesystem::path GetModuleDirectory(HMODULE hModule);

/// Pointer to the running Application after a successful bootstrap, or
/// `nullptr` if bootstrap hasn't completed (or failed).
///
/// Used by loader-specific UI code (e.g. ReShade overlay) to introspect tweak
/// state and tweak the runtime context. The returned pointer is valid for the
/// remainder of the process lifetime once it becomes non-null.
[[nodiscard]] Application* GetRunningApplication() noexcept;

} // namespace jst
