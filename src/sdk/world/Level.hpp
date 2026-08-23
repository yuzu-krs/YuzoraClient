#pragma once

#include "sdk/SdkFunctions.hpp"

namespace yuzora::sdk {

// The loaded world. v0.4: identity only - traversal (actor lists, blocks,
// dimensions) arrives with later milestones.
class Level {
public:
    constexpr Level() = default;

    constexpr Level(const SdkFunctions* functions, void* instance) noexcept
        : functions_{functions}, instance_{instance} {}

    [[nodiscard]] constexpr void* raw() const noexcept { return instance_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return instance_ != nullptr; }

protected:
    const SdkFunctions* functions_ = nullptr;
    void* instance_ = nullptr;
};

}  // namespace yuzora::sdk
