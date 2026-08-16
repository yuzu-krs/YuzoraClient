#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "memory/Scanner.hpp"

namespace yuzora::memory {

// Lifecycle of a named signature.
enum class SignatureState {
    Pending,    // registered, not scanned yet
    Found,      // exactly one occurrence resolved
    Missing,    // scanned, no occurrence (or the target module is not loaded)
    Ambiguous,  // multiple occurrences; refusing to guess
    Invalid,    // the pattern text could not be parsed
};

// One named signature: pattern text, target module and its resolution state.
// A signature is scanned exactly against one module's image.
class Signature {
public:
    Signature(std::string name, std::string patternText, std::wstring moduleName);

    // Scans the signature inside [moduleBase, moduleBase + moduleSize) and
    // updates the state (Found / Missing / Ambiguous).
    void scan(std::uintptr_t moduleBase, std::size_t moduleSize);

    // Marks the signature as Missing because its module is not loaded.
    void markModuleUnavailable();

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::wstring& moduleName() const noexcept { return moduleName_; }
    [[nodiscard]] SignatureState state() const noexcept { return state_; }
    [[nodiscard]] std::uintptr_t address() const noexcept { return address_; }
    [[nodiscard]] std::size_t matchCount() const noexcept { return matchCount_; }

    // Human-readable state for diagnostics, e.g. "0x7FF6A1234567",
    // "not found", "ambiguous (3 matches)", "invalid pattern".
    [[nodiscard]] std::string describeState() const;

private:
    std::string name_;
    std::wstring moduleName_;
    std::string patternText_;
    std::optional<Pattern> pattern_;
    SignatureState state_ = SignatureState::Pending;
    std::uintptr_t address_ = 0;
    std::size_t matchCount_ = 0;
};

}  // namespace yuzora::memory
