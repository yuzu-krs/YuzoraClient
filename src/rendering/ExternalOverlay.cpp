#include "rendering/ExternalOverlay.hpp"

#include <algorithm>
#include <format>
#include <string>

#include "core/Logger.hpp"

namespace yuzora::rendering {

namespace {

constexpr wchar_t kWindowClass[] = L"YuzoraExternalOverlay";
constexpr COLORREF kKeyColor = RGB(255, 0, 255);  // transparent key
constexpr COLORREF kBoxColor = RGB(12, 12, 12);
constexpr COLORREF kTitleColor = RGB(255, 255, 255);
constexpr COLORREF kTextColor = RGB(224, 224, 224);
constexpr UINT kTickMs = 100;

// The UWP host window class (the game's CoreWindow). Newer builds may use a
// normal Win32 window instead, so the search falls back to any visible
// top-level window of this process.
constexpr wchar_t kGameWindowClass[] = L"Windows.UI.Core.CoreWindow";
constexpr wchar_t kFallbackTitlePrefix[] = L"Minecraft";

}  // namespace

bool ExternalOverlay::start(InfoProvider infoProvider, FpsProvider fpsProvider,
                            StatusProvider statusProvider) {
    if (running_.load()) {
        return true;
    }
    infoProvider_ = std::move(infoProvider);
    fpsProvider_ = std::move(fpsProvider);
    statusProvider_ = std::move(statusProvider);

    running_.store(true);
    thread_ = CreateThread(nullptr, 0, threadProcStatic, this, 0, &threadId_);
    if (thread_ == nullptr) {
        running_.store(false);
        return false;
    }
    return true;
}

void ExternalOverlay::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (threadId_ != 0) {
        PostThreadMessage(threadId_, WM_QUIT, 0, 0);
    }
    if (thread_ != nullptr) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
        threadId_ = 0;
    }
}

DWORD WINAPI ExternalOverlay::threadProcStatic(LPVOID param) {
    static_cast<ExternalOverlay*>(param)->threadProc();
    return 0;
}

void ExternalOverlay::createWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWindowClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
        windowClass.hInstance, nullptr);
    if (window_ == nullptr) {
        return;
    }
    SetLayeredWindowAttributes(window_, kKeyColor, 0, LWA_COLORKEY);

    font_ = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_MODERN, L"Consolas");
}

HWND ExternalOverlay::findGameWindow() const {
    struct Candidate {
        HWND window = nullptr;
        bool isCoreWindow = false;
        bool titleMatch = false;
        int width = 0;
        int height = 0;
    };

    struct Context {
        DWORD pid = 0;
        Candidate coreWindow;      // class Windows.UI.Core.CoreWindow
        Candidate titleMatch;      // title starts with "Minecraft"
        Candidate any;             // first visible top-level fallback
        std::wstring inventory;
    } context;
    context.pid = GetCurrentProcessId();

    const auto callback = [](HWND hwnd, LPARAM param) -> BOOL {
        auto* c = reinterpret_cast<Context*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid || !IsWindowVisible(hwnd)) {
            return TRUE;
        }
        wchar_t className[64] = {};
        wchar_t title[128] = {};
        GetClassNameW(hwnd, className, 64);
        GetWindowTextW(hwnd, title, 128);
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) {
            return TRUE;
        }

        c->inventory += std::wstring(L"[") + className + L"] '" + title +
                        L"' " + std::to_wstring(width) + L"x" +
                        std::to_wstring(height) + L"\n";

        Candidate candidate{hwnd, false, false, width, height};
        if (lstrcmpW(className, kGameWindowClass) == 0) {
            candidate.isCoreWindow = true;
            c->coreWindow = candidate;
        }
        if (!c->any.window) {
            c->any = candidate;
        }
        if (wcsncmp(title, kFallbackTitlePrefix, 9) == 0) {
            candidate.titleMatch = true;
            if (!c->titleMatch.window) {
                c->titleMatch = candidate;
            }
        }
        return TRUE;
    };
    EnumWindows(callback, reinterpret_cast<LPARAM>(&context));

    static std::atomic<bool> inventoryLogged{false};
    if (!inventoryLogged.exchange(true)) {
        const int utf8Size = WideCharToMultiByte(
            CP_UTF8, 0, context.inventory.c_str(),
            static_cast<int>(context.inventory.size()), nullptr, 0, nullptr,
            nullptr);
        std::string inventoryUtf8(static_cast<std::size_t>(utf8Size > 0 ? utf8Size : 0), 0);
        if (utf8Size > 0) {
            WideCharToMultiByte(CP_UTF8, 0, context.inventory.c_str(),
                                static_cast<int>(context.inventory.size()),
                                inventoryUtf8.data(), utf8Size, nullptr, nullptr);
        }
        Logger::info("[trace] external overlay window inventory:\n{}",
                     inventoryUtf8);
    }

    if (context.coreWindow.window != nullptr) {
        return context.coreWindow.window;
    }
    if (context.titleMatch.window != nullptr) {
        return context.titleMatch.window;
    }
    return context.any.window;
}

void ExternalOverlay::draw(HDC target, int width, int height) {
    // Fill with the key color: everything magenta becomes transparent.
    RECT full{0, 0, width, height};
    HBRUSH keyBrush = CreateSolidBrush(kKeyColor);
    FillRect(target, &full, keyBrush);
    DeleteObject(keyBrush);

    const OverlayInfo info = infoProvider_ ? infoProvider_() : OverlayInfo{};
    const float fps = fpsProvider_ ? fpsProvider_() : 0.f;

    const float padding = 4.f;
    const float lineStep = 16.f + 2.f;
    const float advance = 9.f;
    const std::string lines[4] = {
        info.gameVersionLine,
        info.coordinatesLine,
        "FPS: " + std::to_string(static_cast<std::uint64_t>(fps > 0.f ? fps : 0.f)),
        info.statusLine,
    };

    float longest = advance * static_cast<float>(info.titleLine.size());
    for (const std::string& line : lines) {
        longest = (std::max)(longest,
                             advance * static_cast<float>(line.size()));
    }
    const float boxWidth = longest + padding * 2.f;
    const float boxHeight = lineStep * 5.f + padding * 2.f;

    RECT box{0, 0, static_cast<LONG>(boxWidth), static_cast<LONG>(boxHeight)};
    HBRUSH boxBrush = CreateSolidBrush(kBoxColor);
    FillRect(target, &box, boxBrush);
    DeleteObject(boxBrush);

    HFONT oldFont = static_cast<HFONT>(SelectObject(target, font_));
    SetBkMode(target, TRANSPARENT);
    SetTextColor(target, kTitleColor);
    TextOutA(target, static_cast<int>(padding), static_cast<int>(padding),
             info.titleLine.c_str(), static_cast<int>(info.titleLine.size()));
    SetTextColor(target, kTextColor);
    float y = padding + lineStep;
    for (const std::string& line : lines) {
        TextOutA(target, static_cast<int>(padding), static_cast<int>(y),
                 line.c_str(), static_cast<int>(line.size()));
        y += lineStep;
    }
    if (statusProvider_) {
        SetTextColor(target, RGB(160, 200, 255));
        const std::string status = statusProvider_();
        TextOutA(target, static_cast<int>(padding), static_cast<int>(y),
                 status.c_str(), static_cast<int>(status.size()));
    }
    SelectObject(target, oldFont);
}

void ExternalOverlay::tick() {
    if (window_ == nullptr) {
        createWindow();
        if (window_ == nullptr) {
            return;
        }
    }

    HWND gameWindow = findGameWindow();
    if (gameWindow_ != gameWindow) {
        gameWindow_ = gameWindow;
        Logger::info("[trace] external overlay tracking window 0x{:016X}",
                     reinterpret_cast<std::uintptr_t>(gameWindow_));
    }
    if (gameWindow_ == nullptr) {
        // No host window found yet - keep the overlay hidden.
        ShowWindow(window_, SW_HIDE);
        return;
    }

    RECT gameRect{};
    GetWindowRect(gameWindow_, &gameRect);
    const int width = gameRect.right - gameRect.left;
    const int height = gameRect.bottom - gameRect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    // (Re)allocate the drawing buffer when the size changes.
    if (bufferWidth_ != static_cast<std::size_t>(width) ||
        bufferHeight_ != static_cast<std::size_t>(height)) {
        if (memDc_ != nullptr) {
            if (oldBitmap_ != nullptr) {
                SelectObject(memDc_, oldBitmap_);
                oldBitmap_ = nullptr;
            }
            DeleteObject(memBitmap_);
            memBitmap_ = nullptr;
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
        bufferWidth_ = static_cast<std::size_t>(width);
        bufferHeight_ = static_cast<std::size_t>(height);
    }
    if (memDc_ == nullptr) {
        HDC windowDc = GetDC(window_);
        memDc_ = CreateCompatibleDC(windowDc);
        ReleaseDC(window_, windowDc);
        if (memDc_ == nullptr) {
            return;
        }
        memBitmap_ = CreateCompatibleBitmap(GetDC(window_), width, height);
        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, memBitmap_));
    }

    draw(memDc_, width, height);

    HDC windowDc = GetDC(window_);
    BitBlt(windowDc, 0, 0, width, height, memDc_, 0, 0, SRCCOPY);
    ReleaseDC(window_, windowDc);

    SetWindowPos(window_, HWND_TOPMOST, gameRect.left, gameRect.top, width,
                 height, SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
}

void ExternalOverlay::threadProc() {
    Logger::info("[trace] external overlay thread started");
    createWindow();
    Logger::info("[trace] external overlay window {}",
                 window_ != nullptr ? "created" : "creation FAILED");

    while (running_.load()) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running_.store(false);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_.load()) {
            break;
        }
        tick();
        Sleep(kTickMs);
    }

    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

}  // namespace yuzora::rendering
