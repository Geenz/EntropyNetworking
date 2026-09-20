/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryConnection.h
 * @brief Shared memory implementation for high-performance local IPC
 *
 * SharedMemoryConnection provides zero-copy message passing between processes using
 * shared memory ring buffers. Inspired by Wayland's wl_shm buffer pool architecture.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "NetworkConnection.h"
#include "SharedMemoryPlatform.h"

namespace EntropyEngine::Networking
{

/**
 * @brief Shared memory implementation for high-performance local IPC
 *
 * SharedMemoryConnection uses memory-mapped shared regions with lock-free ring buffers
 * for ultra-low-latency message passing. Particularly suited for high-frequency property
 * updates in game engines and graphics applications.
 *
 * Features:
 * - Zero-copy message passing via ring buffers
 * - Lock-free synchronization using platform futex/ulock primitives
 * - Configurable region size (default 4 MiB)
 * - Length-prefixed message framing
 * - Backpressure detection via trySend()
 * - Atomic statistics tracking
 *
 * Platform support: Linux (futex), macOS (ulock), Windows (WaitOnAddress)
 *
 * Thread Safety: All public methods are thread-safe. Send operations are serialized
 * via mutex. Receive thread runs independently and invokes callbacks.
 *
 * Memory Layout:
 * @code
 * +------------------+ offset 0
 * |   Control Block  |  (256 bytes: magic, version, atomics)
 * +------------------+
 * |  Server→Client   |  (half of data region)
 * |   Ring Buffer    |
 * +------------------+
 * |  Client→Server   |  (half of data region)
 * |   Ring Buffer    |
 * +------------------+
 * @endcode
 *
 * @code
 * // Client-side usage
 * ConnectionConfig cfg;
 * cfg.endpoint = "entropy_canvas";
 * cfg.sharedMemoryRegionSize = 8 * 1024 * 1024;  // 8 MiB
 *
 * auto conn = std::make_unique<SharedMemoryConnection>(cfg.endpoint, &cfg);
 * conn->setMessageCallback([](const std::vector<uint8_t>& data) {
 *     std::cout << "Received " << data.size() << " bytes\n";
 * });
 *
 * auto result = conn->connect();
 * if (result.success()) {
 *     std::vector<uint8_t> msg = {'h', 'e', 'l', 'l', 'o'};
 *     conn->send(msg);
 * }
 * @endcode
 */
class SharedMemoryConnection : public NetworkConnection
{
public:
    /**
     * @brief Constructs client-side connection to named region
     *
     * Client will connect to the discovery region and receive a per-connection
     * region assignment from the server.
     *
     * @param regionName Base name for shared memory regions
     */
    explicit SharedMemoryConnection(std::string regionName);

    /**
     * @brief Constructs client-side connection with configuration
     *
     * @param regionName Base name for shared memory regions
     * @param cfg Connection configuration (region size, timeouts, etc.)
     */
    SharedMemoryConnection(std::string regionName, const ConnectionConfig* cfg);

    /**
     * @brief Constructs server-side connection from already-mapped region
     *
     * Used by SharedMemoryServer to wrap accepted client connections.
     * Region is already mapped; no need to call connect().
     *
     * @param mappedRegion Pointer to mapped shared memory region
     * @param regionSize Size of the mapped region
     * @param regionName Name of the region (for cleanup)
     * @param handle Native handle to the region
     * @param peerInfo Identifier for logging/debugging
     */
    SharedMemoryConnection(void* mappedRegion, size_t regionSize, std::string regionName,
                           SharedMemory::NativeHandle handle, std::string peerInfo);

    /**
     * @brief Destructor ensures clean shutdown
     *
     * Signals shutdown, stops receive thread, unmaps region, closes handle.
     */
    ~SharedMemoryConnection() override;

    // Disable copy
    SharedMemoryConnection(const SharedMemoryConnection&) = delete;
    SharedMemoryConnection& operator=(const SharedMemoryConnection&) = delete;

    // NetworkConnection interface
    Result<void> connect() override;
    Result<void> disconnect() override;
    bool isConnected() const override {
        return _state.load(std::memory_order_acquire) == ConnectionState::Connected;
    }

    Result<void> send(const std::vector<uint8_t>& data) override;
    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
    Result<void> trySend(const std::vector<uint8_t>& data) override;

    ConnectionState getState() const override {
        return _state.load(std::memory_order_acquire);
    }
    ConnectionType getType() const override {
        return ConnectionType::Local;
    }
    ConnectionStats getStats() const override;

    /**
     * @brief Starts the receive thread for adopted connections
     *
     * Called by ConnectionManager::adoptConnection() AFTER callbacks are set.
     * For connections created via connect(), receive thread starts in connect().
     */
    void startReceiving() override;

    /**
     * @brief Get the region name
     * @return Region name used for this connection
     */
    [[nodiscard]] const std::string& getRegionName() const {
        return _regionName;
    }

    /**
     * @brief Get the region size
     * @return Size of the shared memory region in bytes
     */
    [[nodiscard]] size_t getRegionSize() const {
        return _regionSize;
    }

private:
    /**
     * @brief Receive thread main loop
     *
     * Polls ring buffer for incoming messages and invokes message callback.
     * Uses futex/ulock for efficient waiting.
     */
    void receiveLoop();

    /**
     * @brief Write data to the send ring buffer
     *
     * @param data Message bytes to write
     * @param blocking If true, wait for space; if false, return WouldBlock
     * @return Result indicating success or failure
     */
    Result<void> writeToRingBuffer(const std::vector<uint8_t>& data, bool blocking);

    /**
     * @brief Read a message from the receive ring buffer
     *
     * @param[out] data Vector to receive message bytes
     * @param timeoutMs Maximum time to wait for a message
     * @return true if message was read, false on timeout or shutdown
     */
    bool readFromRingBuffer(std::vector<uint8_t>& data, int timeoutMs);

    // Region state
    std::string _regionName;                                                     ///< Shared memory region name
    SharedMemory::NativeHandle _handle{SharedMemory::INVALID_HANDLE_VALUE_SHM};  ///< Native handle
    void* _mappedRegion{nullptr};                                                ///< Mapped memory pointer
    size_t _regionSize{0};                                                       ///< Total region size
    bool _isServer{false};                                                       ///< True if server-side connection
    bool _ownsRegion{false};  ///< True if we should unmap/close on destroy

    // Ring buffer pointers (derived from mapped region)
    SharedMemory::SharedMemoryControlBlock* _control{nullptr};  ///< Control block
    uint8_t* _sendRing{nullptr};                                ///< Pointer to send ring buffer
    uint8_t* _recvRing{nullptr};                                ///< Pointer to receive ring buffer

    // Atomic pointers for lock-free access (point into control block)
    std::atomic<uint64_t>* _sendWritePos{nullptr};  ///< Our write position
    std::atomic<uint64_t>* _sendReadPos{nullptr};   ///< Peer's read position (their view)
    std::atomic<uint32_t>* _sendWakeFlag{nullptr};  ///< Wake flag for peer
    std::atomic<uint64_t>* _recvWritePos{nullptr};  ///< Peer's write position
    std::atomic<uint64_t>* _recvReadPos{nullptr};   ///< Our read position
    std::atomic<uint32_t>* _recvWakeFlag{nullptr};  ///< Wake flag for us

    // Connection state
    std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
    std::thread _receiveThread;
    std::atomic<bool> _shouldStop{false};
    mutable std::mutex _sendMutex;

    // Configuration
    size_t _configuredRegionSize{SharedMemory::DEFAULT_REGION_SIZE};
    int _connectTimeoutMs{5000};
    int _waitTimeoutMs{100};
    size_t _maxMessageSize{16ull * 1024ull * 1024ull};

    // Statistics
    std::atomic<uint64_t> _bytesSent{0};
    std::atomic<uint64_t> _bytesReceived{0};
    std::atomic<uint64_t> _messagesSent{0};
    std::atomic<uint64_t> _messagesReceived{0};
    std::atomic<uint64_t> _connectTime{0};
    std::atomic<uint64_t> _lastActivityTime{0};
};

}  // namespace EntropyEngine::Networking
