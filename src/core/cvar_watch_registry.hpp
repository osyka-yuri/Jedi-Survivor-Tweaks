#pragma once

#include "cvar_watch.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace jst::core {

// Internal callback registry used by CVarSystem's pump. Resolution and value
// access remain owned by CVarSystem and are supplied through ValueReader.
class CVarWatchRegistry final {
public:
    using ValueReader = std::function<std::optional<int32_t>(std::wstring_view)>;

    [[nodiscard]] uint64_t Register(IntWatchRequest request);
    void Cancel(uint64_t id);
    void Clear();

    [[nodiscard]] bool HasAny() const;
    [[nodiscard]] bool HasFor(std::wstring_view name) const;
    void Evaluate(const ValueReader& readValue);

private:
    struct WatchEntry {
        uint64_t id = 0;
        IntWatchRequest request;
        std::chrono::steady_clock::time_point deadline{};
        bool active = true;
        size_t inFlight = 0;
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    // Registration order makes callback behavior deterministic and lets one
    // callback safely cancel a later subscription before it starts.
    std::map<uint64_t, std::shared_ptr<WatchEntry>> m_entries;
    uint64_t m_nextId = 1;
};

} // namespace jst::core
