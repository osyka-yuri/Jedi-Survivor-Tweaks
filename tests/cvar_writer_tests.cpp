#include "core/cvar_system.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "tweaks/slider_specs.hpp"
#include "tweaks/slider_utils.hpp"
#include "test_check.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
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

void QueueLiveCorrectionReplacement(FakeCVar& object) {
    object.callback = nullptr;
    (void)g_reentrantSystem->SetInt(L"test.CorrectionLive", 33);
}

struct MutexDestructionProbe {
    std::mutex* mutex = nullptr;
    bool* destroyedOutsideMutex = nullptr;

    ~MutexDestructionProbe() {
        if (mutex->try_lock()) {
            *destroyedOutsideMutex = true;
            mutex->unlock();
        } else {
            *destroyedOutsideMutex = false;
        }
    }
};

class ScopedResolverResume final {
public:
    explicit ScopedResolverResume(CVarSystem& system)
        : m_system(system) {}

    ~ScopedResolverResume() {
        Resume();
    }

    void Resume() {
        if (m_active) {
            CVarSystemTestAccess::ResumeResolverAfterModuleFetch(m_system);
            m_active = false;
        }
    }

private:
    CVarSystem& m_system;
    bool m_active = true;
};

} // namespace

void TestCVarWriter() {
    // Serialization is derived from the public value variant before the cache
    // lock. A bad float invalidates the whole related batch, including an
    // otherwise valid integer request.
    {
        CVarSystemTestAccess::Reset(Cvars());
        const std::array invalidBatch{
            jst::core::CVarWriteRequest{
                .name = L"test.Typed.Int", .value = int32_t{7}},
            jst::core::CVarWriteRequest{
                .name = L"test.Typed.Float",
                .value = std::numeric_limits<float>::infinity()},
        };
        const auto invalid = Cvars().QueueBatch(invalidBatch);
        Check(!invalid.Accepted() &&
                  CVarSystemTestAccess::CacheSize(Cvars()) == 0,
              "non-finite typed float rejects the complete batch before mutation");

        const std::array validBatch{
            jst::core::CVarWriteRequest{
                .name = L"test.Typed.Int", .value = int32_t{-7}},
            jst::core::CVarWriteRequest{
                .name = L"test.Typed.Float", .value = 1.25f},
        };
        Check(Cvars().QueueBatch(validBatch).Accepted() &&
                  CVarSystemTestAccess::PendingValue(Cvars(), L"test.Typed.Int") == L"-7" &&
                  CVarSystemTestAccess::PendingValue(Cvars(), L"test.Typed.Float") == L"1.25",
              "typed batch serialization supplies canonical text and comparison kinds");
    }

    {
        CVarSystemTestAccess::Reset(Cvars());
        CVarSystemTestAccess::SetNextGeneration(
            Cvars(), std::numeric_limits<uint64_t>::max());
        const auto finalGeneration = Cvars().SetInt(L"test.Generation.Last", 1);
        const std::array exhaustedBatch{
            jst::core::CVarWriteRequest{
                .name = L"test.Generation.First", .value = int32_t{2}},
            jst::core::CVarWriteRequest{
                .name = L"test.Generation.Second", .value = int32_t{3}},
        };
        const auto exhausted = Cvars().QueueBatch(exhaustedBatch);
        Check(finalGeneration.Accepted() && !exhausted.Accepted() &&
                  CVarSystemTestAccess::CacheSize(Cvars()) == 1 &&
                  CVarSystemTestAccess::PendingValue(
                      Cvars(), L"test.Generation.Last") == L"1",
              "generation exhaustion rejects the full later batch without reuse or mutation");
    }

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
    // call after the first completed Tick has admitted execution.
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

    // During the bounded startup window, only a value change triggers a
    // reapply. A LastSetBy-only change is deliberately ignored, and the
    // reconciler stops writing after its deadline.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        float value = 60.0f;
        object.targetIsFloat = true;
        object.setterTarget = &value;
        Check(Inject(L"test.StartupFloat", object,
                     reinterpret_cast<uintptr_t>(&value)),
              "startup reconciliation float injection succeeds");

        Check(Cvars().SetFloat(L"test.StartupFloat", 120.0f).Accepted(),
              "startup reconciliation target queues");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 120.0f && object.setterCalls == 1,
              "first post-Tick applies and captures the resulting value");

        object.flags =
            (object.flags & jst::core::cvar_layout::kFlagBitsMask) |
            jst::core::cvar_layout::kSetByScalability;
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 120.0f && object.setterCalls == 1,
              "LastSetBy-only change does not trigger reconciliation");

        value = 90.0f;
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 120.0f && object.setterCalls == 2,
              "changed float value is restored during the startup window");

        CVarSystemTestAccess::ExpireStartupReconciliation(Cvars());
        value = 75.0f;
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 75.0f && object.setterCalls == 2,
              "expired startup window leaves later changes untouched");
    }

    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 0;
        object.setterTarget = &value;
        Check(Inject(L"test.StartupInt", object,
                     reinterpret_cast<uintptr_t>(&value)),
              "startup reconciliation integer injection succeeds");

        Check(Cvars().SetInt(L"test.StartupInt", 5).Accepted(),
              "integer startup reconciliation target queues");
        CVarSystemTestAccess::PumpOnce(Cvars());
        value = 2;
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 5 && object.setterCalls == 2,
              "changed integer value is restored during the startup window");
    }

    // A failed reconciliation setter is reported once and removed from the
    // bounded set instead of becoming a retry loop.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 1;
        object.setterTarget = &value;
        Check(Inject(L"test.ReconcileFailure", object,
                     reinterpret_cast<uintptr_t>(&value)),
              "reconciliation-failure CVar injection succeeds");
        Check(Cvars().SetInt(L"test.ReconcileFailure", 5).Accepted(),
              "reconciliation-failure startup target queues");
        CVarSystemTestAccess::PumpOnce(Cvars());

        const uintptr_t setter =
            vtable.slots[jst::core::cvar_layout::kVtableSetString];
        vtable.slots[jst::core::cvar_layout::kVtableSetString] = 0;
        value = 2;
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 2 && object.setterCalls == 1,
              "invalidated setter fails without a reconciliation write");

        vtable.slots[jst::core::cvar_layout::kVtableSetString] = setter;
        value = 3;
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 3 && object.setterCalls == 1,
              "failed reconciliation target is not retried later");
    }

    // Specialized tweaks can request one post-Tick read without installing a
    // persistent watch. The callback runs without the cache mutex and may
    // safely enqueue a normal command for the next pass.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 3000;
        object.setterTarget = &value;
        Check(Inject(L"r.Streaming.PoolSize", object,
                     reinterpret_cast<uintptr_t>(&value)),
              "one-shot read CVar injection succeeds");

        size_t callbackCalls = 0;
        Check(Cvars().ReadIntOnce(
                  L"R.STREAMING.POOLSIZE",
                  [&](std::expected<int32_t, std::string> result) {
                      ++callbackCalls;
                      Check(result.has_value() && *result == 3000,
                            "one-shot read returns the post-Tick engine value");
                      Check(Cvars().SetInt(
                                L"r.Streaming.PoolSize", 3072).Accepted(),
                            "one-shot callback can reenter the command queue");
                  }),
              "valid one-shot read request is accepted");
        Check(callbackCalls == 0,
              "one-shot read never executes before the post-Tick barrier");

        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(callbackCalls == 1 && object.setterCalls == 0,
              "one-shot read executes once without writing its CVar");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(callbackCalls == 1 && value == 3072 && object.setterCalls == 1,
              "one-shot read is removed and its reentrant command runs next Tick");
    }

    // One-shot cohorts are unarmed before Tick, gain an immutable deadline at
    // the barrier, and terminal resolver errors are delivered only by the
    // following post-Tick pass. A throwing callback cannot suppress a peer or
    // poison a later fresh cohort with the same case-insensitive name.
    {
        CVarSystemTestAccess::Reset(Cvars());
        size_t errorCallbacks = 0;
        Check(Cvars().ReadIntOnce(
                  L"test.Deadline",
                  [&](std::expected<int32_t, std::string> value) {
                      ++errorCallbacks;
                      Check(!value, "expired cohort reports an error result");
                      throw std::runtime_error("expected callback probe");
                  }) &&
                  Cvars().ReadIntOnce(
                      L"TEST.deadline",
                      [&](std::expected<int32_t, std::string> value) {
                          ++errorCallbacks;
                          Check(!value, "callback peer receives the same terminal error");
                      }),
              "pre-barrier one-shot reads are admitted case-insensitively");
        Check(!CVarSystemTestAccess::Deadline(Cvars(), L"test.deadline"),
              "pre-barrier cohort has no deadline");
        CVarSystemTestAccess::PumpOnce(Cvars());
        const auto deadline = CVarSystemTestAccess::Deadline(Cvars(), L"test.deadline");
        Check(deadline.has_value(), "first Tick arms the pre-barrier cohort deadline");
        Check(Cvars().ReadIntOnce(
                  L"test.Deadline", [](std::expected<int32_t, std::string>) {}) &&
                  CVarSystemTestAccess::Deadline(Cvars(), L"test.deadline") == deadline,
              "attaching later work never extends an existing cohort deadline");

        CVarSystemTestAccess::ExpireUnresolved(Cvars());
        Check(errorCallbacks == 0,
              "resolver-side terminalization never invokes a user callback");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(errorCallbacks == 2 && CVarSystemTestAccess::CacheSize(Cvars()) == 0,
              "terminal error batch runs peers once on the next post-Tick and erases authority");

        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 19;
        object.setterTarget = &value;
        size_t freshCalls = 0;
        Check(Cvars().ReadIntOnce(
                  L"TEST.DEADLINE",
                  [&](std::expected<int32_t, std::string> result) {
                      ++freshCalls;
                      Check(result && *result == 19,
                            "fresh same-name cohort reads after an old terminal error");
                  }) &&
                  Inject(L"test.deadline", object,
                         reinterpret_cast<uintptr_t>(&value)),
              "fresh same-name work creates a new cache authority");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(freshCalls == 1,
              "fresh cohort callback executes independently of the old error batch");
    }

    // Destruction of callback captures is deliberately detached from the
    // cache mutex for both attached and already-ready error batches.
    {
        CVarSystemTestAccess::Reset(Cvars());
        bool attachedDestroyedOutside = false;
        {
            auto probe = std::make_shared<MutexDestructionProbe>(
                CVarSystemTestAccess::CacheMutex(Cvars()),
                &attachedDestroyedOutside);
            Check(Cvars().ReadIntOnce(
                      L"test.AttachedDestroy",
                      [probe](std::expected<int32_t, std::string>) {}),
                  "attached destruction probe is queued");
        }
        Cvars().Stop();
        Check(attachedDestroyedOutside,
              "attached callback capture is destroyed after releasing the cache mutex");

        CVarSystemTestAccess::Reset(Cvars());
        bool readyDestroyedOutside = false;
        {
            auto probe = std::make_shared<MutexDestructionProbe>(
                CVarSystemTestAccess::CacheMutex(Cvars()),
                &readyDestroyedOutside);
            Check(Cvars().ReadIntOnce(
                      L"test.ReadyDestroy",
                      [probe](std::expected<int32_t, std::string>) {}),
                  "ready destruction probe is queued");
        }
        CVarSystemTestAccess::PumpOnce(Cvars());
        CVarSystemTestAccess::ExpireUnresolved(Cvars());
        Cvars().Stop();
        Check(readyDestroyedOutside,
              "ready error callback capture is destroyed after releasing the cache mutex");
    }

    // An allocation failure while detaching resolved cohorts keeps every
    // accepted read queued for the next post-Tick pass. Captures from a cohort
    // already detached before that failure are restored before their temporary
    // batch is destroyed.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable firstVtable;
        FakeVTable secondVtable;
        FakeCVar firstObject;
        FakeCVar secondObject;
        Bind(firstObject, firstVtable);
        Bind(secondObject, secondVtable);
        int32_t firstValue = 101;
        int32_t secondValue = 202;
        firstObject.setterTarget = &firstValue;
        secondObject.setterTarget = &secondValue;
        Check(Inject(L"test.Detachment.First", firstObject,
                     reinterpret_cast<uintptr_t>(&firstValue)) &&
                  Inject(L"test.Detachment.Second", secondObject,
                         reinterpret_cast<uintptr_t>(&secondValue)),
              "resolved detachment fakes inject");

        size_t callbacks = 0;
        bool firstDestroyedOutside = false;
        bool secondDestroyedOutside = false;
        {
            auto firstProbe = std::make_shared<MutexDestructionProbe>(
                CVarSystemTestAccess::CacheMutex(Cvars()),
                &firstDestroyedOutside);
            auto secondProbe = std::make_shared<MutexDestructionProbe>(
                CVarSystemTestAccess::CacheMutex(Cvars()),
                &secondDestroyedOutside);
            Check(Cvars().ReadIntOnce(
                      L"test.Detachment.First",
                      [firstProbe, &callbacks](std::expected<int32_t, std::string>) {
                          ++callbacks;
                      }) &&
                      Cvars().ReadIntOnce(
                          L"test.Detachment.Second",
                          [secondProbe, &callbacks](std::expected<int32_t, std::string>) {
                              ++callbacks;
                          }),
                  "resolved read cohorts queue for detachment coverage");
        }

        CVarSystemTestAccess::FailResolvedReadDetachmentAfter(Cvars(), 1);
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(callbacks == 0 && !firstDestroyedOutside && !secondDestroyedOutside,
              "allocation failure preserves every accepted resolved-read cohort");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(callbacks == 2 && firstDestroyedOutside && secondDestroyedOutside,
              "restored resolved-read captures execute and are destroyed outside the cache mutex");
    }

    // Stop retains resolver-owned module state until the resolver is joined.
    // The paused resolver has already borrowed its module pointer when Stop
    // reaches join, so destruction must still be deferred at that boundary.
    {
        CVarSystemTestAccess::Reset(Cvars());
        Cvars().Stop();
        CVarSystemTestAccess::PauseResolverAfterModuleFetch(Cvars(), {});
        ScopedResolverResume resume(Cvars());
        const auto started = Cvars().Start(std::chrono::milliseconds(1));
        Check(started && Cvars().ReadIntOnce(
                             L"test.StopModuleLifetime",
                             [](std::expected<int32_t, std::string>) {}),
              "resolver lifetime test starts a bounded pending read");
        const bool paused = CVarSystemTestAccess::WaitForResolverPause(Cvars());
        Check(paused, "resolver pauses after borrowing its cached module");

        if (paused) {
            std::thread stopping([] { Cvars().Stop(); });
            const bool joining =
                CVarSystemTestAccess::WaitForStopBeforeResolverJoin(Cvars());
            const bool moduleRetained = CVarSystemTestAccess::HasCachedModule(Cvars());
            resume.Resume();
            stopping.join();
            Check(joining && moduleRetained &&
                      !CVarSystemTestAccess::HasCachedModule(Cvars()),
                  "Stop joins the resolver before releasing its borrowed module state");
        } else {
            resume.Resume();
            Cvars().Stop();
        }
    }

    // A follower lifecycle call announces its attempt before waiting on the
    // transaction mutex. It cannot enter until the paused resolver lets the
    // first Stop finish joining.
    {
        CVarSystemTestAccess::Reset(Cvars());
        Cvars().Stop();
        CVarSystemTestAccess::PauseResolverAfterModuleFetch(Cvars(), {});
        ScopedResolverResume resume(Cvars());
        const auto started = Cvars().Start(std::chrono::milliseconds(1));
        Check(started && Cvars().ReadIntOnce(
                             L"test.ConcurrentStop",
                             [](std::expected<int32_t, std::string>) {}),
              "concurrent Stop test starts a bounded pending read");
        const bool paused = CVarSystemTestAccess::WaitForResolverPause(Cvars());
        Check(paused, "concurrent Stop resolver pause is reached");

        if (paused) {
            CVarSystemTestAccess::ResetLifecycleCounters(Cvars());
            std::thread firstStop([] { Cvars().Stop(); });
            const bool firstJoining =
                CVarSystemTestAccess::WaitForStopBeforeResolverJoin(Cvars());
            std::thread followerStop([] { Cvars().Stop(); });
            const bool followerAttempted =
                CVarSystemTestAccess::WaitForLifecycleAttempts(Cvars(), 2);
            const size_t entriesBeforeResume =
                CVarSystemTestAccess::LifecycleEntries(Cvars());
            resume.Resume();
            firstStop.join();
            followerStop.join();
            Check(firstJoining && followerAttempted && entriesBeforeResume == 1 &&
                      Cvars().State() == jst::core::CVarSystemState::Stopped,
                  "concurrent Stop follower waits for the first transaction and both finish stopped");
        } else {
            resume.Resume();
            Cvars().Stop();
        }
    }

    // A failed startup report arriving behind Stop is serialized after Stop's
    // terminal state, so it can publish the supplied unavailable reason.
    {
        CVarSystemTestAccess::Reset(Cvars());
        Cvars().Stop();
        CVarSystemTestAccess::PauseResolverAfterModuleFetch(Cvars(), {});
        ScopedResolverResume resume(Cvars());
        const auto started = Cvars().Start(std::chrono::milliseconds(1));
        Check(started && Cvars().ReadIntOnce(
                             L"test.StopThenUnavailable",
                             [](std::expected<int32_t, std::string>) {}),
              "Stop then unavailable test starts a bounded pending read");
        const bool paused = CVarSystemTestAccess::WaitForResolverPause(Cvars());
        Check(paused, "Stop then unavailable resolver pause is reached");

        if (paused) {
            CVarSystemTestAccess::ResetLifecycleCounters(Cvars());
            std::thread firstStop([] { Cvars().Stop(); });
            const bool firstJoining =
                CVarSystemTestAccess::WaitForStopBeforeResolverJoin(Cvars());
            std::thread unavailable([] {
                Cvars().MarkUnavailable("post-stop startup failure");
            });
            const bool followerAttempted =
                CVarSystemTestAccess::WaitForLifecycleAttempts(Cvars(), 2);
            const size_t entriesBeforeResume =
                CVarSystemTestAccess::LifecycleEntries(Cvars());
            resume.Resume();
            firstStop.join();
            unavailable.join();
            Check(firstJoining && followerAttempted && entriesBeforeResume == 1 &&
                      Cvars().State() == jst::core::CVarSystemState::Unavailable &&
                      Cvars().UnavailableReason() == "post-stop startup failure",
                  "Stop then MarkUnavailable serializes to the exact unavailable reason");
        } else {
            resume.Resume();
            Cvars().Stop();
        }
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
        Check(Inject(L"test.Reentrant", object,
                     reinterpret_cast<uintptr_t>(&value)),
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
        value = 5;
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 5 && object.setterCalls == 2,
              "post-barrier replacement exits the startup reconciliation set");
    }

    // A live admission racing a startup correction cancels the correction
    // target under the cache mutex. Once the correction has entered its setter,
    // the just-admitted command is applied at most once in the same Tick tail.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 0;
        object.setterTarget = &value;
        Check(Inject(L"test.CorrectionLive", object,
                     reinterpret_cast<uintptr_t>(&value)) &&
                  Cvars().SetInt(L"test.CorrectionLive", 11).Accepted(),
              "startup correction race fake is initialized");
        CVarSystemTestAccess::PumpOnce(Cvars());
        value = 2;
        object.callback = &QueueLiveCorrectionReplacement;
        g_reentrantSystem = &Cvars();
        CVarSystemTestAccess::ForceStartupReconciliation(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(value == 33 && object.setterCalls == 3 &&
                  !CVarSystemTestAccess::PendingValue(
                      Cvars(), L"test.CorrectionLive"),
              "correction overlap leaves the live same-name value last in its Tick tail");
    }

    {
        CVarSystemTestAccess::Reset(Cvars());
        Cvars().MarkUnavailable("dispatcher unavailable in test");
        Cvars().MarkUnavailable("ignored later failure");
        Check(Cvars().SetInt(L"test.Rejected", 1).result == CVarSetResult::Rejected,
              "fail-closed writer rejects requests without a dispatcher");
        Check(Cvars().UnavailableReason() == "dispatcher unavailable in test",
              "repeated MarkUnavailable preserves the first reason");
        Cvars().Stop();
        Check(Cvars().SetInt(L"test.Stopped", 1).result == CVarSetResult::Rejected,
              "shutdown rejects all subsequent requests");
        Check(Cvars().State() == jst::core::CVarSystemState::Stopped,
              "MarkUnavailable then Stop finishes stopped");
    }
}
