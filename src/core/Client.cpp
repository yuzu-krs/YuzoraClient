#include "Client.hpp"

#include <format>
#include <string>

#include "Logger.hpp"
#include "SelfTest.hpp"
#include "events/EventBus.hpp"
#include "rendering/RenderManager.hpp"
#include "sdk/client/ClientInstance.hpp"
#include "sdk/math/Vec3.hpp"

namespace {

// Client version baked in by CMake.
#define YUZORA_STR2(value) #value
#define YUZORA_STR(value) YUZORA_STR2(value)
constexpr const char* kClientVersion =
    "v" YUZORA_STR(YUZORA_VERSION_MAJOR) "." YUZORA_STR(YUZORA_VERSION_MINOR) "."
    YUZORA_STR(YUZORA_VERSION_PATCH) "-dev";

}  // namespace

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

        // v0.4 scope: the SDK resolves through the signature layer, but no
        // production signatures exist yet - the SDK therefore reports
        // itself unavailable, which is expected and not an error.
        sdk_.resolveFromSignatures(signatureManager_);
        sdk_.logDiagnostics();
        Logger::info("Production SDK functions: none resolvable yet (signatures "
                     "pending reverse engineering)");

        // v0.3 scope: the hook foundation is verified by the standalone
        // self-test. No production Minecraft hooks are registered yet -
        // they arrive once real signatures/SDK land in a later milestone.
        Logger::info("Production hooks: none registered yet (v0.3 scope: hook "
                     "foundation only)");
        hookManager_.installAll();
        hookManager_.logDiagnostics();

        if (!renderManager_.initialize([this] { return buildOverlay(); })) {
            Logger::error("render manager initialization failed; aborting initialization");
            state_ = ClientState::Uninitialized;
            return false;
        }
        Logger::info("Render hook installed - overlay should be visible on the game");
    } else {
        // Standalone (test loader) environment: validate the foundations on
        // our own module instead.
        if (!runMemorySelfTest(signatureManager_)) {
            Logger::error("memory self-test failed; aborting initialization");
            state_ = ClientState::Uninitialized;
            return false;
        }
        if (!runEventSelfTest()) {
            Logger::error("event self-test failed; aborting initialization");
            state_ = ClientState::Uninitialized;
            return false;
        }
        if (!runHookSelfTest(hookManager_)) {
            Logger::error("hook self-test failed; aborting initialization");
            state_ = ClientState::Uninitialized;
            return false;
        }
        if (!runSdkSelfTest(sdk_)) {
            Logger::error("sdk self-test failed; aborting initialization");
            state_ = ClientState::Uninitialized;
            return false;
        }
        if (!runRenderSelfTest(renderManager_)) {
            Logger::error("render self-test failed; aborting initialization");
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
    renderManager_.shutdown();
    hookManager_.uninstallAll();
    events::EventBus::clearAllSubscriptions();
    sdk_.shutdown();
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

rendering::OverlayInfo Client::buildOverlay() {
    rendering::OverlayInfo info;
    info.titleLine = std::format("YuzoraClient {} [{}]", kClientVersion,
                                 versionManager_.isGameDetected() ? "game" : "standalone");

    info.gameVersionLine = versionManager_.isGameDetected()
                               ? std::format("Minecraft: {}", versionManager_.version().toString())
                               : std::string("Minecraft: not detected");

    // Position access goes through the SDK; until real signatures are
    // resolved it honestly reports unavailable.
    std::string coordinates = "XYZ: unavailable";
    if (sdk_.isAvailable()) {
        sdk::ClientInstance* const client = sdk_.getClientInstance();
        sdk::LocalPlayer* const player =
            (client != nullptr) ? client->getLocalPlayer() : nullptr;
        if (player != nullptr) {
            const sdk::Vec3 position = player->getPosition();
            coordinates = std::format("XYZ: {:.1f} {:.1f} {:.1f}", position.x,
                                      position.y, position.z);
        }
    }
    info.coordinatesLine = coordinates;

    const sdk::SdkFunctions& functions = sdk_.functions();
    const unsigned sdkResolved =
        static_cast<unsigned>(functions.getClientInstance != nullptr) +
        static_cast<unsigned>(functions.getLocalPlayer != nullptr) +
        static_cast<unsigned>(functions.getLevel != nullptr) +
        static_cast<unsigned>(functions.getPosition != nullptr);

    info.statusLine = std::format("Signatures: {}/{}  Hooks: {}/{}  SDK: {}/{}",
                                  signatureManager_.resolvedCount(),
                                  signatureManager_.count(), hookManager_.installedCount(),
                                  hookManager_.count(), sdkResolved, 4u);
    return info;
}

}  // namespace yuzora
