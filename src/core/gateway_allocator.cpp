#include "gateway_allocator.hpp"
#include "scoped_virtual_protect.hpp"

#include "memory_scanner.hpp" // GetGameModuleInfo

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <utility>

namespace jst::core::detail {

using jst::core::MakeError;

namespace {

constexpr size_t kArenaSize   = 64 * 1024;
constexpr size_t kGatewaySize = 64;

struct GatewayArenaState {
    std::byte* base = nullptr;
    size_t used = 0;
    bool sealed = false;
    std::once_flag initializeOnce;
    std::mutex mutex;
};

GatewayArenaState g_state;

uintptr_t AlignDown(uintptr_t value, uintptr_t alignment) {
    return value - value % alignment;
}

void* TryAllocateNear(uintptr_t origin, size_t size) {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);

    const uintptr_t granularity = systemInfo.dwAllocationGranularity;
    const uintptr_t minimumAddress =
        reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const uintptr_t maximumAddress =
        reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    const uintptr_t alignedOrigin = AlignDown(origin, granularity);
    const uint64_t maximumDistance =
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max());

    for (uint64_t distance = 0; distance <= maximumDistance; distance += granularity) {
        if (distance <= alignedOrigin - minimumAddress) {
            const uintptr_t candidate = alignedOrigin - static_cast<uintptr_t>(distance);
            if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate),
                                            size,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE)) {
                return memory;
            }
        }

        if (distance != 0 &&
            distance <= maximumAddress - alignedOrigin &&
            alignedOrigin + distance <= maximumAddress - size) {
            const uintptr_t candidate = alignedOrigin + static_cast<uintptr_t>(distance);
            if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate),
                                            size,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE)) {
                return memory;
            }
        }

        if (maximumDistance - distance < granularity) {
            break;
        }
    }
    return nullptr;
}

bool EnsureInitialized() {
    bool ok = true;
    std::call_once(g_state.initializeOnce, [&] {
        auto module = GetGameModuleInfo();
        if (!module) {
            ok = false;
            return;
        }

        void* memory = TryAllocateNear(module->base, kArenaSize);
        if (!memory) {
            ok = false;
            return;
        }
        g_state.base = static_cast<std::byte*>(memory);
    });
    return ok && g_state.base != nullptr;
}

void WriteAbsoluteJumpImpl(std::span<std::byte> destination, uintptr_t target) {
    static constexpr std::array<std::byte, 6> opcode{
        std::byte{0xFF}, std::byte{0x25}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    std::copy(opcode.begin(), opcode.end(), destination.begin());
    std::memcpy(destination.data() + opcode.size(), &target, sizeof(target));
}

} // anonymous namespace

// Public surface

GatewayAllocator& GatewayAllocator::Instance() {
    static GatewayAllocator instance;
    return instance;
}

std::expected<ExecutableMemory, HookError>
GatewayAllocator::AllocateGateway(std::string_view site) {
    if (!EnsureInitialized()) {
        return std::unexpected(MakeError(
            HookErrorCode::AllocationFailure,
            site,
            "Failed to allocate a gateway arena within rel32 range of the game module"));
    }

    std::lock_guard lock(g_state.mutex);
    if (g_state.sealed) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            site,
            "Gateway arena is sealed; new hook sites cannot be registered"));
    }
    if (g_state.used + kGatewaySize > kArenaSize) {
        return std::unexpected(MakeError(
            HookErrorCode::TrampolineCapacityExceeded,
            site,
            std::format("Gateway arena exhausted ({}/{} bytes)",
                        g_state.used, kArenaSize)));
    }

    auto* slice = g_state.base + g_state.used;
    g_state.used += kGatewaySize;
    return ExecutableMemory::FromArenaSlice(slice, kGatewaySize);
}

std::expected<void, HookError> GatewayAllocator::Seal() {
    std::lock_guard lock(g_state.mutex);
    if (g_state.sealed) {
        return {};
    }
    if (!g_state.base || g_state.used == 0) {
        return {};
    }

    ScopedVirtualProtect protection(g_state.base, g_state.used, PAGE_EXECUTE_READ);
    if (!protection.Active()) {
        return std::unexpected(MakeError(
            HookErrorCode::ProtectionFailure,
            {},
            std::format("Failed to seal gateway arena as RX (Win32 error {})",
                        GetLastError())));
    }
    g_state.sealed = true;
    if (!protection.FlushInstructionCache()) {
        return std::unexpected(MakeError(
            HookErrorCode::CacheFlushFailure,
            {},
            std::format("Failed to flush gateway arena (Win32 error {})", GetLastError())));
    }
    protection.Dismiss();
    return {};
}

} // namespace jst::core::detail

namespace jst::core {

// Expose the absolute jump writer (declared in gateway_allocator.hpp).
void WriteAbsoluteJump(std::span<std::byte> destination, uintptr_t target) {
    detail::WriteAbsoluteJumpImpl(destination, target);
}

} // namespace jst::core
