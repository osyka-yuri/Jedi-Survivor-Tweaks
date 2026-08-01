#include "hook_tweak.hpp"

#include "runtime_control.hpp"
#include "core/config.hpp"
#include "core/hook_engine.hpp"
#include "core/logging.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace jst::tweaks {

HookTweak::HookTweak(std::string name,
                     std::string description,
                     bool enabledByDefault,
                     HookTarget target,
                     std::uintptr_t detour,
                     jst::hooks::Slot slot,
                     std::optional<RuntimeFloatConfig> runtimeFloatConfig)
    : m_name(std::move(name)),
      m_description(std::move(description)),
      m_bindings{
          HookBinding{m_name, std::move(target), detour, slot},
      },
      m_runtimeFloatConfig(std::move(runtimeFloatConfig)),
      m_enabledByDefault(enabledByDefault) {}

HookTweak::HookTweak(std::string name,
                     std::string description,
                     bool enabledByDefault,
                     std::vector<HookBinding> bindings,
                     std::optional<RuntimeFloatConfig> runtimeFloatConfig)
    : m_name(std::move(name)),
      m_description(std::move(description)),
      m_bindings(std::move(bindings)),
      m_runtimeFloatConfig(std::move(runtimeFloatConfig)),
      m_enabledByDefault(enabledByDefault) {}

std::expected<TweakActivation, std::string>
HookTweak::Configure(const jst::core::Config& config) {
    m_overlayEnabledPref = config.GetBool(m_name, "Enabled", m_enabledByDefault);
    OnConfigLoaded(config);
    if (m_runtimeFloatConfig) {
        const auto& runtimeFloat = *m_runtimeFloatConfig;
        m_loadedMultiplier = LoadSliderValue(
            config.GetFloatInRange(
                m_name,
                runtimeFloat.configKey,
                runtimeFloat.slider.defaultValue,
                runtimeFloat.slider.min,
                runtimeFloat.slider.max),
            runtimeFloat.slider);
        ApplyMultiplier(m_loadedMultiplier);
    }
    return TweakActivation{.enabled = m_overlayEnabledPref};
}

std::expected<void, std::string>
HookTweak::Prepare(jst::core::HookEngine& hooks) {
    if (m_bindings.empty()) {
        return std::unexpected(
            std::format("Hook tweak '{}' has no hook bindings", m_name));
    }

    auto makeSpec = [this](const HookBinding& b) {
        return core::HookSiteSpec{
            .name = b.siteName,
            .group = m_name,
            .minimumOverwriteLength = b.target.minimumOverwriteLength,
            .continuation = b.target.continuation,
        };
    };

    for (const auto& binding : m_bindings) {
        core::HookSiteSpec spec = makeSpec(binding);

        auto registered = binding.target.pattern.empty()
            ? hooks.RegisterAddressHook(
                  std::move(spec), binding.target.address, binding.detour)
            : hooks.RegisterPatternHook(
                  std::move(spec),
                  binding.target.pattern,
                  binding.target.patternOffset,
                  binding.detour);
        if (!registered) {
            const std::string rollbackError = UnregisterBindings(hooks);
            return std::unexpected(std::format(
                "{} [{}]{}{}",
                registered.error().message,
                binding.siteName,
                rollbackError.empty() ? "" : "; rollback: ",
                rollbackError));
        }
    }

    return {};
}

std::expected<void, std::string>
HookTweak::FinalizeResolution(jst::core::HookEngine& hooks) {
    for (const auto& binding : m_bindings) {
        auto continuation = hooks.GetContinuationAddress(binding.siteName);
        if (!continuation) {
            const std::string rollbackError = UnregisterBindings(hooks);
            return std::unexpected(std::format(
                "Hook site '{}' did not resolve{}{}",
                binding.siteName,
                rollbackError.empty() ? "" : "; rollback: ",
                rollbackError));
        }
        jst::hooks::GetContext(binding.slot).resumeAddress = *continuation;
    }

    JST_LOG_INFO("Prepared hook group '{}' with {} site(s).",
                 m_name, m_bindings.size());
    return {};
}

std::expected<void, std::string>
HookTweak::FinalizeInstallation(jst::core::HookEngine& hooks) {
    if (!hooks.IsGroupInstalled(m_name)) {
        m_effectActive = false;
        return std::unexpected(std::format(
            "Hook group '{}' was not installed atomically", m_name));
    }

    m_effectActive = true;
    if (m_runtimeFloatConfig) {
        JST_LOG_INFO("Installed hook group '{}' | multiplier={} | sites={}",
                     m_name, m_loadedMultiplier, m_bindings.size());
    }
    return {};
}

void HookTweak::ApplyMultiplier(float multiplier) {
    if (!m_runtimeFloatConfig || m_runtimeFloatConfig->writesToMultiplier) {
        for (const auto& binding : m_bindings) {
            jst::hooks::GetContext(binding.slot).multiplier = multiplier;
        }
    }
    OnRuntimeFloatChanged(multiplier);
}

std::string HookTweak::UnregisterBindings(jst::core::HookEngine& hooks) {
    std::string firstError;
    for (const auto& binding : m_bindings) {
        if (auto removed = hooks.UnregisterHook(binding.siteName);
            !removed && firstError.empty()) {
            firstError = std::format(
                "{} [{}]",
                removed.error().message,
                binding.siteName);
        }
    }
    m_effectActive = false;
    return firstError;
}

std::vector<RuntimeControl> HookTweak::GetRuntimeControls() {
    std::vector<RuntimeControl> controls;
    controls.reserve(3);

    controls.push_back(CheckboxControl{
        .label = "Load on launch",
        .current = m_overlayEnabledPref,
        .defaultValue = m_enabledByDefault,
        .apply = [this](bool value) {
            m_overlayEnabledPref = value;
            return AppliedEdit();
        },
        .persistence = ControlPersistence{
            .section = m_name,
            .key = "Enabled",
        },
        .tooltip =
            "Persists [<TweakName>] Enabled to the .ini. Takes effect on next "
            "game launch. Runtime controls below apply immediately.",
    });

    if (m_runtimeFloatConfig && m_effectActive) {
        const auto& runtimeFloat = *m_runtimeFloatConfig;
        controls.push_back(LabelControl{.label = "Live below \xe2\x86\x93"});
        controls.push_back(MakeSliderFloatControl(
            runtimeFloat.slider,
            m_loadedMultiplier,
            [this](float value) {
                m_loadedMultiplier = value;
                ApplyMultiplier(value);
                JST_LOG_INFO("{} multiplier -> {:.3f}", m_name, value);
                return AppliedEdit();
            },
            runtimeFloat.sliderLabel.empty() ? runtimeFloat.configKey : runtimeFloat.sliderLabel,
            m_name,
            runtimeFloat.configKey,
            runtimeFloat.sliderTooltip));
    }

    return controls;
}

RuntimeControlResetResult HookTweak::ResetRuntimeControls(jst::core::Config& config) {
    bool changed = false;

    if (m_overlayEnabledPref != m_enabledByDefault) {
        m_overlayEnabledPref = m_enabledByDefault;
        config.SetBool(m_name, "Enabled", m_overlayEnabledPref);
        changed = true;
    }

    if (m_runtimeFloatConfig && m_effectActive) {
        const auto& runtimeFloat = *m_runtimeFloatConfig;
        const float defaultValue = DefaultSliderValue(runtimeFloat.slider);
        if (!SliderValuesNearlyEqual(m_loadedMultiplier, defaultValue)) {
            m_loadedMultiplier = defaultValue;
            ApplyMultiplier(defaultValue);
            config.SetFloat(m_name, runtimeFloat.configKey, defaultValue);
            changed = true;
        }
    }

    return changed
        ? RuntimeControlResetResult::Changed
        : RuntimeControlResetResult::Unchanged;
}

void HookTweak::Shutdown() {
    m_effectActive = false;
}

} // namespace jst::tweaks
