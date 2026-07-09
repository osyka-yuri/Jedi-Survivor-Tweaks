#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace jst::core {

// -----------------------------------------------------------------------------
// Error model (used by HookEngine, instruction relocation, gateway, etc.)
// -----------------------------------------------------------------------------
enum class HookContinuation {
    Resume,          // After detour, execution resumes at the instruction immediately
                     // after the overwritten bytes at the original site.
    ReplayOriginal,  // The overwritten instructions are relocated (with RIP fixups)
                     // into the gateway; the detour returns into that copy so original
                     // side-effects and any following control flow see the same bytes.
};

enum class HookErrorCode {
    DecodeFailed,
    InsufficientPatchWindow,
    UnsupportedRelativeControlFlow,
    Rel32OutOfRange,
    TrampolineCapacityExceeded,
    ProtectionFailure,
    CacheFlushFailure,
    InvalidState,
    PatternNotFound,
    AllocationFailure,
};

struct HookError {
    HookErrorCode code = HookErrorCode::InvalidState;
    std::string site;
    std::string message;
};

[[nodiscard]] inline HookError MakeError(HookErrorCode code,
                                         std::string_view site,
                                         std::string message) {
    return HookError{code, std::string(site), std::move(message)};
}

struct HookSiteSpec {
    std::string name;
    std::string group;
    size_t minimumOverwriteLength = 5;
    HookContinuation continuation = HookContinuation::Resume;
};

// -----------------------------------------------------------------------------
// ExecutableMemory
//
// Thin handle to a gateway slice allocated from the shared arena. Non-owning
// (arena lifetime > all hooks). Used to write trampolines and relocated code.
// -----------------------------------------------------------------------------
class ExecutableMemory final {
public:
    ExecutableMemory() = default;
    [[nodiscard]] static ExecutableMemory FromArenaSlice(void* address, size_t size) noexcept;
    ~ExecutableMemory() = default;

    ExecutableMemory(const ExecutableMemory&) = delete;
    ExecutableMemory& operator=(const ExecutableMemory&) = delete;
    ExecutableMemory(ExecutableMemory&&) noexcept = default;
    ExecutableMemory& operator=(ExecutableMemory&&) noexcept = default;

    [[nodiscard]] uintptr_t Address() const noexcept {
        return reinterpret_cast<uintptr_t>(m_memory);
    }
    [[nodiscard]] std::span<std::byte> Bytes() const noexcept {
        return m_memory ? std::span<std::byte>(m_memory, m_size) : std::span<std::byte>{};
    }
    [[nodiscard]] bool Valid() const noexcept { return m_memory != nullptr; }

private:
    ExecutableMemory(std::byte* memory, size_t size) noexcept
        : m_memory(memory), m_size(size) {}

    std::byte* m_memory = nullptr;
    size_t m_size = 0;
};

inline ExecutableMemory ExecutableMemory::FromArenaSlice(void* address, size_t size) noexcept {
    return ExecutableMemory(static_cast<std::byte*>(address), size);
}

} // namespace jst::core
