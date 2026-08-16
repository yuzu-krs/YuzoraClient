#include "memory/Memory.hpp"

#include <Windows.h>
#include <psapi.h>

#include <algorithm>

namespace yuzora::memory {

namespace {

ModuleInfo moduleInfoFromHandle(HMODULE handle) {
    ModuleInfo info;
    info.handle = handle;

    MODULEINFO module{};
    if (GetModuleInformation(GetCurrentProcess(), handle, &module, sizeof(module)) == 0) {
        return info;
    }
    info.base = reinterpret_cast<std::uintptr_t>(module.lpBaseOfDll);
    info.size = module.SizeOfImage;

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            break;
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }

    // Reduce to the base file name.
    const std::size_t slash = path.find_last_of(L"\\/");
    info.name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    return info;
}

}  // namespace

std::optional<ModuleInfo> getModule(std::wstring_view name) {
    if (name.empty()) {
        return std::nullopt;
    }

    const std::wstring nullTerminated{name};
    HMODULE handle = GetModuleHandleW(nullTerminated.c_str());
    if (handle == nullptr) {
        return std::nullopt;
    }

    return moduleInfoFromHandle(handle);
}

ModuleInfo getSelfModule() {
    HMODULE handle = nullptr;
    const BOOL ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&getSelfModule), &handle);
    if (ok == 0 || handle == nullptr) {
        return {};
    }
    return moduleInfoFromHandle(handle);
}

void forEachReadableRegion(std::uintptr_t base, std::size_t size,
                           const std::function<void(std::uintptr_t, std::size_t)>& callback) {
    const std::uintptr_t end = base + size;
    std::uintptr_t current = base;

    while (current < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &info, sizeof(info)) == 0) {
            break;
        }

        const std::uintptr_t regionStart =
            reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::uintptr_t regionEnd =
            std::min<std::uintptr_t>(regionStart + info.RegionSize, end);
        current = std::max<std::uintptr_t>(regionStart + info.RegionSize, current + 1);

        if (info.State != MEM_COMMIT) {
            continue;
        }

        // Mask off modifier flags; the remaining base protection decides
        // readability.
        constexpr DWORD kModifierMask =
            PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE | PAGE_TARGETS_INVALID;
        const DWORD protect = info.Protect & ~kModifierMask;
        const bool readable =
            (protect == PAGE_READONLY || protect == PAGE_READWRITE ||
             protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ ||
             protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
        if (!readable || regionEnd <= base || regionStart >= regionEnd) {
            continue;
        }

        const std::uintptr_t clippedStart = std::max<std::uintptr_t>(regionStart, base);
        callback(clippedStart, regionEnd - clippedStart);
    }
}

std::string toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string converted(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        converted.data(), size, nullptr, nullptr);
    return converted;
}

}  // namespace yuzora::memory
