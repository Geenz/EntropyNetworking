/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "NamedPipeServer.h"
#include "NamedPipeConnection.h"
#include "ConnectionManager.h"
#include <Logging/Logger.h>
#include <chrono>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace EntropyEngine::Networking {

NamedPipeServer::NamedPipeServer(ConnectionManager* connMgr, std::string pipeName)
    : _connMgr(connMgr)
    , _pipeName(std::move(pipeName))
{
}

NamedPipeServer::NamedPipeServer(ConnectionManager* connMgr, std::string pipeName, LocalServerConfig config)
    : _connMgr(connMgr)
    , _pipeName(std::move(pipeName))
    , _config(std::move(config))
{
}

NamedPipeServer::~NamedPipeServer() {
    (void)close();
}

std::wstring NamedPipeServer::toWide(const std::string& s) const {
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

Result<void> NamedPipeServer::listen() {
#ifndef _WIN32
    return Result<void>::err(NetworkError::InvalidParameter, "Named pipes only supported on Windows");
#else
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already listening");
    }

    std::wstring wname = toWide(_pipeName);

    // Create initial instance
    _serverPipe = CreateNamedPipeW(
        wname.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        1 * 1024 * 1024,  // out buffer
        1 * 1024 * 1024,  // in buffer
        0,                // default timeout
        nullptr           // default security
    );

    if (_serverPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        ENTROPY_LOG_ERROR("CreateNamedPipeW failed: " + std::to_string(err));
        return Result<void>::err(NetworkError::ConnectionClosed, "CreateNamedPipe failed: " + std::to_string(err));
    }

    _listening.store(true, std::memory_order_release);
    ENTROPY_LOG_INFO(std::string("Named pipe server listening on ") + _pipeName);
    return Result<void>::ok();
#endif
}

ConnectionHandle NamedPipeServer::accept() {
#ifndef _WIN32
    return ConnectionHandle();
#else
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
                    continue; // poll again
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

    // Prepare next instance for future accepts if still listening
    if (_listening.load(std::memory_order_acquire)) {
        std::wstring wname = toWide(_pipeName);
        HANDLE next = CreateNamedPipeW(
            wname.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            1 * 1024 * 1024,
            1 * 1024 * 1024,
            0,
            nullptr
        );
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
#endif
}

Result<void> NamedPipeServer::close() {
#ifndef _WIN32
    return Result<void>::ok();
#else
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
#endif
}

uint64_t NamedPipeServer::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<NamedPipeServer>().id
    );
    return hash;
}

std::string NamedPipeServer::toString() const {
    return std::string(className()) + "@" +
           std::to_string(reinterpret_cast<uintptr_t>(this)) +
           "(name=" + _pipeName +
           ", listening=" + (isListening() ? "true" : "false") + ")";
}

// Factory function implementation for Windows
std::unique_ptr<LocalServer> createLocalServer(
    ConnectionManager* connMgr,
    const std::string& endpoint
) {
#ifdef _WIN32
    return std::make_unique<NamedPipeServer>(connMgr, endpoint);
#else
    (void)connMgr; (void)endpoint;
    throw std::runtime_error("createLocalServer (NamedPipe) called on non-Windows platform");
#endif
}

std::unique_ptr<LocalServer> createLocalServer(
    ConnectionManager* connMgr,
    const std::string& endpoint,
    const LocalServerConfig& config
) {
#ifdef _WIN32
    return std::make_unique<NamedPipeServer>(connMgr, endpoint, config);
#else
    (void)connMgr; (void)endpoint; (void)config;
    throw std::runtime_error("createLocalServer (NamedPipe) called on non-Windows platform");
#endif
}

} // namespace EntropyEngine::Networking
