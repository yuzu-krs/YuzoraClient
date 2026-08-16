#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yuzora::memory {

// An IDA-style byte pattern such as "48 89 5C 24 ?? 57 48 83 EC ??".
// '?' and '??' are wildcards; bytes are written as two hex digits.
// Parsing is strict: anything that is not whitespace, a wildcard, or a pair
// of hex digits makes parse() fail, and so does a pattern without a single
// literal byte (it would match everywhere). Typos surface as an Invalid
// signature instead of a silently wrong scan.
class Pattern {
public:
    // Parses pattern text. Returns nullopt on syntax errors or empty input.
    [[nodiscard]] static std::optional<Pattern> parse(std::string_view text);

    // One pattern element: a required byte, or a wildcard (nullopt).
    using Element = std::optional<std::uint8_t>;

    [[nodiscard]] const std::vector<Element>& elements() const noexcept { return elements_; }
    [[nodiscard]] bool empty() const noexcept { return elements_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return elements_.size(); }

    // Canonical text form ("48 89 ?? 5C"); wildcards render as "??".
    [[nodiscard]] std::string toString() const;

private:
    std::vector<Element> elements_;
};

// Scans memory ranges for pattern occurrences.
class Scanner {
public:
    // Returns the address of every occurrence of the pattern inside
    // [base, base + size). Unreadable sub-ranges are skipped. The result is
    // empty when the pattern never occurs.
    [[nodiscard]] static std::vector<std::uintptr_t> scanAll(std::uintptr_t base,
                                                             std::size_t size,
                                                             const Pattern& pattern);
};

}  // namespace yuzora::memory
