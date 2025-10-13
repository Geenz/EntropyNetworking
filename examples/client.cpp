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
        cout << "Starting EntropyNetworking Client Example" << endl;

        // Connect to signaling server
        auto ws = make_shared<rtc::WebSocket>();
        cout << "Connecting to signaling server at ws://localhost:8080" << endl;

        shared_ptr<rtc::PeerConnection> peerConnection;
        shared_ptr<rtc::DataChannel> dataChannel;

        // Handle messages from server (SDP answer and ICE candidates)
        ws->onMessage([&peerConnection](auto data) {
            if (holds_alternative<string>(data)) {
                string msg = get<string>(data);

                // Check if it's an ICE candidate (contains |)
                size_t separator = msg.find('|');
                if (separator != string::npos) {
                    string candidateStr = msg.substr(0, separator);
                    string mid = msg.substr(separator + 1);
                    cout << "Received ICE candidate from server" << endl;
                    peerConnection->addRemoteCandidate(rtc::Candidate(candidateStr, mid));
                } else {
                    // It's an SDP answer
                    cout << "Received SDP answer from server" << endl;
                    peerConnection->setRemoteDescription(rtc::Description(msg, "answer"));
                }
            }
        });

        ws->onOpen([&]() {
            cout << "Connected to signaling server" << endl;

            // Create peer connection
            rtc::Configuration config;
            // No STUN/TURN needed for local connections

            peerConnection = make_shared<rtc::PeerConnection>(config);

            // Send local description to server via WebSocket
            peerConnection->onLocalDescription([ws](rtc::Description desc) {
                cout << "Sending " << desc.typeString() << " to server" << endl;
                ws->send(string(desc));
            });

            // Send local candidates to server via WebSocket
            peerConnection->onLocalCandidate([ws](rtc::Candidate candidate) {
                cout << "Sending ICE candidate to server" << endl;
                ws->send(candidate.candidate() + "|" + candidate.mid());
            });

            // Create data channel (this triggers offer creation)
            dataChannel = peerConnection->createDataChannel("entropy-data");

            dataChannel->onOpen([dataChannel]() {
                cout << "Data channel open, sending hello from client" << endl;
                dataChannel->send("Hello from client!");
            });

            dataChannel->onMessage([dataChannel](auto data) {
                if (holds_alternative<string>(data)) {
                    cout << "Client received message: " << get<string>(data) << endl;
                }
            });

            dataChannel->onClosed([]() {
                cout << "Data channel closed" << endl;
            });
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
