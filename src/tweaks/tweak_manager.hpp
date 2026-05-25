#pragma once

#include "tweak.hpp"
#include "core/logging.hpp"
#include <memory>
#include <vector>
#include <algorithm>

namespace jst::core {
    class HookEngine;
    class Config;
}

namespace jst::tweaks {

class TweakManager final {
public:
    TweakManager() = default;
    ~TweakManager() = default;

    TweakManager(const TweakManager&) = delete;
    TweakManager& operator=(const TweakManager&) = delete;
    TweakManager(TweakManager&&) = delete;
    TweakManager& operator=(TweakManager&&) = delete;

    [[nodiscard]] std::expected<size_t, std::string> Initialize(core::HookEngine& hooks, core::Config& config);
    void Shutdown();

    template<typename T, typename... Args>
    void RegisterTweak(Args&&... args) {
        static_assert(std::is_base_of_v<ITweak, T>, "T must inherit from ITweak");
        auto tweak = std::make_unique<T>(std::forward<Args>(args)...);
        const std::string_view name = tweak->Name();
        if (std::ranges::contains(m_tweaks, name, &ITweak::Name)) {
            JST_LOG_WARNING("Tweak '{}' already registered.", name);
            return;
        }
        m_tweaks.push_back(std::move(tweak));
    }

    [[nodiscard]] size_t GetTweakCount() const { return m_tweaks.size(); }

private:
    std::vector<std::unique_ptr<ITweak>> m_tweaks;
    bool m_initialized = false;
};

} // namespace jst::tweaks