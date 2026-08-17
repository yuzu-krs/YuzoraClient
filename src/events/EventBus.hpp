#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/Logger.hpp"
#include "events/Event.hpp"

namespace yuzora::events {

// Subscription token returned by EventBus::subscribe. 0 is never issued,
// so it doubles as the "no subscription" state.
using SubscriptionId = std::uint64_t;

namespace detail {

// Registry of every channel that ever got a subscriber, so the client can
// clear all subscriptions at shutdown (implemented in EventBus.cpp).
void registerChannelCleanup(void (*cleanup)());
void clearAllChannels();

}  // namespace detail

// Process-wide, statically typed event bus.
//
// One channel exists per event type; subscribe / dispatch / unsubscribe are
// all typed operations with no casts and no runtime type information.
// Channels are thread-safe:
//   - dispatch iterates a snapshot of the slots, so unsubscribing (or
//     subscribing) from inside a subscriber is safe: the change takes effect
//     from the next dispatch on
//   - a subscriber that throws is isolated and reported; other subscribers
//     still run
class EventBus {
public:
    // Registers a callback for event type E. The returned id is needed to
    // unsubscribe; prefer ScopedSubscription for automatic cleanup.
    template <std::derived_from<Event> E>
    static SubscriptionId subscribe(std::function<void(const E&)> callback) {
        return Channel<E>::add(std::move(callback));
    }

    // Removes the subscription with this id. Unknown or already removed ids
    // are ignored.
    template <std::derived_from<Event> E>
    static void unsubscribe(SubscriptionId id) {
        Channel<E>::remove(id);
    }

    // Delivers the event to every current subscriber of exactly this type.
    template <std::derived_from<Event> E>
    static void dispatch(const E& event) {
        Channel<E>::publish(event);
    }

    // Number of live subscriptions for event type E (diagnostics / tests).
    template <std::derived_from<Event> E>
    [[nodiscard]] static std::size_t subscriberCount() {
        return Channel<E>::count();
    }

    // Removes every subscription on every channel that was ever used.
    // Called from Client::shutdown.
    static void clearAllSubscriptions() { detail::clearAllChannels(); }

private:
    template <typename E>
    class Channel {
    public:
        using Callback = std::function<void(const E&)>;

        static SubscriptionId add(Callback&& callback) {
            Storage& storage = instance();

            const std::scoped_lock lock{storage.mutex};
            if (!storage.registered) {
                storage.registered = true;
                detail::registerChannelCleanup(&Channel<E>::clear);
            }
            storage.slots.push_back(
                std::make_shared<Callback>(std::move(callback)));
            return static_cast<SubscriptionId>(storage.slots.size());
        }

        static void remove(SubscriptionId id) {
            Storage& storage = instance();

            const std::scoped_lock lock{storage.mutex};
            if (id == 0 || id > storage.slots.size()) {
                return;
            }
            storage.slots[static_cast<std::size_t>(id - 1)].reset();
        }

        static void publish(const E& event) {
            // Snapshot the live slots so unsubscribing from inside a
            // subscriber cannot invalidate the iteration (shared_ptr keeps
            // the callback alive) and new subscriptions only apply from the
            // next dispatch on.
            std::vector<std::shared_ptr<Callback>> snapshot;
            try {
                Storage& storage = instance();
                const std::scoped_lock lock{storage.mutex};
                snapshot = storage.slots;
            } catch (...) {
                // Dispatch runs on the host's threads through inline hooks;
                // an allocation failure must never unwind into game frames.
                Logger::error("EventBus dispatch could not snapshot the subscribers");
                return;
            }

            for (const auto& slot : snapshot) {
                if (!slot) {
                    continue;
                }
                try {
                    (*slot)(event);
                } catch (...) {
                    Logger::error("EventBus subscriber threw an exception");
                }
            }
        }

        static void clear() {
            Storage& storage = instance();
            const std::scoped_lock lock{storage.mutex};
            storage.slots.clear();
        }

        static std::size_t count() {
            Storage& storage = instance();
            const std::scoped_lock lock{storage.mutex};
            std::size_t live = 0;
            for (const auto& slot : storage.slots) {
                if (slot) {
                    ++live;
                }
            }
            return live;
        }

    private:
        struct Storage {
            std::mutex mutex;
            std::vector<std::shared_ptr<Callback>> slots;
            bool registered = false;
        };

        static Storage& instance() {
            static Storage storage;
            return storage;
        }
    };
};

// RAII owner of one subscription. Unsubscribes on destruction unless
// released; intended as the building block future modules use to tie
// subscriptions to their own lifetime.
template <std::derived_from<Event> E>
class ScopedSubscription {
public:
    ScopedSubscription() = default;
    explicit ScopedSubscription(SubscriptionId id) : id_{id} {}

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ScopedSubscription(ScopedSubscription&& other) noexcept
        : id_{std::exchange(other.id_, kInvalid)} {}

    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
        if (this != &other) {
            reset();
            id_ = std::exchange(other.id_, kInvalid);
        }
        return *this;
    }

    ~ScopedSubscription() { reset(); }

    void reset() {
        if (id_ != kInvalid) {
            EventBus::unsubscribe<E>(id_);
            id_ = kInvalid;
        }
    }

    [[nodiscard]] SubscriptionId get() const noexcept { return id_; }

private:
    static constexpr SubscriptionId kInvalid = 0;

    SubscriptionId id_ = kInvalid;
};

}  // namespace yuzora::events
