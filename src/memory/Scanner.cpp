#include "memory/Scanner.hpp"

#include <cstring>
#include <format>

#include "memory/Memory.hpp"

namespace yuzora::memory {

namespace {

constexpr std::optional<std::uint8_t> hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    return std::nullopt;
}

bool matchesAt(const std::uint8_t* where, const std::vector<Pattern::Element>& elements) {
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (elements[i].has_value() && where[i] != *elements[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<Pattern> Pattern::parse(std::string_view text) {
    Pattern pattern;
    bool hasLiteralByte = false;

    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }

        if (c == '?') {
            ++i;
            if (i < text.size() && text[i] == '?') {
                ++i;  // "??"
            }
            pattern.elements_.emplace_back(std::nullopt);
            continue;
        }

        if (i + 1 >= text.size()) {
            return std::nullopt;  // dangling half byte
        }
        const auto high = hexDigit(text[i]);
        const auto low = hexDigit(text[i + 1]);
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;  // not a hex pair
        }
        i += 2;
        pattern.elements_.emplace_back(
            static_cast<std::uint8_t>((*high << 4) | *low));
        hasLiteralByte = true;
    }

    if (pattern.elements_.empty() || !hasLiteralByte) {
        return std::nullopt;  // empty or wildcard-only patterns are useless
    }
    return pattern;
}

std::string Pattern::toString() const {
    std::string text;
    text.reserve(elements_.size() * 3);
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (i > 0) {
            text += ' ';
        }
        if (elements_[i].has_value()) {
            text += std::format("{:02X}", *elements_[i]);
        } else {
            text += "??";
        }
    }
    return text;
}

std::vector<std::uintptr_t> Scanner::scanAll(std::uintptr_t base, std::size_t size,
                                             const Pattern& pattern) {
    std::vector<std::uintptr_t> matches;
    if (pattern.empty() || size < pattern.size()) {
        return matches;
    }

    const std::vector<Pattern::Element>& elements = pattern.elements();
    const Pattern::Element& first = elements.front();

    forEachReadableRegion(base, size, [&](std::uintptr_t regionBase, std::size_t regionSize) {
        if (regionSize < pattern.size()) {
            return;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(regionBase);
        const std::size_t limit = regionSize - pattern.size();  // last valid start offset

        for (std::size_t offset = 0; offset <= limit;) {
            if (first.has_value()) {
                // Fast skip to the next occurrence of the first fixed byte.
                const void* hit =
                    std::memchr(bytes + offset, *first, limit - offset + 1);
                if (hit == nullptr) {
                    break;
                }
                offset = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - bytes);
            }

            if (matchesAt(bytes + offset, elements)) {
                matches.push_back(regionBase + offset);
            }
            ++offset;
        }
    });

    return matches;
}

}  // namespace yuzora::memory
