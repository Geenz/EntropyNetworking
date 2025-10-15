/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "XPCConnection.h"
#include <chrono>
#include <sstream>
#include <Logging/Logger.h>

#if defined(__APPLE__)

namespace EntropyEngine::Networking {

// Diagnostic helper for XPC object descriptions
static std::string xpcDescribe(xpc_object_t obj) {
    if (!obj) return std::string();
    char* d = xpc_copy_description(obj);
    if (!d) return std::string();
    std::string out(d);
    free(d);
    return out;
}

// Client-side constructor
XPCConnection::XPCConnection(std::string serviceName)
    : _serviceName(std::move(serviceName))
{
    // Create serial queue for XPC event handling
    _queue = dispatch_queue_create("com.entropyengine.xpc.connection", DISPATCH_QUEUE_SERIAL);
}

// Client-side constructor with config
XPCConnection::XPCConnection(std::string serviceName, const ConnectionConfig* cfg)
    : _serviceName(std::move(serviceName))
{
    // Apply config if provided
    if (cfg) {
        _maxMessageSize = cfg->xpcMaxMessageSize;
        _defaultReplyTimeoutMs = cfg->xpcReplyTimeoutMs;
    }
    // Create serial queue for XPC event handling
    _queue = dispatch_queue_create("com.entropyengine.xpc.connection", DISPATCH_QUEUE_SERIAL);
}

// Server-side constructor
XPCConnection::XPCConnection(xpc_connection_t connection, std::string peerInfo)
    : _serviceName(std::move(peerInfo))
    , _connection(connection)
{
    // Retain the connection
    if (_connection) {
        xpc_retain(_connection);
    }

    // Create serial queue for XPC event handling
    _queue = dispatch_queue_create("com.entropyengine.xpc.connection", DISPATCH_QUEUE_SERIAL);

    // CRITICAL: Set target queue for accepted connection
    // Without this, events may be delivered on the default queue
    if (_connection && _queue) {
        xpc_connection_set_target_queue(_connection, _queue);
    }

    // Set up event handler before activating
    setupEventHandler();

    // CRITICAL: Activate/resume the accepted connection to start event delivery
    // Without this call, the connection will never deliver messages or events
    if (_connection) {
        xpc_connection_resume(_connection);
    }

    // Now safe to mark as Connected (connection is active and event handler is set)
    _state = ConnectionState::Connected;
    onStateChanged(ConnectionState::Connected);

    // Record connection time
    auto now = std::chrono::system_clock::now();
    _connectTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );
    _lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );
}

XPCConnection::~XPCConnection() {
    _shouldStop.store(true, std::memory_order_release);
    disconnect();

    if (_connection) {
        xpc_connection_cancel(_connection);
        xpc_release(_connection);
        _connection = nullptr;
    }

    if (_queue) {
        dispatch_release(_queue);
        _queue = nullptr;
    }
}

Result<void> XPCConnection::connect() {
    ConnectionState currentState = _state.load(std::memory_order_acquire);
    if (currentState == ConnectionState::Connected) {
        return Result<void>::ok();
    }

    if (!_connection) {
        // Client-side: create connection to named service
        // The queue is passed here, so target queue is automatically set
        _connection = xpc_connection_create(_serviceName.c_str(), _queue);
        if (!_connection) {
            setState(ConnectionState::Failed);
            return Result<void>::err(NetworkError::ConnectionClosed,
                "Failed to create XPC connection to " + _serviceName);
        }

        // Set up event handler before activation
        setupEventHandler();
    }

    // Transition to Connecting for consistency with other backends
    setState(ConnectionState::Connecting);

    // Activate the connection - this starts event delivery
    // Note: xpc_connection_activate() is preferred over xpc_connection_resume() on modern APIs
    xpc_connection_activate(_connection);

    // XPC connections don't have a traditional "connecting" phase with handshake
    // The connection becomes usable immediately, but errors are delivered asynchronously
    // Transition to Connected optimistically (errors will transition to Failed via event handler)
    setState(ConnectionState::Connected);

    // Record connection time
    auto now = std::chrono::system_clock::now();
    _connectTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );
    _lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
        std::memory_order_release
    );

    return Result<void>::ok();
}

Result<void> XPCConnection::disconnect() {
    if (_state.load(std::memory_order_acquire) == ConnectionState::Disconnected) {
        return Result<void>::ok();
    }

    setState(ConnectionState::Disconnected);

    if (_connection) {
        xpc_connection_cancel(_connection);
    }

    return Result<void>::ok();
}

bool XPCConnection::isConnected() const {
    return _state.load(std::memory_order_acquire) == ConnectionState::Connected;
}

Result<void> XPCConnection::send(const std::vector<uint8_t>& data) {
    if (!isConnected()) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (!_connection) {
        return Result<void>::err(NetworkError::InvalidParameter, "No XPC connection");
    }

    // Create XPC message dictionary
    xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
    if (!message) {
        return Result<void>::err(NetworkError::InvalidMessage, "Failed to create XPC message");
    }

    // Wrap data in xpc_data_t
    xpc_dictionary_set_data(message, "payload", data.data(), data.size());

    // Send message
    xpc_connection_send_message(_connection, message);
    xpc_release(message);

    // Update statistics
    _bytesSent.fetch_add(data.size(), std::memory_order_relaxed);
    _messagesSent.fetch_add(1, std::memory_order_relaxed);
    {
        auto now = std::chrono::system_clock::now();
        _lastActivityTime.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
            std::memory_order_release
        );
    }

    return Result<void>::ok();
}

Result<void> XPCConnection::sendUnreliable(const std::vector<uint8_t>& data) {
    // XPC doesn't have unreliable messaging - fall back to reliable
    return send(data);
}

Result<std::vector<uint8_t>> XPCConnection::sendWithReply(const std::vector<uint8_t>& data,
                                                          std::chrono::milliseconds timeout) {
#if !defined(__APPLE__)
    (void)data; (void)timeout;
    return Result<std::vector<uint8_t>>::err(NetworkError::InvalidParameter, "XPC not supported on this platform");
#else
    if (!isConnected() || !_connection) {
        return Result<std::vector<uint8_t>>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    // Create request
    xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
    if (!message) {
        return Result<std::vector<uint8_t>>::err(NetworkError::InvalidMessage, "Failed to create XPC message");
    }
    xpc_dictionary_set_data(message, "payload", data.data(), data.size());

    // Use async reply with semaphore for timeout
    __block xpc_object_t replyObj = nullptr;
    __block bool replied = false;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(_connection, message, _queue, ^(xpc_object_t reply) {
        replyObj = reply;
        if (replyObj) xpc_retain(replyObj);
        replied = true;
        dispatch_semaphore_signal(sem);
    });

    // release the original message
    xpc_release(message);

    // Wait with timeout
    int64_t ns = static_cast<int64_t>(timeout.count()) * 1000000LL;
    long waitRes = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, ns));
    dispatch_release(sem);

    if (waitRes != 0) {
        // timed out
        ENTROPY_LOG_WARNING(std::string("XPC reply timed out for service ") + _serviceName);
        return Result<std::vector<uint8_t>>::err(NetworkError::Timeout, "XPC reply timed out");
    }

    if (!replied || !replyObj) {
        ENTROPY_LOG_WARNING(std::string("No XPC reply received for service ") + _serviceName);
        return Result<std::vector<uint8_t>>::err(NetworkError::ConnectionClosed, "No reply received");
    }

    // Process reply without capturing __block vars in a C++ lambda
    xpc_type_t t = xpc_get_type(replyObj);
    if (t == XPC_TYPE_DICTIONARY) {
        size_t length = 0;
        const void* bytes = xpc_dictionary_get_data(replyObj, "payload", &length);
        if (!bytes) {
            xpc_release(replyObj);
            return Result<std::vector<uint8_t>>::err(NetworkError::InvalidMessage, "Reply missing payload");
        }
        std::vector<uint8_t> out(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + length);
        _bytesSent.fetch_add(data.size(), std::memory_order_relaxed);
        _messagesSent.fetch_add(1, std::memory_order_relaxed);
        _bytesReceived.fetch_add(length, std::memory_order_relaxed);
        _messagesReceived.fetch_add(1, std::memory_order_relaxed);
        {
            auto now = std::chrono::system_clock::now();
            _lastActivityTime.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
                std::memory_order_release
            );
        }
        xpc_release(replyObj);
        return Result<std::vector<uint8_t>>::ok(std::move(out));
    } else if (t == XPC_TYPE_ERROR) {
        std::string d = xpcDescribe(replyObj);
        ENTROPY_LOG_ERROR(std::string("XPC reply error for service ") + _serviceName +
                          (d.empty() ? std::string("") : std::string(": ") + d));
        if (replyObj == XPC_ERROR_CONNECTION_INTERRUPTED) {
            xpc_release(replyObj);
            return Result<std::vector<uint8_t>>::err(NetworkError::ConnectionClosed, "XPC interrupted");
        }
        if (replyObj == XPC_ERROR_CONNECTION_INVALID) {
            xpc_release(replyObj);
            return Result<std::vector<uint8_t>>::err(NetworkError::ConnectionClosed, "XPC invalid connection");
        }
        xpc_release(replyObj);
        return Result<std::vector<uint8_t>>::err(NetworkError::InvalidMessage, "XPC error reply");
    }
    xpc_release(replyObj);
    return Result<std::vector<uint8_t>>::err(NetworkError::InvalidMessage, "Unexpected XPC reply type");
#endif
}

ConnectionState XPCConnection::getState() const {
    return _state.load(std::memory_order_acquire);
}

ConnectionStats XPCConnection::getStats() const {
    ConnectionStats stats;
    stats.bytesSent = _bytesSent.load(std::memory_order_relaxed);
    stats.bytesReceived = _bytesReceived.load(std::memory_order_relaxed);
    stats.messagesSent = _messagesSent.load(std::memory_order_relaxed);
    stats.messagesReceived = _messagesReceived.load(std::memory_order_relaxed);
    stats.connectTime = _connectTime.load(std::memory_order_relaxed);
    stats.lastActivityTime = _lastActivityTime.load(std::memory_order_relaxed);
    return stats;
}

void XPCConnection::setupEventHandler() {
    if (!_connection) return;

    // Defensively set target queue (client connections already have this set via
    // xpc_connection_create, but server connections need it explicit)
    if (_queue) {
        xpc_connection_set_target_queue(_connection, _queue);
    }

    // Set event handler - this block is retained by XPC
    // LIFETIME NOTE: The block captures `this` pointer. It's safe because:
    // 1. In destructor, we set _shouldStop before calling xpc_connection_cancel
    // 2. xpc_connection_cancel stops event delivery
    // 3. We then release the connection, ensuring no events fire after `this` is destroyed
    xpc_connection_set_event_handler(_connection, ^(xpc_object_t event) {
        // Early exit if connection is being torn down
        if (_shouldStop.load(std::memory_order_acquire)) {
            return;
        }

        xpc_type_t type = xpc_get_type(event);

        if (type == XPC_TYPE_DICTIONARY) {
            // Incoming message
            handleMessage(event);
        } else if (type == XPC_TYPE_ERROR) {
            // Connection error
            handleError(event);
        }
    });
}

void XPCConnection::handleMessage(xpc_object_t message) {
    // Extract payload from dictionary
    size_t length = 0;
    const void* data = xpc_dictionary_get_data(message, "payload", &length);

    // Validate message
    if (!data || length == 0) {
        // Empty or missing payload - ignore
        return;
    }

    // Message size validation (prevent excessive memory allocation)
    if (length > _maxMessageSize) {
        // Message too large - this could be malicious or corrupted
        // Transition to Failed state to signal the error
        setState(ConnectionState::Failed);
        return;
    }

    // Convert to vector
    std::vector<uint8_t> payload(static_cast<const uint8_t*>(data),
                                 static_cast<const uint8_t*>(data) + length);

    // Update statistics
    _bytesReceived.fetch_add(length, std::memory_order_relaxed);
    _messagesReceived.fetch_add(1, std::memory_order_relaxed);
    {
        auto now = std::chrono::system_clock::now();
        _lastActivityTime.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(),
            std::memory_order_release
        );
    }

    // Notify callback
    onMessageReceived(payload);
}

void XPCConnection::handleError(xpc_object_t error) {
    // Gather diagnostics
    pid_t pid = 0;
    if (_connection) {
        pid = xpc_connection_get_pid(_connection);
    }
    std::string desc = xpcDescribe(error);

    if (error == XPC_ERROR_CONNECTION_INVALID) {
        // Connection was cancelled or invalidated
        ENTROPY_LOG_WARNING(std::string("XPC connection invalid: service=") + _serviceName +
                            ", pid=" + std::to_string(pid) +
                            (desc.empty() ? std::string("") : std::string(", detail=") + desc));
        setState(ConnectionState::Disconnected);
    } else if (error == XPC_ERROR_CONNECTION_INTERRUPTED) {
        // Connection interrupted (peer crashed or was killed)
        ENTROPY_LOG_WARNING(std::string("XPC connection interrupted: service=") + _serviceName +
                            ", pid=" + std::to_string(pid) +
                            (desc.empty() ? std::string("") : std::string(", detail=") + desc));
        setState(ConnectionState::Failed);
    } else if (error == XPC_ERROR_TERMINATION_IMMINENT) {
        // Process is about to exit
        ENTROPY_LOG_INFO(std::string("XPC termination imminent: service=") + _serviceName +
                         ", pid=" + std::to_string(pid));
        setState(ConnectionState::Disconnected);
    } else {
        // Unknown XPC error object
        ENTROPY_LOG_ERROR(std::string("XPC unknown error: service=") + _serviceName +
                          ", pid=" + std::to_string(pid) +
                          (desc.empty() ? std::string("") : std::string(", detail=") + desc));
        setState(ConnectionState::Failed);
    }
}

void XPCConnection::setState(ConnectionState newState) {
    ConnectionState oldState = _state.exchange(newState, std::memory_order_acq_rel);
    if (oldState != newState) {
        onStateChanged(newState);
    }
}

uint64_t XPCConnection::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<XPCConnection>().id
    );
    return hash;
}

std::string XPCConnection::toString() const {
    std::ostringstream oss;
    oss << className() << "@" << static_cast<const void*>(this)
        << "(service=" << _serviceName
        << ", state=" << static_cast<int>(_state.load(std::memory_order_relaxed))
        << ")";
    return oss.str();
}

} // namespace EntropyEngine::Networking

#endif // __APPLE__
