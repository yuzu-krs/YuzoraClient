#pragma once

#include <cstdint>
#include <string>

namespace yuzora::version {

// Four-part version of the Minecraft Bedrock executable,
// e.g. 1.21.94.0. Default constructed value is invalid (all zero).
class GameVersion {
public:
    constexpr GameVersion() = default;

    constexpr GameVersion(std::uint16_t major, std::uint16_t minor,
                          std::uint16_t patch, std::uint16_t build) noexcept
        : major_{major}, minor_{minor}, patch_{patch}, build_{build} {}

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return major_ != 0 || minor_ != 0 || patch_ != 0 || build_ != 0;
    }

    [[nodiscard]] constexpr std::uint16_t major() const noexcept { return major_; }
    [[nodiscard]] constexpr std::uint16_t minor() const noexcept { return minor_; }
    [[nodiscard]] constexpr std::uint16_t patch() const noexcept { return patch_; }
    [[nodiscard]] constexpr std::uint16_t build() const noexcept { return build_; }

    // "1.21.94.0" style text form.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] friend constexpr bool operator==(const GameVersion&,
                                                   const GameVersion&) = default;
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(
        const GameVersion&, const GameVersion&) = default;

private:
    std::uint16_t major_ = 0;
    std::uint16_t minor_ = 0;
    std::uint16_t patch_ = 0;
    std::uint16_t build_ = 0;
};

}  // namespace yuzora::version
