#include "hook_context.hpp"

extern "C" {
    alignas(16) JstContext g_contexts[std::to_underlying(jst::hooks::Slot::Count)] = {
        {0, 1.0f, 1.0f},
        {0, 1.0f, 1.0f},
        {0, 1.0f, 1.0f},
        {0, 1.0f, 1.0f}
    };
}
