#include "streaming_pool_fix.hpp"

#include "core/config.hpp"
#include "core/cvar_system.hpp"
#include "core/logging.hpp"
#include "hooks/tweak_hooks.hpp"
#include "runtime_control.hpp"
#include "slider_utils.hpp"

#include <utility>

namespace {

constexpr const char* kPattern =
    "40 E8 ?? ?? ?? ?? 48 8B 54 24 40 84 C0 74 16 48 8B 83 08 01 00 00 48 2B 44 24 48 48 03 C2 48 89";
constexpr int32_t kOffset = 0x6;
constexpr std::wstring_view kPoolSizeCVar = L"r.Streaming.PoolSize";

} // namespace

namespace jst::tweaks {

StreamingPoolFix::StreamingPoolFix()
    : HookTweak(
          "StreamingPoolFix",
          "Locks the streaming pool to the engine-selected or configured size. "
          "Manual values are capped at 70% of detected dedicated GPU memory.",
          true,
          HookTarget::Pattern(kPattern, kOffset, 16),
          reinterpret_cast<std::uintptr_t>(&StreamingPoolFix_Detour),
          jst::hooks::Slot::StreamingPoolFix) {}

void StreamingPoolFix::OnConfigLoaded(const jst::core::Config& config) {
    const std::string raw = config.GetString(
        Name(), kPoolSizeGbConfigKey, kPoolSizeAutoLiteral);
    m_setting = ParsePoolSizeGb(raw);
    if (!IsPoolSizeAutoLiteral(raw) && m_setting.IsAuto()) {
        JST_LOG_WARNING(
            "Invalid [{}] {}='{}'; using '{}'.",
            Name(),
            kPoolSizeGbConfigKey,
            raw,
            kPoolSizeAutoLiteral);
    }
}

void StreamingPoolFix::WritePoolSizeGb(jst::core::Config& config) const {
    config.SetString(Name(), kPoolSizeGbConfigKey, FormatPoolSizeGb(m_setting));
}

void StreamingPoolFix::LogPolicy(
    const StreamingPoolSnapshot& snapshot) const {
    if (snapshot.policy.dedicatedVideoMemoryBytes) {
        JST_LOG_INFO(
            "StreamingPoolFix | VRAM={:.1f} GB | manualMax={:.1f} GB.",
            PoolSizeBytesToGb(*snapshot.policy.dedicatedVideoMemoryBytes),
            snapshot.policy.limits.MaximumGb());
    } else {
        JST_LOG_WARNING(
            "StreamingPoolFix | VRAM=unavailable | manualMax=12.0 GB.");
    }
}

bool StreamingPoolFix::ReadAutomaticPoolSize(uint64_t generation) {
    const bool queued = jst::core::CVarSystem::Instance().ReadIntOnce(
        kPoolSizeCVar,
        [this, generation](std::expected<int32_t, std::string> value) {
            const auto completion = m_controller.CompleteAutoRead(generation, value);
            if (completion.completion == StreamingPoolAutoCompletion::Stale) {
                return;
            }
            if (completion.completion == StreamingPoolAutoCompletion::Exact) {
                JST_LOG_INFO(
                    "StreamingPoolFix | mode=auto | source=r.Streaming.PoolSize "
                    "| value={} MB ({:.2f} GB) | result=locked.",
                    completion.snapshot.enginePoolMb,
                    completion.snapshot.effectiveGb);
                return;
            }
            JST_LOG_WARNING(
                "StreamingPoolFix | mode=auto | source=r.Streaming.PoolSize "
                "| result=fallback | value=2.00 GB | reason='{}'.",
                completion.diagnostic);
        });
    if (!queued) {
        const auto completion = m_controller.CompleteAutoRead(
            generation,
            std::unexpected(std::string("CVar read request was rejected")));
        if (completion.completion == StreamingPoolAutoCompletion::Fallback) {
            JST_LOG_WARNING(
                "StreamingPoolFix | mode=auto | source=r.Streaming.PoolSize "
                "| result=fallback | value=2.00 GB | reason='{}'.",
                completion.diagnostic);
        }
    }
    return queued;
}

void StreamingPoolFix::ApplySetting() {
    const uint64_t generation =
        m_autoReadGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (m_setting.IsAuto()) {
        m_controller.ArmAuto(generation);
        if (ReadAutomaticPoolSize(generation)) {
            JST_LOG_INFO(
                "StreamingPoolFix | mode=auto | source=r.Streaming.PoolSize | "
                "result=queued for post-Tick read.");
        }
        return;
    }

    m_controller.ArmManual(generation, m_setting.requestedManualGb);
    const auto snapshot = m_controller.Snapshot();
    if (SliderValuesNearlyEqual(
            snapshot.effectiveGb, m_setting.requestedManualGb)) {
        JST_LOG_INFO(
            "StreamingPoolFix | mode=manual | value={:.1f} GB | result=locked.",
            snapshot.effectiveGb);
    } else {
        JST_LOG_INFO(
            "StreamingPoolFix | mode=manual | requested={:.1f} GB | "
            "value={:.1f} GB | result=capped.",
            m_setting.requestedManualGb,
            snapshot.effectiveGb);
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
    jst::core::CVarSystem::Instance().ClaimManaged(kPoolSizeCVar);

    auto& adapterService = jst::core::GraphicsAdapterService::Instance();
    const auto adapterSnapshot = adapterService.Snapshot();
    (void)m_controller.UpdatePolicy(
        MakePoolSizePolicy(adapterSnapshot.dedicatedVideoMemoryBytes));
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
    m_autoReadGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_adapterSubscription.Reset();
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

    if (IsEffectActive()) {
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
                return RuntimeEditResult{};
            }
            m_setting.mode = value ? PoolSizeMode::Auto : PoolSizeMode::Manual;
            if (IsEffectActive()) {
                ApplySetting();
            }
            return AppliedEdit();
        },
        .persistence = ControlPersistence{
            .section = section,
            .key = kPoolSizeGbConfigKey,
            .overrideAction = [this](jst::core::Config& config) {
                WritePoolSizeGb(config);
            },
        },
        .tooltip =
            "Writes PoolSizeGB=auto and locks the r.Streaming.PoolSize "
            "value selected by the game. "
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
            ApplySetting();
            return AppliedEdit();
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

    if (!IsEffectActive()) {
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
