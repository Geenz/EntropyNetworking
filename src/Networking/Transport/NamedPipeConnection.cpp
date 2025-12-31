/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#ifdef _WIN32

#include "NamedPipeConnection.h"

#include <Logging/Logger.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../Core/ConnectionTypes.h"

namespace EntropyEngine::Networking
{

static constexpr size_t FRAME_HEADER_SIZE = sizeof(uint32_t);

NamedPipeConnection::NamedPipeConnection(std::string pipeName) : _pipeName(normalizePipeName(std::move(pipeName))) {}

NamedPipeConnection::NamedPipeConnection(std::string pipeName, const ConnectionConfig* cfg)
    : _pipeName(normalizePipeName(std::move(pipeName))) {
    if (cfg) {
        _connectTimeoutMs = cfg->connectTimeoutMs;
        _sendPollTimeoutMs = cfg->sendPollTimeoutMs;
        _sendMaxPolls = cfg->sendMaxPolls;
        _recvIdlePollMs = cfg->recvIdlePollMs;
        _maxMessageSize = cfg->maxMessageSize;
    }
}

std::string NamedPipeConnection::normalizePipeName(std::string name) const {
    // If already in Windows pipe format, return as-is
    if (name.starts_with("\\\\.\\pipe\\") || name.starts_with("\\\\?\\pipe\\")) {
        return name;
    }

    // Extract basename from Unix-style path (e.g., "/tmp/foo.sock" -> "foo.sock")
    size_t lastSlash = name.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        name = name.substr(lastSlash + 1);
    }

    // Remove file extension if present (e.g., "foo.sock" -> "foo")
    size_t lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos) {
        name = name.substr(0, lastDot);
    }

    // Convert to Windows named pipe format
    return "\\\\.\\pipe\\" + name;
}

NamedPipeConnection::NamedPipeConnection(HANDLE connectedPipe, std::string /*peerInfo*/) : _pipe(connectedPipe) {
    _state = ConnectionState::Connected;

    auto now = std::chrono::system_clock::now();
    _connectTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                       std::memory_order_release);
    _lastActivityTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                            std::memory_order_release);

    _shouldStop = false;
    _receiveThread = std::thread([this]() { receiveLoop(); });
}

NamedPipeConnection::~NamedPipeConnection() {
    shutdownCallbacks();
    (void)disconnect();
}

std::wstring NamedPipeConnection::toWide(const std::string& s) const {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w;
    w.resize(static_cast<size_t>(len - 1));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

Result<void> NamedPipeConnection::connect() {
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

    _pipe = CreateFileW(wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

    if (_pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        ENTROPY_LOG_ERROR("CreateFileW on pipe failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed, "CreateFile on pipe failed: " + std::to_string(err));
    }

    // Set to byte mode and blocking
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(_pipe, &mode, nullptr, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(_pipe);
        _pipe = INVALID_HANDLE_VALUE;
        _state = ConnectionState::Failed;
        onStateChanged(ConnectionState::Failed);
        ENTROPY_LOG_ERROR("SetNamedPipeHandleState failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed,
                                 "SetNamedPipeHandleState failed: " + std::to_string(err));
    }

    _state = ConnectionState::Connected;
    onStateChanged(ConnectionState::Connected);

    auto now = std::chrono::system_clock::now();
    _connectTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                       std::memory_order_release);
    _lastActivityTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                            std::memory_order_release);

    _shouldStop = false;
    _receiveThread = std::thread([this]() { receiveLoop(); });

    return Result<void>::ok();
}

Result<void> NamedPipeConnection::disconnect() {
    _shouldStop = true;

    // Cancel any blocking I/O operations to allow receive thread to exit
    if (_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(_pipe, nullptr);
    }

    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }

    if (_state == ConnectionState::Disconnected) {
        if (_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(_pipe);
            _pipe = INVALID_HANDLE_VALUE;
        }
        return Result<void>::ok();
    }

    _state = ConnectionState::Disconnecting;
    onStateChanged(ConnectionState::Disconnecting);

    if (_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(_pipe);
        _pipe = INVALID_HANDLE_VALUE;
    }

    _state = ConnectionState::Disconnected;
    onStateChanged(ConnectionState::Disconnected);
    return Result<void>::ok();
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
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }
    if (data.size() > _maxMessageSize) {
        return Result<void>::err(NetworkError::InvalidParameter, "Message too large");
    }

    std::lock_guard<std::mutex> lock(_sendMutex);

    // Build frame header (native byte order - local IPC only)
    uint32_t len = static_cast<uint32_t>(data.size());

    // Write header with overlapped I/O
    OVERLAPPED ovh{};
    ovh.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ovh.hEvent) {
        return Result<void>::err(NetworkError::ConnectionClosed, "CreateEvent failed for pipe write header");
    }

    DWORD written = 0;
    BOOL ok = WriteFile(_pipe, &len, sizeof(len), &written, &ovh);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD bytes = 0;
            if (!GetOverlappedResult(_pipe, &ovh, &bytes, TRUE)) {
                DWORD e2 = GetLastError();
                CloseHandle(ovh.hEvent);
                if (e2 == ERROR_BROKEN_PIPE || e2 == ERROR_OPERATION_ABORTED) {
                    if (_state.exchange(ConnectionState::Disconnected) != ConnectionState::Disconnected) {
                        onStateChanged(ConnectionState::Disconnected);
                    }
                }
                return Result<void>::err(NetworkError::ConnectionClosed, "Header write failed: " + std::to_string(e2));
            }
            written = bytes;
        } else {
            CloseHandle(ovh.hEvent);
            if (err == ERROR_BROKEN_PIPE || err == ERROR_OPERATION_ABORTED) {
                if (_state.exchange(ConnectionState::Disconnected) != ConnectionState::Disconnected) {
                    onStateChanged(ConnectionState::Disconnected);
                }
            }
            return Result<void>::err(NetworkError::ConnectionClosed, "Write header failed: " + std::to_string(err));
        }
    }
    if (written != sizeof(len)) {
        CloseHandle(ovh.hEvent);
        return Result<void>::err(NetworkError::ConnectionClosed, "Short header write");
    }
    CloseHandle(ovh.hEvent);

    // Write body in chunks with overlapped I/O
    size_t total = 0;
    const uint8_t* ptr = data.data();
    while (total < data.size()) {
        DWORD toWrite = static_cast<DWORD>(std::min<size_t>(data.size() - total, 64 * 1024));
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return Result<void>::err(NetworkError::ConnectionClosed, "CreateEvent failed for pipe write body");
        }
        written = 0;
        ok = WriteFile(_pipe, ptr + total, toWrite, &written, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD bytes = 0;
                if (!GetOverlappedResult(_pipe, &ov, &bytes, TRUE)) {
                    DWORD e2 = GetLastError();
                    CloseHandle(ov.hEvent);
                    if (e2 == ERROR_BROKEN_PIPE || e2 == ERROR_OPERATION_ABORTED) {
                        if (_state.exchange(ConnectionState::Disconnected) != ConnectionState::Disconnected) {
                            onStateChanged(ConnectionState::Disconnected);
                        }
                    }
                    return Result<void>::err(NetworkError::ConnectionClosed,
                                             "Body write failed: " + std::to_string(e2));
                }
                written = bytes;
            } else {
                CloseHandle(ov.hEvent);
                if (err == ERROR_BROKEN_PIPE || err == ERROR_OPERATION_ABORTED) {
                    if (_state.exchange(ConnectionState::Disconnected) != ConnectionState::Disconnected) {
                        onStateChanged(ConnectionState::Disconnected);
                    }
                }
                return Result<void>::err(NetworkError::ConnectionClosed, "Write body failed: " + std::to_string(err));
            }
        }
        CloseHandle(ov.hEvent);
        if (written == 0) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Zero bytes written to pipe body");
        }
        total += written;
    }

    auto now = std::chrono::system_clock::now();
    _lastActivityTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                            std::memory_order_release);
    _bytesSent.fetch_add(sizeof(len) + data.size(), std::memory_order_relaxed);
    _messagesSent.fetch_add(1, std::memory_order_relaxed);

    return Result<void>::ok();
}

void NamedPipeConnection::receiveLoop() {
    std::vector<uint8_t> buffer;
    buffer.reserve(64 * 1024);

    while (!_shouldStop.load(std::memory_order_acquire)) {
        // Read header (native byte order - local IPC only) with overlapped I/O
        uint32_t len = 0;
        OVERLAPPED ovh{};
        ovh.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ovh.hEvent) {
            ENTROPY_LOG_WARNING("CreateEvent failed for pipe read header");
            break;
        }
        DWORD read = 0;
        BOOL ok = ReadFile(_pipe, &len, sizeof(len), &read, &ovh);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD bytes = 0;
                if (!GetOverlappedResult(_pipe, &ovh, &bytes, TRUE)) {
                    DWORD e2 = GetLastError();
                    CloseHandle(ovh.hEvent);
                    if (e2 == ERROR_BROKEN_PIPE) {
                        _state = ConnectionState::Disconnected;
                        onStateChanged(ConnectionState::Disconnected);
                    }
                    break;
                }
                read = bytes;
            } else if (err == ERROR_BROKEN_PIPE) {
                CloseHandle(ovh.hEvent);
                _state = ConnectionState::Disconnected;
                onStateChanged(ConnectionState::Disconnected);
                break;
            } else if (err == ERROR_OPERATION_ABORTED) {
                CloseHandle(ovh.hEvent);
                break;
            } else {
                CloseHandle(ovh.hEvent);
                ENTROPY_LOG_WARNING("ReadFile header failed: " + std::to_string(err));
                break;
            }
        }
        CloseHandle(ovh.hEvent);
        if (read != sizeof(len)) {
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
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) {
                ENTROPY_LOG_WARNING("CreateEvent failed for pipe read body");
                break;
            }
            read = 0;
            ok = ReadFile(_pipe, buffer.data() + total, toRead, &read, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    DWORD bytes = 0;
                    if (!GetOverlappedResult(_pipe, &ov, &bytes, TRUE)) {
                        DWORD e2 = GetLastError();
                        CloseHandle(ov.hEvent);
                        if (e2 == ERROR_OPERATION_ABORTED) {
                            // shutdown/cancel
                            break;
                        }
                        ENTROPY_LOG_WARNING("ReadFile body failed: " + std::to_string(e2));
                        break;
                    }
                    read = bytes;
                } else if (err == ERROR_OPERATION_ABORTED) {
                    CloseHandle(ov.hEvent);
                    break;
                } else {
                    CloseHandle(ov.hEvent);
                    ENTROPY_LOG_WARNING("ReadFile body failed: " + std::to_string(err));
                    break;
                }
            }
            CloseHandle(ov.hEvent);
            if (read == 0) {
                break;
            }
            total += read;
        }
        if (total != len) {
            // Incomplete frame; treat as disconnect
            break;
        }

        auto now = std::chrono::system_clock::now();
        _lastActivityTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                                std::memory_order_release);
        _bytesReceived.fetch_add(FRAME_HEADER_SIZE + len, std::memory_order_relaxed);
        _messagesReceived.fetch_add(1, std::memory_order_relaxed);

        onMessageReceived(buffer);
    }

    // Emit Disconnected if we exited abnormally (not via shouldStop)
    if (!_shouldStop.load(std::memory_order_acquire)) {
        _state = ConnectionState::Disconnected;
        onStateChanged(ConnectionState::Disconnected);
    }
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

}  // namespace EntropyEngine::Networking

#endif  // _WIN32
