/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file NamedPipeConnection.h
 * @brief Windows named pipe implementation for local IPC
 *
 * This file contains NamedPipeConnection, a NetworkConnection implementation
 * using Windows named pipes for high-performance local inter-process
 * communication on Windows.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "NetworkConnection.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace EntropyEngine::Networking
{

/**
 * @brief Windows named pipe implementation for local IPC
 *
 * NamedPipeConnection provides reliable, ordered message delivery using Windows named
 * pipes. Offers significantly lower latency and higher throughput than TCP loopback
 * for local communication on Windows.
 *
 * Features:
 * - Length-prefixed framing (uint32_t little-endian length + payload)
 * - Dedicated receive thread for async message delivery
 * - Configurable connect timeout and send retry behavior
 * - Atomic statistics tracking
 * - Non-blocking send support via trySend()
 * - Automatic pipe name normalization (accepts "name" or "\\\\.\\pipe\\name")
 *
 * Platform support: Windows only (uses Windows.h HANDLE API)
 *
 * Thread Safety: All public methods are thread-safe. Send operations are serialized
 * via mutex. Receive thread runs independently and invokes callbacks.
 *
 * @code
 * // Client-side usage
 * ConnectionConfig cfg;
 * cfg.endpoint = "entropy_pipe";  // Auto-normalized to "\\\\.\\pipe\\entropy_pipe"
 * cfg.connectTimeoutMs = 5000;
 *
 * auto conn = std::make_unique<NamedPipeConnection>(cfg.endpoint, &cfg);
 * conn->setMessageCallback([](const std::vector<uint8_t>& data) {
 *     std::cout << "Received " << data.size() << " bytes\n";
 * });
 *
 * auto result = conn->connect();
 * if (result.success()) {
 *     std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'};
 *     conn->send(msg);
 * }
 *
 * // Server-side usage (after CreateNamedPipe + ConnectNamedPipe)
 * HANDLE clientPipe = CreateNamedPipe("\\\\.\\pipe\\entropy_pipe", ...);
 * ConnectNamedPipe(clientPipe, nullptr);
 * auto clientConn = std::make_unique<NamedPipeConnection>(clientPipe, "client-1");
 * clientConn->setMessageCallback([](const auto& data) { processMessage(data); });
 * // Connection is already established, ready to send/receive
 * @endcode
 */
class NamedPipeConnection : public NetworkConnection
{
public:
    /**
     * @brief Constructs client-side connection to named pipe
     * @param pipeName Pipe name (e.g., "entropy_pipe" or "\\\\.\\pipe\\entropy_pipe")
     */
    explicit NamedPipeConnection(std::string pipeName);

    /**
     * @brief Constructs client-side connection with configuration
     * @param pipeName Pipe name
     * @param cfg Connection configuration (timeouts, max message size, etc.)
     */
    NamedPipeConnection(std::string pipeName, const struct ConnectionConfig* cfg);

#ifdef _WIN32
    /**
     * @brief Constructs server-side connection from connected pipe handle
     *
     * Used by LocalServer to wrap accepted client connections. Pipe is already
     * connected via ConnectNamedPipe(); no need to call connect().
     *
     * @param connectedPipe HANDLE from CreateNamedPipe + ConnectNamedPipe
     * @param peerInfo Identifier for logging/debugging
     */
    NamedPipeConnection(HANDLE connectedPipe, std::string peerInfo);
#endif

    /**
     * @brief Destructor ensures clean shutdown
     *
     * Stops receive thread, shuts down callbacks, closes pipe handle.
     */
    ~NamedPipeConnection() override;

    // NetworkConnection interface
    Result<void> connect() override;
    Result<void> disconnect() override;
    bool isConnected() const override {
        return _state.load() == ConnectionState::Connected;
    }

    Result<void> send(const std::vector<uint8_t>& data) override;
    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
    Result<void> trySend(const std::vector<uint8_t>& data) override;

    ConnectionState getState() const override {
        return _state.load();
    }
    ConnectionType getType() const override {
        return ConnectionType::Local;
    }
    ConnectionStats getStats() const override;

private:
    /**
     * @brief Receive thread main loop
     *
     * Reads length-prefixed messages from pipe and invokes message callback.
     * Runs until _shouldStop is set or pipe is closed.
     */
    void receiveLoop();

    /**
     * @brief Internal send implementation with retry logic
     * @param data Message bytes to send
     * @return Result indicating success or failure
     */
    Result<void> sendInternal(const std::vector<uint8_t>& data);

    /**
     * @brief Converts UTF-8 string to wide string for Windows API
     * @param s UTF-8 string
     * @return Wide string (UTF-16)
     */
    std::wstring toWide(const std::string& s) const;

    /**
     * @brief Normalizes pipe name to full path format
     *
     * Converts "name" to "\\\\.\\pipe\\name" if needed.
     * @param name Pipe name (short or full path)
     * @return Normalized full pipe path
     */
    std::string normalizePipeName(std::string name) const;

    std::string _pipeName;  ///< Pipe name for client connections
#ifdef _WIN32
    HANDLE _pipe{INVALID_HANDLE_VALUE};  ///< Pipe handle
#endif
    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};  ///< Current connection state

    std::thread _receiveThread;            ///< Dedicated receive thread
    std::atomic<bool> _shouldStop{false};  ///< Shutdown signal for receive thread

    mutable std::mutex _sendMutex;  ///< Serializes send operations

    // Configurable parameters (initialized from ConnectionConfig or defaults)
    int _connectTimeoutMs{5000};                        ///< Connect timeout (milliseconds)
    int _sendPollTimeoutMs{100};                        ///< Per-poll timeout during send (ms)
    int _sendMaxPolls{20};                              ///< Max poll iterations before send timeout
    int _recvIdlePollMs{-1};                            ///< Receive poll timeout (-1 = blocking)
    size_t _maxMessageSize{16ull * 1024ull * 1024ull};  ///< Maximum message size (16 MiB)

    // Atomic stats to avoid data races between send/receive threads
    std::atomic<uint64_t> _bytesSent{0};         ///< Total bytes sent
    std::atomic<uint64_t> _bytesReceived{0};     ///< Total bytes received
    std::atomic<uint64_t> _messagesSent{0};      ///< Total messages sent
    std::atomic<uint64_t> _messagesReceived{0};  ///< Total messages received
    std::atomic<uint64_t> _connectTime{0};       ///< Connection timestamp (ms since epoch)
    std::atomic<uint64_t> _lastActivityTime{0};  ///< Last activity timestamp (ms since epoch)
};

}  // namespace EntropyEngine::Networking
