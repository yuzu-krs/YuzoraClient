#include "core/SelfTest.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "core/Logger.hpp"
#include "events/EventBus.hpp"
#include "events/types/KeyEvent.hpp"
#include "events/types/RenderEvent.hpp"
#include "events/types/TickEvent.hpp"
#include "hooks/Hook.hpp"
#include "hooks/HookManager.hpp"
#include "memory/Memory.hpp"
#include "memory/Signature.hpp"

namespace yuzora {

namespace {

// Unique byte marker compiled into this DLL's image; its bytes are the
// scan target for the positive test.
constexpr std::array<std::uint8_t, 16> kPresentMarker{
    0x7A, 0x59, 0x21, 0x8C, 0xD3, 0x46, 0x0B, 0xF1,
    0xE8, 0x17, 0x92, 0x64, 0xAD, 0x3C, 0x50, 0xDE,
};

// Bytes for the negative test, kept ONLY as pattern text: the ASCII literal
// in the image ("C3 6E ..." as characters) never matches the byte sequence
// it describes, so a scan must come up empty.
constexpr std::string_view kAbsentPattern{
    "C3 6E A0 1D 87 2B F9 44 10 D5 68 03 B1 7E 23 9A"};

std::string toPatternText(const std::array<std::uint8_t, 16>& bytes) {
    std::string text;
    text.reserve(bytes.size() * 3);
    for (const std::uint8_t byte : bytes) {
        text += std::format("{:02X} ", byte);
    }
    text.pop_back();  // trailing space
    return text;
}

bool check(bool condition, std::string_view description) {
    if (condition) {
        Logger::info("[OK] {}", description);
    } else {
        Logger::error("self-test check failed: {}", description);
    }
    return condition;
}

}  // namespace

bool runMemorySelfTest(memory::SignatureManager& signatures) {
    Logger::info("Memory self-test (standalone mode)");

    const memory::ModuleInfo self = memory::getSelfModule();
    if (self.base == 0 || self.size == 0) {
        Logger::error("cannot resolve the own module for the self-test");
        return false;
    }
    Logger::info("Self module {} [0x{:X}, 0x{:X})", memory::toUtf8(self.name),
                 self.base, self.base + self.size);

    signatures.clear();
    signatures.setDefaultModule(self.name);

    signatures.add("SelfTest::PresentMarker", toPatternText(kPresentMarker));
    signatures.add("SelfTest::Int3Padding", "CC CC");
    signatures.add("SelfTest::AbsentMarker", std::string{kAbsentPattern});
    signatures.add("SelfTest::BadPattern", "48 8 9G ?? Z");

    signatures.scanAll();

    const memory::Signature* present = signatures.find("SelfTest::PresentMarker");
    const memory::Signature* padding = signatures.find("SelfTest::Int3Padding");
    const memory::Signature* absent = signatures.find("SelfTest::AbsentMarker");
    const memory::Signature* bad = signatures.find("SelfTest::BadPattern");

    const std::uintptr_t expectedMarkerAddress =
        reinterpret_cast<std::uintptr_t>(kPresentMarker.data());

    bool pass = true;
    pass &= check(present != nullptr &&
                      present->state() == memory::SignatureState::Found &&
                      present->address() == expectedMarkerAddress,
                  "unique marker resolves to exactly its own address");
    pass &= check(padding != nullptr &&
                      padding->state() == memory::SignatureState::Ambiguous &&
                      padding->matchCount() >= 2,
                  "int3 padding pattern is reported ambiguous");
    pass &= check(absent != nullptr &&
                      absent->state() == memory::SignatureState::Missing &&
                      absent->matchCount() == 0,
                  "absent marker is reported missing");
    pass &= check(bad != nullptr && bad->state() == memory::SignatureState::Invalid,
                  "malformed pattern is reported invalid");

    Logger::info("Memory self-test {}", pass ? "passed (4/4)" : "FAILED");

    signatures.clear();
    return pass;
}

bool runEventSelfTest() {
    Logger::info("Event self-test");
    events::EventBus::clearAllSubscriptions();

    bool pass = true;

    // 1. A TickEvent subscriber is called on dispatch.
    int tickCalls = 0;
    const events::SubscriptionId tickId = events::EventBus::subscribe<TickEvent>(
        [&](const TickEvent&) { ++tickCalls; });
    events::EventBus::dispatch(TickEvent{});
    pass &= check(tickCalls == 1, "TickEvent subscriber is called on dispatch");

    // 2. Multiple subscribers of the same type are all called.
    int secondTickCalls = 0;
    const events::SubscriptionId secondTickId = events::EventBus::subscribe<TickEvent>(
        [&](const TickEvent&) { ++secondTickCalls; });
    events::EventBus::dispatch(TickEvent{});
    pass &= check(tickCalls == 2 && secondTickCalls == 1,
                  "multiple subscribers are all called");

    // 3. After unsubscribing, a subscriber is no longer called.
    events::EventBus::unsubscribe<TickEvent>(tickId);
    events::EventBus::dispatch(TickEvent{});
    pass &= check(tickCalls == 2 && secondTickCalls == 2,
                  "unsubscribed subscriber is no longer called");

    // 4. RenderEvent is a separate channel: tick dispatch must not reach it.
    int renderCalls = 0;
    const events::SubscriptionId renderId = events::EventBus::subscribe<RenderEvent>(
        [&](const RenderEvent&) { ++renderCalls; });
    events::EventBus::dispatch(TickEvent{});
    events::EventBus::dispatch(RenderEvent{});
    pass &= check(renderCalls == 1 && secondTickCalls == 3,
                  "RenderEvent and TickEvent channels stay separate");

    // 5. KeyEvent payload arrives correctly (pressed and released).
    KeyEvent captured{};
    int keyCalls = 0;
    const events::SubscriptionId keyId = events::EventBus::subscribe<KeyEvent>(
        [&](const KeyEvent& event) {
            ++keyCalls;
            captured = event;
        });
    events::EventBus::dispatch(KeyEvent{.key = 0x42, .pressed = true});
    const bool pressedOk = keyCalls == 1 && captured.key == 0x42 && captured.pressed;
    events::EventBus::dispatch(KeyEvent{.key = 0x1B, .pressed = false});
    pass &= check(pressedOk && keyCalls == 2 && captured.key == 0x1B && !captured.pressed,
                  "KeyEvent payload reaches subscribers intact");

    // 6. ScopedSubscription unsubscribes on destruction.
    {
        int scopedCalls = 0;
        events::ScopedSubscription<KeyEvent> scoped{
            events::EventBus::subscribe<KeyEvent>(
                [&](const KeyEvent&) { ++scopedCalls; })};
        events::EventBus::dispatch(KeyEvent{.key = 0x43, .pressed = true});
        const bool whileAlive = scopedCalls == 1;
        scoped.reset();
        events::EventBus::dispatch(KeyEvent{.key = 0x43, .pressed = true});
        pass &= check(whileAlive && scopedCalls == 1,
                      "ScopedSubscription unsubscribes on destruction");
    }

    // 7. A subscriber may unsubscribe itself during dispatch without
    //    disturbing the remaining subscribers of that dispatch.
    int selfRemovingCalls = 0;
    int survivingCalls = 0;
    events::SubscriptionId selfRemovingId = 0;
    selfRemovingId = events::EventBus::subscribe<KeyEvent>(
        [&](const KeyEvent&) {
            ++selfRemovingCalls;
            events::EventBus::unsubscribe<KeyEvent>(selfRemovingId);
        });
    const events::SubscriptionId survivingId = events::EventBus::subscribe<KeyEvent>(
        [&](const KeyEvent&) { ++survivingCalls; });
    events::EventBus::dispatch(KeyEvent{.key = 0x41, .pressed = true});
    events::EventBus::dispatch(KeyEvent{.key = 0x41, .pressed = true});
    pass &= check(selfRemovingCalls == 1 && survivingCalls == 2,
                  "self-unsubscribe during dispatch is safe");

    events::EventBus::unsubscribe<TickEvent>(secondTickId);
    events::EventBus::unsubscribe<RenderEvent>(renderId);
    events::EventBus::unsubscribe<KeyEvent>(keyId);
    events::EventBus::unsubscribe<KeyEvent>(survivingId);
    events::EventBus::clearAllSubscriptions();

    Logger::info("Event self-test {}", pass ? "passed (7/7)" : "FAILED");
    return pass;
}

namespace {

// Synthesized x64 machine code for "int add(int a, int b)":
//   mov eax, ecx      8B C1        (2 bytes)
//   add eax, edx      03 C2        (2 bytes)
//   nop x 12          90 ...       (12 bytes of padding so the first 16
//                                   bytes are always safe to displace)
//   ret               C3           (1 byte)
// The first 16 bytes contain only whole, relocatable instructions: no
// branches, no RIP-relative operands - exactly what the trampoline needs.
constexpr std::array<std::uint8_t, 17> kAddFunctionBytes{
    0x8B, 0xC1, 0x03, 0xC2,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0xC3,
};

// The hook handler dispatches TickEvent, then calls the original through
// the trampoline. The trampoline only exists after install(), so the
// self-test publishes it here before invoking the hooked function.
void* g_selfTestTrampoline = nullptr;

int selfTestAddHandler(int a, int b) {
    events::EventBus::dispatch(TickEvent{});
    using AddFunction = int (*)(int, int);
    return reinterpret_cast<AddFunction>(g_selfTestTrampoline)(a, b);
}

}  // namespace

bool runHookSelfTest(hooks::HookManager& hookManager) {
    Logger::info("Hook self-test (controlled target inside this DLL only)");

    // 1. Synthesize the original function in its own executable memory.
    void* const original = VirtualAlloc(nullptr, kAddFunctionBytes.size(),
                                        MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (original == nullptr) {
        Logger::error("cannot allocate executable memory for the hook self-test");
        return false;
    }
    std::memcpy(original, kAddFunctionBytes.data(), kAddFunctionBytes.size());
    FlushInstructionCache(GetCurrentProcess(), original, kAddFunctionBytes.size());
    using AddFunction = int (*)(int, int);
    auto* const add = reinterpret_cast<AddFunction>(original);

    // Independent backup for the byte-level restore verification later.
    std::array<std::uint8_t, 16> originalHead{};
    std::memcpy(originalHead.data(), original, originalHead.size());

    bool pass = true;

    // 2. Direct call before any hooking.
    pass &= check(add(3, 4) == 7, "original function works before hooking");

    // 3. Register, install through the manager and read the diagnostics.
    auto hook = std::make_unique<hooks::Hook>(
        "SelfTest::Add", original,
        reinterpret_cast<void*>(&selfTestAddHandler), 16);
    hooks::Hook* const hookPtr = hook.get();
    if (!hookManager.add(std::move(hook))) {
        Logger::error("hook manager rejected the self-test hook");
        hookManager.clear();
        VirtualFree(original, 0, MEM_RELEASE);
        return false;
    }
    pass &= check(hookManager.installAll() && hookPtr->isInstalled(),
                  "hook installs through the manager");

    hookManager.logDiagnostics();
    g_selfTestTrampoline = hookPtr->trampoline();

    // 4. Idempotency: a second installAll neither breaks nor doubles.
    pass &= check(hookManager.installAll() && hookManager.installedCount() == 1 &&
                      add(10, 20) == 30,
                  "second install is idempotent");

    // 5. The full chain: call -> handler -> TickEvent -> original.
    int ticks = 0;
    const events::SubscriptionId tickSubscription = events::EventBus::subscribe<TickEvent>(
        [&](const TickEvent&) { ++ticks; });
    pass &= check(add(20, 22) == 42 && ticks == 1,
                  "hooked call dispatches TickEvent and returns the original result");

    // 6. Every hooked call dispatches again.
    add(1, 1);
    pass &= check(ticks == 2, "every hooked call dispatches");

    // 7. Uninstall restores the original behavior.
    pass &= check(hookManager.uninstallAll() && !hookPtr->isInstalled(),
                  "hook uninstalls");

    // 8. Idempotency: a second uninstall neither breaks nor patches.
    pass &= check(hookManager.uninstallAll(),
                  "second uninstall is idempotent");

    // 9. After unhooking, direct calls behave natively and dispatch nothing.
    const int afterUnhook = add(5, 6);
    pass &= check(afterUnhook == 11 && ticks == 2,
                  "after uninstall the original runs untouched");

    // 10. Byte-level proof that no hook remains.
    pass &= check(std::memcmp(originalHead.data(), original, originalHead.size()) == 0,
                  "original bytes are fully restored");

    events::EventBus::unsubscribe<TickEvent>(tickSubscription);

    hookManager.clear();
    g_selfTestTrampoline = nullptr;
    VirtualFree(original, 0, MEM_RELEASE);

    Logger::info("Hook self-test {}", pass ? "passed (9/9)" : "FAILED");
    return pass;
}

}  // namespace yuzora
