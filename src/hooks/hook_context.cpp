#include "hook_context.hpp"

namespace {
constexpr std::size_t kSlotCount = std::to_underlying(jst::hooks::Slot::Count);
constexpr JstContext kDefaultContext{0, 1.0f, 1.0f, 0};
} // namespace

extern "C" {
alignas(16) JstContext g_contexts[kSlotCount] = {
#define JST_HOOK_SLOT(name) kDefaultContext,
#include "slots.def"
#undef JST_HOOK_SLOT
};
} // extern "C"