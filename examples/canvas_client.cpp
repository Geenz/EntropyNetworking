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
#include "../src/Networking/Transport/WebRTCConnection.h"
#include "../src/Networking/Session/SessionManager.h"
#include <EntropyCore.h>
#include <rtc/rtc.hpp>
#include <memory>
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

        // Step 2: Connect to WebSocket signaling server
        string signalingUrl = "ws://localhost:8080";
        ENTROPY_LOG_INFO(std::format("[2] Connecting to signaling server: {}", signalingUrl));

        shared_ptr<rtc::WebSocket> ws = make_shared<rtc::WebSocket>();
        atomic<bool> signalingConnected{false};

        // Step 3: Declare connection and session handles upfront
        // They will be initialized in the onOpen callback
        // Use mutex to protect cross-thread access
        ConnectionHandle conn;
        SessionHandle session;
        mutex sessionMutex;
        atomic<bool> sessionReady{false};
        atomic<bool> handshakeComplete{false};
        atomic<int> entitiesReceived{0};

        ws->onError([](string error) {
            ENTROPY_LOG_ERROR(std::format("Signaling error: {}", error));
        });

        // Step 4: Set up signaling message handler BEFORE opening WebSocket
        // This is critical for the client to receive the server's answer/offer
        ws->onMessage([&connMgr, &conn, &sessionMutex](auto data) {
            if (holds_alternative<string>(data)) {
                string msg = get<string>(data);
                lock_guard<mutex> lock(sessionMutex);
                auto* webrtcConn = dynamic_cast<WebRTCConnection*>(connMgr.getConnectionPointer(conn));
                if (!webrtcConn) return;

                // Check if this is a candidate (has '|' separator)
                size_t pipeSeparator = msg.find('|');
                if (pipeSeparator != string::npos) {
                    // ICE candidate format: "candidate|mid"
                    string candidateStr = msg.substr(0, pipeSeparator);
                    string mid = msg.substr(pipeSeparator + 1);
                    webrtcConn->addRemoteCandidate(candidateStr, mid);
                } else {
                    // SDP format: "type\nsdp"
                    size_t newlineSeparator = msg.find('\n');
                    if (newlineSeparator != string::npos) {
                        string type = msg.substr(0, newlineSeparator);
                        string sdp = msg.substr(newlineSeparator + 1);
                        webrtcConn->setRemoteDescription(type, sdp);
                    }
                }
            }
        });

        // Step 5: Set up onOpen callback to create connection after signaling is ready
        ws->onOpen([&]() {
            ENTROPY_LOG_INFO("    Signaling connection established");
            signalingConnected = true;

            // Create WebRTC connection with signaling callbacks
            ConnectionConfig config;
            config.type = ConnectionType::Remote;
            config.backend = ConnectionBackend::WebRTC;
            // Client is polite peer in perfect negotiation (backs off during offer collisions)
            config.webrtcConfig.polite = true;

            config.signalingCallbacks.onLocalDescription = [ws](const string& type, const string& sdp) {
                ENTROPY_LOG_INFO(std::format("    Sending {} to server", type));
                // Send type and SDP separated by newline
                ws->send(type + "\n" + sdp);
            };

            config.signalingCallbacks.onLocalCandidate = [ws](const string& candidate, const string& mid) {
                ws->send(candidate + "|" + mid);
            };

            {
                lock_guard<mutex> lock(sessionMutex);
                conn = connMgr.openConnection(config);
                if (!conn.valid()) {
                    ENTROPY_LOG_ERROR("Failed to create connection");
                    return;
                }

                // Create session
                session = sessMgr.createSession(conn);
                if (!session.valid()) {
                    ENTROPY_LOG_ERROR("Failed to create session");
                    return;
                }

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

                // Connect to server
                ENTROPY_LOG_INFO("[3] Initiating WebRTC connection...");
                auto connectResult = conn.connect();
                if (connectResult.failed()) {
                    ENTROPY_LOG_ERROR(std::format("Failed to connect: {}", connectResult.errorMessage));
                    return;
                }

                // Signal that session is ready for checking
                sessionReady = true;
            }
        });

        ws->open(signalingUrl);

        // Wait for signaling connection
        for (int i = 0; i < 50 && !signalingConnected; i++) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }

        if (!signalingConnected) {
            ENTROPY_LOG_ERROR("Failed to connect to signaling server");
            ENTROPY_LOG_ERROR("Make sure canvas_server is running!");
            return 1;
        }

        // Wait for session to be created
        for (int i = 0; i < 50 && !sessionReady; ++i) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }

        if (!sessionReady) {
            ENTROPY_LOG_ERROR("Session creation timeout");
            return 1;
        }

        // Wait for WebRTC connection (with mutex protection)
        bool connected = false;
        for (int i = 0; i < 150 && !connected; ++i) {
            this_thread::sleep_for(chrono::milliseconds(100));
            lock_guard<mutex> lock(sessionMutex);
            connected = session.valid() && session.isConnected();
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

            // Check connection status with mutex protection
            lock_guard<mutex> lock(sessionMutex);
            stillConnected = session.isConnected();
        }

        ENTROPY_LOG_INFO("\nDisconnected from server");

    } catch (const exception& e) {
        ENTROPY_LOG_ERROR(std::format("Error: {}", e.what()));
        return 1;
    }

    return 0;
}
