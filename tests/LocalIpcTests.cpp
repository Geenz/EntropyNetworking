/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdlib>

using namespace EntropyEngine::Networking;

// Integration test for Unix local IPC using LocalServer + ConnectionManager
// This test exercises an actual Unix domain socket on platforms where it's available.
#if defined(_WIN32)
TEST(LocalIpcTests, UnixLocalServerClientEcho) {
    GTEST_SKIP() << "Unix local IPC test skipped on Windows; NamedPipe backend is available but this test targets Unix sockets.";
}
#else
TEST(LocalIpcTests, UnixLocalServerClientEcho) {
    const std::string socketPath = "/tmp/entropy_test_local.sock";

    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, socketPath);
    auto listenRes = server->listen();
    ASSERT_TRUE(listenRes.success()) << listenRes.errorMessage;

    std::atomic<bool> serverAccepted{false};
    std::atomic<bool> serverStop{false};

    std::thread serverThread([&](){
        auto conn = server->accept();
        if (!conn.valid()) {
            return; // accept failed (test will fail later if needed)
        }
        serverAccepted = true;

        // Echo handler
        conn.setMessageCallback([conn](const std::vector<uint8_t>& data) mutable {
            std::string s(data.begin(), data.end());
            std::string response = std::string("Echo: ") + s;
            std::vector<uint8_t> resp(response.begin(), response.end());
            (void)conn.send(resp);
        });

        // Send a welcome message
        {
            std::string welcome = "WELCOME";
            std::vector<uint8_t> w(welcome.begin(), welcome.end());
            (void)conn.send(w);
        }

        // Wait for client to disconnect
        while (!serverStop.load(std::memory_order_acquire) && conn.isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        (void)conn.close();
        (void)server->close();
    });

    // Client
    ConnectionManager clientMgr(8);
    auto client = clientMgr.openLocalConnection(socketPath);
    ASSERT_TRUE(client.valid());

    bool gotWelcome = false;
    bool gotEcho = false;

    client.setMessageCallback([&](const std::vector<uint8_t>& data){
        std::string msg(data.begin(), data.end());
        if (msg == "WELCOME") {
            gotWelcome = true;
        }
        if (msg == "Echo: ping") {
            gotEcho = true;
        }
    });

    auto r = client.connect();
    ASSERT_TRUE(r.success()) << r.errorMessage;

    // Wait for connected state (up to 3s) by polling state (initial Connected may predate callbacks)
    for (int i = 0; i < 300 && !client.isConnected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(client.isConnected());

    // Ensure server accepted
    for (int i=0;i<30 && !serverAccepted.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(serverAccepted.load());

    // Send a ping and wait a bit for echo
    {
        std::string ping = "ping";
        std::vector<uint8_t> data(ping.begin(), ping.end());
        auto sr = client.send(data);
        ASSERT_TRUE(sr.success()) << sr.errorMessage;
    }

    // Wait up to 1s for both messages
    for (int i=0; i<100 && !(gotWelcome && gotEcho); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(gotWelcome);
    EXPECT_TRUE(gotEcho);

    // Stats sanity
    auto stats = client.getStats();
    EXPECT_GT(stats.bytesSent, 0u);
    EXPECT_GT(stats.bytesReceived, 0u);
    EXPECT_GE(stats.messagesSent, 1u);
    EXPECT_GE(stats.messagesReceived, 1u);

    // Shutdown
    (void)client.disconnect();
    serverStop.store(true, std::memory_order_release);
    if (serverThread.joinable()) serverThread.join();
}
#endif
