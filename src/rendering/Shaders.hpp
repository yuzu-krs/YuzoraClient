#pragma once

#include <cstdint>

namespace yuzora::rendering {

// One overlay quad corner: pixel position, atlas UV, tinted color.
// Shared by the D3D11 and D3D12 renderers so both produce identical input.
struct OverlayVertex {
    float x, y;  // pixels
    float u, v;  // atlas UV
    float r, g, b, a;
};

namespace shaders {

// Vertex shader shared by the D3D11 and D3D12 pipelines (SM 5.0 bytecode is
// accepted by both runtimes): positions arrive in pixels, the constant
// buffer carries the render target size.
inline constexpr char kVertexShader[] = R"(
cbuffer Screen : register(b0) {
    float2 screenSize;
};
struct VSIn {
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
VSOut main(VSIn input) {
    VSOut output;
    output.pos = float4(
        input.pos.x / screenSize.x * 2.0 - 1.0,
        1.0 - input.pos.y / screenSize.y * 2.0,
        0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

inline constexpr char kPixelShader[] = R"(
Texture2D atlas : register(t0);
SamplerState samplerState : register(s0);
struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
float4 main(PSIn input) : SV_Target {
    float4 texel = atlas.Sample(samplerState, input.uv);
    return texel * input.color;
}
)";

// Constant buffer layout: must stay a multiple of 16 bytes.
struct ScreenSize {
    float width;
    float height;
    float reserved[2];
};

}  // namespace shaders

}  // namespace yuzora::rendering
