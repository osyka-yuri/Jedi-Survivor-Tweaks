#pragma once

#include "core/logging.hpp"
#include "tweak.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace jst::core {
class HookEngine;
class Config;
}

namespace jst::tweaks {

enum class TweakStage : uint8_t {
    Registered,
    Disabled,
    Prepared,
    Resolved,
    Active,
    Failed,
};

struct TweakStatus {
    TweakStage stage = TweakStage::Registered;
    std::string error;
};

class TweakManager final {
public:
    TweakManager() = default;
    ~TweakManager() = default;

    TweakManager(const TweakManager&) = delete;
    TweakManager& operator=(const TweakManager&) = delete;
    TweakManager(TweakManager&&) = delete;
    TweakManager& operator=(TweakManager&&) = delete;

    [[nodiscard]] std::expected<void, std::string> Prepare(
        core::HookEngine& hooks,
        const core::Config& config);
    void FinalizeResolution(core::HookEngine& hooks);
    [[nodiscard]] size_t FinalizeInstallation(core::HookEngine& hooks);
    void Shutdown();

    template<typename T, typename... Args>
    bool RegisterTweak(Args&&... args) {
        static_assert(std::is_base_of_v<ITweak, T>);
        if (m_started) {
            JST_LOG_ERROR("Cannot register a tweak after TweakManager start.");
            return false;
        }
        auto tweak = std::make_unique<T>(std::forward<Args>(args)...);
        const std::string_view name = tweak->Name();
        if (std::ranges::any_of(m_entries, [name](const Entry& entry) {
                return entry.tweak->Name() == name;
            })) {
            JST_LOG_WARNING("Tweak '{}' already registered.", name);
            return false;
        }
        m_entries.push_back(Entry{.tweak = std::move(tweak)});
        return true;
    }

    [[nodiscard]] size_t GetTweakCount() const noexcept {
        return m_entries.size();
    }

    void IterateTweaks(
        const std::function<void(ITweak&, const TweakStatus&)>& visitor);
    void IterateTweaks(
        const std::function<void(const ITweak&, const TweakStatus&)>& visitor)
        const;

private:
    struct Entry {
        std::unique_ptr<ITweak> tweak;
        TweakStatus status;
        bool prepared = false;
    };

    std::vector<Entry> m_entries;
    bool m_started = false;
};

} // namespace jst::tweaks
