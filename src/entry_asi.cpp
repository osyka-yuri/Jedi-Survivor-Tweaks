// ASI entry-point. Compiled into `Debug|x64` and `Release|x64` configurations
// (excluded from `ReleaseAddon|x64`). The ASI loader (Ultimate ASI Loader and
// compatible) finds and invokes the `InitializeASI` export after `DllMain`
// returns. We bootstrap the loader-agnostic core from there.

#include <windows.h>

#include "core/d3d12_device_observer.hpp"
#include "main_app.hpp"

namespace {
    HMODULE g_hModule = nullptr;
} // anonymous namespace

extern "C" __declspec(dllexport) void InitializeASI() {
    // The fallback to GetModuleHandleW(nullptr) returns the process's main
    // module (the .exe), not the DLL. It's only reachable if the ASI loader
    // calls InitializeASI before our DllMain runs -- which Ultimate ASI
    // Loader and compatible loaders never do. Kept defensively so the export
    // doesn't dereference a stale null if loaders change behaviour.
    jst::BootstrapAsync(g_hModule ? g_hModule : GetModuleHandleW(nullptr), jst::LoaderVariant::Asi);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        // Must run before bootstrap and before the game can create its D3D12
        // device. The observer performs only bounded PE/IAT work here.
        (void)jst::core::InstallD3D12DeviceObserverEarly();
        InitializeASI();
    }
    return TRUE;
}
