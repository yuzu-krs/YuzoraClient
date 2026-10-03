#include "rendering/RenderManager.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

#include "core/Logger.hpp"
#include "events/EventBus.hpp"
#include "events/types/RenderEvent.hpp"
#include "memory/Memory.hpp"

namespace yuzora::rendering {

namespace {

using PresentFunction = HRESULT (*)(IDXGISwapChain*, UINT, UINT);
using Present1Function =
    HRESULT (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandListsFunction =
    void (*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// Vtable slots, verified against the compiler's own dispatch (decoded from
// the emitted call sites) and the SDK headers: Present is slot 8 (offset
// 0x40) on every IDXGISwapChainN vtable, Present1 is slot 22 (offset 0xB0)
// on the IDXGISwapChain1+ vtables, and ExecuteCommandLists is slot 10
// (offset 0x50) on the ID3D12CommandQueue vtable (IUnknown 0-2 +
// ID3D12Object 3-6 + GetDevice 7 + UpdateTileMappings/CopyTileMappings 8-9).
constexpr std::size_t kPresentSlot = 8;
constexpr std::size_t kPresent1Slot = 22;
constexpr std::size_t kExecuteCommandListsSlot = 10;

std::atomic<RenderManager*> g_renderManager{nullptr};
std::atomic<PresentFunction> g_originalPresent{nullptr};
std::atomic<Present1Function> g_originalPresent1{nullptr};
std::atomic<ExecuteCommandListsFunction> g_originalExecuteCommandLists{nullptr};

// Captured command queues, keyed by their owning device. The overlay must
// submit on the SAME device as the swap chain it renders into - submitting
// a command list through another device's queue is denied by the runtime
// (DXGI_ERROR_ACCESS_DENIED device removal). Hosts such as Minecraft may
// run several devices (render, UI composition, video).
std::mutex g_queueMapMutex;
std::unordered_map<ID3D12CommandQueue*, ID3D12Device*> g_queueToDevice;
std::unordered_map<ID3D12Device*, ID3D12CommandQueue*> g_deviceToQueue;

// One patched (vtable, slot) pair with its captured original.
struct VtablePatch {
    void** vtable = nullptr;
    std::size_t slot = 0;
    void* original = nullptr;
};
std::vector<VtablePatch> g_patches;  // touched only on the loader thread

HRESULT presentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
HRESULT present1Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                     const DXGI_PRESENT_PARAMETERS* presentParameters);
void executeCommandListsHook(ID3D12CommandQueue* queue, UINT numCommandLists,
                             ID3D12CommandList* const* commandLists);

// Swaps one vtable entry and returns the captured original (nullptr on
// failure).
void* writeVtableSlot(void** vtable, std::size_t slot, void* hook) {
    DWORD oldProtect = 0;
    if (VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect) == 0) {
        Logger::error("cannot make the vtable entry writable (error {})",
                      GetLastError());
        return nullptr;
    }
    void* const original = vtable[slot];
    vtable[slot] = hook;
    DWORD restored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &restored);
    return original;
}

void restoreVtableSlot(const VtablePatch& patch) {
    DWORD oldProtect = 0;
    if (VirtualProtect(&patch.vtable[patch.slot], sizeof(void*), PAGE_READWRITE,
                       &oldProtect) == 0) {
        Logger::error("cannot restore the vtable entry (error {})",
                      GetLastError());
        return;
    }
    patch.vtable[patch.slot] = patch.original;
    DWORD restored = 0;
    VirtualProtect(&patch.vtable[patch.slot], sizeof(void*), oldProtect, &restored);
}

// Overlay draws are guarded with SEH: a faulting D3D12 call must disable the
// overlay and report the faulting module instead of crashing the host.
std::atomic<bool> g_drawFaultLogged{false};
std::atomic<bool> g_drawFaulted{false};
DWORD g_drawFaultCode = 0;
void* g_drawFaultAddress = nullptr;

int drawExceptionFilter(PEXCEPTION_POINTERS info) {
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        g_drawFaultCode = info->ExceptionRecord->ExceptionCode;
        if (info->ExceptionRecord->ExceptionCode == 0xC0000005 &&
            info->ExceptionRecord->ExceptionInformation[0] != 0) {
            g_drawFaultAddress =
                reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]);
        } else {
            g_drawFaultAddress = info->ExceptionRecord->ExceptionAddress;
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// Development diagnostic: reads the swap chain's back buffer back and writes
// it as a BMP so the drawn overlay can be inspected on remote sessions.
void dumpBackBufferBMP(ID3D12Device* device, ID3D12CommandQueue* queue,
                       IDXGISwapChain* swapChain) {
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        backBuffer == nullptr) {
        return;
    }
    const D3D12_RESOURCE_DESC backDesc = backBuffer->GetDesc();
    const std::size_t rowPitch =
        (static_cast<std::size_t>(backDesc.Width) * 4 + 255) &
        ~static_cast<std::size_t>(255);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = rowPitch * backDesc.Height;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommittedResource(
            &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence))) ||
        readback == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER toSource = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    toSource.Transition.pResource = backBuffer.Get();
    toSource.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toSource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &toSource);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = backDesc.Format;
    destination.PlacedFootprint.Footprint.Width = static_cast<UINT>(backDesc.Width);
    destination.PlacedFootprint.Footprint.Height = static_cast<UINT>(backDesc.Height);
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER toCommon = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    toCommon.Transition.pResource = backBuffer.Get();
    toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    toCommon.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &toCommon);
    list->Close();

    ID3D12CommandList* lists[] = {list.Get()};
    {
        const std::scoped_lock lock{D3D12Renderer::submissionMutex()};
        queue->ExecuteCommandLists(1, lists);
    }
    queue->Signal(fence.Get(), 1);
    HANDLE completion = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE);
    if (completion != nullptr) {
        fence->SetEventOnCompletion(1, completion);
        WaitForSingleObject(completion, 5000);
        CloseHandle(completion);
    }

    D3D12_RANGE mapRange{0, rowPitch * backDesc.Height};
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, &mapRange, &mapped)) || mapped == nullptr) {
        return;
    }

    const std::size_t width = backDesc.Width;
    const std::size_t height = backDesc.Height;
    std::vector<std::uint8_t> bmp(14 + 40 + width * height * 4, 0);
    auto* fileHeader = reinterpret_cast<BITMAPFILEHEADER*>(bmp.data());
    fileHeader->bfType = 0x4D42;
    fileHeader->bfSize = static_cast<DWORD>(bmp.size());
    fileHeader->bfOffBits = 14 + 40;
    auto* infoHeader = reinterpret_cast<BITMAPINFOHEADER*>(bmp.data() + 14);
    infoHeader->biSize = 40;
    infoHeader->biWidth = static_cast<LONG>(width);
    infoHeader->biHeight = -static_cast<LONG>(height);
    infoHeader->biPlanes = 1;
    infoHeader->biBitCount = 32;
    infoHeader->biCompression = BI_RGB;

    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    std::uint8_t* out = bmp.data() + 54;
    for (std::size_t y = 0; y < height; ++y) {
        std::memcpy(out + y * width * 4, pixels + y * rowPitch, width * 4);
    }
    D3D12_RANGE empty{0, 0};
    readback->Unmap(0, &empty);

    FILE* bmpFile = nullptr;
    if (fopen_s(&bmpFile,
                "C:\\Users\\yuzut\\AppData\\Local\\Packages\\"
                "Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\"
                "yuzora-overlay.bmp",
                "wb") == 0 &&
        bmpFile != nullptr) {
        std::fwrite(bmp.data(), 1, bmp.size(), bmpFile);
        std::fclose(bmpFile);
        Logger::info("[trace] overlay back buffer dumped to yuzora-overlay.bmp");
    }
}

DWORD guardedDraw(RenderManager* manager, IDXGISwapChain* swapChain) {
    __try {
        manager->drawOverlay(swapChain);
        return 0;
    } __except (drawExceptionFilter(GetExceptionInformation())) {
        return g_drawFaultCode;
    }
}

HRESULT presentHookEntry(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        Logger::info("[trace] first intercepted Present (swap chain vtable 0x{:016X})",
                     reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(swapChain)));
    }

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

HRESULT present1HookEntry(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                          const DXGI_PRESENT_PARAMETERS* presentParameters) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        Logger::info("[trace] first intercepted Present1 (swap chain vtable 0x{:016X})",
                     reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(swapChain)));
    }

    RenderManager* const manager = g_renderManager.load();
    if (manager != nullptr && (flags & DXGI_PRESENT_TEST) == 0) {
        manager->onPresent(swapChain);
    }

    const Present1Function original = g_originalPresent1.load();
    if (original == nullptr) {
        Logger::error("Present1 hook fired without an original to call");
        return DXGI_ERROR_INVALID_CALL;
    }
    return original(swapChain, syncInterval, flags, presentParameters);
}

namespace {

HRESULT presentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    return presentHookEntry(swapChain, syncInterval, flags);
}

HRESULT present1Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                     const DXGI_PRESENT_PARAMETERS* presentParameters) {
    return present1HookEntry(swapChain, syncInterval, flags, presentParameters);
}

}  // namespace

void executeCommandListsHookEntry(ID3D12CommandQueue* queue, UINT numCommandLists,
                                  ID3D12CommandList* const* commandLists) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        Logger::info("[trace] first intercepted ExecuteCommandLists (queue 0x{:016X})",
                     reinterpret_cast<std::uintptr_t>(queue));
    }
    // Only graphics queues may receive our overlay's graphics command list;
    // submitting it to a copy/compute queue would be denied by the runtime.
    const D3D12_COMMAND_QUEUE_DESC queueDescription = queue->GetDesc();
    if (queueDescription.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        const std::scoped_lock mapLock{g_queueMapMutex};
        const auto known = g_queueToDevice.find(queue);
        if (known == g_queueToDevice.end()) {
            // First sighting: resolve the owning device once.
            Microsoft::WRL::ComPtr<ID3D12Device> owner;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&owner))) &&
                owner != nullptr) {
                g_queueToDevice.emplace(queue, owner.Get());
                g_deviceToQueue[owner.Get()] = queue;
                static std::atomic<bool> capturedLogged{false};
                if (!capturedLogged.exchange(true)) {
                    Logger::info("[trace] captured direct command queue 0x{:016X} "
                                 "(device 0x{:016X})",
                                 reinterpret_cast<std::uintptr_t>(queue),
                                 reinterpret_cast<std::uintptr_t>(owner.Get()));
                }
            }
        }
    }
    const ExecuteCommandListsFunction original = g_originalExecuteCommandLists.load();
    if (original != nullptr) {
        // Serialize with our own submissions: ID3D12CommandQueue is not safe
        // for concurrent ExecuteCommandLists from multiple threads.
        const std::scoped_lock lock{D3D12Renderer::submissionMutex()};
        original(queue, numCommandLists, commandLists);
    }
}

namespace {

void executeCommandListsHook(ID3D12CommandQueue* queue, UINT numCommandLists,
                             ID3D12CommandList* const* commandLists) {
    executeCommandListsHookEntry(queue, numCommandLists, commandLists);
}

}  // namespace

bool RenderManager::initialize(OverlayProvider provider) {
    shutdown();

    provider_ = std::move(provider);

    // The external overlay is the crash-safe visibility path: it never
    // touches the game's rendering, so hosts whose runtime denies foreign
    // D3D12 submissions (Minecraft's composition/UI swap chains) cannot be
    // disturbed by it.
    externalOverlay_.start(provider_, [this] { return fps_; });

    // Throwaway window + device + swap chains: only the vtables are needed.
    // Two probe swap chains are created so that both dxgi swap chain
    // implementation classes are covered: an HWND flip-model one (desktop
    // hosts) and a composition one (UWP hosts such as Minecraft create
    // their swap chains through CreateSwapChainForCoreWindow/Composition,
    // which may use a different implementation class with its own vtables).
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"YuzoraRenderProbe";
    RegisterClassW(&windowClass);
    HWND probeWindow = CreateWindowExW(0, L"YuzoraRenderProbe", L"", WS_OVERLAPPEDWINDOW,
                                       CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr,
                                       nullptr, windowClass.hInstance, nullptr);

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IUnknown> hwndProbe;       // IDXGISwapChain(1)
    Microsoft::WRL::ComPtr<IUnknown> compositionProbe; // IDXGISwapChain1

    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                       &context);
    if (FAILED(result)) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    if (SUCCEEDED(result) && device != nullptr) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = 64;
            description.Height = 64;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

            Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(
                    device.Get(), probeWindow, &description, nullptr, nullptr,
                    &swapChain1))) {
                hwndProbe = swapChain1;
            }
            Microsoft::WRL::ComPtr<IDXGISwapChain1> compositionSwapChain1;
            if (SUCCEEDED(factory->CreateSwapChainForComposition(
                    device.Get(), &description, nullptr, &compositionSwapChain1))) {
                compositionProbe = compositionSwapChain1;
            }
        }
    }
    if (hwndProbe == nullptr) {
        Microsoft::WRL::ComPtr<IDXGISwapChain> legacyProbe;
        DXGI_SWAP_CHAIN_DESC legacy{};
        legacy.BufferCount = 2;
        legacy.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        legacy.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        legacy.OutputWindow = probeWindow;
        legacy.SampleDesc.Count = 1;
        legacy.Windowed = TRUE;
        legacy.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                               nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &legacy,
                                               &legacyProbe, &device, nullptr,
                                               &context);
        if (FAILED(result)) {
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
                                                   nullptr, 0, nullptr, 0,
                                                   D3D11_SDK_VERSION, &legacy,
                                                   &legacyProbe, &device, nullptr,
                                                   &context);
        }
        hwndProbe = legacyProbe;
    }
    if (FAILED(result) || hwndProbe == nullptr) {
        Logger::error("cannot create the D3D11 probe device (HRESULT 0x{:08X})",
                      static_cast<unsigned>(result));
        if (probeWindow != nullptr) {
            DestroyWindow(probeWindow);
        }
        return false;
    }

    // Collect the distinct vtables of every interface level of both probe
    // swap chains (both classes may share one vtable set). Every level >= 1
    // also carries Present1.
    const IID swapChainIids[] = {
        __uuidof(IDXGISwapChain),  __uuidof(IDXGISwapChain1), __uuidof(IDXGISwapChain2),
        __uuidof(IDXGISwapChain3), __uuidof(IDXGISwapChain4),
    };
    struct ProbeVtable {
        void** vtable = nullptr;
        bool hasPresent1 = false;
    };
    std::vector<ProbeVtable> probeVtables;
    const auto collectVtables = [&](IUnknown* swapChain) {
        std::size_t level = 0;
        for (const IID& iid : swapChainIids) {
            Microsoft::WRL::ComPtr<IUnknown> interfacePointer;
            if (FAILED(swapChain->QueryInterface(
                    iid, reinterpret_cast<void**>(
                             interfacePointer.ReleaseAndGetAddressOf())))) {
                break;  // levels are cumulative; stop at the first missing one
            }
            void** const vtable = *reinterpret_cast<void***>(interfacePointer.Get());
            const auto existing = std::find_if(
                probeVtables.begin(), probeVtables.end(),
                [&](const ProbeVtable& candidate) {
                    return candidate.vtable == vtable;
                });
            if (existing == probeVtables.end()) {
                probeVtables.push_back({vtable, level >= 1});
            } else if (level >= 1) {
                existing->hasPresent1 = true;
            }
            ++level;
        }
    };
    collectVtables(hwndProbe.Get());
    if (compositionProbe != nullptr) {
        collectVtables(compositionProbe.Get());
    }
    if (probeVtables.empty()) {
        Logger::error("cannot read the swap chain vtable");
        if (probeWindow != nullptr) {
            DestroyWindow(probeWindow);
        }
        return false;
    }

    // Capture the originals BEFORE patching anything: a Present that starts
    // dispatching the moment an entry is written must already see valid
    // originals and a live manager.
    const PresentFunction originalPresent = reinterpret_cast<PresentFunction>(
        probeVtables[0].vtable[kPresentSlot]);
    Present1Function originalPresent1 = nullptr;
    for (const ProbeVtable& candidate : probeVtables) {
        if (candidate.hasPresent1) {
            originalPresent1 = reinterpret_cast<Present1Function>(
                candidate.vtable[kPresent1Slot]);
            break;
        }
    }

    g_patches.clear();

    // D3D12 hosts (Minecraft since its DX12 backend): hook
    // ExecuteCommandLists FIRST, so the command queue is already captured by
    // the time the first Present flows through the (soon patched) swap
    // chain vtables - otherwise the first frames would be skipped.
    {
        Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&d3d12Device))) &&
            d3d12Device != nullptr) {
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> probeQueue;
            if (SUCCEEDED(d3d12Device->CreateCommandQueue(
                    &queueDesc, IID_PPV_ARGS(&probeQueue))) &&
                probeQueue != nullptr) {
                void** const queueVtable =
                    *reinterpret_cast<void***>(probeQueue.Get());
                // Publish the original before the vtable write, as with the
                // Present hooks.
                g_originalExecuteCommandLists.store(
                    reinterpret_cast<ExecuteCommandListsFunction>(
                        queueVtable[kExecuteCommandListsSlot]));
                void* const patchOriginal = writeVtableSlot(
                    queueVtable, kExecuteCommandListsSlot,
                    reinterpret_cast<void*>(&executeCommandListsHook));
                if (patchOriginal != nullptr) {
                    g_patches.push_back(
                        {queueVtable, kExecuteCommandListsSlot, patchOriginal});
                    Logger::info("D3D12 ExecuteCommandLists hook installed "
                                 "(original at 0x{:016X})",
                                 reinterpret_cast<std::uintptr_t>(patchOriginal));
                }
            }
        } else {
            Logger::info("D3D12 unavailable on this system - overlay limited "
                         "to D3D11 hosts");
        }
    }

    g_originalPresent.store(originalPresent);
    g_originalPresent1.store(originalPresent1);
    g_renderManager.store(this);

    for (const ProbeVtable& candidate : probeVtables) {
        void* const patchOriginal =
            writeVtableSlot(candidate.vtable, kPresentSlot,
                            reinterpret_cast<void*>(&presentHook));
        if (patchOriginal != nullptr) {
            g_patches.push_back({candidate.vtable, kPresentSlot, patchOriginal});
        }
        if (candidate.hasPresent1 && originalPresent1 != nullptr) {
            void* const patchOriginal1 =
                writeVtableSlot(candidate.vtable, kPresent1Slot,
                                reinterpret_cast<void*>(&present1Hook));
            if (patchOriginal1 != nullptr) {
                g_patches.push_back(
                    {candidate.vtable, kPresent1Slot, patchOriginal1});
            }
        }
    }

    if (probeWindow != nullptr) {
        DestroyWindow(probeWindow);
    }

    if (g_patches.empty()) {
        Logger::error("Present vtable hooks could not be installed");
        g_renderManager.store(nullptr);
        return false;
    }

    hookInstalled_ = true;
    std::string vtableList;
    for (const ProbeVtable& candidate : probeVtables) {
        vtableList += std::format("0x{:016X} ",
                                  reinterpret_cast<std::uintptr_t>(candidate.vtable));
    }
    Logger::info("Present vtable hooks installed ({} entries over {} vtables: "
                 "{}, Present1: {})",
                 g_patches.size(), probeVtables.size(), vtableList,
                 originalPresent1 != nullptr ? "hooked" : "n/a");
    return true;
}

void RenderManager::shutdown() {
    // Stop the overlay thread first: its info callback reads subsystem
    // state that the rest of this function (and Client's later teardown)
    // releases.
    externalOverlay_.stop();

    // Stop our per-frame work first: Presents still racing through a
    // patched entry then only call the (still valid) originals.
    g_renderManager.store(nullptr);

    for (const VtablePatch& patch : g_patches) {
        restoreVtableSlot(patch);
    }
    g_patches.clear();
    Logger::info("[trace] shutdown: vtables restored");

    // Drain frames that entered the hook before the restore before
    // releasing anything they might touch. The original pointers are
    // deliberately kept valid: they point into dxgi.dll, so even a
    // straggler dispatch is safe until the next initialize re-captures them.
    for (int waited = 0; inFlight_.load() != 0 && waited < 100; ++waited) {
        std::this_thread::yield();
    }
    if (inFlight_.load() != 0) {
        Logger::warning("a frame was still inside the Present hook at shutdown");
    }
    Logger::info("[trace] shutdown: drained");

    hookInstalled_ = false;
    firstDrawLogged_ = false;
    {
        const std::scoped_lock mapLock{g_queueMapMutex};
        g_queueToDevice.clear();
        g_deviceToQueue.clear();
    }
    Logger::info("[trace] shutdown: queue maps cleared");
    d3d12Renderer_.shutdown();
    Logger::info("[trace] shutdown: d3d12 renderer released");
    renderer_.shutdown();
    Logger::info("[trace] shutdown: d3d11 renderer released");
    rendererReady_ = false;
    provider_ = nullptr;
}

void RenderManager::onPresent(IDXGISwapChain* swapChain) {
    // Re-entrancy/concurrency guard: only one overlay draw runs at a time.
    // The host may present from several threads (main swap chain, UI swap
    // chain, ...), and the check must be atomic - a check-then-increment
    // race let two threads draw through the same renderer and crashed the
    // host.
    if (inFlight_.fetch_add(1, std::memory_order_acq_rel) != 0) {
        inFlight_.fetch_sub(1, std::memory_order_release);
        return;
    }

    ++frames_;
    updateFps();

    if (provider_ != nullptr) {
        // Exceptions (C++ and SEH) must never unwind into the host's Present
        // call: a faulting draw is reported and disables the D3D12 overlay.
        try {
            if (const DWORD fault = guardedDraw(this, swapChain); fault != 0) {
                g_drawFaulted.store(true);
                if (!g_drawFaultLogged.exchange(true)) {
                    const auto classify = [&](const wchar_t* name) {
                        const auto module = memory::getModule(name);
                        return module.has_value()
                                   ? std::format("{} [0x{:X}, 0x{:X})", memory::toUtf8(name),
                                                 module->base, module->base + module->size)
                                   : std::format("{} <not loaded>", memory::toUtf8(name));
                    };
                    Logger::error("overlay draw faulted: code 0x{:08X} at 0x{:016X} | "
                                  "self {} | d3d12 {} | dxgi {}",
                                  static_cast<unsigned>(fault),
                                  reinterpret_cast<std::uintptr_t>(g_drawFaultAddress),
                                  classify(L"YuzoraClient.dll"), classify(L"d3d12.dll"),
                                  classify(L"dxgi.dll"));
                }
            }
        } catch (...) {
            Logger::error("overlay drawing threw an exception");
        }
    }

    events::EventBus::dispatch(RenderEvent{});

    inFlight_.fetch_sub(1, std::memory_order_release);
}

void RenderManager::drawOverlay(IDXGISwapChain* swapChain) {
    // One-time diagnostics: log the first draw that gets skipped and why.
    const auto skip = [](const char* reason) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            Logger::warning("[trace] overlay draw skipped: {}", reason);
        }
    };

    const OverlayInfo info = provider_();
    const std::string lines[4] = {
        info.gameVersionLine,
        info.coordinatesLine,
        std::format("FPS: {}", static_cast<std::uint64_t>(fps_)),
        info.statusLine,
    };
    const float padding = 4.f;

    const auto logFirstDraw = [this](const char* backend) {
        if (!firstDrawLogged_) {
            firstDrawLogged_ = true;
            Logger::info("[trace] overlay first frame drawn ({})", backend);
        }
    };

    // --- D3D11 hosts -------------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&d3d11Device))) &&
        d3d11Device != nullptr) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        d3d11Device->GetImmediateContext(&context);

        // Back-buffer resources are created per frame and released
        // immediately afterwards: nothing is cached across frames, so the
        // host's ResizeBuffers never sees outstanding references.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
            backBuffer == nullptr) {
            skip("GetBuffer failed");
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(d3d11Device->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                                       &target))) {
            skip("CreateRenderTargetView failed");
            return;
        }

        if (renderer_.device() != d3d11Device.Get()) {
            renderer_.shutdown();
            rendererReady_ = renderer_.initialize(d3d11Device.Get(), context.Get());
            if (!rendererReady_) {
                skip("renderer initialization failed (see earlier errors)");
                return;
            }
        }

        float longest = renderer_.textAdvance() *
                        static_cast<float>(info.titleLine.size());
        for (const std::string& line : lines) {
            longest = (std::max)(
                longest,
                renderer_.textAdvance() * static_cast<float>(line.size()));
        }
        const float lineStep = renderer_.textHeight() + 2.f;

        renderer_.beginFrame(target.Get(), static_cast<float>(desc.Width),
                             static_cast<float>(desc.Height));
        renderer_.drawFilledRect(8.f, 8.f, longest + padding * 2.f,
                                 lineStep * 5.f + padding * 2.f, 0x88000000);
        renderer_.drawText(8.f + padding, 8.f + padding, info.titleLine,
                           0xFFFFFFFF);
        float y = 8.f + padding + lineStep;
        for (const std::string& line : lines) {
            renderer_.drawText(8.f + padding, y, line, 0xFFE0E0E0);
            y += lineStep;
        }
        renderer_.endFrame();
        logFirstDraw("D3D11");
        return;
    }

    // --- D3D12 hosts (Minecraft since its DX12 backend) ---------------------
    // In-game D3D12 drawing is research-only and disabled by default: this
    // host denies foreign submissions touching its composition/UI swap
    // chains (DXGI_ERROR_ACCESS_DENIED device removal). The external overlay
    // window provides visibility instead. YUZORA_D3D12=1 re-enables research.
    static const bool d3d12Enabled = [] {
        char value[2] = {};
        return GetEnvironmentVariableA("YUZORA_D3D12", value, 2) > 0;
    }();
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
    if (d3d12Enabled &&
        SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&d3d12Device))) &&
        d3d12Device != nullptr) {

        // Hosts with several swap chains (render + UI) on different devices
        // would otherwise thrash the renderer through shutdown/re-init on
        // every alternating present. Render for the first device only.
        ID3D12Device* expected = nullptr;
        if (!primaryD3D12Device_.compare_exchange_strong(expected,
                                                         d3d12Device.Get()) &&
            expected != d3d12Device.Get()) {
            static std::atomic<bool> secondaryLogged{false};
            if (!secondaryLogged.exchange(true)) {
                Logger::info("[trace] skipping overlay for a non-primary D3D12 "
                             "device");
            }
            return;
        }

        static std::atomic<bool> d3d12PathLogged{false};
        if (!d3d12PathLogged.exchange(true)) {
            Logger::info("[trace] overlay D3D12 path active");
        }
        ID3D12CommandQueue* queue = nullptr;
        {
            const std::scoped_lock mapLock{g_queueMapMutex};
            const auto entry = g_deviceToQueue.find(d3d12Device.Get());
            if (entry != g_deviceToQueue.end()) {
                queue = entry->second;
            }
        }
        if (queue == nullptr) {
            skip("no captured command queue for this device yet");
            return;
        }
        if (g_drawFaulted.load()) {
            skip("D3D12 overlay disabled after a previous fault");
            return;
        }
        if (!d3d12Renderer_.ensure(d3d12Device.Get(), queue)) {
            skip("D3D12 renderer initialization failed (see earlier errors)");
            return;
        }

        // One-time staged probe: bisect which part of the command stream
        // this host denies (barriers / render-target reference / draw).
        // Disables the overlay and reports instead of killing the host's
        // device. A negative result means the probe infrastructure itself
        // failed - skip this frame without disabling.
        {
            static std::atomic<bool> probeDone{false};
            const int deniedStage =
                probeDone.exchange(true)
                    ? 0
                    : d3d12Renderer_.probeCommandStream(swapChain);
            if (deniedStage > 0) {
                g_drawFaulted.store(true);
                skip("D3D12 overlay disabled: probe stage denied");
                return;
            }
        }

        float longest = d3d12Renderer_.textAdvance() *
                        static_cast<float>(info.titleLine.size());
        for (const std::string& line : lines) {
            longest = (std::max)(
                longest,
                d3d12Renderer_.textAdvance() * static_cast<float>(line.size()));
        }
        const float lineStep = d3d12Renderer_.textHeight() + 2.f;

        if (!d3d12Renderer_.beginFrame(swapChain)) {
            skip("D3D12 frame skipped (allocators still in flight)");
            return;
        }
        d3d12Renderer_.drawFilledRect(8.f, 8.f, longest + padding * 2.f,
                                      lineStep * 5.f + padding * 2.f,
                                      0x88000000);
        d3d12Renderer_.drawText(8.f + padding, 8.f + padding, info.titleLine,
                                0xFFFFFFFF);
        float y = 8.f + padding + lineStep;
        for (const std::string& line : lines) {
            d3d12Renderer_.drawText(8.f + padding, y, line, 0xFFE0E0E0);
            y += lineStep;
        }
        d3d12Renderer_.endFrame();
        logFirstDraw("D3D12");

        // A removed device would crash the host on every further submit -
        // stop drawing the moment the runtime reports one.
        if (FAILED(d3d12Device->GetDeviceRemovedReason())) {
            g_drawFaulted.store(true);
            skip("D3D12 device has been removed - overlay disabled");
            return;
        }

        // Back-buffer dump for remote debugging: opt-in via YUZORA_DUMP=1.
        // (Copying a host's swap chain buffer can itself be denied on
        // protected hosts, so this never runs by default.)
        static std::atomic<bool> dumpDone{false};
        static const bool dumpEnabled = [] {
            char value[2] = {};
            return GetEnvironmentVariableA("YUZORA_DUMP", value, 2) > 0;
        }();
        if (dumpEnabled && !dumpDone.exchange(true)) {
            d3d12Renderer_.waitIdle();
            dumpBackBufferBMP(d3d12Device.Get(), queue, swapChain);
        }
        return;
    }

    skip("swap chain is neither D3D11 nor D3D12");
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
