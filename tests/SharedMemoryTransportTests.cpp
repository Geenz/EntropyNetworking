/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file SharedMemoryTransportTests.cpp
 * @brief Unit tests for shared memory transport
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/SharedMemoryConnection.h"
#include "../src/Networking/Transport/SharedMemoryPlatform.h"
#include "../src/Networking/Transport/SharedMemoryServer.h"

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::SharedMemory;

// ============================================================================
// Platform Operations Tests
// ============================================================================

class SharedMemoryPlatformTest : public ::testing::Test
{
protected:
    void SetUp() override {
        // macOS has a 31-char limit for shm names (including leading /)
        testRegionName = "e_shm_t_" + std::to_string(rand() % 100000);
    }

    void TearDown() override {
        PlatformOps::destroyRegion(testRegionName.c_str());
    }

    std::string testRegionName;
};

TEST_F(SharedMemoryPlatformTest, IsAvailable) {
    EXPECT_TRUE(PlatformOps::isAvailable());
}

TEST_F(SharedMemoryPlatformTest, CreateAndOpenRegion) {
    const size_t regionSize = 4096;

    // Create region
    NativeHandle createHandle = PlatformOps::createRegion(testRegionName.c_str(), regionSize);
    ASSERT_NE(createHandle, INVALID_HANDLE_VALUE_SHM);

    // Map region
    void* ptr = PlatformOps::mapRegion(createHandle, regionSize);
    ASSERT_NE(ptr, nullptr);

    // Write test data
    std::memset(ptr, 0x42, regionSize);

    // Open from "another process" (same process, different handle)
    NativeHandle openHandle = PlatformOps::openRegion(testRegionName.c_str());
    ASSERT_NE(openHandle, INVALID_HANDLE_VALUE_SHM);

    void* ptr2 = PlatformOps::mapRegion(openHandle, regionSize);
    ASSERT_NE(ptr2, nullptr);

    // Verify data is shared
    EXPECT_EQ(static_cast<uint8_t*>(ptr2)[0], 0x42);
    EXPECT_EQ(static_cast<uint8_t*>(ptr2)[regionSize - 1], 0x42);

    // Cleanup
    PlatformOps::unmapRegion(ptr2, regionSize);
    PlatformOps::closeRegion(openHandle);
    PlatformOps::unmapRegion(ptr, regionSize);
    PlatformOps::closeRegion(createHandle);
}

TEST_F(SharedMemoryPlatformTest, WakeWait) {
    const size_t regionSize = 4096;

    NativeHandle handle = PlatformOps::createRegion(testRegionName.c_str(), regionSize);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE_SHM);

    void* ptr = PlatformOps::mapRegion(handle, regionSize);
    ASSERT_NE(ptr, nullptr);

    auto* flag = reinterpret_cast<std::atomic<uint32_t>*>(ptr);
    flag->store(0, std::memory_order_relaxed);

    std::atomic<bool> threadStarted{false};
    std::atomic<bool> threadWoke{false};

    std::thread waiter([&]() {
        threadStarted = true;
        bool changed = PlatformOps::waitUntilChanged(flag, 0, 5000);
        if (changed) {
            threadWoke = true;
        }
    });

    // Wait for thread to start
    while (!threadStarted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Give thread time to enter wait
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Change value and wake
    flag->store(1, std::memory_order_release);
    PlatformOps::wake(flag);

    waiter.join();
    EXPECT_TRUE(threadWoke.load());

    PlatformOps::unmapRegion(ptr, regionSize);
    PlatformOps::closeRegion(handle);
}

TEST_F(SharedMemoryPlatformTest, WaitTimeout) {
    const size_t regionSize = 4096;

    NativeHandle handle = PlatformOps::createRegion(testRegionName.c_str(), regionSize);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE_SHM);

    void* ptr = PlatformOps::mapRegion(handle, regionSize);
    ASSERT_NE(ptr, nullptr);

    auto* flag = reinterpret_cast<std::atomic<uint32_t>*>(ptr);
    flag->store(0, std::memory_order_relaxed);

    auto start = std::chrono::steady_clock::now();
    bool changed = PlatformOps::waitUntilChanged(flag, 0, 100);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(changed);
    EXPECT_GE(elapsed, std::chrono::milliseconds(90));  // Allow some tolerance

    PlatformOps::unmapRegion(ptr, regionSize);
    PlatformOps::closeRegion(handle);
}

// ============================================================================
// Control Block Tests
// ============================================================================

TEST(SharedMemoryControlBlockTest, SizeConstraints) {
    EXPECT_LE(sizeof(SharedMemoryControlBlock), CONTROL_BLOCK_SIZE);
    EXPECT_EQ(sizeof(SharedMemoryControlBlock) % 64, 0);  // Should be cache-line aligned
}

TEST(SharedMemoryControlBlockTest, Initialize) {
    alignas(64) uint8_t buffer[CONTROL_BLOCK_SIZE];
    auto* control = reinterpret_cast<SharedMemoryControlBlock*>(buffer);

    const size_t regionSize = 4 * 1024 * 1024;  // 4 MiB
    control->initializeServer(regionSize);

    EXPECT_TRUE(control->validate());
    EXPECT_EQ(control->magic, SHM_MAGIC);
    EXPECT_EQ(control->version, SHM_VERSION);
    EXPECT_EQ(control->totalSize, regionSize);
    EXPECT_EQ(control->ringBufferSize, (regionSize - CONTROL_BLOCK_SIZE) / 2);

    EXPECT_EQ(control->serverReady.load(), 0u);
    EXPECT_EQ(control->clientReady.load(), 0u);
    EXPECT_EQ(control->shutdown.load(), 0u);

    EXPECT_EQ(control->s2cWritePos.load(), 0u);
    EXPECT_EQ(control->s2cReadPos.load(), 0u);
    EXPECT_EQ(control->c2sWritePos.load(), 0u);
    EXPECT_EQ(control->c2sReadPos.load(), 0u);
}

// ============================================================================
// Ring Buffer Utility Tests
// ============================================================================

TEST(RingBufferUtilsTest, WriteAvailable) {
    const size_t bufferSize = 1024;

    // Empty buffer
    EXPECT_EQ(ringBufferWriteAvailable(0, 0, bufferSize), bufferSize - 1);

    // Partially filled
    EXPECT_EQ(ringBufferWriteAvailable(100, 0, bufferSize), bufferSize - 100 - 1);

    // Wrapped around
    EXPECT_EQ(ringBufferWriteAvailable(100, 200, bufferSize), 99);  // 200-100-1 wrapped
}

TEST(RingBufferUtilsTest, ReadAvailable) {
    const size_t bufferSize = 1024;

    // Empty buffer
    EXPECT_EQ(ringBufferReadAvailable(0, 0, bufferSize), 0u);

    // Some data
    EXPECT_EQ(ringBufferReadAvailable(100, 0, bufferSize), 100u);

    // Wrapped around
    EXPECT_EQ(ringBufferReadAvailable(100, 900, bufferSize), 1024 - 900 + 100);
}

TEST(RingBufferUtilsTest, Advance) {
    const size_t bufferSize = 1024;

    EXPECT_EQ(ringBufferAdvance(0, 100, bufferSize), 100u);
    EXPECT_EQ(ringBufferAdvance(1000, 100, bufferSize), 76u);  // Wrap around
    EXPECT_EQ(ringBufferAdvance(1023, 1, bufferSize), 0u);
}

// ============================================================================
// Server/Client Integration Tests
// ============================================================================

class SharedMemoryTransportTest : public ::testing::Test
{
protected:
    static std::atomic<int> sCounter;

    void SetUp() override {
        // macOS has a 31-char limit for shm names (including leading /)
        // Use PID + counter to ensure unique names across test runs
        int id = sCounter.fetch_add(1, std::memory_order_relaxed);
        baseName = "e_" + std::to_string(getpid() % 10000) + "_" + std::to_string(id);
    }

    void TearDown() override {
        PlatformOps::destroyRegion((baseName + "_discovery").c_str());
        // Note: Connection regions are destroyed by the connection destructors
    }

    std::string baseName;
};

std::atomic<int> SharedMemoryTransportTest::sCounter{0};

TEST_F(SharedMemoryTransportTest, ServerListen) {
    ConnectionManager mgr(8);
    auto server = createSharedMemoryServer(&mgr, baseName);

    auto result = server->listen();
    ASSERT_TRUE(result.success()) << result.errorMessage;
    EXPECT_TRUE(server->isListening());

    result = server->close();
    ASSERT_TRUE(result.success());
    EXPECT_FALSE(server->isListening());
}

TEST_F(SharedMemoryTransportTest, ServerClientConnect) {
    ConnectionManager serverMgr(8);
    ConnectionManager clientMgr(8);

    auto server = createSharedMemoryServer(&serverMgr, baseName);
    auto result = server->listen();
    ASSERT_TRUE(result.success()) << result.errorMessage;

    std::atomic<bool> serverAccepted{false};
    std::atomic<bool> serverStop{false};
    ConnectionHandle serverConn;

    std::thread serverThread([&]() {
        serverConn = server->accept();
        if (serverConn.valid()) {
            serverAccepted = true;
        }

        // Wait for stop signal
        while (!serverStop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Create client connection
    ConnectionConfig cfg;
    cfg.endpoint = baseName;
    cfg.backend = ConnectionBackend::SharedMemory;

    auto clientConn = std::make_unique<SharedMemoryConnection>(baseName, &cfg);
    auto connResult = clientConn->connect();
    ASSERT_TRUE(connResult.success()) << connResult.errorMessage;
    EXPECT_TRUE(clientConn->isConnected());

    // Wait for server to accept
    for (int i = 0; i < 100 && !serverAccepted.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(serverAccepted.load());

    // Disconnect
    clientConn->disconnect();
    serverStop = true;
    serverThread.join();

    server->close();
}

TEST_F(SharedMemoryTransportTest, MessageRoundTrip) {
    ConnectionManager serverMgr(8);
    ConnectionManager clientMgr(8);

    auto server = createSharedMemoryServer(&serverMgr, baseName);
    auto result = server->listen();
    ASSERT_TRUE(result.success()) << result.errorMessage;

    std::atomic<bool> gotMessage{false};
    std::atomic<bool> serverStop{false};
    std::string receivedMessage;
    std::mutex msgMutex;

    std::thread serverThread([&]() {
        auto conn = server->accept();
        if (!conn.valid()) {
            return;
        }

        conn.setMessageCallback([&](const std::vector<uint8_t>& data) {
            std::lock_guard<std::mutex> lock(msgMutex);
            receivedMessage = std::string(data.begin(), data.end());
            gotMessage = true;

            // Echo back
            std::string response = "Echo: " + receivedMessage;
            std::vector<uint8_t> resp(response.begin(), response.end());
            (void)conn.send(resp);
        });

        while (!serverStop.load(std::memory_order_acquire) && conn.isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        conn.close();
    });

    // Create client
    ConnectionConfig cfg;
    cfg.endpoint = baseName;
    cfg.backend = ConnectionBackend::SharedMemory;

    auto client = std::make_unique<SharedMemoryConnection>(baseName, &cfg);

    std::atomic<bool> gotEcho{false};
    client->setMessageCallback([&](const std::vector<uint8_t>& data) {
        std::string msg(data.begin(), data.end());
        if (msg == "Echo: Hello SharedMemory!") {
            gotEcho = true;
        }
    });

    auto connResult = client->connect();
    ASSERT_TRUE(connResult.success()) << connResult.errorMessage;

    // Send message
    std::string testMsg = "Hello SharedMemory!";
    std::vector<uint8_t> msgData(testMsg.begin(), testMsg.end());
    auto sendResult = client->send(msgData);
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    // Wait for server to receive
    for (int i = 0; i < 100 && !gotMessage.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(gotMessage.load());
    {
        std::lock_guard<std::mutex> lock(msgMutex);
        EXPECT_EQ(receivedMessage, testMsg);
    }

    // Wait for echo
    for (int i = 0; i < 100 && !gotEcho.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(gotEcho.load());

    // Cleanup
    client->disconnect();
    serverStop = true;
    serverThread.join();
    server->close();
}

TEST_F(SharedMemoryTransportTest, LargeMessage) {
    ConnectionManager serverMgr(8);
    ConnectionManager clientMgr(8);

    SharedMemoryServer::Config cfg;
    cfg.regionSize = 4 * 1024 * 1024;  // 4 MiB

    auto server = createSharedMemoryServer(&serverMgr, baseName, cfg);
    auto result = server->listen();
    ASSERT_TRUE(result.success()) << result.errorMessage;

    std::atomic<bool> gotLargeMessage{false};
    std::atomic<bool> serverStop{false};
    size_t receivedSize = 0;

    std::thread serverThread([&]() {
        auto conn = server->accept();
        if (!conn.valid()) {
            return;
        }

        conn.setMessageCallback([&](const std::vector<uint8_t>& data) {
            receivedSize = data.size();
            gotLargeMessage = true;
        });

        while (!serverStop.load(std::memory_order_acquire) && conn.isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        conn.close();
    });

    // Create client
    ConnectionConfig clientCfg;
    clientCfg.endpoint = baseName;
    clientCfg.backend = ConnectionBackend::SharedMemory;
    clientCfg.sharedMemoryRegionSize = 4 * 1024 * 1024;

    auto client = std::make_unique<SharedMemoryConnection>(baseName, &clientCfg);
    auto connResult = client->connect();
    ASSERT_TRUE(connResult.success()) << connResult.errorMessage;

    // Send large message (1 MiB - must fit in ring buffer)
    const size_t largeSize = 1024 * 1024;
    std::vector<uint8_t> largeData(largeSize, 0x55);
    auto sendResult = client->send(largeData);
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    // Wait for server to receive
    for (int i = 0; i < 200 && !gotLargeMessage.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(gotLargeMessage.load());
    EXPECT_EQ(receivedSize, largeSize);

    // Cleanup
    client->disconnect();
    serverStop = true;
    serverThread.join();
    server->close();
}

TEST_F(SharedMemoryTransportTest, TrySendBackpressure) {
    ConnectionManager serverMgr(8);
    ConnectionManager clientMgr(8);

    // Small region to trigger backpressure faster
    SharedMemoryServer::Config serverCfg;
    serverCfg.regionSize = 64 * 1024;  // 64 KiB

    auto server = createSharedMemoryServer(&serverMgr, baseName, serverCfg);
    auto result = server->listen();
    ASSERT_TRUE(result.success()) << result.errorMessage;

    std::atomic<bool> serverReady{false};
    std::atomic<bool> serverStop{false};

    std::thread serverThread([&]() {
        auto conn = server->accept();
        if (conn.valid()) {
            serverReady = true;
            // Don't read any messages - let buffer fill up
            while (!serverStop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            conn.close();
        }
    });

    // Create client
    ConnectionConfig clientCfg;
    clientCfg.endpoint = baseName;
    clientCfg.backend = ConnectionBackend::SharedMemory;
    clientCfg.sharedMemoryRegionSize = 64 * 1024;

    auto client = std::make_unique<SharedMemoryConnection>(baseName, &clientCfg);
    auto connResult = client->connect();
    ASSERT_TRUE(connResult.success()) << connResult.errorMessage;

    // Wait for server
    for (int i = 0; i < 100 && !serverReady.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Fill buffer with trySend
    std::vector<uint8_t> data(1024, 0xAA);  // 1 KiB messages
    int successCount = 0;
    int wouldBlockCount = 0;

    for (int i = 0; i < 100; ++i) {
        auto sendResult = client->trySend(data);
        if (sendResult.success()) {
            successCount++;
        } else if (sendResult.error == NetworkError::WouldBlock) {
            wouldBlockCount++;
            break;  // Got backpressure
        }
    }

    EXPECT_GT(successCount, 0);
    EXPECT_GT(wouldBlockCount, 0);

    // Cleanup
    client->disconnect();
    serverStop = true;
    serverThread.join();
    server->close();
}
