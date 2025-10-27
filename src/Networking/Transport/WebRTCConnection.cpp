/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "WebRTCConnection.h"
#include <stdexcept>
#include <optional>
#include <chrono>
#include <EntropyCore.h>
#include <Logging/Logger.h>
#include <cstring>

namespace EntropyEngine::Networking {

    // Debug helpers for readable logs
    static const char* rtcStateToString(rtcState s) {
        switch (s) {
            case RTC_NEW: return "RTC_NEW";
            case RTC_CONNECTING: return "RTC_CONNECTING";
            case RTC_CONNECTED: return "RTC_CONNECTED";
            case RTC_DISCONNECTED: return "RTC_DISCONNECTED";
            case RTC_FAILED: return "RTC_FAILED";
            case RTC_CLOSED: return "RTC_CLOSED";
        }
        return "RTC_?";
    }

    static const char* connStateToString(ConnectionState s) {
        switch (s) {
            case ConnectionState::Disconnected: return "Disconnected";
            case ConnectionState::Disconnecting: return "Disconnecting";
            case ConnectionState::Connecting:   return "Connecting";
            case ConnectionState::Connected:    return "Connected";
            case ConnectionState::Failed:       return "Failed";
        }
        return "?";
    }

    WebRTCConnection::WebRTCConnection(
        WebRTCConfig config,
        SignalingCallbacks signalingCallbacks,
        std::string dataChannelLabel
    )
        : _config(std::move(config))
        , _signalingCallbacks(std::move(signalingCallbacks))
        , _dataChannelLabel(std::move(dataChannelLabel))
        , _unreliableChannelLabel(_dataChannelLabel + "-unreliable")
    {
        _polite = _config.polite;
    }

    WebRTCConnection::~WebRTCConnection() {
        _destroying.store(true, std::memory_order_release);
        disconnect();
        shutdownCallbacks();

        // C API guarantees blocking until all callbacks complete
        if (_dataChannelId >= 0) {
            rtcDeleteDataChannel(_dataChannelId);
            _dataChannelId = -1;
        }

        if (_unreliableDataChannelId >= 0) {
            rtcDeleteDataChannel(_unreliableDataChannelId);
            _unreliableDataChannelId = -1;
        }

        if (_peerConnectionId >= 0) {
            rtcDeletePeerConnection(_peerConnectionId);
            _peerConnectionId = -1;
        }

        setMessageCallback(nullptr);
        setStateCallback(nullptr);
    }

    // C API callback adapters
    void WebRTCConnection::onLocalDescriptionCallback(int, const char* sdp, const char* type, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        ENTROPY_LOG_DEBUG(std::string("onLocalDescription: type=") + (type?type:"(null)") +
                          ", polite=" + (self->_polite ? "true" : "false") +
                          ", sdpLen=" + std::to_string(sdp ? (int)std::strlen(sdp) : 0));

        // Note: We do NOT clear _makingOffer here - it's cleared when we receive
        // a remote answer or when we accept a remote offer (see setRemoteDescription)

        if (self->_signalingCallbacks.onLocalDescription) {
            self->_signalingCallbacks.onLocalDescription(type, sdp);
        }
    }

    void WebRTCConnection::onLocalCandidateCallback(int, const char* cand, const char* mid, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        ENTROPY_LOG_DEBUG(std::string("onLocalCandidate: mid=") + (mid?mid:"(null)") +
                          ", len=" + std::to_string(cand ? (int)std::strlen(cand) : 0));
        if (self->_signalingCallbacks.onLocalCandidate) {
            self->_signalingCallbacks.onLocalCandidate(cand, mid);
        }
    }

    void WebRTCConnection::onStateChangeCallback(int, rtcState state, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        std::optional<ConnectionState> pending;
        {
            std::lock_guard<std::mutex> lock(self->_mutex);
            ConnectionState newState = self->_state;

            switch (state) {
                case RTC_NEW:
                case RTC_CONNECTING:
                    if (self->_state != ConnectionState::Connected) {
                        newState = ConnectionState::Connecting;
                    }
                    break;
                case RTC_CONNECTED:
                    // Don't set Connected here - wait for data channel
                    break;
                case RTC_DISCONNECTED:
                    newState = ConnectionState::Disconnected;
                    break;
                case RTC_FAILED:
                    newState = ConnectionState::Failed;
                    break;
                case RTC_CLOSED:
                    newState = ConnectionState::Disconnected;
                    break;
            }

            if (newState != self->_state) {
                ENTROPY_LOG_INFO(std::string("PeerConnection state change: ") + rtcStateToString(state) +
                                 " => " + connStateToString(newState));
                self->_state = newState;
                pending = newState;
            } else {
                ENTROPY_LOG_DEBUG(std::string("PeerConnection state change (no mapped transition): ") + rtcStateToString(state));
            }
        }

        if (pending && !self->_destroying.load(std::memory_order_acquire)) {
            self->onStateChanged(*pending);
        }
    }

    void WebRTCConnection::onSignalingStateChangeCallback(int, rtcSignalingState state, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        self->_signalingState.store(state, std::memory_order_release);

        const char* stateStr = "unknown";
        switch (state) {
            case RTC_SIGNALING_STABLE: stateStr = "stable"; break;
            case RTC_SIGNALING_HAVE_LOCAL_OFFER: stateStr = "have-local-offer"; break;
            case RTC_SIGNALING_HAVE_REMOTE_OFFER: stateStr = "have-remote-offer"; break;
            case RTC_SIGNALING_HAVE_LOCAL_PRANSWER: stateStr = "have-local-pranswer"; break;
            case RTC_SIGNALING_HAVE_REMOTE_PRANSWER: stateStr = "have-remote-pranswer"; break;
        }
        ENTROPY_LOG_DEBUG(std::string("Signaling state changed to: ") + stateStr);
    }

    void WebRTCConnection::onDataChannelCallback(int, int dc, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        char label[256];
        if (rtcGetDataChannelLabel(dc, label, sizeof(label)) < 0) {
            return;
        }

        ENTROPY_LOG_INFO(std::string("Incoming data channel negotiated: ") + label);

        std::lock_guard<std::mutex> lock(self->_mutex);
        std::string_view labelView(label);

        // Accept remote data channels only if different from our local ones
        // This callback fires when remote peer's channels are negotiated to us
        // Note: During reconnection, old channels will close naturally via onClosedCallback
        if (labelView == self->_dataChannelLabel) {
            if (self->_dataChannelId != dc) {
                self->_dataChannelId = dc;
                self->setupDataChannelCallbacks(dc, true);
            }
        } else if (labelView == self->_unreliableChannelLabel) {
            if (self->_unreliableDataChannelId != dc) {
                self->_unreliableDataChannelId = dc;
                self->setupDataChannelCallbacks(dc, false);
            }
        }
    }

    void WebRTCConnection::onOpenCallback(int id, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        char label[256];
        if (rtcGetDataChannelLabel(id, label, sizeof(label)) < 0) {
            return;
        }

        ENTROPY_LOG_INFO(std::string("Data channel opened: ") + label + ", id=" + std::to_string(id));

        // Only transition to Connected for reliable channel matching our current channel ID
        std::string_view labelView(label);
        if (labelView != self->_dataChannelLabel) return;

        std::optional<ConnectionState> pending;
        {
            std::lock_guard<std::mutex> lock(self->_mutex);
            // Verify this is our current reliable channel (not a stale/replaced one)
            if (id != self->_dataChannelId) {
                return;
            }
            if (self->_state != ConnectionState::Connected) {
                self->_state = ConnectionState::Connected;

                // Track that we've successfully connected at least once
                bool wasFirstConnection = (self->_lifecycleState == LifecycleState::Connecting);
                if (self->_lifecycleState == LifecycleState::Connecting ||
                    self->_lifecycleState == LifecycleState::Reconnecting) {
                    self->_lifecycleState = LifecycleState::Established;
                    ENTROPY_LOG_INFO(std::string("Connection established (") +
                        (wasFirstConnection ? "first" : "reconnected") + ")");
                }

                auto now = std::chrono::system_clock::now();
                self->_stats.connectTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                self->_stats.lastActivityTime = self->_stats.connectTime;
                pending = ConnectionState::Connected;
            }
        }

        if (pending && !self->_destroying.load(std::memory_order_acquire)) {
            self->onStateChanged(*pending);
        }
    }

    void WebRTCConnection::onClosedCallback(int id, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        char label[256];
        if (rtcGetDataChannelLabel(id, label, sizeof(label)) < 0) {
            return;
        }

        ENTROPY_LOG_INFO(std::string("Data channel closed: ") + label + ", id=" + std::to_string(id));

        // Only transition to Disconnected for reliable channel
        std::string_view labelView(label);
        if (labelView != self->_dataChannelLabel) return;

        std::optional<ConnectionState> pending;
        {
            std::lock_guard<std::mutex> lock(self->_mutex);
            if (self->_state != ConnectionState::Disconnected && self->_state != ConnectionState::Failed) {
                self->_state = ConnectionState::Disconnected;
                pending = ConnectionState::Disconnected;
            }
        }

        if (pending && !self->_destroying.load(std::memory_order_acquire)) {
            self->onStateChanged(*pending);
        }
    }

    void WebRTCConnection::onMessageCallback(int, const char* message, int size, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        // Defensive: drop empty/erroneous frames to avoid huge allocations on negative sizes
        if (size <= 0) return;

        // Performance note: This creates a vector (heap allocation) per message.
        // For high message rates (>10K msg/s), consider:
        // - Buffer pool with reusable vectors
        // - Callback API accepting std::span<const uint8_t> (breaking change)
        // Current approach prioritizes API simplicity over allocation efficiency.
        std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(message),
                                   reinterpret_cast<const uint8_t*>(message) + size);

        {
            std::lock_guard<std::mutex> lock(self->_mutex);
            self->_stats.bytesReceived += size;
            self->_stats.messagesReceived++;
            self->_stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        if (!self->_destroying.load(std::memory_order_acquire)) {
            self->onMessageReceived(data);
        }
    }

    Result<void> WebRTCConnection::connect() {
        std::lock_guard<std::mutex> lock(_mutex);

        // connect() is for initial connection only - use reconnect() for subsequent connections
        if (_lifecycleState != LifecycleState::NeverConnected) {
            return Result<void>::err(NetworkError::InvalidParameter,
                "Connection already initialized - use reconnect() for subsequent connections");
        }

        if (_state != ConnectionState::Disconnected) {
            return Result<void>::err(NetworkError::InvalidParameter, "Connection already active");
        }

        _state = ConnectionState::Connecting;
        _lifecycleState = LifecycleState::Connecting;
        _makingOffer.store(true, std::memory_order_release);
        _ignoreOffer.store(false, std::memory_order_release);  // Clean initial state

        try {
            setupPeerConnection();
            setupDataChannel();
            setupUnreliableDataChannel();
            return Result<void>::ok();
        } catch (const std::exception& e) {
            _state = ConnectionState::Disconnected;
            _lifecycleState = LifecycleState::NeverConnected;
            _makingOffer.store(false, std::memory_order_release);
            return Result<void>::err(NetworkError::ConnectionClosed,
                std::string("Failed to create peer connection: ") + e.what());
        }
    }

    Result<void> WebRTCConnection::reconnect() {
        std::optional<ConnectionState> pending;
        int pcId = -1;
        bool wasEstablished = false;

        {
            std::lock_guard<std::mutex> lock(_mutex);

            // Can only reconnect if we previously connected
            if (_lifecycleState == LifecycleState::NeverConnected) {
                return Result<void>::err(NetworkError::InvalidParameter,
                    "Cannot reconnect - never connected. Use connect() first.");
            }

            if (_lifecycleState == LifecycleState::Destroyed) {
                return Result<void>::err(NetworkError::InvalidParameter,
                    "Cannot reconnect - connection destroyed");
            }

            if (_peerConnectionId < 0) {
                return Result<void>::err(NetworkError::InvalidParameter,
                    "Cannot reconnect - peer connection not initialized");
            }

            // ICE restart can happen from any state
            ENTROPY_LOG_INFO("Initiating ICE restart for reconnection");

            // Check if we're reconnecting from an already-established connection BEFORE changing state
            wasEstablished = (_lifecycleState == LifecycleState::Established);

            _state = ConnectionState::Connecting;
            _lifecycleState = LifecycleState::Reconnecting;
            _makingOffer.store(true, std::memory_order_release);
            _reconnecting.store(true, std::memory_order_release);  // Track ICE restart in progress

            // Reset negotiation state for ICE restart
            _haveRemoteDescription.store(false, std::memory_order_release);
            _pendingRemoteCandidates.clear();
            _ignoreOffer.store(false, std::memory_order_release);  // Clean slate for new negotiation

            pcId = _peerConnectionId;
        }

        // Trigger ICE restart by creating a new offer with existing data channels
        // libdatachannel will automatically generate new ICE credentials
        // Do NOT create new data channels - ICE restart reuses existing channels
        int result = rtcSetLocalDescription(pcId, "offer");
        if (result < 0) {
            std::lock_guard<std::mutex> lock(_mutex);
            _state = ConnectionState::Disconnected;
            _lifecycleState = LifecycleState::Established;
            _makingOffer.store(false, std::memory_order_release);
            _reconnecting.store(false, std::memory_order_release);
            return Result<void>::err(NetworkError::ConnectionClosed,
                "Failed to trigger ICE restart");
        }

        // Check channel state outside mutex to avoid potential deadlock
        int dataChannelId = -1;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            dataChannelId = _dataChannelId;
        }

        bool channelStillOpen = wasEstablished && dataChannelId >= 0 && rtcIsOpen(dataChannelId);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            // If reconnecting from an already-established connection, and data channels
            // are still open, transition directly to Connected since onOpenCallback won't fire again
            if (channelStillOpen) {
                ENTROPY_LOG_INFO("Optimistically transitioning to Connected (data channel still open)");
                _state = ConnectionState::Connected;
                _lifecycleState = LifecycleState::Established;
                pending = ConnectionState::Connected;
            }
        }

        // Notify observers outside the lock
        if (pending && !_destroying.load(std::memory_order_acquire)) {
            onStateChanged(*pending);
        }

        return Result<void>::ok();
    }

    Result<void> WebRTCConnection::disconnect() {
        std::optional<ConnectionState> pending;

        {
            std::lock_guard<std::mutex> lock(_mutex);

            if (_state == ConnectionState::Disconnected) {
                return Result<void>::ok();
            }

            // Graceful disconnect for ICE restart - change state but keep connection alive
            // The peer connection and data channels remain intact for ICE restart
            _state = ConnectionState::Disconnected;
            pending = ConnectionState::Disconnected;
        }

        if (pending && !_destroying.load(std::memory_order_acquire)) {
            onStateChanged(*pending);
        }

        _makingOffer.store(false, std::memory_order_release);

        return Result<void>::ok();
    }

    bool WebRTCConnection::isConnected() const {
        return _state == ConnectionState::Connected;
    }

    Result<void> WebRTCConnection::send(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != ConnectionState::Connected) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Connection not established");
        }

        if (_dataChannelId < 0 || !rtcIsOpen(_dataChannelId)) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Data channel not open");
        }

        int result = rtcSendMessage(_dataChannelId, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        if (result < 0) {
            return Result<void>::err(NetworkError::InvalidMessage, "Failed to send data");
        }

        _stats.bytesSent += data.size();
        _stats.messagesSent++;
        _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return Result<void>::ok();
    }

    Result<void> WebRTCConnection::trySend(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != ConnectionState::Connected) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Connection not established");
        }

        if (_dataChannelId < 0 || !rtcIsOpen(_dataChannelId)) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Data channel not open");
        }

        int buffered = rtcGetBufferedAmount(_dataChannelId);
        if (buffered > 0) {
            return Result<void>::err(NetworkError::WouldBlock, "WebRTC data channel backpressured");
        }

        int result = rtcSendMessage(_dataChannelId, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        if (result < 0) {
            return Result<void>::err(NetworkError::InvalidMessage, "Failed to trySend data");
        }

        _stats.bytesSent += data.size();
        _stats.messagesSent++;
        _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return Result<void>::ok();
    }

    Result<void> WebRTCConnection::sendUnreliable(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != ConnectionState::Connected) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Connection not established");
        }

        if (_unreliableDataChannelId < 0 || !rtcIsOpen(_unreliableDataChannelId)) {
            return send(data);
        }

        int result = rtcSendMessage(_unreliableDataChannelId, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        if (result < 0) {
            return Result<void>::err(NetworkError::InvalidMessage, "Failed to send unreliable data");
        }

        _stats.bytesSent += data.size();
        _stats.messagesSent++;
        _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return Result<void>::ok();
    }

    ConnectionState WebRTCConnection::getState() const {
        return _state;
    }

    ConnectionStats WebRTCConnection::getStats() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _stats;
    }

    Result<void> WebRTCConnection::setRemoteDescription(const std::string& type, const std::string& sdp) {
        std::vector<std::pair<std::string, std::string>> toFlush;
        int pcId = -1;

        {
            std::lock_guard<std::mutex> lock(_mutex);

            if (_peerConnectionId < 0) {
                return Result<void>::err(NetworkError::InvalidParameter, "Peer connection not initialized");
            }

            // Perfect negotiation: Mozilla's exact pattern (RFC 8831)
            const bool isOffer = (type == "offer");

            // Calculate readyForOffer - EXACT Mozilla formula
            const bool readyForOffer = !_makingOffer.load(std::memory_order_acquire) &&
                (_signalingState.load(std::memory_order_acquire) == RTC_SIGNALING_STABLE ||
                 _isSettingRemoteAnswerPending.load(std::memory_order_acquire));

            const bool offerCollision = isOffer && !readyForOffer;

            // Set ignoreOffer flag based on collision
            _ignoreOffer.store(!_polite && offerCollision, std::memory_order_release);

            if (_ignoreOffer.load(std::memory_order_acquire)) {
                ENTROPY_LOG_WARNING("Impolite peer ignoring offer collision");
                return Result<void>::ok();  // Early return - ignore the offer
            }

            // Track if we're setting a remote answer
            _isSettingRemoteAnswerPending.store(type == "answer", std::memory_order_release);

            ENTROPY_LOG_DEBUG(std::string("setRemoteDescription: type=") + type +
                              ", sdpLen=" + std::to_string((int)sdp.size()) +
                              ", offerCollision=" + (offerCollision ? "true" : "false") +
                              ", polite=" + (_polite ? "true" : "false"));

            // Remember pc id for out-of-lock RTC calls
            pcId = _peerConnectionId;
        }

        // Set remote description - libdatachannel handles implicit rollback
        int result = rtcSetRemoteDescription(pcId, sdp.c_str(), type.c_str());

        {
            std::lock_guard<std::mutex> lock(_mutex);

            if (result < 0) {
                _isSettingRemoteAnswerPending.store(false, std::memory_order_release);
                return Result<void>::err(NetworkError::InvalidMessage, "Failed to set remote description");
            }

            _isSettingRemoteAnswerPending.store(false, std::memory_order_release);

            // Clear _makingOffer when appropriate
            const bool isOffer = (type == "offer");
            if (isOffer) {
                // Accepting remote offer (ours was implicitly rolled back or we weren't making one)
                _makingOffer.store(false, std::memory_order_release);
            } else if (type == "answer") {
                // Received answer to our offer - negotiation complete
                _makingOffer.store(false, std::memory_order_release);
                _reconnecting.store(false, std::memory_order_release);
                // Reset ignoreOffer flag - negotiation cycle complete
                _ignoreOffer.store(false, std::memory_order_release);
            }

            // Mark that we have a remote description - safe to apply ICE candidates now
            _haveRemoteDescription.store(true, std::memory_order_release);

            // Extract pending candidates to flush outside the lock
            if (!_pendingRemoteCandidates.empty()) {
                ENTROPY_LOG_INFO(std::string("Flushing ") + std::to_string(_pendingRemoteCandidates.size()) +
                               " pending ICE candidates");
                toFlush.swap(_pendingRemoteCandidates);
            }
        }

        // Flush candidates outside the lock to avoid re-entrancy if library calls back
        for (const auto& [candidate, mid] : toFlush) {
            int candResult = rtcAddRemoteCandidate(pcId, candidate.c_str(), mid.c_str());
            if (candResult < 0) {
                ENTROPY_LOG_WARNING("Failed to add queued remote candidate");
            }
        }

        return Result<void>::ok();
    }

    Result<void> WebRTCConnection::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_peerConnectionId < 0) {
            return Result<void>::err(NetworkError::InvalidParameter, "Peer connection not initialized");
        }

        // Ignore empty candidates (end-of-candidates signal)
        if (candidate.empty()) {
            ENTROPY_LOG_DEBUG("addRemoteCandidate: ignoring empty candidate (end-of-candidates)");
            return Result<void>::ok();
        }

        // ICE candidate gating: only apply candidates after setRemoteDescription succeeds
        if (!_haveRemoteDescription.load(std::memory_order_acquire)) {
            ENTROPY_LOG_DEBUG(std::string("Queuing ICE candidate (no remote description yet): mid=") + mid);
            _pendingRemoteCandidates.emplace_back(candidate, mid);
            return Result<void>::ok();
        }

        ENTROPY_LOG_DEBUG(std::string("addRemoteCandidate: mid=") + mid +
                          ", len=" + std::to_string((int)candidate.size()));

        int result = rtcAddRemoteCandidate(_peerConnectionId, candidate.c_str(), mid.c_str());
        if (result < 0) {
            // Mozilla pattern: silently fail if we're ignoring the associated offer
            if (!_ignoreOffer.load(std::memory_order_acquire)) {
                return Result<void>::err(NetworkError::InvalidMessage, "Failed to add remote candidate");
            }
            // Silently ignore the error if offer was ignored
            ENTROPY_LOG_DEBUG("Ignoring candidate error (offer was ignored)");
            return Result<void>::ok();
        }

        return Result<void>::ok();
    }

    bool WebRTCConnection::isReady() const {
        return _peerConnectionId >= 0;
    }

    void WebRTCConnection::setupPeerConnection() {
        rtcConfiguration config;
        std::memset(&config, 0, sizeof(config));

        // Build ICE servers array
        std::vector<const char*> iceServerPtrs;
        iceServerPtrs.reserve(_config.iceServers.size());
        for (const auto& server : _config.iceServers) {
            iceServerPtrs.push_back(server.c_str());
        }
        config.iceServers = iceServerPtrs.empty() ? nullptr : iceServerPtrs.data();
        config.iceServersCount = static_cast<int>(iceServerPtrs.size());

        if (!_config.proxyServer.empty()) {
            config.proxyServer = _config.proxyServer.c_str();
        }

        if (!_config.bindAddress.empty()) {
            config.bindAddress = _config.bindAddress.c_str();
        }

        if (_config.portRangeBegin > 0 && _config.portRangeEnd > 0) {
            config.portRangeBegin = static_cast<uint16_t>(_config.portRangeBegin);
            config.portRangeEnd = static_cast<uint16_t>(_config.portRangeEnd);
        }

        config.enableIceTcp = _config.enableIceTcp;
        config.maxMessageSize = _config.maxMessageSize;

        ENTROPY_LOG_INFO("Creating PeerConnection (ICE-TCP=" + std::string(config.enableIceTcp ? "on" : "off") +
                         ", bindAddress=" + (_config.bindAddress.empty()?"":_config.bindAddress) + ")");
        _peerConnectionId = rtcCreatePeerConnection(&config);
        if (_peerConnectionId < 0) {
            throw std::runtime_error("Failed to create peer connection");
        }

        rtcSetUserPointer(_peerConnectionId, this);
        rtcSetLocalDescriptionCallback(_peerConnectionId, onLocalDescriptionCallback);
        rtcSetLocalCandidateCallback(_peerConnectionId, onLocalCandidateCallback);
        rtcSetStateChangeCallback(_peerConnectionId, onStateChangeCallback);
        rtcSetSignalingStateChangeCallback(_peerConnectionId, onSignalingStateChangeCallback);
        rtcSetDataChannelCallback(_peerConnectionId, onDataChannelCallback);
    }

    void WebRTCConnection::setupDataChannel() {
        ENTROPY_LOG_INFO("Creating reliable data channel: label=" + _dataChannelLabel);
        _dataChannelId = rtcCreateDataChannel(_peerConnectionId, _dataChannelLabel.c_str());
        if (_dataChannelId < 0) {
            throw std::runtime_error("Failed to create data channel");
        }

        setupDataChannelCallbacks(_dataChannelId, true);
    }

    void WebRTCConnection::setupUnreliableDataChannel() {
        rtcDataChannelInit init;
        std::memset(&init, 0, sizeof(init));
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;

        ENTROPY_LOG_INFO("Creating unreliable data channel: label=" + _unreliableChannelLabel);
        _unreliableDataChannelId = rtcCreateDataChannelEx(_peerConnectionId,
            _unreliableChannelLabel.c_str(), &init);
        if (_unreliableDataChannelId < 0) {
            throw std::runtime_error("Failed to create unreliable data channel");
        }

        setupDataChannelCallbacks(_unreliableDataChannelId, false);
    }

    void WebRTCConnection::setupDataChannelCallbacks(int channelId, bool isReliable) {
        rtcSetUserPointer(channelId, this);
        rtcSetOpenCallback(channelId, onOpenCallback);
        rtcSetClosedCallback(channelId, onClosedCallback);
        rtcSetMessageCallback(channelId, onMessageCallback);
    }

} // namespace EntropyEngine::Networking
