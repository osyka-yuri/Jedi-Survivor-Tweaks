#pragma once

#include "hook.hpp"
#include "hook_types.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace jst::core {

namespace detail {

// Lightweight step for transactional install/rollback.
// We store a raw pointer (non-owning) instead of std::function to avoid
// allocation + type erasure at install time and to get clean stack traces.
struct HookTransactionStep {
    std::string_view site;
    Hook* hook;   // non-owning; must outlive the step vector
};

[[nodiscard]] std::vector<HookError>
RunHookTransaction(std::span<const HookTransactionStep> steps);

} // namespace detail
} // namespace jst::core
