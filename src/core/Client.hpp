#pragma once

#include <mutex>

#include "hooks/HookManager.hpp"
#include "memory/SignatureManager.hpp"
#include "rendering/RenderManager.hpp"
#include "sdk/Sdk.hpp"
#include "version/VersionManager.hpp"

namespace yuzora {

// Lifecycle states of the client.
enum class ClientState {
    Uninitialized,  // DLL loaded, initialize() not called yet
    Initialized,    // initialize() completed successfully
    Shutdown,       // shutdown() completed, the client is no longer usable
};

// Owns the client lifecycle.
//
// DllMain does nothing beyond the bare minimum; every subsystem of later
// versions (memory, hooks, modules, ...) is started and stopped from here,
// in one place, in dependency order.
class Client {
public:
    Client() = default;
    ~Client() = default;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // Process-wide client instance. The exported YuzoraInitialize /
    // YuzoraShutdown functions are thin wrappers around this object.
    static Client& instance() noexcept;

    // Brings the client up.
    //
    // Returns true when the client is initialized after the call. Calling
    // initialize() on an already initialized client logs a warning and stays
    // true; calling it after shutdown is an error and returns false.
    bool initialize();

    // Tears the client down, in reverse initialization order.
    //
    // Returns true when the client is shut down after the call. Shutting down
    // an uninitialized client only logs a warning (there is nothing to do).
    bool shutdown();

    [[nodiscard]] ClientState state() const;

    // SDK access: the sanctioned path to game internals. Valid between
    // initialize() and shutdown(); reports "unavailable" rather than
    // crashing when the game functions are not resolved.
    [[nodiscard]] sdk::Sdk& sdk() noexcept { return sdk_; }

    // Read-only subsystem views used by the overlay and diagnostics.
    [[nodiscard]] const version::VersionManager& versionManager() const noexcept {
        return versionManager_;
    }
    [[nodiscard]] const memory::SignatureManager& signatureManager() const noexcept {
        return signatureManager_;
    }
    [[nodiscard]] const hooks::HookManager& hookManager() const noexcept {
        return hookManager_;
    }

private:
    // Assembles the overlay content from subsystem state. Called on the
    // render thread after initialization; only reads initialized state
    // (besides the SDK's internal view re-wrapping).
    [[nodiscard]] rendering::OverlayInfo buildOverlay();

    mutable std::mutex mutex_;

    // Subsystems owned by the client. Initialized in declaration order,
    // shut down in reverse order:
    // version -> signatures -> sdk -> hooks -> rendering.
    version::VersionManager versionManager_;
    memory::SignatureManager signatureManager_;
    sdk::Sdk sdk_;
    hooks::HookManager hookManager_;
    rendering::RenderManager renderManager_;

    ClientState state_ = ClientState::Uninitialized;
};

}  // namespace yuzora
