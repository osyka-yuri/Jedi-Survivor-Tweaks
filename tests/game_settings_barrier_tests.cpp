#include "core/cvar_system.hpp"
#include "core/game_settings_barrier.hpp"
#include "core/game_thread_dispatcher.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "game_settings_barrier_test_access.hpp"
#include "game_thread_dispatcher_test_access.hpp"
#include "test_check.hpp"

#include <thread>
#include <utility>

namespace {

using jst::core::CVarCommandState;
using jst::core::CVarSystem;
using jst::core::CVarSystemTestAccess;
using jst::core::CVarWatchDecision;
using jst::core::IntWatchRequest;
using jst::core::GameSettingsBarrier;
using jst::core::GameSettingsBarrierState;
using jst::core::GameSettingsBarrierTestAccess;
using jst::core::GameThreadDispatcher;
using jst::core::GameThreadDispatcherTestAccess;
using cvar_test::Bind;
using cvar_test::FakeCVar;
using cvar_test::FakeVTable;

bool g_barrierReadyInsideOriginal = false;

void __fastcall NoopTick(void*) {}

void ObserveBarrierInsideOriginal(FakeCVar&) {
    g_barrierReadyInsideOriginal = GameSettingsBarrier::Instance().IsReady();
}

} // namespace

void TestGameSettingsBarrier() {
    auto& cvars = CVarSystem::Instance();
    auto& barrier = GameSettingsBarrier::Instance();
    auto& dispatcher = GameThreadDispatcher::Instance();
    CVarSystemTestAccess::Reset(cvars);
    CVarSystemTestAccess::SetGameSettingsReady(cvars, false);
    GameSettingsBarrierTestAccess::Reset(barrier);
    GameThreadDispatcherTestAccess::Reset(dispatcher, &NoopTick);
    GameThreadDispatcherTestAccess::Invoke(dispatcher);

    FakeVTable vtable;
    FakeCVar object;
    Bind(object, vtable);
    int32_t value = 10;
    object.setterTarget = &value;
    object.callback = &ObserveBarrierInsideOriginal;
    Check(CVarSystemTestAccess::InjectResolved(
              cvars,
              L"test.LateSettings",
              reinterpret_cast<uintptr_t>(&object),
              reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
              reinterpret_cast<uintptr_t>(&value)),
          "late-settings fake injection succeeds");

    int watchCalls = 0;
    int32_t watchedValue = 0;
    IntWatchRequest watchRequest;
    watchRequest.name = L"TEST.latesettings";
    watchRequest.onValue = [&](int32_t observed) {
        ++watchCalls;
        watchedValue = observed;
        return CVarWatchDecision::Complete;
    };
    auto watch = cvars.WatchInt(std::move(watchRequest));
    Check(static_cast<bool>(watch),
          "late-settings watch registers before the startup barrier");

    const auto startup = cvars.SetInt(L"test.LateSettings", 20);
    CVarSystemTestAccess::PumpOnce(cvars);
    Check(value == 10 && object.setterCalls == 0 &&
              startup.ticket.Snapshot().state == CVarCommandState::Pending &&
              watchCalls == 0,
          "first post-Tick releases neither startup setters nor watches");

    GameSettingsBarrierTestAccess::BindStringSetter(
        barrier,
        reinterpret_cast<uintptr_t>(&object),
        &cvar_test::FakeStringSetter);

    FakeVTable foreignVtable;
    FakeCVar foreignObject;
    Bind(foreignObject, foreignVtable);
    int32_t foreignValue = 0;
    foreignObject.setterTarget = &foreignValue;
    GameSettingsBarrierTestAccess::InvokeStringSetter(
        reinterpret_cast<uintptr_t>(&foreignObject),
        L"11",
        jst::core::cvar_layout::kSetByConsole);
    Check(foreignValue == 11 &&
              barrier.State() ==
                  GameSettingsBarrierState::WaitingForPostStartupSetter,
          "a setter call for another CVar cannot open the settings barrier");

    std::thread foreignThread([&] {
        GameSettingsBarrierTestAccess::InvokeStringSetter(
            reinterpret_cast<uintptr_t>(&object),
            L"12",
            jst::core::cvar_layout::kSetByScalability);
    });
    foreignThread.join();
    CVarSystemTestAccess::PumpOnce(cvars);
    Check(value == 12 && object.setterCalls == 1 && watchCalls == 0 &&
              barrier.State() ==
                  GameSettingsBarrierState::WaitingForPostStartupSetter,
          "the exact t.MaxFPS object cannot open the barrier off the game thread");

    GameSettingsBarrierTestAccess::InvokeStringSetter(
        reinterpret_cast<uintptr_t>(&object),
        L"13",
        jst::core::cvar_layout::kSetByConsole);
    Check(value == 13 && object.setterCalls == 2 && !barrier.IsReady(),
          "an exact game-thread Console write cannot impersonate late game settings");

    g_barrierReadyInsideOriginal = true;
    GameSettingsBarrierTestAccess::InvokeStringSetter(
        reinterpret_cast<uintptr_t>(&object),
        L"14",
        jst::core::cvar_layout::kSetByScalability | 0x40u);
    Check(value == 14 && object.setterCalls == 3 &&
              !g_barrierReadyInsideOriginal && barrier.IsReady(),
          "the exact game-thread Scalability setter opens only after original returns");
    CVarSystemTestAccess::PumpOnce(cvars);
    Check(value == 20 && object.setterCalls == 4 &&
              startup.ticket.Snapshot().state == CVarCommandState::Applied &&
              watchCalls == 1 && watchedValue == 20 &&
              !CVarSystemTestAccess::HasWatchFor(cvars, L"test.LateSettings"),
          "queued setters and watches release together on the next post-Tick");

    const auto runtime = cvars.SetInt(L"test.LateSettings", 30);
    CVarSystemTestAccess::PumpOnce(cvars);
    Check(value == 30 && object.setterCalls == 5 &&
              runtime.ticket.Snapshot().state == CVarCommandState::Applied,
          "runtime CVar edits remain next-post-Tick commands after startup");

    // Every UE setter overload is a valid observation point, but all share
    // the same exact-object, game-thread and Scalability-source contract.
    CVarSystemTestAccess::SetGameSettingsReady(cvars, false);
    GameSettingsBarrierTestAccess::Reset(barrier);
    float floatValue = 0.0f;
    object.setterTarget = &floatValue;
    object.callback = nullptr;
    GameSettingsBarrierTestAccess::BindFloatSetter(
        barrier,
        reinterpret_cast<uintptr_t>(&object),
        &cvar_test::FakeFloatSetter);
    GameSettingsBarrierTestAccess::InvokeFloatSetter(
        reinterpret_cast<uintptr_t>(&object),
        60.0f,
        jst::core::cvar_layout::kSetByScalability);
    Check(floatValue == 60.0f && barrier.IsReady(),
          "float Scalability setter opens the exact late-settings barrier");

    CVarSystemTestAccess::SetGameSettingsReady(cvars, false);
    GameSettingsBarrierTestAccess::Reset(barrier);
    int32_t intValue = 0;
    object.setterTarget = &intValue;
    GameSettingsBarrierTestAccess::BindIntSetter(
        barrier,
        reinterpret_cast<uintptr_t>(&object),
        &cvar_test::FakeIntSetter);
    GameSettingsBarrierTestAccess::InvokeIntSetter(
        reinterpret_cast<uintptr_t>(&object),
        61,
        jst::core::cvar_layout::kSetByScalability);
    Check(intValue == 61 && barrier.IsReady(),
          "int Scalability setter opens the exact late-settings barrier");

    GameSettingsBarrierTestAccess::Stop(barrier);
    dispatcher.Stop();
    cvars.Stop();
}
