#pragma once

#include <atomic>
#include <cstdint>

namespace jst::core {

class AtomicActivityGuard final {
public:
    explicit AtomicActivityGuard(std::atomic<uint32_t>& counter) noexcept
        : m_counter(counter) {
        m_counter.fetch_add(1, std::memory_order_acq_rel);
    }

    ~AtomicActivityGuard() {
        if (m_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_counter.notify_all();
        }
    }

    AtomicActivityGuard(const AtomicActivityGuard&) = delete;
    AtomicActivityGuard& operator=(const AtomicActivityGuard&) = delete;

private:
    std::atomic<uint32_t>& m_counter;
};

} // namespace jst::core
