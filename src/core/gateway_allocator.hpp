#pragma once

#include "hook_types.hpp"

#include <expected>
#include <string_view>

namespace jst::core {

namespace detail {

// GatewayAllocator owns the single process-lifetime arena used for all
// hook trampolines/gateways.
//
// - Allocated near the game module base so that 32-bit relative jumps from
//   splice sites can reach the absolute indirect jumps we place in the gateways.
// - 64 KiB arena carved into 64-byte slots.
// - Sealed exactly once (RW -> RX) the first time any group successfully prepares.
//   After sealing, further registrations are rejected (prevents use-after-seal bugs).
// - Thread-safe allocation under a mutex; initialization uses call_once.
class GatewayAllocator final {
public:
    [[nodiscard]] static GatewayAllocator& Instance();

    GatewayAllocator(const GatewayAllocator&) = delete;
    GatewayAllocator& operator=(const GatewayAllocator&) = delete;

    // Allocate one gateway slot. Returns a non-owning slice view.
    // Fails after Seal() or when the arena is exhausted.
    [[nodiscard]] std::expected<ExecutableMemory, HookError>
    AllocateGateway(std::string_view site);

    // One-way transition: mark the used portion of the arena executable and
    // flush the instruction cache. Idempotent and safe to call multiple times.
    // After success, AllocateGateway will refuse new callers.
    [[nodiscard]] std::expected<void, HookError> Seal();

private:
    GatewayAllocator() = default;
    ~GatewayAllocator() = default;

    // Internal implementation details live in the .cpp.
};

} // namespace detail

// Convenience free functions used by the engine (thin wrappers over Instance()).
[[nodiscard]] inline std::expected<ExecutableMemory, HookError>
AllocateGateway(std::string_view site) {
    return detail::GatewayAllocator::Instance().AllocateGateway(site);
}

[[nodiscard]] inline std::expected<void, HookError> SealGatewayArena() {
    return detail::GatewayAllocator::Instance().Seal();
}

// Low-level helper to emit the 14-byte absolute indirect jump used inside gateways.
// Declared here so hook_engine.cpp can use it after including this header.
void WriteAbsoluteJump(std::span<std::byte> destination, uintptr_t target);

} // namespace jst::core
