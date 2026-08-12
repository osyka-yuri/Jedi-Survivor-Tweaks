#include "core/config.hpp"
#include "core/cvar_system.hpp"
#include "core/hook_engine.hpp"
#include "tweaks/custom_cvars.hpp"
#include "tweaks/cvar_runtime_status.hpp"
#include "tweaks/graphical_tweaks.hpp"
#include "tweaks/runtime_control.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "test_check.hpp"

#include <array>
#include <string>
#include <variant>

void TestManagedCVars() {
    using namespace jst::core;
    using namespace jst::tweaks;
    auto& cvars = CVarSystem::Instance();

    {
        CVarSystemTestAccess::Reset(cvars);
        const std::array invalidBatch{
            CVarWriteRequest{.name = L"test.Valid", .value = int32_t{1}},
            CVarWriteRequest{.name = L"", .value = int32_t{2}},
        };
        const auto rejected = cvars.QueueBatch(invalidBatch);
        Check(!rejected.Accepted() &&
                  rejected.rejection == CVarRejectionReason::InvalidRequest &&
                  CVarSystemTestAccess::CacheSize(cvars) == 0,
              "managed batch validation rejects every command atomically");
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        std::wstring absentName = L"jst.test.dynamic.absent.";
        absentName += std::to_wstring(
            reinterpret_cast<uintptr_t>(&cvars));
        const auto absent = cvars.SetInt(absentName, 1);
        CVarSystemTestAccess::ScanOnce(cvars);
        Check(absent.ticket.Snapshot().state == CVarCommandState::Failed &&
                  CVarSystemTestAccess::CacheSize(cvars) == 0 &&
                  CVarSystemTestAccess::InitialScanQueueSize(cvars) == 0,
              "completed scan fails and removes a name absent from the PE "
              "without a periodic resolver tombstone");
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        cvar_test::FakeVTable firstTable;
        cvar_test::FakeVTable secondTable;
        cvar_test::FakeCVar first;
        cvar_test::FakeCVar second;
        cvar_test::Bind(first, firstTable);
        cvar_test::Bind(second, secondTable);
        int32_t firstValue = 0;
        int32_t secondValue = 0;
        first.setterTarget = &firstValue;
        second.setterTarget = &secondValue;
        Check(CVarSystemTestAccess::InjectResolved(
                  cvars,
                  L"test.Batch.First",
                  reinterpret_cast<uintptr_t>(&first),
                  reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
                  reinterpret_cast<uintptr_t>(&firstValue)) &&
                  CVarSystemTestAccess::InjectResolved(
                      cvars,
                      L"test.Batch.Second",
                      reinterpret_cast<uintptr_t>(&second),
                      reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
                      reinterpret_cast<uintptr_t>(&secondValue)),
              "managed batch fakes inject");

        const std::array batch{
            CVarWriteRequest{.name = L"test.Batch.First", .value = int32_t{11}},
            CVarWriteRequest{.name = L"test.Batch.Second", .value = int32_t{22}},
        };
        const auto queued = cvars.QueueBatch(batch);
        Check(queued.Accepted() && queued.commands.size() == 2,
              "valid managed batch queues every command");
        CVarSystemTestAccess::PumpOnce(cvars);
        Check(firstValue == 11 && secondValue == 22 &&
                  queued.commands[0].ticket.Snapshot().state ==
                      CVarCommandState::Applied &&
                  queued.commands[1].ticket.Snapshot().state ==
                      CVarCommandState::Applied,
              "managed batch tickets report each late setter result");

        const std::array lateFailureBatch{
            CVarWriteRequest{.name = L"test.Batch.First", .value = int32_t{33}},
            CVarWriteRequest{.name = L"test.Batch.Second", .value = int32_t{44}},
        };
        const auto lateFailure = cvars.QueueBatch(lateFailureBatch);
        secondTable.slots[cvar_layout::kVtableSetString] = 0;
        CVarSystemTestAccess::PumpOnce(cvars);
        const std::array lateTickets{
            lateFailure.commands[0].ticket,
            lateFailure.commands[1].ticket,
        };
        Check(firstValue == 33 && secondValue == 22 &&
                  lateTickets[0].Snapshot().state ==
                      CVarCommandState::Applied &&
                  lateTickets[1].Snapshot().state ==
                      CVarCommandState::Failed &&
                  CVarTicketsStatus(lateTickets).state ==
                      TweakRuntimeState::Failed,
              "managed batch preserves per-command late failure diagnostics");
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        const auto pendingCustom = cvars.SetInt(
            L"test.ClaimOnly",
            7,
            CVarCommandSource::Custom);
        cvars.ClaimManaged(L"TEST.claimonly");
        const auto claimed = cvars.SetInt(
            L"TEST.claimonly",
            1,
            CVarCommandSource::Custom);
        Check(pendingCustom.ticket.Snapshot().state ==
                  CVarCommandState::Superseded &&
                  pendingCustom.ticket.Snapshot().supersedeReason ==
                      CVarSupersedeReason::ManagedPriority &&
                  !CVarSystemTestAccess::PendingValue(
                      cvars, L"test.ClaimOnly") &&
                  !claimed.Accepted() &&
                  claimed.rejection == CVarRejectionReason::ManagedConflict &&
                  CVarSystemTestAccess::CacheSize(cvars) == 1,
              "a managed claim atomically supersedes an older custom write "
              "and rejects later custom requests regardless of order");
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        const auto custom = cvars.SetInt(
            L"test.Owned",
            1,
            CVarCommandSource::Custom);
        const auto managed = cvars.SetInt(L"TEST.owned", 2);
        const auto conflict = cvars.SetInt(
            L"test.OWNED",
            3,
            CVarCommandSource::Custom);
        const auto unrelated = cvars.SetInt(
            L"test.Unrelated",
            4,
            CVarCommandSource::Custom);
        Check(custom.ticket.Snapshot().state ==
                  CVarCommandState::Superseded &&
                  custom.ticket.Snapshot().supersedeReason ==
                      CVarSupersedeReason::ManagedPriority &&
                  CVarTicketStatus(custom.ticket).state ==
                      TweakRuntimeState::Applied,
              "a stale superseded ticket is a neutral completion");
        Check(managed.Accepted() && !conflict.Accepted() &&
                  conflict.rejection == CVarRejectionReason::ManagedConflict &&
                  unrelated.Accepted(),
              "specialized ownership rejects only the conflicting custom item");
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        Config config;
        config.SetString("CVars", "test.LateClaim", "7");
        HookEngine hooks;
        CustomCVarsTweak tweak;
        const auto activation = tweak.Configure(config);
        Check(activation && activation->enabled &&
                  tweak.Prepare(hooks).has_value(),
              "custom CVar may queue before a specialized ownership claim");
        cvars.ClaimManaged(L"TEST.lateclaim");
        const auto status = tweak.RuntimeStatus();
        Check(status && status->state != TweakRuntimeState::Failed &&
                  !status->message.empty() &&
                  !CVarSystemTestAccess::PendingValue(
                      cvars, L"test.LateClaim"),
              "late specialized claim remains neutral and surfaces the same "
              "custom-conflict warning regardless of registration order");
        tweak.Shutdown();
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        (void)cvars.SetInt(L"test.ManagedFromConfig", 5);
        Config config;
        config.SetString("CVars", "test.ManagedFromConfig", "6");
        config.SetString("CVars", "test.CustomAccepted", "7");
        config.SetString("CVars", "test.Invalid", "not-a-number");
        HookEngine hooks;
        CustomCVarsTweak tweak;
        const auto activation = tweak.Configure(config);
        Check(activation && activation->enabled &&
                  tweak.Prepare(hooks).has_value(),
              "custom CVar package prepares after full configuration parsing");
        Check(CVarSystemTestAccess::PendingValue(
                  cvars, L"test.ManagedFromConfig") == L"5" &&
                  CVarSystemTestAccess::PendingValue(
                      cvars, L"test.CustomAccepted") == L"7",
              "custom package preserves managed value and accepts unrelated items");
        const auto status = tweak.RuntimeStatus();
        Check(status && status->state != TweakRuntimeState::Failed &&
                  !status->message.empty(),
              "custom conflict is surfaced as a warning instead of failure");
        tweak.Shutdown();
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        Config config;
        config.SetBool("GraphicalTweaks", "Enabled", false);
        HookEngine hooks;
        GraphicalTweaks tweak;
        const auto activation = tweak.Configure(config);
        Check(activation && !activation->enabled &&
                  CVarSystemTestAccess::CacheSize(cvars) == 0 &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Inactive,
              "startup-disabled graphical tweak is neutral and inactive");

        auto controls = tweak.GetRuntimeControls();
        const bool toggled = !std::get<CheckboxControl>(controls[0]).current;
        const auto edit = std::get<CheckboxControl>(controls[0]).apply(toggled);
        Check(edit.state == RuntimeEditState::Queued &&
                  CVarSystemTestAccess::CacheSize(cvars) == 1 &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
              "an individual graphical live edit queues only its own CVar");
        tweak.Shutdown();
    }

    cvars.Stop();
}
