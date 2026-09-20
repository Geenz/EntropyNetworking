/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryServer.cpp
 * @brief Implementation of shared memory server
 */

#include "SharedMemoryServer.h"

#include <EntropyCore.h>

#include <chrono>
#include <format>
#include <new>  // For placement new

#include "ConnectionManager.h"
#include "SharedMemoryConnection.h"

namespace EntropyEngine::Networking
{

using namespace SharedMemory;

// ============================================================================
// Construction / Destruction
// ============================================================================

SharedMemoryServer::SharedMemoryServer(ConnectionManager* connMgr, std::string baseName)
    : _connMgr(connMgr), _baseName(std::move(baseName)) {
    ENTROPY_LOG_DEBUG(std::format("SharedMemoryServer: Created server with base name '{}'", _baseName));
}

SharedMemoryServer::SharedMemoryServer(ConnectionManager* connMgr, std::string baseName, const Config& config)
    : _connMgr(connMgr), _baseName(std::move(baseName)), _config(config) {
    ENTROPY_LOG_DEBUG(
        std::format("SharedMemoryServer: Created server with base name '{}' (regionSize={}, timeout={}ms)", _baseName,
                    _config.regionSize, _config.handshakeTimeoutMs));
}

SharedMemoryServer::~SharedMemoryServer() {
    ENTROPY_LOG_DEBUG(std::format("SharedMemoryServer: Destroying server '{}'", _baseName));

    // Signal shutdown
    _shouldStop.store(true, std::memory_order_release);

    if (_discovery) {
        _discovery->serverListening.store(0, std::memory_order_release);
        PlatformOps::wakeAll(&_discovery->clientWaiting);
    }

    // Clean up discovery region
    if (_discoveryRegion) {
        PlatformOps::unmapRegion(_discoveryRegion, sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH);
        _discoveryRegion = nullptr;
    }

    if (_discoveryHandle != INVALID_HANDLE_VALUE_SHM) {
        PlatformOps::closeRegion(_discoveryHandle);
        PlatformOps::destroyRegion(getDiscoveryRegionName().c_str());
        _discoveryHandle = INVALID_HANDLE_VALUE_SHM;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemoryServer: Destroyed server '{}'", _baseName));
}

// ============================================================================
// Server Operations
// ============================================================================

Result<void> SharedMemoryServer::listen() {
    if (_listening.load(std::memory_order_acquire)) {
        return Result<void>::ok();  // Already listening
    }

    ENTROPY_LOG_INFO(std::format("SharedMemoryServer: Starting to listen on '{}'", _baseName));

    // Check if shared memory is available
    if (!PlatformOps::isAvailable()) {
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable,
                                 "Shared memory is not available on this platform");
    }

    // Create discovery region
    std::string discoveryName = getDiscoveryRegionName();
    size_t discoverySize = sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH;

    _discoveryHandle = PlatformOps::createRegion(discoveryName.c_str(), discoverySize);
    if (_discoveryHandle == INVALID_HANDLE_VALUE_SHM) {
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable,
                                 std::format("Failed to create discovery region '{}'", discoveryName));
    }

    _discoveryRegion = PlatformOps::mapRegion(_discoveryHandle, discoverySize);
    if (!_discoveryRegion) {
        PlatformOps::closeRegion(_discoveryHandle);
        PlatformOps::destroyRegion(discoveryName.c_str());
        _discoveryHandle = INVALID_HANDLE_VALUE_SHM;
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable, "Failed to map discovery region");
    }

    // Use placement new to properly start the lifetime of atomic members
    // (reinterpret_cast alone is UB per C++ standard)
    _discovery = new (_discoveryRegion) DiscoveryControlBlock();
    _discovery->initializeServer();
    _discovery->serverListening.store(1, std::memory_order_release);

    _listening.store(true, std::memory_order_release);
    _shouldStop.store(false, std::memory_order_release);

    ENTROPY_LOG_INFO(std::format("SharedMemoryServer: Listening on '{}'", _baseName));
    return Result<void>::ok();
}

ConnectionHandle SharedMemoryServer::accept() {
    std::lock_guard<std::mutex> lock(_acceptMutex);

    if (!_listening.load(std::memory_order_acquire)) {
        ENTROPY_LOG_WARNING("SharedMemoryServer::accept: Server not listening");
        return ConnectionHandle();
    }

    // Wait for a client
    uint32_t connectionId = 0;
    if (!waitForClient(connectionId)) {
        return ConnectionHandle();  // Server closing
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemoryServer: Client detected, creating connection {}", connectionId));

    // Generate random region name and security token for this connection
    std::string regionName = std::format("{}_{}", _baseName, generateRandomHex(12));
    uint64_t securityToken = generateSecurityToken();

    // Create and initialize per-connection region BEFORE signaling client
    NativeHandle connHandle = PlatformOps::createRegion(regionName.c_str(), _config.regionSize);
    if (connHandle == INVALID_HANDLE_VALUE_SHM) {
        ENTROPY_LOG_ERROR(std::format("SharedMemoryServer: Failed to create connection region '{}'", regionName));
        _discovery->serverResponded.store(0, std::memory_order_release);
        _discovery->clientWaiting.store(0, std::memory_order_release);
        return ConnectionHandle();
    }

    void* connRegion = PlatformOps::mapRegion(connHandle, _config.regionSize);
    if (!connRegion) {
        PlatformOps::closeRegion(connHandle);
        PlatformOps::destroyRegion(regionName.c_str());
        ENTROPY_LOG_ERROR("SharedMemoryServer: Failed to map connection region");
        _discovery->serverResponded.store(0, std::memory_order_release);
        _discovery->clientWaiting.store(0, std::memory_order_release);
        return ConnectionHandle();
    }

    // Initialize control block BEFORE telling the client about this region
    // Use placement new to properly start the lifetime of atomic members
    auto* control = new (connRegion) SharedMemoryControlBlock();
    control->initializeServer(_config.regionSize);

    // Set security fields
    control->validationToken = securityToken;
    control->serverPid = getCurrentProcessId();
    control->clientPid = 0;  // Will be set by client

    // Now write assigned region name and token to discovery and signal client
    std::strncpy(_discovery->assignedRegionName, regionName.c_str(), MAX_REGION_NAME_LENGTH - 1);
    _discovery->assignedRegionName[MAX_REGION_NAME_LENGTH - 1] = '\0';
    _discovery->assignedToken = securityToken;
    _discovery->serverResponded.store(1, std::memory_order_release);
    PlatformOps::wake(&_discovery->serverResponded);

    // Wait for client to acknowledge
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(_config.handshakeTimeoutMs);
    while (_discovery->clientAcknowledged.load(std::memory_order_acquire) == 0) {
        if (_shouldStop.load(std::memory_order_acquire)) {
            PlatformOps::unmapRegion(connRegion, _config.regionSize);
            PlatformOps::closeRegion(connHandle);
            PlatformOps::destroyRegion(regionName.c_str());
            return ConnectionHandle();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ENTROPY_LOG_WARNING("SharedMemoryServer: Client acknowledgment timeout");
            PlatformOps::unmapRegion(connRegion, _config.regionSize);
            PlatformOps::closeRegion(connHandle);
            PlatformOps::destroyRegion(regionName.c_str());
            _discovery->serverResponded.store(0, std::memory_order_release);
            _discovery->clientWaiting.store(0, std::memory_order_release);
            return ConnectionHandle();
        }
        PlatformOps::waitUntilChanged(&_discovery->clientAcknowledged, 0, _config.acceptPollIntervalMs);
    }

    // Reset discovery state for next client
    _discovery->clientWaiting.store(0, std::memory_order_release);
    _discovery->serverResponded.store(0, std::memory_order_release);
    _discovery->clientAcknowledged.store(0, std::memory_order_release);

    // Signal server ready
    control->serverReady.store(1, std::memory_order_release);
    PlatformOps::wake(&control->serverReady);

    // Wait for client ready
    while (control->clientReady.load(std::memory_order_acquire) == 0) {
        if (_shouldStop.load(std::memory_order_acquire)) {
            PlatformOps::unmapRegion(connRegion, _config.regionSize);
            PlatformOps::closeRegion(connHandle);
            PlatformOps::destroyRegion(regionName.c_str());
            return ConnectionHandle();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ENTROPY_LOG_WARNING("SharedMemoryServer: Client ready timeout");
            PlatformOps::unmapRegion(connRegion, _config.regionSize);
            PlatformOps::closeRegion(connHandle);
            PlatformOps::destroyRegion(regionName.c_str());
            return ConnectionHandle();
        }
        PlatformOps::waitUntilChanged(&control->clientReady, 0, _config.acceptPollIntervalMs);
    }

    // Create connection object
    std::string peerInfo = std::format("shm:{}", connectionId);
    auto connection =
        std::make_unique<SharedMemoryConnection>(connRegion, _config.regionSize, regionName, connHandle, peerInfo);

    // Adopt into connection manager
    ConnectionHandle handle = _connMgr->adoptConnection(std::move(connection), ConnectionType::Local);

    ENTROPY_LOG_INFO(std::format("SharedMemoryServer: Accepted connection {} (region: {})", connectionId, regionName));

    return handle;
}

Result<void> SharedMemoryServer::close() {
    if (!_listening.load(std::memory_order_acquire)) {
        return Result<void>::ok();  // Already closed
    }

    ENTROPY_LOG_INFO(std::format("SharedMemoryServer: Closing server '{}'", _baseName));

    _shouldStop.store(true, std::memory_order_release);
    _listening.store(false, std::memory_order_release);

    if (_discovery) {
        _discovery->serverListening.store(0, std::memory_order_release);
        PlatformOps::wakeAll(&_discovery->clientWaiting);
    }

    ENTROPY_LOG_INFO(std::format("SharedMemoryServer: Server '{}' closed", _baseName));
    return Result<void>::ok();
}

// ============================================================================
// Helper Methods
// ============================================================================

bool SharedMemoryServer::waitForClient(uint32_t& connectionCounter) {
    while (!_shouldStop.load(std::memory_order_acquire)) {
        if (_discovery->clientWaiting.load(std::memory_order_acquire) != 0) {
            // Client is waiting
            connectionCounter = _discovery->connectionCounter.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        PlatformOps::waitUntilChanged(&_discovery->clientWaiting, 0, _config.acceptPollIntervalMs);
    }

    return false;  // Server closing
}

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<SharedMemoryServer> createSharedMemoryServer(ConnectionManager* connMgr, const std::string& baseName) {
    return std::make_unique<SharedMemoryServer>(connMgr, baseName);
}

std::unique_ptr<SharedMemoryServer> createSharedMemoryServer(ConnectionManager* connMgr, const std::string& baseName,
                                                             const SharedMemoryServer::Config& config) {
    return std::make_unique<SharedMemoryServer>(connMgr, baseName, config);
}

}  // namespace EntropyEngine::Networking
