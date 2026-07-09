#pragma once

#include "hook_types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace jst::core::detail {

// -----------------------------------------------------------------------------
// Instruction analysis & relocation helpers (Zydis-backed)
//
// These are used exclusively during hook preparation:
//   - MeasureInstructionWindow: determine how many bytes must be overwritten
//     to contain at least `minimumLength` complete instructions.
//   - CalculateRel32: safe signed 32-bit displacement for relative jumps.
//   - RelocateInstructions: copy a window of instructions into a gateway while
//     rewriting RIP-relative memory operands. Called only for ReplayOriginal
//     hooks; Resume mode never relocates instructions into the gateway.
//     Relative control-flow instructions are rejected.
//
// All functions are pure with respect to the provided byte span and addresses.
// -----------------------------------------------------------------------------
[[nodiscard]] std::expected<size_t, HookError>
MeasureInstructionWindow(std::span<const std::byte> code,
                         size_t minimumLength,
                         std::string_view site = {});

[[nodiscard]] std::expected<int32_t, HookError>
CalculateRel32(uintptr_t instructionEnd,
               uintptr_t target,
               std::string_view site = {});

[[nodiscard]] std::expected<std::vector<std::byte>, HookError>
RelocateInstructions(std::span<const std::byte> code,
                     uintptr_t oldAddress,
                     uintptr_t newAddress,
                     size_t outputCapacity,
                     std::string_view site = {});

} // namespace jst::core::detail
