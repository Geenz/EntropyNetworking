/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryPlatform.h
 * @brief Cross-platform shared memory abstraction
 *
 * Platform abstraction layer for shared memory operations. Provides a unified API
 * for creating, mapping, and synchronizing shared memory regions across POSIX
 * (Linux/macOS) and Windows platforms.
 *
 * Design inspired by Wayland's wl_shm buffer pool architecture for high-throughput,
 * low-latency local IPC.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace EntropyEngine::Networking::SharedMemory
{

// ============================================================================
// Platform-Specific Handle Types
// ============================================================================

#ifdef _WIN32
using NativeHandle = HANDLE;
static constexpr NativeHandle INVALID_HANDLE_VALUE_SHM = INVALID_HANDLE_VALUE;
#else
using NativeHandle = int;
static constexpr NativeHandle INVALID_HANDLE_VALUE_SHM = -1;
#endif

// ============================================================================
// Constants
// ============================================================================

/// Magic number for control block validation ("ENTR" in little-endian)
static constexpr uint32_t SHM_MAGIC = 0x52544E45;  // "ENTR"

/// Current protocol version
static constexpr uint32_t SHM_VERSION = 1;

/// Default shared memory region size (4 MiB)
static constexpr size_t DEFAULT_REGION_SIZE = 4ull * 1024ull * 1024ull;

/// Control block size (256 bytes, cache-aligned)
static constexpr size_t CONTROL_BLOCK_SIZE = 256;

/// Maximum region name length
static constexpr size_t MAX_REGION_NAME_LENGTH = 256;

/// Timeout for wake/wait operations (ms)
static constexpr int DEFAULT_WAIT_TIMEOUT_MS = 100;

// ============================================================================
// Security Utilities
// ============================================================================

/**
 * @brief Generate a cryptographically random 64-bit token
 * @return Random token for connection validation
 */
inline uint64_t generateSecurityToken() {
    std::random_device rd;
    std::uniform_int_distribution<uint64_t> dist;
    return dist(rd);
}

/**
 * @brief Generate a random hex string for region names
 * @param length Number of hex characters (default 16 = 64 bits)
 * @return Random hex string
 */
inline std::string generateRandomHex(size_t length = 16) {
    static constexpr char hexChars[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += hexChars[dist(rd)];
    }
    return result;
}

/**
 * @brief Get current process ID (cross-platform)
 * @return Process ID as uint32_t
 */
uint32_t getCurrentProcessId();

// ============================================================================
// Shared Memory Control Block
// ============================================================================

/**
 * @brief Control block structure placed at the start of each shared memory region
 *
 * Layout is fixed and must match across all platforms. Structure is organized
 * into cache lines to prevent false sharing:
 *
 * Cache line 0 [0-63]:   Header + connection state (read-mostly)
 * Cache line 1 [64-127]: Server-to-Client ring buffer atomics
 * Cache line 2 [128-191]: Client-to-Server ring buffer atomics
 * Cache line 3 [192-255]: Reserved for future use
 */
struct alignas(64) SharedMemoryControlBlock
{
    // ========== Cache Line 0: Header + Connection State ==========
    uint32_t magic;           ///< Must be SHM_MAGIC
    uint32_t version;         ///< Must be <= SHM_VERSION
    uint64_t totalSize;       ///< Total region size
    uint64_t ringBufferSize;  ///< Size of each ring buffer

    std::atomic<uint32_t> serverReady;  ///< Server has initialized region
    std::atomic<uint32_t> clientReady;  ///< Client has connected
    std::atomic<uint32_t> shutdown;     ///< Shutdown requested
    uint32_t reserved0;                 ///< Reserved for alignment
    uint8_t padding0[24];               ///< Pad to 64 bytes

    // ========== Cache Line 1: Server-to-Client Ring State ==========
    std::atomic<uint64_t> s2cWritePos;  ///< Server write position
    std::atomic<uint64_t> s2cReadPos;   ///< Client read position
    std::atomic<uint32_t> s2cWakeFlag;  ///< Wake flag for client
    uint32_t reserved1;                 ///< Reserved for alignment
    uint8_t padding1[40];               ///< Pad to 64 bytes

    // ========== Cache Line 2: Client-to-Server Ring State ==========
    std::atomic<uint64_t> c2sWritePos;  ///< Client write position
    std::atomic<uint64_t> c2sReadPos;   ///< Server read position
    std::atomic<uint32_t> c2sWakeFlag;  ///< Wake flag for server
    uint32_t reserved2;                 ///< Reserved for alignment
    uint8_t padding2[40];               ///< Pad to 64 bytes

    // ========== Cache Line 3: Security ==========
    uint64_t validationToken;  ///< Random token for connection validation
    uint32_t serverPid;        ///< Server process ID
    uint32_t clientPid;        ///< Client process ID (set after connect)

    /**
     * @brief Initialize control block for server
     * @param regionSize Total size of the shared memory region
     */
    void initializeServer(size_t regionSize) {
        magic = SHM_MAGIC;
        version = SHM_VERSION;
        totalSize = regionSize;
        // Each ring buffer gets half the data region (after control block)
        ringBufferSize = (regionSize - CONTROL_BLOCK_SIZE) / 2;

        serverReady.store(0, std::memory_order_relaxed);
        clientReady.store(0, std::memory_order_relaxed);
        shutdown.store(0, std::memory_order_relaxed);
        reserved0 = 0;

        s2cWritePos.store(0, std::memory_order_relaxed);
        s2cReadPos.store(0, std::memory_order_relaxed);
        s2cWakeFlag.store(0, std::memory_order_relaxed);
        reserved1 = 0;

        c2sWritePos.store(0, std::memory_order_relaxed);
        c2sReadPos.store(0, std::memory_order_relaxed);
        c2sWakeFlag.store(0, std::memory_order_relaxed);
        reserved2 = 0;

        validationToken = 0;  // Set by server after initialization
        serverPid = 0;        // Set by server after initialization
        clientPid = 0;        // Set by client after connection

        std::memset(padding0, 0, sizeof(padding0));
        std::memset(padding1, 0, sizeof(padding1));
        std::memset(padding2, 0, sizeof(padding2));

        // Memory fence to ensure all writes are visible
        std::atomic_thread_fence(std::memory_order_release);
    }

    /**
     * @brief Validate control block was initialized correctly
     * @return true if magic and version are valid
     */
    [[nodiscard]] bool validate() const {
        return magic == SHM_MAGIC && version <= SHM_VERSION;
    }
};

static_assert(sizeof(SharedMemoryControlBlock) <= CONTROL_BLOCK_SIZE, "Control block exceeds allocated size");

// ============================================================================
// Discovery Region Control Block
// ============================================================================

/**
 * @brief Control block for the discovery/handshake region
 *
 * Server creates a discovery region that clients use to initiate connections.
 * The handshake flow:
 * 1. Server creates discovery region with baseName_discovery
 * 2. Client opens discovery region, sets clientWaiting=1
 * 3. Server detects waiting client, creates per-connection region
 * 4. Server writes region name to assignedRegionName, sets serverResponded=1
 * 5. Client reads assignedRegionName, opens that region
 * 6. Client sets clientAcknowledged=1, both proceed with ring buffer protocol
 */
struct alignas(64) DiscoveryControlBlock
{
    // ========== Cache Line 0: Header + State ==========
    uint32_t magic;    ///< Must be SHM_MAGIC
    uint32_t version;  ///< Must be SHM_VERSION

    std::atomic<uint32_t> serverListening;     ///< Server is ready for connections
    std::atomic<uint32_t> clientWaiting;       ///< Client is waiting for assignment
    std::atomic<uint32_t> serverResponded;     ///< Server has assigned a region
    std::atomic<uint32_t> clientAcknowledged;  ///< Client has opened assigned region
    std::atomic<uint32_t> connectionCounter;   ///< Monotonic counter for region names
    uint32_t reserved;                         ///< Reserved for alignment
    uint64_t assignedToken;                    ///< Validation token for assigned region

    // ========== Remaining space: Assigned region name ==========
    /// Assigned region name (written by server after clientWaiting=1)
    char assignedRegionName[MAX_REGION_NAME_LENGTH];

    /**
     * @brief Initialize discovery block for server
     */
    void initializeServer() {
        magic = SHM_MAGIC;
        version = SHM_VERSION;
        serverListening.store(0, std::memory_order_relaxed);
        clientWaiting.store(0, std::memory_order_relaxed);
        serverResponded.store(0, std::memory_order_relaxed);
        clientAcknowledged.store(0, std::memory_order_relaxed);
        connectionCounter.store(0, std::memory_order_relaxed);
        reserved = 0;
        assignedToken = 0;  // Set per-connection by server
        std::memset(assignedRegionName, 0, sizeof(assignedRegionName));

        std::atomic_thread_fence(std::memory_order_release);
    }

    /**
     * @brief Validate discovery block
     * @return true if magic and version are valid
     */
    [[nodiscard]] bool validate() const {
        return magic == SHM_MAGIC && version <= SHM_VERSION;
    }
};

// ============================================================================
// Platform Operations (Implemented per-platform)
// ============================================================================

/**
 * @brief Platform-specific shared memory operations
 *
 * All methods are static and thread-safe. Each platform provides its own
 * implementation in SharedMemoryPlatformUnix.cpp or SharedMemoryPlatformWin32.cpp.
 */
struct PlatformOps
{
    /**
     * @brief Create a new shared memory region
     *
     * Creates a new shared memory region with the given name. The region is
     * created with read/write permissions for the owner.
     *
     * @param name Region name (platform-specific prefixing is handled internally)
     * @param size Size of the region in bytes
     * @return Handle to the created region, or INVALID_HANDLE_VALUE_SHM on failure
     */
    static NativeHandle createRegion(const char* name, size_t size);

    /**
     * @brief Open an existing shared memory region
     *
     * Opens a shared memory region previously created by another process.
     *
     * @param name Region name (must match createRegion name)
     * @return Handle to the opened region, or INVALID_HANDLE_VALUE_SHM on failure
     */
    static NativeHandle openRegion(const char* name);

    /**
     * @brief Map a shared memory region into process address space
     *
     * @param handle Handle from createRegion or openRegion
     * @param size Size of the region to map
     * @return Pointer to mapped memory, or nullptr on failure
     */
    static void* mapRegion(NativeHandle handle, size_t size);

    /**
     * @brief Unmap a previously mapped region
     *
     * @param ptr Pointer returned by mapRegion
     * @param size Size of the mapped region
     */
    static void unmapRegion(void* ptr, size_t size);

    /**
     * @brief Close a region handle
     *
     * Does not destroy the region if other processes have it open.
     *
     * @param handle Handle to close
     */
    static void closeRegion(NativeHandle handle);

    /**
     * @brief Destroy a shared memory region
     *
     * Removes the region from the filesystem/namespace. Existing mappings
     * remain valid until unmapped.
     *
     * @param name Region name
     */
    static void destroyRegion(const char* name);

    // ========================================================================
    // Synchronization Primitives
    // ========================================================================

    /**
     * @brief Wake threads waiting on an atomic flag
     *
     * Uses platform-specific futex (Linux), ulock (macOS), or WaitOnAddress (Windows).
     *
     * @param addr Address of the atomic flag
     */
    static void wake(std::atomic<uint32_t>* addr);

    /**
     * @brief Wake all threads waiting on an atomic flag
     *
     * @param addr Address of the atomic flag
     */
    static void wakeAll(std::atomic<uint32_t>* addr);

    /**
     * @brief Wait until an atomic flag changes from expected value
     *
     * Blocks until the value at addr is no longer equal to expected, or timeout expires.
     *
     * @param addr Address of the atomic flag
     * @param expected Value to wait for change from
     * @param timeoutMs Maximum wait time in milliseconds (-1 for infinite)
     * @return true if value changed, false if timed out
     */
    static bool waitUntilChanged(std::atomic<uint32_t>* addr, uint32_t expected, int timeoutMs);

    // ========================================================================
    // Utility Functions
    // ========================================================================

    /**
     * @brief Get the last platform error as a string
     *
     * Returns a human-readable string for the last platform-specific error.
     *
     * @return Error description
     */
    static std::string getLastErrorString();

    /**
     * @brief Check if shared memory is available on this platform
     *
     * @return true if shared memory operations are supported
     */
    static bool isAvailable();
};

// ============================================================================
// Ring Buffer Utilities
// ============================================================================

/**
 * @brief Calculate available space in a ring buffer for writing
 *
 * Uses monotonic 64-bit counters that wrap via natural overflow. The actual ring
 * buffer position is computed as (counter % bufferSize) when accessing memory.
 * This eliminates wrap-around edge case logic in availability calculations.
 *
 * @param writePos Current write position (monotonic counter)
 * @param readPos Current read position (monotonic counter)
 * @param bufferSize Total buffer size
 * @return Number of bytes available for writing
 */
inline size_t ringBufferWriteAvailable(uint64_t writePos, uint64_t readPos, size_t bufferSize) {
    // Monotonic counters: used bytes is simply the difference
    // Reserve one byte to distinguish full from empty
    size_t used = static_cast<size_t>(writePos - readPos);
    return bufferSize - used - 1;
}

/**
 * @brief Calculate available data in a ring buffer for reading
 *
 * Uses monotonic 64-bit counters. The difference between write and read positions
 * gives the number of unread bytes directly.
 *
 * @param writePos Current write position (monotonic counter)
 * @param readPos Current read position (monotonic counter)
 * @param bufferSize Total buffer size (unused, kept for API consistency)
 * @return Number of bytes available for reading
 */
inline size_t ringBufferReadAvailable(uint64_t writePos, uint64_t readPos, [[maybe_unused]] size_t bufferSize) {
    // Monotonic counters: available bytes is simply the difference
    return static_cast<size_t>(writePos - readPos);
}

/**
 * @brief Advance a ring buffer position
 *
 * Simply adds the amount to the monotonic counter. Actual memory access
 * should use (pos % bufferSize) to get the ring buffer index.
 *
 * @param pos Current position (monotonic counter)
 * @param amount Amount to advance
 * @param bufferSize Total buffer size (unused, kept for API consistency)
 * @return New position (monotonic counter)
 */
inline uint64_t ringBufferAdvance(uint64_t pos, size_t amount, [[maybe_unused]] size_t bufferSize) {
    return pos + amount;
}

}  // namespace EntropyEngine::Networking::SharedMemory
