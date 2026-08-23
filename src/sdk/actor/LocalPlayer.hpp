#pragma once

#include "sdk/actor/Actor.hpp"

namespace yuzora::sdk {

// The player this client controls. v0.4: carries Actor behavior only;
// player-specific access arrives with later milestones.
class LocalPlayer : public Actor {
public:
    using Actor::Actor;
};

}  // namespace yuzora::sdk
