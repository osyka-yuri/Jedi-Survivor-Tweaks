#include "core/pe_imports.hpp"
#include "test_check.hpp"

#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kNormalDirectoryRva = 0x100;
constexpr uint32_t kModuleNameRva = 0x180;
constexpr uint32_t kFunctionImportRva = 0x1A0;
constexpr uint32_t kNormalNamesRva = 0x200;
constexpr uint32_t kNormalAddressesRva = 0x220;

template <typename T>
T& At(std::vector<std::byte>& image, uint32_t rva, size_t index = 0) {
    return reinterpret_cast<T*>(image.data() + rva)[index];
}

void WriteString(
    std::vector<std::byte>& image, uint32_t rva, std::string_view value) {
    std::memcpy(image.data() + rva, value.data(), value.size());
    image[rva + value.size()] = std::byte{0};
}

void WriteImportByName(
    std::vector<std::byte>& image, uint32_t rva, std::string_view value) {
    At<uint16_t>(image, rva) = 0;
    WriteString(image, rva + offsetof(IMAGE_IMPORT_BY_NAME, Name), value);
}

std::vector<std::byte> MakeNormalImage() {
    std::vector<std::byte> image(0x800);
    auto& descriptor = At<IMAGE_IMPORT_DESCRIPTOR>(image, kNormalDirectoryRva);
    descriptor.Name = kModuleNameRva;
    descriptor.OriginalFirstThunk = kNormalNamesRva;
    descriptor.FirstThunk = kNormalAddressesRva;

    WriteString(image, kModuleNameRva, "D3D12.DLL");
    WriteImportByName(image, kFunctionImportRva, "D3D12CreateDevice");
    At<IMAGE_THUNK_DATA64>(image, kNormalNamesRva).u1.AddressOfData = kFunctionImportRva;
    At<IMAGE_THUNK_DATA64>(image, kNormalAddressesRva).u1.Function = 0x12345678;
    return image;
}

IMAGE_DATA_DIRECTORY NormalDirectory() {
    return IMAGE_DATA_DIRECTORY{
        .VirtualAddress = kNormalDirectoryRva,
        .Size = 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR),
    };
}

} // namespace

void TestPeImports() {
    using jst::core::FindNamedImportAddressSlot;
    using jst::core::ImportAddressTableKind;

    {
        auto image = MakeNormalImage();
        auto* expected = &At<IMAGE_THUNK_DATA64>(image, kNormalAddressesRva).u1.Function;
        const auto slot = FindNamedImportAddressSlot(
            image, NormalDirectory(), {}, "d3d12.dll", "D3D12CreateDevice");
        Check(slot && slot->address == expected &&
                  slot->table == ImportAddressTableKind::Normal,
              "normal import resolves and identifies its IAT address slot");
        Check(slot && *slot->address == 0x12345678,
              "normal import returns the mutable resolved IAT word");
        Check(!FindNamedImportAddressSlot(
                  image, NormalDirectory(), {}, "d3d12.dll", "OtherFunction"),
              "function names remain exact");
    }

    // Resolved IAT contents are never reinterpreted as name RVAs.
    {
        auto image = MakeNormalImage();
        At<IMAGE_IMPORT_DESCRIPTOR>(image, kNormalDirectoryRva).OriginalFirstThunk = 0;
        Check(!FindNamedImportAddressSlot(
                  image, NormalDirectory(), {}, "d3d12.dll", "D3D12CreateDevice"),
              "normal import without name table is skipped safely");
    }

    // Ordinal imports and malformed RVAs are ignored without leaving the image.
    {
        auto ordinal = MakeNormalImage();
        At<IMAGE_THUNK_DATA64>(ordinal, kNormalNamesRva).u1.Ordinal =
            IMAGE_ORDINAL_FLAG64 | 7;
        Check(!FindNamedImportAddressSlot(
                  ordinal, NormalDirectory(), {}, "d3d12.dll", "D3D12CreateDevice"),
              "ordinal import is not mistaken for a named import");

        auto badModule = MakeNormalImage();
        At<IMAGE_IMPORT_DESCRIPTOR>(badModule, kNormalDirectoryRva).Name = 0xFFFF'FFF0;
        Check(!FindNamedImportAddressSlot(
                  badModule, NormalDirectory(), {}, "d3d12.dll", "D3D12CreateDevice"),
              "out-of-bounds module RVA is rejected");

        auto badName = MakeNormalImage();
        At<IMAGE_THUNK_DATA64>(badName, kNormalNamesRva).u1.AddressOfData = 0xFFFF'FFF0;
        Check(!FindNamedImportAddressSlot(
                  badName, NormalDirectory(), {}, "d3d12.dll", "D3D12CreateDevice"),
              "out-of-bounds import-name RVA is rejected");

        auto badDirectory = MakeNormalImage();
        const IMAGE_DATA_DIRECTORY directory{
            .VirtualAddress = static_cast<DWORD>(badDirectory.size() - 1),
            .Size = sizeof(IMAGE_IMPORT_DESCRIPTOR),
        };
        Check(!FindNamedImportAddressSlot(
                  badDirectory, directory, {}, "d3d12.dll", "D3D12CreateDevice"),
              "truncated import directory is rejected");
    }

    // Delay-import tables use their own independent bounds and name table.
    {
        constexpr uint32_t descriptorRva = 0x300;
        constexpr uint32_t moduleRva = 0x380;
        constexpr uint32_t importRva = 0x3A0;
        constexpr uint32_t namesRva = 0x400;
        constexpr uint32_t addressesRva = 0x420;
        std::vector<std::byte> image(0x800);
        auto& descriptor = At<IMAGE_DELAYLOAD_DESCRIPTOR>(image, descriptorRva);
        descriptor.Attributes.RvaBased = 1;
        descriptor.DllNameRVA = moduleRva;
        descriptor.ImportNameTableRVA = namesRva;
        descriptor.ImportAddressTableRVA = addressesRva;
        WriteString(image, moduleRva, "d3d12.dll");
        WriteImportByName(image, importRva, "D3D12CreateDevice");
        At<IMAGE_THUNK_DATA64>(image, namesRva).u1.AddressOfData = importRva;
        At<IMAGE_THUNK_DATA64>(image, addressesRva).u1.Function = 0x87654321;
        const IMAGE_DATA_DIRECTORY delayDirectory{
            .VirtualAddress = descriptorRva,
            .Size = 2 * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR),
        };

        auto* expected = &At<IMAGE_THUNK_DATA64>(image, addressesRva).u1.Function;
        const auto slot = FindNamedImportAddressSlot(
            image, {}, delayDirectory, "D3D12.DLL", "D3D12CreateDevice");
        Check(slot && slot->address == expected &&
                  slot->table == ImportAddressTableKind::Delay,
              "delay import resolves and identifies its IAT address slot");
        descriptor.ImportNameTableRVA = 0;
        Check(!FindNamedImportAddressSlot(
                  image, {}, delayDirectory, "d3d12.dll", "D3D12CreateDevice"),
              "delay import without name table is skipped safely");
    }
}
