#include <windows.h>
#include <filesystem>
#include <memory>

#include "core/logging.hpp"
#include "core/config.hpp"
#include "core/hook_engine.hpp"
#include "core/cvar_system.hpp"
#include "tweaks/tweak_manager.hpp"
#include "tweaks/letterbox_pillarbox_fix.hpp"
#include "tweaks/gameplay_fov.hpp"
#include "tweaks/camera_distance.hpp"
#include "tweaks/aspect_ratio_ui_fix.hpp"
#include "tweaks/graphical_tweaks.hpp"
#include "tweaks/interpolated_rendering.hpp"
#include "tweaks/custom_cvars.hpp"

namespace fs = std::filesystem;

class Application final {
public:
    explicit Application(const fs::path& baseDir);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] bool IsOk() const noexcept { return m_ok; }

private:
    bool m_ok = false;
    jst::core::Config m_config;
    jst::core::HookEngine m_hookEngine;
    jst::tweaks::TweakManager m_tweakManager;
};

Application::Application(const fs::path& baseDir) {
    auto logPath = baseDir / "JediSurvivorTweaks.log";
    if (!jst::core::Logger::Instance().Initialize(logPath)) {
        return;
    }
    JST_LOG_INFO("Jedi Survivor Tweaks bootstrap started.");
    JST_LOG_INFO("Base directory: '{}'.", baseDir.string());

    auto configPath = baseDir / "JediSurvivorTweaks.ini";
    if (!m_config.Load(configPath)) {
        JST_LOG_WARNING("Config not found or failed to load, using defaults.");
    }

    // Apply log level from config (optional [Logger] MinLevel = Debug|Info|Warn|Error).
    const auto minLevelStr = m_config.GetString("Logger", "MinLevel", "Info");
    jst::core::Logger::Instance().SetMinLevel(
        jst::core::ParseLogLevel(minLevelStr, jst::core::LogLevel::Info));

    // Independent pump: applies pending CVars asynchronously regardless of which
    // tweaks are enabled.
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
    jst::core::CVarSystem::Instance().StopPump();
    jst::core::Logger::Instance().Shutdown();
}

namespace {
    HMODULE g_hModule = nullptr;

    fs::path GetDllDirectory() {
        std::wstring path(MAX_PATH, L'\0');
        DWORD len = GetModuleFileNameW(g_hModule ? g_hModule : GetModuleHandleW(nullptr),
                                       path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) return {};
        path.resize(len);
        return fs::path(path).parent_path();
    }
} // anonymous namespace

extern "C" __declspec(dllexport) void InitializeASI() {
    static bool s_initialized = false;
    if (s_initialized) return;

    HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        static std::unique_ptr<Application> s_app;
        if (!s_app) {
            fs::path baseDir = GetDllDirectory();
            s_app = std::make_unique<Application>(baseDir);
            if (!s_app->IsOk()) {
                s_app.reset();
            }
        }
        return 0;
    }, nullptr, 0, nullptr);

    if (hThread) {
        // Only latch initialization on a successful spawn so a retry path
        // (e.g. a second InitializeASI invocation) still has a chance to run.
        s_initialized = true;
        CloseHandle(hThread);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        InitializeASI();
    }
    return TRUE;
}
