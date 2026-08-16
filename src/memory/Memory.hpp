#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace yuzora::memory {

// A loaded module in the current process.
struct ModuleInfo {
    std::wstring name;    // base file name, e.g. L"YuzoraClient.dll"
    void* handle = nullptr;  // HMODULE
    std::uintptr_t base = 0;  // image base address
    std::size_t size = 0;     // image size in bytes
};

// Looks up a loaded module by base file name (case-insensitive, no path).
// Returns nullopt when the module is not loaded in this process.
[[nodiscard]] std::optional<ModuleInfo> getModule(std::wstring_view name);

// Returns information about the module this code lives in (YuzoraClient.dll).
[[nodiscard]] ModuleInfo getSelfModule();

// Invokes the callback for every committed, readable sub-range of
// [base, base + size). Guard and no-access pages are skipped, so callers can
// scan a whole image without tripping over inter-section gaps.
void forEachReadableRegion(std::uintptr_t base, std::size_t size,
                           const std::function<void(std::uintptr_t, std::size_t)>& callback);

// Converts UTF-16 text (e.g. module names) to UTF-8 for logging.
[[nodiscard]] std::string toUtf8(std::wstring_view text);

}  // namespace yuzora::memory
