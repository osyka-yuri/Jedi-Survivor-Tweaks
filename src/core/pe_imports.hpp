#pragma once

#include <windows.h>
#include <delayimp.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace jst::core {

// Locate the first named import address slot in a validated in-memory image.
// Normal imports without OriginalFirstThunk are deliberately skipped because
// their resolved IAT no longer contains IMAGE_IMPORT_BY_NAME RVAs.
enum class ImportAddressTableKind {
    Normal,
    Delay,
};

struct ImportAddressSlot {
    ULONG_PTR* address = nullptr;
    ImportAddressTableKind table = ImportAddressTableKind::Normal;
};

[[nodiscard]] std::optional<ImportAddressSlot> FindNamedImportAddressSlot(
    std::span<std::byte> image,
    const IMAGE_DATA_DIRECTORY& normalImports,
    const IMAGE_DATA_DIRECTORY& delayImports,
    std::string_view moduleName,
    std::string_view functionName) noexcept;

} // namespace jst::core
