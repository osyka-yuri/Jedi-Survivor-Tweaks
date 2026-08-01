#pragma once

#include "core/cvar_system.hpp"
#include "tweak.hpp"

#include <cstddef>

namespace jst::tweaks {

class CustomCVarsTweak final : public ITweak {
public:
    [[nodiscard]] std::string_view Name() const noexcept override {
        return "CVars";
    }
    [[nodiscard]] std::string_view Description() const noexcept override {
        return "Applies custom numeric Unreal Engine Console Variable values.";
    }
    [[nodiscard]] bool IsEnabledByDefault() const noexcept override {
        return true;
    }

    [[nodiscard]] std::expected<TweakActivation, std::string> Configure(
        const jst::core::Config& config) override;
    [[nodiscard]] std::expected<void, std::string> Prepare(
        jst::core::HookEngine& hooks) override;
    void Shutdown() override;
    [[nodiscard]] std::optional<TweakRuntimeStatus> RuntimeStatus()
        const override;

private:
    struct Entry {
        std::wstring name;
        std::wstring value;
    };

    std::vector<Entry> m_entries;
    std::vector<jst::core::CVarCommandTicket> m_tickets;
    size_t m_initialManagedConflicts = 0;
};

} // namespace jst::tweaks
