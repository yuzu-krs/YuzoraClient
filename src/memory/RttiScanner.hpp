#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "memory/Memory.hpp"

namespace yuzora::memory {

// MSVC x64 RTTI resolution for in-process modules.
//
// Chain: class name string (".?AVClientInstance@@") -> TypeDescriptor ->
// CompleteObjectLocator (COL, holds a 32-bit RVA of the TypeDescriptor) ->
// vftable (the qword before the pointer to the COL) -> object instances
// (pointers to the vftable found in the module's data sections).
//
// This survives game updates far better than byte-pattern signatures: the
// class NAME is stable across versions even when everything around it moves.
class RttiScanner {
public:
    struct VftableInfo {
        std::uintptr_t vftableAddress = 0;  // address of the vftable itself
        std::uintptr_t completeObjectLocator = 0;
    };

    // Finds the TypeDescriptor for a class name (e.g. ".?AVClientInstance@@"
    // - pass the name with or without the leading dot). Returns 0 when not
    // found.
    [[nodiscard]] static std::uintptr_t findTypeDescriptor(
        const ModuleInfo& module, const std::string& className);

    // Finds every vftable whose CompleteObjectLocator references this
    // TypeDescriptor.
    [[nodiscard]] static std::vector<VftableInfo> findVftables(
        const ModuleInfo& module, std::uintptr_t typeDescriptor);

    // Finds objects in the module's data sections whose first qword points
    // at this vftable (singletons like ClientInstance live there).
    [[nodiscard]] static std::vector<std::uintptr_t> findInstances(
        const ModuleInfo& module, std::uintptr_t vftableAddress,
        std::size_t objectScanBytes = 0x400000);

    // From an object address, finds the first member pointer (within
    // scanBytes) whose target object starts with the given vftable - used to
    // chase pointers like ClientInstance -> LocalPlayer without offsets.
    [[nodiscard]] static std::uintptr_t findMemberWithVftable(
        std::uintptr_t object, std::uintptr_t vftableAddress,
        std::size_t scanBytes);
};

}  // namespace yuzora::memory
