#include "streaming_pool_fix.hpp"

#include "core/config.hpp"
#include "core/cvar_system.hpp"
#include "core/logging.hpp"
#include "hooks/tweak_hooks.hpp"
#include "runtime_control.hpp"
#include "slider_utils.hpp"

#include <chrono>
#include <utility>

namespace {

constexpr const char* kPattern =
    "40 E8 ?? ?? ?? ?? 48 8B 54 24 40 84 C0 74 16 48 8B 83 08 01 00 00 48 2B 44 24 48 48 03 C2 48 89";
constexpr int32_t kOffset = 0x6;
constexpr std::wstring_view kPoolSizeCVar = L"r.Streaming.PoolSize";
constexpr std::chrono::milliseconds kAutoWatchTimeout{30'000};

[[nodiscard]] jst::tweaks::PoolSizeSetting LoadPoolSizeSetting(
    const jst::core::Config& config, std::string_view section) {
    return jst::tweaks::ParsePoolSizeGb(config.GetString(
        section,
        jst::tweaks::kPoolSizeGbConfigKey,
        jst::tweaks::kPoolSizeAutoLiteral));
}

} // namespace

namespace jst::tweaks {

StreamingPoolFix::StreamingPoolFix()
    : HookTweak(
          "StreamingPoolFix",
          "Locks the streaming pool to a safe engine or manual size. "
          "The maximum is 70% of detected dedicated GPU memory.",
          true,
          HookTarget::Pattern(kPattern, kOffset, 16),
          reinterpret_cast<std::uintptr_t>(&StreamingPoolFix_Detour),
          jst::hooks::Slot::StreamingPoolFix) {}

void StreamingPoolFix::OnConfigLoaded(jst::core::Config& config) {
    m_setting = LoadPoolSizeSetting(config, Name());
}

void StreamingPoolFix::WritePoolSizeGb(jst::core::Config& config) const {
    config.SetString(Name(), kPoolSizeGbConfigKey, FormatPoolSizeGb(m_setting));
}

void StreamingPoolFix::LogPolicy(const StreamingPoolSnapshot& snapshot) const {
    if (snapshot.policy.dedicatedVideoMemoryBytes) {
        JST_LOG_INFO(
            "StreamingPoolFix | detected {:.1f} GB dedicated VRAM; "
            "safe pool maximum {:.1f} GB (70%)",
            PoolSizeBytesToGb(*snapshot.policy.dedicatedVideoMemoryBytes),
            snapshot.policy.limits.MaximumGb());
    } else {
        JST_LOG_WARNING(
            "StreamingPoolFix | game GPU VRAM is unavailable; "
            "using legacy 12.0 GB ceiling");
    }
}

void StreamingPoolFix::LogAutoLock(const StreamingPoolSnapshot& snapshot) const {
    if (snapshot.state == StreamingPoolState::LockedFromCVar) {
        JST_LOG_INFO(
            "StreamingPoolFix | auto locked at {:.2f} GB (engine-reported {} MB)",
            snapshot.effectiveGb,
            snapshot.enginePoolMb);
    } else if (snapshot.state == StreamingPoolState::LockedFromPathSample) {
        JST_LOG_INFO(
            "StreamingPoolFix | auto locked at {:.2f} GB (streaming path)",
            snapshot.effectiveGb);
    }
}

void StreamingPoolFix::ApplySetting() {
    StopEngineWatch();
    m_lastLoggedRejectedEngineMb.store(0, std::memory_order_relaxed);

    if (m_setting.IsAuto()) {
        m_controller.ArmAuto();
        const auto snapshot = m_controller.Snapshot();
        StartEngineWatch();
        JST_LOG_INFO(
            "StreamingPoolFix | auto: waiting for r.Streaming.PoolSize "
            "(then path sample or {:.1f} GB fallback)",
            snapshot.policy.limits.FallbackGb());
        return;
    }

    m_controller.ArmManual(m_setting.requestedManualGb);
    const auto snapshot = m_controller.Snapshot();
    if (SliderValuesNearlyEqual(
            snapshot.effectiveGb, m_setting.requestedManualGb)) {
        JST_LOG_INFO("StreamingPoolFix | manual: {:.1f} GB", snapshot.effectiveGb);
    } else {
        JST_LOG_INFO(
            "StreamingPoolFix | manual: {:.1f} GB (requested {:.1f} GB)",
            snapshot.effectiveGb,
            m_setting.requestedManualGb);
    }
}

void StreamingPoolFix::StartEngineWatch() {
    jst::core::IntWatchRequest request;
    request.name = std::wstring(kPoolSizeCVar);
    request.timeout = kAutoWatchTimeout;
    request.onValue = [this](int32_t value) {
        // Priority: in-range CVar first, then first streaming-path sample.
        // shouldAbort must not adopt the path sample before this runs (registry
        // order is shouldAbort → timeout → onValue).
        const auto observation = m_controller.ObserveEnginePoolMb(value);
        switch (observation) {
        case EnginePoolObservation::NotReady:
            break;
        case EnginePoolObservation::RejectedBelowMinimum:
        case EnginePoolObservation::RejectedAboveMaximum: {
            int32_t expected = m_lastLoggedRejectedEngineMb.load(std::memory_order_relaxed);
            if (expected != value && m_lastLoggedRejectedEngineMb.compare_exchange_strong(
                    expected, value, std::memory_order_relaxed)) {
                const auto snapshot = m_controller.Snapshot();
                JST_LOG_WARNING(
                    "StreamingPoolFix | ignoring engine-reported {} MB: "
                    "outside safe range {:.1f}-{:.1f} GB",
                    value,
                    snapshot.policy.limits.MinimumGb(),
                    snapshot.policy.limits.MaximumGb());
            }
            break;
        }
        case EnginePoolObservation::LockedFromCVar:
        case EnginePoolObservation::LockedFromPathSample:
            LogAutoLock(m_controller.Snapshot());
            return jst::core::CVarWatchDecision::Complete;
        case EnginePoolObservation::Inactive:
            return jst::core::CVarWatchDecision::Complete;
        }

        if (m_controller.TryAdoptPathSample()) {
            LogAutoLock(m_controller.Snapshot());
            return jst::core::CVarWatchDecision::Complete;
        }
        return jst::core::CVarWatchDecision::Continue;
    };
    request.onTimeout = [this] { OnEngineWatchTimeout(); };
    request.shouldAbort = [this] {
        return !m_controller.IsWaitingForEngine();
    };

    m_engineWatch = jst::core::CVarSystem::Instance().WatchInt(std::move(request));
    if (!m_engineWatch) {
        JST_LOG_WARNING("StreamingPoolFix | auto: failed to start engine pool watch");
    }
}

void StreamingPoolFix::StopEngineWatch() {
    m_engineWatch.Reset();
}

void StreamingPoolFix::OnEngineWatchTimeout() {
    m_controller.OnAutoTimeout();
    const auto snapshot = m_controller.Snapshot();
    if (snapshot.state == StreamingPoolState::Fallback) {
        JST_LOG_WARNING(
            "StreamingPoolFix | auto: engine pool size not ready within {}s; "
            "falling back to {:.1f} GB",
            kAutoWatchTimeout.count() / 1000,
            snapshot.effectiveGb);
    } else {
        LogAutoLock(snapshot);
    }
}

void StreamingPoolFix::OnGraphicsAdapterChanged(
    const jst::core::GraphicsAdapterSnapshot& snapshot) {
    if (m_controller.UpdatePolicy(
            MakePoolSizePolicy(snapshot.dedicatedVideoMemoryBytes))) {
        LogPolicy(m_controller.Snapshot());
    }
}

std::expected<void, std::string>
StreamingPoolFix::FinalizeInstallation(jst::core::HookEngine& hooks) {
    auto result = HookTweak::FinalizeInstallation(hooks);
    if (!result) {
        return result;
    }

    auto& adapterService = jst::core::GraphicsAdapterService::Instance();
    (void)m_controller.UpdatePolicy(
        MakePoolSizePolicy(adapterService.Snapshot().dedicatedVideoMemoryBytes));
    m_controller.BindPayload(PrimaryContext().streamingPool);
    ApplySetting();
    LogPolicy(m_controller.Snapshot());

    m_adapterSubscription = adapterService.Subscribe(
        [this](const jst::core::GraphicsAdapterSnapshot& snapshot) {
            OnGraphicsAdapterChanged(snapshot);
        });
    return {};
}

void StreamingPoolFix::Shutdown() {
    m_adapterSubscription.Reset();
    StopEngineWatch();
    HookTweak::Shutdown();
}

RuntimeControlResetResult StreamingPoolFix::ResetRuntimeControls(
    jst::core::Config& config) {
    const auto baseResult = HookTweak::ResetRuntimeControls(config);
    bool changed = baseResult == RuntimeControlResetResult::Changed;

    const PoolSizeSetting defaults{};
    if (m_setting.mode != defaults.mode ||
        !SliderValuesNearlyEqual(
            m_setting.requestedManualGb, defaults.requestedManualGb)) {
        m_setting = defaults;
        WritePoolSizeGb(config);
        changed = true;
    }

    if (IsInitialized()) {
        // Composite reset is a deliberate re-arm even when values were already
        // defaults, so its watch and timeout restart from a known state.
        ApplySetting();
    }
    return changed
        ? RuntimeControlResetResult::Changed
        : RuntimeControlResetResult::Unchanged;
}

RuntimeControl StreamingPoolFix::MakeAutoCheckbox(std::string_view section) {
    return CheckboxControl{
        .label = "Auto (engine pool size)",
        .current = m_setting.IsAuto(),
        .defaultValue = true,
        .apply = [this](bool value) {
            if (m_setting.IsAuto() == value) {
                return;
            }
            m_setting.mode = value ? PoolSizeMode::Auto : PoolSizeMode::Manual;
            if (IsInitialized()) {
                ApplySetting();
            }
        },
        .persistence = ControlPersistence{
            .section = section,
            .key = kPoolSizeGbConfigKey,
            .overrideAction = [this](jst::core::Config& config) {
                WritePoolSizeGb(config);
            },
        },
        .tooltip =
            "Writes PoolSizeGB=auto when enabled. Prefers r.Streaming.PoolSize "
            "when in range, else the first streaming-path sample, else fallback. "
            "Manual mode uses the slider below.",
    };
}

RuntimeControl StreamingPoolFix::MakeManualSlider(std::string_view section) {
    const auto snapshot = m_controller.Snapshot();
    auto slider = MakeSliderFloatControl(
        MakePoolSizeSliderSpec(snapshot.policy.limits),
        NormalizePoolSizeGb(
            m_setting.requestedManualGb, snapshot.policy.limits),
        [this](float value) {
            m_setting.requestedManualGb = value;
            m_setting.mode = PoolSizeMode::Manual;
            (void)m_controller.UpdateManualSize(value);
            JST_LOG_INFO("StreamingPoolFix | manual: {:.1f} GB", value);
        },
        "Pool Size (GB)",
        section,
        kPoolSizeGbConfigKey,
        "Streaming pool size in binary GiB (the legacy GB label is retained). "
        "Maximum: 70% of dedicated VRAM; legacy maximum: 12.0 GiB when the "
        "game adapter is unavailable.");
    slider.persistence.overrideAction = [this](jst::core::Config& config) {
        WritePoolSizeGb(config);
    };
    return slider;
}

std::vector<RuntimeControl> StreamingPoolFix::GetRuntimeControls() {
    auto controls = HookTweak::GetRuntimeControls();
    const auto section = Name();
    controls.push_back(MakeAutoCheckbox(section));

    if (!IsInitialized()) {
        return controls;
    }
    controls.push_back(LabelControl{.label = "Status"});
    controls.push_back(LabelControl{
        .label = FormatStreamingPoolStatus(m_controller.Snapshot()),
    });
    if (!m_setting.IsAuto()) {
        controls.push_back(MakeManualSlider(section));
    }
    return controls;
}

} // namespace jst::tweaks
