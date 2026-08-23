#pragma once

#include "sdk/SdkFunctions.hpp"
#include "sdk/math/Vec3.hpp"

namespace yuzora::sdk {

// Base wrapper for entities that exist in the world (players, mobs, ...).
//
// Wrappers are non-owning views over a raw game pointer. They never touch
// signatures or offsets themselves: every access goes through the resolved
// function table given at construction, which is what keeps Minecraft
// version knowledge out of the consuming code.
class Actor {
public:
    constexpr Actor() = default;

    constexpr Actor(const SdkFunctions* functions, void* instance) noexcept
        : functions_{functions}, instance_{instance} {}

    [[nodiscard]] constexpr void* raw() const noexcept { return instance_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return instance_ != nullptr; }

    // World-space position. Returns a zero vector while the actor, the
    // wrapper or the resolved function are unavailable - callers must treat
    // that as "unknown", never as a real position.
    [[nodiscard]] Vec3 getPosition() const noexcept {
        if (functions_ == nullptr || functions_->getPosition == nullptr ||
            instance_ == nullptr) {
            return Vec3{};
        }
        return functions_->getPosition(instance_);
    }

protected:
    const SdkFunctions* functions_ = nullptr;
    void* instance_ = nullptr;
};

}  // namespace yuzora::sdk
