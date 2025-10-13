// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Session/NetworkSession.h"
#include "../src/Networking/Transport/WebRTCConnection.h"
#include <rtc/rtc.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace std;
using namespace EntropyEngine::Networking;

int main() {
    try {
        cout << "Starting EntropyNetworking Protocol Server" << endl;

        // Create WebSocket server for signaling
        rtc::WebSocketServer::Configuration wsConfig;
        wsConfig.port = 8080;
        wsConfig.enableTls = false;

        rtc::WebSocketServer wsServer(wsConfig);
        cout << "WebSocket signaling server listening on port 8080" << endl;

        shared_ptr<NetworkSession> session;

        // Handle incoming WebSocket connections
        wsServer.onClient([&](shared_ptr<rtc::WebSocket> ws) {
            cout << "Client connected to signaling server" << endl;

            // Create WebRTC connection
            WebRTCConfig config;
            SignalingCallbacks callbacks;

            callbacks.onLocalDescription = [ws](const string& type, const string& sdp) {
                cout << "Sending " << type << " to client" << endl;
                ws->send(sdp);
            };

            callbacks.onLocalCandidate = [ws](const string& candidate, const string& mid) {
                cout << "Sending ICE candidate to client" << endl;
                ws->send(candidate + "|" + mid);
            };

            auto* webrtcConn = new WebRTCConnection(config, callbacks);

            // Create network session
            session = make_shared<NetworkSession>(webrtcConn);

            // Set up protocol message callbacks
            session->setEntityCreatedCallback([](uint64_t entityId, const string& appId,
                                                  const string& typeName, uint64_t parentId) {
                cout << "Server received EntityCreated:" << endl;
                cout << "  Entity ID: " << entityId << endl;
                cout << "  App ID: " << appId << endl;
                cout << "  Type: " << typeName << endl;
                cout << "  Parent ID: " << parentId << endl;
            });

            session->setEntityDestroyedCallback([](uint64_t entityId) {
                cout << "Server received EntityDestroyed: " << entityId << endl;
            });

            session->setErrorCallback([](NetworkError error, const string& message) {
                cout << "Error: " << message << endl;
            });

            // Connect via session (which will call webrtcConn->connect())
            auto connectResult = session->connect();
            if (connectResult.failed()) {
                cerr << "Failed to initialize WebRTC connection: " << connectResult.errorMessage << endl;
                return;
            }

            cout << "WebRTC connection initiated" << endl;

            // Handle signaling messages
            ws->onMessage([webrtcConn](auto data) {
                if (holds_alternative<string>(data)) {
                    string msg = get<string>(data);

                    size_t separator = msg.find('|');
                    if (separator != string::npos) {
                        string candidateStr = msg.substr(0, separator);
                        string mid = msg.substr(separator + 1);
                        webrtcConn->addRemoteCandidate(candidateStr, mid);
                    } else {
                        webrtcConn->setRemoteDescription("offer", msg);
                    }
                }
            });

            // Send an EntityCreated message after a delay
            thread([session]() {
                this_thread::sleep_for(chrono::seconds(3));
                if (session && session->isConnected()) {
                    cout << "Sending EntityCreated message to client..." << endl;
                    auto result = session->sendEntityCreated(
                        12345,                  // entityId
                        "com.entropy.example",  // appId
                        "ExampleNode",          // typeName
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

        cout << "Server ready. Waiting for client connection..." << endl;
        cout << "Press Ctrl+C to quit" << endl;

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
