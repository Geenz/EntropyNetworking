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
#include "../Core/NetworkTypes.h"
#include "../Core/ConnectionTypes.h"
#include "../Core/ErrorCodes.h"
#include <vector>
#include <functional>
#include <atomic>
#include <memory>

namespace EntropyEngine::Networking {

/**
 * NetworkConnection - Represents a network connection to a peer
 *
 * Derives from EntropyObject for ref counting. Connections are created
 * by ConnectionManager and stamped with appropriate handles.
 */
class NetworkConnection : public Core::EntropyObject {
public:
    using MessageCallback = std::function<void(const std::vector<uint8_t>&)>;
    using StateCallback = std::function<void(ConnectionState)>;

    virtual ~NetworkConnection() = default;

    // Connection lifecycle
    virtual Result<void> connect() = 0;
    virtual Result<void> disconnect() = 0;
    virtual bool isConnected() const = 0;

    // Message transmission
    virtual Result<void> send(const std::vector<uint8_t>& data) = 0;
    virtual Result<void> sendUnreliable(const std::vector<uint8_t>& data) = 0;

    // Non-blocking send API (default: not supported, returns InvalidParameter)
    virtual Result<void> trySend(const std::vector<uint8_t>& data) {
        (void)data;
        return Result<void>::err(NetworkError::InvalidParameter, "trySend not supported by this backend");
    }

    // State and info
    virtual ConnectionState getState() const = 0;
    virtual ConnectionType getType() const = 0;
    virtual ConnectionStats getStats() const = 0;

    // Callbacks
    void setMessageCallback(MessageCallback callback) { _messageCallback = std::move(callback); }
    void setStateCallback(StateCallback callback) { _stateCallback = std::move(callback); }

protected:
    NetworkConnection() = default;

    void onMessageReceived(const std::vector<uint8_t>& data) {
        if (_messageCallback) {
            _messageCallback(data);
        }
    }

    void onStateChanged(ConnectionState state) {
        if (_stateCallback) {
            _stateCallback(state);
        }
    }

private:
    MessageCallback _messageCallback;
    StateCallback _stateCallback;
};

} // namespace EntropyEngine::Networking
