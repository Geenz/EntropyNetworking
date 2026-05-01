/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "WebRTCServer.h"

#include <Logging/Logger.h>

#include <cstring>
#include <format>

#include "WebRTCConnection.h"

namespace EntropyEngine
{
namespace Networking
{

// Binary envelope framing for signaling messages (same as in WebRTCConnection.cpp)
// Format: [Type:1B][Length:4B big-endian][Payload]
// Type: 0x01=SDP, 0x02=ICE
// Payload: For SDP: type\0sdp, For ICE: candidate\0mid

enum class SignalingMessageType : uint8_t
{
    SDP = 0x01,
    ICE = 0x02
};

// Encode uint32_t to big-endian bytes
static void encodeU32BigEndian(uint32_t value, uint8_t* dest) {
    dest[0] = (value >> 24) & 0xFF;
    dest[1] = (value >> 16) & 0xFF;
    dest[2] = (value >> 8) & 0xFF;
    dest[3] = value & 0xFF;
}

// Decode big-endian bytes to uint32_t
static uint32_t decodeU32BigEndian(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

// Encode SDP description into binary envelope
static std::vector<uint8_t> encodeSDPMessage(const std::string& type, const std::string& sdp) {
    // Payload: type\0sdp
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), type.begin(), type.end());
    payload.push_back(0);  // null terminator
    payload.insert(payload.end(), sdp.begin(), sdp.end());

    // Envelope: [Type:1B][Length:4B][Payload]
    std::vector<uint8_t> message;
    message.reserve(5 + payload.size());
    message.push_back(static_cast<uint8_t>(SignalingMessageType::SDP));

    uint8_t lengthBytes[4];
    encodeU32BigEndian(static_cast<uint32_t>(payload.size()), lengthBytes);
    message.insert(message.end(), lengthBytes, lengthBytes + 4);
    message.insert(message.end(), payload.begin(), payload.end());

    return message;
}

// Encode ICE candidate into binary envelope
static std::vector<uint8_t> encodeICEMessage(const std::string& candidate, const std::string& mid) {
    // Payload: candidate\0mid
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), candidate.begin(), candidate.end());
    payload.push_back(0);  // null terminator
    payload.insert(payload.end(), mid.begin(), mid.end());

    // Envelope: [Type:1B][Length:4B][Payload]
    std::vector<uint8_t> message;
    message.reserve(5 + payload.size());
    message.push_back(static_cast<uint8_t>(SignalingMessageType::ICE));

    uint8_t lengthBytes[4];
    encodeU32BigEndian(static_cast<uint32_t>(payload.size()), lengthBytes);
    message.insert(message.end(), lengthBytes, lengthBytes + 4);
    message.insert(message.end(), payload.begin(), payload.end());

    return message;
}

// Decode binary envelope message
// Returns: {type, str1, str2} where for SDP: str1=type, str2=sdp; for ICE: str1=candidate, str2=mid
static std::optional<std::tuple<SignalingMessageType, std::string, std::string>> decodeSignalingMessage(
    const std::vector<uint8_t>& data) {
    // Minimum size: Type(1) + Length(4) = 5 bytes
    if (data.size() < 5) {
        ENTROPY_LOG_ERROR("Signaling message too short");
        return std::nullopt;
    }

    SignalingMessageType msgType = static_cast<SignalingMessageType>(data[0]);
    uint32_t payloadLength = decodeU32BigEndian(&data[1]);

    // Verify payload length
    if (data.size() != 5 + payloadLength) {
        ENTROPY_LOG_ERROR(
            std::format("Signaling message length mismatch: expected {}, got {}", 5 + payloadLength, data.size()));
        return std::nullopt;
    }

    // Extract payload
    const uint8_t* payload = &data[5];

    // Find null terminator
    const uint8_t* nullPos = static_cast<const uint8_t*>(std::memchr(payload, 0, payloadLength));
    if (!nullPos) {
        ENTROPY_LOG_ERROR("Signaling message payload missing null terminator");
        return std::nullopt;
    }

    size_t firstStringLen = nullPos - payload;
    size_t secondStringStart = firstStringLen + 1;

    if (secondStringStart > payloadLength) {
        ENTROPY_LOG_ERROR("Signaling message payload truncated");
        return std::nullopt;
    }

    std::string str1(reinterpret_cast<const char*>(payload), firstStringLen);
    std::string str2(reinterpret_cast<const char*>(payload + secondStringStart), payloadLength - secondStringStart);

    return std::make_tuple(msgType, str1, str2);
}

WebRTCServer::WebRTCServer(ConnectionManager* connMgr, const RemoteServerConfig& config)
    : _connMgr(connMgr), _config(config) {
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
        return Result<void>::err(NetworkError::AlreadyExists, "Server is already listening");
    }

    if (!_connMgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "ConnectionManager is null");
    }

    try {
        // Configure WebSocket server
        rtc::WebSocketServer::Configuration wsConfig;
        wsConfig.port = _config.port;
        wsConfig.enableTls = _config.enableTls;

        // Create WebSocket server
        _wsServer = std::make_unique<rtc::WebSocketServer>(wsConfig);

        // Set up client handler
        _wsServer->onClient([this](std::shared_ptr<rtc::WebSocket> ws) { handleWebSocketClient(ws); });

        _listening.store(true, std::memory_order_release);

        ENTROPY_LOG_INFO(std::format("WebRTCServer listening on port {}", _config.port));

        return Result<void>::ok();

    } catch (const std::exception& e) {
        return Result<void>::err(NetworkError::ConnectionClosed,
                                 std::format("Failed to start WebSocket server: {}", e.what()));
    }
}

ConnectionHandle WebRTCServer::accept() {
    std::unique_lock<std::mutex> lock(_queueMutex);

    // Wait for a connection or shutdown
    _queueCV.wait(lock, [this] { return !_pendingConnections.empty() || !_listening.load(std::memory_order_acquire); });

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
    // Uses binary envelope framing for robustness
    config.signalingCallbacks.onLocalDescription = [ws](const std::string& type, const std::string& sdp) {
        ENTROPY_LOG_INFO(std::format("WebRTCServer: Sending {} to client", type));
        auto encoded = encodeSDPMessage(type, sdp);
        ws->send(reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
    };

    config.signalingCallbacks.onLocalCandidate = [ws](const std::string& candidate, const std::string& mid) {
        auto encoded = encodeICEMessage(candidate, mid);
        ws->send(reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
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
    // Expects binary envelope format
    ws->onMessage([conn, connMgr = _connMgr](auto data) {
        if (std::holds_alternative<rtc::binary>(data)) {
            const auto& binaryData = std::get<rtc::binary>(data);
            // Convert std::byte to uint8_t
            std::vector<uint8_t> msg;
            msg.reserve(binaryData.size());
            for (const auto& byte : binaryData) {
                msg.push_back(static_cast<uint8_t>(byte));
            }

            auto* webrtcConn = dynamic_cast<WebRTCConnection*>(connMgr->getConnectionPointer(conn));
            if (!webrtcConn) return;

            auto decoded = decodeSignalingMessage(msg);
            if (!decoded) {
                ENTROPY_LOG_ERROR("Failed to decode signaling message");
                return;
            }

            auto [msgType, str1, str2] = *decoded;
            if (msgType == SignalingMessageType::SDP) {
                // str1=type, str2=sdp
                webrtcConn->setRemoteDescription(str1, str2);
            } else if (msgType == SignalingMessageType::ICE) {
                // str1=candidate, str2=mid
                webrtcConn->addRemoteCandidate(str1, str2);
            }
        }
    });

    ws->onError([](std::string error) { ENTROPY_LOG_ERROR(std::format("WebRTCServer: Signaling error: {}", error)); });

    // Connect the WebRTC peer
    auto connectResult = conn.connect();
    if (connectResult.failed()) {
        ENTROPY_LOG_ERROR(std::format("WebRTCServer: Failed to connect: {}", connectResult.errorMessage));
        return;
    }
}

}  // namespace Networking
}  // namespace EntropyEngine
