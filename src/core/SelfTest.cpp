#include "core/SelfTest.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <string>

#include "core/Logger.hpp"
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

}  // namespace yuzora
