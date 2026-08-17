#include "events/EventBus.hpp"

#include <mutex>
#include <vector>

namespace yuzora::events::detail {

namespace {

std::mutex& registryMutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

// Cleanup callbacks, one per event type that ever got a subscriber.
std::vector<void (*)()>& cleanups() noexcept {
    static std::vector<void (*)()> functions;
    return functions;
}

}  // namespace

void registerChannelCleanup(void (*cleanup)()) {
    const std::scoped_lock lock{registryMutex()};
    cleanups().push_back(cleanup);
}

void clearAllChannels() {
    std::vector<void (*)()> pending;
    {
        const std::scoped_lock lock{registryMutex()};
        pending = cleanups();
    }

    for (void (*cleanup)() : pending) {
        if (cleanup != nullptr) {
            cleanup();
        }
    }
}

}  // namespace yuzora::events::detail
