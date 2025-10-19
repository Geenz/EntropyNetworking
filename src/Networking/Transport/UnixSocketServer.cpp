/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "UnixSocketServer.h"
#include "UnixSocketConnection.h"
#include "ConnectionManager.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH || TARGET_OS_VISION
#include "XPCServer.h"
#endif
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cstddef>
#include <cerrno>
#include <sys/stat.h>
#include <Logging/Logger.h>

namespace EntropyEngine::Networking {

UnixSocketServer::UnixSocketServer(ConnectionManager* connMgr, std::string socketPath)
    : _connMgr(connMgr)
    , _socketPath(std::move(socketPath))
{
}

UnixSocketServer::UnixSocketServer(ConnectionManager* connMgr, std::string socketPath, LocalServerConfig config)
    : _connMgr(connMgr)
    , _socketPath(std::move(socketPath))
    , _config(std::move(config))
{
}

UnixSocketServer::~UnixSocketServer() {
    close();
}

Result<void> UnixSocketServer::listen() {
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already listening");
    }

    // Remove existing socket file if present (configurable)
    if (_config.unlinkOnStart) {
        ::unlink(_socketPath.c_str());
    }

    // Create server socket with non-blocking for interruptible accept
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    _serverSocket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    _serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
#endif

    if (_serverSocket < 0) {
        ENTROPY_LOG_ERROR(std::string("Failed to create server socket: ") + strerror(errno));
        return Result<void>::err(NetworkError::ConnectionClosed,
            std::string("Failed to create server socket: ") + strerror(errno));
    }

#if !defined(SOCK_NONBLOCK) || !defined(SOCK_CLOEXEC)
    // Set non-blocking and close-on-exec if not set at creation
    int flags = fcntl(_serverSocket, F_GETFL, 0);
    fcntl(_serverSocket, F_SETFL, flags | O_NONBLOCK);
    fcntl(_serverSocket, F_SETFD, FD_CLOEXEC);
#endif

    // Prepare address
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    // Validate path length
    if (_socketPath.size() >= sizeof(addr.sun_path)) {
        ::close(_serverSocket);
        _serverSocket = -1;
        return Result<void>::err(NetworkError::InvalidParameter, "Socket path too long");
    }

    std::strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

    // Use BSD-portable sockaddr length
    socklen_t addrlen = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path)
    );

    // Bind
    if (::bind(_serverSocket, reinterpret_cast<sockaddr*>(&addr), addrlen) < 0) {
        std::string error = std::string("Failed to bind socket: ") + strerror(errno);
        ENTROPY_LOG_ERROR(error);
        ::close(_serverSocket);
        _serverSocket = -1;
        return Result<void>::err(NetworkError::ConnectionClosed, error);
    }

    // Optionally set permissions on the socket file
    if (_config.chmodMode >= 0) {
        ::chmod(_socketPath.c_str(), static_cast<mode_t>(_config.chmodMode));
    }

    // Listen with configured backlog
    if (::listen(_serverSocket, _config.backlog) < 0) {
        std::string error = std::string("Failed to listen on socket: ") + strerror(errno);
        ENTROPY_LOG_ERROR(error);
        ::close(_serverSocket);
        _serverSocket = -1;
        ::unlink(_socketPath.c_str());
        return Result<void>::err(NetworkError::ConnectionClosed, error);
    }

    _listening.store(true, std::memory_order_release);
    ENTROPY_LOG_INFO(std::string("Unix socket server listening on ") + _socketPath);
    return Result<void>::ok();
}

ConnectionHandle UnixSocketServer::accept() {
    if (!_listening.load(std::memory_order_acquire)) {
        return ConnectionHandle();  // Invalid handle - not listening
    }

    // Non-blocking accept with poll for clean shutdown
    pollfd pfd{_serverSocket, POLLIN, 0};

    while (_listening.load(std::memory_order_acquire)) {
        int ret = ::poll(&pfd, 1, _config.acceptPollIntervalMs);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Socket ready for accept
            int clientSocket = ::accept(_serverSocket, nullptr, nullptr);

            if (clientSocket >= 0) {
                // Successfully accepted - wrap in backend and adopt
                ENTROPY_LOG_INFO("Accepted Unix local connection");
                auto backend = std::make_unique<UnixSocketConnection>(clientSocket, "accepted");
                return _connMgr->adoptConnection(std::move(backend), ConnectionType::Local);
            }

            // Accept failed - check errno
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Spurious wakeup, continue polling
                continue;
            }
            if (errno == EINTR) {
                // Interrupted by signal, retry
                continue;
            }

            // Other error - return invalid handle
            ENTROPY_LOG_WARNING(std::string("accept() failed: ") + strerror(errno));
            return ConnectionHandle();
        }

        if (ret < 0) {
            // Poll error
            if (errno == EINTR) {
                // Interrupted by signal, continue
                continue;
            }
            // Other poll error
            ENTROPY_LOG_WARNING(std::string("poll() failed in accept: ") + strerror(errno));
            return ConnectionHandle();
        }

        // ret == 0: timeout, loop to check _listening flag
    }

    // Server closed while waiting
    return ConnectionHandle();
}

Result<void> UnixSocketServer::close() {
    if (!_listening.load(std::memory_order_acquire)) {
        return Result<void>::ok();
    }

    _listening.store(false, std::memory_order_release);

    if (_serverSocket >= 0) {
        ::close(_serverSocket);
        _serverSocket = -1;
    }

    ::unlink(_socketPath.c_str());
    ENTROPY_LOG_INFO(std::string("Unix socket server closed: ") + _socketPath);

    return Result<void>::ok();
}

uint64_t UnixSocketServer::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<UnixSocketServer>().id
    );
    return hash;
}

std::string UnixSocketServer::toString() const {
    return std::string(className()) + "@" +
           std::to_string(reinterpret_cast<uintptr_t>(this)) +
           "(path=" + _socketPath +
           ", listening=" + (isListening() ? "true" : "false") + ")";
}

// Factory function implementation
std::unique_ptr<LocalServer> createLocalServer(
    ConnectionManager* connMgr,
    const std::string& endpoint
) {
#if defined(__APPLE__)
    #if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH || TARGET_OS_VISION
        // iOS family - use XPC (Unix sockets unavailable)
        return std::make_unique<XPCServer>(connMgr, endpoint);
    #else
        // macOS - use Unix sockets
        return std::make_unique<UnixSocketServer>(connMgr, endpoint);
    #endif
#elif defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
    // Linux/Android - use Unix sockets
    return std::make_unique<UnixSocketServer>(connMgr, endpoint);
#elif defined(_WIN32)
    throw std::runtime_error("Named pipe server not yet implemented");
#else
    throw std::runtime_error("No local server implementation for this platform");
#endif
}

std::unique_ptr<LocalServer> createLocalServer(
    ConnectionManager* connMgr,
    const std::string& endpoint,
    const LocalServerConfig& config
) {
#if defined(__APPLE__)
    #if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH || TARGET_OS_VISION
        // iOS family - XPC server does not currently consume LocalServerConfig; ignore config
        (void)config;
        return std::make_unique<XPCServer>(connMgr, endpoint);
    #else
        // macOS - use Unix sockets with config
        return std::make_unique<UnixSocketServer>(connMgr, endpoint, config);
    #endif
#elif defined(__unix__) || defined(__linux__) || defined(__ANDROID__)
    // Linux/Android - use Unix sockets with config
    return std::make_unique<UnixSocketServer>(connMgr, endpoint, config);
#elif defined(_WIN32)
    (void)config;
    throw std::runtime_error("Named pipe server not yet implemented");
#else
    (void)config;
    throw std::runtime_error("No local server implementation for this platform");
#endif
}

} // namespace EntropyEngine::Networking

#endif // defined(__unix__) || defined(__APPLE__)
