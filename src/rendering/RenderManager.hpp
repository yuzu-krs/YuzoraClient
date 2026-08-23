#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "rendering/Renderer.hpp"

namespace yuzora::rendering {

// Overlay content supplied by the Client on every frame. Assembled off the
// render thread's concerns so rendering stays game-agnostic.
struct OverlayInfo {
    std::string titleLine;       // e.g. "YuzoraClient v0.5.0-dev"
    std::string gameVersionLine; // e.g. "Minecraft: 1.21.94"
    std::string coordinatesLine; // e.g. "XYZ: 12.5 64.0 -8.3"
    std::string statusLine;      // e.g. "Signatures: 0/4  Hooks: 1/1  SDK: 0/4"
};

using OverlayProvider = std::function<OverlayInfo()>;

// Owns the render hook: swaps the Present entry of the shared swap chain
// vtable for our handler (a data patch - no code is displaced), so every
// swap chain in the process, the game's included, dispatches through us.
// The original Present is kept as a plain function pointer and called
// directly. Minecraft Bedrock is D3D11, so the same hook works in
// LoaderTest and in the game.
//
// Threading: the hook fires on the host's render thread while initialize()
// and shutdown() run on the loader thread. The install order (original
// pointer first, vtable write last) and the shutdown order (unhook, drain
// in-flight frames, then release resources) make that safe; back-buffer
// resources are created per frame and never cached, so the host's
// ResizeBuffers stays fully functional.
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
    // Internal: invoked from the static Present handler on the render
    // thread.
    void onPresent(IDXGISwapChain* swapChain);
    void drawOverlay(IDXGISwapChain* swapChain);
    void updateFps();

    friend HRESULT presentHookEntry(IDXGISwapChain* swapChain, UINT syncInterval,
                                    UINT flags);

    OverlayProvider provider_;

    void** vtable_ = nullptr;
    HRESULT (*originalPresent_)(IDXGISwapChain*, UINT, UINT) = nullptr;
    bool hookInstalled_ = false;
    std::atomic<int> inFlight_{0};

    Renderer renderer_;
    std::uint64_t frames_ = 0;
    float fps_ = 0.f;
    std::uint64_t fpsFrameBase_ = 0;
    std::int64_t fpsTimeBase_ = 0;
    std::int64_t fpsFrequency_ = 0;
    bool rendererReady_ = false;
};

}  // namespace yuzora::rendering
