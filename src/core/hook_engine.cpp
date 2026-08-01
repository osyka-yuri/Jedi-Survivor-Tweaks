#include "hook_engine.hpp"

#include "detour_gate.hpp"
#include "gateway_allocator.hpp"
#include "instruction_relocator.hpp"
#include "hook_transaction.hpp"
#include "logging.hpp"
#include "scoped_virtual_protect.hpp"
#include "thread_suspender.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace jst::core {

namespace {

constexpr size_t kRelativeJumpSize = 5;
constexpr size_t kAbsoluteJumpSize = 14;

void WriteGuardedReturningCall(
    std::span<std::byte> destination,
    uintptr_t gateState,
    uintptr_t detour,
    uintptr_t continuation) {
    size_t cursor = 0;
    const auto emitByte = [&](uint8_t value) {
        destination[cursor++] = static_cast<std::byte>(value);
    };
    const auto emitBytes = [&](std::initializer_list<uint8_t> values) {
        for (const uint8_t value : values) {
            emitByte(value);
        }
    };
    const auto emitAddress = [&](uintptr_t value) {
        std::memcpy(destination.data() + cursor, &value, sizeof(value));
        cursor += sizeof(value);
    };
    const auto emitMovRax = [&](uintptr_t value) {
        emitBytes({0x48, 0xB8});
        emitAddress(value);
    };
    const auto emitMovR11 = [&](uintptr_t value) {
        emitBytes({0x49, 0xBB});
        emitAddress(value);
    };

    // state bit 0 = admission, bits 1..31 = active calls in units of two.
    // The cmpxchg either publishes activity while admission is still open or
    // observes the closed state and bypasses DLL code entirely.
    emitMovR11(gateState);
    const size_t retryOffset = cursor;
    emitBytes({0x41, 0x8B, 0x03});             // mov eax, [r11]
    emitBytes({0xA8, 0x01});                   // test al, 1
    emitByte(0x74);                            // je bypass
    const size_t bypassDisplacement = cursor++;
    emitBytes({0x44, 0x8D, 0x50, 0x02});       // lea r10d, [rax+2]
    emitBytes({0xF0, 0x45, 0x0F, 0xB1, 0x13}); // lock cmpxchg [r11], r10d
    emitByte(0x75);                            // jne retry
    const size_t retryDisplacement = cursor++;

    // This guarded mode is deliberately limited to returning void detours with
    // at most four register arguments. The 0x28-byte frame provides shadow
    // space and preserves Windows x64 stack alignment.
    emitBytes({0x48, 0x83, 0xEC, 0x28});       // sub rsp, 28h
    emitMovRax(detour);
    emitBytes({0xFF, 0xD0});                   // call rax
    emitBytes({0x48, 0x83, 0xC4, 0x28});       // add rsp, 28h

    emitMovR11(gateState);
    emitBytes({0xF0, 0x41, 0x83, 0x2B, 0x02}); // lock sub dword ptr [r11], 2
    // With admission open the remaining state is at least 1. During shutdown
    // only the last active detour reaches zero and needs to wake the waiter.
    emitByte(0x75);                            // jne return
    const size_t returnDisplacement = cursor++;
    emitBytes({0x48, 0x83, 0xEC, 0x28});       // sub rsp, 28h
    emitBytes({0x4C, 0x89, 0xD9});             // mov rcx, r11
    emitMovRax(reinterpret_cast<uintptr_t>(&::WakeByAddressAll));
    emitBytes({0xFF, 0xD0});                   // call rax
    emitBytes({0x48, 0x83, 0xC4, 0x28});       // add rsp, 28h
    const size_t returnOffset = cursor;
    emitByte(0xC3);                            // ret

    const size_t bypassOffset = cursor;
    emitMovRax(continuation);
    emitBytes({0xFF, 0xE0});                   // jmp rax

    const auto patchRel8 = [&](size_t displacementOffset, size_t target) {
        const ptrdiff_t displacement =
            static_cast<ptrdiff_t>(target) -
            static_cast<ptrdiff_t>(displacementOffset + 1);
        destination[displacementOffset] = static_cast<std::byte>(
            static_cast<uint8_t>(static_cast<int8_t>(displacement)));
    };
    patchRel8(bypassDisplacement, bypassOffset);
    patchRel8(retryDisplacement, retryOffset);
    patchRel8(returnDisplacement, returnOffset);
}

void LogSuspenderWarning(std::string_view operation) {
    JST_LOG_WARNING(
        "ThreadSuspender suspended 0 threads before {}; patching proceeds without suspension",
        operation);
}

} // namespace

// -----------------------------------------------------------------------------
// Hook implementation
// -----------------------------------------------------------------------------

Hook::Hook(HookSiteSpec spec,
           ParsedPattern pattern,
           int32_t patternOffset,
           uintptr_t detour)
    : m_spec(std::move(spec)),
      m_parsedPattern(std::move(pattern)),
      m_patternOffset(patternOffset),
      m_detour(detour) {}

Hook::Hook(HookSiteSpec spec, uintptr_t targetRva, uintptr_t detour)
    : m_spec(std::move(spec)), m_target(targetRva), m_detour(detour) {}

Hook::~Hook() {
    if (m_installed) {
        (void)Uninstall();
    }
}

std::expected<void, HookError> Hook::ResolveAddress() {
    if (IsPatternHook()) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "ResolveAddress called for a pattern-based hook"));
    }

    auto module = GetGameModuleInfo();
    if (!module) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Failed to query the game module"));
    }
    if (m_target == 0) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Address hook RVA must be non-zero"));
    }

    m_target += module->base;
    return FinalizeTarget(module->base, module->base + module->size);
}

std::expected<void, HookError> Hook::FinalizeResolve(uintptr_t matchAddress) {
    if (!IsPatternHook()) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "FinalizeResolve called for an address-based hook"));
    }

    auto module = GetGameModuleInfo();
    if (!module) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Failed to query the game module"));
    }

    m_target = matchAddress + m_patternOffset;
    return FinalizeTarget(module->base, module->base + module->size);
}

std::expected<void, HookError>
Hook::FinalizeTarget(uintptr_t moduleBase, uintptr_t moduleEnd) {
    if (m_target < moduleBase || m_target >= moduleEnd || m_detour == 0) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            std::format("Invalid target 0x{:X} or detour 0x{:X}", m_target, m_detour)));
    }
    if (m_spec.minimumOverwriteLength < kRelativeJumpSize) {
        return std::unexpected(MakeError(
            HookErrorCode::InsufficientPatchWindow,
            Name(),
            std::format("Minimum overwrite length {} is smaller than the {}-byte splice",
                        m_spec.minimumOverwriteLength, kRelativeJumpSize)));
    }

    const size_t available =
        std::min<size_t>(kOriginalBytesCapacity, moduleEnd - m_target);
    auto measured = detail::MeasureInstructionWindow(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(m_target), available),
        m_spec.minimumOverwriteLength,
        Name());
    if (!measured) {
        return std::unexpected(std::move(measured).error());
    }
    if (*measured > kOriginalBytesCapacity) {
        return std::unexpected(MakeError(
            HookErrorCode::InsufficientPatchWindow,
            Name(),
            std::format("Overwrite window {} exceeds the {}-byte limit",
                        *measured, kOriginalBytesCapacity)));
    }

    m_overwriteLength = *measured;
    m_resumeAddress = m_target + m_overwriteLength;
    m_parsedPattern = {};

    auto gateway = AllocateGateway(Name());
    if (!gateway) {
        return std::unexpected(std::move(gateway).error());
    }
    m_gateway = std::move(*gateway);

    if (m_spec.detourGate &&
        m_spec.continuation == HookContinuation::ReplayOriginal) {
        auto continuationGateway = AllocateGateway(Name());
        if (!continuationGateway) {
            return std::unexpected(std::move(continuationGateway).error());
        }
        m_continuationGateway = std::move(*continuationGateway);
    }

    auto displacement =
        detail::CalculateRel32(m_target + kRelativeJumpSize, m_gateway.Address(), Name());
    if (!displacement) {
        return std::unexpected(std::move(displacement).error());
    }
    m_cachedRel32 = *displacement;

    JST_LOG_INFO(
        "Resolved hook '{}' | group='{}' | target=0x{:X} | window={} | continuation={}",
        Name(),
        Group(),
        m_target,
        m_overwriteLength,
        m_spec.continuation == HookContinuation::Resume ? "resume" : "replay-original");
    return {};
}

uintptr_t Hook::ContinuationAddress() const noexcept {
    if (!IsResolved()) {
        return 0;
    }
    if (m_spec.continuation == HookContinuation::Resume) {
        return m_resumeAddress;
    }
    return m_spec.detourGate
        ? m_continuationGateway.Address()
        : m_gateway.Address() + kAbsoluteJumpSize;
}

std::expected<void, HookError> Hook::ValidatePrepare() {
    if (m_prepared) {
        return {};
    }
    if (!IsResolved() || !m_gateway.Valid()) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Hook must be resolved before it can be prepared"));
    }

    if (m_spec.detourGate && m_spec.detourGate->StateAddress() == 0) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Guarded detour gate must be opened before hook preparation"));
    }

    if (m_spec.continuation == HookContinuation::ReplayOriginal) {
        if (m_spec.detourGate && !m_continuationGateway.Valid()) {
            return std::unexpected(MakeError(
                HookErrorCode::InvalidState,
                Name(),
                "Guarded replay hook is missing its continuation gateway"));
        }
        const size_t relocatedCapacity = m_spec.detourGate
            ? m_continuationGateway.Bytes().size() - kAbsoluteJumpSize
            : m_gateway.Bytes().size() - 2 * kAbsoluteJumpSize;
        const uintptr_t relocatedAddress = m_spec.detourGate
            ? m_continuationGateway.Address()
            : m_gateway.Address() + kAbsoluteJumpSize;
        auto relocated = detail::RelocateInstructions(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(m_target),
                                       m_overwriteLength),
            m_target,
            relocatedAddress,
            relocatedCapacity,
            Name());
        if (!relocated) {
            return std::unexpected(std::move(relocated).error());
        }

        const size_t resumeJumpOffset = kAbsoluteJumpSize + relocated->size();
        if (resumeJumpOffset + kAbsoluteJumpSize > m_gateway.Bytes().size()) {
            return std::unexpected(MakeError(
                HookErrorCode::TrampolineCapacityExceeded,
                Name(),
                "Relocated instructions and resume jump exceed the gateway slot"));
        }
    }

    return {};
}

std::expected<void, HookError> Hook::Prepare() {
    if (m_prepared) {
        return {};
    }

    auto validated = ValidatePrepare();
    if (!validated) {
        return validated;
    }

    MEMORY_BASIC_INFORMATION memoryInfo{};
    if (VirtualQuery(reinterpret_cast<const void*>(m_target),
                     &memoryInfo,
                     sizeof(memoryInfo)) != sizeof(memoryInfo) ||
        memoryInfo.State != MEM_COMMIT) {
        return std::unexpected(MakeError(
            HookErrorCode::ProtectionFailure,
            Name(),
            "Failed to query splice-site memory protection"));
    }
    m_originalProtection = memoryInfo.Protect;

    std::memcpy(m_originalBytes.data(),
                reinterpret_cast<const void*>(m_target),
                m_overwriteLength);
    m_originalBytesLength = static_cast<uint8_t>(m_overwriteLength);

    auto gateway = m_gateway.Bytes();
    std::fill(gateway.begin(), gateway.end(), std::byte{0xCC});

    if (m_spec.continuation == HookContinuation::ReplayOriginal) {
        auto continuationGateway = m_spec.detourGate
            ? m_continuationGateway.Bytes()
            : gateway.subspan(kAbsoluteJumpSize);
        if (m_spec.detourGate) {
            std::fill(
                continuationGateway.begin(),
                continuationGateway.end(),
                std::byte{0xCC});
        }
        const size_t relocatedCapacity =
            continuationGateway.size() - kAbsoluteJumpSize;
        auto relocated = detail::RelocateInstructions(
            std::span<const std::byte>(m_originalBytes.data(), m_originalBytesLength),
            m_target,
            ContinuationAddress(),
            relocatedCapacity,
            Name());
        if (!relocated) {
            return std::unexpected(std::move(relocated).error());
        }

        std::copy(
            relocated->begin(),
            relocated->end(),
            continuationGateway.begin());
        const size_t resumeJumpOffset = relocated->size();
        WriteAbsoluteJump(
            continuationGateway.subspan(resumeJumpOffset, kAbsoluteJumpSize),
            m_resumeAddress);
    }

    if (m_spec.detourGate) {
        WriteGuardedReturningCall(
            gateway,
            m_spec.detourGate->StateAddress(),
            m_detour,
            ContinuationAddress());
    } else {
        WriteAbsoluteJump(gateway.first(kAbsoluteJumpSize), m_detour);
    }

    m_prepared = true;
    return {};
}

std::expected<void, HookError>
Hook::WriteTarget(std::span<const std::byte> bytes) {
    if (bytes.size() != m_overwriteLength || m_originalProtection == 0) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Invalid protected-write state"));
    }

    ScopedVirtualProtect protection(
        reinterpret_cast<void*>(m_target), m_overwriteLength, PAGE_EXECUTE_READWRITE);
    if (!protection.Active()) {
        return std::unexpected(MakeError(
            HookErrorCode::ProtectionFailure,
            Name(),
            std::format("Failed to make splice site writable (Win32 error {})",
                        GetLastError())));
    }

    std::memcpy(reinterpret_cast<void*>(m_target), bytes.data(), bytes.size());

    if (!protection.FlushInstructionCache()) {
        return std::unexpected(MakeError(
            HookErrorCode::CacheFlushFailure,
            Name(),
            std::format("Failed to flush splice-site instructions (Win32 error {})",
                        GetLastError())));
    }
    return {};
}

std::expected<void, HookError> Hook::Install() {
    if (m_installed) {
        return {};
    }
    if (!m_prepared) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Hook must be prepared before installation"));
    }

    if (!m_cachedRel32) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            Name(),
            "Hook is missing a cached rel32 displacement"));
    }

    std::array<std::byte, kOriginalBytesCapacity> patch{};
    std::fill_n(patch.begin(), m_overwriteLength, std::byte{0x90});
    patch[0] = std::byte{0xE9};
    std::memcpy(patch.data() + 1, &*m_cachedRel32, sizeof(*m_cachedRel32));

    auto result = WriteTarget(
        std::span<const std::byte>(patch.data(), m_overwriteLength));
    if (!result) {
        auto rollback = WriteTarget(
            std::span<const std::byte>(m_originalBytes.data(), m_originalBytesLength));
        if (!rollback) {
            m_installRollbackPending = true;
            return rollback;
        }
        return result;
    }

    m_installed = true;
    m_installRollbackPending = false;
    return {};
}

std::expected<void, HookError> Hook::Uninstall() {
    if (!m_installed && !m_installRollbackPending) {
        return {};
    }

    auto result = WriteTarget(
        std::span<const std::byte>(m_originalBytes.data(), m_originalBytesLength));
    if (!result) {
        return result;
    }
    m_installed = false;
    m_installRollbackPending = false;
    return {};
}

void HookEngine::Shutdown() {
    UninstallAll();
    m_hooks.clear();
}

std::expected<void, HookError>
HookEngine::RegisterPatternHook(HookSiteSpec spec,
                                std::string_view pattern,
                                int32_t offset,
                                uintptr_t detour) {
    auto parsed = ParsePattern(pattern);
    if (!parsed) {
        return std::unexpected(MakeError(
            HookErrorCode::DecodeFailed,
            spec.name,
            std::format("Pattern parse failed: {}", parsed.error())));
    }
    return InsertHook(
        Hook(std::move(spec), std::move(*parsed), offset, detour));
}

std::expected<void, HookError>
HookEngine::RegisterAddressHook(HookSiteSpec spec,
                                uintptr_t targetRva,
                                uintptr_t detour) {
    Hook hook(std::move(spec), targetRva, detour);
    auto resolved = hook.ResolveAddress();
    if (!resolved) {
        return resolved;
    }
    return InsertHook(std::move(hook));
}

std::expected<void, HookError> HookEngine::InsertHook(Hook&& hook) {
    if (m_hooks.contains(hook.Name())) {
        return std::unexpected(MakeError(
            HookErrorCode::InvalidState,
            hook.Name(),
            "Hook site is already registered"));
    }
    m_hooks.emplace(hook.Name(), std::move(hook));
    return {};
}

std::vector<HookError> HookEngine::ResolveAll() {
    std::vector<Hook*> pending;
    std::vector<ParsedPattern> patterns;
    pending.reserve(m_hooks.size());
    patterns.reserve(m_hooks.size());

    for (auto&& [name, hook] : m_hooks) {
        if (hook.IsPatternHook() && !hook.IsResolved()) {
            pending.push_back(&hook);
            patterns.push_back(hook.Pattern());
        }
    }
    if (pending.empty()) {
        return {};
    }

    auto module = GetGameModuleInfo();
    if (!module) {
        return {MakeError(
            HookErrorCode::InvalidState, {}, "Failed to query the game module")};
    }
    const auto region = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(module->base), module->size);
    auto matches = FindPatternsBatch(patterns, region);

    std::vector<HookError> errors;
    for (size_t index = 0; index < pending.size(); ++index) {
        Hook& hook = *pending[index];
        if (!matches[index]) {
            errors.push_back(MakeError(
                HookErrorCode::PatternNotFound,
                hook.Name(),
                "Pattern was not found"));
            continue;
        }
        auto resolved = hook.FinalizeResolve(*matches[index]);
        if (!resolved) {
            errors.push_back(std::move(resolved).error());
        }
    }
    return errors;
}

std::vector<HookError> HookEngine::InstallAll() {
    std::flat_map<std::string, std::vector<Hook*>, std::less<>> groups;
    for (auto&& [name, hook] : m_hooks) {
        groups[hook.Group()].push_back(&hook);
    }

    std::vector<HookError> errors;
    std::flat_map<std::string, uint8_t, std::less<>> readyGroups;
    for (auto&& [group, hooks] : groups) {
        bool ready = true;
        for (Hook* hook : hooks) {
            auto validated = hook->ValidatePrepare();
            if (!validated) {
                errors.push_back(std::move(validated).error());
                ready = false;
                break;
            }
        }
        if (ready) {
            for (Hook* hook : hooks) {
                auto prepared = hook->Prepare();
                if (!prepared) {
                    errors.push_back(std::move(prepared).error());
                    ready = false;
                    break;
                }
            }
        }
        readyGroups[group] = static_cast<uint8_t>(ready);
    }

    bool hasReadyGroup = std::ranges::any_of(
        readyGroups, [](const auto& entry) { return entry.second != 0; });
    if (hasReadyGroup) {
        auto sealed = SealGatewayArena();
        if (!sealed) {
            HookError failure = std::move(sealed).error();
            for (auto&& [group, ready] : readyGroups) {
                if (!ready) {
                    continue;
                }
                HookError groupFailure = failure;
                groupFailure.site = group;
                errors.push_back(std::move(groupFailure));
                ready = false;
            }
        }
    }

    ThreadSuspender suspender;
    if (suspender.SuspendedCount() == 0) {
        LogSuspenderWarning("InstallAll");
    }

    for (auto&& [group, hooks] : groups) {
        if (!readyGroups[group]) {
            continue;
        }

        std::vector<detail::HookTransactionStep> steps;
        steps.reserve(hooks.size());
        for (Hook* hook : hooks) {
            steps.push_back(detail::HookTransactionStep{
                .site = hook->Name(),
                .hook = hook,
            });
        }

        auto transactionErrors = detail::RunHookTransaction(steps);
        if (!transactionErrors.empty()) {
            errors.insert(errors.end(),
                          std::make_move_iterator(transactionErrors.begin()),
                          std::make_move_iterator(transactionErrors.end()));
            continue;
        }

        for (Hook* hook : hooks) {
            JST_LOG_INFO(
                "Installed hook '{}' | group='{}' | target=0x{:X} | gateway=0x{:X} | continuation=0x{:X}",
                hook->Name(),
                group,
                hook->Target(),
                hook->Gateway(),
                hook->ContinuationAddress());
        }
    }

    return errors;
}

void HookEngine::UninstallAll() {
    ThreadSuspender suspender;
    if (suspender.SuspendedCount() == 0) {
        LogSuspenderWarning("UninstallAll");
    }

    for (auto it = m_hooks.rbegin(); it != m_hooks.rend(); ++it) {
        Hook& hook = it->second;
        if (!hook.IsInstalled()) {
            continue;
        }
        auto removed = hook.Uninstall();
        if (!removed) {
            JST_LOG_ERROR("Failed to uninstall hook '{}': {}",
                          hook.Name(), removed.error().message);
        } else {
            JST_LOG_INFO("Uninstalled hook '{}'.", hook.Name());
        }
    }
}

bool HookEngine::IsHookInstalled(std::string_view name) const {
    auto it = m_hooks.find(name);
    return it != m_hooks.end() && it->second.IsInstalled();
}

bool HookEngine::IsGroupInstalled(std::string_view group) const {
    bool found = false;
    for (auto&& [name, hook] : m_hooks) {
        if (hook.Group() != group) {
            continue;
        }
        found = true;
        if (!hook.IsInstalled()) {
            return false;
        }
    }
    return found;
}

std::optional<uintptr_t>
HookEngine::GetContinuationAddress(std::string_view name) const {
    auto it = m_hooks.find(name);
    if (it == m_hooks.end()) {
        return std::nullopt;
    }
    const uintptr_t address = it->second.ContinuationAddress();
    return address != 0 ? std::optional{address} : std::nullopt;
}

std::expected<void, HookError>
HookEngine::UnregisterHook(std::string_view name) {
    auto it = m_hooks.find(name);
    if (it == m_hooks.end()) {
        return {};
    }

    ThreadSuspender suspender;
    if (it->second.IsInstalled() || it->second.NeedsInstallRollback()) {
        auto removed = it->second.Uninstall();
        if (!removed) {
            return removed;
        }
    }
    m_hooks.erase(it);
    return {};
}

} // namespace jst::core
