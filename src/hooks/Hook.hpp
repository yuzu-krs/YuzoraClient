#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace yuzora::hooks {

// One inline hook: the first bytes of `target` are replaced with an absolute
// jump to `handler`, and a trampoline is built that executes the displaced
// prologue and jumps back, so handlers can call the original function
// through trampoline().
//
// Constraints on the displaced prologue (caller's responsibility - checking
// it needs a full length disassembler, which is deliberately out of scope):
//   - at least `displacedBytes` whole instructions before the first branch,
//     RIP-relative instruction or relocation target
//   - displacedBytes >= 14 (the size of the absolute jump patch)
//
// Known limitation, acceptable for v0.3: uninstall() frees the trampoline
// immediately. Another thread executing inside the trampoline at that
// moment would crash; making that safe needs thread suspension or deferred
// freeing, which can be added when real game hooks exist.
class Hook {
public:
    // `displacedBytes`: how many bytes of the target prologue are moved into
    // the trampoline. Must satisfy the constraints above.
    Hook(std::string name, void* target, void* handler,
         std::size_t displacedBytes = 16);

    // The hook owns a live patch; copying or moving it would alias it.
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook(Hook&&) = delete;
    Hook& operator=(Hook&&) = delete;

    ~Hook();

    // Installs the hook. Idempotent: returns true when the hook is
    // installed afterwards (a second call on an installed hook succeeds
    // without patching anything new).
    bool install();

    // Restores the original bytes and frees the trampoline. Idempotent:
    // returns true when the hook is not installed afterwards.
    bool uninstall();

    [[nodiscard]] bool isInstalled() const;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] void* target() const noexcept { return target_; }
    [[nodiscard]] void* handler() const noexcept { return handler_; }

    // Address to CALL to invoke the original function. Only valid while the
    // hook is installed; null otherwise.
    [[nodiscard]] void* trampoline() const noexcept { return trampoline_; }

    // Size of the entry patch written over the target prologue
    // ("mov rax, imm64 ; jmp rax").
    static constexpr std::size_t kPatchSize = 12;

private:
    void releaseTrampoline() noexcept;

    std::string name_;
    void* target_ = nullptr;
    void* handler_ = nullptr;
    std::size_t displacedBytes_ = 0;

    mutable std::mutex mutex_;
    void* trampoline_ = nullptr;                 // executable buffer
    std::vector<std::uint8_t> originalBytes_ {};  // displaced bytes backup
    bool installed_ = false;
};

}  // namespace yuzora::hooks
