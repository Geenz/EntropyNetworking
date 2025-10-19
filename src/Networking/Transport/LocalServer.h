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
#include "ConnectionHandle.h"
#include "../Core/ErrorCodes.h"
#include <string>

namespace EntropyEngine::Networking {

// Forward declarations
class ConnectionManager;

/**
 * @brief Platform-agnostic local server for accepting IPC connections
 *
 * LocalServer provides a cross-platform abstraction for server-side local
 * connections. It handles:
 * - Unix domain sockets on Linux/macOS
 * - Named pipes on Windows (future)
 * - XPC on macOS (future)
 *
 * Derives from EntropyObject for type system integration and debugging.
 *
 * Usage:
 * @code
 * ConnectionManager connMgr(64);
 * LocalServer server(&connMgr, "/tmp/server.sock");
 *
 * auto result = server.listen();
 * if (result.failed()) { ... }
 *
 * // Accept connections (blocking)
 * while (running) {
 *     auto conn = server.accept();
 *     if (conn.valid()) {
 *         conn.send(data);
 *     }
 * }
 * @endcode
 */
class LocalServer : public Core::EntropyObject {
public:
    virtual ~LocalServer() = default;

    /**
     * @brief Starts listening for connections
     * @return Result indicating success or failure
     */
    virtual Result<void> listen() = 0;

    /**
     * @brief Accepts a connection (blocks until client connects)
     * @return ConnectionHandle for the accepted connection, or invalid handle on error
     */
    virtual ConnectionHandle accept() = 0;

    /**
     * @brief Stops listening and closes the server
     * @return Result indicating success or failure
     */
    virtual Result<void> close() = 0;

    /**
     * @brief Checks if server is currently listening
     * @return true if listening for connections
     */
    virtual bool isListening() const = 0;

protected:
    LocalServer() = default;
};

/**
 * @brief Local server configuration (platform-agnostic)
 */
struct LocalServerConfig {
    int backlog = 128;                 ///< listen() backlog
    int acceptPollIntervalMs = 500;    ///< poll interval or wait timeout in accept loop
    int chmodMode = -1;                ///< if >= 0, chmod the socket path to this mode (Unix)
    bool unlinkOnStart = true;         ///< if true, unlink socket path before bind (Unix)
    size_t pipeOutBufferSize = 1 * 1024 * 1024;  ///< Named pipe output buffer size (Windows)
    size_t pipeInBufferSize = 1 * 1024 * 1024;   ///< Named pipe input buffer size (Windows)
};

/**
 * @brief Creates a platform-appropriate LocalServer instance
 *
 * @param connMgr Connection manager that will own accepted connections
 * @param endpoint Platform-specific endpoint (socket path or pipe name)
 * @return Unique pointer to LocalServer implementation
 */
std::unique_ptr<LocalServer> createLocalServer(
    ConnectionManager* connMgr,
    const std::string& endpoint
);

/**
 * @brief Creates a LocalServer with configuration options
 */
std::unique_ptr<LocalServer> createLocalServer(
    ConnectionManager* connMgr,
    const std::string& endpoint,
    const LocalServerConfig& config
);

} // namespace EntropyEngine::Networking
