#include "memory/RttiScanner.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>

#include "core/Logger.hpp"

namespace yuzora::memory {

namespace {

// Section memory ranges of a module, with read/access classification.
struct SectionRange {
    std::uintptr_t start = 0;
    std::size_t size = 0;
    bool readable = false;
    bool writable = false;
};

std::vector<SectionRange> moduleSections(const ModuleInfo& module) {
    std::vector<SectionRange> sections;
    const auto* dosHeader =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return sections;
    }
    const auto* ntHeaders =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(module.base +
                                                  dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return sections;
    }

    const auto* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (UINT i = 0; i < ntHeaders->FileHeader.NumberOfSections;
         ++i, ++section) {
        SectionRange range;
        range.start = module.base + section->VirtualAddress;
        range.size = section->Misc.VirtualSize;
        range.readable = true;
        range.writable =
            (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        sections.push_back(range);
    }
    return sections;
}

bool safeRead(std::uintptr_t address, void* buffer, std::size_t size) {
    // Same-process reads of mapped image/data memory are safe here because
    // callers pass section ranges; kept as a helper for clarity.
    std::memcpy(buffer, reinterpret_cast<const void*>(address), size);
    return true;
}

}  // namespace

std::uintptr_t RttiScanner::findTypeDescriptor(const ModuleInfo& module,
                                               const std::string& className) {
    // MSVC 64-bit TypeDescriptor: { void* pVFTable; void* spare; char name[]; }
    // The name is the decorated form ".?AVClientInstance@@". Search every
    // section for the name, then validate the struct backwards.
    std::string decorated = className;
    if (decorated.rfind(".?", 0) != 0) {
        decorated = ".?" + decorated;
    }

    const auto sections = moduleSections(module);
    const std::size_t nameLength = decorated.size() + 1;  // NUL included

    for (const SectionRange& range : sections) {
        if (!range.readable || range.size < nameLength + 16) {
            continue;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(range.start);
        const std::size_t limit = range.size - nameLength;

        for (std::size_t offset = 0; offset <= limit; ++offset) {
            if (bytes[offset] != '.') {
                // Fast skip: every type descriptor name starts with '.'.
                while (offset <= limit && bytes[offset] != '.') {
                    ++offset;
                }
                if (offset > limit) {
                    break;
                }
            }
            if (std::memcmp(bytes + offset, decorated.data(),
                            decorated.size()) != 0) {
                continue;
            }
            // Validate: pVFTable must point into this module's rdata.
            const std::uintptr_t descriptor = range.start + offset - 16;
            const auto vfPtr = *reinterpret_cast<std::uintptr_t*>(descriptor);
            if (vfPtr >= module.base && vfPtr < module.base + module.size) {
                return descriptor;
            }
        }
    }
    return 0;
}

std::vector<RttiScanner::VftableInfo> RttiScanner::findVftables(
    const ModuleInfo& module, std::uintptr_t typeDescriptor) {
    std::vector<VftableInfo> vftables;
    if (typeDescriptor == 0) {
        return vftables;
    }

    const std::uint32_t descriptorRva =
        static_cast<std::uint32_t>(typeDescriptor - module.base);
    const auto sections = moduleSections(module);

    // Stage 1: find CompleteObjectLocators - structs whose DWORD at offset
    // 0x0C equals the TypeDescriptor's RVA.
    std::vector<std::uintptr_t> locators;
    for (const SectionRange& range : sections) {
        if (!range.readable || range.writable || range.size < 0x18) {
            continue;  // COLs live in read-only rdata
        }
        const auto* dwords =
            reinterpret_cast<const std::uint32_t*>(range.start);
        const std::size_t dwordCount = range.size / 4;
        for (std::size_t i = 0; i + 6 <= dwordCount; ++i) {
            if (dwords[i + 3] == descriptorRva) {
                locators.push_back(range.start + i * 4);
            }
        }
    }

    // Stage 2: vftables - qwords in read-only data that point at a locator;
    // the vftable starts right after that pointer slot.
    for (const std::uintptr_t locator : locators) {
        for (const SectionRange& range : sections) {
            if (!range.readable || range.writable) {
                continue;
            }
            const auto* qwords =
                reinterpret_cast<const std::uintptr_t*>(range.start);
            const std::size_t qwordCount = range.size / 8;
            for (std::size_t i = 0; i < qwordCount; ++i) {
                if (qwords[i] == locator) {
                    vftables.push_back({range.start + (i + 1) * 8, locator});
                    break;
                }
            }
        }
    }

    // Deduplicate (the locator scan can find the same COL from overlapping
    // section views).
    std::sort(vftables.begin(), vftables.end(),
              [](const VftableInfo& a, const VftableInfo& b) {
                  return a.vftableAddress < b.vftableAddress;
              });
    vftables.erase(std::unique(vftables.begin(), vftables.end(),
                               [](const VftableInfo& a, const VftableInfo& b) {
                                   return a.vftableAddress ==
                                          b.vftableAddress;
                               }),
                   vftables.end());
    return vftables;
}

std::vector<std::uintptr_t> RttiScanner::findInstances(
    const ModuleInfo& module, std::uintptr_t vftableAddress,
    std::size_t objectScanBytes) {
    std::vector<std::uintptr_t> instances;
    const auto sections = moduleSections(module);

    for (const SectionRange& range : sections) {
        if (!range.readable || !range.writable || range.size < 8) {
            continue;  // singletons live in writable data
        }
        const auto* qwords = reinterpret_cast<const std::uintptr_t*>(range.start);
        const std::size_t qwordCount =
            std::min(range.size / 8, objectScanBytes / 8);
        for (std::size_t i = 0; i < qwordCount; ++i) {
            if (qwords[i] == vftableAddress) {
                instances.push_back(range.start + i * 8);
            }
        }
    }
    return instances;
}

std::uintptr_t RttiScanner::findMemberWithVftable(
    std::uintptr_t object, std::uintptr_t vftableAddress,
    std::size_t scanBytes) {
    // Candidate qwords inside an object can be ANY value (floats, sizes),
    // so every dereference must be guarded by a readability check.
    const auto readable = [](std::uintptr_t address, [[maybe_unused]] std::size_t size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi,
                         sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD protect = mbi.Protect & ~PAGE_GUARD;
        return protect == PAGE_READONLY || protect == PAGE_READWRITE ||
               protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE;
    };

    const auto* qwords = reinterpret_cast<const std::uintptr_t*>(object);
    const std::size_t count = scanBytes / 8;
    for (std::size_t i = 1; i < count; ++i) {  // skip offset 0: the vftable
        const std::uintptr_t candidate = qwords[i];
        if (candidate < 0x10000 || !readable(candidate, 8)) {
            continue;
        }
        // The target must start with the wanted vftable pointer.
        if (*reinterpret_cast<std::uintptr_t*>(candidate) ==
            vftableAddress) {
            return candidate;
        }
    }
    return 0;
}

}  // namespace yuzora::memory
