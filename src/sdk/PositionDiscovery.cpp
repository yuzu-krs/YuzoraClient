#include "sdk/PositionDiscovery.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include "core/Logger.hpp"

namespace yuzora::sdk {

namespace {

constexpr std::size_t kSliceSize = 32 * 1024 * 1024;  // 32 MB slices
constexpr int kMaxPasses = 10;

struct RegionRange {
    std::uintptr_t base = 0;
    std::size_t size = 0;
};

bool isReadableWriteable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) {
        return false;
    }
    const DWORD protect = mbi.Protect & ~PAGE_GUARD;
    return protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE;
}

// Writable private regions (heap) of this process, capped per region.
std::vector<RegionRange> collectRegions() {
    std::vector<RegionRange> regions;
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &mbi,
                        sizeof(mbi)) != 0) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            mbi.RegionSize >= 0x1000) {
            const DWORD protect = mbi.Protect & ~PAGE_GUARD;
            if (protect == PAGE_READWRITE) {
                regions.push_back(
                    {reinterpret_cast<std::uintptr_t>(mbi.BaseAddress),
                     mbi.RegionSize});
            }
        }
        address = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                  mbi.RegionSize;
    }
    return regions;
}

struct SnapshotValue {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

bool plausibleCoords(const SnapshotValue& v) {
    const auto finite = [](float f) {
        return std::isfinite(f) && f == f;
    };
    return finite(v.x) && finite(v.y) && finite(v.z) &&
           std::abs(v.y) <= 400.f && std::abs(v.x) <= 100000.f &&
           std::abs(v.z) <= 100000.f;
}

bool movedPlausibly(const SnapshotValue& a, const SnapshotValue& b) {
    const float dx = std::abs(b.x - a.x);
    const float dy = std::abs(b.y - a.y);
    const float dz = std::abs(b.z - a.z);
    // A walking player moves ~0.05-2 per axis in 300 ms; the camera and
    // physics also jitter values slightly. Anything teleported or static is
    // filtered.
    const float moved = dx + dy + dz;
    return moved > 0.005f && moved < 400.f && dx < 100.f && dy < 100.f &&
           dz < 100.f;
}

// SEH-guarded copy: the game frees/decommits memory concurrently, so a
// region validated moments ago can fault mid-copy. Must stay a free
// function without C++ unwinding objects.
bool safeCopy(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

SnapshotValue readTriple(std::uintptr_t address) {
    SnapshotValue v{};
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v;
}

}  // namespace

void PositionDiscovery::threadProc() {
    Logger::info("[esp] position discovery started - join a world and walk");

    // Declared outside the try so the result extraction below the catch
    // handlers can read it.
    std::unordered_map<std::uintptr_t, SnapshotValue> candidates;

    try {
    const auto regions = collectRegions();
    Logger::info("[esp] scanning {} writable private region(s)",
                 regions.size());

    bool firstPass = true;

    std::vector<std::uint8_t> snapA;
    std::vector<std::uint8_t> snapB;

    for (int pass = 1; pass <= kMaxPasses && running_.load(); ++pass) {
        std::size_t scanned = 0;
        for (const RegionRange& region : regions) {
            if (!running_.load()) {
                return;
            }
            for (std::size_t sliceOffset = 0;
                 sliceOffset < region.size; sliceOffset += kSliceSize) {
                if (!running_.load()) {
                    return;
                }
                const std::uintptr_t sliceBase = region.base + sliceOffset;
                const std::size_t sliceSize =
                    (std::min)(kSliceSize, region.size - sliceOffset);
                if (!isReadableWriteable(sliceBase)) {
                    continue;
                }

                snapA.resize(sliceSize);
                if (!safeCopy(snapA.data(), reinterpret_cast<const void*>(sliceBase),
                              sliceSize)) {
                    continue;  // region freed/decommitted mid-scan
                }
                Sleep(300);
                if (!running_.load()) {
                    return;
                }
                if (!isReadableWriteable(sliceBase)) {
                    continue;
                }
                snapB.resize(sliceSize);
                if (!safeCopy(snapB.data(), reinterpret_cast<const void*>(sliceBase),
                              sliceSize)) {
                    continue;
                }
                scanned += sliceSize;

                const std::size_t floatSlots = sliceSize / 4;
                const auto* fa = reinterpret_cast<const float*>(snapA.data());
                const auto* fb = reinterpret_cast<const float*>(snapB.data());

                for (std::size_t f = 0; f + 2 < floatSlots; ++f) {
                    const SnapshotValue a{fa[f], fa[f + 1], fa[f + 2]};
                    if (!plausibleCoords(a)) {
                        continue;
                    }
                    const SnapshotValue b{fb[f], fb[f + 1], fb[f + 2]};
                    if (!plausibleCoords(b) || !movedPlausibly(a, b)) {
                        continue;
                    }
                    const std::uintptr_t address = sliceBase + f * 4;
                    if (firstPass) {
                        candidates.emplace(address, b);
                    } else {
                        const auto it = candidates.find(address);
                        if (it != candidates.end()) {
                            if (movedPlausibly(it->second, b)) {
                                it->second = b;
                            } else {
                                candidates.erase(it);
                            }
                        }
                    }
                }
            }
        }
        firstPass = false;

        Logger::info("[esp] pass {} done: scanned {:.1f} MB, {} candidate(s)",
                     pass, static_cast<double>(scanned) / (1024.0 * 1024.0),
                     candidates.size());

        if (candidates.size() == 1) {
            break;
        }
        if (candidates.empty()) {
            Logger::error("[esp] no candidates remain - is the player in a "
                          "world and moving?");
            return;
        }
        // Give the player time to move before the next narrowing pass.
        for (int wait = 0; wait < 30 && running_.load(); ++wait) {
            Sleep(100);
        }
    }
    } catch (const std::exception& e) {
        Logger::error("[esp] discovery thread exception: {}", e.what());
    } catch (...) {
        Logger::error("[esp] discovery thread unknown exception");
    }

    if (candidates.empty()) {
        Logger::info("[esp] discovery ended with no result");
        return;
    }

    const auto& entry = *candidates.begin();
    positionAddress_.store(entry.first);
    found_.store(true);
    Logger::info("[esp] position address discovered: 0x{:016X} "
                 "({:.2f}, {:.2f}, {:.2f})",
                 entry.first, entry.second.x, entry.second.y, entry.second.z);

    // Backtrack to the containing object start: scan backwards for a qword
    // that points into the game module's read-only data (a vftable).
    const auto readable = [](std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi,
                         sizeof(mbi)) == 0) {
            return false;
        }
        return mbi.State == MEM_COMMIT;
    };
    std::uintptr_t objectStart = 0;
    std::uintptr_t vftable = 0;
    for (std::size_t back = 8; back <= 0x2000; back += 8) {
        const std::uintptr_t candidateAddress = entry.first - back;
        if (!readable(candidateAddress)) {
            break;
        }
        const auto value = *reinterpret_cast<std::uintptr_t*>(candidateAddress);
        if (value > 0x7FF000000000ULL || value < 0x10000) {
            continue;
        }
        if (!readable(value)) {
            continue;
        }
        // A vftable's first entry is a code pointer into the module.
        const auto firstEntry = *reinterpret_cast<std::uintptr_t*>(value);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(value), &mbi, sizeof(mbi)) ==
                0 ||
            mbi.Type == MEM_PRIVATE) {
            continue;
        }
        if (firstEntry > 0x7FF000000000ULL && readable(firstEntry)) {
            objectStart = candidateAddress;
            vftable = value;
            break;
        }
    }
    objectAddress_.store(objectStart);
    vftableAddress_.store(vftable);
    Logger::info("[esp] containing object 0x{:016X} vftable 0x{:016X} "
                 "(delta {} bytes)",
                 objectStart, vftable, entry.first - objectStart);
    Logger::info("[esp] discovery complete - position tracking live");
}

bool PositionDiscovery::start() {
    if (running_.exchange(true)) {
        return true;
    }
    found_.store(false);
    positionAddress_.store(0);
    objectAddress_.store(0);
    vftableAddress_.store(0);
    thread_ = std::thread(&PositionDiscovery::threadProc, this);
    return true;
}

void PositionDiscovery::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool PositionDiscovery::getPosition(Vec3& out) const {
    const std::uintptr_t address = positionAddress_.load();
    if (!found_.load() || address == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT) {
        return false;
    }
    const SnapshotValue v = readTriple(address);
    if (!plausibleCoords(v)) {
        return false;
    }
    out = Vec3{v.x, v.y, v.z};
    return true;
}

}  // namespace yuzora::sdk
