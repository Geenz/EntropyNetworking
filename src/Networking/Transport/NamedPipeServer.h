/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "LocalServer.h"
#include <string>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif

namespace EntropyEngine::Networking {

/**
 * @brief Windows named pipe server implementation
 *
 * Provides server-side Named Pipe functionality for Windows.
 * Handles pipe creation, listening (ConnectNamedPipe), and accepting connections.
 */
class NamedPipeServer : public LocalServer {
public:
    NamedPipeServer(ConnectionManager* connMgr, std::string pipeName);
    NamedPipeServer(ConnectionManager* connMgr, std::string pipeName, LocalServerConfig config);
    ~NamedPipeServer() override;

    // LocalServer interface
    Result<void> listen() override;
    ConnectionHandle accept() override;
    Result<void> close() override;
    bool isListening() const override { return _listening.load(std::memory_order_acquire); }

    // EntropyObject interface
    const char* className() const noexcept override { return "NamedPipeServer"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    std::wstring toWide(const std::string& s) const;
    std::string normalizePipeName(std::string name) const;

    ConnectionManager* _connMgr;
    std::string _pipeName;
#ifdef _WIN32
    std::wstring _pipeNameWide;  // Cached wide string conversion to avoid redundant toWide() calls
    HANDLE _serverPipe{INVALID_HANDLE_VALUE};
#endif
    std::atomic<bool> _listening{false};
    LocalServerConfig _config{};
};

} // namespace EntropyEngine::Networking
