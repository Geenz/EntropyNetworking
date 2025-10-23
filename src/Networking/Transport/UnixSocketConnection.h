/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file UnixSocketConnection.h
 * @brief Unix domain socket implementation for local IPC
 *
 * This file contains UnixSocketConnection, a NetworkConnection implementation
 * using Unix domain sockets (AF_UNIX) for high-performance local inter-process
 * communication on Linux and macOS.
 */

#pragma once

#include "NetworkConnection.h"
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

namespace EntropyEngine::Networking {

/**
 * @brief Unix domain socket implementation for local IPC
 *
 * UnixSocketConnection provides reliable, ordered message delivery using Unix domain
 * sockets (AF_UNIX). Offers significantly lower latency and higher throughput than
 * TCP loopback for local communication.
 *
 * Features:
 * - Length-prefixed framing (uint32_t length + payload)
 * - Dedicated receive thread for async message delivery
 * - Configurable socket buffer sizes (SO_SNDBUF/SO_RCVBUF)
 * - Configurable send/receive timeouts with poll-based retries
 * - Atomic statistics tracking
 * - Non-blocking send support via trySend()
 *
 * Platform support: Linux, macOS (Unix systems only)
 *
 * Thread Safety: All public methods are thread-safe. Send operations are serialized
 * via mutex. Receive thread runs independently and invokes callbacks.
 *
 * @code
 * // Client-side usage
 * ConnectionConfig cfg;
 * cfg.endpoint = "/tmp/entropy.sock";
 * cfg.connectTimeoutMs = 5000;
 * cfg.socketSendBuf = 256 * 1024;  // 256 KB send buffer
 * cfg.socketRecvBuf = 256 * 1024;  // 256 KB receive buffer
 *
 * auto conn = std::make_unique<UnixSocketConnection>(cfg.endpoint, &cfg);
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
 * // Server-side usage (after accept())
 * int clientFd = accept(serverFd, nullptr, nullptr);
 * auto clientConn = std::make_unique<UnixSocketConnection>(clientFd, "client-1");
 * clientConn->setMessageCallback([](const auto& data) { processMessage(data); });
 * // Connection is already established, ready to send/receive
 * @endcode
 */
class UnixSocketConnection : public NetworkConnection {
public:
    /**
     * @brief Constructs client-side connection to socket path
     * @param socketPath Path to Unix domain socket (e.g., "/tmp/entropy.sock")
     */
    UnixSocketConnection(std::string socketPath);

    /**
     * @brief Constructs client-side connection with configuration
     * @param socketPath Path to Unix domain socket
     * @param cfg Connection configuration (timeouts, buffer sizes, etc.)
     */
    UnixSocketConnection(std::string socketPath, const struct ConnectionConfig* cfg);

    /**
     * @brief Constructs server-side connection from accepted socket
     *
     * Used by LocalServer to wrap accepted client connections. Socket is already
     * connected; no need to call connect().
     *
     * @param connectedSocketFd File descriptor from accept()
     * @param peerInfo Identifier for logging/debugging
     */
    UnixSocketConnection(int connectedSocketFd, std::string peerInfo);

    /**
     * @brief Destructor ensures clean shutdown
     *
     * Stops receive thread, shuts down callbacks, closes socket.
     */
    ~UnixSocketConnection() override;

    // NetworkConnection interface
    Result<void> connect() override;
    Result<void> disconnect() override;
    bool isConnected() const override { return _state.load() == ConnectionState::Connected; }

    Result<void> send(const std::vector<uint8_t>& data) override;
    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
    Result<void> trySend(const std::vector<uint8_t>& data) override;

    ConnectionState getState() const override { return _state.load(); }
    ConnectionType getType() const override { return ConnectionType::Local; }
    ConnectionStats getStats() const override;

private:
    /**
     * @brief Receive thread main loop
     *
     * Reads length-prefixed messages from socket and invokes message callback.
     * Runs until _shouldStop is set or socket is closed.
     */
    void receiveLoop();

    /**
     * @brief Internal send implementation with poll-based retry
     * @param data Message bytes to send
     * @return Result indicating success or failure
     */
    Result<void> sendInternal(const std::vector<uint8_t>& data);

    std::string _socketPath;                                 ///< Socket path for client connections
    int _socket{-1};                                         ///< Socket file descriptor
    std::atomic<ConnectionState> _state{ConnectionState::Disconnected}; ///< Current connection state

    std::thread _receiveThread;                              ///< Dedicated receive thread
    std::atomic<bool> _shouldStop{false};                    ///< Shutdown signal for receive thread

    mutable std::mutex _sendMutex;                           ///< Serializes send operations

    // Configurable parameters (initialized from ConnectionConfig or defaults)
    int _connectTimeoutMs{5000};                             ///< Connect timeout (milliseconds)
    int _sendPollTimeoutMs{100};                             ///< Per-poll timeout during send (ms)
    int _sendMaxPolls{20};                                   ///< Max poll iterations before send timeout
    int _recvIdlePollMs{-1};                                 ///< Receive poll timeout (-1 = blocking)
    size_t _maxMessageSize{16ull * 1024ull * 1024ull};      ///< Maximum message size (16 MiB)
    int _socketSendBuf{0};                                   ///< SO_SNDBUF size (0 = OS default)
    int _socketRecvBuf{0};                                   ///< SO_RCVBUF size (0 = OS default)

    // Atomic stats to avoid data races between send/receive threads
    std::atomic<uint64_t> _bytesSent{0};                     ///< Total bytes sent
    std::atomic<uint64_t> _bytesReceived{0};                 ///< Total bytes received
    std::atomic<uint64_t> _messagesSent{0};                  ///< Total messages sent
    std::atomic<uint64_t> _messagesReceived{0};              ///< Total messages received
    std::atomic<uint64_t> _connectTime{0};                   ///< Connection timestamp (ms since epoch)
    std::atomic<uint64_t> _lastActivityTime{0};              ///< Last activity timestamp (ms since epoch)
};

} // namespace EntropyEngine::Networking
