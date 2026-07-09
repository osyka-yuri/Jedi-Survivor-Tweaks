#pragma once

#include "core/hook_types.hpp"
#include "hooks/hook_context.hpp"
#include "slider_specs.hpp"
#include "tweak.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jst::tweaks {

struct HookTarget {
    std::string pattern;
    int32_t patternOffset = 0;
    std::uintptr_t address = 0;
    size_t minimumOverwriteLength = 5;
    core::HookContinuation continuation = core::HookContinuation::Resume;

    [[nodiscard]] static HookTarget Pattern(
        std::string_view signature,
        int32_t offset = 0,
        size_t minimumLength = 5,
        core::HookContinuation continuation = core::HookContinuation::Resume) {
        return HookTarget{
            std::string(signature),
            offset,
            0,
            minimumLength,
            continuation,
        };
    }

    [[nodiscard]] static HookTarget Address(
        std::uintptr_t rva,
        size_t minimumLength = 5,
        core::HookContinuation continuation = core::HookContinuation::Resume) {
        return HookTarget{{}, 0, rva, minimumLength, continuation};
    }
};

struct HookBinding {
    std::string siteName;
    HookTarget target;
    std::uintptr_t detour = 0;
    jst::hooks::Slot slot{};
};

struct RuntimeFloatConfig {
    FloatSliderSpec slider = kMultiplierSliderSpec;
    std::string_view configKey = "Multiplier";
    std::string_view sliderLabel{};
    std::string_view sliderTooltip =
        "Multiplier applied at runtime. Takes effect immediately.";
    bool writesToMultiplier = true;
};

class HookTweak : public ITweak {
public:
    HookTweak(std::string name,
              std::string description,
              bool enabledByDefault,
              HookTarget target,
              std::uintptr_t detour,
              jst::hooks::Slot slot,
              std::optional<RuntimeFloatConfig> runtimeFloatConfig = std::nullopt);
    HookTweak(std::string name,
              std::string description,
              bool enabledByDefault,
              std::vector<HookBinding> bindings,
              std::optional<RuntimeFloatConfig> runtimeFloatConfig = std::nullopt);
    ~HookTweak() override = default;

    HookTweak(const HookTweak&) = delete;
    HookTweak& operator=(const HookTweak&) = delete;
    HookTweak(HookTweak&&) = delete;
    HookTweak& operator=(HookTweak&&) = delete;

    [[nodiscard]] std::expected<void, std::string>
    Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) override;
    [[nodiscard]] std::expected<void, std::string>
    FinalizeResolution(jst::core::HookEngine& hooks) override;
    [[nodiscard]] std::expected<void, std::string>
    FinalizeInstallation(jst::core::HookEngine& hooks) override;
    void Shutdown() override;

    [[nodiscard]] bool IsInitialized() const override { return m_initialized; }
    [[nodiscard]] std::string_view Name() const noexcept override { return m_name; }
    [[nodiscard]] std::string_view Description() const noexcept override { return m_description; }
    [[nodiscard]] bool IsEnabledByDefault() const noexcept override { return m_enabledByDefault; }
    [[nodiscard]] std::vector<RuntimeControl> GetRuntimeControls() override;

protected:
    virtual void OnConfigLoaded(jst::core::Config& /*config*/) {}
    virtual void OnRuntimeFloatChanged(float /*value*/) {}

    [[nodiscard]] jst::hooks::Context& PrimaryContext() const {
        return jst::hooks::GetContext(m_bindings.front().slot);
    }

    void ApplyMultiplier(float multiplier);
    void SetPayload0(uint64_t value) { PrimaryContext().payload0 = value; }
    [[nodiscard]] uint64_t GetPayload0() const { return PrimaryContext().payload0; }
    [[nodiscard]] float LoadedMultiplier() const noexcept { return m_loadedMultiplier; }

private:
    void UnregisterBindings(jst::core::HookEngine& hooks);

    std::string m_name;
    std::string m_description;
    std::vector<HookBinding> m_bindings;
    std::optional<RuntimeFloatConfig> m_runtimeFloatConfig;
    float m_loadedMultiplier = 0.0f;
    bool m_overlayEnabledPref = false;
    bool m_enabledByDefault = false;
    bool m_resolutionFinalized = false;
    bool m_initialized = false;
};

} // namespace jst::tweaks