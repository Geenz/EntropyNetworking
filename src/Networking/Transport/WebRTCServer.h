/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "RemoteServer.h"
#include "ConnectionManager.h"
#include <rtc/rtc.hpp>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief WebRTC-based implementation of RemoteServer
 *
 * WebRTCServer handles WebRTC connections using WebSocket signaling.
 * It manages the WebSocket server internally and queues incoming
 * WebRTC connections for accept().
 *
 * Architecture:
 * - WebSocket signaling server runs on configured port
 * - Incoming WebSocket connections trigger WebRTC setup
 * - Completed WebRTC connections are queued for accept()
 * - Perfect negotiation: server is impolite peer
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - accept() can be called from multiple threads
 * - Internal queue protected by mutex
 * - Condition variable for efficient blocking
 *
 * Implementation Details:
 * - WebSocket callbacks run on libdatachannel threads
 * - Signaling messages handled internally
 * - Connection adoption handled automatically
 * - Cleanup on close() or destruction
 */
class WebRTCServer : public RemoteServer {
public:
    /**
     * @brief Construct WebRTC server with configuration
     * @param connMgr ConnectionManager to adopt connections
     * @param config Server configuration
     */
    WebRTCServer(ConnectionManager* connMgr, const RemoteServerConfig& config);

    /**
     * @brief Destructor - ensures cleanup
     */
    ~WebRTCServer() override;

    // Disable copy/move
    WebRTCServer(const WebRTCServer&) = delete;
    WebRTCServer& operator=(const WebRTCServer&) = delete;

    /**
     * @brief Start WebSocket signaling server
     * @return Result indicating success or failure
     */
    Result<void> listen() override;

    /**
     * @brief Accept an incoming WebRTC connection (blocking)
     *
     * Blocks until:
     * - A WebRTC connection completes and is queued
     * - The server is closed
     *
     * Uses condition variable for efficient waiting.
     *
     * @return ConnectionHandle for completed connection, or invalid if closed
     */
    ConnectionHandle accept() override;

    /**
     * @brief Close server and reject pending connections
     * @return Result indicating success or failure
     */
    Result<void> close() override;

    /**
     * @brief Check if server is listening
     * @return true if listening, false otherwise
     */
    bool isListening() const override;

private:
    /**
     * @brief Handle incoming WebSocket client connection
     * @param ws WebSocket connection from client
     *
     * Sets up:
     * - WebRTC peer connection
     * - Signaling callbacks
     * - Perfect negotiation (server is impolite)
     * - Connection queueing on completion
     */
    void handleWebSocketClient(std::shared_ptr<rtc::WebSocket> ws);

    ConnectionManager* _connMgr;
    RemoteServerConfig _config;

    // WebSocket signaling server
    std::unique_ptr<rtc::WebSocketServer> _wsServer;

    // Connection queue for accept()
    std::mutex _queueMutex;
    std::condition_variable _queueCV;
    std::queue<ConnectionHandle> _pendingConnections;

    // Server state
    std::atomic<bool> _listening{false};
};

} // namespace Networking
} // namespace EntropyEngine
