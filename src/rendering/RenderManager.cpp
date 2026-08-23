#include "rendering/RenderManager.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <thread>

#include "core/Logger.hpp"
#include "events/EventBus.hpp"
#include "events/types/RenderEvent.hpp"

namespace yuzora::rendering {

namespace {

using PresentFunction = HRESULT (*)(IDXGISwapChain*, UINT, UINT);

// Reached from the static handler installed in the vtable. The original
// Present pointer is stored BEFORE the vtable entry is patched and stays
// valid for the process lifetime (it points into dxgi.dll), so a Present
// racing the install or shutdown windows always has a valid function to
// call.
std::atomic<RenderManager*> g_renderManager{nullptr};
std::atomic<PresentFunction> g_originalPresent{nullptr};

// IDXGISwapChain vtable slot of Present, verified against the compiler's
// own dispatch ([vtable+0x40]): slot 8.
constexpr std::size_t kPresentSlot = 8;

HRESULT presentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

}  // namespace

HRESULT presentHookEntry(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    RenderManager* const manager = g_renderManager.load();
    if (manager != nullptr && (flags & DXGI_PRESENT_TEST) == 0) {
        manager->onPresent(swapChain);
    }

    const PresentFunction original = g_originalPresent.load();
    if (original == nullptr) {
        // Unreachable with the install ordering; never crash the host.
        Logger::error("Present hook fired without an original to call");
        return DXGI_ERROR_INVALID_CALL;
    }
    return original(swapChain, syncInterval, flags);
}

namespace {

HRESULT presentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    return presentHookEntry(swapChain, syncInterval, flags);
}

}  // namespace

bool RenderManager::initialize(OverlayProvider provider) {
    shutdown();

    provider_ = std::move(provider);

    // Throwaway device + swap chain: only its vtable is needed. Swap chains
    // of the same DXGI implementation share one vtable, so replacing the
    // Present entry covers every swap chain in the process, the game's
    // included - no signature scan and no code patching required.
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"YuzoraRenderProbe";
    RegisterClassW(&windowClass);
    HWND probeWindow = CreateWindowExW(0, L"YuzoraRenderProbe", L"", WS_OVERLAPPEDWINDOW,
                                       CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr,
                                       nullptr, windowClass.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = probeWindow;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> probeSwapChain;
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &description, &probeSwapChain, &device, nullptr,
        &context);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &description, &probeSwapChain, &device, nullptr,
            &context);
    }
    if (FAILED(result) || probeSwapChain == nullptr) {
        Logger::error("cannot create the D3D11 probe device (HRESULT 0x{:08X})",
                      static_cast<unsigned>(result));
        if (probeWindow != nullptr) {
            DestroyWindow(probeWindow);
        }
        return false;
    }

    void** const vtable = *reinterpret_cast<void***>(probeSwapChain.Get());
    originalPresent_ = reinterpret_cast<PresentFunction>(vtable[kPresentSlot]);

    // Publish the originals BEFORE patching the vtable: a Present that
    // starts dispatching the moment the entry is written must already see a
    // valid original and manager.
    g_originalPresent.store(originalPresent_);
    g_renderManager.store(this);

    // Swap the shared vtable entry. Vtables live in read-only sections, so
    // lift the protection for the one pointer-sized slot only.
    DWORD oldProtect = 0;
    if (VirtualProtect(&vtable[kPresentSlot], sizeof(void*), PAGE_READWRITE,
                       &oldProtect) == 0) {
        Logger::error("cannot make the vtable entry writable (error {})",
                      GetLastError());
        g_renderManager.store(nullptr);
        g_originalPresent.store(nullptr);
        originalPresent_ = nullptr;
        if (probeWindow != nullptr) {
            DestroyWindow(probeWindow);
        }
        return false;
    }
    vtable[kPresentSlot] = reinterpret_cast<void*>(&presentHook);
    DWORD restored = 0;
    VirtualProtect(&vtable[kPresentSlot], sizeof(void*), oldProtect, &restored);

    vtable_ = vtable;
    hookInstalled_ = true;

    if (probeWindow != nullptr) {
        DestroyWindow(probeWindow);
    }
    Logger::info("Present vtable hook installed (vtable at 0x{:016X}, original at "
                 "0x{:016X})",
                 reinterpret_cast<std::uintptr_t>(vtable_),
                 reinterpret_cast<std::uintptr_t>(originalPresent_));
    return true;
}

void RenderManager::shutdown() {
    if (hookInstalled_ && vtable_ != nullptr && originalPresent_ != nullptr) {
        // Stop our per-frame work first: Presents still racing through the
        // patched entry then only call the (still valid) original.
        g_renderManager.store(nullptr);

        DWORD oldProtect = 0;
        if (VirtualProtect(&vtable_[kPresentSlot], sizeof(void*), PAGE_READWRITE,
                           &oldProtect) != 0) {
            vtable_[kPresentSlot] = reinterpret_cast<void*>(originalPresent_);
            DWORD restored = 0;
            VirtualProtect(&vtable_[kPresentSlot], sizeof(void*), oldProtect,
                           &restored);
        } else {
            Logger::error("cannot restore the vtable entry (error {})",
                          GetLastError());
        }

        // Drain frames that entered the hook before the restore before
        // releasing anything they might touch. g_originalPresent is
        // deliberately kept: it points into dxgi.dll and stays valid, so
        // even a straggler dispatch is safe.
        for (int waited = 0; inFlight_.load() != 0 && waited < 100; ++waited) {
            std::this_thread::yield();
        }
        if (inFlight_.load() != 0) {
            Logger::warning("a frame was still inside the Present hook at shutdown");
        }
    }

    g_renderManager.store(nullptr);
    vtable_ = nullptr;
    originalPresent_ = nullptr;
    hookInstalled_ = false;

    renderer_.shutdown();
    rendererReady_ = false;
    provider_ = nullptr;
}

void RenderManager::onPresent(IDXGISwapChain* swapChain) {
    // Re-entrancy guard: providers or lazy renderer initialization must not
    // recurse into Present handling.
    if (inFlight_.load() > 0) {
        return;
    }
    ++inFlight_;

    ++frames_;
    updateFps();

    if (provider_ != nullptr) {
        // Exceptions must never unwind into the host's Present call.
        try {
            drawOverlay(swapChain);
        } catch (...) {
            Logger::error("overlay drawing threw an exception");
        }
    }

    events::EventBus::dispatch(RenderEvent{});

    --inFlight_;
}

void RenderManager::drawOverlay(IDXGISwapChain* swapChain) {
    // Back-buffer resources are created per frame and released immediately
    // afterwards: nothing is cached across frames, so the host's
    // ResizeBuffers never sees outstanding references and resize/fullscreen
    // stays fully functional.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        backBuffer == nullptr) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target))) {
        return;
    }

    if (renderer_.device() != device.Get()) {
        renderer_.shutdown();
        rendererReady_ = renderer_.initialize(device.Get(), context.Get());
        if (!rendererReady_) {
            return;
        }
    }

    const OverlayInfo info = provider_();
    const std::string lines[4] = {
        info.gameVersionLine,
        info.coordinatesLine,
        std::format("FPS: {}", static_cast<std::uint64_t>(fps_)),
        info.statusLine,
    };

    float longest = renderer_.textAdvance() *
                    static_cast<float>(info.titleLine.size());
    for (const std::string& line : lines) {
        longest = (std::max)(longest,
                             renderer_.textAdvance() * static_cast<float>(line.size()));
    }

    const float padding = 4.f;
    const float lineStep = renderer_.textHeight() + 2.f;
    const float boxWidth = longest + padding * 2.f;
    const float boxHeight = lineStep * 5.f + padding * 2.f;
    const float width = static_cast<float>(desc.Width);
    const float height = static_cast<float>(desc.Height);

    renderer_.beginFrame(target.Get(), width, height);
    renderer_.drawFilledRect(8.f, 8.f, boxWidth, boxHeight, 0x88000000);
    renderer_.drawText(8.f + padding, 8.f + padding, info.titleLine, 0xFFFFFFFF);
    float y = 8.f + padding + lineStep;
    for (const std::string& line : lines) {
        renderer_.drawText(8.f + padding, y, line, 0xFFE0E0E0);
        y += lineStep;
    }
    renderer_.endFrame();
}

void RenderManager::updateFps() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (fpsTimeBase_ == 0) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        fpsFrequency_ = frequency.QuadPart;
        fpsTimeBase_ = now.QuadPart;
        fpsFrameBase_ = frames_;
        return;
    }
    const std::int64_t elapsed = now.QuadPart - fpsTimeBase_;
    if (fpsFrequency_ == 0 || elapsed < fpsFrequency_) {
        return;
    }
    const double seconds =
        static_cast<double>(elapsed) / static_cast<double>(fpsFrequency_);
    fps_ = static_cast<float>(static_cast<double>(frames_ - fpsFrameBase_) / seconds);
    fpsTimeBase_ = now.QuadPart;
    fpsFrameBase_ = frames_;
}

}  // namespace yuzora::rendering
