#pragma once

#if !defined(JST_UNIT_TESTS)
#error "cvar_watch_subscription_test_access.hpp is test-only"
#endif

#include "core/cvar_watch.hpp"

#include <memory>

namespace jst::core {

class CVarWatchSubscriptionTestAccess final {
public:
    [[nodiscard]] static CVarWatchSubscription Make(
        std::shared_ptr<CVarWatchControl> control) {
        return CVarWatchSubscription(std::move(control));
    }
};

} // namespace jst::core
