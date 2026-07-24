#pragma once

#include "tweak.hpp"
#include <string>

namespace jst::tweaks {

class InterpolatedRenderingTweak final : public ITweak {
public:
    InterpolatedRenderingTweak() = default;
    ~InterpolatedRenderingTweak() override = default;

    [[nodiscard]] std::string_view Name() const override { return "InterpolatedRendering"; }
    [[nodiscard]] std::string_view Description() const override { return "Reduces CPU stutters and camera jitters by enabling frame interpolation."; }
    [[nodiscard]] bool IsEnabledByDefault() const override { return true; }

    [[nodiscard]] std::expected<void, std::string> Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) override;
    void Shutdown() override;
    [[nodiscard]] bool IsInitialized() const override { return m_initialized; }

    [[nodiscard]] std::vector<RuntimeControl> GetRuntimeControls() override;

private:
    bool m_initialized = false;
    bool m_irEnabled   = false;     // loaded from config in Initialize; mutated by overlay
};

} // namespace jst::tweaks
