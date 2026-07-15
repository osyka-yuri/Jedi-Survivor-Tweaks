#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace jst::core {

class CVarSystem;

enum class CVarWatchDecision : uint8_t {
    Continue,
    Complete,
};

/**
 * Pump-driven observation of an integer CVar.
 *
 * Callback priority on a pump tick is shouldAbort -> timeout -> onValue.
 * Callbacks run on the CVar pump thread with no resolver or registry mutex
 * held. They may call SetInt/SetFloat/WatchInt, but must not reset their own
 * subscription or stop the pump.
 */
struct IntWatchRequest {
    std::wstring name;
    std::function<CVarWatchDecision(int32_t)> onValue;
    std::function<void()> onTimeout;
    std::function<bool()> shouldAbort;
    std::chrono::milliseconds timeout{30'000};
};

/**
 * Move-only watch ownership. Reset and destruction are join barriers: after
 * they return no predicate or callback belonging to this subscription can be
 * executing. An empty subscription is harmless and evaluates to false.
 */
class CVarWatchSubscription final {
public:
    CVarWatchSubscription() = default;
    ~CVarWatchSubscription();

    CVarWatchSubscription(const CVarWatchSubscription&) = delete;
    CVarWatchSubscription& operator=(const CVarWatchSubscription&) = delete;
    CVarWatchSubscription(CVarWatchSubscription&& other) noexcept;
    CVarWatchSubscription& operator=(CVarWatchSubscription&& other) noexcept;

    void Reset();
    [[nodiscard]] explicit operator bool() const noexcept { return m_owner != nullptr; }

private:
    friend class CVarSystem;
    CVarWatchSubscription(CVarSystem* owner, uint64_t id) noexcept
        : m_owner(owner), m_id(id) {}

    CVarSystem* m_owner = nullptr;
    uint64_t m_id = 0;
};

} // namespace jst::core
