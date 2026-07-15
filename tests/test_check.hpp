#pragma once

// Shared assertion helper for the unit-test binary.

#include <cmath>
#include <iostream>
#include <string_view>

extern int g_failures;

inline void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

inline bool NearlyEqual(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}
