#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "sdk/math/Vec3.hpp"

namespace yuzora::sdk {

// Discovers the local player's world position address at runtime by
// heap-delta scanning, no offsets or signatures required:
//
// 1. Snapshot every writable private memory region (the game heap).
// 2. A moment later, snapshot again: any 12-byte window holding three
//    finite floats that look like world coordinates AND moved between the
//    two snapshots is a position candidate.
// 3. Repeat the diff a few times while the player moves: the candidate set
//    shrinks until only the real position (and rare coincidences) remain.
//
// The result is a raw address whose 12 bytes track the player's world
// position. Also identifies the containing object's vftable by backtracking,
// which pins the LocalPlayer object for later milestones (entities,
// rendering).
class PositionDiscovery {
public:
    // Starts the discovery thread (game mode only). The player must be in a
    // world and moving for the scan to converge. gameModuleBase is used to
    // persist the discovered offsets for instant reuse in later sessions.
    bool start(std::uintptr_t gameModuleBase);

    // Stops the thread.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    // The discovered position. Valid once `found()` is true; the thread
    // keeps reading the live address.
    [[nodiscard]] bool getPosition(Vec3& out) const;

    // One-line status for overlays ("scan pass 2/10, 1423 candidates" /
    // "position locked" / "idle").
    [[nodiscard]] std::string statusText() const;

    // Backtracked containing-object info (valid after discovery).
    [[nodiscard]] std::uintptr_t positionAddress() const noexcept {
        return positionAddress_.load();
    }
    [[nodiscard]] std::uintptr_t objectAddress() const noexcept {
        return objectAddress_.load();
    }
    [[nodiscard]] std::uintptr_t vftableAddress() const noexcept {
        return vftableAddress_.load();
    }
    [[nodiscard]] bool found() const noexcept { return found_.load(); }

private:
    void threadProc();

    std::uintptr_t gameModuleBase_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> found_{false};
    std::atomic<std::uintptr_t> positionAddress_{0};
    std::atomic<std::uintptr_t> objectAddress_{0};
    std::atomic<std::uintptr_t> vftableAddress_{0};
    std::atomic<int> pass_{0};
    std::atomic<std::size_t> candidates_{0};
    std::thread thread_;
};

}  // namespace yuzora::sdk
