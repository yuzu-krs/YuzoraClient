#pragma once

#include <string_view>
#include <vector>

#include "version/GameVersion.hpp"

namespace yuzora::version {

// Detects which Minecraft Bedrock executable is loaded in the current
// process and reads its version. This is the only place that is allowed to
// know module names of the game; everything else asks this manager.
//
// The version is read from the loaded image's RT_VERSION resource, so no
// file on disk is opened (important for packaged-app paths).
class VersionManager {
public:
    VersionManager();

    // Scans the candidate game modules and reads the version of the first
    // one found. Safe to call repeatedly. Returns true when a game module
    // was detected, false when running standalone (e.g. inside LoaderTest).
    bool detect();

    // Clears the detection result (called from Client::shutdown).
    void reset();

    [[nodiscard]] bool isGameDetected() const noexcept { return detected_; }
    [[nodiscard]] const GameVersion& version() const noexcept { return version_; }

    // File name of the detected game module, e.g. L"Minecraft.Windows.exe".
    // Empty when nothing was detected.
    [[nodiscard]] const std::wstring& gameModuleName() const noexcept { return gameModuleName_; }

private:
    std::vector<std::wstring> candidates_;
    std::wstring gameModuleName_;
    GameVersion version_{};
    bool detected_ = false;
};

}  // namespace yuzora::version
