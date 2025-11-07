// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

/**
 * Canvas Client Example
 *
 * Demonstrates a complete protocol-level client that connects to canvas_server.
 * This example shows:
 * - Connecting to a server via WebRTC + WebSocket signaling
 * - Automatic handshake and client identification
 * - Receiving schema advertisements from server
 * - Receiving entity creation messages
 *
 * Run canvas_server first, then run this client.
 */

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Session/SessionManager.h"
#include <EntropyCore.h>
#include <thread>
#include <chrono>
#include <atomic>

using namespace std;
using namespace EntropyEngine;
using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core;

int main() {
    try {
        ENTROPY_LOG_INFO("=== Entropy Canvas Client ===");
        ENTROPY_LOG_INFO("Connecting to canvas_server...");
        ENTROPY_LOG_INFO("");

        // Step 1: Create the core managers
        ConnectionManager connMgr(64);
        SessionManager sessMgr(&connMgr, 64);  // Client doesn't need schema registry

        ENTROPY_LOG_INFO("[1] Managers initialized");

        // Step 2: Connect to server (WebRTC with internal signaling)
        string signalingUrl = "ws://localhost:8080";
        ENTROPY_LOG_INFO(std::format("[2] Connecting to server: {}", signalingUrl));
        ENTROPY_LOG_INFO("    WebRTC signaling handled internally");

        auto conn = connMgr.openRemoteConnection(signalingUrl);
        if (!conn.valid()) {
            ENTROPY_LOG_ERROR("Failed to create connection");
            return 1;
        }

        // Step 3: Create session
        auto session = sessMgr.createSession(conn);
        if (!session.valid()) {
            ENTROPY_LOG_ERROR("Failed to create session");
            return 1;
        }

        atomic<bool> handshakeComplete{false};
        atomic<int> entitiesReceived{0};

        // Set up protocol callbacks
        // Handshake callback - server will call this after connection is established
        sessMgr.setHandshakeCallback(session, [&](const string& clientType, const string& clientId) {
            ENTROPY_LOG_INFO("\n>>> Handshake complete!");
            ENTROPY_LOG_INFO("    Connected to server");
            ENTROPY_LOG_INFO("    Waiting for schema advertisements...");
            handshakeComplete = true;
        });

        // Entity created callback - receive canvas objects from server
        sessMgr.setEntityCreatedCallback(session,
            [&entitiesReceived](uint64_t entityId, const string& appId, const string& typeName, uint64_t parentId) {
                ENTROPY_LOG_INFO("\n>>> Received Entity Created:");
                ENTROPY_LOG_INFO(std::format("    Entity ID: {}", entityId));
                ENTROPY_LOG_INFO(std::format("    App ID: {}", appId));
                ENTROPY_LOG_INFO(std::format("    Type: {}", typeName));
                ENTROPY_LOG_INFO(std::format("    Parent: {}", parentId));
                entitiesReceived++;
            });

        // Error callback
        sessMgr.setErrorCallback(session, [](NetworkError error, const string& message) {
            ENTROPY_LOG_ERROR(std::format("Error: {}", message));
        });

        // Step 4: Connect to server
        ENTROPY_LOG_INFO("[3] Initiating connection...");
        auto connectResult = conn.connect();
        if (connectResult.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to connect: {}", connectResult.errorMessage));
            return 1;
        }

        // Wait for connection
        bool connected = false;
        for (int i = 0; i < 150 && !connected; ++i) {
            this_thread::sleep_for(chrono::milliseconds(100));
            connected = session.isConnected();
        }

        if (!connected) {
            ENTROPY_LOG_ERROR("Connection timeout");
            return 1;
        }

        ENTROPY_LOG_INFO("\n[4] Connected!");
        ENTROPY_LOG_INFO("=== Listening for messages from server ===");
        ENTROPY_LOG_INFO("The server will automatically send schema advertisements");
        ENTROPY_LOG_INFO("after the handshake completes.");
        ENTROPY_LOG_INFO("");

        // Wait for handshake
        for (int i = 0; i < 50 && !handshakeComplete; i++) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }

        // Note: Schema advertisements are handled internally by NetworkSession
        // The client's PropertyRegistry is automatically updated
        // In a real app, you would query the PropertyRegistry for schema information

        // Keep client running to receive entity updates
        ENTROPY_LOG_INFO("\nWaiting for canvas objects...");
        ENTROPY_LOG_INFO("Press Ctrl+C to quit");
        ENTROPY_LOG_INFO("");

        int lastEntityCount = 0;
        bool stillConnected = true;
        while (stillConnected) {
            this_thread::sleep_for(chrono::milliseconds(500));

            int currentCount = entitiesReceived.load();
            if (currentCount != lastEntityCount) {
                ENTROPY_LOG_INFO(std::format("\nTotal entities received: {}", currentCount));
                lastEntityCount = currentCount;
            }

            // Check connection status
            stillConnected = session.isConnected();
        }

        ENTROPY_LOG_INFO("\nDisconnected from server");

    } catch (const exception& e) {
        ENTROPY_LOG_ERROR(std::format("Error: {}", e.what()));
        return 1;
    }

    return 0;
}
