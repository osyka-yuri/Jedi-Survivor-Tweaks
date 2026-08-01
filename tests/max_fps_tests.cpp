#include "core/config.hpp"
#include "core/cvar_system.hpp"
#include "core/hook_engine.hpp"
#include "tweaks/max_fps.hpp"
#include "tweaks/runtime_control.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "test_check.hpp"

#include <variant>

namespace {

using jst::core::CVarSystem;
using jst::core::CVarSystemTestAccess;
using jst::tweaks::CheckboxControl;
using jst::tweaks::MaxFPSTweak;
using jst::tweaks::RuntimeEditState;
using jst::tweaks::SliderFloatControl;
using jst::tweaks::TweakRuntimeState;
using cvar_test::Bind;
using cvar_test::FakeCVar;
using cvar_test::FakeVTable;

CVarSystem& Cvars() {
    return CVarSystem::Instance();
}

void InjectMaxFPS(FakeCVar& object, float value) {
    object.inlineFloat = value;
    object.targetIsFloat = true;
    object.setterTarget = &object.inlineFloat;
    Check(CVarSystemTestAccess::InjectResolved(
              Cvars(),
              L"t.MaxFPS",
              reinterpret_cast<uintptr_t>(&object),
              reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter)),
          "MaxFPS fake injection succeeds");
}

void Prepare(
    MaxFPSTweak& tweak,
    jst::core::HookEngine& hooks,
    const jst::core::Config& config,
    bool expectedEnabled) {
    const auto configured = tweak.Configure(config);
    Check(configured && configured->enabled == expectedEnabled,
          "MaxFPS configuration returns activation only");
    Check(tweak.Prepare(hooks).has_value(),
          "MaxFPS runtime infrastructure prepares");
}

} // namespace

void TestMaxFPS() {
    // Opt-out remains neutral: no CVar cache entry and no engine write.
    {
        CVarSystemTestAccess::Reset(Cvars());
        jst::core::Config config;
        config.SetBool("MaxFPS", "Enabled", false);
        config.SetFloat("MaxFPS", "TargetFPS", 120.0f);
        jst::core::HookEngine hooks;
        MaxFPSTweak tweak;
        Prepare(tweak, hooks, config, false);

        Check(CVarSystemTestAccess::CacheSize(Cvars()) == 0,
              "startup-disabled MaxFPS creates no resolver entry");
        const auto status = tweak.RuntimeStatus();
        Check(status && status->state == TweakRuntimeState::Inactive &&
                  status->message.empty(),
              "startup-disabled MaxFPS reports a quiet inactive state");
        tweak.Shutdown();
    }

    // Startup enable uses the ordinary one-shot queue. The core settings
    // barrier, tested separately, decides when startup commands are released.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        object.flags = 0x01000000;
        InjectMaxFPS(object, 60.0f);
        jst::core::Config config;
        config.SetBool("MaxFPS", "Enabled", true);
        config.SetFloat("MaxFPS", "TargetFPS", 90.0f);
        jst::core::HookEngine hooks;
        MaxFPSTweak tweak;
        Prepare(tweak, hooks, config, true);

        Check(object.inlineFloat == 60.0f && object.setterCalls == 0 &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
              "startup MaxFPS remains pending before the command pass");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(object.inlineFloat == 90.0f && object.setterCalls == 1 &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Applied,
              "released startup MaxFPS applies exactly once");

        tweak.Shutdown();
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(object.inlineFloat == 90.0f && object.setterCalls == 1,
              "tweak shutdown performs no MaxFPS write");
    }

    // Pending edits coalesce through the ordinary latest-value-wins queue.
    // Live disable queues zero, Unreal's uncapped t.MaxFPS value.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        InjectMaxFPS(object, 60.0f);
        jst::core::Config config;
        config.SetBool("MaxFPS", "Enabled", false);
        config.SetFloat("MaxFPS", "TargetFPS", 100.0f);
        jst::core::HookEngine hooks;
        MaxFPSTweak tweak;
        Prepare(tweak, hooks, config, false);

        auto controls = tweak.GetRuntimeControls();
        Check(std::get<SliderFloatControl>(controls[1]).apply(155.0f).state ==
                  RuntimeEditState::Applied,
              "disabled slider changes only the desired target");
        Check(std::get<CheckboxControl>(controls[0]).apply(true).state ==
                  RuntimeEditState::Queued,
              "runtime enable queues the retained target");
        controls = tweak.GetRuntimeControls();
        Check(std::get<SliderFloatControl>(controls[1]).apply(165.0f).state ==
                  RuntimeEditState::Queued,
              "pending target update replaces the older target");
        controls = tweak.GetRuntimeControls();
        Check(std::get<CheckboxControl>(controls[0]).apply(false).state ==
                  RuntimeEditState::Queued &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Pending &&
                  tweak.RuntimeStatus()->message.empty(),
              "live disable remains pending while uncapped zero is queued");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(object.inlineFloat == 0.0f && object.setterCalls == 1 &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Inactive,
              "only the latest value reaches Unreal before status becomes inactive");

        controls = tweak.GetRuntimeControls();
        Check(std::get<CheckboxControl>(controls[0]).apply(true).state ==
                  RuntimeEditState::Queued,
              "MaxFPS can be enabled again after uncapping");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(object.inlineFloat == 165.0f && object.setterCalls == 2,
              "re-enable applies the retained target once");
        tweak.Shutdown();
    }

    // A late Unreal rejection remains visible without rolling back the
    // accepted desired state. A successful disable becomes quiet immediately.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        InjectMaxFPS(object, 60.0f);
        jst::core::Config config;
        config.SetBool("MaxFPS", "Enabled", true);
        jst::core::HookEngine hooks;
        MaxFPSTweak tweak;
        Prepare(tweak, hooks, config, true);
        object.flags = 0x01000000;
        object.ignoreSetBy = true;
        CVarSystemTestAccess::PumpOnce(Cvars());

        const auto failed = tweak.RuntimeStatus();
        Check(failed && failed->state == TweakRuntimeState::Failed &&
                  !failed->message.empty(),
              "late MaxFPS setter failure remains visible to the overlay");
        object.ignoreSetBy = false;
        auto controls = tweak.GetRuntimeControls();
        Check(std::get<CheckboxControl>(controls[0]).apply(false).state ==
                  RuntimeEditState::Queued &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
              "disable replaces the old failure with a pending uncap");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(object.inlineFloat == 0.0f && object.setterCalls == 2 &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Inactive,
              "successful uncap leaves MaxFPS inactive without extra text");

        controls = tweak.GetRuntimeControls();
        Check(std::get<CheckboxControl>(controls[0]).apply(true).state ==
                  RuntimeEditState::Queued,
              "MaxFPS can be re-enabled before testing disable failure");
        CVarSystemTestAccess::PumpOnce(Cvars());
        object.flags = 0x01000000;
        object.ignoreSetBy = true;
        controls = tweak.GetRuntimeControls();
        Check(std::get<CheckboxControl>(controls[0]).apply(false).state ==
                  RuntimeEditState::Queued &&
                  tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
              "failed disable starts in pending state");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(tweak.RuntimeStatus()->state == TweakRuntimeState::Failed &&
                  !tweak.RuntimeStatus()->message.empty(),
              "late MaxFPS disable failure remains visible in red");
        tweak.Shutdown();
    }

    Cvars().Stop();
}
