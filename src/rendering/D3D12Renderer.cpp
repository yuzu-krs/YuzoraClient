#include "rendering/D3D12Renderer.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/Logger.hpp"

namespace yuzora::rendering {

namespace {

constexpr std::size_t kMaxVertices = 8192;
constexpr std::size_t kVertexBufferSize = 512 * 1024;

// One-time phase tracing: pinpoints the first crashing call when running on
// an unknown host.
std::atomic<bool> g_phaseLogged[32]{};
void phase(std::size_t index, const char* name) {
    if (index < 32 && !g_phaseLogged[index].exchange(true)) {
        Logger::info("[trace] d3d12 phase: {}", name);
    }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

// Swap chains are frequently created with a TYPELESS format; view and PSO
// formats must be the typed equivalent.
DXGI_FORMAT typedViewFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:
            return format;
    }
}

}  // namespace

std::recursive_mutex& D3D12Renderer::submissionMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

bool D3D12Renderer::ensure(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (device == nullptr || queue == nullptr) {
        return false;
    }
    if (ready_ && device_.Get() == device && queue_ == queue) {
        return true;
    }
    shutdown();

    device_ = device;
    queue_ = queue;  // non-owning
    phase(0, "ensure start");

    // Own direct queue for one-time uploads: keeps the host's queue free of
    // our initialization work while its Present is in progress.
    D3D12_COMMAND_QUEUE_DESC initQueueDesc{};
    initQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&initQueueDesc,
                                          IID_PPV_ARGS(&initQueue_)))) {
        Logger::error("[d3d12] init queue creation failed");
        shutdown();
        return false;
    }

    if (!font_.build(16)) {
        Logger::error("[d3d12] font atlas rasterization failed");
        shutdown();
        return false;
    }
    fontCellHeight_ = font_.cellHeight();
    fontAdvance_ = font_.advance(' ');
    solidU0_ = font_.solidU0();
    solidV0_ = font_.solidV0();
    solidU1_ = font_.solidU1();
    solidV1_ = font_.solidV1();

    for (ComPtr<ID3D12CommandAllocator>& allocator : allocators_) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator)))) {
            Logger::error("[d3d12] command allocator creation failed");
            shutdown();
            return false;
        }
    }
    phase(1, "allocators created");
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocators_[0].Get(), nullptr,
                                         IID_PPV_ARGS(&commandList_))) ||
        FAILED(commandList_->Close())) {
        Logger::error("[d3d12] command list creation failed");
        shutdown();
        return false;
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        Logger::error("[d3d12] fence creation failed");
        shutdown();
        return false;
    }
    phase(2, "list and fence created");

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_)))) {
        Logger::error("[d3d12] RTV heap creation failed");
        shutdown();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap_)))) {
        Logger::error("[d3d12] SRV heap creation failed");
        shutdown();
        return false;
    }
    phase(3, "heaps created");

    // Root signature: root constants (b0) screen size + SRV table (t0) +
    // static sampler.
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 2;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = 2;
    rootSignatureDesc.pParameters = parameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc,
                                           D3D_ROOT_SIGNATURE_VERSION_1_0,
                                           &signature, &errorBlob)) ||
        FAILED(device->CreateRootSignature(
            0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)))) {
        Logger::error("[d3d12] root signature creation failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                                : "unknown error");
        shutdown();
        return false;
    }
    phase(4, "root signature created");

    // Atlas texture (default heap) fed from an upload buffer.
    const std::size_t atlasRowPitch =
        (font_.width() * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const std::size_t atlasUploadSize = atlasRowPitch * font_.height();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC atlasDesc{};
    atlasDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    atlasDesc.Width = font_.width();
    atlasDesc.Height = static_cast<UINT16>(font_.height());
    atlasDesc.DepthOrArraySize = 1;
    atlasDesc.MipLevels = 1;
    atlasDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    atlasDesc.SampleDesc.Count = 1;

    D3D12_RESOURCE_DESC atlasUploadDesc{};
    atlasUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    atlasUploadDesc.Width = atlasUploadSize;
    atlasUploadDesc.Height = 1;
    atlasUploadDesc.DepthOrArraySize = 1;
    atlasUploadDesc.MipLevels = 1;
    atlasUploadDesc.SampleDesc.Count = 1;
    atlasUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &atlasDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&atlas_))) ||
        FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &atlasUploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&atlasUpload_)))) {
        Logger::error("[d3d12] atlas resources creation failed");
        shutdown();
        return false;
    }

    {
        D3D12_RANGE readRange{0, 0};
        void* uploadData = nullptr;
        if (FAILED(atlasUpload_->Map(0, &readRange, &uploadData))) {
            Logger::error("[d3d12] atlas upload map failed");
            shutdown();
            return false;
        }
        const auto* source = font_.rgba();
        auto* destination = static_cast<std::uint8_t*>(uploadData);
        for (std::size_t y = 0; y < font_.height(); ++y) {
            std::memcpy(destination + y * atlasRowPitch, source + y * font_.width() * 4,
                        font_.width() * 4);
        }
        atlasUpload_->Unmap(0, nullptr);
    }

    // Vertex buffer: upload heap with a persistent map.
    D3D12_RESOURCE_DESC vertexDesc{};
    vertexDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexDesc.Width = kVertexBufferSize;
    vertexDesc.Height = 1;
    vertexDesc.DepthOrArraySize = 1;
    vertexDesc.MipLevels = 1;
    vertexDesc.SampleDesc.Count = 1;
    vertexDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&vertexBuffer_)))) {
        Logger::error("[d3d12] vertex buffer creation failed");
        shutdown();
        return false;
    }

    D3D12_RANGE noRead{0, 0};
    if (FAILED(vertexBuffer_->Map(0, &noRead,
                                  reinterpret_cast<void**>(&vertexData_)))) {
        Logger::error("[d3d12] persistent buffer map failed");
        shutdown();
        return false;
    }

    // Upload the atlas once on our own queue and transition it to a shader
    // resource.
    {
        phase(5, "atlas upload begin");
        if (FAILED(commandList_->Reset(allocators_[0].Get(), nullptr))) {
            Logger::error("[d3d12] upload list reset failed");
            shutdown();
            return false;
        }
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = atlasUpload_.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        source.PlacedFootprint.Footprint.Width = static_cast<UINT>(font_.width());
        source.PlacedFootprint.Footprint.Height = static_cast<UINT>(font_.height());
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch =
            static_cast<UINT>(atlasRowPitch);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = atlas_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        commandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        D3D12_RESOURCE_BARRIER toShaderResource = transition(
            atlas_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &toShaderResource);
        commandList_->Close();

        ID3D12CommandList* lists[] = {commandList_.Get()};
        {
            const std::scoped_lock lock{submissionMutex()};
            initQueue_->ExecuteCommandLists(1, lists);
        }
        ++fenceValue_;
        fenceValues_[0] = fenceValue_;
        initQueue_->Signal(fence_.Get(), fenceValue_);

        HANDLE completion = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE);
        if (completion != nullptr) {
            fence_->SetEventOnCompletion(fenceValue_, completion);
            WaitForSingleObject(completion, 5000);
            CloseHandle(completion);
        }
        phase(7, "atlas upload completed");
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
        srvHeap_->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(atlas_.Get(), nullptr, srvHandle);
    phase(8, "srv created");

    ready_ = true;
    return true;
}

void D3D12Renderer::waitIdle() {
    if (fence_ != nullptr && fenceValue_ != 0) {
        HANDLE completion = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE);
        if (completion != nullptr) {
            fence_->SetEventOnCompletion(fenceValue_, completion);
            WaitForSingleObject(completion, 2000);
            CloseHandle(completion);
        }
    }
}

void D3D12Renderer::shutdown() {
    Logger::info("[trace] d3d12 shutdown: enter");
    // Wait for the GPU to finish everything submitted on our fence before
    // releasing any object it may still reference.
    waitIdle();
    Logger::info("[trace] d3d12 shutdown: gpu drained");

    if (vertexBuffer_ != nullptr && vertexData_ != nullptr) {
        vertexBuffer_->Unmap(0, nullptr);
    }
    vertexData_ = nullptr;
    vertexBuffer_.Reset();
    atlasUpload_.Reset();
    atlas_.Reset();
    Logger::info("[trace] d3d12 shutdown: atlas released");
    srvHeap_.Reset();
    rtvHeap_.Reset();
    pipelines_.clear();
    rootSignature_.Reset();
    Logger::info("[trace] d3d12 shutdown: pipeline released");
    fence_.Reset();
    commandList_.Reset();
    for (ComPtr<ID3D12CommandAllocator>& allocator : allocators_) {
        allocator.Reset();
    }
    initQueue_.Reset();
    Logger::info("[trace] d3d12 shutdown: init queue released");
    queue_ = nullptr;  // non-owning: never release the host's queue
    device_.Reset();
    Logger::info("[trace] d3d12 shutdown: objects released");
    vertices_.clear();
    frameActive_ = false;
    ready_ = false;
}

ID3D12PipelineState* D3D12Renderer::buildPipeline(DXGI_FORMAT format,
                                                  UINT sampleCount,
                                                  UINT sampleQuality) {
    const std::uint32_t formatKey =
        static_cast<std::uint32_t>(format) | (sampleCount << 8);
    const auto existing = pipelines_.find(formatKey);
    if (existing != pipelines_.end()) {
        return existing->second.Get();
    }
    phase(9, "build pipeline begin");

    ComPtr<ID3DBlob> vsCode;
    ComPtr<ID3DBlob> psCode;
    ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3DCompile(shaders::kVertexShader,
                          sizeof(shaders::kVertexShader) - 1, nullptr, nullptr,
                          nullptr, "main", "vs_5_0", 0, 0, &vsCode, &errorBlob))) {
        Logger::error("[d3d12] vertex shader compile failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                                : "unknown error");
        return nullptr;
    }
    if (FAILED(D3DCompile(shaders::kPixelShader,
                          sizeof(shaders::kPixelShader) - 1, nullptr, nullptr,
                          nullptr, "main", "ps_5_0", 0, 0, &psCode, &errorBlob))) {
        Logger::error("[d3d12] pixel shader compile failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                                : "unknown error");
        return nullptr;
    }

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {layout, 3};
    pso.pRootSignature = rootSignature_.Get();
    pso.VS = {vsCode->GetBufferPointer(), vsCode->GetBufferSize()};
    pso.PS = {psCode->GetBufferPointer(), psCode->GetBufferSize()};
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    // Zero-initialized D3D12_RASTERIZER_DESC means WIREFRAME with depth clip
    // off - every field must be set explicitly.
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthBias = 0;
    pso.RasterizerState.DepthBiasClamp = 0.f;
    pso.RasterizerState.SlopeScaledDepthBias = 0.f;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = FALSE;
    pso.RasterizerState.AntialiasedLineEnable = FALSE;
    pso.RasterizerState.ForcedSampleCount = 0;
    pso.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = sampleCount;
    pso.SampleDesc.Quality = sampleQuality;

    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)))) {
        Logger::error("[d3d12] pipeline state creation failed (format {})",
                      static_cast<unsigned>(format));
        return nullptr;
    }
    pipelines_.emplace(formatKey, pipeline);
    phase(10, "pipeline created");
    return pipeline.Get();
}

bool D3D12Renderer::beginFrame(IDXGISwapChain* swapChain) {
    if (!ready_ || frameActive_) {
        return false;
    }

    // Pick an allocator the GPU is done with; skip the frame otherwise.
    std::size_t freeSlot = kAllocatorSlots;
    const std::uint64_t completed = fence_->GetCompletedValue();
    for (std::size_t attempt = 1; attempt <= kAllocatorSlots; ++attempt) {
        const std::size_t slot = (allocatorSlot_ + attempt) % kAllocatorSlots;
        if (fenceValues_[slot] == 0 || fenceValues_[slot] <= completed) {
            freeSlot = slot;
            break;
        }
    }
    if (freeSlot == kAllocatorSlots) {
        return false;
    }
    allocatorSlot_ = freeSlot;

    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        backBuffer == nullptr) {
        return false;
    }
    // MSVC's d3d12.h returns D3D12_RESOURCE_DESC by value.
    const D3D12_RESOURCE_DESC bufferDesc = backBuffer->GetDesc();
    phase(14, "beginFrame: got buffer");
    const DXGI_FORMAT viewFormat = typedViewFormat(bufferDesc.Format);
    if (bufferDesc.Width == 0 || bufferDesc.Height == 0 ||
        viewFormat == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    ID3D12PipelineState* const pipeline =
        buildPipeline(viewFormat, bufferDesc.SampleDesc.Count,
                      bufferDesc.SampleDesc.Quality);
    if (pipeline == nullptr) {
        return false;
    }
    frameBackBuffer_ = backBuffer;

    {
        static std::atomic<bool> bufferLogged{false};
        if (!bufferLogged.exchange(true)) {
            Logger::info("[trace] host back buffer: {}x{} format={} samples={} "
                         "flags=0x{:08X}",
                         static_cast<unsigned>(bufferDesc.Width),
                         static_cast<unsigned>(bufferDesc.Height),
                         static_cast<unsigned>(bufferDesc.Format),
                         static_cast<unsigned>(bufferDesc.SampleDesc.Count),
                         static_cast<unsigned>(bufferDesc.Flags));
        }
    }

    if (FAILED(allocators_[allocatorSlot_]->Reset()) ||
        FAILED(commandList_->Reset(allocators_[allocatorSlot_].Get(), nullptr))) {
        return false;
    }
    phase(15, "beginFrame: list reset");

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = viewFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, rtvHandle);
    phase(16, "beginFrame: rtv created");

    D3D12_RESOURCE_BARRIER toRenderTarget =
        transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &toRenderTarget);
    phase(18, "beginFrame: barrier recorded");

    viewport_.TopLeftX = 0.f;
    viewport_.TopLeftY = 0.f;
    viewport_.Width = static_cast<float>(bufferDesc.Width);
    viewport_.Height = static_cast<float>(bufferDesc.Height);
    viewport_.MinDepth = 0.f;
    viewport_.MaxDepth = 1.f;
    scissor_.left = 0;
    scissor_.top = 0;
    scissor_.right = static_cast<LONG>(bufferDesc.Width);
    scissor_.bottom = static_cast<LONG>(bufferDesc.Height);

    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    commandList_->SetPipelineState(pipeline);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // REQUIRED before any descriptor table bind: without a bound heap the
    // runtime crashes validating SetGraphicsRootDescriptorTable.
    commandList_->SetDescriptorHeaps(1, srvHeap_.GetAddressOf());
    phase(19, "beginFrame: pipeline bound");
    const float screenSize[2] = {viewport_.Width, viewport_.Height};
    commandList_->SetGraphicsRoot32BitConstants(0, 2, screenSize, 0);
    phase(21, "beginFrame: constants set");
    commandList_->SetGraphicsRootDescriptorTable(
        1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
    phase(20, "beginFrame: descriptors bound");
    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissor_);
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    phase(17, "beginFrame: state recorded");

    frameActive_ = true;
    vertices_.clear();
    phase(11, "beginFrame done");
    return true;
}

void D3D12Renderer::drawFilledRect(float x, float y, float width, float height,
                                   std::uint32_t argb) {
    if (!frameActive_) {
        return;
    }
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.f;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.f;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.f;
    const float b = static_cast<float>(argb & 0xFF) / 255.f;
    const OverlayVertex corners[6] = {
        {x, y, solidU0_, solidV0_, r, g, b, a},
        {x + width, y, solidU1_, solidV0_, r, g, b, a},
        {x, y + height, solidU0_, solidV1_, r, g, b, a},
        {x, y + height, solidU0_, solidV1_, r, g, b, a},
        {x + width, y, solidU1_, solidV0_, r, g, b, a},
        {x + width, y + height, solidU1_, solidV1_, r, g, b, a},
    };
    vertices_.insert(vertices_.end(), corners, corners + 6);
}

void D3D12Renderer::drawText(float x, float y, std::string_view text,
                             std::uint32_t argb) {
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
        const OverlayVertex corners[6] = {
            {cursor, y, glyph.u0, glyph.v0, r, g, b, a},
            {cursor + glyph.width, y, glyph.u1, glyph.v0, r, g, b, a},
            {cursor, y + glyph.height, glyph.u0, glyph.v1, r, g, b, a},
            {cursor, y + glyph.height, glyph.u0, glyph.v1, r, g, b, a},
            {cursor + glyph.width, y, glyph.u1, glyph.v0, r, g, b, a},
            {cursor + glyph.width, y + glyph.height, glyph.u1, glyph.v1, r, g, b, a},
        };
        vertices_.insert(vertices_.end(), corners, corners + 6);
        cursor += font_.advance(c);
    }
}

void D3D12Renderer::endFrame() {
    if (!frameActive_) {
        return;
    }
    frameActive_ = false;

    std::size_t count = vertices_.size();
    if (count > kMaxVertices) {
        count = kMaxVertices;
    }
    if (count > 0) {
        std::memcpy(vertexData_, vertices_.data(), count * sizeof(OverlayVertex));
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        vertexView.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
        vertexView.SizeInBytes = static_cast<UINT>(count * sizeof(OverlayVertex));
        vertexView.StrideInBytes = sizeof(OverlayVertex);
        commandList_->IASetVertexBuffers(0, 1, &vertexView);
        commandList_->DrawInstanced(static_cast<UINT>(count), 1, 0, 0);
    }

    if (frameBackBuffer_ != nullptr) {
        D3D12_RESOURCE_BARRIER toPresent = transition(
            frameBackBuffer_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        commandList_->ResourceBarrier(1, &toPresent);
    }
    commandList_->Close();

    ID3D12CommandList* lists[] = {commandList_.Get()};
    phase(12, "overlay execute");
    {
        const std::scoped_lock lock{submissionMutex()};
        queue_->ExecuteCommandLists(1, lists);
        ++fenceValue_;
        fenceValues_[allocatorSlot_] = fenceValue_;
        queue_->Signal(fence_.Get(), fenceValue_);
    }
    frameBackBuffer_.Reset();

    {
        static std::atomic<bool> removalLogged{false};
        if (!removalLogged.exchange(true)) {
            const HRESULT removed = device_->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                Logger::error("[d3d12] device removed after overlay submit "
                              "(reason 0x{:08X})",
                              static_cast<unsigned>(removed));
            } else {
                phase(22, "device alive after first overlay execute");
            }
        }
    }

    vertices_.clear();
    phase(13, "endFrame done");
}

int D3D12Renderer::probeCommandStream(IDXGISwapChain* swapChain) {
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        backBuffer == nullptr) {
        return -1;
    }
    const D3D12_RESOURCE_DESC bufferDesc = backBuffer->GetDesc();
    const DXGI_FORMAT viewFormat = typedViewFormat(bufferDesc.Format);
    ID3D12PipelineState* const pipeline =
        buildPipeline(viewFormat, bufferDesc.SampleDesc.Count,
                      bufferDesc.SampleDesc.Quality);
    if (pipeline == nullptr) {
        return -1;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> probeAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> probeList;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&probeAllocator))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          probeAllocator.Get(), nullptr,
                                          IID_PPV_ARGS(&probeList))) ||
        FAILED(probeList->Close())) {
        return -1;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();

    // Submits one probe variant and returns true when the device survived.
    const auto submit = [&](bool withBarriers, bool withState, bool withDraw) {
        if (FAILED(probeAllocator->Reset()) ||
            FAILED(probeList->Reset(probeAllocator.Get(), pipeline))) {
            return false;
        }
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

        if (withBarriers) {
            D3D12_RESOURCE_BARRIER toRenderTarget = transition(
                backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            probeList->ResourceBarrier(1, &toRenderTarget);
        }

        if (withState) {
            D3D12_VIEWPORT viewport{0, 0, static_cast<float>(bufferDesc.Width),
                                    static_cast<float>(bufferDesc.Height),
                                    0.f, 1.f};
            D3D12_RECT scissor{0, 0, static_cast<LONG>(bufferDesc.Width),
                               static_cast<LONG>(bufferDesc.Height)};
            probeList->SetGraphicsRootSignature(rootSignature_.Get());
            probeList->SetPipelineState(pipeline);
            probeList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            probeList->SetDescriptorHeaps(1, srvHeap_.GetAddressOf());
            const float screenSize[2] = {viewport.Width, viewport.Height};
            probeList->SetGraphicsRoot32BitConstants(0, 2, screenSize, 0);
            probeList->SetGraphicsRootDescriptorTable(
                1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
            probeList->RSSetViewports(1, &viewport);
            probeList->RSSetScissorRects(1, &scissor);
            probeList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        }

        if (withDraw) {
            const OverlayVertex triangle[3] = {
                {10.f, 10.f, solidU0_, solidV0_, 1.f, 1.f, 1.f, 1.f},
                {40.f, 10.f, solidU0_, solidV0_, 1.f, 1.f, 1.f, 1.f},
                {10.f, 40.f, solidU0_, solidV0_, 1.f, 1.f, 1.f, 1.f},
            };
            std::memcpy(vertexData_, triangle, sizeof(triangle));
            D3D12_VERTEX_BUFFER_VIEW vertexView{};
            vertexView.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
            vertexView.SizeInBytes = sizeof(triangle);
            vertexView.StrideInBytes = sizeof(OverlayVertex);
            probeList->IASetVertexBuffers(0, 1, &vertexView);
            probeList->DrawInstanced(3, 1, 0, 0);
        }

        if (withBarriers) {
            D3D12_RESOURCE_BARRIER toPresent = transition(
                backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
            probeList->ResourceBarrier(1, &toPresent);
        }
        probeList->Close();

        ID3D12CommandList* lists[] = {probeList.Get()};
        {
            const std::scoped_lock lock{submissionMutex()};
            queue_->ExecuteCommandLists(1, lists);
        }
        Sleep(150);
        return SUCCEEDED(device_->GetDeviceRemovedReason());
    };

    const HRESULT baseline = device_->GetDeviceRemovedReason();
    Logger::info("[trace] d3d12 probe: baseline reason 0x{:08X}",
                 static_cast<unsigned>(baseline));

    // P1: resource-state transitions on the host back buffer only.
    if (!submit(true, false, false)) {
        Logger::error("[trace] d3d12 probe: P1 (barriers only) DENIED - swap "
                      "chain buffer transitions are blocked in this host");
        return 1;
    }
    Logger::info("[trace] d3d12 probe: P1 (barriers only) accepted");

    // P2: + full state binding and render-target reference, no draw.
    if (!submit(true, true, false)) {
        Logger::error("[trace] d3d12 probe: P2 (+ state / render target) "
                      "DENIED - back-buffer reference is blocked");
        return 2;
    }
    Logger::info("[trace] d3d12 probe: P2 (+ state / render target) accepted");

    // P3: + the draw itself.
    if (!submit(true, true, true)) {
        Logger::error("[trace] d3d12 probe: P3 (+ draw) DENIED - the draw "
                      "submission is blocked");
        return 3;
    }
    Logger::info("[trace] d3d12 probe: P3 (+ draw) accepted - stream fully "
                 "accepted");
    return 0;
}

}  // namespace yuzora::rendering
