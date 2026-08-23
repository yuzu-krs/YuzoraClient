#pragma once

#include "hooks/HookManager.hpp"
#include "memory/SignatureManager.hpp"
#include "sdk/Sdk.hpp"

namespace yuzora {

// Validates the memory + signature foundation by scanning YuzoraClient.dll
// itself. Runs from Client::initialize when no game module is detected
// (standalone test loading through LoaderTest), so the scanner, the
// signature states and the diagnostics output are exercised end to end
// without touching the game.
//
// Returns true when every check passed.
[[nodiscard]] bool runMemorySelfTest(memory::SignatureManager& signatures);

// Validates the EventBus (subscribe / dispatch / unsubscribe, multiple
// subscribers, per-type separation, KeyEvent payload, unsubscribe during
// dispatch) without any hooking. Runs in the same standalone mode.
//
// Returns true when every check passed.
[[nodiscard]] bool runEventSelfTest();

// Validates the hook foundation with a controlled hook installed on a
// function synthesized inside this DLL's own memory: install -> hooked call
// -> EventBus dispatch -> original via trampoline -> uninstall -> bytes
// restored. No game function is ever touched.
//
// Returns true when every check passed.
[[nodiscard]] bool runHookSelfTest(hooks::HookManager& hookManager);

// Validates the SDK foundation against a tiny fake game synthesized inside
// this DLL's own memory: math types, the resolved-function table, the
// ClientInstance -> LocalPlayer -> Actor chain, position access through a
// hand-written x64 function (verifying the struct-return ABI), null
// safety and shutdown. No game memory is ever touched.
//
// Returns true when every check passed.
[[nodiscard]] bool runSdkSelfTest(sdk::Sdk& sdk);

}  // namespace yuzora
