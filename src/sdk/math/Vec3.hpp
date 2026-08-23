#pragma once

#include <cmath>

namespace yuzora::sdk {

// Three-component float vector; the world-space position type of the SDK.
struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) noexcept : x{x_}, y{y_}, z{z_} {}

    [[nodiscard]] constexpr Vec3 operator+(const Vec3& other) const noexcept {
        return {x + other.x, y + other.y, z + other.z};
    }

    [[nodiscard]] constexpr Vec3 operator-(const Vec3& other) const noexcept {
        return {x - other.x, y - other.y, z - other.z};
    }

    [[nodiscard]] constexpr Vec3 operator*(float scalar) const noexcept {
        return {x * scalar, y * scalar, z * scalar};
    }

    [[nodiscard]] constexpr Vec3 operator/(float scalar) const noexcept {
        return {x / scalar, y / scalar, z / scalar};
    }

    [[nodiscard]] friend constexpr bool operator==(const Vec3&,
                                                   const Vec3&) = default;

    [[nodiscard]] constexpr float dot(const Vec3& other) const noexcept {
        return x * other.x + y * other.y + z * other.z;
    }

    [[nodiscard]] constexpr float lengthSquared() const noexcept {
        return dot(*this);
    }

    [[nodiscard]] float length() const noexcept {
        return std::sqrt(lengthSquared());
    }

    // Unit vector in the same direction; a zero vector stays zero.
    [[nodiscard]] Vec3 normalized() const noexcept {
        const float len = length();
        return len > 0.f ? Vec3{x / len, y / len, z / len} : Vec3{};
    }
};

}  // namespace yuzora::sdk
