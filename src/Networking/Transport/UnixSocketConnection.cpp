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
#include <cstring>
#include <chrono>

namespace EntropyEngine::Networking {

// Message framing: [4-byte length][payload]
static constexpr size_t FRAME_HEADER_SIZE = sizeof(uint32_t);
static constexpr size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024; // 16MB

UnixSocketConnection::UnixSocketConnection(std::string socketPath)
    : _socketPath(std::move(socketPath))
{
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

    // Create socket
    _socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_socket < 0) {
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::ConnectionClosed, "Failed to create socket");
    }

    // Set non-blocking
    int flags = fcntl(_socket, F_GETFL, 0);
    fcntl(_socket, F_SETFL, flags | O_NONBLOCK);

    // Connect
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            close(_socket);
            _socket = -1;
            _state = ConnectionState::Failed;
            onStateChanged(ConnectionState::Failed);
            return Result<void>::err(NetworkError::ConnectionClosed, "Failed to connect");
        }
    }

    _state = ConnectionState::Connected;
    onStateChanged(ConnectionState::Connected);

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

    // Stop receive thread
    _shouldStop = true;
    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }

    // Close socket
    if (_socket >= 0) {
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

Result<void> UnixSocketConnection::sendInternal(const std::vector<uint8_t>& data) {
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (data.size() > MAX_MESSAGE_SIZE) {
        return Result<void>::err(NetworkError::InvalidParameter, "Message too large");
    }

    std::lock_guard<std::mutex> lock(_sendMutex);

    // Send frame header (message length)
    uint32_t length = static_cast<uint32_t>(data.size());
    uint32_t lengthBE = htonl(length);

    ssize_t sent = ::send(_socket, &lengthBE, sizeof(lengthBE), 0);
    if (sent != sizeof(lengthBE)) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Failed to send frame header");
    }

    // Send payload
    size_t totalSent = 0;
    while (totalSent < data.size()) {
        sent = ::send(_socket, data.data() + totalSent, data.size() - totalSent, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return Result<void>::err(NetworkError::ConnectionClosed, "Failed to send data");
        }
        totalSent += sent;
    }

    _stats.bytesSent += FRAME_HEADER_SIZE + data.size();
    _stats.messagesSent++;

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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }

        if (received == 0) {
            // Connection closed
            break;
        }

        _stats.bytesReceived += received;

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

                    if (expectedLength > MAX_MESSAGE_SIZE) {
                        // Invalid message length
                        _state = ConnectionState::Failed;
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
                    _stats.messagesReceived++;
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
    return _stats;
}

} // namespace EntropyEngine::Networking
