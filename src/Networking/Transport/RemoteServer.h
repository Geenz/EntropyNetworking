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

#include <cstdint>
#include <memory>
#include <string>

#include "ConnectionHandle.h"
#include "WebRTCConnection.h"

namespace EntropyEngine
{
namespace Networking
{

// Forward declarations
class ConnectionManager;

/**
 * @brief Configuration for WebRTC-specific settings
 *
 * These settings are specific to WebRTC protocol and nested
 * within RemoteServerConfig for extensibility.
 */
struct WebRTCServerConfig
{
    bool polite = false;                  // Server is typically impolite in perfect negotiation
    std::vector<std::string> iceServers;  // STUN/TURN server URLs
};

/**
 * @brief Configuration for remote server connections
 *
 * This configuration is protocol-agnostic with protocol-specific
 * settings nested within. This allows future protocols (QUIC, etc.)
 * to be added without changing the base config structure.
 */
struct RemoteServerConfig
{
    // Generic remote connection settings
    uint16_t port = 8080;
    int backlog = 128;
    int acceptPollIntervalMs = 100;

    // TLS/security (generic across protocols)
    bool enableTls = false;
    std::string tlsCertPath;
    std::string tlsKeyPath;

    // Protocol-specific configurations
    WebRTCServerConfig webrtcConfig;
    // Future: QUICServerConfig quicConfig, etc.
};

/**
 * @brief Abstract base class for remote server connections
 *
 * RemoteServer provides an abstract interface for remote connection servers,
 * analogous to LocalServer but for network-based transports. Concrete
 * implementations handle specific protocols (WebRTC, QUIC, etc.).
 *
 * Design Philosophy:
 * - Match LocalServer interface exactly for consistency
 * - Abstract away protocol-specific details
 * - Blocking accept() pattern for simplicity
 * - Factory pattern for instantiation
 *
 * Usage:
 * @code
 * auto server = createRemoteServer(&connMgr, 8080);
 * auto listenResult = server->listen();
 * if (listenResult.success()) {
 *     auto conn = server->accept();  // Blocks until client connects
 * }
 * @endcode
 *
 * Thread Safety:
 * - Implementations must be thread-safe
 * - accept() may be called from multiple threads
 * - close() must be safe to call from any thread
 */
class RemoteServer : public Core::EntropyObject
{
public:
    virtual ~RemoteServer() = default;

    /**
     * @brief Start listening for incoming remote connections
     * @return Result indicating success or failure with error details
     */
    virtual Result<void> listen() = 0;

    /**
     * @brief Accept an incoming connection (blocking)
     *
     * Blocks until a client connects or the server is closed.
     * Uses internal polling with acceptPollIntervalMs to check for
     * shutdown while waiting.
     *
     * @return ConnectionHandle for the new connection, or invalid handle if closed
     */
    virtual ConnectionHandle accept() = 0;

    /**
     * @brief Stop listening and close the server
     * @return Result indicating success or failure
     */
    virtual Result<void> close() = 0;

    /**
     * @brief Check if the server is currently listening
     * @return true if listening, false otherwise
     */
    virtual bool isListening() const = 0;

protected:
    /**
     * @brief Protected constructor - use factory functions
     *
     * Direct instantiation is prevented to enforce factory pattern.
     */
    RemoteServer() = default;
};

/**
 * @brief Create a remote server with default configuration
 *
 * Creates a RemoteServer using the default protocol (WebRTC).
 * The server will listen on the specified port.
 *
 * @param connMgr ConnectionManager to adopt new connections
 * @param port Port number to listen on (default 8080)
 * @return Unique pointer to RemoteServer instance
 */
std::unique_ptr<RemoteServer> createRemoteServer(ConnectionManager* connMgr, uint16_t port = 8080);

/**
 * @brief Create a remote server with custom configuration
 *
 * Creates a RemoteServer with full control over configuration.
 * Allows customization of TLS, polling intervals, protocol-specific
 * settings, etc.
 *
 * @param connMgr ConnectionManager to adopt new connections
 * @param config Complete server configuration
 * @return Unique pointer to RemoteServer instance
 */
std::unique_ptr<RemoteServer> createRemoteServer(ConnectionManager* connMgr, const RemoteServerConfig& config);

}  // namespace Networking
}  // namespace EntropyEngine
