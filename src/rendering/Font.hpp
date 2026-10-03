#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace yuzora::rendering {

// Rasterizes the ASCII range into a white-on-transparent atlas using GDI,
// so no font data is embedded and no external library is needed. One solid
// white cell is appended after the glyph cells: filled rectangles sample
// it, text samples the glyphs. Glyph UVs are normalized against the FINAL
// atlas width (including the solid cell), so the renderers can upload
// pixels_ as-is. The RGB channels are white everywhere and the glyph shape
// lives in the alpha channel.
class FontAtlas {
public:
    struct Glyph {
        float u0 = 0.f, v0 = 0.f;  // atlas coordinates
        float u1 = 0.f, v1 = 0.f;
        float width = 0.f;         // glyph size in pixels
        float height = 0.f;
    };

    // Builds the atlas with the requested cell height. Returns false when
    // GDI rasterization fails.
    [[nodiscard]] bool build(int cellHeight);

    [[nodiscard]] const Glyph& glyph(char c) const noexcept;
    [[nodiscard]] float advance(char c) const noexcept;  // cell-width step

    [[nodiscard]] const std::uint8_t* rgba() const noexcept { return pixels_.data(); }
    [[nodiscard]] std::size_t width() const noexcept { return width_; }
    [[nodiscard]] std::size_t height() const noexcept { return height_; }
    [[nodiscard]] float cellHeight() const noexcept { return cellHeight_; }

    // Atlas UVs of the appended solid white cell (for filled rectangles).
    [[nodiscard]] float solidU0() const noexcept { return solidU0_; }
    [[nodiscard]] float solidV0() const noexcept { return solidV0_; }
    [[nodiscard]] float solidU1() const noexcept { return solidU1_; }
    [[nodiscard]] float solidV1() const noexcept { return solidV1_; }

    static constexpr char kFirstChar = ' ';
    static constexpr char kLastChar = '~';
    static constexpr std::size_t kGlyphCount = kLastChar - kFirstChar + 1;

private:
    std::vector<std::uint8_t> pixels_;  // RGBA, one glyph per grid cell
    Glyph glyphs_[kGlyphCount]{};
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    float cellHeight_ = 0.f;
    float advance_ = 0.f;
    float solidU0_ = 0.f;
    float solidV0_ = 0.f;
    float solidU1_ = 0.f;
    float solidV1_ = 0.f;
};

}  // namespace yuzora::rendering
