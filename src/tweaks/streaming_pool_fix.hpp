#pragma once

#include "core/graphics_adapter_service.hpp"
#include "hook_tweak.hpp"
#include "streaming_pool_controller.hpp"
#include "streaming_pool_policy.hpp"

#include <atomic>

namespace jst::tweaks {

class StreamingPoolFix final : public HookTweak {
public:
    StreamingPoolFix();
    ~StreamingPoolFix() override = default;

    StreamingPoolFix(const StreamingPoolFix&) = delete;
    StreamingPoolFix& operator=(const StreamingPoolFix&) = delete;
    StreamingPoolFix(StreamingPoolFix&&) = delete;
    StreamingPoolFix& operator=(StreamingPoolFix&&) = delete;

    [[nodiscard]] std::expected<void, std::string>
    FinalizeInstallation(jst::core::HookEngine& hooks) override;
    void Shutdown() override;
    [[nodiscard]] std::vector<RuntimeControl> GetRuntimeControls() override;
    [[nodiscard]] RuntimeControlResetResult ResetRuntimeControls(
        jst::core::Config& config) override;

protected:
    void OnConfigLoaded(const jst::core::Config& config) override;

private:
    void ApplySetting();
    [[nodiscard]] bool ReadAutomaticPoolSize(uint64_t generation);
    void WritePoolSizeGb(jst::core::Config& config) const;
    void OnGraphicsAdapterChanged(
        const jst::core::GraphicsAdapterSnapshot& snapshot);
    void LogPolicy(const StreamingPoolSnapshot& snapshot) const;
    [[nodiscard]] RuntimeControl MakeAutoCheckbox(std::string_view section);
    [[nodiscard]] RuntimeControl MakeManualSlider(std::string_view section);

    StreamingPoolController m_controller;
    PoolSizeSetting m_setting{};
    jst::core::GraphicsAdapterService::Subscription m_adapterSubscription;
    std::atomic<uint64_t> m_autoReadGeneration{0};
};

} // namespace jst::tweaks
