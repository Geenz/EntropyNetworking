/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryConnection.cpp
 * @brief Implementation of shared memory connection transport
 */

#include "SharedMemoryConnection.h"

#include <EntropyCore.h>

#include <chrono>
#include <cstring>
#include <format>
#include <thread>

namespace EntropyEngine::Networking
{

using namespace SharedMemory;

// ============================================================================
// Construction / Destruction
// ============================================================================

SharedMemoryConnection::SharedMemoryConnection(std::string regionName) : _regionName(std::move(regionName)) {
    ENTROPY_LOG_DEBUG(std::format("SharedMemoryConnection: Created client connection for region '{}'", _regionName));
}

SharedMemoryConnection::SharedMemoryConnection(std::string regionName, const ConnectionConfig* cfg)
    : _regionName(std::move(regionName)) {
    if (cfg) {
        _configuredRegionSize = cfg->sharedMemoryRegionSize;
        _connectTimeoutMs = cfg->sharedMemoryConnectTimeoutMs;
        _waitTimeoutMs = cfg->sharedMemoryWaitTimeoutMs;
        _maxMessageSize = cfg->maxMessageSize;
    }
    ENTROPY_LOG_DEBUG(
        std::format("SharedMemoryConnection: Created client connection for region '{}' (size={}, timeout={}ms)",
                    _regionName, _configuredRegionSize, _connectTimeoutMs));
}

SharedMemoryConnection::SharedMemoryConnection(void* mappedRegion, size_t regionSize, std::string regionName,
                                               NativeHandle handle, std::string peerInfo)
    : _regionName(std::move(regionName)),
      _handle(handle),
      _mappedRegion(mappedRegion),
      _regionSize(regionSize),
      _isServer(true),
      _ownsRegion(true) {
    // Set up pointers into the mapped region
    _control = reinterpret_cast<SharedMemoryControlBlock*>(_mappedRegion);

    // Server uses S2C for sending, C2S for receiving
    auto* dataStart = reinterpret_cast<uint8_t*>(_mappedRegion) + CONTROL_BLOCK_SIZE;
    size_t ringSize = _control->ringBufferSize;

    _sendRing = dataStart;             // S2C ring
    _recvRing = dataStart + ringSize;  // C2S ring

    _sendWritePos = &_control->s2cWritePos;
    _sendReadPos = &_control->s2cReadPos;
    _sendWakeFlag = &_control->s2cWakeFlag;

    _recvWritePos = &_control->c2sWritePos;
    _recvReadPos = &_control->c2sReadPos;
    _recvWakeFlag = &_control->c2sWakeFlag;

    // Mark as connected
    _state.store(ConnectionState::Connected, std::memory_order_release);
    _connectTime.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count()),
                       std::memory_order_relaxed);

    ENTROPY_LOG_INFO(std::format("SharedMemoryConnection: Server adopted connection for region '{}' (peer: {})",
                                 _regionName, peerInfo));
}

SharedMemoryConnection::~SharedMemoryConnection() {
    ENTROPY_LOG_DEBUG(std::format("SharedMemoryConnection: Destroying connection for region '{}'", _regionName));

    // Signal shutdown
    _shouldStop.store(true, std::memory_order_release);

    // Set shutdown flag in control block if we have one
    if (_control) {
        _control->shutdown.store(1, std::memory_order_release);

        // Wake any waiting threads
        if (_sendWakeFlag) {
            PlatformOps::wake(_sendWakeFlag);
        }
        if (_recvWakeFlag) {
            PlatformOps::wakeAll(_recvWakeFlag);
        }
    }

    // Shutdown callbacks to prevent use-after-free
    shutdownCallbacks();

    // Stop receive thread
    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }

    // Clean up region
    if (_ownsRegion && _mappedRegion) {
        PlatformOps::unmapRegion(_mappedRegion, _regionSize);
        _mappedRegion = nullptr;
    }

    if (_ownsRegion && _handle != INVALID_HANDLE_VALUE_SHM) {
        PlatformOps::closeRegion(_handle);
        _handle = INVALID_HANDLE_VALUE_SHM;
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemoryConnection: Destroyed connection for region '{}'", _regionName));
}

// ============================================================================
// Connection Management
// ============================================================================

Result<void> SharedMemoryConnection::connect() {
    if (_state.load(std::memory_order_acquire) == ConnectionState::Connected) {
        return Result<void>::ok();
    }

    if (_isServer) {
        // Server-side connections are already connected
        return Result<void>::ok();
    }

    _state.store(ConnectionState::Connecting, std::memory_order_release);
    onStateChanged(ConnectionState::Connecting);

    ENTROPY_LOG_INFO(std::format("SharedMemoryConnection: Connecting to region '{}'", _regionName));

    // Client-side: Open discovery region and wait for assignment
    std::string discoveryName = _regionName + "_discovery";
    NativeHandle discoveryHandle = PlatformOps::openRegion(discoveryName.c_str());

    if (discoveryHandle == INVALID_HANDLE_VALUE_SHM) {
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable,
                                 std::format("Failed to open discovery region '{}'", discoveryName));
    }

    // Map discovery region
    void* discoveryPtr =
        PlatformOps::mapRegion(discoveryHandle, sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH);
    if (!discoveryPtr) {
        PlatformOps::closeRegion(discoveryHandle);
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable, "Failed to map discovery region");
    }

    auto* discovery = reinterpret_cast<DiscoveryControlBlock*>(discoveryPtr);

    // Validate discovery block
    if (!discovery->validate()) {
        PlatformOps::unmapRegion(discoveryPtr, sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH);
        PlatformOps::closeRegion(discoveryHandle);
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryCorrupted, "Discovery region has invalid magic/version");
    }

    // Check if server is listening
    if (discovery->serverListening.load(std::memory_order_acquire) == 0) {
        PlatformOps::unmapRegion(discoveryPtr, sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH);
        PlatformOps::closeRegion(discoveryHandle);
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::ConnectionClosed, "Server is not listening");
    }

    // Signal that we're waiting
    discovery->clientWaiting.store(1, std::memory_order_release);
    PlatformOps::wake(&discovery->clientWaiting);

    // Wait for server to respond
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(_connectTimeoutMs);
    while (discovery->serverResponded.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            discovery->clientWaiting.store(0, std::memory_order_release);
            PlatformOps::unmapRegion(discoveryPtr, sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH);
            PlatformOps::closeRegion(discoveryHandle);
            _state.store(ConnectionState::Failed, std::memory_order_release);
            onStateChanged(ConnectionState::Failed);
            return Result<void>::err(NetworkError::Timeout, "Timeout waiting for server to assign connection region");
        }
        PlatformOps::waitUntilChanged(&discovery->serverResponded, 0, _waitTimeoutMs);
    }

    // Read assigned region name and security token
    std::string assignedName(discovery->assignedRegionName);
    uint64_t expectedToken = discovery->assignedToken;
    ENTROPY_LOG_DEBUG(std::format("SharedMemoryConnection: Server assigned region '{}'", assignedName));

    // Acknowledge and clean up discovery
    discovery->clientAcknowledged.store(1, std::memory_order_release);
    PlatformOps::wake(&discovery->clientAcknowledged);

    PlatformOps::unmapRegion(discoveryPtr, sizeof(DiscoveryControlBlock) + MAX_REGION_NAME_LENGTH);
    PlatformOps::closeRegion(discoveryHandle);

    // Open assigned connection region
    _handle = PlatformOps::openRegion(assignedName.c_str());
    if (_handle == INVALID_HANDLE_VALUE_SHM) {
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable,
                                 std::format("Failed to open assigned region '{}'", assignedName));
    }

    // Map the connection region
    _regionSize = _configuredRegionSize;
    _mappedRegion = PlatformOps::mapRegion(_handle, _regionSize);
    if (!_mappedRegion) {
        PlatformOps::closeRegion(_handle);
        _handle = INVALID_HANDLE_VALUE_SHM;
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryNotAvailable, "Failed to map connection region");
    }

    _ownsRegion = true;
    _control = reinterpret_cast<SharedMemoryControlBlock*>(_mappedRegion);

    // Validate control block
    if (!_control->validate()) {
        PlatformOps::unmapRegion(_mappedRegion, _regionSize);
        PlatformOps::closeRegion(_handle);
        _mappedRegion = nullptr;
        _handle = INVALID_HANDLE_VALUE_SHM;
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryCorrupted, "Connection region has invalid magic/version");
    }

    // Validate security token matches what server told us in discovery
    if (_control->validationToken != expectedToken) {
        ENTROPY_LOG_ERROR("SharedMemoryConnection: Security token mismatch - possible region hijacking");
        PlatformOps::unmapRegion(_mappedRegion, _regionSize);
        PlatformOps::closeRegion(_handle);
        _mappedRegion = nullptr;
        _handle = INVALID_HANDLE_VALUE_SHM;
        _state.store(ConnectionState::Failed, std::memory_order_release);
        onStateChanged(ConnectionState::Failed);
        return Result<void>::err(NetworkError::SharedMemoryCorrupted, "Security token mismatch");
    }

    // Set our client PID for server validation
    _control->clientPid = getCurrentProcessId();

    // Client uses C2S for sending, S2C for receiving (opposite of server)
    auto* dataStart = reinterpret_cast<uint8_t*>(_mappedRegion) + CONTROL_BLOCK_SIZE;
    size_t ringSize = _control->ringBufferSize;

    _sendRing = dataStart + ringSize;  // C2S ring
    _recvRing = dataStart;             // S2C ring

    _sendWritePos = &_control->c2sWritePos;
    _sendReadPos = &_control->c2sReadPos;
    _sendWakeFlag = &_control->c2sWakeFlag;

    _recvWritePos = &_control->s2cWritePos;
    _recvReadPos = &_control->s2cReadPos;
    _recvWakeFlag = &_control->s2cWakeFlag;

    // Signal client ready
    _control->clientReady.store(1, std::memory_order_release);
    PlatformOps::wake(&_control->clientReady);

    // Wait for server ready
    while (_control->serverReady.load(std::memory_order_acquire) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            PlatformOps::unmapRegion(_mappedRegion, _regionSize);
            PlatformOps::closeRegion(_handle);
            _mappedRegion = nullptr;
            _handle = INVALID_HANDLE_VALUE_SHM;
            _state.store(ConnectionState::Failed, std::memory_order_release);
            onStateChanged(ConnectionState::Failed);
            return Result<void>::err(NetworkError::Timeout, "Timeout waiting for server ready");
        }
        PlatformOps::waitUntilChanged(&_control->serverReady, 0, _waitTimeoutMs);
    }

    // Mark as connected
    _state.store(ConnectionState::Connected, std::memory_order_release);
    _connectTime.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count()),
                       std::memory_order_relaxed);

    // Start receive thread
    _receiveThread = std::thread(&SharedMemoryConnection::receiveLoop, this);

    onStateChanged(ConnectionState::Connected);
    ENTROPY_LOG_INFO(std::format("SharedMemoryConnection: Connected to region '{}'", assignedName));

    return Result<void>::ok();
}

Result<void> SharedMemoryConnection::disconnect() {
    ConnectionState expected = ConnectionState::Connected;
    if (!_state.compare_exchange_strong(expected, ConnectionState::Disconnecting, std::memory_order_acq_rel)) {
        return Result<void>::ok();  // Already disconnected or disconnecting
    }

    onStateChanged(ConnectionState::Disconnecting);
    ENTROPY_LOG_INFO(std::format("SharedMemoryConnection: Disconnecting from region '{}'", _regionName));

    // Signal shutdown
    _shouldStop.store(true, std::memory_order_release);
    if (_control) {
        _control->shutdown.store(1, std::memory_order_release);
        if (_sendWakeFlag) {
            PlatformOps::wake(_sendWakeFlag);
        }
        if (_recvWakeFlag) {
            PlatformOps::wakeAll(_recvWakeFlag);
        }
    }

    // Stop receive thread
    if (_receiveThread.joinable()) {
        _receiveThread.join();
    }

    _state.store(ConnectionState::Disconnected, std::memory_order_release);
    onStateChanged(ConnectionState::Disconnected);

    ENTROPY_LOG_INFO(std::format("SharedMemoryConnection: Disconnected from region '{}'", _regionName));
    return Result<void>::ok();
}

void SharedMemoryConnection::startReceiving() {
    if (_receiveThread.joinable()) {
        return;  // Already started
    }

    if (_state.load(std::memory_order_acquire) != ConnectionState::Connected) {
        ENTROPY_LOG_WARNING("SharedMemoryConnection::startReceiving: Not connected");
        return;
    }

    _receiveThread = std::thread(&SharedMemoryConnection::receiveLoop, this);
    ENTROPY_LOG_DEBUG("SharedMemoryConnection::startReceiving: Receive thread started");
}

// ============================================================================
// Message I/O
// ============================================================================

Result<void> SharedMemoryConnection::send(const std::vector<uint8_t>& data) {
    return writeToRingBuffer(data, true /* blocking */);
}

Result<void> SharedMemoryConnection::sendUnreliable(const std::vector<uint8_t>& data) {
    // Shared memory is always reliable, fall back to send
    return send(data);
}

Result<void> SharedMemoryConnection::trySend(const std::vector<uint8_t>& data) {
    return writeToRingBuffer(data, false /* non-blocking */);
}

Result<void> SharedMemoryConnection::writeToRingBuffer(const std::vector<uint8_t>& data, bool blocking) {
    if (_state.load(std::memory_order_acquire) != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
    }

    if (data.empty()) {
        return Result<void>::ok();
    }

    // Message format: [uint32_t length][payload]
    size_t totalSize = sizeof(uint32_t) + data.size();
    // Max message must leave at least 1 byte free in ring buffer (for wrap detection)
    size_t maxMessage = _control->ringBufferSize - 1;
    if (totalSize > maxMessage) {
        // Message too large for ring buffer
        return Result<void>::err(NetworkError::InvalidParameter,
                                 std::format("Message size {} exceeds maximum {}", totalSize, maxMessage));
    }

    std::lock_guard<std::mutex> lock(_sendMutex);

    size_t ringSize = _control->ringBufferSize;

    // Wait for space in ring buffer
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(_connectTimeoutMs);
    while (true) {
        uint64_t writePos = _sendWritePos->load(std::memory_order_acquire);
        uint64_t readPos = _sendReadPos->load(std::memory_order_acquire);
        size_t available = ringBufferWriteAvailable(writePos, readPos, ringSize);

        if (available >= totalSize) {
            break;
        }

        if (!blocking) {
            return Result<void>::err(NetworkError::WouldBlock, "Ring buffer full");
        }

        if (_control->shutdown.load(std::memory_order_acquire) != 0) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Connection shutdown");
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return Result<void>::err(NetworkError::Timeout, "Timeout waiting for ring buffer space");
        }

        // Poll for reader to consume (can't wait on 64-bit position directly)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Write message to ring buffer
    uint64_t writePos = _sendWritePos->load(std::memory_order_relaxed);

    // Write length prefix
    uint32_t length = static_cast<uint32_t>(data.size());
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        _sendRing[writePos % ringSize] = reinterpret_cast<uint8_t*>(&length)[i];
        writePos++;
    }

    // Write payload
    for (size_t i = 0; i < data.size(); ++i) {
        _sendRing[writePos % ringSize] = data[i];
        writePos++;
    }

    // Update write position (release ensures writes are visible)
    _sendWritePos->store(writePos, std::memory_order_release);

    // Wake receiver
    _sendWakeFlag->store(1, std::memory_order_release);
    PlatformOps::wake(_sendWakeFlag);

    // Update stats
    _bytesSent.fetch_add(data.size(), std::memory_order_relaxed);
    _messagesSent.fetch_add(1, std::memory_order_relaxed);
    _lastActivityTime.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::system_clock::now().time_since_epoch())
                                                      .count()),
                            std::memory_order_relaxed);

    return Result<void>::ok();
}

bool SharedMemoryConnection::readFromRingBuffer(std::vector<uint8_t>& data, int timeoutMs) {
    if (!_control || _state.load(std::memory_order_acquire) != ConnectionState::Connected) {
        return false;
    }

    size_t ringSize = _control->ringBufferSize;

    // Wait for data
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        uint64_t writePos = _recvWritePos->load(std::memory_order_acquire);
        uint64_t readPos = _recvReadPos->load(std::memory_order_relaxed);
        size_t available = ringBufferReadAvailable(writePos, readPos, ringSize);

        if (available >= sizeof(uint32_t)) {
            // Have at least length prefix, try to read message
            break;
        }

        if (_shouldStop.load(std::memory_order_acquire) || _control->shutdown.load(std::memory_order_acquire) != 0) {
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return false;  // Timeout
        }

        // Reset wake flag and wait
        _recvWakeFlag->store(0, std::memory_order_relaxed);
        PlatformOps::waitUntilChanged(_recvWakeFlag, 0, _waitTimeoutMs);
    }

    // Read length prefix
    uint64_t readPos = _recvReadPos->load(std::memory_order_relaxed);
    uint32_t length = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        reinterpret_cast<uint8_t*>(&length)[i] = _recvRing[readPos % ringSize];
        readPos++;
    }

    if (length == 0 || length > _maxMessageSize) {
        ENTROPY_LOG_ERROR(std::format("SharedMemoryConnection: Invalid message length {}", length));
        return false;
    }

    // Wait for full message
    while (true) {
        uint64_t writePos = _recvWritePos->load(std::memory_order_acquire);
        size_t available = ringBufferReadAvailable(writePos, readPos, ringSize);

        if (available >= length) {
            break;
        }

        if (_shouldStop.load(std::memory_order_acquire) || _control->shutdown.load(std::memory_order_acquire) != 0) {
            return false;
        }

        // Wait for more data
        _recvWakeFlag->store(0, std::memory_order_relaxed);
        PlatformOps::waitUntilChanged(_recvWakeFlag, 0, _waitTimeoutMs);
    }

    // Read payload
    data.resize(length);
    for (size_t i = 0; i < length; ++i) {
        data[i] = _recvRing[readPos % ringSize];
        readPos++;
    }

    // Update read position (release ensures reads are complete)
    _recvReadPos->store(readPos, std::memory_order_release);

    // Update stats
    _bytesReceived.fetch_add(length, std::memory_order_relaxed);
    _messagesReceived.fetch_add(1, std::memory_order_relaxed);
    _lastActivityTime.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::system_clock::now().time_since_epoch())
                                                      .count()),
                            std::memory_order_relaxed);

    return true;
}

void SharedMemoryConnection::receiveLoop() {
    ENTROPY_LOG_DEBUG(std::format("SharedMemoryConnection: Receive loop started for region '{}'", _regionName));

    std::vector<uint8_t> message;
    while (!_shouldStop.load(std::memory_order_acquire)) {
        if (_control && _control->shutdown.load(std::memory_order_acquire) != 0) {
            break;
        }

        if (readFromRingBuffer(message, _waitTimeoutMs)) {
            onMessageReceived(message);
            message.clear();
        }
    }

    ENTROPY_LOG_DEBUG(std::format("SharedMemoryConnection: Receive loop ended for region '{}'", _regionName));
}

// ============================================================================
// Statistics
// ============================================================================

ConnectionStats SharedMemoryConnection::getStats() const {
    ConnectionStats stats;
    stats.bytesSent.store(_bytesSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    stats.bytesReceived.store(_bytesReceived.load(std::memory_order_relaxed), std::memory_order_relaxed);
    stats.messagesSent.store(_messagesSent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    stats.messagesReceived.store(_messagesReceived.load(std::memory_order_relaxed), std::memory_order_relaxed);
    stats.connectTime.store(_connectTime.load(std::memory_order_relaxed), std::memory_order_relaxed);
    stats.lastActivityTime.store(_lastActivityTime.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return stats;
}

}  // namespace EntropyEngine::Networking
