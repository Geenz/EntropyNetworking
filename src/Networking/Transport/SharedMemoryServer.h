/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryServer.h
 * @brief Server for accepting shared memory connections
 *
 * SharedMemoryServer manages shared memory regions for accepting client connections.
 * Uses a discovery region for connection handshake followed by per-client connection regions.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "LocalServer.h"
#include "SharedMemoryPlatform.h"

namespace EntropyEngine::Networking
{

// Forward declarations
class ConnectionManager;

/**
 * @brief Server for accepting shared memory connections
 *
 * SharedMemoryServer creates a discovery region that clients use to initiate connections.
 * When a client connects:
 * 1. Server detects client waiting in discovery region
 * 2. Server creates a per-connection shared memory region
 * 3. Server assigns the region to the client
 * 4. Both sides proceed with ring buffer communication
 *
 * Thread Safety: All public methods are thread-safe. The accept loop runs on a
 * dedicated thread and uses atomic flags for coordination.
 *
 * @code
 * ConnectionManager connMgr(64);
 * SharedMemoryServerConfig cfg;
 * cfg.regionSize = 4 * 1024 * 1024;  // 4 MiB per connection
 *
 * auto server = std::make_unique<SharedMemoryServer>(&connMgr, "entropy_canvas", cfg);
 * auto result = server->listen();
 * if (result.failed()) { ... }
 *
 * // Accept connections (blocking)
 * while (running) {
 *     auto conn = server->accept();
 *     if (conn.valid()) {
 *         conn.send(data);
 *     }
 * }
 *
 * server->close();
 * @endcode
 */
class SharedMemoryServer : public LocalServer
{
public:
    /**
     * @brief Configuration for shared memory server
     */
    struct Config
    {
        size_t regionSize = SharedMemory::DEFAULT_REGION_SIZE;  ///< Per-connection region size
        int acceptPollIntervalMs = 100;                         ///< Poll interval for accept loop
        int handshakeTimeoutMs = 5000;                          ///< Timeout for client handshake
    };

    /**
     * @brief Constructs server with region base name
     *
     * @param connMgr Connection manager that will own accepted connections
     * @param baseName Base name for shared memory regions
     */
    SharedMemoryServer(ConnectionManager* connMgr, std::string baseName);

    /**
     * @brief Constructs server with configuration
     *
     * @param connMgr Connection manager that will own accepted connections
     * @param baseName Base name for shared memory regions
     * @param config Server configuration
     */
    SharedMemoryServer(ConnectionManager* connMgr, std::string baseName, const Config& config);

    /**
     * @brief Destructor cleans up discovery region
     */
    ~SharedMemoryServer() override;

    // Disable copy
    SharedMemoryServer(const SharedMemoryServer&) = delete;
    SharedMemoryServer& operator=(const SharedMemoryServer&) = delete;

    // LocalServer interface
    Result<void> listen() override;
    ConnectionHandle accept() override;
    Result<void> close() override;
    bool isListening() const override {
        return _listening.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the base name used for regions
     * @return Region base name
     */
    [[nodiscard]] const std::string& getBaseName() const {
        return _baseName;
    }

    /**
     * @brief Get the discovery region name
     * @return Discovery region name ({baseName}_discovery)
     */
    [[nodiscard]] std::string getDiscoveryRegionName() const {
        return _baseName + "_discovery";
    }

private:
    /**
     * @brief Wait for a client to connect via discovery region
     *
     * @param[out] connectionCounter Counter value assigned to this connection
     * @return true if client connected, false if server is closing
     */
    bool waitForClient(uint32_t& connectionCounter);

    ConnectionManager* _connMgr;  ///< Connection manager
    std::string _baseName;        ///< Base name for regions
    Config _config;               ///< Server configuration

    // Discovery region
    SharedMemory::NativeHandle _discoveryHandle{SharedMemory::INVALID_HANDLE_VALUE_SHM};
    void* _discoveryRegion{nullptr};                           ///< Mapped discovery region
    SharedMemory::DiscoveryControlBlock* _discovery{nullptr};  ///< Control block pointer

    // State
    std::atomic<bool> _listening{false};   ///< True if listening
    std::atomic<bool> _shouldStop{false};  ///< Shutdown signal
    mutable std::mutex _acceptMutex;       ///< Serializes accept operations
};

/**
 * @brief Creates a shared memory server
 *
 * @param connMgr Connection manager that will own accepted connections
 * @param baseName Base name for shared memory regions
 * @return Unique pointer to SharedMemoryServer
 */
std::unique_ptr<SharedMemoryServer> createSharedMemoryServer(ConnectionManager* connMgr, const std::string& baseName);

/**
 * @brief Creates a shared memory server with configuration
 *
 * @param connMgr Connection manager that will own accepted connections
 * @param baseName Base name for shared memory regions
 * @param config Server configuration
 * @return Unique pointer to SharedMemoryServer
 */
std::unique_ptr<SharedMemoryServer> createSharedMemoryServer(ConnectionManager* connMgr, const std::string& baseName,
                                                             const SharedMemoryServer::Config& config);

}  // namespace EntropyEngine::Networking
