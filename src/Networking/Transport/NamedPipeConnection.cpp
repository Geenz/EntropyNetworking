/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "NamedPipeConnection.h"
#include "../Core/ConnectionTypes.h"
#include <Logging/Logger.h>
#include <chrono>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace EntropyEngine::Networking {

static constexpr size_t FRAME_HEADER_SIZE = sizeof(uint32_t);

NamedPipeConnection::NamedPipeConnection(std::string pipeName)
    : _pipeName(std::move(pipeName))
{
}

NamedPipeConnection::NamedPipeConnection(std::string pipeName, const ConnectionConfig* cfg)
    : _pipeName(std::move(pipeName))
{
    if (cfg) {
        _connectTimeoutMs = cfg->connectTimeoutMs;
        _sendPollTimeoutMs = cfg->sendPollTimeoutMs;
        _sendMaxPolls = cfg->sendMaxPolls;
        _recvIdlePollMs = cfg->recvIdlePollMs;
        _maxMessageSize = cfg->maxMessageSize;
    }
}

#ifdef _WIN32
NamedPipeConnection::NamedPipeConnection(HANDLE connectedPipe, std::string /*peerInfo*/)
    : _pipe(connectedPipe)
{
    _state = ConnectionState::Connected;

    auto now = std::chrono::system_clock::now();
    _connectTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );
    _lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );

    _shouldStop = false;
    _receiveThread = std::thread([this]() { receiveLoop(); });
}
#endif

NamedPipeConnection::~NamedPipeConnection() {
    (void)disconnect();
}

std::wstring NamedPipeConnection::toWide(const std::string& s) const {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w;
    w.resize(static_cast<size_t>(len - 1));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
#else
    return std::wstring();
#endif
}

Result<void> NamedPipeConnection::connect() {
#ifndef _WIN32
    return Result<void>::err(NetworkError::InvalidParameter, "Named pipes only supported on Windows");
#else
    if (_state != ConnectionState::Disconnected) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already connected or connecting");
    }

    _state = ConnectionState::Connecting;
    onStateChanged(ConnectionState::Connecting);
    ENTROPY_LOG_INFO(std::string("Connecting to pipe ") + _pipeName);

    std::wstring wname = toWide(_pipeName);

    // Optionally wait for pipe to become available
    DWORD waitMs = _connectTimeoutMs > 0 ? static_cast<DWORD>(_connectTimeoutMs) : NMPWAIT_WAIT_FOREVER;
    if (!WaitNamedPipeW(wname.c_str(), waitMs)) {
        DWORD err = GetLastError();
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        ENTROPY_LOG_ERROR("WaitNamedPipeW failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed, "WaitNamedPipe failed: " + std::to_string(err));
    }

    _pipe = CreateFileW(
        wname.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (_pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        ENTROPY_LOG_ERROR("CreateFileW on pipe failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed, "CreateFile on pipe failed: " + std::to_string(err));
    }

    // Set to byte mode and blocking
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    SetNamedPipeHandleState(_pipe, &mode, nullptr, nullptr);

    _state = ConnectionState::Connected;
    onStateChanged(ConnectionState::Connected);

    auto now = std::chrono::system_clock::now();
    _connectTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );
    _lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );

    _shouldStop = false;
    _receiveThread = std::thread([this]() { receiveLoop(); });

    return Result<void>::ok();
#endif
}

Result<void> NamedPipeConnection::disconnect() {
#ifdef _WIN32
    _shouldStop = true;
    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }

    if (_state == ConnectionState::Disconnected) {
        if (_pipe != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(_pipe);
            CloseHandle(_pipe);
            _pipe = INVALID_HANDLE_VALUE;
        }
        return Result<void>::ok();
    }

    _state = ConnectionState::Disconnecting;
    onStateChanged(ConnectionState::Disconnecting);

    if (_pipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(_pipe);
        CloseHandle(_pipe);
        _pipe = INVALID_HANDLE_VALUE;
    }

    _state = ConnectionState::Disconnected;
    onStateChanged(ConnectionState::Disconnected);
    return Result<void>::ok();
#else
    return Result<void>::ok();
#endif
}

Result<void> NamedPipeConnection::send(const std::vector<uint8_t>& data) {
    return sendInternal(data);
}

Result<void> NamedPipeConnection::sendUnreliable(const std::vector<uint8_t>& data) {
    // Named pipes are reliable; same as send
    return sendInternal(data);
}

Result<void> NamedPipeConnection::trySend(const std::vector<uint8_t>& data) {
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (data.size() > _maxMessageSize) {
        return Result<void>::err(NetworkError::InvalidParameter, "Message too large");
    }
    return Result<void>::err(NetworkError::WouldBlock, "Non-blocking send not yet supported for NamedPipeConnection");
}

Result<void> NamedPipeConnection::sendInternal(const std::vector<uint8_t>& data) {
#ifdef _WIN32
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (data.size() > _maxMessageSize) {
        return Result<void>::err(NetworkError::InvalidParameter, "Message too large");
    }

    std::lock_guard<std::mutex> lock(_sendMutex);

    // Build frame header (little-endian length)
    uint32_t len = static_cast<uint32_t>(data.size());

    DWORD written = 0;
    BOOL ok = WriteFile(_pipe, &len, sizeof(len), &written, nullptr);
    if (!ok || written != sizeof(len)) {
        DWORD err = GetLastError();
        ENTROPY_LOG_WARNING("WriteFile header failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed, "Write header failed: " + std::to_string(err));
    }

    size_t total = 0;
    const uint8_t* ptr = data.data();
    while (total < data.size()) {
        DWORD toWrite = static_cast<DWORD>(std::min<size_t>(data.size() - total, 64 * 1024));
        written = 0;
        ok = WriteFile(_pipe, ptr + total, toWrite, &written, nullptr);
        if (!ok || written == 0) {
            DWORD err = GetLastError();
            ENTROPY_LOG_WARNING("WriteFile body failed: " + std::to_string(err));
            return Result<void>::err(NetworkError::ConnectionClosed, "Write body failed: " + std::to_string(err));
        }
        total += written;
    }

    auto now = std::chrono::system_clock::now();
    _lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );
    _bytesSent.fetch_add(sizeof(len) + data.size(), std::memory_order_relaxed);
    _messagesSent.fetch_add(1, std::memory_order_relaxed);

    return Result<void>::ok();
#else
    (void)data;
    return Result<void>::err(NetworkError::InvalidParameter, "Named pipes only supported on Windows");
#endif
}

void NamedPipeConnection::receiveLoop() {
#ifdef _WIN32
    std::vector<uint8_t> buffer;
    buffer.reserve(64 * 1024);

    while (!_shouldStop.load(std::memory_order_acquire)) {
        // Read header
        uint32_t len = 0;
        DWORD read = 0;
        BOOL ok = ReadFile(_pipe, &len, sizeof(len), &read, nullptr);
        if (!ok || read == 0) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                // Peer closed
                _state = ConnectionState::Disconnected;
                onStateChanged(ConnectionState::Disconnected);
                break;
            }
            // On other errors, attempt to continue only if not stopping
            if (_shouldStop.load(std::memory_order_acquire)) break;
            ENTROPY_LOG_WARNING("ReadFile header failed: " + std::to_string(err));
            break;
        }

        if (len > _maxMessageSize) {
            ENTROPY_LOG_WARNING("Received message too large on pipe");
            break;
        }

        buffer.resize(len);
        size_t total = 0;
        while (total < len) {
            DWORD toRead = static_cast<DWORD>(std::min<size_t>(len - total, 64 * 1024));
            read = 0;
            ok = ReadFile(_pipe, buffer.data() + total, toRead, &read, nullptr);
            if (!ok || read == 0) {
                DWORD err = GetLastError();
                if (_shouldStop.load(std::memory_order_acquire)) break;
                ENTROPY_LOG_WARNING("ReadFile body failed: " + std::to_string(err));
                break;
            }
            total += read;
        }
        if (total != len) {
            // Incomplete frame; treat as disconnect
            break;
        }

        auto now = std::chrono::system_clock::now();
        _lastActivityTime.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
            std::memory_order_release
        );
        _bytesReceived.fetch_add(FRAME_HEADER_SIZE + len, std::memory_order_relaxed);
        _messagesReceived.fetch_add(1, std::memory_order_relaxed);

        onMessageReceived(buffer);
    }
#endif
}

ConnectionStats NamedPipeConnection::getStats() const {
    ConnectionStats s{};
    s.bytesSent = _bytesSent.load(std::memory_order_relaxed);
    s.bytesReceived = _bytesReceived.load(std::memory_order_relaxed);
    s.messagesSent = _messagesSent.load(std::memory_order_relaxed);
    s.messagesReceived = _messagesReceived.load(std::memory_order_relaxed);
    s.connectTime = _connectTime.load(std::memory_order_relaxed);
    s.lastActivityTime = _lastActivityTime.load(std::memory_order_relaxed);
    return s;
}

} // namespace EntropyEngine::Networking
