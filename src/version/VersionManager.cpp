#include "version/VersionManager.hpp"

#include <Windows.h>

#include <cstddef>

#include "core/Logger.hpp"
#include "memory/Memory.hpp"

namespace yuzora::version {

namespace {

// Reads the fixed version info of a loaded module from its RT_VERSION
// resource. The VS_VERSIONINFO resource starts with
//   WORD wLength; WORD wValueLength; WORD wType; WCHAR szKey[15 + 1];
// followed by 32-bit alignment padding and the VS_FIXEDFILEINFO.
std::optional<GameVersion> readModuleVersion(HMODULE module) {
    // VS_VERSION_INFO / RT_VERSION are A-flavored MAKEINTRESOURCE macros;
    // reinterpret the integer payloads for the W flavor of FindResource.
    const HRSRC resource =
        FindResourceW(module, MAKEINTRESOURCEW(1), reinterpret_cast<LPCWSTR>(RT_VERSION));
    if (resource == nullptr) {
        return std::nullopt;
    }

    const HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr) {
        return std::nullopt;
    }

    const void* data = LockResource(loaded);
    const DWORD size = SizeofResource(module, resource);
    if (data == nullptr || size < 6 + 32 + sizeof(VS_FIXEDFILEINFO)) {
        return std::nullopt;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);

    // szKey is L"VS_VERSION_INFO" (15 characters + terminator) at offset 6.
    constexpr wchar_t kKey[] = L"VS_VERSION_INFO";
    const auto* key = reinterpret_cast<const wchar_t*>(bytes + 6);
    for (std::size_t i = 0; i < 15; ++i) {
        if (kKey[i] == L'\0' || key[i] != kKey[i]) {
            return std::nullopt;
        }
    }

    const std::size_t fixedOffset = (6 + 32 + 3) & ~static_cast<std::size_t>(3);
    if (size < fixedOffset + sizeof(VS_FIXEDFILEINFO)) {
        return std::nullopt;
    }

    const auto* fixed =
        reinterpret_cast<const VS_FIXEDFILEINFO*>(bytes + fixedOffset);
    if (fixed->dwSignature != 0xFEEF04BD) {
        return std::nullopt;
    }

    return GameVersion{
        HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
        HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS),
    };
}

}  // namespace

VersionManager::VersionManager() {
    // Bedrock for Windows (UWP) first, pre-UWP name second.
    candidates_.push_back(L"Minecraft.Windows.exe");
    candidates_.push_back(L"Minecraft.exe");
}

bool VersionManager::detect() {
    detected_ = false;
    gameModuleName_.clear();
    version_ = GameVersion{};

    for (const std::wstring& candidate : candidates_) {
        const std::optional<memory::ModuleInfo> module = memory::getModule(candidate);
        if (!module.has_value() || module->handle == nullptr) {
            continue;
        }

        std::optional<GameVersion> parsed =
            readModuleVersion(static_cast<HMODULE>(module->handle));
        if (!parsed.has_value()) {
            Logger::warning("found game module {} but could not read its version resource",
                            memory::toUtf8(candidate));
            parsed.emplace();
        }

        gameModuleName_ = candidate;
        version_ = *parsed;
        detected_ = true;
        Logger::info("Minecraft detected: {} ({})", version_.toString(),
                     memory::toUtf8(candidate));
        return true;
    }

    Logger::info("Minecraft module not found - running standalone");
    return false;
}

void VersionManager::reset() {
    detected_ = false;
    gameModuleName_.clear();
    version_ = GameVersion{};
}

}  // namespace yuzora::version
