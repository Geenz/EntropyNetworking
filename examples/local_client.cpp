// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Session/SessionManager.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <Logging/Logger.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <format>

using namespace std;
using namespace EntropyEngine::Networking;

int main() {
    const string socketPath = "/tmp/entropy_local.sock";

    try {
        ENTROPY_LOG_INFO("Starting EntropyNetworking Local Client");
        ENTROPY_LOG_INFO(std::format("Connecting to: {}", socketPath));

        // Create connection manager
        ConnectionManager connMgr(64);

        // Open local connection using platform-agnostic API
        // This will use Unix socket on Linux/macOS, Named Pipe on Windows
        auto conn = connMgr.openLocalConnection(socketPath);
        if (!conn.valid()) {
            ENTROPY_LOG_ERROR("Failed to create local connection");
            return 1;
        }

        ENTROPY_LOG_INFO(std::format("Connection created, state: {}", static_cast<int>(conn.getState())));

        // Set up synchronization for connection state
        mutex connMutex;
        condition_variable connCV;
        bool isConnected = false;
        bool connectionFailed = false;

        // Set up message callback to receive data
        atomic<int> messagesReceived{0};

        // Set state callback using convenient API (no raw pointer needed!)
        conn.setStateCallback([&](ConnectionState state) {
            lock_guard<mutex> lock(connMutex);
            if (state == ConnectionState::Connected) {
                ENTROPY_LOG_INFO("State callback: Connected!");
                isConnected = true;
                connCV.notify_one();
            } else if (state == ConnectionState::Failed) {
                ENTROPY_LOG_INFO("State callback: Connection failed");
                connectionFailed = true;
                connCV.notify_one();
            } else if (state == ConnectionState::Disconnected) {
                ENTROPY_LOG_INFO("State callback: Disconnected");
            }
        });

        // Set message callback using convenient API
        conn.setMessageCallback([&messagesReceived](const vector<uint8_t>& data) {
            string message(data.begin(), data.end());
            ENTROPY_LOG_INFO(std::format("Received message: {}", message));
            messagesReceived++;
        });

        // Connect to server
        ENTROPY_LOG_INFO("Connecting...");
        auto result = conn.connect();
        if (result.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to connect: {}", result.errorMessage));
            return 1;
        }

        // Wait for connection using condition variable (more efficient than polling)
        {
            unique_lock<mutex> lock(connMutex);
            if (!connCV.wait_for(lock, chrono::seconds(5), [&]{ return isConnected || connectionFailed; })) {
                ENTROPY_LOG_ERROR("Connection timeout");
                return 1;
            }

            if (connectionFailed) {
                ENTROPY_LOG_ERROR("Connection failed");
                return 1;
            }
        }

        ENTROPY_LOG_INFO(std::format("Connected! State: {}", static_cast<int>(conn.getState())));

        // Wait for welcome message
        this_thread::sleep_for(chrono::milliseconds(500));

        // Send test messages
        vector<string> messages = {
            "Hello from local client!",
            "Testing Unix socket communication",
            "This is a local connection",
            "Fast and reliable IPC"
        };

        for (const auto& msg : messages) {
            ENTROPY_LOG_INFO(std::format("Sending: {}", msg));
            vector<uint8_t> data(msg.begin(), msg.end());
            auto sendResult = conn.send(data);

            if (sendResult.failed()) {
                ENTROPY_LOG_ERROR(std::format("Failed to send: {}", sendResult.errorMessage));
                break;
            }

            // Wait for echo response
            this_thread::sleep_for(chrono::milliseconds(200));
        }

        // Give time for final responses
        this_thread::sleep_for(chrono::seconds(1));

        // Display statistics
        auto stats = conn.getStats();
        ENTROPY_LOG_INFO("");
        ENTROPY_LOG_INFO("Connection Statistics:");
        ENTROPY_LOG_INFO(std::format("  Bytes sent: {}", stats.bytesSent));
        ENTROPY_LOG_INFO(std::format("  Bytes received: {}", stats.bytesReceived));
        ENTROPY_LOG_INFO(std::format("  Messages sent: {}", stats.messagesSent));
        ENTROPY_LOG_INFO(std::format("  Messages received: {}", stats.messagesReceived));
        ENTROPY_LOG_INFO(std::format("  Messages processed: {}", messagesReceived.load()));
        ENTROPY_LOG_INFO(std::format("  Connection time: {} ms since epoch", stats.connectTime));

        // Disconnect
        ENTROPY_LOG_INFO("");
        ENTROPY_LOG_INFO("Disconnecting...");
        conn.disconnect();

        ENTROPY_LOG_INFO("Client shutdown complete");

    } catch (const exception& e) {
        ENTROPY_LOG_ERROR(std::format("Error: {}", e.what()));
        return 1;
    }

    return 0;
}
