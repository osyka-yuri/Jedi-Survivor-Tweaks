#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace jst::core {

class ScopedVirtualProtect final {
public:
    ScopedVirtualProtect(void* address, size_t size, DWORD newProtection) noexcept
        : m_address(address), m_size(size) {
        if (m_address && m_size > 0) {
            m_active = VirtualProtect(m_address, m_size, newProtection, &m_previousProtection) != FALSE;
        }
    }

    ~ScopedVirtualProtect() {
        if (!m_active || !m_address || m_size == 0) {
            return;
        }
        DWORD ignored = 0;
        (void)VirtualProtect(m_address, m_size, m_previousProtection, &ignored);
    }

    ScopedVirtualProtect(const ScopedVirtualProtect&) = delete;
    ScopedVirtualProtect& operator=(const ScopedVirtualProtect&) = delete;

    [[nodiscard]] bool Active() const noexcept { return m_active; }
    [[nodiscard]] DWORD PreviousProtection() const noexcept { return m_previousProtection; }

    void Dismiss() noexcept { m_active = false; }

    [[nodiscard]] bool FlushInstructionCache() const noexcept {
        if (!m_active || !m_address || m_size == 0) {
            return false;
        }
        return ::FlushInstructionCache(GetCurrentProcess(), m_address, m_size) != FALSE;
    }

private:
    void* m_address = nullptr;
    size_t m_size = 0;
    DWORD m_previousProtection = 0;
    bool m_active = false;
};

} // namespace jst::core