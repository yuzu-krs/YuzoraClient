#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "rendering/D3D12Renderer.hpp"
#include "rendering/ExternalOverlay.hpp"
#include "rendering/OverlayInfo.hpp"
#include "rendering/Renderer.hpp"

namespace yuzora::rendering {

// Owns the render hook: swaps the Present entries of the shared swap chain
// vtables for our handlers (a data patch - no code is displaced), so every
// swap chain in the process dispatches through us. The hook covers both
// IDXGISwapChain::Present and IDXGISwapChain1::Present1 on every swap
// chain interface level (0-4): legacy hosts present through the base
// interface, UWP hosts such as Minecraft through SwapChain1::Present1.
// The originals are kept as plain function pointers and called directly.
class RenderManager {
public:
    RenderManager() = default;
    ~RenderManager() = default;

    RenderManager(const RenderManager&) = delete;
    RenderManager& operator=(const RenderManager&) = delete;

    [[nodiscard]] bool initialize(OverlayProvider provider);
    void shutdown();

    [[nodiscard]] bool isHookInstalled() const noexcept { return hookInstalled_; }
    [[nodiscard]] std::uint64_t presentedFrames() const noexcept { return frames_; }
    [[nodiscard]] float fps() const noexcept { return fps_; }
    [[nodiscard]] std::uint64_t drawCalls() const noexcept { return renderer_.drawCalls(); }

private:
    // Internal: invoked from the static Present handlers on the render
    // thread.
    void onPresent(IDXGISwapChain* swapChain);
    void drawOverlay(IDXGISwapChain* swapChain);
    void updateFps();

    friend HRESULT presentHookEntry(IDXGISwapChain* swapChain, UINT syncInterval,
                                    UINT flags);
    friend HRESULT present1HookEntry(IDXGISwapChain* swapChain, UINT syncInterval,
                                     UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* presentParameters);
    friend void executeCommandListsHookEntry(ID3D12CommandQueue* queue,
                                             UINT numCommandLists,
                                             ID3D12CommandList* const* commandLists);
    friend DWORD guardedDraw(RenderManager* manager, IDXGISwapChain* swapChain);

    OverlayProvider provider_;

    bool hookInstalled_ = false;
    bool firstDrawLogged_ = false;
    std::atomic<int> inFlight_{0};
    std::atomic<ID3D12Device*> primaryD3D12Device_{nullptr};

    Renderer renderer_;
    D3D12Renderer d3d12Renderer_;
    ExternalOverlay externalOverlay_;
    std::uint64_t frames_ = 0;
    float fps_ = 0.f;
    std::uint64_t fpsFrameBase_ = 0;
    std::int64_t fpsTimeBase_ = 0;
    std::int64_t fpsFrequency_ = 0;
    bool rendererReady_ = false;
};

}  // namespace yuzora::rendering
