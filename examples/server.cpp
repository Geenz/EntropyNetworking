// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <rtc/rtc.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace std;

int main() {
    try {
        cout << "Starting EntropyNetworking Server Example" << endl;

        // Create WebSocket server for signaling
        rtc::WebSocketServer::Configuration wsConfig;
        wsConfig.port = 8080;
        wsConfig.enableTls = false;

        rtc::WebSocketServer wsServer(wsConfig);
        cout << "WebSocket signaling server listening on port 8080" << endl;

        shared_ptr<rtc::PeerConnection> peerConnection;
        shared_ptr<rtc::DataChannel> dataChannel;

        // Handle incoming WebSocket connections
        wsServer.onClient([&](shared_ptr<rtc::WebSocket> ws) {
            cout << "Client connected to signaling server" << endl;

            // Create peer connection for this client
            rtc::Configuration config;
            // No STUN/TURN needed for local connections

            peerConnection = make_shared<rtc::PeerConnection>(config);

            // Handle incoming data channels from client
            peerConnection->onDataChannel([](shared_ptr<rtc::DataChannel> dc) {
                cout << "Data channel '" << dc->label() << "' received from client" << endl;

                dc->onOpen([dc]() {
                    cout << "Data channel open on server" << endl;
                    // Give a moment for things to settle
                    this_thread::sleep_for(chrono::milliseconds(100));
                    cout << "Sending hello from server" << endl;
                    dc->send("Hello from server!");
                });

                dc->onMessage([dc](auto data) {
                    if (holds_alternative<string>(data)) {
                        cout << "Server received message: " << get<string>(data) << endl;
                        // Echo back
                        dc->send("Server got your message!");
                    }
                });

                dc->onClosed([]() {
                    cout << "Data channel closed on server" << endl;
                });
            });

            // Send local description to client via WebSocket
            peerConnection->onLocalDescription([ws](rtc::Description desc) {
                cout << "Sending " << desc.typeString() << " to client" << endl;
                ws->send(string(desc));
            });

            // Send local candidates to client via WebSocket
            peerConnection->onLocalCandidate([ws](rtc::Candidate candidate) {
                cout << "Sending ICE candidate to client" << endl;
                ws->send(candidate.candidate() + "|" + candidate.mid());
            });

            // Handle messages from client (SDP offer and ICE candidates)
            ws->onMessage([peerConnection](auto data) {
                if (holds_alternative<string>(data)) {
                    string msg = get<string>(data);

                    // Check if it's an ICE candidate (contains |)
                    size_t separator = msg.find('|');
                    if (separator != string::npos) {
                        string candidateStr = msg.substr(0, separator);
                        string mid = msg.substr(separator + 1);
                        cout << "Received ICE candidate from client" << endl;
                        peerConnection->addRemoteCandidate(rtc::Candidate(candidateStr, mid));
                    } else {
                        // It's an SDP offer
                        cout << "Received SDP offer from client" << endl;
                        peerConnection->setRemoteDescription(rtc::Description(msg, "offer"));
                    }
                }
            });

            ws->onClosed([]() {
                cout << "Client disconnected from signaling server" << endl;
            });
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
