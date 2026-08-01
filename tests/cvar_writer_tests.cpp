#include "core/cvar_system.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "tweaks/slider_specs.hpp"
#include "tweaks/slider_utils.hpp"
#include "test_check.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace {

using jst::core::CVarSetResult;
using jst::core::CVarSystem;
using jst::core::CVarSystemTestAccess;
using cvar_test::Bind;
using cvar_test::FakeCVar;
using cvar_test::FakeVTable;

CVarSystem& Cvars() {
    return CVarSystem::Instance();
}

bool Inject(
    std::wstring_view name,
    FakeCVar& object,
    uintptr_t readAddress = 0) {
    return CVarSystemTestAccess::InjectResolved(
        Cvars(),
        name,
        reinterpret_cast<uintptr_t>(&object),
        reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
        readAddress);
}

CVarSystem* g_reentrantSystem = nullptr;
void QueueReplacement(FakeCVar& object) {
    object.callback = nullptr;
    (void)g_reentrantSystem->SetInt(L"test.Reentrant", 22);
}

} // namespace

void TestCVarWriter() {
    // Constructor is allowed to remain Constructor forever before the
    // barrier; every documented source is written on exactly the same tick.
    for (uint32_t source = 0; source <= 9; ++source) {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        object.flags = (source << 24) | 0x40;
        int32_t value = 60;
        object.setterTarget = &value;
        Check(Inject(L"test.Source", object, reinterpret_cast<uintptr_t>(&value)),
              "fake CVar injection succeeds");

        Check(Cvars().SetInt(L"test.Source", 144).result == CVarSetResult::Queued,
              "first write is queued regardless of LastSetBy source");
        Check(value == 60 && object.setterCalls == 0 &&
                  (object.flags >> 24) == source,
              "queueing performs no engine write before the post-Tick barrier");

        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 144 && object.setterCalls == 1,
              "post-Tick pass invokes exactly one string setter");
        Check(object.lastSetBy == jst::core::cvar_layout::kSetByConsole &&
                  (object.flags & jst::core::cvar_layout::kSetByMask) ==
                      jst::core::cvar_layout::kSetByConsole,
              "writer passes exact ECVF_SetByConsole and observes Console");
    }

    // The writer never treats the stored LastSetBy value as readiness:
    // t.MaxFPS advances naturally from Scalability to Console through one API
    // call after the separate settings barrier has admitted execution.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        object.flags = 0x01000040;
        float value = 60.0f;
        object.targetIsFloat = true;
        object.setterTarget = &value;
        Check(Inject(L"t.MaxFPS", object, reinterpret_cast<uintptr_t>(&value)),
              "t.MaxFPS fake injection succeeds");

        Check(Cvars().SetFloat(L"t.MaxFPS", 117.0f).result == CVarSetResult::Queued,
              "Scalability t.MaxFPS request is queued");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 117.0f && object.setterCalls == 1 &&
                  (object.flags & jst::core::cvar_layout::kSetByMask) ==
                      jst::core::cvar_layout::kSetByConsole,
              "Scalability t.MaxFPS becomes Console in one setter call");
    }

    // Locale-independent float formatting uses the shortest round-trippable
    // representation instead of exposing binary float noise in UE and logs.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        float value = 0.0f;
        object.targetIsFloat = true;
        object.setterTarget = &value;
        Check(Inject(L"test.FloatText", object),
              "float-format fake injection succeeds");

        const std::array cases{
            std::pair{0.87f, std::wstring_view(L"0.9")},
            std::pair{1.83f, std::wstring_view(L"1.8")},
            std::pair{3.07f, std::wstring_view(L"3.1")},
            std::pair{6.68f, std::wstring_view(L"6.7")},
            std::pair{8.93f, std::wstring_view(L"8.9")},
            std::pair{7.18f, std::wstring_view(L"7.2")},
        };
        for (const auto& [raw, expected] : cases) {
            const float normalized = jst::tweaks::NormalizeFloatSlider(
                raw,
                jst::tweaks::kSharpenSliderSpec);
            Check(Cvars().SetFloat(L"test.FloatText", normalized).Accepted(),
                  "normalized float request is queued");
            const auto pending = CVarSystemTestAccess::PendingValue(
                Cvars(), L"test.FloatText");
            Check(pending && *pending == expected,
                  "slider float request preserves clean decimal text");
            CVarSystemTestAccess::PumpOnce(Cvars());
        }
        Check(object.setterCalls == cases.size(),
              "every clean slider float reaches the string setter once");
    }

    // Queue keys are case-insensitive and the most recent text wins without a
    // primary/shadow memory write.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        object.inlineValue = 101;
        object.shadowValue = 202;
        int32_t setterStorage = 0;
        object.setterTarget = &setterStorage;
        Check(Inject(L"test.Coalesce", object),
              "coalescing fake injection succeeds");

        const auto first = Cvars().SetString(L"TEST.coalesce", L"1e2");
        Check(first.result == CVarSetResult::Queued &&
                  first.ticket.Snapshot().state ==
                      jst::core::CVarCommandState::Pending,
              "first differently-cased request queues");
        const auto second = Cvars().SetString(L"test.COALESCE", L"37");
        Check(second.result == CVarSetResult::Replaced &&
                  first.ticket.Snapshot().state ==
                      jst::core::CVarCommandState::Superseded,
              "second differently-cased request replaces the first");
        const auto pending = CVarSystemTestAccess::PendingValue(
            Cvars(), L"Test.Coalesce");
        Check(pending && *pending == L"37",
              "coalesced queue retains the latest string verbatim");

        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(setterStorage == 37 && object.setterCalls == 1,
              "coalesced request emits one UE callback");
        Check(second.ticket.Snapshot().state ==
                  jst::core::CVarCommandState::Applied,
              "latest command ticket records the late setter result");
        Check(object.inlineValue == 101 && object.shadowValue == 202,
              "writer never mutates primary or shadow storage directly");
    }

    // Queue admission is intentionally distinct from the late engine result.
    // Pre-call validation failure completes the ticket and is never retried.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 0;
        object.setterTarget = &value;
        Check(Inject(L"test.LateFailure", object),
              "late-failure fake injection succeeds");

        const auto command = Cvars().SetInt(L"test.LateFailure", 9);
        Check(command.Accepted() && command.ticket.Snapshot().state ==
                  jst::core::CVarCommandState::Pending,
              "accepted queue command remains pending before post-Tick");
        vtable.slots[jst::core::cvar_layout::kVtableSetString] = 0;
        CVarSystemTestAccess::PumpOnce(Cvars());
        const auto failed = command.ticket.Snapshot();
        Check(failed.state == jst::core::CVarCommandState::Failed &&
                  !failed.diagnostic.empty() && object.setterCalls == 0,
              "invalidated setter becomes a late ticket failure");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(object.setterCalls == 0 &&
                  !CVarSystemTestAccess::PendingValue(
                      Cvars(), L"test.LateFailure"),
              "failed one-shot command is removed without retry");
    }

    // A CVar registered after the startup barrier is picked up by the next
    // post-Tick pass once the resolver supplies its object.
    {
        CVarSystemTestAccess::Reset(Cvars());
        Check(Cvars().SetInt(L"test.Late", 77).result == CVarSetResult::Queued,
              "late CVar request enters the startup/runtime queue");
        CVarSystemTestAccess::PumpOnce(Cvars());

        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 0;
        object.setterTarget = &value;
        Check(CVarSystemTestAccess::CommitPendingAsResolved(
                  Cvars(),
                  L"TEST.late",
                  reinterpret_cast<uintptr_t>(&object),
                  reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
                  reinterpret_cast<uintptr_t>(&value)),
              "late registration commits case-insensitively");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 77 && object.setterCalls == 1,
              "late registration writes on the first following Tick");
    }

    // Setter callbacks may enqueue a newer value. Snapshot execution holds no
    // CVar mutex, and generation checks preserve the reentrant replacement.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 0;
        object.setterTarget = &value;
        object.callback = &QueueReplacement;
        g_reentrantSystem = &Cvars();
        Check(Inject(L"test.Reentrant", object),
              "reentrant fake injection succeeds");

        (void)Cvars().SetInt(L"test.Reentrant", 11);
        CVarSystemTestAccess::PumpOnce(Cvars());
        const auto replacement = CVarSystemTestAccess::PendingValue(
            Cvars(), L"test.Reentrant");
        Check(value == 11 && replacement && *replacement == L"22",
              "reentrant callback leaves its newer generation queued");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 22 && object.setterCalls == 2,
              "reentrant replacement executes exactly once next Tick");
    }

    {
        CVarSystemTestAccess::Reset(Cvars());
        Cvars().MarkUnavailable("dispatcher unavailable in test");
        Check(Cvars().SetInt(L"test.Rejected", 1).result == CVarSetResult::Rejected,
              "fail-closed writer rejects requests without a dispatcher");
        Cvars().Stop();
        Check(Cvars().SetInt(L"test.Stopped", 1).result == CVarSetResult::Rejected,
              "shutdown rejects all subsequent requests");
    }
}
