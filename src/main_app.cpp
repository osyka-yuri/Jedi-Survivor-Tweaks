#include "main_app.hpp"

#include "core/logging.hpp"
#include "core/cvar_system.hpp"
#include "tweaks/letterbox_pillarbox_fix.hpp"
#include "tweaks/gameplay_fov.hpp"
#include "tweaks/camera_distance.hpp"
#include "tweaks/aspect_ratio_ui_fix.hpp"
#include "tweaks/graphical_tweaks.hpp"
#include "tweaks/interpolated_rendering.hpp"
#include "tweaks/custom_cvars.hpp"

#include <atomic>
#include <memory>

namespace fs = std::filesystem;

namespace jst {

namespace {
    // Published only after a successful bootstrap. Loader-specific UI code
    // reads this via GetRunningApplication().
    std::atomic<Application*> g_runningApp{nullptr};

    // Process-lifetime owner of the singleton Application. Lives in the
    // anonymous namespace (rather than inside the worker thread function)
    // so its destruction during DLL unload is unambiguous to readers.
    std::unique_ptr<Application> g_app;

    // One-shot latch for BootstrapAsync. test_and_set returns the previous
    // value; first caller sees false and proceeds, subsequent callers bail
    // out. On bootstrap failure we clear the flag so a higher-level retry
    // can still try again.
    std::atomic_flag g_bootstrapStarted = ATOMIC_FLAG_INIT;
} // anonymous namespace

Application::Application(const fs::path& baseDir, LoaderVariant variant)
    : m_variant(variant) {
    auto logPath = baseDir / "JediSurvivorTweaks.log";
    if (!jst::core::Logger::Instance().Initialize(logPath)) {
        return;
    }
    JST_LOG_INFO("Jedi Survivor Tweaks bootstrap started.");
    JST_LOG_INFO("Base directory: '{}'.", baseDir.string());
    JST_LOG_INFO("Loader variant: {}.", variant == LoaderVariant::Asi ? "ASI" : "ReShade Addon");

    // Both loader variants read and write the same JediSurvivorTweaks.ini.
    // The mod ships in two separate archives (ASI / ReShade), each with its
    // own preconfigured copy of the file, so they can't get out of sync
    // unless the user manually mixes them. The save policy differs:
    //   - ASI variant: SaveMode::PreserveComments. The ASI core never calls
    //     Save() today, so the human-edited file is read-only in practice.
    //   - ReShade variant: SaveMode::Deterministic. The overlay autosaves on
    //     every slider/checkbox change with a 500 ms debounce; comments are
    //     stripped on the first save (acceptable -- the overlay carries the
    //     same documentation via tooltips, and the file is regenerated from
    //     the in-memory cache, not patched in place).
    const auto configPath = baseDir / "JediSurvivorTweaks.ini";
    const auto saveMode = (variant == LoaderVariant::Asi)
                            ? jst::core::Config::SaveMode::PreserveComments
                            : jst::core::Config::SaveMode::Deterministic;
    if (!m_config.Load(configPath, saveMode)) {
        JST_LOG_WARNING("Config not found or failed to load, using defaults: '{}'.",
                        configPath.string());
    }

    // Apply log level from config (optional [Logger] MinLevel = Debug|Info|Warn|Error).
    const auto minLevelStr = m_config.GetString("Logger", "MinLevel", "Info");
    jst::core::Logger::Instance().SetMinLevel(
        jst::core::ParseLogLevel(minLevelStr, jst::core::LogLevel::Info));

    jst::core::CVarSystem::Instance().StartPump();

    m_tweakManager.RegisterTweak<jst::tweaks::LetterboxPillarboxFix>();
    m_tweakManager.RegisterTweak<jst::tweaks::GameplayFOV>();
    m_tweakManager.RegisterTweak<jst::tweaks::CameraDistance>();
    m_tweakManager.RegisterTweak<jst::tweaks::AspectRatioUIFix>();
    m_tweakManager.RegisterTweak<jst::tweaks::GraphicalTweaks>();
    m_tweakManager.RegisterTweak<jst::tweaks::InterpolatedRenderingTweak>();
    m_tweakManager.RegisterTweak<jst::tweaks::CustomCVarsTweak>();

    auto initRes = m_tweakManager.Initialize(m_hookEngine, m_config);
    if (!initRes) {
        JST_LOG_ERROR("Failed to initialize TweakManager: '{}'.", initRes.error());
        return;
    }

    m_ok = true;
}

Application::~Application() {
    m_tweakManager.Shutdown();
    m_hookEngine.Shutdown();
    jst::core::Logger::Instance().Shutdown();
}

fs::path GetModuleDirectory(HMODULE hModule) {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD len = GetModuleFileNameW(hModule ? hModule : GetModuleHandleW(nullptr),
                                         path.data(), static_cast<DWORD>(path.size()));
    if (len == 0) return {};
    path.resize(len);
    return fs::path(path).parent_path();
}

namespace {
    struct BootstrapArgs {
        HMODULE hModule;
        LoaderVariant variant;
    };
} // anonymous namespace

void BootstrapAsync(HMODULE hModule, LoaderVariant variant) {
    // Atomic CAS-style latch: first caller proceeds, the rest bail. On any
    // failure below we clear the flag so a retry path remains open.
    if (g_bootstrapStarted.test_and_set(std::memory_order_acq_rel)) return;

    auto* args = new BootstrapArgs{hModule, variant};
    const HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        auto* args = static_cast<BootstrapArgs*>(param);
        fs::path baseDir = GetModuleDirectory(args->hModule);
        g_app = std::make_unique<Application>(baseDir, args->variant);
        if (!g_app->IsOk()) {
            g_app.reset();
            g_bootstrapStarted.clear(std::memory_order_release);
            delete args;
            return 0;
        }
        // Publish for loader-specific UI consumers (e.g. overlay).
        g_runningApp.store(g_app.get(), std::memory_order_release);
        delete args;
        return 0;
    }, args, 0, nullptr);

    if (hThread) {
        CloseHandle(hThread);
    } else {
        // CreateThread failed; release the args and the latch so a retry
        // is possible.
        delete args;
        g_bootstrapStarted.clear(std::memory_order_release);
    }
}

Application* GetRunningApplication() noexcept {
    return g_runningApp.load(std::memory_order_acquire);
}

} // namespace jst
