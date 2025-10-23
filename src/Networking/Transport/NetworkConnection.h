/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file NetworkConnection.h
 * @brief Base interface for network connections
 *
 * This file contains NetworkConnection, the abstract interface for all connection
 * backends (Unix sockets, named pipes, WebRTC, XPC).
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
 * @brief Abstract base interface for network connections
 *
 * NetworkConnection defines the interface that all connection backends must implement.
 * Derives from EntropyObject for type system integration. Connections are created by
 * ConnectionManager and accessed via ConnectionHandle.
 *
 * Implementations:
 * - UnixSocketConnection (Linux/macOS local IPC)
 * - NamedPipeConnection (Windows local IPC)
 * - WebRTCConnection (all platforms, remote)
 * - XPCConnection (macOS, local IPC)
 *
 * Thread Safety: All methods are thread-safe unless documented otherwise.
 * Callbacks are invoked with reference-counted guards to prevent use-after-free.
 */
class NetworkConnection : public Core::EntropyObject {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>&)>;  ///< Callback for received messages
    using StateCallback = std::function<void(ConnectionState)>;                ///< Callback for state changes

    virtual ~NetworkConnection() = default;

    /**
     * @brief Initiates connection to endpoint
     * @return Result indicating success or failure
     */
    virtual Result<void> connect() = 0;

    /**
     * @brief Disconnects from endpoint
     * @return Result indicating success or failure
     */
    virtual Result<void> disconnect() = 0;

    /**
     * @brief Checks if connection is established
     * @return true if state is Connected
     */
    virtual bool isConnected() const = 0;
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

    /**
     * @brief Non-blocking send with backpressure detection
     * @param data Bytes to send
     * @return Result with WouldBlock error if backpressured, or InvalidParameter if not supported
     */
    virtual Result<void> trySend(const std::vector<uint8_t>& data) {
        (void)data;
        return Result<void>::err(NetworkError::InvalidParameter, "trySend not supported by this backend");
    }

    /**
     * @brief Gets current connection state
     * @return Connection state (Disconnected, Connecting, Connected, etc.)
     */
    virtual ConnectionState getState() const = 0;

    /**
     * @brief Gets connection type (Local or Remote)
     * @return Connection type determined at creation
     */
    virtual ConnectionType getType() const = 0;

    /**
     * @brief Gets connection statistics
     * @return Stats with bytes/messages sent/received
     */
    virtual ConnectionStats getStats() const = 0;

    /**
     * @brief Sets callback for incoming messages
     *
     * Thread-safe: Can be called from any thread.
     * @param callback Function called when messages arrive
     */
    void setMessageCallback(MessageCallback callback) noexcept {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _messageCallback = std::move(callback);
    }

    /**
     * @brief Sets callback for state changes
     *
     * Thread-safe: Can be called from any thread.
     * @param callback Function called when connection state changes
     */
    void setStateCallback(StateCallback callback) noexcept {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _stateCallback = std::move(callback);
    }

protected:
    NetworkConnection() = default;

    /**
     * @brief Invokes message callback with lifetime guards
     *
     * Called by derived classes when messages arrive. Uses RAII guard and shutdown
     * flag to prevent use-after-free during destruction.
     * @param data Received message bytes
     */
    void onMessageReceived(const std::vector<uint8_t>& data) noexcept {
        // Check shutdown flag first (fast path)
        if (_callbacksShutdown.load(std::memory_order_acquire)) {
            return;
        }

        // Increment active callback counter (RAII guard ensures decrement)
        _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
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

    /**
     * @brief Invokes state callback with lifetime guards
     *
     * Called by derived classes when state changes. Uses RAII guard and shutdown
     * flag to prevent use-after-free during destruction.
     * @param state New connection state
     */
    void onStateChanged(ConnectionState state) noexcept {
        // Check shutdown flag first (fast path)
        if (_callbacksShutdown.load(std::memory_order_acquire)) {
            return;
        }

        // Increment active callback counter (RAII guard ensures decrement)
        _activeCallbacks.fetch_add(1, std::memory_order_relaxed);
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

    /**
     * @brief Shuts down callbacks and waits for in-flight invocations
     *
     * Sets shutdown flag and spin-waits for active callbacks to complete.
     * Call this from derived class destructor BEFORE base destructor runs.
     */
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
    mutable std::mutex _cbMutex;                         ///< Protects callback access
    MessageCallback _messageCallback;                    ///< User message callback
    StateCallback _stateCallback;                        ///< User state callback
    std::atomic<int> _activeCallbacks{0};                ///< Count of active callback invocations
    std::atomic<bool> _callbacksShutdown{false};         ///< Shutdown flag for destructor
};

} // namespace EntropyEngine::Networking
