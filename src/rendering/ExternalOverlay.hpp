#pragma once

#include <atomic>
#include <functional>

#include <Windows.h>

#include "rendering/OverlayInfo.hpp"

namespace yuzora::rendering {

// External overlay: a transparent, click-through, always-on-top window that
// tracks the host game window and draws the watermark with GDI.
//
// Rationale: in-game D3D12 rendering into Minecraft's swap chains is blocked
// by the host runtime (barrier submissions on the composition/UI swap chain
// are denied with DXGI_ERROR_ACCESS_DENIED, which removes the device and
// crashes the game). The external window has zero interaction with the
// game's rendering and cannot crash it. The Present hook keeps running for
// FPS and future event data.
class ExternalOverlay {
public:
    using InfoProvider = std::function<OverlayInfo()>;
    using FpsProvider = std::function<float()>;

    ExternalOverlay() = default;
    ~ExternalOverlay() = default;

    ExternalOverlay(const ExternalOverlay&) = delete;
    ExternalOverlay& operator=(const ExternalOverlay&) = delete;

    // Starts the overlay thread. The callbacks are invoked on the overlay
    // thread roughly every 100 ms.
    bool start(InfoProvider infoProvider, FpsProvider fpsProvider);

    // Signals the thread and waits for it to finish. Call before any state
    // the info callback reads is torn down.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    static DWORD WINAPI threadProcStatic(LPVOID param);
    void threadProc();
    void createWindow();
    void tick();
    void draw(HDC target, int width, int height);
    HWND findGameWindow() const;

    InfoProvider infoProvider_;
    FpsProvider fpsProvider_;

    std::atomic<bool> running_{false};
    HANDLE thread_ = nullptr;
    DWORD threadId_ = 0;
    HWND window_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP memBitmap_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    std::size_t bufferWidth_ = 0;
    std::size_t bufferHeight_ = 0;
    HFONT font_ = nullptr;
    HWND gameWindow_ = nullptr;
};

}  // namespace yuzora::rendering
