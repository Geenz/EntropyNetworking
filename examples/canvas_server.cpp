// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

/**
 * Canvas Server Example
 *
 * Demonstrates a complete protocol-level server with schema support.
 * This example shows:
 * - ComponentSchemaRegistry for defining shared data structures
 * - SessionManager for protocol-level communication
 * - Automatic schema broadcasting to connected clients
 * - Entity creation and property updates
 *
 * Run this server first, then connect with canvas_client.
 */

#include <EntropyCore.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../src/Networking/Core/ComponentSchema.h"
#include "../src/Networking/Core/ComponentSchemaRegistry.h"
#include "../src/Networking/Session/SessionManager.h"
#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/RemoteServer.h"

using namespace std;
using namespace EntropyEngine;
using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core;

int main() {
    try {
        ENTROPY_LOG_INFO("=== Entropy Canvas Server ===");
        ENTROPY_LOG_INFO("This example demonstrates a complete protocol server with schemas.");
        ENTROPY_LOG_INFO("");

        // Step 1: Create the core managers
        // ConnectionManager handles low-level transport (WebRTC, Unix sockets, etc.)
        ConnectionManager connMgr(64);

        // ComponentSchemaRegistry stores and publishes component schemas
        ComponentSchemaRegistry schemaRegistry;

        // SessionManager handles protocol-level messaging (entities, properties, etc.)
        // Pass the schema registry so it can auto-broadcast schemas to clients
        SessionManager sessMgr(&connMgr, 64, &schemaRegistry);

        ENTROPY_LOG_INFO("[1] Managers initialized");

        // Step 2: Define a CanvasObject schema
        // This describes the structure of objects on our canvas
        vector<PropertyDefinition> canvasObjectProps = {
            {"position", PropertyType::Vec2, 0, sizeof(Vec2)},                           // x, y position
            {"color", PropertyType::Vec4, sizeof(Vec2), sizeof(Vec4)},                   // RGBA color
            {"size", PropertyType::Float32, sizeof(Vec2) + sizeof(Vec4), sizeof(float)}  // radius
        };

        auto schemaResult = ComponentSchema::create("com.entropy.canvas",  // Application ID (reverse domain notation)
                                                    "CanvasObject",        // Component type name
                                                    1,                     // Schema version
                                                    canvasObjectProps,     // Property definitions
                                                    sizeof(Vec2) + sizeof(Vec4) + sizeof(float),  // Total size
                                                    true  // Make it public (discoverable)
        );

        if (schemaResult.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to create schema: {}", schemaResult.errorMessage));
            return 1;
        }

        ComponentSchema canvasObjectSchema = schemaResult.value;
        ENTROPY_LOG_INFO("[2] Created CanvasObject schema");

        // Step 3: Register and publish the schema
        // Publishing triggers automatic broadcast to all connected clients
        auto registerResult = schemaRegistry.registerSchema(canvasObjectSchema);
        if (registerResult.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to register schema: {}", registerResult.errorMessage));
            return 1;
        }

        auto publishResult = schemaRegistry.publishSchema(canvasObjectSchema.typeHash);
        if (publishResult.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to publish schema: {}", publishResult.errorMessage));
            return 1;
        }

        ENTROPY_LOG_INFO("[3] Registered and published schema");
        ENTROPY_LOG_INFO("    Schema will be automatically sent to clients on connection");
        ENTROPY_LOG_INFO("");

        // Step 4: Set up remote server (WebRTC with internal signaling)
        auto server = createRemoteServer(&connMgr, 8080);
        auto listenResult = server->listen();
        if (listenResult.failed()) {
            ENTROPY_LOG_ERROR(std::format("Failed to start server: {}", listenResult.errorMessage));
            return 1;
        }

        ENTROPY_LOG_INFO("[4] Remote server listening on port 8080");
        ENTROPY_LOG_INFO("    WebRTC signaling handled internally");

        // Track connected sessions
        vector<SessionHandle> sessions;
        mutex sessionsMutex;

        // Step 5: Accept connections in background thread
        thread acceptThread([&]() {
            while (true) {
                auto conn = server->accept();  // Blocks until client connects
                if (!conn.valid()) break;

                ENTROPY_LOG_INFO("\n>>> Client connected");

                // Create session (wraps connection with protocol layer)
                auto session = sessMgr.createSession(conn);
                if (!session.valid()) {
                    ENTROPY_LOG_ERROR("Failed to create session");
                    continue;
                }

                {
                    lock_guard<mutex> lock(sessionsMutex);
                    sessions.push_back(session);
                }

                // Set up handshake callback
                // This fires after connection is established and schemas are sent
                sessMgr.setHandshakeCallback(session, [](const string& clientType, const string& clientId) {
                    ENTROPY_LOG_INFO(">>> Handshake complete!");
                    ENTROPY_LOG_INFO(std::format("    Client type: {}", clientType));
                    ENTROPY_LOG_INFO(std::format("    Client ID: {}", clientId));
                    ENTROPY_LOG_INFO("    Schemas have been automatically sent to client");
                });

                // Set up entity callbacks to handle messages from client
                sessMgr.setEntityCreatedCallback(
                    session, [](uint64_t entityId, const string& appId, const string& typeName, uint64_t parentId) {
                        ENTROPY_LOG_INFO(std::format("Client created entity: {} ({})", entityId, typeName));
                    });
            }
        });

        acceptThread.detach();

        ENTROPY_LOG_INFO("\n[5] Server ready! Waiting for clients...");
        ENTROPY_LOG_INFO("    Connect with canvas_client");
        ENTROPY_LOG_INFO("");

        // Step 6: Simulate canvas activity
        // After clients connect, create and update canvas objects
        thread simulationThread([&]() {
            // Wait for at least one client
            while (true) {
                {
                    lock_guard<mutex> lock(sessionsMutex);
                    bool hasConnected = false;
                    for (const auto& s : sessions) {
                        if (s.isConnected()) {
                            hasConnected = true;
                            break;
                        }
                    }
                    if (hasConnected) break;
                }
                this_thread::sleep_for(chrono::milliseconds(100));
            }

            this_thread::sleep_for(chrono::seconds(2));

            ENTROPY_LOG_INFO("\n=== Starting canvas simulation ===");

            // Create a few canvas objects
            for (int i = 0; i < 3; i++) {
                uint64_t entityId = 1000 + i;

                ENTROPY_LOG_INFO(std::format("\nCreating canvas object {}...", entityId));

                lock_guard<mutex> lock(sessionsMutex);
                for (auto& session : sessions) {
                    if (!session.isConnected()) continue;

                    auto result = session.sendEntityCreated(entityId, "com.entropy.canvas", "CanvasObject",
                                                            0  // No parent (root level)
                    );

                    if (result.success()) {
                        ENTROPY_LOG_INFO("  Sent EntityCreated to client");
                    }
                }

                this_thread::sleep_for(chrono::milliseconds(500));
            }

            ENTROPY_LOG_INFO("\n=== Canvas objects created ===");
            ENTROPY_LOG_INFO("You can now see them in the client!");
            ENTROPY_LOG_INFO("Press Ctrl+C to quit");
        });

        simulationThread.detach();

        // Keep server running
        while (true) {
            this_thread::sleep_for(chrono::seconds(1));
        }

    } catch (const exception& e) {
        ENTROPY_LOG_ERROR(std::format("Error: {}", e.what()));
        return 1;
    }

    return 0;
}
