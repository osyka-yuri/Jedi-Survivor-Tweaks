#pragma once

#include "tweak.hpp"
#include "hooks/hook_context.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jst::tweaks {

/// Describes the location of a code-patch hook target. Exactly one of the two
/// addressing modes should be populated:
///   - pattern is non-empty -> resolve target via byte-pattern scan
///   - pattern is empty     -> use `address` as RVA in the game module
struct HookTarget {
    std::string pattern;
    int32_t patternOffset = 0;
    std::uintptr_t address = 0;

    [[nodiscard]] static HookTarget Pattern(std::string_view sig, int32_t offset = 0) {
        return HookTarget{std::string(sig), offset, 0};
    }
    [[nodiscard]] static HookTarget Address(std::uintptr_t rva) {
        return HookTarget{{}, 0, rva};
    }
};

/// Optional standard behavior for hook tweaks that expose a single
/// `Multiplier` config key feeding `Context::multiplier`. When supplied, the
/// base class:
///   1. Reads `[Name()] / configKey` from config, clamped to [clampMin, clampMax].
///   2. Writes it to `GetContext().multiplier`.
///   3. Logs a uniform "Hook installed | multiplier=... | resume=..." line.
/// Subclasses with non-standard config or no multiplier should omit this.
struct MultiplierConfig {
    std::string_view configKey = "Multiplier";
    float defaultValue = 1.0f;
    float clampMin = 0.0f;
    float clampMax = 10.0f;
};

/// Unified hook-based tweak base class. Concrete tweaks supply a `HookTarget`,
/// a detour routine, and a context slot at construction. Optional virtual hooks
/// `OnConfigLoaded`/`OnHookResolved` allow injecting per-tweak behaviour.
class HookTweak : public ITweak {
public:
    HookTweak(std::string name, std::string description, bool enabledByDefault,
              HookTarget target, std::uintptr_t detour, jst::hooks::Slot slot,
              std::optional<MultiplierConfig> multiplierCfg = std::nullopt);
    ~HookTweak() override = default;

    HookTweak(const HookTweak&) = delete;
    HookTweak& operator=(const HookTweak&) = delete;
    HookTweak(HookTweak&&) = delete;
    HookTweak& operator=(HookTweak&&) = delete;

    [[nodiscard]] std::expected<void, std::string> Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) override;
    [[nodiscard]] std::expected<void, std::string> FinalizeResolution(jst::core::HookEngine& hooks) override;
    void Shutdown() override;
    [[nodiscard]] bool IsInitialized() const override { return m_initialized; }

    [[nodiscard]] std::string_view Name() const override { return m_name; }
    [[nodiscard]] std::string_view Description() const override { return m_description; }
    [[nodiscard]] bool IsEnabledByDefault() const override { return m_enabledByDefault; }

    [[nodiscard]] std::vector<RuntimeControl> GetRuntimeControls() override;

protected:
    virtual void OnConfigLoaded(jst::core::Config& /*config*/) {}
    virtual void OnHookResolved(const jst::hooks::Context& /*context*/) {}

    [[nodiscard]] jst::hooks::Context& GetContext() const { return jst::hooks::GetContext(m_slot); }

    // Last value loaded by the standard MultiplierConfig path. 0.0f if no
    // MultiplierConfig was provided.  Updated by the runtime slider so that
    // re-enabling the tweak restores the user's current slider position.
    [[nodiscard]] float LoadedMultiplier() const noexcept { return m_loadedMultiplier; }

private:
    /// Common tail of Initialize (address-hook) and FinalizeResolution
    /// (pattern-hook): stamps resume address, logs, calls OnHookResolved.
    void ApplyResolution(std::uintptr_t resumeAddress);


    std::string m_name;
    std::string m_description;
    HookTarget m_target;
    std::uintptr_t m_detour = 0;
    jst::hooks::Slot m_slot;
    std::optional<MultiplierConfig> m_multiplierCfg;
    float m_loadedMultiplier = 0.0f;
    bool  m_configEnabled;               // mirrors [TweakName] Enabled in .ini
    bool  m_enabledByDefault = false;
    bool  m_initialized      = false;
};

} // namespace jst::tweaks
