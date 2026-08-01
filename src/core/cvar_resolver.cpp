#include "cvar_resolver.hpp"

#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"
#include "cvar_scanner.hpp"
#include "logging.hpp"
#include "pe_utils.hpp"
#include "string_utils.hpp"

namespace jst::core {

namespace {

[[nodiscard]] bool ValidateCVarObject(uintptr_t object) noexcept {
    if (!object || !utils::IsValidPointer(object)) {
        return false;
    }
    const uintptr_t vtable = utils::SafeReadPointer(object);
    if (!utils::IsValidPointer(vtable)) {
        return false;
    }
    const uintptr_t setter = utils::SafeReadPointer(
        vtable + cvar_layout::kVtableSetString * sizeof(uintptr_t));
    if (!utils::IsExecutablePointer(setter)) {
        return false;
    }
    const uintptr_t flagsAddress = object + cvar_layout::kFlagsOffset;
    if (!utils::IsValidPointer(flagsAddress)) {
        return false;
    }
    const uint32_t flags = utils::SafeReadInt32(flagsAddress);
    return (flags & cvar_layout::kSetByMask) <= cvar_layout::kSetByConsole;
}

ResolvedCVar MakeResolvedCVar(
    uintptr_t object,
    const CVarOverride* override) {
    const uintptr_t vtable = utils::SafeReadPointer(object);
    const uintptr_t setter = utils::SafeReadPointer(
        vtable + cvar_layout::kVtableSetString * sizeof(uintptr_t));

    return ResolvedCVar{
        .cvarObject = object,
        .stringSetter = setter,
        .readLayout = override
            ? override->readLayout
            : CVarReadLayout{},
    };
}

} // namespace

bool ValidateResolvedCVar(const ResolvedCVar& resolved) noexcept {
    if (!ValidateCVarObject(resolved.cvarObject) ||
        !utils::IsExecutablePointer(resolved.stringSetter)) {
        return false;
    }
    const uintptr_t vtable = utils::SafeReadPointer(resolved.cvarObject);
    const uintptr_t currentSetter = utils::SafeReadPointer(
        vtable + cvar_layout::kVtableSetString * sizeof(uintptr_t));
    return currentSetter == resolved.stringSetter;
}

uintptr_t ResolveCVarReadAddress(const ResolvedCVar& resolved) noexcept {
    if (!resolved.cvarObject || !utils::IsValidPointer(resolved.cvarObject)) {
        return 0;
    }
    switch (resolved.readLayout.kind) {
    case CVarStorageKind::Inline:
        return resolved.cvarObject + resolved.readLayout.valueOffset;
    case CVarStorageKind::ExternalReference:
        return utils::SafeReadPointer(
            resolved.cvarObject + resolved.readLayout.referenceOffset);
    }
    return 0;
}

std::optional<ResolvedCVar> ResolveFromScan(
    const ScanEntry& scan,
    [[maybe_unused]] const ModuleInfo& module,
    const CVarOverride* override) {
    JST_LOG_DEBUG(
        "'{}': resolving CVar object (global={}, ref={}, static={}).",
        utils::WideToUtf8(scan.name),
        scan.globalPtrCandidates.size(),
        scan.refVarCandidates.size(),
        scan.staticRefCandidates.size());

    const auto tryObject = [&](uintptr_t object)
        -> std::optional<ResolvedCVar> {
        if (!ValidateCVarObject(object)) {
            return std::nullopt;
        }
        return MakeResolvedCVar(object, override);
    };

    for (const uintptr_t globalPointer : scan.globalPtrCandidates) {
        if (auto resolved = tryObject(utils::SafeReadPointer(globalPointer))) {
            return resolved;
        }
    }

    for (const uintptr_t reference : scan.refVarCandidates) {
        if (auto resolved = tryObject(utils::SafeReadPointer(reference))) {
            return resolved;
        }
    }

    for (const uintptr_t reference : scan.staticRefCandidates) {
        if (auto resolved = tryObject(utils::SafeReadPointer(reference))) {
            return resolved;
        }
        // Some registration records store the object itself rather than an
        // IConsoleVariable**. Accept that form only after full object checks.
        if (auto resolved = tryObject(reference)) {
            return resolved;
        }
    }

    if (scan.strAddr) {
        JST_LOG_INFO(
            "'{}': name found at 0x{:X}, but no valid IConsoleVariable object is registered yet.",
            utils::WideToUtf8(scan.name),
            scan.strAddr);
    }
    return std::nullopt;
}

} // namespace jst::core
