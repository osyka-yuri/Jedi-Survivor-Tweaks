#include "core/gateway_allocator.hpp"
#include "core/hook_engine.hpp"
#include "core/instruction_relocator.hpp"
#include "hooks/hook_context.hpp"
#include "tweaks/hook_tweak.hpp"
#include "test_check.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

int g_failures = 0;

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

extern "C" __declspec(noinline) void TestDetour() {}

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
    jst::core::HookEngine engine;
    auto registered = engine.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = "Lifecycle.Site",
            .group = "Lifecycle",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::Resume,
        },
        ModuleRva(kLifecycleTarget.data()),
        reinterpret_cast<uintptr_t>(&TestDetour));
    Check(registered.has_value(), "lifecycle hook must register");

    auto installErrors = engine.InstallAll();
    Check(installErrors.empty() && engine.IsGroupInstalled("Lifecycle"),
          "hook group must install");
    Check(ReadCodeByte(kLifecycleTarget.data()) == std::byte{0xE9},
          "installed splice must start with a relative jump");

    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(kLifecycleTarget.data(), &info, sizeof(info));
    Check((info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0,
          "splice-site execute protection must be restored");

    Check(engine.InstallAll().empty(), "repeated install must be idempotent");
    engine.UninstallAll();
    Check(CodeEquals(kLifecycleTarget, original),
          "uninstall must restore original bytes");
    engine.UninstallAll();

    Check(engine.InstallAll().empty() && engine.IsGroupInstalled("Lifecycle"),
          "prepared hook must support reinstall after uninstall");

    auto secondSeal = jst::core::SealGatewayArena();
    Check(secondSeal.has_value(), "second arena seal must be idempotent");

    engine.UninstallAll();
    Check(CodeEquals(kLifecycleTarget, original),
          "final uninstall must restore original bytes");
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

void TestSliderUtils();                 // slider_utils_tests.cpp
void TestRuntimeControls();             // runtime_control_tests.cpp
void TestStreamingPoolController();     // streaming_pool_controller_tests.cpp
void TestPoolSizeSetting();             // pool_size_setting_tests.cpp
void TestCVarWatch();                   // cvar_watch_tests.cpp
void TestGraphicsAdapterService();       // graphics_adapter_service_tests.cpp
void TestImportAddressHook();            // import_address_hook_tests.cpp
void TestPeImports();                    // pe_imports_tests.cpp

int main() {
    TestInstructionWindows();
    TestRelocation();
    TestMultiContextMultiplier();
    TestStreamingPoolController();
    TestPoolSizeSetting();
    TestGraphicsAdapterService();
    TestPeImports();
    TestImportAddressHook();
    TestCVarWatch();
    TestSliderUtils();
    TestRuntimeControls();
    TestMixedContinuationGroup();
    TestAtomicPrepareFailure();
    TestInstallLifecycle();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}
