#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "hooks/Hook.hpp"

namespace yuzora::hooks {

// Owns every registered hook and drives their lifecycle: registered in
// order, installed in order, uninstalled in reverse order. The Client owns
// the one process-wide HookManager.
class HookManager {
public:
    HookManager() = default;

    // Not copyable (owns unique_ptr hooks); default construction only.
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    ~HookManager();

    // Takes ownership of the hook. Returns false when the hook is null or
    // its name is already taken; the rejected hook is destroyed with the
    // call.
    bool add(std::unique_ptr<Hook> hook);

    // Installs every hook in registration order. Per-hook failures are
    // logged; returns true only when every hook is installed afterwards.
    bool installAll();

    // Uninstalls every hook in reverse registration order. Idempotent.
    bool uninstallAll();

    // Logs the diagnostics block:
    //   [OK] Name
    //   N / M hooks installed
    void logDiagnostics() const;

    [[nodiscard]] std::size_t installedCount() const;
    [[nodiscard]] std::size_t count() const noexcept { return hooks_.size(); }

    // Uninstalls everything and drops ownership.
    void clear();

    [[nodiscard]] const Hook* find(std::string_view name) const;
    [[nodiscard]] Hook* find(std::string_view name);

private:
    std::vector<std::unique_ptr<Hook>> hooks_;
};

}  // namespace yuzora::hooks
