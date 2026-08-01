#include "core/gateway_allocator.hpp"
#include "core/detour_gate.hpp"
#include "core/hook_engine.hpp"
#include "core/instruction_relocator.hpp"
#include "hooks/hook_context.hpp"
#include "tweaks/hook_tweak.hpp"
#include "test_check.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace jst::core {

class HookEngineTestAccess final {
public:
    [[nodiscard]] static uint32_t InvalidateOriginalProtection(
        HookEngine& engine,
        std::string_view name) {
        auto found = engine.m_hooks.find(name);
        if (found == engine.m_hooks.end()) {
            return 0;
        }
        const uint32_t previous = found->second.m_originalProtection;
        found->second.m_originalProtection = 0;
        return previous;
    }

    static void RestoreOriginalProtection(
        HookEngine& engine,
        std::string_view name,
        uint32_t protection) {
        auto found = engine.m_hooks.find(name);
        if (found != engine.m_hooks.end()) {
            found->second.m_originalProtection = protection;
        }
    }
};

} // namespace jst::core

namespace {

#pragma section(".testcode", read, execute)
__declspec(allocate(".testcode")) alignas(16)
const std::array<std::byte, 16> kAtomicTarget{
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xC3},
};

__declspec(allocate(".testcode")) alignas(16)
const std::array<std::byte, 16> kRelativeControlFlowTarget{
    std::byte{0xE8}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xC3},
};

__declspec(allocate(".testcode")) alignas(16)
const std::array<std::byte, 16> kLifecycleTarget{
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xC3},
};

__declspec(allocate(".testcode")) alignas(16)
const std::array<std::byte, 16> kUnregisterFailureTarget{
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xC3},
};

__declspec(allocate(".testcode")) alignas(16)
const std::array<std::byte, 16> kGuardedArgumentsTarget{
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xC3},
};

uint32_t g_testDetourCalls = 0;
using TestTargetFn = void (*)();
TestTargetFn g_testOriginal = nullptr;
std::mutex g_testDetourMutex;
std::condition_variable g_testDetourCv;
bool g_testDetourShouldBlock = false;
bool g_testDetourEntered = false;
bool g_testDetourRelease = false;
extern "C" __declspec(noinline) void TestDetour() {
    ++g_testDetourCalls;
    {
        std::unique_lock lock(g_testDetourMutex);
        if (g_testDetourShouldBlock) {
            g_testDetourEntered = true;
            g_testDetourCv.notify_all();
            g_testDetourCv.wait(lock, [] { return g_testDetourRelease; });
        }
    }
    if (g_testOriginal) {
        g_testOriginal();
    }
}

uintptr_t g_guardedObject = 0;
float g_guardedFloat = 0.0f;
uint32_t g_guardedSetBy = 0;
extern "C" __declspec(noinline) void __fastcall GuardedArgumentsDetour(
    uintptr_t object,
    float value,
    uint32_t setBy) {
    g_guardedObject = object;
    g_guardedFloat = value;
    g_guardedSetBy = setBy;
}

uintptr_t ModuleRva(const void* address) {
    return reinterpret_cast<uintptr_t>(address) -
           reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
}

std::byte ReadCodeByte(const std::byte* address) {
    return *reinterpret_cast<volatile const std::byte*>(address);
}

template <size_t Size>
bool CodeEquals(const std::array<std::byte, Size>& code,
                const std::array<std::byte, Size>& expected) {
    for (size_t index = 0; index < Size; ++index) {
        if (ReadCodeByte(code.data() + index) != expected[index]) {
            return false;
        }
    }
    return true;
}

void TestInstructionWindows() {
    using jst::core::detail::MeasureInstructionWindow;

    const std::array<std::byte, 8> longInstruction{
        std::byte{0x48}, std::byte{0x8B}, std::byte{0x84}, std::byte{0x24},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
    };
    auto crossed = MeasureInstructionWindow(longInstruction, 5, "long");
    Check(crossed && *crossed == 8,
          "minimum window must expand to the complete crossing instruction");

    std::array<std::byte, 20> nops{};
    nops.fill(std::byte{0x90});
    for (const size_t minimum : {5u, 11u, 14u, 15u, 16u}) {
        auto measured = MeasureInstructionWindow(nops, minimum, "nops");
        Check(measured && *measured == minimum,
              "one-byte instructions must preserve exact requested windows");
    }

    const std::array<std::byte, 1> truncated{std::byte{0x0F}};
    auto invalid = MeasureInstructionWindow(truncated, 1, "invalid");
    Check(!invalid &&
              invalid.error().code == jst::core::HookErrorCode::DecodeFailed,
          "truncated instruction must fail decoding");
}

void TestRelocation() {
    using jst::core::HookErrorCode;
    using jst::core::detail::CalculateRel32;
    using jst::core::detail::RelocateInstructions;

    const std::array<std::byte, 7> ripLoad{
        std::byte{0x48}, std::byte{0x8B}, std::byte{0x05},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
    };
    constexpr uintptr_t oldAddress = 0x0000000010000000;
    constexpr uintptr_t newAddress = 0x0000000011000000;
    const uintptr_t oldAbsolute = oldAddress + ripLoad.size() + 0x12345678;

    auto relocated =
        RelocateInstructions(ripLoad, oldAddress, newAddress, 32, "rip");
    Check(relocated.has_value(), "RIP-relative load must relocate");
    if (relocated) {
        int32_t newDisplacement = 0;
        std::memcpy(&newDisplacement, relocated->data() + 3, sizeof(newDisplacement));
        const uintptr_t newAbsolute =
            newAddress + relocated->size() + newDisplacement;
        Check(newAbsolute == oldAbsolute,
              "RIP-relative relocation must preserve the absolute target");
    }

    const uintptr_t edgeNewEnd =
        oldAbsolute - static_cast<uintptr_t>(std::numeric_limits<int32_t>::max());
    auto edge = RelocateInstructions(
        ripLoad, oldAddress, edgeNewEnd - ripLoad.size(), 32, "edge");
    Check(edge.has_value(), "signed disp32 upper boundary must be accepted");

    auto overflow = RelocateInstructions(
        ripLoad, oldAddress, edgeNewEnd - ripLoad.size() - 1, 32, "overflow");
    Check(!overflow && overflow.error().code == HookErrorCode::Rel32OutOfRange,
          "RIP-relative disp32 overflow must fail");

    const std::array<std::byte, 5> relativeCall{
        std::byte{0xE8}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00},
    };
    auto resumeWindow =
        jst::core::detail::MeasureInstructionWindow(relativeCall, 5, "resume");
    Check(resumeWindow && *resumeWindow == 5,
          "Resume windows may contain relative control flow");

    auto replayed =
        RelocateInstructions(relativeCall, oldAddress, newAddress, 32, "replay");
    Check(!replayed &&
              replayed.error().code == HookErrorCode::UnsupportedRelativeControlFlow,
          "ReplayOriginal must reject relative control flow");

    auto capacity =
        RelocateInstructions(ripLoad, oldAddress, newAddress, 6, "capacity");
    Check(!capacity &&
              capacity.error().code == HookErrorCode::TrampolineCapacityExceeded,
          "relocation must enforce output capacity");

    constexpr uintptr_t rel32Origin = 0x1000;
    auto rel32Max = CalculateRel32(
        rel32Origin,
        rel32Origin + static_cast<uintptr_t>(std::numeric_limits<int32_t>::max()),
        "rel32-max");
    Check(rel32Max && *rel32Max == INT32_MAX,
          "signed rel32 upper boundary must be accepted");
    auto rel32Overflow =
        CalculateRel32(
            rel32Origin,
            rel32Origin +
                static_cast<uintptr_t>(std::numeric_limits<int32_t>::max()) + 1,
            "rel32-overflow");
    Check(!rel32Overflow &&
              rel32Overflow.error().code == HookErrorCode::Rel32OutOfRange,
          "signed rel32 overflow must fail");
}

void TestAtomicPrepareFailure() {
    const auto original = kAtomicTarget;
    jst::core::HookEngine engine;

    auto first = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Atomic.First",
            .group = "Atomic",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::Resume,
        },
        ModuleRva(kAtomicTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));
    auto second = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Atomic.Second",
            .group = "Atomic",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::ReplayOriginal,
        },
        ModuleRva(kRelativeControlFlowTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));

    Check(first && second, "atomic test hooks must register");
    auto errors = engine.InstallAll();
    Check(!errors.empty(), "unsupported second binding must fail preparation");
    Check(errors.front().site == "Atomic.Second",
          "prepare failure must identify the failing hook site");
    Check(!engine.IsHookInstalled("Atomic.First"),
          "first hook must not install when second validation fails");
    Check(!engine.IsGroupInstalled("Atomic"),
          "failed multi-binding group must remain unpublished");
    Check(CodeEquals(kAtomicTarget, original),
          "first binding must remain untouched when second preparation fails");
}

void TestInstallLifecycle() {
    const auto original = kLifecycleTarget;
    const auto failureOriginal = kUnregisterFailureTarget;
    jst::core::HookEngine engine;
    jst::core::DetourGate gate;
    Check(gate.Open().has_value(), "guarded lifecycle gate opens");
    auto registered = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Lifecycle.Site",
            .group = "Lifecycle",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::ReplayOriginal,
            .detourGate = &gate,
        },
        ModuleRva(kLifecycleTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));
    Check(registered.has_value(), "lifecycle hook must register");
    auto failureRegistered = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Lifecycle.UnregisterFailure",
            .group = "LifecycleFailure",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::Resume,
        },
        ModuleRva(kUnregisterFailureTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));
    Check(failureRegistered.has_value(),
          "unregister-failure sentinel hook must register");
    auto argumentsRegistered = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Lifecycle.GuardedArguments",
            .group = "LifecycleArguments",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::Resume,
            .detourGate = &gate,
        },
        ModuleRva(kGuardedArgumentsTarget.data()),
        reinterpret_cast<uintptr_t>(&GuardedArgumentsDetour));
    Check(argumentsRegistered.has_value(),
          "guarded register-argument hook must register");
    const auto continuation = engine.GetContinuationAddress("Lifecycle.Site");
    Check(continuation && *continuation != 0,
          "guarded replay hook publishes its original trampoline");
    g_testOriginal = continuation
        ? reinterpret_cast<TestTargetFn>(*continuation)
        : nullptr;

    auto installErrors = engine.InstallAll();
    Check(installErrors.empty() && engine.IsGroupInstalled("Lifecycle"),
          "hook group must install");
    Check(ReadCodeByte(kLifecycleTarget.data()) == std::byte{0xE9},
          "installed splice must start with a relative jump");

    const auto invokeTarget =
        reinterpret_cast<TestTargetFn>(
            const_cast<std::byte*>(kLifecycleTarget.data()));
    g_testDetourCalls = 0;
    invokeTarget();
    Check(g_testDetourCalls == 1 && gate.ActiveCountForTests() == 0,
          "open guarded gateway calls the detour and releases activity");
    using ArgumentsTargetFn = void (*)(uintptr_t, float, uint32_t);
    const auto invokeArguments = reinterpret_cast<ArgumentsTargetFn>(
        const_cast<std::byte*>(kGuardedArgumentsTarget.data()));
    invokeArguments(0x12345678u, 61.5f, 0x01000040u);
    Check(g_guardedObject == 0x12345678u &&
              g_guardedFloat == 61.5f &&
              g_guardedSetBy == 0x01000040u,
          "guarded gateway preserves integer and floating register arguments");

    {
        std::lock_guard lock(g_testDetourMutex);
        g_testDetourShouldBlock = true;
        g_testDetourEntered = false;
        g_testDetourRelease = false;
    }
    std::thread activeCall(invokeTarget);
    {
        std::unique_lock lock(g_testDetourMutex);
        g_testDetourCv.wait(lock, [] { return g_testDetourEntered; });
    }
    gate.Close();
    std::promise<void> idleReturned;
    auto idleReturnedFuture = idleReturned.get_future();
    std::thread idleWaiter([&] {
        gate.WaitForIdle();
        idleReturned.set_value();
    });
    Check(idleReturnedFuture.wait_for(std::chrono::milliseconds(20)) ==
              std::future_status::timeout,
          "closed gate waits for a call admitted before shutdown");

    // A call arriving after Close bypasses immediately even while an older
    // admitted call is still inside the detour.
    invokeTarget();
    Check(g_testDetourCalls == 2 && gate.ActiveCountForTests() == 1,
          "closed guarded gateway bypasses DLL code through original continuation");
    {
        std::lock_guard lock(g_testDetourMutex);
        g_testDetourRelease = true;
    }
    g_testDetourCv.notify_all();
    activeCall.join();
    idleWaiter.join();
    Check(idleReturnedFuture.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready &&
              gate.ActiveCountForTests() == 0,
          "guarded gateway publishes idle only after admitted DLL code exits");
    {
        std::lock_guard lock(g_testDetourMutex);
        g_testDetourShouldBlock = false;
    }
    Check(gate.Open().has_value(), "idle guarded gate can reopen before reinstall");

    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(kLifecycleTarget.data(), &info, sizeof(info));
    Check((info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0,
          "splice-site execute protection must be restored");

    const auto repeatedInstallErrors = engine.InstallAll();
    Check(
        repeatedInstallErrors.empty(),
        repeatedInstallErrors.empty()
            ? "repeated install must be idempotent"
            : std::format(
                  "repeated install failed: {}",
                  repeatedInstallErrors.front().message));
    engine.UninstallAll();
    Check(CodeEquals(kLifecycleTarget, original),
          "uninstall must restore original bytes");
    engine.UninstallAll();

    Check(engine.InstallAll().empty() && engine.IsGroupInstalled("Lifecycle"),
          "prepared hook must support reinstall after uninstall");
    invokeTarget();
    Check(g_testDetourCalls == 3,
          "reinstalled guarded hook admits calls after reopening");

    auto secondSeal = jst::core::SealGatewayArena();
    Check(secondSeal.has_value(), "second arena seal must be idempotent");

    const uint32_t savedProtection =
        jst::core::HookEngineTestAccess::InvalidateOriginalProtection(
            engine,
            "Lifecycle.UnregisterFailure");
    const auto failedUnregister =
        engine.UnregisterHook("Lifecycle.UnregisterFailure");
    Check(!failedUnregister &&
              engine.IsHookInstalled("Lifecycle.UnregisterFailure") &&
              engine.GetContinuationAddress("Lifecycle.UnregisterFailure"),
          "failed unregister preserves the installed hook and its retry state");
    jst::core::HookEngineTestAccess::RestoreOriginalProtection(
        engine,
        "Lifecycle.UnregisterFailure",
        savedProtection);
    Check(engine.UnregisterHook("Lifecycle.UnregisterFailure").has_value() &&
              CodeEquals(kUnregisterFailureTarget, failureOriginal),
          "unregister retry succeeds after the write precondition is restored");

    gate.Close();
    Check(engine.UnregisterHook("Lifecycle.GuardedArguments").has_value(),
          "guarded argument hook unregisters cleanly");
    const auto unregistered = engine.UnregisterHook("Lifecycle.Site");
    Check(unregistered.has_value(), "guarded hook unregisters without losing errors");
    gate.WaitForIdle();
    Check(CodeEquals(kLifecycleTarget, original),
          "guarded unregister restores original bytes");
    Check(engine.UnregisterHook("Lifecycle.Site").has_value(),
          "guarded unregister is idempotent");
    g_testOriginal = nullptr;
}

class MultiContextTweak final : public jst::tweaks::HookTweak {
public:
    MultiContextTweak()
        : HookTweak(
              "MultiContext",
              "test",
              false,
              std::vector<jst::tweaks::HookBinding>{
                  {
                      "MultiContext.Hud",
                      jst::tweaks::HookTarget::Address(1),
                      1,
                      jst::hooks::Slot::AspectRatioUIHud,
                  },
                  {
                      "MultiContext.Menu",
                      jst::tweaks::HookTarget::Address(1),
                      1,
                      jst::hooks::Slot::AspectRatioUIMenu,
                  },
              },
              jst::tweaks::RuntimeFloatConfig{}) {}

    void SetMultiplier(float value) { ApplyMultiplier(value); }
};

void TestMultiContextMultiplier() {
    auto& hud = jst::hooks::GetContext(jst::hooks::Slot::AspectRatioUIHud);
    auto& menu = jst::hooks::GetContext(jst::hooks::Slot::AspectRatioUIMenu);
    hud.multiplier = 1.0f;
    menu.multiplier = 0.5f;

    MultiContextTweak tweak;
    tweak.SetMultiplier(0.875f);
    Check(hud.multiplier == 0.875f && menu.multiplier == 0.875f,
          "one public multiplier must update both UI context slots");
}

void TestMixedContinuationGroup() {
    jst::core::HookEngine engine;

    auto reg1 = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Mixed.Resume",
            .group = "Mixed",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::Resume,
        },
        ModuleRva(kLifecycleTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));

    auto reg2 = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Mixed.Replay",
            .group = "Mixed",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::ReplayOriginal,
        },
        ModuleRva(kAtomicTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));

    Check(reg1 && reg2, "mixed continuation sites must register successfully");
}

} // namespace

void TestHookEngine() {
    TestInstructionWindows();
    TestRelocation();
    TestMultiContextMultiplier();
    TestMixedContinuationGroup();
    TestAtomicPrepareFailure();
    TestInstallLifecycle();
}
