#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace jst::core {

class CVarWatchControl;

enum class CVarWatchDecision : uint8_t {
    Continue,
    Complete,
};

/**
 * Game-thread observation of an integer CVar.
 *
 * Callback priority on a post-Tick pass is shouldAbort -> timeout -> onValue.
 * Callbacks run after FEngineLoop::Tick with no resolver or registry mutex
 * held. They may call SetInt/SetFloat/WatchInt, but must not reset their own
 * subscription or stop the CVar system.
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
    [[nodiscard]] explicit operator bool() const noexcept {
        return m_control != nullptr;
    }

private:
    friend class CVarSystem;
#if defined(JST_UNIT_TESTS)
    friend class CVarWatchSubscriptionTestAccess;
#endif
    explicit CVarWatchSubscription(
        std::shared_ptr<CVarWatchControl> control) noexcept
        : m_control(std::move(control)) {}

    std::shared_ptr<CVarWatchControl> m_control;
};

} // namespace jst::core
