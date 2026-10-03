#include "sdk/Sdk.hpp"

#include <iterator>

#include "core/Logger.hpp"
#include "memory/RttiScanner.hpp"
#include "memory/SignatureManager.hpp"

namespace yuzora::sdk {

void Sdk::initialize(SdkFunctions functions) {
    functions_ = functions;
}

void Sdk::resolveFromSignatures(const memory::SignatureManager& signatures) {
    functions_ = {};

    if (const auto address = signatures.get(kSignatureClientInstance)) {
        functions_.getClientInstance = reinterpret_cast<void* (*)()>(*address);
    }
    if (const auto address = signatures.get(kSignatureGetLocalPlayer)) {
        functions_.getLocalPlayer = reinterpret_cast<void* (*)(void*)>(*address);
    }
    if (const auto address = signatures.get(kSignatureGetLevel)) {
        functions_.getLevel = reinterpret_cast<void* (*)(void*)>(*address);
    }
    if (const auto address = signatures.get(kSignatureGetPosition)) {
        functions_.getPosition = reinterpret_cast<Vec3 (*)(void*)>(*address);
    }
}

bool Sdk::resolveRuntime(const memory::ModuleInfo& gameModule) {
    runtimeClientInstance_ = 0;
    runtimeLocalPlayer_ = 0;

    using memory::RttiScanner;

    // Chain 1: ClientInstance - the singleton lives in the module's data.
    const auto clientTd =
        RttiScanner::findTypeDescriptor(gameModule, ".?AVClientInstance@@");
    Logger::info("[rtti] ClientInstance type descriptor: 0x{:016X}", clientTd);
    if (clientTd == 0) {
        return false;
    }
    const auto clientVftables =
        RttiScanner::findVftables(gameModule, clientTd);
    Logger::info("[rtti] ClientInstance vftables: {}", clientVftables.size());
    if (clientVftables.empty()) {
        return false;
    }
    for (const auto& info : clientVftables) {
        const auto instances =
            RttiScanner::findInstances(gameModule, info.vftableAddress);
        Logger::info("[rtti] ClientInstance vftable 0x{:016X} -> {} instance(s)",
                     info.vftableAddress, instances.size());
        if (!instances.empty()) {
            runtimeClientInstance_ = instances.front();
            break;
        }
    }
    Logger::info("[rtti] ClientInstance @ 0x{:016X}", runtimeClientInstance_);
    if (runtimeClientInstance_ == 0) {
        return false;
    }

    // Chain 2: LocalPlayer - a member pointer inside ClientInstance whose
    // target starts with the LocalPlayer vftable.
    const auto playerTd =
        RttiScanner::findTypeDescriptor(gameModule, ".?AVLocalPlayer@@");
    if (playerTd == 0) {
        Logger::info("[rtti] LocalPlayer type descriptor not found");
        return false;
    }
    const auto playerVftables = RttiScanner::findVftables(gameModule, playerTd);
    Logger::info("[rtti] LocalPlayer vftables: {}", playerVftables.size());
    for (const auto& info : playerVftables) {
        const auto member = RttiScanner::findMemberWithVftable(
            runtimeClientInstance_, info.vftableAddress, 0x900);
        if (member != 0) {
            runtimeLocalPlayer_ = member;
            break;
        }
    }
    Logger::info("[rtti] LocalPlayer @ 0x{:016X}", runtimeLocalPlayer_);
    return true;
}

void Sdk::shutdown() {
    functions_ = {};
    clientInstance_ = ClientInstance{};
}

bool Sdk::isAvailable() const noexcept {
    return functions_.getClientInstance != nullptr;
}

ClientInstance* Sdk::getClientInstance() {
    if (!isAvailable()) {
        return nullptr;
    }
    void* instance = functions_.getClientInstance();
    if (instance == nullptr) {
        return nullptr;
    }
    clientInstance_ = ClientInstance{&functions_, instance};
    return &clientInstance_;
}

void Sdk::logDiagnostics() const {
    Logger::info("SDK diagnostics");

    const std::string_view names[] = {
        kSignatureClientInstance,
        kSignatureGetLocalPlayer,
        kSignatureGetLevel,
        kSignatureGetPosition,
    };
    const bool resolved[] = {
        functions_.getClientInstance != nullptr,
        functions_.getLocalPlayer != nullptr,
        functions_.getLevel != nullptr,
        functions_.getPosition != nullptr,
    };

    std::size_t count = 0;
    for (std::size_t i = 0; i < std::size(names); ++i) {
        if (resolved[i]) {
            ++count;
            Logger::info("[OK] {}", names[i]);
        } else {
            Logger::info("[!!] {} - not resolved", names[i]);
        }
    }
    Logger::info("{} / {} SDK functions resolved", count, std::size(names));
}

}  // namespace yuzora::sdk
