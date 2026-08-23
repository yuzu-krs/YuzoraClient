#pragma once

#include <string_view>

#include "sdk/SdkFunctions.hpp"
#include "sdk/client/ClientInstance.hpp"

namespace yuzora::memory {
class SignatureManager;
}

namespace yuzora::sdk {

// Canonical signature names the SDK resolves. The matching patterns are
// registered by the version layer once reverse-engineered offsets for the
// supported game versions land; until then every entry stays unresolved
// and the SDK reports itself unavailable (which is not an error).
inline constexpr std::string_view kSignatureClientInstance =
    "ClientInstance::getInstance";
inline constexpr std::string_view kSignatureGetLocalPlayer =
    "ClientInstance::getLocalPlayer";
inline constexpr std::string_view kSignatureGetLevel =
    "ClientInstance::getLevel";
inline constexpr std::string_view kSignatureGetPosition =
    "Actor::getPosition";

// The SDK facade: the only sanctioned way to reach Minecraft internals.
//
// Consumers ask for typed wrappers; signatures, offsets and patterns stay
// outside this layer. The wrappers' raw() is a diagnostics escape hatch
// for lower layers, not for feature code. Availability depends entirely
// on the resolved function table; without it every accessor safely
// reports "nothing".
class Sdk {
public:
    Sdk() = default;
    ~Sdk() = default;

    // Wrappers hold a pointer into this object's table, so the Sdk itself
    // must stay put.
    Sdk(const Sdk&) = delete;
    Sdk& operator=(const Sdk&) = delete;
    Sdk(Sdk&&) = delete;
    Sdk& operator=(Sdk&&) = delete;

    // Installs a function table directly (used by tests and by
    // resolveFromSignatures internally).
    void initialize(SdkFunctions functions);

    // Fills the table from resolved signatures, using the canonical names
    // above. Called by the Client in game mode.
    void resolveFromSignatures(const memory::SignatureManager& signatures);

    // Clears the table; every accessor becomes unavailable.
    void shutdown();

    // True when the root access function is resolved.
    [[nodiscard]] bool isAvailable() const noexcept;

    // Root entry point. Null while unavailable or when the game itself
    // returned no client instance.
    [[nodiscard]] ClientInstance* getClientInstance();

    // The function table (diagnostics and tests).
    [[nodiscard]] const SdkFunctions& functions() const noexcept { return functions_; }

    // Logs the per-function resolution status and a summary, in the
    // established "[OK] / [!!]" diagnostics format.
    void logDiagnostics() const;

private:
    SdkFunctions functions_{};
    ClientInstance clientInstance_{};  // re-wrapped on every access
};

}  // namespace yuzora::sdk
