#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "memory/Signature.hpp"

namespace yuzora::memory {

// Owns the named signature registry and resolves it against loaded modules.
//
// Modules must never resolve raw addresses themselves; they ask the manager
// (in later versions through the SDK) by name.
class SignatureManager {
public:
    // Module signatures are scanned against when no explicit module is given.
    // Set from the detected game module name by the Client.
    void setDefaultModule(std::wstring_view name);

    // Registers a signature. Returns false when the name is already taken or
    // the pattern text is empty.
    bool add(std::string name, std::string pattern, std::wstring moduleName = {});

    // Resolves module lookups once, scans every registered signature and
    // logs the diagnostics block:
    //   [OK] Name -> 0x...
    //   [!!] Name - not found
    //   N / M signatures resolved
    void scanAll();

    // Removes every registered signature (called from Client::shutdown).
    void clear();

    // Resolved address of a signature, or nullopt when it is unknown
    // (missing, ambiguous, invalid or not registered).
    [[nodiscard]] std::optional<std::uintptr_t> get(std::string_view name) const;

    // Read access for diagnostics; nullptr when the name is unknown.
    [[nodiscard]] const Signature* find(std::string_view name) const;

    [[nodiscard]] std::size_t count() const noexcept { return signatures_.size(); }
    [[nodiscard]] std::size_t resolvedCount() const noexcept;

private:
    std::vector<Signature> signatures_;
    std::wstring defaultModule_;
};

}  // namespace yuzora::memory
