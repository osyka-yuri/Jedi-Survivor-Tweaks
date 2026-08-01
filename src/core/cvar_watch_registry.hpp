#pragma once

#include "cvar_watch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace jst::core {

struct CVarWatchRegistryState;

// Shared cancellation state retained by both the registry and the public
// subscription. Cancellation remains a join barrier even after Clear removes
// the registry entry.
class CVarWatchControl final {
public:
    void CancelAndWait();
    [[nodiscard]] bool IsActive() const;

private:
    friend class CVarWatchRegistry;

    void Cancel();
    void Wait();
    void DetachFromRegistry();

    uint64_t id = 0;
    IntWatchRequest request;
    std::chrono::steady_clock::time_point deadline{};
    std::weak_ptr<CVarWatchRegistryState> registryState;

    mutable std::mutex mutex;
    std::condition_variable cv;
    bool active = true;
    size_t inFlight = 0;
};

// Internal callback registry used by CVarSystem's game-thread pass. Resolution and value
// access remain owned by CVarSystem and are supplied through ValueReader.
class CVarWatchRegistry final {
public:
    using ValueReader = std::function<std::optional<int32_t>(std::wstring_view)>;

    CVarWatchRegistry();

    [[nodiscard]] std::shared_ptr<CVarWatchControl> Register(
        IntWatchRequest request);
    void Clear();
    void OpenStartupBarrier(std::chrono::steady_clock::time_point now);

    [[nodiscard]] bool HasFor(std::wstring_view name) const;
#if defined(JST_UNIT_TESTS)
    [[nodiscard]] bool WaitUntilAbsent(
        std::wstring_view name,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] size_t EntryCount() const;
#endif
    void Evaluate(const ValueReader& readValue);

private:
    std::shared_ptr<CVarWatchRegistryState> m_state;
};

} // namespace jst::core
