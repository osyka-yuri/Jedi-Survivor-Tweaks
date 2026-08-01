#include "core/config.hpp"
#include "core/cvar_system.hpp"
#include "core/hook_engine.hpp"
#include "tweaks/interpolated_rendering.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "test_check.hpp"

#include <variant>

void TestInterpolatedRendering() {
    using jst::core::CVarSystem;
    using jst::core::CVarSystemTestAccess;
    using jst::tweaks::CheckboxControl;
    using jst::tweaks::InterpolatedRenderingTweak;
    using jst::tweaks::RuntimeEditState;
    using jst::tweaks::TweakRuntimeState;

    auto& cvars = CVarSystem::Instance();
    CVarSystemTestAccess::Reset(cvars);

    jst::core::Config config;
    config.SetBool("InterpolatedRendering", "Enabled", false);
    jst::core::HookEngine hooks;
    InterpolatedRenderingTweak tweak;
    const auto configured = tweak.Configure(config);
    Check(configured && !configured->enabled,
          "InterpolatedRendering is opt-in by default");
    Check(tweak.Prepare(hooks).has_value() &&
              CVarSystemTestAccess::CacheSize(cvars) == 0,
          "startup-disabled interpolation creates no resolver entry or write");
    const auto inactive = tweak.RuntimeStatus();
    Check(inactive && inactive->state == TweakRuntimeState::Inactive,
          "startup-disabled interpolation reports inactive runtime state");

    cvar_test::FakeVTable vtable;
    cvar_test::FakeCVar object;
    cvar_test::Bind(object, vtable);
    int32_t value = 0;
    object.setterTarget = &value;
    Check(CVarSystemTestAccess::InjectResolved(
              cvars,
              L"respawn.InterpolatedRendering",
              reinterpret_cast<uintptr_t>(&object),
              reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
              reinterpret_cast<uintptr_t>(&value)),
          "InterpolatedRendering fake injection succeeds");

    auto controls = tweak.GetRuntimeControls();
    const auto enabled = std::get<CheckboxControl>(controls[0]).apply(true);
    Check(enabled.state == RuntimeEditState::Queued && value == 0,
          "explicit runtime enable queues instead of writing synchronously");
    CVarSystemTestAccess::PumpOnce(cvars);
    Check(value == 1 && object.setterCalls == 1,
          "opt-in interpolation applies once on post-Tick");
    const auto applied = tweak.RuntimeStatus();
    Check(applied && applied->state == TweakRuntimeState::Applied,
          "runtime-enabled interpolation overrides launch-disabled dot state");

    controls = tweak.GetRuntimeControls();
    const auto disabled = std::get<CheckboxControl>(controls[0]).apply(false);
    Check(disabled.state == RuntimeEditState::Queued &&
              tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
          "runtime disable stays pending while interpolation update is queued");
    CVarSystemTestAccess::PumpOnce(cvars);
    const auto disabledStatus = tweak.RuntimeStatus();
    Check(value == 0 && disabledStatus &&
              disabledStatus->state == TweakRuntimeState::Inactive,
          "runtime-disabled interpolation returns to inactive state");

    controls = tweak.GetRuntimeControls();
    Check(std::get<CheckboxControl>(controls[0]).apply(true).state ==
              RuntimeEditState::Queued,
          "interpolation can be re-enabled before testing disable failure");
    CVarSystemTestAccess::PumpOnce(cvars);
    object.flags = 0x01000000;
    object.ignoreSetBy = true;
    controls = tweak.GetRuntimeControls();
    Check(std::get<CheckboxControl>(controls[0]).apply(false).state ==
              RuntimeEditState::Queued &&
              tweak.RuntimeStatus()->state == TweakRuntimeState::Pending,
          "interpolation disable failure starts in pending state");
    CVarSystemTestAccess::PumpOnce(cvars);
    Check(tweak.RuntimeStatus()->state == TweakRuntimeState::Failed &&
              !tweak.RuntimeStatus()->message.empty(),
          "late interpolation disable failure remains visible");
    tweak.Shutdown();
    cvars.Stop();
}
