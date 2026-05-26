#include "hook_tweak.hpp"
#include "core/hook_engine.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"

#include <algorithm>
#include <format>

namespace jst::tweaks {

HookTweak::HookTweak(std::string name, std::string description, bool enabledByDefault,
                     HookTarget target, std::uintptr_t detour, jst::hooks::Slot slot,
                     std::optional<MultiplierConfig> multiplierCfg)
    : m_name(std::move(name)),
      m_description(std::move(description)),
      m_target(std::move(target)),
      m_detour(detour),
      m_slot(slot),
      m_multiplierCfg(std::move(multiplierCfg)),
      m_configEnabled(enabledByDefault),      // overwritten in Initialize if called
      m_enabledByDefault(enabledByDefault) {}

std::expected<void, std::string> HookTweak::Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) {
    // Sync the in-memory config-enabled flag so GetRuntimeControls() shows the
    // current .ini value even for initialized tweaks.
    m_configEnabled = config.GetBool(m_name, "Enabled", m_enabledByDefault);
    OnConfigLoaded(config);

    // Standard multiplier path: clamp the configured float and stamp it into
    // the shared hook context. Subclasses that need different behaviour can
    // omit the MultiplierConfig and roll their own in OnConfigLoaded.
    if (m_multiplierCfg) {
        const auto& cfg = *m_multiplierCfg;
        m_loadedMultiplier = std::clamp(
            config.GetFloat(m_name, cfg.configKey, cfg.defaultValue),
            cfg.clampMin, cfg.clampMax);
        GetContext().multiplier = m_loadedMultiplier;
    }

    // Register only; the actual pattern scan is batched in HookEngine::ResolveAll()
    // by TweakManager. Address hooks still resolve immediately inside Register.
    auto registerRes = m_target.pattern.empty()
        ? hooks.RegisterAddressHook(m_name, m_target.address, m_detour)
        : hooks.RegisterPatternHook(m_name, m_target.pattern, m_target.patternOffset, m_detour);
    if (!registerRes) return registerRes;

    // Address-hook resume address is available immediately. For pattern-hooks
    // GetResumeAddress will return nullopt here; we'll finish in FinalizeResolution.
    if (m_target.pattern.empty()) {
        auto resumeAddrOpt = hooks.GetResumeAddress(m_name);
        if (!resumeAddrOpt) {
            return std::unexpected(std::format("Address hook '{}' missing resume address", m_name));
        }
        ApplyResolution(*resumeAddrOpt);
        m_initialized = true;
    }
    return {};
}

std::expected<void, std::string> HookTweak::FinalizeResolution(jst::core::HookEngine& hooks) {
    if (m_initialized) return {};   // already finished (address-hook path)

    auto resumeAddrOpt = hooks.GetResumeAddress(m_name);
    if (!resumeAddrOpt) {
        return std::unexpected(std::format("Failed to resolve resume address for '{}'", m_name));
    }
    ApplyResolution(*resumeAddrOpt);
    m_initialized = true;
    return {};
}

void HookTweak::ApplyResolution(std::uintptr_t resumeAddress) {
    auto& context = GetContext();
    context.resumeAddress = resumeAddress;

    if (m_multiplierCfg) {
        JST_LOG_INFO("Hook installed | multiplier={} | resume=0x{:X}",
                     m_loadedMultiplier, resumeAddress);
    }
    OnHookResolved(context);
}

std::vector<RuntimeControl> HookTweak::GetRuntimeControls() {
    std::vector<RuntimeControl> controls;
    // Upper bound on what this base impl emits: enable checkbox + (optional)
    // label separator + multiplier slider. Reserving up-front turns the
    // per-frame push_back doublings into a single allocation.
    controls.reserve(3);

    // Pre-launch toggle: writes [TweakName] Enabled to .ini. The hook itself
    // can't be installed/uninstalled at runtime (the trampoline arena is
    // sealed after HookEngine::InstallAll), so this checkbox is honestly a
    // "next launch" setting. The renamed label + retitled tooltip make that
    // visible without requiring the user to hover.
    controls.push_back(CheckboxControl{
        .label         = "Load on launch",
        .current       = m_configEnabled,
        .defaultValue  = m_enabledByDefault,
        .apply         = [this](bool v) { m_configEnabled = v; },
        .configSection = m_name,
        .configKey     = "Enabled",
        .tooltip       = "Persists [<TweakName>] Enabled to the .ini. Takes effect on next "
                         "game launch. The runtime slider below (if present) is the way to "
                         "change behaviour immediately while playing.",
    });

    // Runtime multiplier slider -- only available when the hook is installed.
    if (m_multiplierCfg && m_initialized) {
        const auto& cfg = *m_multiplierCfg;
        // Visual cue separating the "next launch" toggle above from the
        // immediate-effect slider below.
        controls.push_back(LabelControl{ .label = "Live below \xe2\x86\x93" });  // ↓ (U+2193)
        controls.push_back(SliderFloatControl{
            .label         = cfg.configKey,
            .min           = cfg.clampMin,
            .max           = cfg.clampMax,
            .current       = m_loadedMultiplier,
            .defaultValue  = cfg.defaultValue,
            .apply         = [this](float v) {
                m_loadedMultiplier = v;
                GetContext().multiplier = v;
            },
            .configSection = m_name,
            .configKey     = cfg.configKey,
            .tooltip       = "Multiplier applied at runtime. Takes effect immediately.",
        });
    }

    return controls;
}

void HookTweak::Shutdown() {
    m_initialized = false;
}

} // namespace jst::tweaks
