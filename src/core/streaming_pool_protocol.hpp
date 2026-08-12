#pragma once

#include <cstdint>
#include <type_traits>

namespace jst::core {

// Shared C++/MASM protocol for StreamingPoolFix. C++ publishes the selected
// size and the detour reads it atomically. Zero preserves the engine value.
struct StreamingPoolPayload {
    uint64_t forcedBytes = 0;
};

static_assert(sizeof(StreamingPoolPayload) == 8);
static_assert(alignof(StreamingPoolPayload) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<StreamingPoolPayload>);
static_assert(std::is_trivially_copyable_v<StreamingPoolPayload>);

} // namespace jst::core
