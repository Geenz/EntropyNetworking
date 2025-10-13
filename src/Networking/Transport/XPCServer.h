// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "LocalServer.h"
#include <string>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

#if defined(__APPLE__)
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>
#endif

namespace EntropyEngine::Networking {

#if defined(__APPLE__)

/**
 * @brief XPC-based server for iOS/visionOS/macOS IPC
 *
 * XPCServer provides server-side XPC connection listening. This is the primary
 * server mechanism on iOS/visionOS/iPadOS where Unix sockets are unavailable.
 *
 * Two server modes:
 * 1. Mach service (recommended for iOS/visionOS):
 *    - Registered with launchd via Info.plist
 *    - Service name: bundle ID + suffix (e.g., "com.example.MyApp.xpc")
 *    - Survives process restart
 *    - Requires XPCService target in Xcode project
 *
 * 2. Anonymous listener (for testing/macOS):
 *    - No registration required
 *    - Clients connect via xpc_endpoint_t
 *    - Dies with process
 *
 * Platform requirements:
 * - iOS/visionOS: Must use Mach service mode with XPCService target
 * - macOS: Can use either mode
 * - Requires NSXPCConnectionCodeSigningRequirement in Info.plist for security
 *
 * Usage (iOS/visionOS XPCService):
 * @code
 * // In your XPCService target's main.cpp:
 * ConnectionManager connMgr(64);
 * auto server = createLocalServer(&connMgr, "com.example.MyApp.xpc");
 * server->listen();
 *
 * // Run event loop
 * xpc_main(^(xpc_connection_t connection) {
 *     // Connection accepted by XPCServer automatically
 * });
 * @endcode
 */
class XPCServer : public LocalServer {
public:
    /**
     * @brief Creates XPC server for named Mach service
     * @param connMgr Connection manager that will own accepted connections
     * @param serviceName Mach service name (bundle ID)
     */
    XPCServer(ConnectionManager* connMgr, std::string serviceName);

    ~XPCServer() override;

    // LocalServer interface
    Result<void> listen() override;
    ConnectionHandle accept() override;
    Result<void> close() override;
    bool isListening() const override { return _listening.load(std::memory_order_acquire); }

    // XPC-specific: set optional peer validator using peer pid
    using PeerValidator = std::function<bool(pid_t)>;
    void setPeerValidator(PeerValidator validator) {
        _peerValidator = std::move(validator);
    }

    // EntropyObject interface
    const char* className() const noexcept override { return "XPCServer"; }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;

private:
    ConnectionManager* _connMgr;
    std::string _serviceName;
    xpc_connection_t _listener{nullptr};
    dispatch_queue_t _queue{nullptr};

    std::atomic<bool> _listening{false};
    std::atomic<bool> _shouldStop{false};

    // Optional peer validator
    PeerValidator _peerValidator;

    // Queue of pending connections
    std::queue<xpc_connection_t> _pendingConnections;
    std::mutex _queueMutex;
    std::condition_variable _queueCV;

    void setupListener();
    void handleNewConnection(xpc_connection_t connection);
};

#endif // __APPLE__

} // namespace EntropyEngine::Networking
