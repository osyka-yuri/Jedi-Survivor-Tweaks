#pragma once

#include "hook_tweak.hpp"

namespace jst::tweaks {

class StreamingPoolFix final : public HookTweak {
public:
    StreamingPoolFix();

protected:
    void OnMultiplierChanged(float value) override;
};

} // namespace jst::tweaks
