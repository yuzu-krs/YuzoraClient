#include "Client.hpp"

#include <format>
#include <string>

#include "Logger.hpp"
#include "SelfTest.hpp"
#include "memory/Memory.hpp"
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

// Parses the loaded game image's debug directory and logs the CodeView PDB
// reference (name + GUID + age). Microsoft publishes Bedrock client PDBs on
// its public symbol server, which would give exact class layouts for free.
void logGamePdbInfo(std::uintptr_t moduleBase) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const auto& debugDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debugDir.Size == 0) {
        yuzora::Logger::info("[pdb] game image has no debug directory");
        return;
    }
    const auto* entries =
        reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(moduleBase +
                                                       debugDir.VirtualAddress);
    const std::size_t count = debugDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
            continue;
        }
        const auto* cv =
            reinterpret_cast<const std::uint8_t*>(moduleBase +
                                                  entries[i].AddressOfRawData);
        if (cv == nullptr || std::memcmp(cv, "RSDS", 4) != 0) {
            continue;
        }
        GUID guid{};
        std::memcpy(&guid, cv + 4, sizeof(guid));
        const std::uint32_t age =
            *reinterpret_cast<const std::uint32_t*>(cv + 20);
        const char* pdbName = reinterpret_cast<const char*>(cv + 24);

        // Symbol-server GUID text: D1 as byte-swapped hex, D2/D3 as
        // byte-swapped words, then Data4 in memory order.
        const auto byte = [](std::uint8_t b) {
            char buf[3] = {};
            constexpr char kHex[] = "0123456789ABCDEF";
            buf[0] = kHex[b >> 4];
            buf[1] = kHex[b & 0xF];
            return std::string(buf, 2);
        };
        const auto be16 = [](std::uint16_t v) {
            return static_cast<std::uint16_t>((v >> 8) | ((v & 0xFF) << 8));
        };
        std::string guidText;
        for (int shift = 24; shift >= 0; shift -= 8) {
            guidText += byte(static_cast<std::uint8_t>(guid.Data1 >> shift));
        }
        guidText += byte(static_cast<std::uint8_t>(be16(guid.Data2) >> 8));
        guidText += byte(static_cast<std::uint8_t>(be16(guid.Data2) & 0xFF));
        guidText += byte(static_cast<std::uint8_t>(be16(guid.Data3) >> 8));
        guidText += byte(static_cast<std::uint8_t>(be16(guid.Data3) & 0xFF));
        for (int k = 0; k < 8; ++k) {
            guidText += byte(guid.Data4[k]);
        }

        yuzora::Logger::info("[pdb] {} guid={} age={} | symbol server: https://"
                             "msdl.microsoft.com/download/symbols",
                             pdbName, guidText, age);
    }
}

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

        // Passive RTTI resolution groundwork (ESP / position access path).
        if (const auto gameModule =
                memory::getModule(versionManager_.gameModuleName())) {
            (void)sdk_.resolveRuntime(*gameModule);
            logGamePdbInfo(gameModule->base);
        }

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
