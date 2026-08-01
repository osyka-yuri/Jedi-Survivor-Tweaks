#include "core/config.hpp"
#include "core/hook_engine.hpp"
#include "tweaks/tweak_manager.hpp"
#include "test_check.hpp"

#include <string>
#include <vector>

namespace {

enum class FailurePoint {
    None,
    Configure,
    Prepare,
    Resolve,
    Install,
};

struct LifecycleSpec {
    std::string name;
    jst::tweaks::TweakActivation activation{};
    jst::tweaks::TweakActivationMode mode =
        jst::tweaks::TweakActivationMode::LaunchGated;
    FailurePoint failure = FailurePoint::None;
};

class LifecycleTweak final : public jst::tweaks::ITweak {
public:
    LifecycleTweak(LifecycleSpec spec, std::vector<std::string>* trace)
        : m_spec(std::move(spec)), m_trace(trace) {}

    [[nodiscard]] std::string_view Name() const noexcept override {
        return m_spec.name;
    }
    [[nodiscard]] std::string_view Description() const noexcept override {
        return "Lifecycle test tweak";
    }
    [[nodiscard]] jst::tweaks::TweakActivationMode ActivationMode()
        const noexcept override {
        return m_spec.mode;
    }
    [[nodiscard]] std::expected<jst::tweaks::TweakActivation, std::string>
    Configure(const jst::core::Config&) override {
        m_trace->push_back(m_spec.name + ":configure");
        if (m_spec.failure == FailurePoint::Configure) {
            return std::unexpected("configure failure");
        }
        return m_spec.activation;
    }
    [[nodiscard]] std::expected<void, std::string> Prepare(
        jst::core::HookEngine&) override {
        m_trace->push_back(m_spec.name + ":prepare");
        if (m_spec.failure == FailurePoint::Prepare) {
            return std::unexpected("prepare failure");
        }
        return {};
    }
    [[nodiscard]] std::expected<void, std::string> FinalizeResolution(
        jst::core::HookEngine&) override {
        m_trace->push_back(m_spec.name + ":resolve");
        if (m_spec.failure == FailurePoint::Resolve) {
            return std::unexpected("resolve failure");
        }
        return {};
    }
    [[nodiscard]] std::expected<void, std::string> FinalizeInstallation(
        jst::core::HookEngine&) override {
        m_trace->push_back(m_spec.name + ":install");
        if (m_spec.failure == FailurePoint::Install) {
            return std::unexpected("install failure");
        }
        return {};
    }
    void Shutdown() override {
        m_trace->push_back(m_spec.name + ":shutdown");
    }

private:
    LifecycleSpec m_spec;
    std::vector<std::string>* m_trace = nullptr;
};

} // namespace

void TestTweakManager() {
    using namespace jst::tweaks;

    std::vector<std::string> trace;
    jst::core::Config config;
    jst::core::HookEngine hooks;
    TweakManager manager;

    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "Disabled",
        .activation = {.enabled = false},
    }, &trace);
    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "RuntimeDisabled",
        .activation = {.enabled = false},
        .mode = TweakActivationMode::Runtime,
    }, &trace);
    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "ConfigureFailed",
        .activation = {.enabled = true},
        .failure = FailurePoint::Configure,
    }, &trace);
    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "PrepareFailed",
        .activation = {.enabled = true},
        .failure = FailurePoint::Prepare,
    }, &trace);
    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "ResolveFailed",
        .activation = {.enabled = true},
        .failure = FailurePoint::Resolve,
    }, &trace);
    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "InstallFailed",
        .activation = {.enabled = true},
        .failure = FailurePoint::Install,
    }, &trace);
    manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
        .name = "Active",
        .activation = {.enabled = true},
    }, &trace);

    Check(manager.Prepare(hooks, config).has_value(),
          "TweakManager records individual failures without aborting peers");
    Check(hooks.ResolveAll().empty(), "empty test hook set resolves cleanly");
    manager.FinalizeResolution(hooks);
    Check(hooks.InstallAll().empty(), "empty test hook set installs cleanly");
    Check(manager.FinalizeInstallation(hooks) == 2,
          "runtime-disabled infrastructure and enabled tweak become active");

    std::vector<TweakStage> stages;
    std::vector<std::string> errors;
    manager.IterateTweaks([&](ITweak&, const TweakStatus& status) {
        stages.push_back(status.stage);
        errors.push_back(status.error);
    });
    Check(stages == std::vector<TweakStage>{
              TweakStage::Disabled,
              TweakStage::Active,
              TweakStage::Failed,
              TweakStage::Failed,
              TweakStage::Failed,
              TweakStage::Failed,
              TweakStage::Active,
          }, "manager is the sole owner of every lifecycle stage");
    Check(errors[2] == "configure failure" &&
              errors[3] == "prepare failure" &&
              errors[4] == "resolve failure" &&
              errors[5] == "install failure",
          "manager retains the exact failure diagnostic for the overlay");

    size_t constVisits = 0;
    const TweakManager& constManager = manager;
    constManager.IterateTweaks(
        [&](const ITweak&, const TweakStatus&) { ++constVisits; });
    Check(constVisits == manager.GetTweakCount(),
          "const manager traversal never exposes mutable tweak access");

    Check(!manager.RegisterTweak<LifecycleTweak>(LifecycleSpec{
              .name = "TooLate",
              .activation = {.enabled = true},
          }, &trace) && manager.GetTweakCount() == 7,
          "manager rejects registration after lifecycle start");

    manager.Shutdown();
    std::vector<std::string> shutdowns;
    for (const auto& item : trace) {
        if (item.ends_with(":shutdown")) {
            shutdowns.push_back(item);
        }
    }
    Check(shutdowns == std::vector<std::string>{
              "Active:shutdown",
              "InstallFailed:shutdown",
              "ResolveFailed:shutdown",
              "RuntimeDisabled:shutdown",
          }, "every successfully prepared tweak shuts down in reverse order");
}
