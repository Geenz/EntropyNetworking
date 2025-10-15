/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "XPCServer.h"
#include "XPCConnection.h"
#include "ConnectionManager.h"
#include <sstream>
#include <Logging/Logger.h>

#if defined(__APPLE__)

namespace EntropyEngine::Networking {

XPCServer::XPCServer(ConnectionManager* connMgr, std::string serviceName)
    : _connMgr(connMgr)
    , _serviceName(std::move(serviceName))
{
    // Create serial queue for XPC events
    _queue = dispatch_queue_create("com.entropyengine.xpc.server", DISPATCH_QUEUE_SERIAL);
}

XPCServer::~XPCServer() {
    close();

    if (_queue) {
        dispatch_release(_queue);
        _queue = nullptr;
    }
}

Result<void> XPCServer::listen() {
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::err(NetworkError::InvalidParameter, "Already listening");
    }

    setupListener();

    if (!_listener) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Failed to create XPC listener");
    }

    _listening.store(true, std::memory_order_release);
    return Result<void>::ok();
}

ConnectionHandle XPCServer::accept() {
    if (!_listening.load(std::memory_order_acquire)) {
        return ConnectionHandle();  // Invalid handle - not listening
    }

    while (_listening.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(_queueMutex);

        // Wait for a connection with timeout for responsiveness
        if (_queueCV.wait_for(lock, std::chrono::milliseconds(500),
                              [this] { return !_pendingConnections.empty() || !_listening; })) {

            // Check if we should stop
            if (!_listening.load(std::memory_order_acquire)) {
                break;
            }

            // Got a connection
            if (!_pendingConnections.empty()) {
                xpc_connection_t connection = _pendingConnections.front();
                _pendingConnections.pop();
                lock.unlock();

                // Wrap in XPCConnection and adopt
                auto backend = std::make_unique<XPCConnection>(connection, "accepted");

                // Release our reference (XPCConnection constructor retains it)
                xpc_release(connection);

                return _connMgr->adoptConnection(std::move(backend), ConnectionType::Local);
            }
        }
    }

    // Server closed while waiting
    return ConnectionHandle();
}

Result<void> XPCServer::close() {
    if (!_listening.load(std::memory_order_acquire)) {
        return Result<void>::ok();
    }

    _listening.store(false, std::memory_order_release);
    _shouldStop.store(true, std::memory_order_release);

    // Wake up accept() if it's waiting
    _queueCV.notify_all();

    if (_listener) {
        xpc_connection_cancel(_listener);
        xpc_release(_listener);
        _listener = nullptr;
    }

    // Clean up pending connections
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        while (!_pendingConnections.empty()) {
            xpc_connection_t conn = _pendingConnections.front();
            _pendingConnections.pop();
            xpc_connection_cancel(conn);
            xpc_release(conn);
        }
    }

    return Result<void>::ok();
}

void XPCServer::setupListener() {
    // Create Mach service listener
    _listener = xpc_connection_create_mach_service(
        _serviceName.c_str(),
        _queue,
        XPC_CONNECTION_MACH_SERVICE_LISTENER
    );

    if (!_listener) {
        return;
    }

    // Set up event handler for incoming connections
    xpc_connection_set_event_handler(_listener, ^(xpc_object_t event) {
        if (_shouldStop.load(std::memory_order_acquire)) {
            return;
        }

        xpc_type_t type = xpc_get_type(event);

        if (type == XPC_TYPE_CONNECTION) {
            // New incoming connection
            handleNewConnection((xpc_connection_t)event);
        } else if (type == XPC_TYPE_ERROR) {
            // Listener error - log it
            ENTROPY_LOG_WARNING("XPC listener error event");
        }
    });

    // Activate the listener
    xpc_connection_resume(_listener);
}

void XPCServer::handleNewConnection(xpc_connection_t connection) {
    if (!connection) return;

    // If validator is set, obtain peer pid and validate
    if (_peerValidator) {
        pid_t pid = xpc_connection_get_pid(connection);
        if (!_peerValidator(pid)) {
            // Reject connection
            xpc_connection_cancel(connection);
            return;
        }
    }

    // Retain the connection
    xpc_retain(connection);

    // Add to pending queue
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _pendingConnections.push(connection);
    }

    // Notify accept() that a connection is available
    _queueCV.notify_one();
}

uint64_t XPCServer::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<XPCServer>().id
    );
    return hash;
}

std::string XPCServer::toString() const {
    std::ostringstream oss;
    oss << className() << "@" << static_cast<const void*>(this)
        << "(service=" << _serviceName
        << ", listening=" << (isListening() ? "true" : "false") << ")";
    return oss.str();
}

} // namespace EntropyEngine::Networking

#endif // __APPLE__
