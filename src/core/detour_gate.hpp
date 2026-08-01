#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace jst::core {

/**
 * Process-lifetime admission state for a returning detour gateway.
 *
 * The gateway atomically acquires admission before it enters DLL code. Once
 * Close() returns, a gateway invocation either belongs to the active count or
 * bypasses the detour and continues through the original trampoline. The
 * backing word is deliberately retained until process exit because a thread
 * may already have reached the process-lifetime gateway when the DLL shuts
 * down.
 */
class DetourGate final {
public:
    DetourGate() = default;
    ~DetourGate() = default;

    DetourGate(const DetourGate&) = delete;
    DetourGate& operator=(const DetourGate&) = delete;

    [[nodiscard]] std::expected<void, std::string> Open();
    void Close() noexcept;
    void WaitForIdle() const noexcept;

    [[nodiscard]] uintptr_t StateAddress() const noexcept {
        return reinterpret_cast<uintptr_t>(m_state);
    }

#if defined(JST_UNIT_TESTS)
    [[nodiscard]] uint32_t ActiveCountForTests() const noexcept;
#endif

private:
    // Bit 0 is admission; bits 1..31 contain active returning detours in
    // units of two. This lets the gateway acquire admission and activity with
    // one cmpxchg, leaving no pre-counter entry window during shutdown.
    static constexpr uint32_t kAdmissionOpen = 1;
    static constexpr uint32_t kActiveIncrement = 2;
    static constexpr uint32_t kActiveMask = ~kAdmissionOpen;

    uint32_t* m_state = nullptr;
};

} // namespace jst::core
