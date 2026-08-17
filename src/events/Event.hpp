#pragma once

namespace yuzora {

// Base type of every YuzoraClient event.
//
// Events are small value types that describe something that happened. The
// base is an empty tag: EventBus enforces `std::derived_from<Event>` at
// compile time, so being an event stays a zero-cost convention instead of a
// runtime hierarchy.
struct Event {
    Event() = default;
    ~Event() = default;
    Event(const Event&) = default;
    Event& operator=(const Event&) = default;
    Event(Event&&) = default;
    Event& operator=(Event&&) = default;
};

}  // namespace yuzora
