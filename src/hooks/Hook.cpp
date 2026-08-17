#include "hooks/Hook.hpp"

#include <Windows.h>

#include <cstring>

#include "core/Logger.hpp"

namespace yuzora::hooks {

namespace {

// Entry patch: "mov rax, imm64 ; jmp rax" - 12 bytes.
//   48 B8 <8 byte destination> FF E0
// RAX is dead at any function entry (it is the return value register and the
// caller cannot expect it to survive the call), so clobbering it here is
// safe. Used only to redirect the target's first instructions to the
// handler; the argument registers RCX/RDX/R8/R9 are untouched.
bool writeEntryJump(void* where, const void* destination) noexcept;

// Trampoline tail: "push imm32 ; mov dword ptr [rsp+4], imm32 ; ret" -
// 14 bytes. Unlike the RAX-based jump this preserves every register and
// the flags: at the tail the displaced code has already executed and
// volatile registers (RAX in particular, often holding the in-flight
// return value) are still live.
bool writeTailJump(void* where, const void* destination) noexcept;

bool writeBytes(void* where, const void* data, std::size_t size) noexcept {
    // VirtualProtect to writable, copy, restore, flush caches.
    DWORD oldProtect = 0;
    if (VirtualProtect(where, size, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
        Logger::error("VirtualProtect(writable) failed with error {}", GetLastError());
        return false;
    }

    std::memcpy(where, data, size);

    DWORD restored = 0;
    if (VirtualProtect(where, size, oldProtect, &restored) == 0) {
        // The bytes were written, so the write itself succeeded, but the
        // page keeps PAGE_EXECUTE_READWRITE - surface that instead of
        // staying silent.
        Logger::warning("VirtualProtect(restore) failed with error {}; the page "
                        "stays writable+executable",
                        GetLastError());
    }
    FlushInstructionCache(GetCurrentProcess(), where, size);
    return true;
}

bool writeEntryJump(void* where, const void* destination) noexcept {
    std::uint8_t patch[12] = {0x48, 0xB8};
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    std::memcpy(patch + 2, &address, sizeof(address));
    patch[10] = 0xFF;
    patch[11] = 0xE0;
    return writeBytes(where, patch, sizeof(patch));
}

bool writeTailJump(void* where, const void* destination) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    const auto low = static_cast<std::uint32_t>(address & 0xFFFFFFFFu);
    const auto high = static_cast<std::uint32_t>(address >> 32);

    std::uint8_t patch[14];
    patch[0] = 0x68;  // push imm32 (sign-extended to 64)
    std::memcpy(patch + 1, &low, sizeof(low));
    patch[5] = 0xC7;  // mov dword ptr [rsp+4], imm32
    patch[6] = 0x44;
    patch[7] = 0x24;
    patch[8] = 0x04;
    std::memcpy(patch + 9, &high, sizeof(high));
    patch[13] = 0xC3;  // ret
    return writeBytes(where, patch, sizeof(patch));
}

}  // namespace

Hook::Hook(std::string name, void* target, void* handler, std::size_t displacedBytes)
    : name_{std::move(name)}, target_{target}, handler_{handler},
      displacedBytes_{displacedBytes} {}

Hook::~Hook() {
    // Safety net: never let a live patch outlive its Hook object.
    uninstall();
}

bool Hook::install() {
    const std::scoped_lock lock{mutex_};

    if (installed_) {
        return true;  // idempotent
    }
    if (target_ == nullptr || handler_ == nullptr) {
        Logger::error("hook '{}' cannot install: null target or handler", name_);
        return false;
    }
    if (displacedBytes_ < kPatchSize) {
        Logger::error("hook '{}' cannot install: {} displaced bytes are below the "
                      "patch size of {}",
                      name_, displacedBytes_, kPatchSize);
        return false;
    }

    // Trampoline: displaced prologue + register-preserving jump back to
    // target + N.
    trampoline_ = VirtualAlloc(nullptr, displacedBytes_ + 14,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline_ == nullptr) {
        Logger::error("hook '{}' cannot install: trampoline allocation failed with "
                      "error {}",
                      name_, GetLastError());
        return false;
    }
    auto* trampolineBytes = static_cast<std::uint8_t*>(trampoline_);
    std::memcpy(trampolineBytes, target_, displacedBytes_);
    if (!writeTailJump(trampolineBytes + displacedBytes_,
                       static_cast<const std::uint8_t*>(target_) + displacedBytes_)) {
        releaseTrampoline();
        return false;
    }

    // Back up the original prologue, then patch the target.
    const auto* targetBytes = static_cast<const std::uint8_t*>(target_);
    originalBytes_.assign(targetBytes, targetBytes + displacedBytes_);

    if (!writeEntryJump(target_, handler_)) {
        releaseTrampoline();
        originalBytes_.clear();
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), trampoline_, displacedBytes_ + 14);

    installed_ = true;
    Logger::info("hook '{}' installed at 0x{:016X}", name_,
                 reinterpret_cast<std::uintptr_t>(target_));
    return true;
}

bool Hook::uninstall() {
    const std::scoped_lock lock{mutex_};

    if (!installed_) {
        return true;  // idempotent
    }

    if (!writeBytes(target_, originalBytes_.data(), displacedBytes_)) {
        // The patch is still live. Keep installed_, the original-bytes
        // backup and the trampoline so a retry can attempt the restore
        // again; tearing the state down here would leave a live patch with
        // a freed trampoline and no way back.
        Logger::error("hook '{}' failed to restore the original bytes", name_);
        return false;
    }

    originalBytes_.clear();
    releaseTrampoline();
    installed_ = false;

    Logger::info("hook '{}' uninstalled", name_);
    return true;
}

bool Hook::isInstalled() const {
    const std::scoped_lock lock{mutex_};
    return installed_;
}

void Hook::releaseTrampoline() noexcept {
    if (trampoline_ != nullptr) {
        VirtualFree(trampoline_, 0, MEM_RELEASE);
        trampoline_ = nullptr;
    }
}

}  // namespace yuzora::hooks
