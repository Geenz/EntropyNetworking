/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"

using namespace EntropyEngine::Networking;

// Platform-agnostic integration test validating aggregate manager metrics and trySend semantics.
// Uses Unix domain sockets on Unix/macOS, Named Pipes on Windows
TEST(ManagerMetricsTests, LocalIpcMetricsAndTrySendWouldBlock) {
    // Platform-agnostic endpoint - normalization handled by implementation
    const std::string socketPath = "/tmp/entropy_metrics_test.sock";

    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, socketPath);
    ASSERT_TRUE(server->listen().success());

    std::atomic<bool> serverAccepted{false};
    std::thread serverThread([&] {
        auto conn = server->accept();
        if (!conn.valid()) return;
        serverAccepted = true;
        // Echo back any payloads
        conn.setMessageCallback([conn](const std::vector<uint8_t>& data) mutable { (void)conn.send(data); });
        while (conn.isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        (void)server->close();
    });

    ConnectionManager clientMgr(8);
    auto client = clientMgr.openLocalConnection(socketPath);
    ASSERT_TRUE(client.valid());

    // Connect client
    ASSERT_TRUE(client.connect().success());
    // Wait up to 1s to be connected
    for (int i = 0; i < 100 && !client.isConnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(client.isConnected());

    // Ensure server accepted
    for (int i = 0; i < 100 && !serverAccepted.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(serverAccepted.load());

    // Send a message
    std::vector<uint8_t> payload{'p', 'i', 'n', 'g'};
    ASSERT_TRUE(client.send(payload).success());

    // Give it a moment for echo and metric increments
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Manager metrics should show activity
    auto cm = clientMgr.getManagerMetrics();
    EXPECT_GE(cm.totalBytesSent, 1u);
    EXPECT_GE(cm.totalMessagesSent, 1u);

    auto sm = serverMgr.getManagerMetrics();
    EXPECT_GE(sm.totalBytesReceived, 1u);
    EXPECT_GE(sm.totalMessagesReceived, 1u);

    // trySend currently returns WouldBlock by design (both Unix and Windows don't support partial non-blocking writes)
    auto tr = client.trySend(payload);
    EXPECT_FALSE(tr.success());
    EXPECT_EQ(tr.error, NetworkError::WouldBlock);

    (void)client.disconnect();
    if (serverThread.joinable()) serverThread.join();
}
