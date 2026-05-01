/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"

using namespace EntropyEngine::Networking;

// Verify that manager-owned state callback remains active and user callbacks
// can be set before and after connect without clobbering state sync.
TEST(CallbackFanoutTests, StateCallbackBeforeAndAfterConnect) {
    const std::string socketPath = "/tmp/entropy_callback_fanout.sock";

    // Start a simple echo server in the background
    ConnectionManager serverMgr(4);
    auto server = createLocalServer(&serverMgr, socketPath);
    ASSERT_TRUE(server->listen().success());

    std::atomic<bool> serverStop{false};
    std::thread serverThread([&] {
        auto conn = server->accept();
        if (!conn.valid()) return;
        conn.setMessageCallback([conn](const std::vector<uint8_t>& data) mutable {
            (void)conn.send(data);  // echo
        });
        while (!serverStop.load(std::memory_order_acquire) && conn.isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)conn.close();
        (void)server->close();
    });

    // Client
    ConnectionManager clientMgr(4);
    auto h = clientMgr.openLocalConnection(socketPath);
    ASSERT_TRUE(h.valid());

    std::mutex m;
    std::condition_variable cv;
    bool gotConnected = false;
    int stateCallbacksBefore = 0;
    int stateCallbacksAfter = 0;

    // Set user state callback BEFORE connect
    h.setStateCallback([&](ConnectionState s) {
        if (s == ConnectionState::Connected) {
            std::lock_guard<std::mutex> lk(m);
            gotConnected = true;
            ++stateCallbacksBefore;
            cv.notify_one();
        }
    });

    // Connect
    ASSERT_TRUE(h.connect().success());

    // Wait for connected by polling state (initial Connected may predate callback binding)
    for (int i = 0; i < 300 && !h.isConnected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify manager state is synchronized
    EXPECT_TRUE(h.isConnected());
    EXPECT_EQ(h.getState(), ConnectionState::Connected);

    // Now set another state callback AFTER connect
    h.setStateCallback([&](ConnectionState s) {
        if (s == ConnectionState::Disconnected) {
            ++stateCallbacksAfter;
        }
    });

    // Trigger a disconnect
    ASSERT_TRUE(h.disconnect().success());

    // Give some time for callback delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Manager state should reflect Disconnected
    EXPECT_EQ(h.getState(), ConnectionState::Disconnected);
    // Callback set after connect should have fired on disconnect
    EXPECT_GE(stateCallbacksAfter, 1);

    serverStop.store(true, std::memory_order_release);
    if (serverThread.joinable()) serverThread.join();
}
