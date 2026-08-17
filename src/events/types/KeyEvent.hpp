#pragma once

#include "events/Event.hpp"

namespace yuzora {

// A key state transition observed on the game's input path.
struct KeyEvent : Event {
    int key = 0;          // Win32 virtual-key code
    bool pressed = false;  // true = pressed, false = released
};

}  // namespace yuzora
