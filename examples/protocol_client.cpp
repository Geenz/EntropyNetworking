// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/WebRTCConnection.h"
#include "../src/Networking/Session/SessionManager.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <rtc/rtc.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace std;
using namespace EntropyEngine::Networking;

int main() {
    try {
        cout << "Starting EntropyNetworking Protocol Client" << endl;

        // Create managers
        ConnectionManager connMgr(64);
        SessionManager sessMgr(&connMgr, 64);

        // Connect to signaling server
        auto ws = make_shared<rtc::WebSocket>();
        cout << "Connecting to signaling server at ws://localhost:8080" << endl;

        ConnectionHandle conn;
        SessionHandle session;

        // Handle signaling messages
        ws->onMessage([&connMgr, &conn](auto data) {
            if (holds_alternative<string>(data)) {
                string msg = get<string>(data);

                auto* webrtcConn = dynamic_cast<WebRTCConnection*>(connMgr.getConnectionPointer(conn));
                if (!webrtcConn) return;

                size_t separator = msg.find('|');
                if (separator != string::npos) {
                    string candidateStr = msg.substr(0, separator);
                    string mid = msg.substr(separator + 1);
                    cout << "Received ICE candidate from server" << endl;
                    webrtcConn->addRemoteCandidate(candidateStr, mid);
                } else {
                    cout << "Received SDP answer from server" << endl;
                    webrtcConn->setRemoteDescription("answer", msg);
                }
            }
        });

        ws->onOpen([&]() {
            cout << "Connected to signaling server" << endl;

            // Create connection with WebRTC backend
            ConnectionConfig config;
            config.type = ConnectionType::Remote;
            config.backend = ConnectionBackend::WebRTC;

            config.signalingCallbacks.onLocalDescription = [ws](const string& type, const string& sdp) {
                cout << "Sending " << type << " to server" << endl;
                ws->send(sdp);
            };

            config.signalingCallbacks.onLocalCandidate = [ws](const string& candidate, const string& mid) {
                cout << "Sending ICE candidate to server" << endl;
                ws->send(candidate + "|" + mid);
            };

            conn = connMgr.openConnection(config);
            if (!conn.valid()) {
                cerr << "Failed to create connection" << endl;
                return;
            }

            // Create session
            session = sessMgr.createSession(conn);
            if (!session.valid()) {
                cerr << "Failed to create session" << endl;
                return;
            }

            // Set up protocol message callbacks
            sessMgr.setEntityCreatedCallback(session, [](uint64_t entityId, const string& appId,
                                                          const string& typeName, uint64_t parentId) {
                cout << "Client received EntityCreated:" << endl;
                cout << "  Entity ID: " << entityId << endl;
                cout << "  App ID: " << appId << endl;
                cout << "  Type: " << typeName << endl;
                cout << "  Parent ID: " << parentId << endl;
            });

            sessMgr.setEntityDestroyedCallback(session, [](uint64_t entityId) {
                cout << "Client received EntityDestroyed: " << entityId << endl;
            });

            sessMgr.setErrorCallback(session, [](NetworkError error, const string& message) {
                cout << "Error: " << message << endl;
            });

            // Connect
            auto connectResult = conn.connect();
            if (connectResult.failed()) {
                cerr << "Failed to initialize WebRTC connection: " << connectResult.errorMessage << endl;
                return;
            }

            cout << "WebRTC connection initiated" << endl;

            // Send an EntityCreated message after a delay
            thread([session]() {
                this_thread::sleep_for(chrono::seconds(5));
                if (session.valid() && session.isConnected()) {
                    cout << "Sending EntityCreated message to server..." << endl;
                    auto result = session.sendEntityCreated(
                        67890,                  // entityId
                        "com.entropy.client",   // appId
                        "ClientNode",           // typeName
                        0                       // parentId (root)
                    );

                    if (result.success()) {
                        cout << "EntityCreated message sent successfully" << endl;
                    } else {
                        cout << "Failed to send EntityCreated: " << result.errorMessage << endl;
                    }
                }
            }).detach();
        });

        ws->onClosed([]() {
            cout << "Disconnected from signaling server" << endl;
        });

        ws->onError([](string error) {
            cerr << "WebSocket error: " << error << endl;
        });

        // Connect to signaling server
        ws->open("ws://localhost:8080");

        cout << "Client ready. Press Ctrl+C to quit" << endl;

        // Keep running
        while (true) {
            this_thread::sleep_for(chrono::seconds(1));
        }

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
