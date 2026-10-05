#include "sdk/PositionDiscovery.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include "core/Logger.hpp"

namespace yuzora::sdk {

namespace {

constexpr std::size_t kGroupBytes = 32 * 1024 * 1024;  // per-group snapshot cap
constexpr int kMaxPasses = 16;                          // 8 walk/stop cycles
constexpr int kWorkerThreads = 6;
constexpr UINT kSettleMs = 1500;  // A/B window: clear movement or stillness

struct RegionRange {
    std::uintptr_t base = 0;
    std::size_t size = 0;
};

struct Slice {
    std::uintptr_t base = 0;
    std::size_t size = 0;
};

// A scan group: slices totaling <= kGroupBytes, snapshotted with one sleep.
struct ScanGroup {
    std::vector<Slice> slices;
    std::size_t totalSize = 0;
};

bool isWritablePrivate(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) {
        return false;
    }
    const DWORD protect = mbi.Protect & ~PAGE_GUARD;
    return protect == PAGE_READWRITE;
}

bool isReadableRange(std::uintptr_t address, [[maybe_unused]] std::size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protect = mbi.Protect & ~PAGE_GUARD;
    return protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE;
}

// SEH-guarded copy: the game frees/decommits memory concurrently, so a
// validated range can fault mid-copy. Free function - no C++ unwinding.
bool safeCopy(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
    const float moved = dx + dy + dz;
    return moved > 0.005f && moved < 400.f && dx < 100.f && dy < 100.f &&
           dz < 100.f;
}

bool stayedStill(const SnapshotValue& a, const SnapshotValue& b) {
    const float dx = std::abs(b.x - a.x);
    const float dy = std::abs(b.y - a.y);
    const float dz = std::abs(b.z - a.z);
    return dx < 0.0005f && dy < 0.0005f && dz < 0.0005f;
}

SnapshotValue readTriple(std::uintptr_t address) {
    SnapshotValue v{};
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v;
}

std::mutex g_candidatesMutex;

// Packs writable private memory into scan groups (slices of big regions,
// small regions batched together).
std::vector<ScanGroup> buildScanGroups() {
    std::vector<RegionRange> regions;
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) !=
           0) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            mbi.RegionSize >= 0x1000 && isWritablePrivate(
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress))) {
            regions.push_back(
                {reinterpret_cast<std::uintptr_t>(mbi.BaseAddress),
                 mbi.RegionSize});
        }
        address = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                  mbi.RegionSize;
    }

    std::vector<ScanGroup> groups;
    ScanGroup current;
    for (const RegionRange& region : regions) {
        std::uintptr_t offset = 0;
        while (offset < region.size) {
            const std::size_t take =
                (std::min)(kGroupBytes - current.totalSize, region.size - offset);
            current.slices.push_back({region.base + offset, take});
            current.totalSize += take;
            offset += take;
            if (current.totalSize >= kGroupBytes) {
                groups.push_back(std::move(current));
                current = {};
            }
        }
    }
    if (current.totalSize > 0) {
        groups.push_back(std::move(current));
    }
    return groups;
}

}  // namespace

std::string PositionDiscovery::statusText() const {
    if (found_.load()) {
        return "ESP: position locked";
    }
    const int pass = pass_.load();
    if (pass == 0) {
        return "ESP scan: enter a world";
    }
    // Odd passes: walk. Even passes: stand still.
    const bool walk = (pass % 2) != 0;
    return (walk ? "ESP: WALK now! (pass " : "ESP: STAND STILL (pass ") +
           std::to_string(pass) + "/" + std::to_string(kMaxPasses) + "), " +
           std::to_string(candidates_.load()) + " candidates";
}

bool PositionDiscovery::start() {
    if (running_.exchange(true)) {
        return true;
    }
    found_.store(false);
    positionAddress_.store(0);
    objectAddress_.store(0);
    vftableAddress_.store(0);
    pass_.store(0);
    candidates_.store(0);
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

void PositionDiscovery::threadProc() {
    Logger::info("[esp] position discovery started - enter a world, then "
                 "follow the WALK / STAND STILL prompts on the overlay");

    // Declared outside the try so the result extraction after the catch
    // handlers can read it.
    std::unordered_map<std::uintptr_t, SnapshotValue> candidates;
    std::mutex candidatesMutex;
    bool firstPass = true;

    try {
        const auto groups = buildScanGroups();
        std::size_t totalBytes = 0;
        for (const auto& group : groups) {
            totalBytes += group.totalSize;
        }
        Logger::info("[esp] {} scan group(s), {:.1f} MB total", groups.size(),
                     static_cast<double>(totalBytes) / (1024.0 * 1024.0));
        if (groups.empty()) {
            return;
        }

        std::vector<std::uint8_t> snapA;
        std::vector<std::uint8_t> snapB;
        std::vector<std::uint8_t> snapC;

        // Alternating WALK / STAND STILL cycles. A real player position
        // moves while walking, KEEPS THE SAME DIRECTION across consecutive
        // windows (straight-line walking), and freezes EXACTLY while
        // standing still. Animations, oscillators and timers fail the
        // direction-consistency or freeze test and are eliminated.
        for (int pass = 1; pass <= kMaxPasses && running_.load(); ++pass) {
            pass_.store(pass);
            const bool walkPhase = (pass % 2) != 0;
            std::atomic<std::size_t> nextGroup{0};

            const auto worker = [&](int) {
                std::size_t index = 0;
                while (running_.load()) {
                    index = nextGroup.fetch_add(1);
                    if (index >= groups.size()) {
                        break;
                    }
                    const ScanGroup& group = groups[index];

                    std::vector<std::uint8_t> snapA(group.totalSize);
                    std::vector<std::uint8_t> snapB(group.totalSize);
                    std::vector<std::uint8_t> snapC(walkPhase ? group.totalSize : 0);

                    std::size_t copied = 0;
                    bool ok = true;
                    for (const Slice& slice : group.slices) {
                        if (!isReadableRange(slice.base, slice.size)) {
                            ok = false;
                            break;
                        }
                        if (!safeCopy(snapA.data() + copied,
                                      reinterpret_cast<const void*>(slice.base),
                                      slice.size)) {
                            ok = false;
                            break;
                        }
                        copied += slice.size;
                    }
                    if (!ok) {
                        continue;
                    }

                    Sleep(kSettleMs);

                    copied = 0;
                    ok = true;
                    for (const Slice& slice : group.slices) {
                        if (!isReadableRange(slice.base, slice.size)) {
                            ok = false;
                            break;
                        }
                        if (!safeCopy(snapB.data() + copied,
                                      reinterpret_cast<const void*>(slice.base),
                                      slice.size)) {
                            ok = false;
                            break;
                        }
                        copied += slice.size;
                    }
                    if (!ok) {
                        continue;
                    }

                    // Walk cycles take a third window for direction
                    // consistency; still cycles compare A/B only.
                    if (walkPhase) {
                        Sleep(kSettleMs);
                        copied = 0;
                        ok = true;
                        for (const Slice& slice : group.slices) {
                            if (!isReadableRange(slice.base, slice.size)) {
                                ok = false;
                                break;
                            }
                            if (!safeCopy(snapC.data() + copied,
                                          reinterpret_cast<const void*>(slice.base),
                                          slice.size)) {
                                ok = false;
                                break;
                            }
                            copied += slice.size;
                        }
                        if (!ok) {
                            continue;
                        }
                    }

                    // Diff every slice in the group.
                    std::size_t offset = 0;
                    for (const Slice& slice : group.slices) {
                        const std::size_t floatSlots = slice.size / 4;
                        const auto* fa = reinterpret_cast<const float*>(
                            snapA.data() + offset);
                        const auto* fb = reinterpret_cast<const float*>(
                            snapB.data() + offset);
                        for (std::size_t f = 0; f + 2 < floatSlots; ++f) {
                            const SnapshotValue a{fa[f], fa[f + 1], fa[f + 2]};
                            if (!plausibleCoords(a)) {
                                continue;
                            }
                            const SnapshotValue b{fb[f], fb[f + 1], fb[f + 2]};
                            if (!plausibleCoords(b)) {
                                continue;
                            }
                            const std::uintptr_t address =
                                slice.base + offset + f * 4;
                            const std::scoped_lock lock{g_candidatesMutex};
                            const bool moved = movedPlausibly(a, b);
                            bool keep;
                            if (walkPhase) {
                                keep = moved;
                                if (keep && !snapC.empty()) {
                                    // Direction consistency: A->B and B->C
                                    // must not flip sign on any axis.
                                    const auto* fc = reinterpret_cast<const float*>(
                                        snapC.data() + offset);
                                    const SnapshotValue c{fc[f], fc[f + 1],
                                                          fc[f + 2]};
                                    keep = ((b.x - a.x) * (c.x - b.x) >= 0.f) &&
                                           ((b.y - a.y) * (c.y - b.y) >= 0.f) &&
                                           ((b.z - a.z) * (c.z - b.z) >= 0.f);
                                }
                            } else {
                                keep = stayedStill(a, b);
                            }
                            if (!keep) {
                                continue;
                            }
                            if (firstPass) {
                                candidates.emplace(address, b);
                                continue;
                            }
                            // Narrowing: candidates failing the phase filter
                            // are eliminated (this erase IS the algorithm).
                            const auto it = candidates.find(address);
                            if (it == candidates.end()) {
                                continue;
                            }
                            const bool stillOk = stayedStill(a, b);
                            if (stillOk) {
                                it->second = b;
                            } else {
                                candidates.erase(it);
                            }
                        }
                        offset += slice.size;
                    }
                }
            };

            std::vector<std::thread> workers;
            for (int w = 0; w < kWorkerThreads; ++w) {
                workers.emplace_back(worker, w);
            }
            for (std::thread& t : workers) {
                t.join();
            }

            firstPass = false;
            candidates_.store(candidates.size());
            Logger::info("[esp] pass {} ({}): {} candidate(s)", pass,
                         walkPhase ? "WALK" : "STILL", candidates.size());

            if (candidates.size() <= 1) {
                break;
            }
            if (candidates.empty()) {
                Logger::error("[esp] no candidates remain - was the player in "
                              "a world following the prompts?");
                return;
            }
        }

        if (candidates.size() > 1) {
            // Not converged: log the survivors for diagnosis instead of
            // locking onto a probable false positive.
            const std::scoped_lock lock{g_candidatesMutex};
            int shown = 0;
            for (const auto& [address, value] : candidates) {
                Logger::info("[esp] candidate 0x{:016X} ({:.2f}, {:.2f}, {:.2f})",
                             address, value.x, value.y, value.z);
                if (++shown >= 8) {
                    break;
                }
            }
            Logger::error("[esp] not converged: {} candidates remain",
                          candidates.size());
            return;
        }

        const auto& entry = *candidates.begin();
        positionAddress_.store(entry.first);
        found_.store(true);
        Logger::info("[esp] position address discovered: 0x{:016X} "
                     "({:.2f}, {:.2f}, {:.2f})",
                     entry.first, entry.second.x, entry.second.y, entry.second.z);

        // Backtrack to the containing object start: scan backwards for a
        // qword that points into the game module's read-only data (a
        // vftable).
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
        if (objectStart != 0) {
            Logger::info("[esp] containing object 0x{:016X} vftable 0x{:016X} "
                         "(delta {} bytes)",
                         objectStart, vftable, entry.first - objectStart);
        }
        Logger::info("[esp] discovery complete - position tracking live");
    } catch (const std::exception& e) {
        Logger::error("[esp] discovery thread exception: {}", e.what());
    } catch (...) {
        Logger::error("[esp] discovery thread unknown exception");
    }
}

}  // namespace yuzora::sdk
