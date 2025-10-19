/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <EntropyCore.h>
#include "../Core/NetworkTypes.h"
#include "../Core/ConnectionTypes.h"
#include "../Core/ErrorCodes.h"
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace EntropyEngine::Networking {

/**
 * NetworkConnection - Represents a network connection to a peer
 *
 * Derives from EntropyObject for ref counting. Connections are created
 * by ConnectionManager and stamped with appropriate handles.
 */
class NetworkConnection : public Core::EntropyObject {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>&)>;
    using StateCallback = std::function<void(ConnectionState)>;

    virtual ~NetworkConnection() = default;

    // Connection lifecycle
    virtual Result<void> connect() = 0;
    virtual Result<void> disconnect() = 0;
    virtual bool isConnected() const = 0;

    // Message transmission
    /**
     * @brief Sends data over the reliable channel
     *
     * Thread-Safety: All send operations are serialized through a per-connection
     * mutex. For high-throughput scenarios with large messages, this can become
     * a bottleneck. Consider:
     * - Using sendUnreliable for non-critical data
     * - Batching multiple small messages into larger payloads
     * - Using multiple connections for parallel sends
     *
     * @param data Bytes to send
     * @return Result indicating success or failure reason
     */
    virtual Result<void> send(const std::vector<uint8_t>& data) = 0;

    /**
     * @brief Sends data over the unreliable channel (if available)
     *
     * Falls back to reliable channel if unreliable is not supported.
     * Thread-Safety: Same mutex contention considerations as send().
     *
     * @param data Bytes to send
     * @return Result indicating success or failure reason
     */
    virtual Result<void> sendUnreliable(const std::vector<uint8_t>& data) = 0;

    // Non-blocking send API (default: not supported, returns InvalidParameter)
    virtual Result<void> trySend(const std::vector<uint8_t>& data) {
        (void)data;
        return Result<void>::err(NetworkError::InvalidParameter, "trySend not supported by this backend");
    }

    // State and info
    virtual ConnectionState getState() const = 0;
    virtual ConnectionType getType() const = 0;
    virtual ConnectionStats getStats() const = 0;

    // Callbacks (thread-safe)
    void setMessageCallback(MessageCallback callback) noexcept {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _messageCallback = std::move(callback);
    }
    void setStateCallback(StateCallback callback) noexcept {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _stateCallback = std::move(callback);
    }

protected:
    NetworkConnection() = default;

    void onMessageReceived(const std::vector<uint8_t>& data) noexcept {
        // Check shutdown flag first (fast path)
        if (_callbacksShutdown.load(std::memory_order_acquire)) {
            return;
        }

        // Increment active callback counter (RAII guard ensures decrement)
        _activeCallbacks.fetch_add(1, std::memory_order_acquire);
        struct CallbackGuard {
            std::atomic<int>& counter;
            ~CallbackGuard() { counter.fetch_sub(1, std::memory_order_release); }
        } guard{_activeCallbacks};

        // Double-check shutdown flag after incrementing counter
        if (_callbacksShutdown.load(std::memory_order_acquire)) {
            return; // Bail early if shutting down
        }

        MessageCallback cb;
        {
            std::lock_guard<std::mutex> lock(_cbMutex);
            cb = _messageCallback; // copy under lock
        }

        if (cb) {
            cb(data); // invoke outside lock
        }

        // Counter decrements here via RAII, ensuring destructor waits
        // until we're completely done with the mutex
    }

    void onStateChanged(ConnectionState state) noexcept {
        // Check shutdown flag first (fast path)
        if (_callbacksShutdown.load(std::memory_order_acquire)) {
            return;
        }

        // Increment active callback counter (RAII guard ensures decrement)
        _activeCallbacks.fetch_add(1, std::memory_order_acquire);
        struct CallbackGuard {
            std::atomic<int>& counter;
            ~CallbackGuard() { counter.fetch_sub(1, std::memory_order_release); }
        } guard{_activeCallbacks};

        // Double-check shutdown flag after incrementing counter
        if (_callbacksShutdown.load(std::memory_order_acquire)) {
            return; // Bail early if shutting down
        }

        StateCallback cb;
        {
            std::lock_guard<std::mutex> lock(_cbMutex);
            cb = _stateCallback; // copy under lock
        }

        if (cb) {
            cb(state); // invoke outside lock
        }

        // Counter decrements here via RAII, ensuring destructor waits
        // until we're completely done with the mutex
    }

    // Shutdown callbacks and wait for all active ones to complete
    // Call this from derived class destructor BEFORE base class destructor runs
    void shutdownCallbacks() noexcept {
        // Set shutdown flag to prevent new callbacks from proceeding
        _callbacksShutdown.store(true, std::memory_order_release);

        // Spin-wait for active callbacks to finish (they're short-lived)
        while (_activeCallbacks.load(std::memory_order_acquire) > 0) {
            // Yield to allow callback threads to complete
            std::this_thread::yield();
        }
    }

private:
    mutable std::mutex _cbMutex;
    MessageCallback _messageCallback;
    StateCallback _stateCallback;
    std::atomic<int> _activeCallbacks{0};
    std::atomic<bool> _callbacksShutdown{false};
};

} // namespace EntropyEngine::Networking
