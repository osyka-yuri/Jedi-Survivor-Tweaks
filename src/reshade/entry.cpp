// ReShade addon entry-point. Compiled into the `ReleaseAddon|x64`
// configuration only (excluded from Release/Debug). ReShade discovers this
// addon by the `NAME` / `DESCRIPTION` exports below and the call to
// `reshade::register_addon` in DllMain.
//
// Bootstrap delegates to the same loader-agnostic core that the ASI variant
// uses (`jst::BootstrapAsync`), so behaviour, log output, and `.ini` reading
// are identical between the two build targets.
//
// The ImGui overlay lives in src/reshade/overlay.cpp and exposes a single
// DrawOverlay callback registered below. This entry point stays slim.

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>

#include "core/graphics_adapter_service.hpp"
#include "main_app.hpp"
#include "reshade/overlay.hpp"

// ReShade headers leak a couple of W4 noise patterns through their template
// expansions. Wrap the include site rather than tag the headers per-file in
// vcxproj (which would not work, since they are headers not TUs).
#pragma warning(push)
#pragma warning(disable: 4100)  // unreferenced formal parameter
#pragma warning(disable: 4127)  // conditional expression is constant
#pragma warning(disable: 4324)  // structure padded due to alignment
// imgui_compat.hpp must be included here (before reshade.hpp) so that
// IMGUI_VERSION_NUM is defined when register_addon runs. Without it the
// #if defined(IMGUI_VERSION_NUM) block in register_addon is skipped,
// imgui_function_table_instance() stays nullptr, and DrawOverlay crashes
// on the first ImGui call.
#include <external/reshade/imgui_compat.hpp>
#include <external/reshade/reshade.hpp>
#pragma warning(pop)

extern "C" __declspec(dllexport) const char* const NAME        = "JediSurvivorTweaks";
extern "C" __declspec(dllexport) const char* const DESCRIPTION =
    "FOV, camera distance, UI aspect, letterbox fix and custom CVars for Jedi: Survivor.";

namespace {
    HMODULE g_hModule = nullptr;
    std::atomic<uint64_t> g_lastAdapterKey{0};
    std::atomic<bool> g_hasAdapterKey{false};

    [[nodiscard]] jst::core::GraphicsAdapterId ToAdapterId(LUID luid) noexcept {
        return jst::core::GraphicsAdapterId{
            .lowPart = luid.LowPart,
            .highPart = luid.HighPart,
        };
    }

    void ReportD3D12Adapter(reshade::api::device* device) {
        if (!device || device->get_api() != reshade::api::device_api::d3d12) {
            return;
        }
        auto* native = reinterpret_cast<ID3D12Device*>(device->get_native());
        if (native) {
            const auto id = ToAdapterId(native->GetAdapterLuid());
            const uint64_t key =
                (static_cast<uint64_t>(static_cast<uint32_t>(id.highPart)) << 32) |
                id.lowPart;
            if (g_hasAdapterKey.load(std::memory_order_acquire) &&
                g_lastAdapterKey.load(std::memory_order_acquire) == key) {
                return;
            }
            g_lastAdapterKey.store(key, std::memory_order_release);
            g_hasAdapterKey.store(true, std::memory_order_release);
            jst::core::GraphicsAdapterService::Instance().ReportAdapterId(id);
        }
    }

    void ReportD3D12AdapterAfterPresent(
        reshade::api::command_queue* queue,
        reshade::api::swapchain* /*swapchain*/) {
        if (queue) {
            ReportD3D12Adapter(queue->get_device());
        }
    }
} // anonymous namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        if (!reshade::register_addon(hModule)) return FALSE;
        reshade::register_event<reshade::addon_event::init_device>(&ReportD3D12Adapter);
        // init_device is not replayed for an add-on loaded after device
        // creation. The first subsequent present recovers the existing device;
        // adapter-key deduplication makes later frames no-ops.
        reshade::register_event<reshade::addon_event::finish_present>(
            &ReportD3D12AdapterAfterPresent);
        reshade::register_overlay("JediSurvivorTweaks", &jst::DrawOverlay);
        jst::BootstrapAsync(hModule, jst::LoaderVariant::ReShadeAddon);
    } else if (reason == DLL_PROCESS_DETACH) {
        reshade::unregister_overlay("JediSurvivorTweaks", &jst::DrawOverlay);
        reshade::unregister_event<reshade::addon_event::finish_present>(
            &ReportD3D12AdapterAfterPresent);
        reshade::unregister_event<reshade::addon_event::init_device>(&ReportD3D12Adapter);
        reshade::unregister_addon(hModule);
    }
    return TRUE;
}
