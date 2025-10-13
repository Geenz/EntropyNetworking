// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Session/SessionManager.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>

using namespace std;
using namespace EntropyEngine::Networking;

int main() {
    const string socketPath = "/tmp/entropy_local.sock";

    try {
        cout << "Starting EntropyNetworking Local Client" << endl;
        cout << "Connecting to: " << socketPath << endl;

        // Create connection manager
        ConnectionManager connMgr(64);

        // Open local connection using platform-agnostic API
        // This will use Unix socket on Linux/macOS, Named Pipe on Windows
        auto conn = connMgr.openLocalConnection(socketPath);
        if (!conn.valid()) {
            cerr << "Failed to create local connection" << endl;
            return 1;
        }

        cout << "Connection created, state: " << static_cast<int>(conn.getState()) << endl;

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
                cout << "State callback: Connected!" << endl;
                isConnected = true;
                connCV.notify_one();
            } else if (state == ConnectionState::Failed) {
                cout << "State callback: Connection failed" << endl;
                connectionFailed = true;
                connCV.notify_one();
            } else if (state == ConnectionState::Disconnected) {
                cout << "State callback: Disconnected" << endl;
            }
        });

        // Set message callback using convenient API
        conn.setMessageCallback([&messagesReceived](const vector<uint8_t>& data) {
            string message(data.begin(), data.end());
            cout << "Received message: " << message << endl;
            messagesReceived++;
        });

        // Connect to server
        cout << "Connecting..." << endl;
        auto result = conn.connect();
        if (result.failed()) {
            cerr << "Failed to connect: " << result.errorMessage << endl;
            return 1;
        }

        // Wait for connection using condition variable (more efficient than polling)
        {
            unique_lock<mutex> lock(connMutex);
            if (!connCV.wait_for(lock, chrono::seconds(5), [&]{ return isConnected || connectionFailed; })) {
                cerr << "Connection timeout" << endl;
                return 1;
            }

            if (connectionFailed) {
                cerr << "Connection failed" << endl;
                return 1;
            }
        }

        cout << "Connected! State: " << static_cast<int>(conn.getState()) << endl;

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
            cout << "Sending: " << msg << endl;
            vector<uint8_t> data(msg.begin(), msg.end());
            auto sendResult = conn.send(data);

            if (sendResult.failed()) {
                cerr << "Failed to send: " << sendResult.errorMessage << endl;
                break;
            }

            // Wait for echo response
            this_thread::sleep_for(chrono::milliseconds(200));
        }

        // Give time for final responses
        this_thread::sleep_for(chrono::seconds(1));

        // Display statistics
        auto stats = conn.getStats();
        cout << "\nConnection Statistics:" << endl;
        cout << "  Bytes sent: " << stats.bytesSent << endl;
        cout << "  Bytes received: " << stats.bytesReceived << endl;
        cout << "  Messages sent: " << stats.messagesSent << endl;
        cout << "  Messages received: " << stats.messagesReceived << endl;
        cout << "  Messages processed: " << messagesReceived << endl;
        cout << "  Connection time: " << stats.connectTime << " ms since epoch" << endl;

        // Disconnect
        cout << "\nDisconnecting..." << endl;
        conn.disconnect();

        cout << "Client shutdown complete" << endl;

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
