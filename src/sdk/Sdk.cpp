#include "sdk/Sdk.hpp"

#include <iterator>

#include "core/Logger.hpp"
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
