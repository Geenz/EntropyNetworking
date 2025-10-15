/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "UnixSocketConnection.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstddef>
#include <chrono>
#include <cerrno>
#include "../Core/ConnectionTypes.h"
#include <Logging/Logger.h>

namespace EntropyEngine::Networking {

// Message framing: [4-byte length][payload]
static constexpr size_t FRAME_HEADER_SIZE = sizeof(uint32_t);
static constexpr size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024; // 16MB

UnixSocketConnection::UnixSocketConnection(std::string socketPath)
    : _socketPath(std::move(socketPath))
{
}

UnixSocketConnection::UnixSocketConnection(std::string socketPath, const ConnectionConfig* cfg)
    : _socketPath(std::move(socketPath))
{
    if (cfg) {
        _connectTimeoutMs = cfg->connectTimeoutMs;
        _sendPollTimeoutMs = cfg->sendPollTimeoutMs;
        _sendMaxPolls = cfg->sendMaxPolls;
        _maxMessageSize = cfg->maxMessageSize;
        _socketSendBuf = cfg->socketSendBuf;
        _socketRecvBuf = cfg->socketRecvBuf;
    }
}

UnixSocketConnection::UnixSocketConnection(int connectedSocketFd, std::string peerInfo)
    : _socketPath(std::move(peerInfo))
    , _socket(connectedSocketFd)
{
    // Socket is already connected - set it up for our use

    // Set non-blocking
    int flags = fcntl(_socket, F_GETFL, 0);
    fcntl(_socket, F_SETFL, flags | O_NONBLOCK);
    fcntl(_socket, F_SETFD, FD_CLOEXEC);

#ifdef SO_NOSIGPIPE
    // macOS: prevent SIGPIPE on this socket
    int set = 1;
    setsockopt(_socket, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif

    // Mark as connected
    _state = ConnectionState::Connected;

    // Record connection time
    auto now = std::chrono::system_clock::now();
    _connectTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );

    // Start receive thread
    _shouldStop = false;
    _receiveThread = std::thread([this]() { receiveLoop(); });
}

UnixSocketConnection::~UnixSocketConnection() {
    disconnect();
}

Result<void> UnixSocketConnection::connect() {
    if (_state != ConnectionState::Disconnected) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already connected or connecting");
    }

    _state = ConnectionState::Connecting;
    onStateChanged(ConnectionState::Connecting);
    ENTROPY_LOG_INFO(std::string("Connecting to ") + _socketPath);

    // Create socket with non-blocking and close-on-exec flags when available
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    _socket = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    _socket = socket(AF_UNIX, SOCK_STREAM, 0);
#endif

    if (_socket < 0) {
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        ENTROPY_LOG_ERROR(std::string("Failed to create socket: ") + strerror(errno));
        return Result<void>::err(NetworkError::ConnectionClosed,
            std::string("Failed to create socket: ") + strerror(errno));
    }

#if !defined(SOCK_NONBLOCK) || !defined(SOCK_CLOEXEC)
    // Set non-blocking and close-on-exec if not set at creation
    int flags = fcntl(_socket, F_GETFL, 0);
    fcntl(_socket, F_SETFL, flags | O_NONBLOCK);
    fcntl(_socket, F_SETFD, FD_CLOEXEC);
#endif

#ifdef SO_NOSIGPIPE
    // macOS: prevent SIGPIPE on this socket
    int set = 1;
    setsockopt(_socket, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
#endif

    // Optional socket buffer sizing
    if (_socketSendBuf > 0) {
        ::setsockopt(_socket, SOL_SOCKET, SO_SNDBUF, &_socketSendBuf, sizeof(_socketSendBuf));
    }
    if (_socketRecvBuf > 0) {
        ::setsockopt(_socket, SOL_SOCKET, SO_RCVBUF, &_socketRecvBuf, sizeof(_socketRecvBuf));
    }

    // Connect
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    // Validate path length
    if (_socketPath.length() >= sizeof(addr.sun_path)) {
        close(_socket);
        _socket = -1;
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::InvalidParameter, "Socket path too long");
    }

    strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

    // Use BSD-portable sockaddr length for better portability
    socklen_t addrlen = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path)
    );

    if (::connect(_socket, reinterpret_cast<sockaddr*>(&addr), addrlen) < 0) {
        if (errno == EINPROGRESS) {
            // Wait for connection to complete
            pollfd pfd;
            pfd.fd = _socket;
            pfd.events = POLLOUT;

            int ret = poll(&pfd, 1, _connectTimeoutMs);
            if (ret < 0) {
                close(_socket);
                _socket = -1;
                _state = ConnectionState::Failed;
                onStateChanged(ConnectionState::Failed);
                ENTROPY_LOG_ERROR(std::string("Poll failed during connect: ") + strerror(errno));
                return Result<void>::err(NetworkError::ConnectionClosed,
                    std::string("Poll failed: ") + strerror(errno));
            }

            if (ret == 0) {
                close(_socket);
                _socket = -1;
                _state = ConnectionState::Failed;
                onStateChanged(ConnectionState::Failed);
                ENTROPY_LOG_WARNING("Unix socket connect timeout");
                return Result<void>::err(NetworkError::Timeout, "Connection timeout");
            }

            // Check if connection succeeded
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(_socket, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
                close(_socket);
                _socket = -1;
                _state = ConnectionState::Failed;
                onStateChanged(ConnectionState::Failed);
                ENTROPY_LOG_ERROR(std::string("getsockopt(SO_ERROR) failed: ") + strerror(errno));
                return Result<void>::err(NetworkError::ConnectionClosed,
                    std::string("getsockopt failed: ") + strerror(errno));
            }

            if (error != 0) {
                close(_socket);
                _socket = -1;
                _state = ConnectionState::Failed;
                onStateChanged(ConnectionState::Failed);
                ENTROPY_LOG_ERROR(std::string("Unix socket connect failed: ") + strerror(error));
                return Result<void>::err(NetworkError::ConnectionClosed,
                    std::string("Connection failed: ") + strerror(error));
            }
        } else {
            close(_socket);
            _socket = -1;
            _state = ConnectionState::Failed;
            onStateChanged(ConnectionState::Failed);
            ENTROPY_LOG_ERROR(std::string("Failed to connect: ") + strerror(errno));
            return Result<void>::err(NetworkError::ConnectionClosed,
                std::string("Failed to connect: ") + strerror(errno));
        }
    }

    _state = ConnectionState::Connected;
    onStateChanged(ConnectionState::Connected);
    ENTROPY_LOG_INFO(std::string("Connected to ") + _socketPath);

    // Record connection time
    auto now = std::chrono::system_clock::now();
    _connectTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );

    // Start receive thread
    _shouldStop = false;
    _receiveThread = std::thread([this]() { receiveLoop(); });

    return Result<void>::ok();
}

Result<void> UnixSocketConnection::disconnect() {
    if (_state == ConnectionState::Disconnected) {
        return Result<void>::ok();
    }

    _state = ConnectionState::Disconnecting;
    onStateChanged(ConnectionState::Disconnecting);
    ENTROPY_LOG_INFO("Disconnecting Unix socket");

    // Stop receive thread
    _shouldStop = true;
    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }

    // Gracefully shutdown and close socket
    if (_socket >= 0) {
        // shutdown() helps unblock any pending operations
        ::shutdown(_socket, SHUT_RDWR);
        close(_socket);
        _socket = -1;
    }

    _state = ConnectionState::Disconnected;
    onStateChanged(ConnectionState::Disconnected);

    return Result<void>::ok();
}

Result<void> UnixSocketConnection::send(const std::vector<uint8_t>& data) {
    return sendInternal(data);
}

Result<void> UnixSocketConnection::sendUnreliable(const std::vector<uint8_t>& data) {
    // Unix sockets are always reliable, so this is the same as send()
    return sendInternal(data);
}

Result<void> UnixSocketConnection::trySend(const std::vector<uint8_t>& data) {
    // Conservative non-blocking API: to avoid partial frame corruption without an internal send queue,
    // we currently do not attempt partial writes. Always report backpressure unless fully supported.
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (data.size() > _maxMessageSize) {
        return Result<void>::err(NetworkError::InvalidParameter, "Message too large");
    }
    return Result<void>::err(NetworkError::WouldBlock, "Non-blocking send not yet supported for UnixSocketConnection");
}

Result<void> UnixSocketConnection::sendInternal(const std::vector<uint8_t>& data) {
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (data.size() > _maxMessageSize) {
        return Result<void>::err(NetworkError::InvalidParameter, "Message too large");
    }

    std::lock_guard<std::mutex> lock(_sendMutex);

    // Send flags: use MSG_NOSIGNAL on Linux to prevent SIGPIPE
    int sendFlags = 0;
#ifdef MSG_NOSIGNAL
    sendFlags |= MSG_NOSIGNAL;
#endif

    // Send frame header (message length) with EAGAIN handling
    uint32_t length = static_cast<uint32_t>(data.size());
    uint32_t lengthBE = htonl(length);
    const uint8_t* hdrPtr = reinterpret_cast<const uint8_t*>(&lengthBE);
    size_t hdrSent = 0;
    int retryCount = 0;
    const int MAX_RETRIES = 100;
    ssize_t sent; // Declare outside both loops

    while (hdrSent < sizeof(lengthBE)) {
        sent = ::send(_socket, hdrPtr + hdrSent, sizeof(lengthBE) - hdrSent, sendFlags);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait for socket to become writable
                pollfd pfd;
                pfd.fd = _socket;
                pfd.events = POLLOUT;

                int ret = poll(&pfd, 1, _sendPollTimeoutMs);
                if (ret < 0) {
                    return Result<void>::err(NetworkError::ConnectionClosed,
                        std::string("Poll failed during header send: ") + strerror(errno));
                }
                if (ret == 0) {
                    if (++retryCount > _sendMaxPolls) {
                        ENTROPY_LOG_WARNING("Unix socket send timeout (header)");
                        return Result<void>::err(NetworkError::Timeout, "Send timeout (header)");
                    }
                }
                continue;
            }
            return Result<void>::err(NetworkError::ConnectionClosed,
                std::string("Failed to send frame header: ") + strerror(errno));
        }
        hdrSent += static_cast<size_t>(sent);
        retryCount = 0; // Reset retry count on successful send
    }

    // Send payload with proper EAGAIN handling
    size_t totalSent = 0;
    retryCount = 0; // Reset for payload send

    while (totalSent < data.size()) {
        sent = ::send(_socket, data.data() + totalSent, data.size() - totalSent, sendFlags);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait for socket to become writable
                pollfd pfd;
                pfd.fd = _socket;
                pfd.events = POLLOUT;

                int ret = poll(&pfd, 1, _sendPollTimeoutMs);
                if (ret < 0) {
                    ENTROPY_LOG_ERROR(std::string("Poll failed during send: ") + strerror(errno));
                    return Result<void>::err(NetworkError::ConnectionClosed,
                        std::string("Poll failed during send: ") + strerror(errno));
                }
                if (ret == 0) {
                    if (++retryCount > _sendMaxPolls) {
                        ENTROPY_LOG_WARNING("Unix socket send timeout (payload)");
                        return Result<void>::err(NetworkError::Timeout, "Send timeout");
                    }
                }
                continue;
            }
            ENTROPY_LOG_ERROR(std::string("Failed to send data: ") + strerror(errno));
            return Result<void>::err(NetworkError::ConnectionClosed,
                std::string("Failed to send data: ") + strerror(errno));
        }
        totalSent += sent;
        retryCount = 0; // Reset retry count on successful send
    }

    // Update stats atomically
    _bytesSent.fetch_add(FRAME_HEADER_SIZE + data.size(), std::memory_order_relaxed);
    _messagesSent.fetch_add(1, std::memory_order_relaxed);

    return Result<void>::ok();
}

void UnixSocketConnection::receiveLoop() {
    std::vector<uint8_t> buffer(65536);
    std::vector<uint8_t> messageBuffer;
    uint32_t expectedLength = 0;
    bool readingHeader = true;

    while (!_shouldStop && _state == ConnectionState::Connected) {
        ssize_t received = recv(_socket, buffer.data(), buffer.size(), 0);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // EAGAIN/EWOULDBLOCK: no data available yet
                // EINTR: interrupted by signal - retry
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // Receive error
            _state = ConnectionState::Failed;
            onStateChanged(ConnectionState::Failed);
            return;
        }

        if (received == 0) {
            // Connection closed by peer
            break;
        }

        // Update bytes received atomically
        _bytesReceived.fetch_add(received, std::memory_order_relaxed);

        // Process received data
        size_t offset = 0;
        while (offset < static_cast<size_t>(received)) {
            if (readingHeader) {
                // Read frame header
                size_t needed = FRAME_HEADER_SIZE - messageBuffer.size();
                size_t available = received - offset;
                size_t toCopy = std::min(needed, available);

                messageBuffer.insert(messageBuffer.end(),
                                   buffer.begin() + offset,
                                   buffer.begin() + offset + toCopy);
                offset += toCopy;

                if (messageBuffer.size() == FRAME_HEADER_SIZE) {
                    uint32_t lengthBE;
                    memcpy(&lengthBE, messageBuffer.data(), sizeof(lengthBE));
                    expectedLength = ntohl(lengthBE);

                    if (expectedLength > _maxMessageSize) {
                        // Invalid message length - protocol error
                        _state = ConnectionState::Failed;
                        onStateChanged(ConnectionState::Failed);
                        return;
                    }

                    messageBuffer.clear();
                    readingHeader = false;
                }
            } else {
                // Read payload
                size_t needed = expectedLength - messageBuffer.size();
                size_t available = received - offset;
                size_t toCopy = std::min(needed, available);

                messageBuffer.insert(messageBuffer.end(),
                                   buffer.begin() + offset,
                                   buffer.begin() + offset + toCopy);
                offset += toCopy;

                if (messageBuffer.size() == expectedLength) {
                    // Complete message received
                    _messagesReceived.fetch_add(1, std::memory_order_relaxed);
                    onMessageReceived(messageBuffer);

                    messageBuffer.clear();
                    readingHeader = true;
                    expectedLength = 0;
                }
            }
        }
    }

    // Connection closed or error
    if (_state == ConnectionState::Connected) {
        _state = ConnectionState::Disconnected;
        onStateChanged(ConnectionState::Disconnected);
    }
}

ConnectionStats UnixSocketConnection::getStats() const {
    // Create snapshot of atomic stats
    ConnectionStats stats;
    stats.bytesSent = _bytesSent.load(std::memory_order_relaxed);
    stats.bytesReceived = _bytesReceived.load(std::memory_order_relaxed);
    stats.messagesSent = _messagesSent.load(std::memory_order_relaxed);
    stats.messagesReceived = _messagesReceived.load(std::memory_order_relaxed);
    stats.connectTime = _connectTime.load(std::memory_order_relaxed);
    return stats;
}

} // namespace EntropyEngine::Networking
