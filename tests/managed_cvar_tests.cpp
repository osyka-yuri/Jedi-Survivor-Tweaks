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

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

jst::tweaks::RuntimeControl* FindCheckboxControl(
    std::vector<jst::tweaks::RuntimeControl>& controls,
    std::string_view label) {
    const auto found = std::find_if(
        controls.begin(), controls.end(),
        [label](const jst::tweaks::RuntimeControl& control) {
            const auto* checkbox =
                std::get_if<jst::tweaks::CheckboxControl>(&control);
            return checkbox && checkbox->label == label;
        });
    return found == controls.end() ? nullptr : &*found;
}

} // namespace

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
        struct StartupToneCase {
            bool vignetteEnabled;
            bool filmGrainEnabled;
            int32_t expectedQuality;
        };
        constexpr std::array cases{
            StartupToneCase{false, false, 1},
            StartupToneCase{false, true, 1},
            StartupToneCase{true, false, 3},
            StartupToneCase{true, true, 5},
        };

        for (const auto& item : cases) {
            CVarSystemTestAccess::Reset(cvars);
            Config config;
            config.SetBool("GraphicalTweaks", "Enabled", true);
            config.SetBool("Sharpening", "Enabled", true);
            config.SetFloat("Sharpening", "Strength", 1.0f);
            config.SetBool("ChromaticAberration", "Enabled", false);
            config.SetBool("Vignette", "Enabled", item.vignetteEnabled);
            config.SetBool("FilmGrain", "Enabled", item.filmGrainEnabled);
            config.SetBool("DepthOfField", "Enabled", false);
            config.SetBool("MotionBlur", "Enabled", true);
            HookEngine hooks;
            GraphicalTweaks tweak;
            const auto activation = tweak.Configure(config);
            Check(activation && activation->enabled &&
                      tweak.Prepare(hooks).has_value() &&
                      CVarSystemTestAccess::CacheSize(cvars) == 5 &&
                      CVarSystemTestAccess::PendingValue(
                          cvars, L"r.Tonemapper.Quality") ==
                          std::to_wstring(item.expectedQuality) &&
                      CVarSystemTestAccess::PendingValue(
                          cvars, L"r.DepthOfFieldQuality") == L"0" &&
                      CVarSystemTestAccess::PendingValue(
                          cvars, L"r.MotionBlurQuality") == L"4" &&
                      !CVarSystemTestAccess::PendingValue(
                          cvars, L"r.EnableFilmGrain"),
                  "graphical startup batch combines vignette and grain into "
                  "the expected tonemapper quality");
            tweak.Shutdown();
        }
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
        auto* grainControl = FindCheckboxControl(controls, "Film Grain");
        Check(grainControl != nullptr,
              "film-grain live control is available while startup is disabled");
        if (grainControl) {
            const auto edit =
                std::get<CheckboxControl>(*grainControl).apply(true);
            Check(edit.state == RuntimeEditState::Queued &&
                      CVarSystemTestAccess::CacheSize(cvars) == 1 &&
                      CVarSystemTestAccess::PendingValue(
                          cvars, L"r.Tonemapper.Quality") == L"5" &&
                      !CVarSystemTestAccess::PendingValue(
                          cvars, L"r.DepthOfFieldQuality") &&
                      !CVarSystemTestAccess::PendingValue(
                          cvars, L"r.EnableFilmGrain") &&
                      tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
                  "an explicit graphical live edit queues the combined "
                  "tonemapper CVar only");
        }
        tweak.Shutdown();
    }

    {
        struct LiveToggleCase {
            std::string_view label;
            std::string_view section;
            std::wstring_view cvar;
            bool initialValue;
            bool editedValue;
            bool initialVignette;
            bool initialFilmGrain;
            int32_t expectedValue;
        };
        constexpr std::array cases{
            LiveToggleCase{
                "Depth of Field", "DepthOfField", L"r.DepthOfFieldQuality",
                false, true, true, false, 2},
            LiveToggleCase{
                "Motion Blur", "MotionBlur", L"r.MotionBlurQuality",
                true, false, true, false, 0},
            LiveToggleCase{
                "Vignette", "Vignette", L"r.Tonemapper.Quality",
                true, false, true, true, 1},
            LiveToggleCase{
                "Film Grain", "FilmGrain", L"r.Tonemapper.Quality",
                false, true, true, false, 5},
            LiveToggleCase{
                "Film Grain", "FilmGrain", L"r.Tonemapper.Quality",
                true, false, true, true, 3},
            LiveToggleCase{
                "Film Grain", "FilmGrain", L"r.Tonemapper.Quality",
                false, true, false, false, 1},
        };

        for (const auto& item : cases) {
            CVarSystemTestAccess::Reset(cvars);
            Config config;
            config.SetBool("GraphicalTweaks", "Enabled", true);
            config.SetBool("Vignette", "Enabled", item.initialVignette);
            config.SetBool("FilmGrain", "Enabled", item.initialFilmGrain);
            config.SetBool("DepthOfField", "Enabled", true);
            config.SetBool("MotionBlur", "Enabled", true);
            config.SetBool(item.section, "Enabled", item.initialValue);
            HookEngine hooks;
            GraphicalTweaks tweak;
            const auto activation = tweak.Configure(config);
            auto controls = tweak.GetRuntimeControls();
            auto* runtimeControl = FindCheckboxControl(controls, item.label);
            Check(activation && activation->enabled && runtimeControl,
                  "graphical checkbox is exposed after configuration");
            if (!runtimeControl) {
                tweak.Shutdown();
                continue;
            }

            auto& checkbox = std::get<CheckboxControl>(*runtimeControl);
            Check(checkbox.current == item.initialValue &&
                      checkbox.persistence.section == item.section &&
                      checkbox.persistence.key == "Enabled",
                  "graphical checkbox loads and names its persistence key");
            const auto edit = checkbox.apply(item.editedValue);
            Check(edit.state == RuntimeEditState::Queued &&
                      CVarSystemTestAccess::CacheSize(cvars) == 1 &&
                      CVarSystemTestAccess::PendingValue(cvars, item.cvar) ==
                          std::to_wstring(item.expectedValue) &&
                      tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
                  "accepted live toggle queues its mapped CVar and ticket");

            checkbox.current = item.editedValue;
            PersistControl(*runtimeControl, config);
            Check(config.GetBool(item.section, "Enabled", item.initialValue) ==
                      item.editedValue,
                  "accepted graphical toggle persists under its own section");

            auto liveControls = tweak.GetRuntimeControls();
            auto* updatedControl =
                FindCheckboxControl(liveControls, item.label);
            Check(updatedControl &&
                      std::get<CheckboxControl>(*updatedControl).current ==
                          item.editedValue,
                  "accepted graphical toggle updates the desired runtime value");

            cvar_test::FakeVTable table;
            cvar_test::FakeCVar fake;
            cvar_test::Bind(fake, table);
            int32_t appliedValue = -1;
            fake.setterTarget = &appliedValue;
            Check(CVarSystemTestAccess::InjectResolved(
                      cvars,
                      item.cvar,
                      reinterpret_cast<uintptr_t>(&fake),
                      reinterpret_cast<uintptr_t>(
                          &cvar_test::FakeStringSetter),
                      reinterpret_cast<uintptr_t>(&appliedValue)),
                  "graphical live CVar fake resolves");
            CVarSystemTestAccess::PumpOnce(cvars);
            const auto status = tweak.RuntimeStatus();
            Check(appliedValue == item.expectedValue && status &&
                      status->state == TweakRuntimeState::Applied,
                  "graphical control ticket completes after the game setter");
            tweak.Shutdown();
        }
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        Config config;
        config.SetBool("GraphicalTweaks", "Enabled", true);
        config.SetBool("Vignette", "Enabled", false);
        config.SetBool("FilmGrain", "Enabled", false);
        HookEngine hooks;
        GraphicalTweaks tweak;
        (void)tweak.Configure(config);
        auto controls = tweak.GetRuntimeControls();
        auto* grainControl = FindCheckboxControl(controls, "Film Grain");
        Check(grainControl != nullptr,
              "film-grain preference exists while vignette is disabled");
        if (grainControl) {
            auto& checkbox = std::get<CheckboxControl>(*grainControl);
            const auto grainEdit = checkbox.apply(true);
            checkbox.current = true;
            PersistControl(*grainControl, config);
            Check(grainEdit.state == RuntimeEditState::Queued &&
                      CVarSystemTestAccess::PendingValue(
                          cvars, L"r.Tonemapper.Quality") == L"1" &&
                      config.GetBool("FilmGrain", "Enabled", false),
                  "grain preference persists while vignette keeps rendering off");

            auto vignetteControls = tweak.GetRuntimeControls();
            auto* vignetteControl =
                FindCheckboxControl(vignetteControls, "Vignette");
            Check(vignetteControl != nullptr,
                  "vignette control remains available for grain preference");
            if (vignetteControl) {
                auto& vignette = std::get<CheckboxControl>(*vignetteControl);
                const auto vignetteEdit = vignette.apply(true);
                vignette.current = true;
                PersistControl(*vignetteControl, config);
                Check(vignetteEdit.state == RuntimeEditState::Queued &&
                          CVarSystemTestAccess::CacheSize(cvars) == 1 &&
                          CVarSystemTestAccess::PendingValue(
                              cvars, L"r.Tonemapper.Quality") == L"5" &&
                          config.GetBool("Vignette", "Enabled", false) &&
                          tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
                      "enabling vignette later activates the saved grain "
                      "preference through the shared ticket");

                cvar_test::FakeVTable table;
                cvar_test::FakeCVar fake;
                cvar_test::Bind(fake, table);
                int32_t appliedValue = -1;
                fake.setterTarget = &appliedValue;
                Check(CVarSystemTestAccess::InjectResolved(
                          cvars,
                          L"r.Tonemapper.Quality",
                          reinterpret_cast<uintptr_t>(&fake),
                          reinterpret_cast<uintptr_t>(
                              &cvar_test::FakeStringSetter),
                          reinterpret_cast<uintptr_t>(&appliedValue)),
                      "shared tonemapper CVar fake resolves");
                CVarSystemTestAccess::PumpOnce(cvars);
                const auto status = tweak.RuntimeStatus();
                Check(appliedValue == 5 && status &&
                          status->state == TweakRuntimeState::Applied,
                      "combined vignette and grain ticket completes once");
            }
        }
        tweak.Shutdown();
    }

    {
        CVarSystemTestAccess::Reset(cvars);
        Config config;
        config.SetBool("GraphicalTweaks", "Enabled", true);
        config.SetBool("Vignette", "Enabled", true);
        config.SetBool("FilmGrain", "Enabled", false);
        HookEngine hooks;
        GraphicalTweaks tweak;
        (void)tweak.Configure(config);
        auto controls = tweak.GetRuntimeControls();
        auto* grainControl = FindCheckboxControl(controls, "Film Grain");
        Check(grainControl != nullptr,
              "film-grain control exists for rejected-edit coverage");
        if (grainControl) {
            CVarSystemTestAccess::SetWritesAvailable(cvars, false);
            const auto rejected =
                std::get<CheckboxControl>(*grainControl).apply(true);
            auto updatedControls = tweak.GetRuntimeControls();
            auto* updatedGrain =
                FindCheckboxControl(updatedControls, "Film Grain");
            Check(rejected.state == RuntimeEditState::Rejected &&
                      !rejected.ShouldPersist() &&
                      CVarSystemTestAccess::CacheSize(cvars) == 0 &&
                      updatedGrain &&
                      !std::get<CheckboxControl>(*updatedGrain).current &&
                      config.GetBool("FilmGrain", "Enabled", true) == false &&
                      tweak.RuntimeStatus()->state == TweakRuntimeState::Inactive,
                  "rejected combined edit preserves desired state, "
                  "persistence, and ticket state");
        }
        tweak.Shutdown();
    }

    cvars.Stop();
}
