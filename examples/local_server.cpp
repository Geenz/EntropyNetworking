// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/LocalServer.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <Logging/Logger.h>
#include <csignal>
#include <atomic>
#include <format>

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
        ENTROPY_LOG_INFO("Starting EntropyNetworking Local Server (Platform-Agnostic)");
        ENTROPY_LOG_INFO(std::format("Socket path: {}", socketPath));

        // Create connection manager
        ConnectionManager connMgr(64);

        // Create platform-agnostic local server
        auto server = createLocalServer(&connMgr, socketPath);

        // Start listening
        auto listenResult = server->listen();
        if (listenResult.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to listen: {}", listenResult.errorMessage));
            return 1;
        }

        ENTROPY_LOG_INFO(std::format("Server listening on {}", socketPath));
        ENTROPY_LOG_INFO("Waiting for client connections...");
        ENTROPY_LOG_INFO("Press Ctrl+C to quit");

        // Accept and handle connections
        while (keepRunning) {
            // Accept connection (blocking)
            auto conn = server->accept();

            if (!conn.valid()) {
                if (!keepRunning) break;
                ENTROPY_LOG_ERROR("Failed to accept connection");
                continue;
            }

            ENTROPY_LOG_INFO("Client connected!");

            // Set up message callback using convenient API
            conn.setMessageCallback([conn](const vector<uint8_t>& data) mutable {
                string message(data.begin(), data.end());
                ENTROPY_LOG_INFO(std::format("Received: {}", message));

                // Echo back
                string response = "Echo: " + message;
                vector<uint8_t> responseData(response.begin(), response.end());
                auto result = conn.send(responseData);

                if (result.failed()) {
                    ENTROPY_LOG_ERROR(std::format("Failed to send response: {}", result.errorMessage));
                } else {
                    ENTROPY_LOG_INFO(std::format("Sent response: {}", response));
                }
            });

            // Send welcome message
            string welcome = "Welcome to Entropy Local Server!";
            vector<uint8_t> welcomeData(welcome.begin(), welcome.end());
            auto sendResult = conn.send(welcomeData);

            if (sendResult.failed()) {
                ENTROPY_LOG_ERROR(std::format("Failed to send welcome: {}", sendResult.errorMessage));
            } else {
                ENTROPY_LOG_INFO("Sent welcome message");
            }

            // Handle this client until disconnect
            // The message callback will process incoming messages
            ENTROPY_LOG_INFO("Handling client (waiting for messages)...");

            // Wait for client to disconnect by polling state
            while (keepRunning && conn.isConnected()) {
                this_thread::sleep_for(chrono::milliseconds(100));
            }

            ENTROPY_LOG_INFO("Client disconnected");

            // IMPORTANT: Close the connection to free the slot
            conn.close();
        }

        // Cleanup
        server->close();
        ENTROPY_LOG_INFO("Server shutdown complete");

    } catch (const exception& e) {
        ENTROPY_LOG_ERROR(std::format("Error: {}", e.what()));
        return 1;
    }

    return 0;
}
