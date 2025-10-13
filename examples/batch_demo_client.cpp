// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/WebRTCConnection.h"
#include "../src/Networking/Session/SessionManager.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include "../src/Networking/Protocol/MessageSerializer.h"
#include "src/Networking/Protocol/entropy.capnp.h"
#include <EntropyCore.h>
#include <Concurrency/WorkService.h>
#include <Concurrency/WorkContractGroup.h>
#include <rtc/rtc.hpp>
#include <capnp/message.h>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>

using namespace std;
using namespace EntropyEngine;
using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;

int main() {
    try {
        cout << "Starting EntropyNetworking Batch Demo Client" << endl;

        // Create managers
        ConnectionManager connMgr(64);
        SessionManager sessMgr(&connMgr, 64);

        // Create WorkService for executing work contracts
        WorkService::Config workConfig;
        workConfig.threadCount = 4;
        WorkService workService(workConfig);

        // Create WorkContractGroup for message processing
        WorkContractGroup messageGroup(1024, "MessageProcessing");
        workService.addWorkContractGroup(&messageGroup);
        workService.start();

        cout << "WorkService started with " << workConfig.threadCount << " threads" << endl;

        // Connect to signaling server
        auto ws = make_shared<rtc::WebSocket>();
        cout << "Connecting to signaling server at ws://localhost:8080" << endl;

        ConnectionHandle conn;
        SessionHandle session;

        atomic<uint64_t> batchesReceived{0};
        atomic<uint64_t> updatesReceived{0};
        atomic<uint64_t> bytesReceived{0};

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

            // Set up property update callback to track received batches
            sessMgr.setPropertyUpdateCallback(session, [&](const std::vector<uint8_t>& batchData) {
                batchesReceived++;
                bytesReceived += batchData.size();

                // Deserialize to count updates in this batch
                auto deserialized = deserialize(batchData);
                if (deserialized.success()) {
                    try {
                        kj::ArrayPtr<const ::capnp::word> words(
                            reinterpret_cast<const ::capnp::word*>(deserialized.value.begin()),
                            deserialized.value.size()
                        );
                        ::capnp::FlatArrayMessageReader reader(words);
                        auto message = reader.getRoot<Message>();

                        if (message.isPropertyUpdateBatch()) {
                            auto batch = message.getPropertyUpdateBatch();
                            updatesReceived += batch.getUpdates().size();
                        }
                    } catch (...) {
                        // Ignore deserialization errors for stats
                    }
                }
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

        // Statistics reporting thread
        thread statsThread([&]() {
            this_thread::sleep_for(chrono::seconds(3)); // Wait for connection

            cout << "\n=== Starting statistics reporting ===" << endl;

            auto startTime = chrono::steady_clock::now();
            uint64_t lastBatches = 0;
            uint64_t lastUpdates = 0;
            uint64_t lastBytes = 0;

            while (true) {
                this_thread::sleep_for(chrono::seconds(2));

                auto now = chrono::steady_clock::now();
                auto elapsed = chrono::duration_cast<chrono::seconds>(now - startTime).count();

                uint64_t currentBatches = batchesReceived.load();
                uint64_t currentUpdates = updatesReceived.load();
                uint64_t currentBytes = bytesReceived.load();

                uint64_t deltaBatches = currentBatches - lastBatches;
                uint64_t deltaUpdates = currentUpdates - lastUpdates;
                uint64_t deltaBytes = currentBytes - lastBytes;

                cout << "\n--- Receive Statistics (t=" << elapsed << "s) ---" << endl;
                cout << "  Total batches received: " << currentBatches << endl;
                cout << "  Total updates received: " << currentUpdates << endl;
                cout << "  Total bytes received: " << currentBytes << endl;
                cout << "  Recent rate (2s): " << (deltaBatches / 2.0) << " batches/s, "
                     << (deltaUpdates / 2.0) << " updates/s, "
                     << (deltaBytes / 2048.0) << " KB/s" << endl;

                if (currentBatches > 0) {
                    cout << "  Average batch size: " << (currentUpdates / (double)currentBatches) << " updates" << endl;
                    cout << "  Average batch bytes: " << (currentBytes / currentBatches) << " bytes" << endl;
                }

                lastBatches = currentBatches;
                lastUpdates = currentUpdates;
                lastBytes = currentBytes;
            }
        });

        // Keep running
        statsThread.join();

        workService.stop();

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
