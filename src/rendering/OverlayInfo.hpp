#pragma once

#include <functional>
#include <string>

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

}  // namespace yuzora::rendering
