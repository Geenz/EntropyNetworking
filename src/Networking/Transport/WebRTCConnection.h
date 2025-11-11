/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "NetworkConnection.h"
#include <rtc/rtc.h>
#include <rtc/rtc.hpp>
#include <rtc/track.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <optional>

namespace EntropyEngine::Networking {

    /**
     * @brief WebRTC-based network connection using data channels
     *
     * Implements NetworkConnection interface using libdatachannel for WebRTC data channels.
     * Supports reliable and unreliable data transfer over WebRTC.
     *
     * Supports two modes:
     * - Server mode: SignalingCallbacks provided (used by WebRTCServer)
     * - Client mode: Signaling URL provided, WebSocket managed internally
     */
    class WebRTCConnection : public NetworkConnection {
    public:
        /**
         * @brief Construct a new WebRTC connection
         * @param config WebRTC configuration (ICE servers, etc.)
         * @param signalingCallbacks Callbacks for sending signaling messages (server mode)
         * @param signalingUrl WebSocket URL for signaling (client mode, empty if using callbacks)
         * @param dataChannelLabel Label for the data channel (default: "entropy-data")
         */
        WebRTCConnection(
            WebRTCConfig config,
            SignalingCallbacks signalingCallbacks,
            std::string signalingUrl = "",
            std::string dataChannelLabel = "entropy-data"
        );

        ~WebRTCConnection() override;

        // NetworkConnection interface
        Result<void> connect() override;

        /**
         * @brief Gracefully close the connection without destroying peer connection
         *
         * This allows subsequent reconnection via reconnect() using ICE restart.
         * The peer connection remains alive for faster reconnection.
         */
        Result<void> disconnect() override;

        bool isConnected() const override;

        /**
         * @brief Reconnect using ICE restart without destroying peer connection
         *
         * This method triggers ICE restart by generating a new offer with fresh ICE
         * credentials. The peer connection remains alive, which is faster and more
         * reliable than destroying and recreating the connection.
         *
         * ICE restart in libdatachannel is performed by calling rtcSetLocalDescription(pc, "offer")
         * on an existing connection. The library automatically generates new ICE credentials
         * (ice-ufrag and ice-pwd) and triggers a new offer/answer exchange through signaling.
         *
         * @return Result<void> Success or error
         */
        Result<void> reconnect();

        Result<void> send(const std::vector<uint8_t>& data) override;
        Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;
        Result<void> trySend(const std::vector<uint8_t>& data) override;

        ConnectionState getState() const override;
        ConnectionType getType() const override { return ConnectionType::Remote; }
        ConnectionStats getStats() const override;

        /**
         * @brief Set remote description received from signaling
         * @param type SDP type ("offer" or "answer")
         * @param sdp SDP string
         */
        Result<void> setRemoteDescription(const std::string& type, const std::string& sdp);

        /**
         * @brief Add remote ICE candidate received from signaling
         * @param candidate ICE candidate string
         * @param mid Media stream identification tag
         */
        Result<void> addRemoteCandidate(const std::string& candidate, const std::string& mid);

        /**
         * @brief Check if peer connection is created and ready for signaling
         */
        bool isReady() const;

    private:
        // Internal connection lifecycle states
        enum class LifecycleState {
            NeverConnected,    // Initial state, never called connect()
            Connecting,        // First connection in progress
            Established,       // Connection was successful at least once
            Reconnecting,      // ICE restart in progress
            Destroyed          // Peer connection destroyed, no further ops allowed
        };

        // Callback context for safe callback lifetime management
        // Passed as void* user pointer to libdatachannel C API
        // Uses reference counting to track active callbacks
        struct CallbackContext {
            WebRTCConnection* connection;
            std::atomic<bool> valid{true};
            std::atomic<int> activeCallbacks{0};  // Count of callbacks currently executing
        };

        // RAII guard for callback execution tracking
        // Increments activeCallbacks on entry, decrements on exit
        struct CallbackGuard {
            CallbackContext* ctx;

            explicit CallbackGuard(CallbackContext* c) : ctx(c) {
                if (ctx) ctx->activeCallbacks.fetch_add(1, std::memory_order_acquire);
            }

            ~CallbackGuard() {
                if (ctx) ctx->activeCallbacks.fetch_sub(1, std::memory_order_release);
            }

            // Non-copyable, non-movable
            CallbackGuard(const CallbackGuard&) = delete;
            CallbackGuard& operator=(const CallbackGuard&) = delete;

            bool isValid() const {
                return ctx && ctx->valid.load(std::memory_order_acquire);
            }

            WebRTCConnection* getConnection() const {
                return ctx ? ctx->connection : nullptr;
            }
        };

        void setupPeerConnection();
        void setupDataChannel();
        void setupUnreliableDataChannel();
        void setupDataChannelCallbacks(int channelId, bool isReliable);
        void setupInternalWebSocket();  // Client mode: set up internal WebSocket for signaling

        // C API callback adapters (static with user pointer)
        static void onLocalDescriptionCallback(int pc, const char* sdp, const char* type, void* user);
        static void onLocalCandidateCallback(int pc, const char* cand, const char* mid, void* user);
        static void onStateChangeCallback(int pc, rtcState state, void* user);
        static void onSignalingStateChangeCallback(int pc, rtcSignalingState state, void* user);
        static void onDataChannelCallback(int pc, int dc, void* user);
        static void onOpenCallback(int id, void* user);
        static void onClosedCallback(int id, void* user);
        static void onMessageCallback(int id, const char* message, int size, void* user);

        // Callback context - deleted after all active callbacks complete
        // libdatachannel may fire callbacks from background threads even after
        // rtcSetXXXCallback(id, nullptr) is called. We use reference counting
        // (activeCallbacks) to track executing callbacks and wait for them to
        // complete before deleting the context in the destructor.
        CallbackContext* _callbackContext;

        WebRTCConfig _config;
        SignalingCallbacks _signalingCallbacks;
        std::string _signalingUrl;  // Client mode: signaling server URL
        std::shared_ptr<rtc::WebSocket> _webSocket;  // Client mode: managed WebSocket
        std::string _dataChannelLabel;
        std::string _unreliableChannelLabel;

        // Using C API handles directly for proper blocking cleanup semantics
        // The C++ wrapper doesn't implement the blocking guarantee documented in the C API
        int _peerConnectionId = -1;
        int _dataChannelId = -1;
        int _unreliableDataChannelId = -1;

        // Cached channel state to avoid rtcIsOpen() race conditions and deadlock
        std::atomic<bool> _dataChannelOpen{false};
        std::atomic<bool> _unreliableDataChannelOpen{false};

        std::atomic<ConnectionState> _state{ConnectionState::Disconnected};

        // Perfect negotiation: Mozilla pattern flags (RFC 8831)
        std::atomic<bool> _makingOffer{false};
        std::atomic<bool> _isSettingRemoteAnswerPending{false};
        std::atomic<bool> _ignoreOffer{false};
        std::atomic<bool> _reconnecting{false};  // Tracks ICE restart in progress
        bool _polite = false;

        // Internal lifecycle state tracking (requires mutex)
        LifecycleState _lifecycleState{LifecycleState::NeverConnected};

        // Perfect negotiation: signaling state and ICE candidate gating
        std::atomic<rtcSignalingState> _signalingState{RTC_SIGNALING_STABLE};
        std::atomic<bool> _haveRemoteDescription{false};
        std::vector<std::pair<std::string, std::string>> _pendingRemoteCandidates; // {candidate, mid} (requires mutex)

        mutable std::mutex _mutex;
        ConnectionStats _stats;  // Atomic counters (no mutex needed for updates)
    };

} // namespace EntropyEngine::Networking
