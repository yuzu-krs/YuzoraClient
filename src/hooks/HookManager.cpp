#include "hooks/HookManager.hpp"

#include "core/Logger.hpp"

namespace yuzora::hooks {

HookManager::~HookManager() {
    // Safety net; the Client uninstalls explicitly first.
    uninstallAll();
}

bool HookManager::add(std::unique_ptr<Hook> hook) {
    if (hook == nullptr) {
        return false;
    }
    if (find(hook->name()) != nullptr) {
        Logger::error("a hook named '{}' is already registered", hook->name());
        return false;
    }
    hooks_.push_back(std::move(hook));
    return true;
}

bool HookManager::installAll() {
    bool all = true;
    for (const std::unique_ptr<Hook>& hook : hooks_) {
        if (!hook->install()) {
            all = false;
        }
    }
    return all;
}

bool HookManager::uninstallAll() {
    bool all = true;
    for (auto it = hooks_.rbegin(); it != hooks_.rend(); ++it) {
        if (!(**it).uninstall()) {
            all = false;
        }
    }
    return all;
}

void HookManager::logDiagnostics() const {
    Logger::info("Hook diagnostics");
    for (const std::unique_ptr<Hook>& hook : hooks_) {
        if (hook->isInstalled()) {
            Logger::info("[OK] {}", hook->name());
        } else {
            Logger::info("[!!] {} - not installed", hook->name());
        }
    }
    Logger::info("{} / {} hooks installed", installedCount(), hooks_.size());
}

std::size_t HookManager::installedCount() const {
    std::size_t installed = 0;
    for (const std::unique_ptr<Hook>& hook : hooks_) {
        if (hook->isInstalled()) {
            ++installed;
        }
    }
    return installed;
}

void HookManager::clear() {
    uninstallAll();
    hooks_.clear();
}

const Hook* HookManager::find(std::string_view name) const {
    for (const std::unique_ptr<Hook>& hook : hooks_) {
        if (hook->name() == name) {
            return hook.get();
        }
    }
    return nullptr;
}

Hook* HookManager::find(std::string_view name) {
    for (const std::unique_ptr<Hook>& hook : hooks_) {
        if (hook->name() == name) {
            return hook.get();
        }
    }
    return nullptr;
}

}  // namespace yuzora::hooks
