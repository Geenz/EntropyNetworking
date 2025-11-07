/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "WebRTCServer.h"
#include "WebRTCConnection.h"
#include <Logging/Logger.h>
#include <format>

namespace EntropyEngine {
namespace Networking {

WebRTCServer::WebRTCServer(ConnectionManager* connMgr, const RemoteServerConfig& config)
    : _connMgr(connMgr)
    , _config(config)
{
    if (!_connMgr) {
        ENTROPY_LOG_ERROR("WebRTCServer: ConnectionManager is null");
    }
}

WebRTCServer::~WebRTCServer() {
    if (_listening.load(std::memory_order_acquire)) {
        close();
    }
}

Result<void> WebRTCServer::listen() {
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::err(
            NetworkError::AlreadyExists,
            "Server is already listening"
        );
    }

    if (!_connMgr) {
        return Result<void>::err(
            NetworkError::InvalidParameter,
            "ConnectionManager is null"
        );
    }

    try {
        // Configure WebSocket server
        rtc::WebSocketServer::Configuration wsConfig;
        wsConfig.port = _config.port;
        wsConfig.enableTls = _config.enableTls;

        // Create WebSocket server
        _wsServer = std::make_unique<rtc::WebSocketServer>(wsConfig);

        // Set up client handler
        _wsServer->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
            handleWebSocketClient(ws);
        });

        _listening.store(true, std::memory_order_release);

        ENTROPY_LOG_INFO(std::format("WebRTCServer listening on port {}", _config.port));

        return Result<void>::ok();

    } catch (const std::exception& e) {
        return Result<void>::err(
            NetworkError::ConnectionClosed,
            std::format("Failed to start WebSocket server: {}", e.what())
        );
    }
}

ConnectionHandle WebRTCServer::accept() {
    std::unique_lock<std::mutex> lock(_queueMutex);

    // Wait for a connection or shutdown
    _queueCV.wait(lock, [this] {
        return !_pendingConnections.empty() || !_listening.load(std::memory_order_acquire);
    });

    // Check if we're shutting down
    if (!_listening.load(std::memory_order_acquire)) {
        return ConnectionHandle();
    }

    // Get connection from queue
    if (!_pendingConnections.empty()) {
        auto conn = _pendingConnections.front();
        _pendingConnections.pop();
        return conn;
    }

    return ConnectionHandle();
}

Result<void> WebRTCServer::close() {
    if (!_listening.exchange(false, std::memory_order_acq_rel)) {
        return Result<void>::ok();  // Already closed
    }

    // Shutdown WebSocket server
    if (_wsServer) {
        _wsServer.reset();
    }

    // Wake up any threads waiting in accept()
    _queueCV.notify_all();

    ENTROPY_LOG_INFO("WebRTCServer closed");

    return Result<void>::ok();
}

bool WebRTCServer::isListening() const {
    return _listening.load(std::memory_order_acquire);
}

void WebRTCServer::handleWebSocketClient(std::shared_ptr<rtc::WebSocket> ws) {
    if (!_listening.load(std::memory_order_acquire)) {
        return;  // Server is shutting down
    }

    ENTROPY_LOG_INFO("WebRTCServer: Client connecting via signaling");

    // Create WebRTC connection configuration
    ConnectionConfig config;
    config.type = ConnectionType::Remote;
    config.backend = ConnectionBackend::WebRTC;

    // Server is impolite peer in perfect negotiation
    config.webrtcConfig.polite = _config.webrtcConfig.polite;

    // Copy ICE servers if configured
    config.webrtcConfig.iceServers = _config.webrtcConfig.iceServers;

    // Set up signaling callbacks to send through WebSocket
    config.signalingCallbacks.onLocalDescription = [ws](const std::string& type, const std::string& sdp) {
        ENTROPY_LOG_INFO(std::format("WebRTCServer: Sending {} to client", type));
        // Format: "type\nsdp"
        ws->send(type + "\n" + sdp);
    };

    config.signalingCallbacks.onLocalCandidate = [ws](const std::string& candidate, const std::string& mid) {
        // Format: "candidate|mid"
        ws->send(candidate + "|" + mid);
    };

    // Open WebRTC connection
    auto conn = _connMgr->openConnection(config);
    if (!conn.valid()) {
        ENTROPY_LOG_ERROR("WebRTCServer: Failed to create connection");
        return;
    }

    // Set up state callback to queue connection when ready
    conn.setStateCallback([this, conn](ConnectionState state) {
        if (state == ConnectionState::Connected) {
            ENTROPY_LOG_INFO("WebRTCServer: Connection established, queueing for accept()");

            // Add to pending queue
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _pendingConnections.push(conn);
            }

            // Notify accept() that a connection is ready
            _queueCV.notify_one();
        }
    });

    // Set up signaling message handler
    ws->onMessage([conn, connMgr = _connMgr](auto data) {
        if (std::holds_alternative<std::string>(data)) {
            std::string msg = std::get<std::string>(data);
            auto* webrtcConn = dynamic_cast<WebRTCConnection*>(connMgr->getConnectionPointer(conn));
            if (!webrtcConn) return;

            // Check if this is an ICE candidate (has '|' separator)
            size_t pipeSeparator = msg.find('|');
            if (pipeSeparator != std::string::npos) {
                // ICE candidate format: "candidate|mid"
                std::string candidateStr = msg.substr(0, pipeSeparator);
                std::string mid = msg.substr(pipeSeparator + 1);
                webrtcConn->addRemoteCandidate(candidateStr, mid);
            } else {
                // SDP format: "type\nsdp"
                size_t newlineSeparator = msg.find('\n');
                if (newlineSeparator != std::string::npos) {
                    std::string type = msg.substr(0, newlineSeparator);
                    std::string sdp = msg.substr(newlineSeparator + 1);
                    webrtcConn->setRemoteDescription(type, sdp);
                }
            }
        }
    });

    ws->onError([](std::string error) {
        ENTROPY_LOG_ERROR(std::format("WebRTCServer: Signaling error: {}", error));
    });

    // Connect the WebRTC peer
    auto connectResult = conn.connect();
    if (connectResult.failed()) {
        ENTROPY_LOG_ERROR(std::format("WebRTCServer: Failed to connect: {}", connectResult.errorMessage));
        return;
    }
}

} // namespace Networking
} // namespace EntropyEngine
