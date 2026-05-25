#pragma once

#include "tweak.hpp"
#include <string>

namespace jst::tweaks {

class CustomCVarsTweak final : public ITweak {
public:
    CustomCVarsTweak() = default;
    ~CustomCVarsTweak() override = default;

    [[nodiscard]] std::string_view Name() const override { return "CVars"; }
    [[nodiscard]] std::string_view Description() const override { return "Applies arbitrary custom Unreal Engine Console Variable (CVar) values."; }
    [[nodiscard]] bool IsEnabledByDefault() const override { return true; }

    [[nodiscard]] std::expected<void, std::string> Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) override;
    void Shutdown() override;
    [[nodiscard]] bool IsInitialized() const override { return m_initialized; }

private:
    bool m_initialized = false;
};

} // namespace jst::tweaks
