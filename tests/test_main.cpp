#include <iostream>

int g_failures = 0;

void TestHookEngine();
void TestConfig();
void TestCustomCVarValue();
void TestSliderUtils();
void TestRuntimeControls();
void TestStreamingPoolController();
void TestPoolSizeSetting();
void TestCVarResolver();
void TestCVarStartupReconciler();
void TestCVarWriter();
void TestManagedCVars();
void TestCVarRuntimeCoordinator();
void TestGameThreadDispatcher();
void TestMaxFPS();
void TestInterpolatedRendering();
void TestTweakManager();
void TestGraphicsAdapterService();
void TestImportAddressHook();
void TestPeImports();

int main() {
    // Coordinator rollback registers real hooks and therefore must run before
    // the hook-engine suite seals the process-lifetime gateway arena.
    TestCVarRuntimeCoordinator();
    TestHookEngine();
    TestConfig();
    TestCustomCVarValue();
    TestStreamingPoolController();
    TestPoolSizeSetting();
    TestGraphicsAdapterService();
    TestPeImports();
    TestImportAddressHook();
    TestCVarResolver();
    TestCVarStartupReconciler();
    TestCVarWriter();
    TestManagedCVars();
    TestGameThreadDispatcher();
    TestMaxFPS();
    TestInterpolatedRendering();
    TestTweakManager();
    TestSliderUtils();
    TestRuntimeControls();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed\n";
    return 0;
}
