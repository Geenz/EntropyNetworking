// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "UnixSocketServer.h"
#include "UnixSocketConnection.h"
#include "ConnectionManager.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cstddef>
#include <cerrno>

namespace EntropyEngine::Networking {

UnixSocketServer::UnixSocketServer(ConnectionManager* connMgr, std::string socketPath)
    : _connMgr(connMgr)
    , _socketPath(std::move(socketPath))
{
}

UnixSocketServer::~UnixSocketServer() {
    close();
}

Result<void> UnixSocketServer::listen() {
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already listening");
    }

    // Remove existing socket file if present
    ::unlink(_socketPath.c_str());

    // Create server socket with non-blocking for interruptible accept
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    _serverSocket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    _serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
#endif

    if (_serverSocket < 0) {
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
        ::close(_serverSocket);
        _serverSocket = -1;
        return Result<void>::err(NetworkError::ConnectionClosed, error);
    }

    // Listen with reasonable backlog
    if (::listen(_serverSocket, 128) < 0) {
        std::string error = std::string("Failed to listen on socket: ") + strerror(errno);
        ::close(_serverSocket);
        _serverSocket = -1;
        ::unlink(_socketPath.c_str());
        return Result<void>::err(NetworkError::ConnectionClosed, error);
    }

    _listening.store(true, std::memory_order_release);
    return Result<void>::ok();
}

ConnectionHandle UnixSocketServer::accept() {
    if (!_listening.load(std::memory_order_acquire)) {
        return ConnectionHandle();  // Invalid handle - not listening
    }

    // Non-blocking accept with poll for clean shutdown
    pollfd pfd{_serverSocket, POLLIN, 0};

    while (_listening.load(std::memory_order_acquire)) {
        int ret = ::poll(&pfd, 1, 500);  // 500ms timeout for responsiveness

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Socket ready for accept
            int clientSocket = ::accept(_serverSocket, nullptr, nullptr);

            if (clientSocket >= 0) {
                // Successfully accepted - wrap in backend and adopt
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
            return ConnectionHandle();
        }

        if (ret < 0) {
            // Poll error
            if (errno == EINTR) {
                // Interrupted by signal, continue
                continue;
            }
            // Other poll error
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
#if defined(__unix__) || defined(__APPLE__)
    return std::make_unique<UnixSocketServer>(connMgr, endpoint);
#elif defined(_WIN32)
    throw std::runtime_error("Named pipe server not yet implemented");
#else
    throw std::runtime_error("No local server implementation for this platform");
#endif
}

} // namespace EntropyEngine::Networking
