#pragma once

#include <cmath>

namespace yuzora::sdk {

// Two-component float vector.
struct Vec2 {
    float x = 0.f;
    float y = 0.f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) noexcept : x{x_}, y{y_} {}

    [[nodiscard]] constexpr Vec2 operator+(const Vec2& other) const noexcept {
        return {x + other.x, y + other.y};
    }

    [[nodiscard]] constexpr Vec2 operator-(const Vec2& other) const noexcept {
        return {x - other.x, y - other.y};
    }

    [[nodiscard]] constexpr Vec2 operator*(float scalar) const noexcept {
        return {x * scalar, y * scalar};
    }

    [[nodiscard]] constexpr Vec2 operator/(float scalar) const noexcept {
        return {x / scalar, y / scalar};
    }

    [[nodiscard]] friend constexpr bool operator==(const Vec2&,
                                                   const Vec2&) = default;

    [[nodiscard]] constexpr float dot(const Vec2& other) const noexcept {
        return x * other.x + y * other.y;
    }

    [[nodiscard]] constexpr float lengthSquared() const noexcept {
        return dot(*this);
    }

    [[nodiscard]] float length() const noexcept {
        return std::sqrt(lengthSquared());
    }

    // Unit vector in the same direction; a zero vector stays zero.
    [[nodiscard]] Vec2 normalized() const noexcept {
        const float len = length();
        return len > 0.f ? Vec2{x / len, y / len} : Vec2{};
    }
};

}  // namespace yuzora::sdk
