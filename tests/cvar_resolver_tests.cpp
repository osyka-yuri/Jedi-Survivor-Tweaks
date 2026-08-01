#include "core/cvar_overrides.hpp"
#include "core/cvar_resolver.hpp"
#include "core/cvar_scanner.hpp"
#include "cvar_test_fakes.hpp"
#include "test_check.hpp"

#include <array>
#include <cstdint>

namespace {

using cvar_test::Bind;
using cvar_test::FakeCVar;
using cvar_test::FakeVTable;

jst::core::ScanEntry GlobalCandidate(uintptr_t* globalPointer) {
    jst::core::ScanEntry scan;
    scan.name = L"test.Layout";
    scan.strAddr = 1;
    scan.globalPtrCandidates.push_back(
        reinterpret_cast<uintptr_t>(globalPointer));
    return scan;
}

} // namespace

void TestCVarResolver() {
    const jst::core::ModuleInfo module{};

    // This is intentionally independent of FakeVTable::Bind and guards the
    // documented UE 4.26/4.27 IConsoleObject/IConsoleVariable ABI boundary.
    Check(jst::core::cvar_layout::kVtableSetString == 16,
          "UE 4.26/4.27 string Set uses vtable slot 16");

    // LastSetBy is validation metadata, never an initialization state. Every
    // documented UE priority resolves through the identical path.
    for (uint32_t source = 0; source <= 9; ++source) {
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        object.flags = source << 24;
        uintptr_t global = reinterpret_cast<uintptr_t>(&object);

        const auto resolved = jst::core::ResolveFromScan(
            GlobalCandidate(&global), module, nullptr);
        Check(resolved.has_value(),
              "every documented LastSetBy source resolves immediately");
    }

    {
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        object.flags = 0x0A000000;
        uintptr_t global = reinterpret_cast<uintptr_t>(&object);
        Check(!jst::core::ResolveFromScan(
                  GlobalCandidate(&global), module, nullptr),
              "unknown LastSetBy upper byte is rejected");
    }

    {
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        vtable.slots[jst::core::cvar_layout::kVtableSetString] = 0;
        uintptr_t global = reinterpret_cast<uintptr_t>(&object);
        Check(!jst::core::ResolveFromScan(
                  GlobalCandidate(&global), module, nullptr),
              "object without an executable string setter is rejected");
    }

    // Standard storage remains explicitly inline even when object+0x20 holds
    // a perfectly valid pointer. The old pointer-looking heuristic is gone.
    {
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t external = 44;
        object.externalValue = &external;
        uintptr_t global = reinterpret_cast<uintptr_t>(&object);

        const auto resolved = jst::core::ResolveFromScan(
            GlobalCandidate(&global), module, nullptr);
        Check(resolved && jst::core::ResolveCVarReadAddress(*resolved) ==
                  reinterpret_cast<uintptr_t>(&object.inlineValue),
              "standard layout uses the declared inline read offset");
    }

    // The declarative external-reference layout remains available for CVars
    // that actually store a backing pointer and re-reads it on every access.
    {
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        float external = 60.0f;
        object.externalValue = &external;
        uintptr_t global = reinterpret_cast<uintptr_t>(&object);
        const jst::core::CVarOverride layout{
            .name = L"test.External",
            .readLayout = jst::core::CVarReadLayout{
                .kind = jst::core::CVarStorageKind::ExternalReference,
                .referenceOffset =
                    jst::core::cvar_layout::kRefOffset,
            },
        };
        const auto resolved = jst::core::ResolveFromScan(
            GlobalCandidate(&global), module, &layout);
        Check(resolved && jst::core::ResolveCVarReadAddress(*resolved) ==
                  reinterpret_cast<uintptr_t>(&external),
              "declared external-reference layout dereferences its backing value");

        float replacement = 75.0f;
        object.externalValue = &replacement;
        Check(resolved && jst::core::ResolveCVarReadAddress(*resolved) ==
                  reinterpret_cast<uintptr_t>(&replacement),
              "external-reference address is dereferenced again for each read");
    }

    {
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        uintptr_t global = reinterpret_cast<uintptr_t>(&object);

        const auto* layout = jst::core::FindCVarOverride(
            L"respawn.InterpolatedRendering");
        const auto resolved = jst::core::ResolveFromScan(
            GlobalCandidate(&global), module, layout);
        Check(resolved && jst::core::ResolveCVarReadAddress(*resolved) ==
                  reinterpret_cast<uintptr_t>(&object.customValue),
              "InterpolatedRendering uses its explicit 0x50 read offset");
    }

    // Primitive addresses remain useful to scanner diagnostics but can never
    // become write-capable ResolvedCVar instances.
    {
        int64_t primitive = 4000;
        jst::core::ScanEntry scan;
        scan.name = L"test.Primitive";
        scan.strAddr = 1;
        scan.refVarCandidates.push_back(
            reinterpret_cast<uintptr_t>(&primitive));
        Check(!jst::core::ResolveFromScan(scan, module, nullptr),
              "primitive-only candidate is never accepted for writes");
    }
}
