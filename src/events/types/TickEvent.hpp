#pragma once

#include "events/Event.hpp"

namespace yuzora {

// Dispatched once per Minecraft client tick once the tick hook exists.
// v0.3: a timing marker only - deliberately no payload yet.
struct TickEvent : Event {};

}  // namespace yuzora
