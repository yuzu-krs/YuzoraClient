#pragma once

#include <mutex>

#include "hooks/HookManager.hpp"
#include "memory/SignatureManager.hpp"
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

private:
    mutable std::mutex mutex_;

    // Subsystems owned by the client. Initialized in declaration order,
    // shut down in reverse order. v0.3+ code reaches them through accessors
    // added when the first real consumer exists.
    version::VersionManager versionManager_;
    memory::SignatureManager signatureManager_;
    hooks::HookManager hookManager_;

    ClientState state_ = ClientState::Uninitialized;
};

}  // namespace yuzora
