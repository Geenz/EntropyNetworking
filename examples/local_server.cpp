// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace std;
using namespace EntropyEngine::Networking;

static atomic<bool> keepRunning{true};

void signalHandler(int) {
    keepRunning = false;
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const string socketPath = "/tmp/entropy_local.sock";

    try {
        cout << "Starting EntropyNetworking Local Server (Platform-Agnostic)" << endl;
        cout << "Socket path: " << socketPath << endl;

        // Create connection manager
        ConnectionManager connMgr(64);

        // Create platform-agnostic local server
        auto server = createLocalServer(&connMgr, socketPath);

        // Start listening
        auto listenResult = server->listen();
        if (listenResult.failed()) {
            cerr << "Failed to listen: " << listenResult.errorMessage << endl;
            return 1;
        }

        cout << "Server listening on " << socketPath << endl;
        cout << "Waiting for client connections..." << endl;
        cout << "Press Ctrl+C to quit" << endl;

        // Accept and handle connections
        while (keepRunning) {
            // Accept connection (blocking)
            auto conn = server->accept();

            if (!conn.valid()) {
                if (!keepRunning) break;
                cerr << "Failed to accept connection" << endl;
                continue;
            }

            cout << "Client connected!" << endl;

            // Set up message callback using convenient API
            conn.setMessageCallback([conn](const vector<uint8_t>& data) mutable {
                string message(data.begin(), data.end());
                cout << "Received: " << message << endl;

                // Echo back
                string response = "Echo: " + message;
                vector<uint8_t> responseData(response.begin(), response.end());
                auto result = conn.send(responseData);

                if (result.failed()) {
                    cerr << "Failed to send response: " << result.errorMessage << endl;
                } else {
                    cout << "Sent response: " << response << endl;
                }
            });

            // Send welcome message
            string welcome = "Welcome to Entropy Local Server!";
            vector<uint8_t> welcomeData(welcome.begin(), welcome.end());
            auto sendResult = conn.send(welcomeData);

            if (sendResult.failed()) {
                cerr << "Failed to send welcome: " << sendResult.errorMessage << endl;
            } else {
                cout << "Sent welcome message" << endl;
            }

            // Handle this client until disconnect
            // The message callback will process incoming messages
            cout << "Handling client (waiting for messages)..." << endl;

            // Wait for client to disconnect by polling state
            while (keepRunning && conn.isConnected()) {
                this_thread::sleep_for(chrono::milliseconds(100));
            }

            cout << "Client disconnected" << endl;

            // IMPORTANT: Close the connection to free the slot
            conn.close();
        }

        // Cleanup
        server->close();
        cout << "Server shutdown complete" << endl;

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
