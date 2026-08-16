#pragma once

#include "memory/SignatureManager.hpp"

namespace yuzora {

// Validates the memory + signature foundation by scanning YuzoraClient.dll
// itself. Runs from Client::initialize when no game module is detected
// (standalone test loading through LoaderTest), so the scanner, the
// signature states and the diagnostics output are exercised end to end
// without touching the game.
//
// Returns true when every check passed.
[[nodiscard]] bool runMemorySelfTest(memory::SignatureManager& signatures);

}  // namespace yuzora
