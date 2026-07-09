#pragma once

#include <windows.h>

namespace jst::core {

class DebounceTimer final {
public:
    explicit DebounceTimer(ULONGLONG intervalMs) noexcept : m_intervalMs(intervalMs) {}

    void MarkDirty() noexcept {
        m_pending = true;
        m_lastTick = GetTickCount64();
    }

    [[nodiscard]] bool ShouldFlush() const noexcept {
        return m_pending && (GetTickCount64() - m_lastTick >= m_intervalMs);
    }

    void Reset() noexcept { m_pending = false; }

private:
    ULONGLONG m_intervalMs;
    bool      m_pending  = false;
    ULONGLONG m_lastTick = 0;
};

} // namespace jst::core
