#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "rendering/Font.hpp"

namespace yuzora::rendering {

// Minimal immediate-mode D3D11 renderer: batches colored, textured quads
// (glyphs from the font atlas plus solid fills via a reserved atlas cell)
// into one dynamic vertex buffer and draws them with a single pipeline.
// All coordinates are pixels in the current render target.
class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Compiles shaders, builds the font atlas and creates device objects.
    // Returns false with a logged reason on failure.
    [[nodiscard]] bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    // Must be called once per frame before drawing.
    void beginFrame(ID3D11RenderTargetView* target, float width, float height);
    void endFrame();

    void drawFilledRect(float x, float y, float width, float height,
                        std::uint32_t argb);
    void drawText(float x, float y, std::string_view text, std::uint32_t argb);

    [[nodiscard]] float textHeight() const noexcept { return font_.cellHeight(); }
    [[nodiscard]] float textAdvance() const noexcept { return font_.advance(' '); }
    [[nodiscard]] std::uint64_t drawCalls() const noexcept { return drawCalls_; }
    // Device this renderer is bound to; null before initialize().
    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }

private:
    struct Vertex {
        float x, y;    // pixels
        float u, v;    // atlas UV
        float r, g, b, a;
    };

    void pushQuad(float x, float y, float w, float h, float u0, float v0,
                  float u1, float v1, float r, float g, float b, float a);
    void flush();

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    FontAtlas font_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11ShaderResourceView> atlasView_;
    ComPtr<ID3D11SamplerState> sampler_;

    ID3D11RenderTargetView* target_ = nullptr;
    float viewportWidth_ = 0.f;
    float viewportHeight_ = 0.f;

    // Atlas UVs of the appended solid-white cell used for filled rects.
    float solidU0_ = 0.f;
    float solidV0_ = 0.f;
    float solidU1_ = 0.f;
    float solidV1_ = 0.f;

    std::vector<Vertex> vertices_;
    std::uint64_t drawCalls_ = 0;
    bool frameActive_ = false;
};

}  // namespace yuzora::rendering
