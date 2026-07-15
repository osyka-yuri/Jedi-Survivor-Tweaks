#include "d3d12_device_observer.hpp"

#include "graphics_adapter_service.hpp"
#include "import_address_hook.hpp"
#include "pe_imports.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <span>

namespace jst::core {

namespace {

using D3D12CreateDeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

HRESULT WINAPI ObservedD3D12CreateDevice(
    IUnknown* adapter,
    D3D_FEATURE_LEVEL minimumFeatureLevel,
    REFIID riid,
    void** device);

ImportAddressHook g_createDeviceHook{
    reinterpret_cast<ULONG_PTR>(&ObservedD3D12CreateDevice)};

[[nodiscard]] bool RangeBelongsToImage(
    const std::byte* imageBase,
    const void* address,
    size_t size,
    bool requireReadable = false) noexcept {
    if (!imageBase || !address || size == 0) {
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
            memory.AllocationBase != imageBase ||
            (requireReadable &&
             (memory.State != MEM_COMMIT ||
              (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0))) {
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
        cursor = (std::min)(end, regionEnd);
    }
    return true;
}

[[nodiscard]] GraphicsAdapterId ToAdapterId(LUID luid) noexcept {
    return GraphicsAdapterId{
        .lowPart = luid.LowPart,
        .highPart = luid.HighPart,
    };
}

HRESULT WINAPI ObservedD3D12CreateDevice(
    IUnknown* adapter,
    D3D_FEATURE_LEVEL minimumFeatureLevel,
    REFIID riid,
    void** device) {
    const auto original = reinterpret_cast<D3D12CreateDeviceFn>(
        g_createDeviceHook.Original());
    if (!original) {
        return E_FAIL;
    }

    const HRESULT result = original(adapter, minimumFeatureLevel, riid, device);
    // The MSVC delay helper replaces its IAT thunk with the resolved export.
    // Adopt that export and put the observer back before returning to the game.
    g_createDeviceHook.CompleteCall();
    if (FAILED(result) || !device || !*device) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
    auto* unknown = static_cast<IUnknown*>(*device);
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&d3d12Device)))) {
        GraphicsAdapterService::Instance().ReportAdapterId(
            ToAdapterId(d3d12Device->GetAdapterLuid()));
    }
    return result;
}

} // namespace

bool InstallD3D12DeviceObserverEarly() noexcept {
    auto* imageBase = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (!RangeBelongsToImage(
            imageBase, imageBase, sizeof(IMAGE_DOS_HEADER), true)) {
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    const auto imageAddress = reinterpret_cast<uintptr_t>(imageBase);
    if (ntOffset > std::numeric_limits<uintptr_t>::max() - imageAddress) {
        return false;
    }
    const auto ntAddress = reinterpret_cast<const void*>(
        imageAddress + ntOffset);
    if (!RangeBelongsToImage(
            imageBase, ntAddress, sizeof(IMAGE_NT_HEADERS64), true)) {
        return false;
    }
    const auto* nt = static_cast<const IMAGE_NT_HEADERS64*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage < sizeof(IMAGE_DOS_HEADER) ||
        nt->OptionalHeader.SizeOfHeaders < ntOffset + sizeof(IMAGE_NT_HEADERS64) ||
        nt->OptionalHeader.SizeOfImage < nt->OptionalHeader.SizeOfHeaders ||
        !RangeBelongsToImage(imageBase, imageBase, nt->OptionalHeader.SizeOfImage)) {
        return false;
    }

    const std::span image(imageBase, nt->OptionalHeader.SizeOfImage);
    const auto& directories = nt->OptionalHeader.DataDirectory;
    const auto normalImports =
        nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT
        ? directories[IMAGE_DIRECTORY_ENTRY_IMPORT]
        : IMAGE_DATA_DIRECTORY{};
    const auto delayImports =
        nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
        ? directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
        : IMAGE_DATA_DIRECTORY{};
    const auto slot = FindNamedImportAddressSlot(
        image,
        normalImports,
        delayImports,
        "d3d12.dll",
        "D3D12CreateDevice");
    return slot && g_createDeviceHook.Install(*slot);
}

} // namespace jst::core
