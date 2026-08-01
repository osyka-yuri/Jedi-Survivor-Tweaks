#include "detour_gate.hpp"

#include <windows.h>

#include <atomic>
#include <format>
#include <new>

namespace jst::core {

std::expected<void, std::string> DetourGate::Open() {
    if (!m_state) {
        void* storage = VirtualAlloc(
            nullptr,
            sizeof(uint32_t),
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!storage) {
            return std::unexpected(std::format(
                "failed to allocate process-lifetime detour gate (Win32 error {})",
                GetLastError()));
        }
        m_state = ::new (storage) uint32_t(0);
    }

    std::atomic_ref state(*m_state);
    uint32_t expected = 0;
    if (!state.compare_exchange_strong(
            expected,
            kAdmissionOpen,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return std::unexpected(std::format(
            "detour gate cannot open while state 0x{:X} is active",
            expected));
    }
    return {};
}

void DetourGate::Close() noexcept {
    if (!m_state) {
        return;
    }
    std::atomic_ref(*m_state).fetch_and(
        kActiveMask,
        std::memory_order_acq_rel);
}

void DetourGate::WaitForIdle() const noexcept {
    if (!m_state) {
        return;
    }

    std::atomic_ref state(*m_state);
    for (uint32_t observed = state.load(std::memory_order_acquire);
         (observed & kActiveMask) != 0;
         observed = state.load(std::memory_order_acquire)) {
        (void)WaitOnAddress(
            m_state,
            &observed,
            sizeof(observed),
            INFINITE);
    }
}

#if defined(JST_UNIT_TESTS)
uint32_t DetourGate::ActiveCountForTests() const noexcept {
    if (!m_state) {
        return 0;
    }
    return (std::atomic_ref(*m_state).load(std::memory_order_acquire) &
            kActiveMask) /
        kActiveIncrement;
}
#endif

} // namespace jst::core
