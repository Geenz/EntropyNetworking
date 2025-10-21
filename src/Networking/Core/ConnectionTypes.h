/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file ConnectionTypes.h
 * @brief Connection types, states, and configuration for networking
 *
 * This file contains enums and structs for connection abstraction including
 * connection types, states, backends, WebRTC configuration, and unified connection config.
 */

#pragma once

#include <string>
#include <optional>
#include <vector>
#include <functional>

namespace EntropyEngine::Networking {

// Default configuration constants
static constexpr size_t DEFAULT_MAX_MESSAGE_SIZE = 16ull * 1024ull * 1024ull;   // 16 MiB
static constexpr size_t DEFAULT_XPC_MAX_MESSAGE_SIZE = 64ull * 1024ull * 1024ull; // 64 MiB
static constexpr int DEFAULT_WEBRTC_MAX_MESSAGE_SIZE = 256 * 1024;               // 256 KiB

/**
 * @brief High-level connection type abstraction
 *
 * Represents platform-agnostic connection categories:
 * - Local: In-process or local IPC (Unix socket, Named pipe, XPC)
 * - Remote: Network connections (WebRTC, future: QUIC, WebTransport)
 */
enum class ConnectionType {
    Local,   ///< Local IPC - platform selects appropriate backend
    Remote   ///< Remote network connection - uses WebRTC
};

/**
 * @brief Explicit backend selection (advanced use)
 *
 * Allows overriding platform auto-selection for specific backend choice.
 * Use Auto for typical cases - platform picks the best backend.
 */
enum class ConnectionBackend {
    Auto,           ///< Automatic selection based on platform
    UnixSocket,     ///< Force Unix domain socket (Linux/macOS)
    NamedPipe,      ///< Force named pipe (Windows)
    XPC,            ///< Force XPC connection (macOS)
    WebRTC          ///< Force WebRTC data channel (all platforms)
};

/**
 * @brief Connection state during lifecycle
 */
enum class ConnectionState {
    Disconnected,   ///< Initial state or after disconnect
    Connecting,     ///< Connection in progress
    Connected,      ///< Fully connected and ready
    Disconnecting,  ///< Graceful disconnect in progress
    Failed          ///< Connection failed
};

/**
 * @brief Connection statistics
 */
struct ConnectionStats {
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t messagesSent = 0;
    uint64_t messagesReceived = 0;
    uint64_t connectTime = 0;      ///< Timestamp of connection establishment (ms since epoch)
    uint64_t lastActivityTime = 0; ///< Timestamp of last send/receive activity (ms since epoch)
};

/**
 * @brief WebRTC-specific configuration
 */
struct WebRTCConfig {
    std::vector<std::string> iceServers;                          ///< ICE server URLs (STUN/TURN)
    std::string proxyServer;                                      ///< Optional proxy server
    std::string bindAddress;                                      ///< Optional local bind address
    uint16_t portRangeBegin = 0;                                  ///< Port range start (0 = OS chooses)
    uint16_t portRangeEnd = 0;                                    ///< Port range end (0 = OS chooses)
    int maxMessageSize = DEFAULT_WEBRTC_MAX_MESSAGE_SIZE;         ///< Maximum message size
    bool enableIceTcp = false;                                    ///< Enable ICE-TCP candidates
    bool polite = false;                                          ///< Perfect negotiation: true = polite peer (accepts remote offers during glare)
};

/**
 * @brief WebRTC signaling callbacks
 *
 * The application must provide these callbacks to handle WebRTC signaling.
 * Typically, these would send data over WebSocket or another signaling channel.
 */
struct SignalingCallbacks {
    using LocalDescriptionCallback = std::function<void(const std::string& type, const std::string& sdp)>;
    using LocalCandidateCallback = std::function<void(const std::string& candidate, const std::string& mid)>;

    LocalDescriptionCallback onLocalDescription;
    LocalCandidateCallback onLocalCandidate;
};

/**
 * @brief Unified connection configuration
 *
 * Supports both simple (Local/Remote) and advanced (explicit backend) use cases.
 * Use the high-level helpers (openLocalConnection, openRemoteConnection) for
 * typical scenarios, or populate this struct for advanced control.
 */
struct ConnectionConfig {
    ConnectionType type = ConnectionType::Local;
    ConnectionBackend backend = ConnectionBackend::Auto;

    // Common fields
    std::string endpoint;  ///< Path, pipe name, or signaling server URL

    // Operational knobs (defaults preserve current behavior)
    int connectTimeoutMs = 5000;                          ///< Connect timeout for blocking waits (Unix sockets)
    int sendPollTimeoutMs = 1000;                         ///< Per-poll timeout during send retries (ms)
    int sendMaxPolls = 100;                               ///< Max poll iterations before timing out a send
    int recvIdlePollMs = -1;                              ///< If >= 0, use poll(POLLIN, recvIdlePollMs) instead of fixed sleep when idle
    size_t maxMessageSize = DEFAULT_MAX_MESSAGE_SIZE;     ///< Max message size for local transports

    // Socket buffer sizing (Unix); 0 = leave as OS default
    int socketSendBuf = 0;                                ///< SO_SNDBUF size in bytes (0 to skip)
    int socketRecvBuf = 0;                                ///< SO_RCVBUF size in bytes (0 to skip)

    // WebRTC-specific (only for Remote/WebRTC)
    WebRTCConfig webrtcConfig;                            ///< WebRTC configuration
    SignalingCallbacks signalingCallbacks;                ///< Signaling callbacks for SDP/ICE
    std::string dataChannelLabel = "entropy-data";        ///< Data channel label

    // XPC-specific (Apple)
    size_t xpcMaxMessageSize = DEFAULT_XPC_MAX_MESSAGE_SIZE; ///< Max allowed XPC payload size
    int xpcReplyTimeoutMs = 5000;                         ///< Default reply timeout for XPC sendWithReply

    // Platform-specific options
    std::optional<std::string> xpcServiceName;  ///< macOS XPC service identifier
};

} // namespace EntropyEngine::Networking
