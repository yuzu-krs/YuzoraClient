#include "rendering/Font.hpp"

#include <Windows.h>

#include <cstring>

namespace yuzora::rendering {

namespace {

constexpr std::size_t kColumns = 16;

}  // namespace

bool FontAtlas::build(int cellHeight) {
    if (cellHeight <= 0) {
        return false;
    }

    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }
    const HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (dc == nullptr) {
        return false;
    }

    // Measure one cell first so the atlas fits the widest glyph.
    const HFONT measuringFont = CreateFontW(
        cellHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_MODERN, L"Consolas");
    if (measuringFont == nullptr) {
        DeleteDC(dc);
        return false;
    }
    SelectObject(dc, measuringFont);

    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int maxCharWidth = metrics.tmMaxCharWidth;
    SIZE probe{};
    GetTextExtentPoint32W(dc, L"W", 1, &probe);

    const std::size_t cellWidth =
        static_cast<std::size_t>((probe.cx > maxCharWidth ? probe.cx : maxCharWidth) + 2);
    const std::size_t cellH = static_cast<std::size_t>(metrics.tmHeight);

    const std::size_t rows = (kGlyphCount + kColumns - 1) / kColumns;
    width_ = cellWidth * kColumns;
    height_ = cellH * rows;
    cellHeight_ = static_cast<float>(cellH);
    advance_ = static_cast<float>(cellWidth);
    pixels_.assign(width_ * height_ * 4, 0);

    // A DIB section gives direct pixel access to GDI output.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width_);
    info.bmiHeader.biHeight = -static_cast<LONG>(cellH);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        DeleteObject(measuringFont);
        DeleteDC(dc);
        return false;
    }
    SelectObject(dc, dib);

    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);

    for (std::size_t i = 0; i < kGlyphCount; ++i) {
        const wchar_t character = static_cast<wchar_t>(kFirstChar + i);

        std::memset(bits, 0, width_ * cellH * 4);
        RECT rect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(cellH)};
        DrawTextW(dc, &character, 1, &rect,
                  DT_LEFT | DT_TOP | DT_NOPREFIX | DT_NOCLIP);

        const std::size_t atlasX = (i % kColumns) * cellWidth;
        const std::size_t atlasY = (i / kColumns) * cellH;
        const auto* src = static_cast<const std::uint8_t*>(bits);

        for (std::size_t y = 0; y < cellH; ++y) {
            for (std::size_t x = 0; x < cellWidth; ++x) {
                const std::size_t srcIndex = (y * width_ + x) * 4;
                // White text on black: blue channel doubles as alpha.
                const std::uint8_t alpha = src[srcIndex];
                const std::size_t dstIndex =
                    ((atlasY + y) * width_ + (atlasX + x)) * 4;
                pixels_[dstIndex + 0] = 255;
                pixels_[dstIndex + 1] = 255;
                pixels_[dstIndex + 2] = 255;
                pixels_[dstIndex + 3] = alpha;
            }
        }

        glyphs_[i].u0 = static_cast<float>(atlasX) / static_cast<float>(width_);
        glyphs_[i].v0 = static_cast<float>(atlasY) / static_cast<float>(height_);
        glyphs_[i].u1 = static_cast<float>(atlasX + cellWidth) / static_cast<float>(width_);
        glyphs_[i].v1 = static_cast<float>(atlasY + cellH) / static_cast<float>(height_);
        glyphs_[i].width = static_cast<float>(cellWidth);
        glyphs_[i].height = static_cast<float>(cellH);
    }

    DeleteObject(dib);
    DeleteObject(measuringFont);
    DeleteDC(dc);
    return true;
}

const FontAtlas::Glyph& FontAtlas::glyph(char c) const noexcept {
    if (c < kFirstChar || c > kLastChar) {
        return glyphs_[0];  // space cell for anything unexpected
    }
    return glyphs_[c - kFirstChar];
}

float FontAtlas::advance(char c) const noexcept {
    (void)c;
    return advance_;
}

}  // namespace yuzora::rendering
