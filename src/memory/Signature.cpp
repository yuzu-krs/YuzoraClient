#include "memory/Signature.hpp"

#include <format>

namespace yuzora::memory {

Signature::Signature(std::string name, std::string patternText, std::wstring moduleName)
    : name_{std::move(name)},
      moduleName_{std::move(moduleName)},
      patternText_{std::move(patternText)} {
    pattern_ = Pattern::parse(patternText_);
    if (!pattern_.has_value()) {
        state_ = SignatureState::Invalid;
    }
}

void Signature::scan(std::uintptr_t moduleBase, std::size_t moduleSize) {
    address_ = 0;
    matchCount_ = 0;

    if (!pattern_.has_value()) {
        state_ = SignatureState::Invalid;
        return;
    }

    const std::vector<std::uintptr_t> matches =
        Scanner::scanAll(moduleBase, moduleSize, *pattern_);
    matchCount_ = matches.size();

    if (matchCount_ == 0) {
        state_ = SignatureState::Missing;
        return;
    }

    if (matchCount_ > 1) {
        state_ = SignatureState::Ambiguous;
        return;
    }

    state_ = SignatureState::Found;
    address_ = matches.front();
}

void Signature::markModuleUnavailable() {
    // An unparseable pattern stays Invalid: hiding that behind "not found"
    // would swallow the actual problem in the diagnostics output.
    if (state_ != SignatureState::Invalid) {
        state_ = SignatureState::Missing;
    }
    address_ = 0;
    matchCount_ = 0;
}

std::string Signature::describeState() const {
    switch (state_) {
        case SignatureState::Found:
            return std::format("0x{:016X}", address_);
        case SignatureState::Missing:
            return "not found";
        case SignatureState::Ambiguous:
            return std::format("ambiguous ({} matches)", matchCount_);
        case SignatureState::Invalid:
            return "invalid pattern '" + patternText_ + "'";
        case SignatureState::Pending:
            break;
    }
    return "not scanned";
}

}  // namespace yuzora::memory
