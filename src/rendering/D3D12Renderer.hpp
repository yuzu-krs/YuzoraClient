#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "rendering/Font.hpp"
#include "rendering/Shaders.hpp"

namespace yuzora::rendering {

// Minimal D3D12 immediate-mode overlay renderer: the D3D12 counterpart of
// Renderer. Records the overlay into a command list that is executed on the
// host's captured command queue right inside its Present call.
//
// Resource safety: command allocators rotate through a small ring and are
// only reused once a fence proves the GPU finished with them; frames whose
// allocator is still in flight are skipped instead of blocking the game.
// All submissions on the host's queue are serialized through
// submissionMutex() because ID3D12CommandQueue is not safe for concurrent
// ExecuteCommandLists from multiple threads.
class D3D12Renderer {
public:
    D3D12Renderer() = default;
    ~D3D12Renderer() = default;

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    // Serializes ExecuteCommandLists on the host's queue between our
    // submissions and the host's own (recursive: our submissions re-enter
    // the hook on the same thread).
    [[nodiscard]] static std::recursive_mutex& submissionMutex();

    // (Re)initializes against a device + queue pair. Cheap no-op when
    // already bound to this exact pair. Returns false on failure with a
    // logged reason.
    [[nodiscard]] bool ensure(ID3D12Device* device, ID3D12CommandQueue* queue);
    void shutdown();

    // One-time diagnostic: bisects which part of the command stream the
    // runtime denies for rendering into the host's back buffer (barriers /
    // render-target reference / draw). Returns 0 when everything is
    // accepted, the first denied stage (1..3) otherwise, -1 on probe
    // infrastructure failure.
    [[nodiscard]] int probeCommandStream(IDXGISwapChain* swapChain);

    // Blocks until all our submitted GPU work has completed.
    void waitIdle();

    [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }

    // Starts recording into the swap chain's back buffer. Returns false when
    // the frame must be skipped (allocator still in flight, buffers
    // unavailable).
    [[nodiscard]] bool beginFrame(IDXGISwapChain* swapChain);
    void drawFilledRect(float x, float y, float width, float height,
                        std::uint32_t argb);
    void drawText(float x, float y, std::string_view text, std::uint32_t argb);
    void endFrame();

    [[nodiscard]] float textHeight() const noexcept { return fontCellHeight_; }
    [[nodiscard]] float textAdvance() const noexcept { return fontAdvance_; }

private:
    static constexpr std::size_t kAllocatorSlots = 3;

    // Returns the PSO for this render target format/sample setup, creating
    // it on first use; nullptr on failure.
    [[nodiscard]] ID3D12PipelineState* buildPipeline(DXGI_FORMAT format,
                                                     UINT sampleCount,
                                                     UINT sampleQuality);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Device> device_;
    // NON-owning: the host's captured command queue. We only submit to it;
    // its lifetime belongs to the host.
    ID3D12CommandQueue* queue_ = nullptr;
    ComPtr<ID3D12CommandQueue> initQueue_;  // own queue for one-time uploads

    ComPtr<ID3D12CommandAllocator> allocators_[kAllocatorSlots];
    ComPtr<ID3D12GraphicsCommandList> commandList_;
    ComPtr<ID3D12Fence> fence_;
    std::uint64_t fenceValues_[kAllocatorSlots] = {};
    std::uint64_t fenceValue_ = 0;
    std::size_t allocatorSlot_ = 0;

    ComPtr<ID3D12RootSignature> rootSignature_;
    std::unordered_map<std::uint32_t, ComPtr<ID3D12PipelineState>> pipelines_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12Resource> atlas_;
    ComPtr<ID3D12Resource> atlasUpload_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    OverlayVertex* vertexData_ = nullptr;      // persistently mapped
    ComPtr<ID3D12Resource> frameBackBuffer_;   // back buffer of the open frame

    FontAtlas font_;
    float fontCellHeight_ = 0.f;
    float fontAdvance_ = 0.f;
    float solidU0_ = 0.f;
    float solidV0_ = 0.f;
    float solidU1_ = 0.f;
    float solidV1_ = 0.f;

    std::vector<OverlayVertex> vertices_;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    bool useBarriers_ = true;  // chosen by the probe
    bool frameActive_ = false;
    bool ready_ = false;
};

}  // namespace yuzora::rendering
