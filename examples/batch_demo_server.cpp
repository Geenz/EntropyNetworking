// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../src/Networking/Transport/ConnectionManager.h"
#include "../src/Networking/Transport/WebRTCConnection.h"
#include "../src/Networking/Session/SessionManager.h"
#include "../src/Networking/Session/BatchManager.h"
#include "../src/Networking/Core/ConnectionTypes.h"
#include <EntropyCore.h>
#include <Concurrency/WorkService.h>
#include <Concurrency/WorkContractGroup.h>
#include <rtc/rtc.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <random>

using namespace std;
using namespace EntropyEngine;
using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;

int main() {
    try {
        cout << "Starting EntropyNetworking Batch Demo Server" << endl;

        // Create managers
        ConnectionManager connMgr(64);
        SessionManager sessMgr(&connMgr, 64);

        // Create WorkService for executing work contracts
        WorkService::Config workConfig;
        workConfig.threadCount = 4;
        WorkService workService(workConfig);

        // Create WorkContractGroup for batch processing
        WorkContractGroup batchGroup(1024, "BatchProcessing");
        workService.addWorkContractGroup(&batchGroup);
        workService.start();

        cout << "WorkService started with " << workConfig.threadCount << " threads" << endl;

        // Create WebSocket server for signaling
        rtc::WebSocketServer::Configuration wsConfig;
        wsConfig.port = 8080;
        wsConfig.enableTls = false;

        rtc::WebSocketServer wsServer(wsConfig);
        cout << "WebSocket signaling server listening on port 8080" << endl;

        SessionHandle session;
        shared_ptr<BatchManager> batchManager;

        // Handle incoming WebSocket connections
        wsServer.onClient([&](shared_ptr<rtc::WebSocket> ws) {
            cout << "Client connected to signaling server" << endl;

            // Create connection with WebRTC backend
            ConnectionConfig config;
            config.type = ConnectionType::Remote;
            config.backend = ConnectionBackend::WebRTC;

            config.signalingCallbacks.onLocalDescription = [ws](const string& type, const string& sdp) {
                cout << "Sending " << type << " to client" << endl;
                ws->send(sdp);
            };

            config.signalingCallbacks.onLocalCandidate = [ws](const string& candidate, const string& mid) {
                cout << "Sending ICE candidate to client" << endl;
                ws->send(candidate + "|" + mid);
            };

            auto conn = connMgr.openConnection(config);
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

            // Create BatchManager
            batchManager = make_shared<BatchManager>(session, 16); // 16ms = 60Hz

            // Set up protocol message callbacks
            sessMgr.setEntityCreatedCallback(session, [](uint64_t entityId, const string& appId,
                                                          const string& typeName, uint64_t parentId) {
                cout << "Server received EntityCreated:" << endl;
                cout << "  Entity ID: " << entityId << endl;
                cout << "  App ID: " << appId << endl;
                cout << "  Type: " << typeName << endl;
                cout << "  Parent ID: " << parentId << endl;
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

            // Handle signaling messages
            ws->onMessage([&connMgr, conn](auto data) {
                if (holds_alternative<string>(data)) {
                    string msg = get<string>(data);

                    auto* webrtcConn = dynamic_cast<WebRTCConnection*>(connMgr.getConnectionPointer(conn));
                    if (!webrtcConn) return;

                    size_t separator = msg.find('|');
                    if (separator != string::npos) {
                        string candidateStr = msg.substr(0, separator);
                        string mid = msg.substr(separator + 1);
                        cout << "Received ICE candidate from client" << endl;
                        webrtcConn->addRemoteCandidate(candidateStr, mid);
                    } else {
                        cout << "Received SDP offer from client" << endl;
                        webrtcConn->setRemoteDescription("offer", msg);
                    }
                }
            });
        });

        // Simulate multiple entities with frequently updating properties
        thread simulationThread([&]() {
            // Wait for connection
            while (!session.valid() || !session.isConnected()) {
                this_thread::sleep_for(chrono::milliseconds(100));
            }

            cout << "Connection established, starting simulation..." << endl;
            this_thread::sleep_for(chrono::seconds(2));

            cout << "\n=== Starting property update simulation ===" << endl;

            random_device rd;
            mt19937 gen(rd());
            uniform_real_distribution<float> posDist(-10.0f, 10.0f);
            uniform_real_distribution<float> rotDist(0.0f, 1.0f);

            // Simulate 10 entities with transform properties
            const int NUM_ENTITIES = 10;
            uint64_t entityIds[NUM_ENTITIES];

            for (int i = 0; i < NUM_ENTITIES; i++) {
                entityIds[i] = 1000 + i;
            }

            // Update properties rapidly (200 Hz) to demonstrate batching
            for (int frame = 0; frame < 300; frame++) {
                if (!session.valid() || !session.isConnected()) break;

                for (int i = 0; i < NUM_ENTITIES; i++) {
                    // Compute property hashes for position and rotation
                    auto posHash = computePropertyHash(
                        entityIds[i],
                        "com.entropy.demo",
                        "Transform",
                        "position"
                    );

                    auto rotHash = computePropertyHash(
                        entityIds[i],
                        "com.entropy.demo",
                        "Transform",
                        "rotation"
                    );

                    // Update with random values
                    Vec3 position{posDist(gen), posDist(gen), posDist(gen)};
                    Quat rotation{rotDist(gen), rotDist(gen), rotDist(gen), rotDist(gen)};

                    batchManager->updateProperty(posHash, PropertyType::Vec3, position);
                    batchManager->updateProperty(rotHash, PropertyType::Quat, rotation);
                }

                this_thread::sleep_for(chrono::milliseconds(5)); // 200 Hz update rate

                // Print stats every 60 frames
                if ((frame + 1) % 60 == 0) {
                    auto stats = batchManager->getStats();
                    cout << "\n--- Batch Statistics (frame " << (frame + 1) << ") ---" << endl;
                    cout << "  Total batches sent: " << stats.totalBatchesSent << endl;
                    cout << "  Total updates sent: " << stats.totalUpdatesSent << endl;
                    cout << "  Batches dropped: " << stats.batchesDropped << endl;
                    cout << "  Updates deduped: " << stats.updatesDeduped << endl;
                    cout << "  Average batch size: " << stats.averageBatchSize << endl;
                    cout << "  Current batch interval: " << stats.currentBatchInterval << "ms" << endl;
                    cout << "  Pending updates: " << batchManager->getPendingCount() << endl;
                }
            }

            cout << "\n=== Simulation complete ===" << endl;
            auto finalStats = batchManager->getStats();
            cout << "\n--- Final Statistics ---" << endl;
            cout << "  Total batches sent: " << finalStats.totalBatchesSent << endl;
            cout << "  Total updates sent: " << finalStats.totalUpdatesSent << endl;
            cout << "  Batches dropped: " << finalStats.batchesDropped << endl;
            cout << "  Updates deduped: " << finalStats.updatesDeduped << endl;
            cout << "  Average batch size: " << finalStats.averageBatchSize << endl;

            // Calculate efficiency
            int totalIndividualUpdates = 300 * NUM_ENTITIES * 2; // 300 frames, 10 entities, 2 properties each
            float batchingEfficiency = 100.0f * (1.0f - (float)finalStats.totalUpdatesSent / totalIndividualUpdates);
            cout << "  Batching efficiency: " << batchingEfficiency << "%" << endl;
        });

        // Main application loop: continuously schedule batch processing work contracts
        thread batchSchedulerThread([&]() {
            while (true) {
                if (batchManager) {
                    // Schedule a fire-and-forget work contract to process the batch
                    auto handle = batchGroup.createContract([&]() {
                        batchManager->processBatch();
                    });
                    handle.schedule();
                }

                // Sleep for batch interval (16ms = 60Hz)
                this_thread::sleep_for(chrono::milliseconds(16));
            }
        });

        cout << "Server ready. Waiting for client connection..." << endl;
        cout << "Press Ctrl+C to quit" << endl;

        // Keep main thread alive
        simulationThread.join();

        // Cleanup
        cout << "\nShutting down..." << endl;
        batchSchedulerThread.detach();
        workService.stop();

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
