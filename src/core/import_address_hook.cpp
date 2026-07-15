#include "import_address_hook.hpp"

#include "scoped_virtual_protect.hpp"

namespace jst::core {

bool ImportAddressHook::ReplaceSlot(
    ULONG_PTR* slot, ULONG_PTR expected, ULONG_PTR replacement) const noexcept {
    if (!slot || expected == 0 || replacement == 0) {
        return false;
    }

    ScopedVirtualProtect protection(slot, sizeof(*slot), PAGE_READWRITE);
    if (!protection.Active()) {
        return false;
    }

    std::atomic_ref<ULONG_PTR> slotValue(*slot);
    return slotValue.compare_exchange_strong(
               expected,
               replacement,
               std::memory_order_release,
               std::memory_order_acquire) ||
           expected == replacement;
}

bool ImportAddressHook::Install(ImportAddressSlot slot) noexcept {
    if (!slot.address || m_observer == 0) {
        return false;
    }

    std::atomic_ref<ULONG_PTR> slotValue(*slot.address);
    const ULONG_PTR current = slotValue.load(std::memory_order_acquire);
    if (current == m_observer) {
        return m_slot.load(std::memory_order_acquire) == slot.address;
    }
    if (current == 0) {
        return false;
    }

    ULONG_PTR expectedOriginal = 0;
    if (!m_original.compare_exchange_strong(
            expectedOriginal,
            current,
            std::memory_order_acq_rel,
            std::memory_order_acquire) &&
        expectedOriginal != current) {
        return false;
    }

    ULONG_PTR* expectedSlot = nullptr;
    if (!m_slot.compare_exchange_strong(
            expectedSlot,
            slot.address,
            std::memory_order_acq_rel,
            std::memory_order_acquire) &&
        expectedSlot != slot.address) {
        return false;
    }

    const bool isDelay = slot.table == ImportAddressTableKind::Delay;
    m_delayOriginalPromoted.store(false, std::memory_order_release);
    m_awaitingDelayResolution.store(isDelay, std::memory_order_release);
    if (ReplaceSlot(slot.address, current, m_observer)) {
        return true;
    }

    m_awaitingDelayResolution.store(false, std::memory_order_release);
    m_delayOriginalPromoted.store(false, std::memory_order_release);
    expectedSlot = slot.address;
    (void)m_slot.compare_exchange_strong(
        expectedSlot, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    expectedOriginal = current;
    (void)m_original.compare_exchange_strong(
        expectedOriginal, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    return false;
}

void ImportAddressHook::CompleteCall() noexcept {
    if (!m_awaitingDelayResolution.load(std::memory_order_acquire)) {
        return;
    }

    auto* slot = m_slot.load(std::memory_order_acquire);
    if (!slot) {
        return;
    }

    std::atomic_ref<ULONG_PTR> slotValue(*slot);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const ULONG_PTR current = slotValue.load(std::memory_order_acquire);
        if (current == 0) {
            return;
        }
        if (current == m_observer) {
            if (m_delayOriginalPromoted.load(std::memory_order_acquire)) {
                m_awaitingDelayResolution.store(false, std::memory_order_release);
            }
            return;
        }

        ULONG_PTR original = m_original.load(std::memory_order_acquire);
        if (current != original) {
            if (m_original.compare_exchange_strong(
                    original,
                    current,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire) ||
                original == current) {
                m_delayOriginalPromoted.store(true, std::memory_order_release);
            } else {
                continue;
            }
        }

        if (ReplaceSlot(slot, current, m_observer)) {
            if (m_delayOriginalPromoted.load(std::memory_order_acquire)) {
                m_awaitingDelayResolution.store(false, std::memory_order_release);
            }
            return;
        }
    }
}

} // namespace jst::core
