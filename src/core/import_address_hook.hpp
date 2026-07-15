#pragma once

#include "pe_imports.hpp"

#include <atomic>

namespace jst::core {

// Owns the atomic protocol for one IAT observer. A delay-load helper replaces
// its IAT slot with the resolved export during the first call; CompleteCall()
// adopts that export as the new original and restores the observer.
class ImportAddressHook final {
public:
    explicit ImportAddressHook(ULONG_PTR observer) noexcept : m_observer(observer) {}

    ImportAddressHook(const ImportAddressHook&) = delete;
    ImportAddressHook& operator=(const ImportAddressHook&) = delete;

    [[nodiscard]] bool Install(ImportAddressSlot slot) noexcept;
    [[nodiscard]] ULONG_PTR Original() const noexcept {
        return m_original.load(std::memory_order_acquire);
    }

    void CompleteCall() noexcept;

    [[nodiscard]] bool AwaitingDelayResolution() const noexcept {
        return m_awaitingDelayResolution.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] bool ReplaceSlot(
        ULONG_PTR* slot, ULONG_PTR expected, ULONG_PTR replacement) const noexcept;

    const ULONG_PTR m_observer = 0;
    std::atomic<ULONG_PTR> m_original{0};
    std::atomic<ULONG_PTR*> m_slot{nullptr};
    std::atomic<bool> m_awaitingDelayResolution{false};
    std::atomic<bool> m_delayOriginalPromoted{false};
};

} // namespace jst::core
