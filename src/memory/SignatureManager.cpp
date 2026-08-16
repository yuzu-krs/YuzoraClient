#include "memory/SignatureManager.hpp"

#include <unordered_map>

#include "core/Logger.hpp"
#include "memory/Memory.hpp"

namespace yuzora::memory {

void SignatureManager::setDefaultModule(std::wstring_view name) {
    defaultModule_ = std::wstring{name};
}

bool SignatureManager::add(std::string name, std::string pattern, std::wstring moduleName) {
    if (name.empty() || pattern.empty() || find(name) != nullptr) {
        return false;
    }
    if (moduleName.empty()) {
        moduleName = defaultModule_;
    }
    signatures_.emplace_back(std::move(name), std::move(pattern), std::move(moduleName));
    return true;
}

void SignatureManager::scanAll() {
    // Resolve each distinct target module once per scan. try_emplace alone
    // would still evaluate getModule() eagerly, so fill the entry lazily.
    std::unordered_map<std::wstring, std::optional<ModuleInfo>> moduleCache;

    for (Signature& signature : signatures_) {
        const std::wstring& moduleName = signature.moduleName();
        auto [entry, inserted] = moduleCache.try_emplace(moduleName);
        if (inserted) {
            entry->second = getModule(moduleName);
        }
        const std::optional<ModuleInfo>& module = entry->second;

        if (!module.has_value() || module->base == 0) {
            signature.markModuleUnavailable();
            continue;
        }

        signature.scan(module->base, module->size);
    }

    Logger::info("Signature Scan");
    for (const Signature& signature : signatures_) {
        if (signature.state() == SignatureState::Found) {
            Logger::info("[OK] {} -> {}", signature.name(), signature.describeState());
        } else {
            Logger::info("[!!] {} - {}", signature.name(), signature.describeState());
        }
    }
    Logger::info("{} / {} signatures resolved", resolvedCount(), signatures_.size());
}

void SignatureManager::clear() {
    signatures_.clear();
    // defaultModule_ is kept: it describes the environment, not the registry.
}

std::optional<std::uintptr_t> SignatureManager::get(std::string_view name) const {
    const Signature* signature = find(name);
    if (signature == nullptr || signature->state() != SignatureState::Found) {
        return std::nullopt;
    }
    return signature->address();
}

const Signature* SignatureManager::find(std::string_view name) const {
    for (const Signature& signature : signatures_) {
        if (signature.name() == name) {
            return &signature;
        }
    }
    return nullptr;
}

std::size_t SignatureManager::resolvedCount() const noexcept {
    std::size_t resolved = 0;
    for (const Signature& signature : signatures_) {
        if (signature.state() == SignatureState::Found) {
            ++resolved;
        }
    }
    return resolved;
}

}  // namespace yuzora::memory
