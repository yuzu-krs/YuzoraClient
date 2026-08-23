#pragma once

#include <type_traits>

#include "sdk/math/Vec3.hpp"

namespace yuzora::sdk {

// getPosition hands Vec3 back through the raw game ABI. On MSVC x64 a
// 12-byte trivially-copyable struct is returned through the hidden RCX
// buffer - the exact convention the hand-written test code and, later,
// the game functions rely on. Guard the invariant at compile time: a Vec3
// that shrinks into the 1/2/4/8-byte register classes or stops being
// trivially copyable would silently change that ABI.
static_assert(sizeof(Vec3) == 12 && std::is_trivially_copyable_v<Vec3>,
              "Vec3 must stay a 12-byte trivially-copyable type");

// The function table the SDK calls into. Every entry uses the raw pointer
// ABI of the game functions it wraps.
//
// In game mode the table is filled by Sdk::resolveFromSignatures from the
// signature layer; tests synthesize their own entries. Null entries are
// legal - every SDK accessor then degrades safely to "unavailable".
struct SdkFunctions {
    void* (*getClientInstance)() = nullptr;                    // () -> ClientInstance*
    void* (*getLocalPlayer)(void* clientInstance) = nullptr;   // -> LocalPlayer*
    void* (*getLevel)(void* clientInstance) = nullptr;         // -> Level*
    Vec3 (*getPosition)(void* actor) = nullptr;                // -> world position
};

}  // namespace yuzora::sdk
