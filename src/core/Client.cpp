#include "Client.hpp"

#include "Logger.hpp"
#include "SelfTest.hpp"

namespace yuzora {

Client& Client::instance() noexcept {
    // Function-local static: constructed on first use (thread-safely), so the
    // DLL performs no global-static work while being loaded.
    static Client client;
    return client;
}

bool Client::initialize() {
    const std::scoped_lock lock{mutex_};

    switch (state_) {
        case ClientState::Initialized:
            Logger::warning("initialize() called, but the client is already initialized");
            return true;

        case ClientState::Shutdown:
            Logger::error("initialize() called after shutdown; the client cannot restart");
            return false;

        case ClientState::Uninitialized:
            break;
    }

    // Subsystems are initialized here, in dependency order:
    // memory utilities are stateless, version detection comes first, then
    // the signature layer resolves against the detected game module.
    const bool gameDetected = versionManager_.detect();

    if (gameDetected) {
        // Real signature set is registered here in later versions; the scan
        // below already reports whatever is registered.
        signatureManager_.setDefaultModule(versionManager_.gameModuleName());
        signatureManager_.scanAll();
    } else {
        // Standalone (test loader) environment: validate the memory and
        // signature foundation on our own module instead.
        if (!runMemorySelfTest(signatureManager_)) {
            Logger::error("memory self-test failed; aborting initialization");
            state_ = ClientState::Uninitialized;
            return false;
        }
    }

    state_ = ClientState::Initialized;
    Logger::info("Initialized");
    return true;
}

bool Client::shutdown() {
    const std::scoped_lock lock{mutex_};

    switch (state_) {
        case ClientState::Uninitialized:
            Logger::warning("shutdown() called, but the client was never initialized");
            return true;

        case ClientState::Shutdown:
            Logger::warning("shutdown() called twice; ignoring");
            return true;

        case ClientState::Initialized:
            break;
    }

    // Subsystems are shut down here, in reverse initialization order.
    signatureManager_.clear();
    versionManager_.reset();

    state_ = ClientState::Shutdown;
    Logger::info("Shutdown");
    return true;
}

ClientState Client::state() const {
    const std::scoped_lock lock{mutex_};
    return state_;
}

}  // namespace yuzora
