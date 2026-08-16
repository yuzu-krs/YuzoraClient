#include "Client.hpp"

#include "Logger.hpp"

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

    // Subsystems of later versions are initialized here, in dependency order.

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

    // Subsystems of later versions are shut down here, in reverse order.

    state_ = ClientState::Shutdown;
    Logger::info("Shutdown");
    return true;
}

ClientState Client::state() const {
    const std::scoped_lock lock{mutex_};
    return state_;
}

}  // namespace yuzora
