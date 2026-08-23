#include "rendering/Renderer.hpp"

#include <d3dcompiler.h>

#include <cstring>

#include "core/Logger.hpp"

namespace yuzora::rendering {

namespace {

// Positions arrive in pixels; the vertex shader maps them to clip space.
constexpr char kVertexShader[] = R"(
cbuffer Screen : register(b0) {
    float4 screenSize;  // xy = width/height; zw reserved (16-byte cbuffer rule)
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

constexpr char kPixelShader[] = R"(
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

// Constant buffers must be a multiple of 16 bytes.
struct ScreenSize {
    float width;
    float height;
    float reserved[2];
};

}  // namespace

bool Renderer::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (device == nullptr || context == nullptr) {
        return false;
    }
    shutdown();

    if (!font_.build(16)) {
        Logger::error("font atlas rasterization failed");
        return false;
    }

    ComPtr<ID3DBlob> vsCode;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT result = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, nullptr,
                                nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode,
                                &errorBlob);
    if (FAILED(result)) {
        Logger::error("vertex shader compile failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                                : "unknown error");
        return false;
    }

    ComPtr<ID3DBlob> psCode;
    result = D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, nullptr, nullptr,
                        nullptr, "main", "ps_5_0", 0, 0, &psCode, &errorBlob);
    if (FAILED(result)) {
        Logger::error("pixel shader compile failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                                : "unknown error");
        return false;
    }

    device_ = device;
    context_ = context;

    if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(),
                                          vsCode->GetBufferSize(), nullptr,
                                          &vertexShader_)) ||
        FAILED(device->CreatePixelShader(psCode->GetBufferPointer(),
                                         psCode->GetBufferSize(), nullptr,
                                         &pixelShader_))) {
        Logger::error("shader creation failed");
        shutdown();
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(layout, 3, vsCode->GetBufferPointer(),
                                         vsCode->GetBufferSize(), &inputLayout_))) {
        Logger::error("input layout creation failed");
        shutdown();
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, &blendState_))) {
        Logger::error("blend state creation failed");
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    // One batch frame: 4096 quads = 4096 * 6 vertices (must match the
    // clamp in flush()).
    buffer.ByteWidth = 4096 * 6 * sizeof(Vertex);
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &vertexBuffer_))) {
        Logger::error("vertex buffer creation failed");
        shutdown();
        return false;
    }

    buffer = {};
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.ByteWidth = sizeof(ScreenSize);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constantBuffer_))) {
        Logger::error("constant buffer creation failed");
        shutdown();
        return false;
    }

    // Atlas + one extra solid-white column appended for filled rects.
    const std::size_t solidX = font_.width();
    const std::size_t atlasWidth =
        font_.width() + static_cast<std::size_t>(font_.glyph(' ').width);
    std::vector<std::uint8_t> atlas(atlasWidth * font_.height() * 4, 0);
    std::memcpy(atlas.data(), font_.rgba(), font_.width() * font_.height() * 4);
    const float cellW = font_.glyph(' ').width;
    const float cellH = font_.glyph(' ').height;
    for (std::size_t y = 0; y < font_.height(); ++y) {
        for (std::size_t x = 0; x < static_cast<std::size_t>(cellW); ++x) {
            const std::size_t index = (y * atlasWidth + solidX + x) * 4;
            atlas[index + 0] = 255;
            atlas[index + 1] = 255;
            atlas[index + 2] = 255;
            atlas[index + 3] = 255;
        }
    }
    solidU0_ = static_cast<float>(solidX) / static_cast<float>(atlasWidth);
    solidV0_ = 0.f;
    solidU1_ = static_cast<float>(solidX + cellW) / static_cast<float>(atlasWidth);
    solidV1_ = cellH / static_cast<float>(font_.height());

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = static_cast<UINT>(atlasWidth);
    texture.Height = static_cast<UINT>(font_.height());
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_IMMUTABLE;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = atlas.data();
    initData.SysMemPitch = static_cast<UINT>(atlasWidth * 4);

    ComPtr<ID3D11Texture2D> atlasTexture;
    if (FAILED(device->CreateTexture2D(&texture, &initData, &atlasTexture)) ||
        FAILED(device->CreateShaderResourceView(atlasTexture.Get(), nullptr,
                                                &atlasView_))) {
        Logger::error("atlas texture creation failed");
        shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &sampler_))) {
        Logger::error("sampler creation failed");
        shutdown();
        return false;
    }

    vertices_.reserve(1024);
    return true;
}

void Renderer::shutdown() {
    sampler_.Reset();
    atlasView_.Reset();
    constantBuffer_.Reset();
    vertexBuffer_.Reset();
    blendState_.Reset();
    inputLayout_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    context_.Reset();
    device_.Reset();
    vertices_.clear();
    frameActive_ = false;
}

void Renderer::beginFrame(ID3D11RenderTargetView* target, float width, float height) {
    target_ = target;
    viewportWidth_ = width;
    viewportHeight_ = height;
    vertices_.clear();
    frameActive_ = true;
}

void Renderer::endFrame() {
    flush();
    target_ = nullptr;
    frameActive_ = false;
}

void Renderer::drawFilledRect(float x, float y, float width, float height,
                              std::uint32_t argb) {
    if (!frameActive_) {
        return;
    }
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.f;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.f;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.f;
    const float b = static_cast<float>(argb & 0xFF) / 255.f;
    pushQuad(x, y, width, height, solidU0_, solidV0_, solidU1_, solidV1_, r, g, b, a);
}

void Renderer::drawText(float x, float y, std::string_view text, std::uint32_t argb) {
    if (!frameActive_) {
        return;
    }
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.f;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.f;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.f;
    const float b = static_cast<float>(argb & 0xFF) / 255.f;

    float cursor = x;
    for (const char c : text) {
        const FontAtlas::Glyph& glyph = font_.glyph(c);
        pushQuad(cursor, y, glyph.width, glyph.height, glyph.u0, glyph.v0,
                 glyph.u1, glyph.v1, r, g, b, a);
        cursor += font_.advance(c);
    }
}

void Renderer::pushQuad(float x, float y, float w, float h, float u0, float v0,
                        float u1, float v1, float r, float g, float b, float a) {
    const Vertex corners[6] = {
        {x, y, u0, v0, r, g, b, a},
        {x + w, y, u1, v0, r, g, b, a},
        {x, y + h, u0, v1, r, g, b, a},
        {x, y + h, u0, v1, r, g, b, a},
        {x + w, y, u1, v0, r, g, b, a},
        {x + w, y + h, u1, v1, r, g, b, a},
    };
    vertices_.insert(vertices_.end(), corners, corners + 6);
}

void Renderer::flush() {
    if (!frameActive_ || vertices_.empty() || context_ == nullptr || target_ == nullptr) {
        vertices_.clear();
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped))) {
        Logger::error("vertex buffer map failed");
        vertices_.clear();
        return;
    }
    // The buffer holds one batch frame (4096 quads = 4096*6 vertices);
    // larger frames are clipped - oversized frames cannot occur with the
    // current overlay.
    const std::size_t count =
        vertices_.size() > 4096 * 6 ? 4096 * 6 : vertices_.size();
    std::memcpy(mapped.pData, vertices_.data(), count * sizeof(Vertex));
    context_->Unmap(vertexBuffer_.Get(), 0);

    {
        D3D11_MAPPED_SUBRESOURCE constants{};
        if (SUCCEEDED(context_->Map(constantBuffer_.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &constants))) {
            ScreenSize size{viewportWidth_, viewportHeight_};
            std::memcpy(constants.pData, &size, sizeof(size));
            context_->Unmap(constantBuffer_.Get(), 0);
        }
    }

    const float blendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    context_->OMSetRenderTargets(1, &target_, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = viewportWidth_;
    viewport.Height = viewportHeight_;
    viewport.MaxDepth = 1.f;
    context_->RSSetViewports(1, &viewport);

    context_->OMSetBlendState(blendState_.Get(), blendFactor, 0xFFFFFFFF);
    context_->IASetInputLayout(inputLayout_.Get());
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, atlasView_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->Draw(static_cast<UINT>(count), 0);

    ++drawCalls_;
    vertices_.clear();
}

}  // namespace yuzora::rendering
