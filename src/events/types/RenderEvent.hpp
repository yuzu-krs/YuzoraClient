#pragma once

#include "events/Event.hpp"

namespace yuzora {

// Dispatched once per frame / render pass once the render hook exists.
// v0.3: a timing marker only - deliberately carries no drawing API; real
// rendering support arrives in a later milestone.
struct RenderEvent : Event {};

}  // namespace yuzora
