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

// Platform-agnostic integration test for local IPC using LocalServer + ConnectionManager
// Uses Unix domain sockets on Unix/macOS, Named Pipes on Windows
TEST(LocalIpcTests, LocalServerClientEcho) {
    // Platform-agnostic endpoint - normalization handled by implementation
    // Unix: /tmp/entropy_test_local.sock
    // Windows: \\.\pipe\entropy_test_local
    const std::string endpoint = "/tmp/entropy_test_local.sock";

    // Server setup
    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, endpoint);
    auto listenRes = server->listen();
    ASSERT_TRUE(listenRes.success()) << listenRes.errorMessage;

    std::atomic<bool> serverAccepted{false};
    std::atomic<bool> serverStop{false};

    std::thread serverThread([&](){
        auto conn = server->accept();
        if (!conn.valid()) {
            return; // accept failed
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
        for (int i = 0; i < 500 && !serverStop.load(std::memory_order_acquire) && conn.isConnected(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        (void)conn.close();
        (void)server->close();
    });

    // Client
    ConnectionManager clientMgr(8);
    auto client = clientMgr.openLocalConnection(endpoint);
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

    // Wait for connected state (up to 3s)
    for (int i = 0; i < 300 && !client.isConnected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(client.isConnected());

    // Ensure server accepted
    for (int i=0; i<50 && !serverAccepted.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(serverAccepted.load());

    // Send a ping and wait for echo
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


// Extended coverage: large payload transfer
TEST(LocalIpcTests, LargePayloadRoundTrip) {
    const std::string endpoint = "/tmp/entropy_test_local.sock";

    // Server
    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, endpoint);
    ASSERT_TRUE(server->listen().success());

    std::atomic<bool> serverAccepted{false};
    std::atomic<bool> gotLarge{false};
    std::atomic<bool> stop{false};

    std::thread st([&]{
        auto conn = server->accept();
        if (!conn.valid()) return;
        serverAccepted = true;
        conn.setMessageCallback([&](const std::vector<uint8_t>& d){
            if (d.size() >= (1u << 20)) {
                gotLarge = true;
                const char* ok = "OK";
                (void)conn.send(std::vector<uint8_t>(ok, ok+2));
            }
        });
        for (int i=0;i<600 && !stop.load() && conn.isConnected();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)conn.close();
        (void)server->close();
    });

    // Client
    ConnectionManager clientMgr(8);
    auto client = clientMgr.openLocalConnection(endpoint);
    ASSERT_TRUE(client.valid());

    std::atomic<bool> gotAck{false};
    client.setMessageCallback([&](const std::vector<uint8_t>& d){
        std::string s(d.begin(), d.end());
        if (s == "OK") gotAck = true;
    });

    ASSERT_TRUE(client.connect().success());
    for (int i=0;i<300 && !client.isConnected();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(client.isConnected());
    for (int i=0;i<100 && !serverAccepted.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(serverAccepted.load());

    // Prepare 2 MiB payload
    const size_t size = 2u * 1024u * 1024u;
    std::vector<uint8_t> payload(size);
    for (size_t i=0;i<size;++i) payload[i] = static_cast<uint8_t>(i & 0xFF);

    auto sr = client.send(payload);
    ASSERT_TRUE(sr.success()) << sr.errorMessage;

    for (int i=0;i<400 && !gotAck.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(gotAck.load());
    EXPECT_TRUE(gotLarge.load());

    auto stats = client.getStats();
    EXPECT_GE(stats.bytesSent, size + 4u);
    EXPECT_GE(stats.messagesSent, 1u);

    (void)client.disconnect();
    stop.store(true);
    if (st.joinable()) st.join();
}

// Extended coverage: multiple back-to-back frames
TEST(LocalIpcTests, MultipleBackToBackFrames) {
    const std::string endpoint = "/tmp/entropy_test_local.sock";

    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, endpoint);
    ASSERT_TRUE(server->listen().success());

    std::atomic<int> receivedCount{0};
    std::atomic<bool> accepted{false};
    std::atomic<bool> stop{false};

    std::thread st([&]{
        auto conn = server->accept();
        if (!conn.valid()) return;
        accepted = true;
        conn.setMessageCallback([&](const std::vector<uint8_t>& d){
            (void)d;
            receivedCount.fetch_add(1);
        });
        for (int i=0;i<400 && !stop.load() && conn.isConnected();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)conn.close();
        (void)server->close();
    });

    ConnectionManager clientMgr(8);
    auto client = clientMgr.openLocalConnection(endpoint);
    ASSERT_TRUE(client.valid());
    ASSERT_TRUE(client.connect().success());
    for (int i=0;i<300 && !client.isConnected();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(client.isConnected());
    for (int i=0;i<100 && !accepted.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(accepted.load());

    // Send 5 small messages rapidly
    for (int i=0;i<5;++i) {
        std::string msg = "m" + std::to_string(i);
        std::vector<uint8_t> v(msg.begin(), msg.end());
        auto r = client.send(v);
        ASSERT_TRUE(r.success()) << r.errorMessage;
    }

    for (int i=0;i<200 && receivedCount.load() < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(receivedCount.load(), 5);

    (void)client.disconnect();
    stop.store(true);
    if (st.joinable()) st.join();
}

// Extended coverage: shutdown during accept should not deadlock
TEST(LocalIpcTests, ShutdownDuringAccept) {
    const std::string endpoint = "/tmp/entropy_test_local.sock";

    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, endpoint);
    ASSERT_TRUE(server->listen().success());

    std::atomic<bool> acceptReturned{false};
    std::atomic<bool> accepted{false};

    std::thread st([&]{
        auto conn = server->accept();
        accepted = conn.valid();
        acceptReturned = true;
    });

    // Give accept a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Close server while accept is pending
    auto cr = server->close();
    ASSERT_TRUE(cr.success()) << cr.errorMessage;

    // Wait up to ~2s for accept to unwind
    for (int i=0;i<200 && !acceptReturned.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(acceptReturned.load());
    EXPECT_FALSE(accepted.load());

    if (st.joinable()) st.join();
}

// Extended coverage: peer disconnect immediately after connect
TEST(LocalIpcTests, PeerDisconnectEarly) {
    const std::string endpoint = "/tmp/entropy_test_local.sock";

    ConnectionManager serverMgr(8);
    auto server = createLocalServer(&serverMgr, endpoint);
    ASSERT_TRUE(server->listen().success());

    std::atomic<bool> serverAccepted{false};
    std::atomic<bool> serverObservedClose{false};

    std::thread st([&]{
        auto conn = server->accept();
        if (!conn.valid()) return;
        serverAccepted = true;
        conn.setMessageCallback([&](const std::vector<uint8_t>&){ /* no-op */ });
        // Poll until the connection reports disconnected
        for (int i=0;i<300 && conn.isConnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        serverObservedClose = !conn.isConnected();
        (void)conn.close();
        (void)server->close();
    });

    // Client connects and immediately disconnects
    ConnectionManager clientMgr(8);
    auto client = clientMgr.openLocalConnection(endpoint);
    ASSERT_TRUE(client.valid());
    ASSERT_TRUE(client.connect().success());
    for (int i=0;i<200 && !client.isConnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(client.isConnected());
    (void)client.disconnect();

    for (int i=0;i<300 && !serverAccepted.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(serverAccepted.load());

    for (int i=0;i<300 && !serverObservedClose.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(serverObservedClose.load());

    if (st.joinable()) st.join();
}
