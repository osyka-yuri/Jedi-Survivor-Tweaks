#include "instruction_relocator.hpp"

#include <Zydis.h>

#include <cstring>
#include <format>
#include <limits>

namespace jst::core::detail {

using jst::core::MakeError;

namespace {

bool IsRelativeControlFlow(const ZydisDecodedInstruction& instruction,
                           std::span<const ZydisDecodedOperand> operands) {
    if (instruction.meta.category != ZYDIS_CATEGORY_CALL &&
        instruction.meta.category != ZYDIS_CATEGORY_COND_BR &&
        instruction.meta.category != ZYDIS_CATEGORY_UNCOND_BR) {
        return false;
    }

    for (const auto& operand : operands) {
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            operand.imm.is_relative == ZYAN_TRUE) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool InitX64Decoder(ZydisDecoder& decoder) {
    return ZYAN_SUCCESS(
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64));
}

} // namespace

std::expected<size_t, HookError>
MeasureInstructionWindow(std::span<const std::byte> code,
                         size_t minimumLength,
                         std::string_view site) {
    if (minimumLength == 0 || code.size() < minimumLength) {
        return std::unexpected(MakeError(
            HookErrorCode::InsufficientPatchWindow,
            site,
            std::format("Need at least {} byte(s), but only {} are available",
                        minimumLength, code.size())));
    }

    ZydisDecoder decoder;
    if (!InitX64Decoder(decoder)) {
        return std::unexpected(
            MakeError(HookErrorCode::DecodeFailed, site, "Failed to initialize Zydis decoder"));
    }

    size_t length = 0;
    while (length < minimumLength) {
        ZydisDecodedInstruction instruction;
        const size_t available = std::min<size_t>(ZYDIS_MAX_INSTRUCTION_LENGTH,
                                                   code.size() - length);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
                &decoder, nullptr, code.data() + length, available, &instruction))) {
            return std::unexpected(MakeError(
                HookErrorCode::DecodeFailed,
                site,
                std::format("Failed to decode instruction at +0x{:X}", length)));
        }
        if (instruction.length == 0 || length + instruction.length > code.size()) {
            return std::unexpected(MakeError(
                HookErrorCode::InsufficientPatchWindow,
                site,
                std::format("Instruction at +0x{:X} crosses the available code buffer", length)));
        }
        length += instruction.length;
    }
    return length;
}

std::expected<int32_t, HookError>
CalculateRel32(uintptr_t instructionEnd, uintptr_t target, std::string_view site) {
    const auto displacement =
        static_cast<int64_t>(target) - static_cast<int64_t>(instructionEnd);
    if (displacement < std::numeric_limits<int32_t>::min() ||
        displacement > std::numeric_limits<int32_t>::max()) {
        return std::unexpected(MakeError(
            HookErrorCode::Rel32OutOfRange,
            site,
            std::format("Target 0x{:X} is outside rel32 range from 0x{:X}",
                        target, instructionEnd)));
    }
    return static_cast<int32_t>(displacement);
}

std::expected<std::vector<std::byte>, HookError>
RelocateInstructions(std::span<const std::byte> code,
                     uintptr_t oldAddress,
                     uintptr_t newAddress,
                     size_t outputCapacity,
                     std::string_view site) {
    if (code.size() > outputCapacity) {
        return std::unexpected(MakeError(
            HookErrorCode::TrampolineCapacityExceeded,
            site,
            std::format("Relocated window requires {} byte(s), capacity is {}",
                        code.size(), outputCapacity)));
    }

    ZydisDecoder decoder;
    if (!InitX64Decoder(decoder)) {
        return std::unexpected(
            MakeError(HookErrorCode::DecodeFailed, site, "Failed to initialize Zydis decoder"));
    }

    std::vector<std::byte> relocated(code.begin(), code.end());
    size_t offset = 0;
    while (offset < code.size()) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                &decoder,
                code.data() + offset,
                code.size() - offset,
                &instruction,
                operands))) {
            return std::unexpected(MakeError(
                HookErrorCode::DecodeFailed,
                site,
                std::format("Failed to decode relocated instruction at +0x{:X}", offset)));
        }

        const auto visibleOperands =
            std::span<const ZydisDecodedOperand>(operands, instruction.operand_count_visible);
        if (IsRelativeControlFlow(instruction, visibleOperands)) {
            return std::unexpected(MakeError(
                HookErrorCode::UnsupportedRelativeControlFlow,
                site,
                std::format("Relative control flow at +0x{:X} is not supported in ReplayOriginal",
                            offset)));
        }

        for (const auto& operand : visibleOperands) {
            if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY ||
                (operand.mem.base != ZYDIS_REGISTER_RIP &&
                 operand.mem.base != ZYDIS_REGISTER_EIP)) {
                continue;
            }
            if (instruction.raw.disp.size != 32) {
                return std::unexpected(MakeError(
                    HookErrorCode::DecodeFailed,
                    site,
                    std::format("Unsupported RIP-relative displacement width at +0x{:X}", offset)));
            }

            ZyanU64 absoluteAddress = 0;
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(
                    &instruction, &operand, oldAddress + offset, &absoluteAddress))) {
                return std::unexpected(MakeError(
                    HookErrorCode::DecodeFailed,
                    site,
                    std::format("Failed to resolve RIP-relative operand at +0x{:X}", offset)));
            }

            const uintptr_t newInstructionEnd = newAddress + offset + instruction.length;
            auto displacement = CalculateRel32(
                newInstructionEnd, static_cast<uintptr_t>(absoluteAddress), site);
            if (!displacement) {
                return std::unexpected(std::move(displacement).error());
            }

            const size_t displacementOffset = offset + instruction.raw.disp.offset;
            if (displacementOffset + sizeof(int32_t) > relocated.size()) {
                return std::unexpected(MakeError(
                    HookErrorCode::DecodeFailed,
                    site,
                    std::format("Invalid displacement offset at +0x{:X}", offset)));
            }
            std::memcpy(relocated.data() + displacementOffset,
                        &*displacement,
                        sizeof(*displacement));
        }

        offset += instruction.length;
    }

    return relocated;
}

} // namespace jst::core::detail
