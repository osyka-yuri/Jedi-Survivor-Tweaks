#include "core/import_address_hook.hpp"
#include "test_check.hpp"

#include <atomic>

void TestImportAddressHook() {
    using jst::core::ImportAddressHook;
    using jst::core::ImportAddressSlot;
    using jst::core::ImportAddressTableKind;

    constexpr ULONG_PTR delayThunk = 0x1000;
    constexpr ULONG_PTR resolvedExport = 0x2000;
    constexpr ULONG_PTR observer = 0x3000;

    {
        alignas(ULONG_PTR) ULONG_PTR slot = delayThunk;
        ImportAddressHook hook(observer);
        Check(hook.Install(ImportAddressSlot{
                  .address = &slot,
                  .table = ImportAddressTableKind::Delay,
              }),
              "delay IAT observer installs over the helper thunk");
        Check(slot == observer && hook.Original() == delayThunk &&
                  hook.AwaitingDelayResolution(),
              "delay observer retains the helper until first resolution");

        // Models __delayLoadHelper2 overwriting the IAT while the observer is
        // calling the original thunk.
        std::atomic_ref<ULONG_PTR>(slot).store(resolvedExport, std::memory_order_release);
        hook.CompleteCall();
        Check(slot == observer && hook.Original() == resolvedExport,
              "resolved delay export is adopted and observer is restored");
        Check(!hook.AwaitingDelayResolution(),
              "delay restoration becomes a one-time transition");
        hook.CompleteCall();
        Check(slot == observer && hook.Original() == resolvedExport,
              "completed delay restoration is idempotent");
    }

    {
        alignas(ULONG_PTR) ULONG_PTR slot = resolvedExport;
        ImportAddressHook hook(observer);
        Check(hook.Install(ImportAddressSlot{
                  .address = &slot,
                  .table = ImportAddressTableKind::Normal,
              }),
              "normal IAT observer installs over resolved import");
        Check(slot == observer && hook.Original() == resolvedExport &&
                  !hook.AwaitingDelayResolution(),
              "normal imports do not enter delay-resolution state");
        Check(hook.Install(ImportAddressSlot{
                  .address = &slot,
                  .table = ImportAddressTableKind::Normal,
              }),
              "normal IAT installation is idempotent");
    }
}
