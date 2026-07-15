#include "pe_imports.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace jst::core {

namespace {

[[nodiscard]] bool RangeIsReadable(const void* address, size_t size) noexcept {
    if (!address || size == 0) {
        return false;
    }

    const auto begin = reinterpret_cast<uintptr_t>(address);
    if (size > std::numeric_limits<uintptr_t>::max() - begin) {
        return false;
    }
    const uintptr_t end = begin + size;
    uintptr_t cursor = begin;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0 ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        const auto regionBegin = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > std::numeric_limits<uintptr_t>::max() - regionBegin) {
            return false;
        }
        const uintptr_t regionEnd = regionBegin + memory.RegionSize;
        if (regionEnd <= cursor) {
            return false;
        }
        cursor = std::min(end, regionEnd);
    }
    return true;
}

class ImageView final {
public:
    explicit ImageView(std::span<std::byte> image) : m_image(image) {}

    template <typename T>
    [[nodiscard]] T* At(uint32_t rva, size_t count = 1) const noexcept {
        if (rva > m_image.size() || count > (m_image.size() - rva) / sizeof(T)) {
            return nullptr;
        }
        auto* address = m_image.data() + rva;
        if (reinterpret_cast<uintptr_t>(address) % alignof(T) != 0) {
            return nullptr;
        }
        return RangeIsReadable(address, count * sizeof(T))
            ? reinterpret_cast<T*>(address)
            : nullptr;
    }

    [[nodiscard]] bool StringEquals(uint32_t rva, std::string_view expected) const noexcept {
        if (rva >= m_image.size() || expected.size() >= m_image.size() - rva) {
            return false;
        }
        const auto* text = reinterpret_cast<const char*>(m_image.data() + rva);
        if (!RangeIsReadable(text, expected.size() + 1)) {
            return false;
        }
        return std::memcmp(text, expected.data(), expected.size()) == 0 &&
               text[expected.size()] == '\0';
    }

    [[nodiscard]] bool StringEqualsIgnoreCaseAscii(
        uint32_t rva, std::string_view expected) const noexcept {
        if (rva >= m_image.size() || expected.size() >= m_image.size() - rva) {
            return false;
        }
        const auto* text = reinterpret_cast<const char*>(m_image.data() + rva);
        if (!RangeIsReadable(text, expected.size() + 1)) {
            return false;
        }
        for (size_t index = 0; index < expected.size(); ++index) {
            const auto fold = [](unsigned char value) noexcept {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value + ('a' - 'A'))
                    : value;
            };
            if (fold(static_cast<unsigned char>(text[index])) !=
                fold(static_cast<unsigned char>(expected[index]))) {
                return false;
            }
        }
        return text[expected.size()] == '\0';
    }

    [[nodiscard]] size_t RemainingElements(uint32_t rva, size_t elementSize) const noexcept {
        return rva <= m_image.size() ? (m_image.size() - rva) / elementSize : 0;
    }

private:
    std::span<std::byte> m_image;
};

[[nodiscard]] bool DirectoryIsUsable(
    const ImageView& image,
    const IMAGE_DATA_DIRECTORY& directory,
    size_t elementSize) noexcept {
    return directory.VirtualAddress != 0 && directory.Size >= elementSize &&
           image.RemainingElements(directory.VirtualAddress, elementSize) != 0;
}

[[nodiscard]] ULONG_PTR* FindInThunkTable(
    const ImageView& image,
    uint32_t namesRva,
    uint32_t addressesRva,
    std::string_view functionName) noexcept {
    const size_t count = std::min(
        image.RemainingElements(namesRva, sizeof(IMAGE_THUNK_DATA64)),
        image.RemainingElements(addressesRva, sizeof(IMAGE_THUNK_DATA64)));
    auto* names = image.At<IMAGE_THUNK_DATA64>(namesRva, count);
    auto* addresses = image.At<IMAGE_THUNK_DATA64>(addressesRva, count);
    if (!names || !addresses) {
        return nullptr;
    }

    for (size_t index = 0; index < count; ++index) {
        const auto nameData = names[index].u1.AddressOfData;
        if (nameData == 0) {
            break;
        }
        if (IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal) ||
            nameData > std::numeric_limits<uint32_t>::max()) {
            continue;
        }

        const uint32_t nameRva = static_cast<uint32_t>(nameData);
        if (!image.At<IMAGE_IMPORT_BY_NAME>(nameRva) ||
            nameRva > std::numeric_limits<uint32_t>::max() -
                          static_cast<uint32_t>(offsetof(IMAGE_IMPORT_BY_NAME, Name))) {
            continue;
        }
        const uint32_t textRva =
            nameRva + static_cast<uint32_t>(offsetof(IMAGE_IMPORT_BY_NAME, Name));
        if (image.StringEquals(textRva, functionName)) {
            return &addresses[index].u1.Function;
        }
    }
    return nullptr;
}

[[nodiscard]] ULONG_PTR* FindNormalImport(
    const ImageView& image,
    const IMAGE_DATA_DIRECTORY& directory,
    std::string_view moduleName,
    std::string_view functionName) noexcept {
    if (!DirectoryIsUsable(image, directory, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        return nullptr;
    }

    const size_t directoryCount = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    const size_t imageCount = image.RemainingElements(
        directory.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    const size_t count = std::min(directoryCount, imageCount);
    auto* descriptors = image.At<IMAGE_IMPORT_DESCRIPTOR>(directory.VirtualAddress, count);
    if (!descriptors) {
        return nullptr;
    }

    for (size_t index = 0; index < count; ++index) {
        const auto& descriptor = descriptors[index];
        if (descriptor.Name == 0) {
            break;
        }
        if (descriptor.OriginalFirstThunk == 0 || descriptor.FirstThunk == 0 ||
            !image.StringEqualsIgnoreCaseAscii(descriptor.Name, moduleName)) {
            continue;
        }
        if (auto* slot = FindInThunkTable(
                image, descriptor.OriginalFirstThunk, descriptor.FirstThunk, functionName)) {
            return slot;
        }
    }
    return nullptr;
}

[[nodiscard]] ULONG_PTR* FindDelayImport(
    const ImageView& image,
    const IMAGE_DATA_DIRECTORY& directory,
    std::string_view moduleName,
    std::string_view functionName) noexcept {
    if (!DirectoryIsUsable(image, directory, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR))) {
        return nullptr;
    }

    const size_t directoryCount = directory.Size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
    const size_t imageCount = image.RemainingElements(
        directory.VirtualAddress, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR));
    const size_t count = std::min(directoryCount, imageCount);
    auto* descriptors = image.At<IMAGE_DELAYLOAD_DESCRIPTOR>(directory.VirtualAddress, count);
    if (!descriptors) {
        return nullptr;
    }

    for (size_t index = 0; index < count; ++index) {
        const auto& descriptor = descriptors[index];
        if (descriptor.DllNameRVA == 0) {
            break;
        }
        if (descriptor.Attributes.RvaBased == 0 ||
            descriptor.ImportNameTableRVA == 0 ||
            descriptor.ImportAddressTableRVA == 0 ||
            !image.StringEqualsIgnoreCaseAscii(descriptor.DllNameRVA, moduleName)) {
            continue;
        }
        if (auto* slot = FindInThunkTable(
                image,
                descriptor.ImportNameTableRVA,
                descriptor.ImportAddressTableRVA,
                functionName)) {
            return slot;
        }
    }
    return nullptr;
}

} // namespace

std::optional<ImportAddressSlot> FindNamedImportAddressSlot(
    std::span<std::byte> image,
    const IMAGE_DATA_DIRECTORY& normalImports,
    const IMAGE_DATA_DIRECTORY& delayImports,
    std::string_view moduleName,
    std::string_view functionName) noexcept {
    const ImageView view(image);
    if (auto* slot = FindNormalImport(view, normalImports, moduleName, functionName)) {
        return ImportAddressSlot{
            .address = slot,
            .table = ImportAddressTableKind::Normal,
        };
    }
    if (auto* slot = FindDelayImport(view, delayImports, moduleName, functionName)) {
        return ImportAddressSlot{
            .address = slot,
            .table = ImportAddressTableKind::Delay,
        };
    }
    return std::nullopt;
}

} // namespace jst::core
