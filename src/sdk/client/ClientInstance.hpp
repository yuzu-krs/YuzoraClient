#pragma once

#include "sdk/SdkFunctions.hpp"
#include "sdk/actor/LocalPlayer.hpp"
#include "sdk/world/Level.hpp"

namespace yuzora::sdk {

// Root of the game's client object graph: the entry point every consumer
// starts from. Obtained through Sdk::getClientInstance().
class ClientInstance {
public:
    ClientInstance() = default;

    ClientInstance(const SdkFunctions* functions, void* instance) noexcept
        : functions_{functions}, instance_{instance} {}

    ClientInstance(const ClientInstance&) = default;
    ClientInstance& operator=(const ClientInstance&) = default;

    [[nodiscard]] void* raw() const noexcept { return instance_; }
    [[nodiscard]] bool isValid() const noexcept { return instance_ != nullptr; }

    // The local player; null while the function is unresolved, the client
    // instance is null, or the player is not in a world yet.
    [[nodiscard]] LocalPlayer* getLocalPlayer() noexcept {
        if (functions_ == nullptr || functions_->getLocalPlayer == nullptr ||
            instance_ == nullptr) {
            return nullptr;
        }
        void* player = functions_->getLocalPlayer(instance_);
        if (player == nullptr) {
            return nullptr;
        }
        localPlayer_ = LocalPlayer{functions_, player};
        return &localPlayer_;
    }

    // The loaded level; null under the same conditions as above.
    [[nodiscard]] Level* getLevel() noexcept {
        if (functions_ == nullptr || functions_->getLevel == nullptr ||
            instance_ == nullptr) {
            return nullptr;
        }
        void* level = functions_->getLevel(instance_);
        if (level == nullptr) {
            return nullptr;
        }
        level_ = Level{functions_, level};
        return &level_;
    }

private:
    const SdkFunctions* functions_ = nullptr;
    void* instance_ = nullptr;

    // Reusable views re-wrapped on every call. Any later call on this
    // ClientInstance - or a fresh Sdk::getClientInstance() - re-writes
    // these slots, so returned pointers must be consumed immediately.
    LocalPlayer localPlayer_{};
    Level level_{};
};

}  // namespace yuzora::sdk
