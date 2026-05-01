/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "NamedPipeServer.h"

#ifdef _WIN32

#include <Logging/Logger.h>
#include <windows.h>

#include <chrono>
#include <string>

#include "ConnectionManager.h"
#include "NamedPipeConnection.h"

namespace EntropyEngine::Networking
{

NamedPipeServer::NamedPipeServer(ConnectionManager* connMgr, std::string pipeName)
    : _connMgr(connMgr), _pipeName(normalizePipeName(std::move(pipeName))), _pipeNameWide(toWide(_pipeName)) {}

NamedPipeServer::NamedPipeServer(ConnectionManager* connMgr, std::string pipeName, LocalServerConfig config)
    : _connMgr(connMgr),
      _pipeName(normalizePipeName(std::move(pipeName))),
      _pipeNameWide(toWide(_pipeName)),
      _config(std::move(config)) {}

std::string NamedPipeServer::normalizePipeName(std::string name) const {
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

NamedPipeServer::~NamedPipeServer() {
    (void)close();
}

std::wstring NamedPipeServer::toWide(const std::string& s) const {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w;
    w.resize(static_cast<size_t>(len - 1));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

Result<void> NamedPipeServer::listen() {
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already listening");
    }

    // Create initial instance with configurable buffer sizes (use cached wide string)
    _serverPipe =
        CreateNamedPipeW(_pipeNameWide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                         static_cast<DWORD>(_config.pipeOutBufferSize), static_cast<DWORD>(_config.pipeInBufferSize),
                         0,       // default timeout
                         nullptr  // default security
        );

    if (_serverPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        ENTROPY_LOG_ERROR("CreateNamedPipeW failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed, "CreateNamedPipe failed: " + std::to_string(err));
    }

    _listening.store(true, std::memory_order_release);
    ENTROPY_LOG_INFO(std::string("Named pipe server listening on ") + _pipeName);
    return Result<void>::ok();
}

ConnectionHandle NamedPipeServer::accept() {
    if (!_listening.load(std::memory_order_acquire)) {
        return ConnectionHandle();
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        ENTROPY_LOG_WARNING("Failed to create event for ConnectNamedPipe");
        return ConnectionHandle();
    }

    BOOL connected = FALSE;
    BOOL res = ConnectNamedPipe(_serverPipe, &ov);
    if (res) {
        connected = TRUE;
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            // Wait with polling to support shutdown
            DWORD pollMs = _config.acceptPollIntervalMs > 0 ? static_cast<DWORD>(_config.acceptPollIntervalMs) : 500;
            for (;;) {
                if (!_listening.load(std::memory_order_acquire)) {
                    CancelIoEx(_serverPipe, &ov);
                    CloseHandle(ov.hEvent);
                    return ConnectionHandle();
                }
                DWORD wr = WaitForSingleObject(ov.hEvent, pollMs);
                if (wr == WAIT_OBJECT_0) {
                    DWORD bytes = 0;
                    if (!GetOverlappedResult(_serverPipe, &ov, &bytes, FALSE)) {
                        ENTROPY_LOG_WARNING("GetOverlappedResult failed after ConnectNamedPipe");
                        CloseHandle(ov.hEvent);
                        return ConnectionHandle();
                    }
                    connected = TRUE;
                    break;
                } else if (wr == WAIT_TIMEOUT) {
                    continue;  // poll again
                } else {
                    // WAIT_FAILED or abandoned
                    ENTROPY_LOG_WARNING("WaitForSingleObject failed in accept");
                    CloseHandle(ov.hEvent);
                    return ConnectionHandle();
                }
            }
        } else if (err == ERROR_PIPE_CONNECTED) {
            // Client connected between CreateNamedPipe and ConnectNamedPipe
            SetEvent(ov.hEvent);
            connected = TRUE;
        } else {
            ENTROPY_LOG_WARNING("ConnectNamedPipe failed: " + std::to_string(err));
            CloseHandle(ov.hEvent);
            return ConnectionHandle();
        }
    }

    CloseHandle(ov.hEvent);

    if (!connected) {
        return ConnectionHandle();
    }

    // At this point, _serverPipe is a connected instance; wrap it in a connection
    ENTROPY_LOG_INFO("Accepted Windows named pipe connection");
    auto backend = std::make_unique<NamedPipeConnection>(_serverPipe, "accepted");

    // Prepare next instance for future accepts if still listening (use cached wide string)
    if (_listening.load(std::memory_order_acquire)) {
        HANDLE next = CreateNamedPipeW(_pipeNameWide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                       static_cast<DWORD>(_config.pipeOutBufferSize),
                                       static_cast<DWORD>(_config.pipeInBufferSize), 0, nullptr);
        if (next == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            ENTROPY_LOG_WARNING("Failed to create next pipe instance: " + std::to_string(err));
            // Still return the accepted connection; server won't accept more
            _serverPipe = INVALID_HANDLE_VALUE;
        } else {
            _serverPipe = next;
        }
    }

    return _connMgr->adoptConnection(std::move(backend), ConnectionType::Local);
}

Result<void> NamedPipeServer::close() {
    if (!_listening.load(std::memory_order_acquire)) {
        return Result<void>::ok();
    }

    _listening.store(false, std::memory_order_release);

    if (_serverPipe != INVALID_HANDLE_VALUE) {
        // Cancel any pending IO and close handle
        CancelIoEx(_serverPipe, nullptr);
        DisconnectNamedPipe(_serverPipe);
        CloseHandle(_serverPipe);
        _serverPipe = INVALID_HANDLE_VALUE;
    }

    ENTROPY_LOG_INFO(std::string("Named pipe server closed: ") + _pipeName);
    return Result<void>::ok();
}

uint64_t NamedPipeServer::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(Core::TypeSystem::createTypeId<NamedPipeServer>().id);
    return hash;
}

std::string NamedPipeServer::toString() const {
    return std::string(className()) + "@" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "(name=" + _pipeName +
           ", listening=" + (isListening() ? "true" : "false") + ")";
}

// Factory function implementation for Windows
std::unique_ptr<LocalServer> createLocalServer(ConnectionManager* connMgr, const std::string& endpoint) {
    return std::make_unique<NamedPipeServer>(connMgr, endpoint);
}

std::unique_ptr<LocalServer> createLocalServer(ConnectionManager* connMgr, const std::string& endpoint,
                                               const LocalServerConfig& config) {
    return std::make_unique<NamedPipeServer>(connMgr, endpoint, config);
}

}  // namespace EntropyEngine::Networking

#endif  // _WIN32
