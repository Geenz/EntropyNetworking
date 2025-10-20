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

namespace EntropyEngine::Networking {

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

        self->_makingOffer.store(false, std::memory_order_release);

        if (self->_signalingCallbacks.onLocalDescription) {
            self->_signalingCallbacks.onLocalDescription(type, sdp);
        }
    }

    void WebRTCConnection::onLocalCandidateCallback(int, const char* cand, const char* mid, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

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
                self->_state = newState;
                pending = newState;
            }
        }

        if (pending && !self->_destroying.load(std::memory_order_acquire)) {
            self->onStateChanged(*pending);
        }
    }

    void WebRTCConnection::onDataChannelCallback(int, int dc, void* user) {
        auto* self = static_cast<WebRTCConnection*>(user);
        if (self->_destroying.load(std::memory_order_acquire)) return;

        char label[256];
        if (rtcGetDataChannelLabel(dc, label, sizeof(label)) < 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(self->_mutex);
        std::string_view labelView(label);

        // Accept remote data channels only if different from our local ones
        // This callback fires when remote peer's channels are negotiated to us
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

        ENTROPY_LOG_INFO(std::string("Data channel opened: ") + label);

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

        if (_state != ConnectionState::Disconnected) {
            return Result<void>::err(NetworkError::InvalidParameter, "Connection already active");
        }

        _state = ConnectionState::Connecting;
        _makingOffer.store(true, std::memory_order_release);

        try {
            setupPeerConnection();
            setupDataChannel();
            setupUnreliableDataChannel();
            return Result<void>::ok();
        } catch (const std::exception& e) {
            _state = ConnectionState::Disconnected;
            _makingOffer.store(false, std::memory_order_release);
            return Result<void>::err(NetworkError::ConnectionClosed,
                std::string("Failed to create peer connection: ") + e.what());
        }
    }

    Result<void> WebRTCConnection::disconnect() {
        std::optional<ConnectionState> pending;

        {
            std::lock_guard<std::mutex> lock(_mutex);

            if (_state == ConnectionState::Disconnected) {
                return Result<void>::ok();
            }

            _state = ConnectionState::Disconnected;
            pending = ConnectionState::Disconnected;
        }

        if (pending && !_destroying.load(std::memory_order_acquire)) {
            onStateChanged(*pending);
        }

        // Detach callbacks and user pointers to prevent further calls into this during teardown
        auto detachChannel = [](int id) {
            if (id < 0) return;
            rtcSetUserPointer(id, nullptr);
            rtcSetOpenCallback(id, nullptr);
            rtcSetClosedCallback(id, nullptr);
            rtcSetMessageCallback(id, nullptr);
        };
        auto detachPeer = [](int id) {
            if (id < 0) return;
            rtcSetUserPointer(id, nullptr);
            rtcSetLocalDescriptionCallback(id, nullptr);
            rtcSetLocalCandidateCallback(id, nullptr);
            rtcSetStateChangeCallback(id, nullptr);
            rtcSetDataChannelCallback(id, nullptr);
        };

        detachChannel(_dataChannelId);
        detachChannel(_unreliableDataChannelId);
        detachPeer(_peerConnectionId);

        // Close and delete handles (C API guarantees blocking until no more callbacks)
        if (_dataChannelId >= 0) {
            rtcClose(_dataChannelId);
            rtcDeleteDataChannel(_dataChannelId);
            _dataChannelId = -1;
        }
        if (_unreliableDataChannelId >= 0) {
            rtcClose(_unreliableDataChannelId);
            rtcDeleteDataChannel(_unreliableDataChannelId);
            _unreliableDataChannelId = -1;
        }
        if (_peerConnectionId >= 0) {
            rtcClose(_peerConnectionId);
            rtcDeletePeerConnection(_peerConnectionId);
            _peerConnectionId = -1;
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
        std::lock_guard<std::mutex> lock(_mutex);

        if (_peerConnectionId < 0) {
            return Result<void>::err(NetworkError::InvalidParameter, "Peer connection not initialized");
        }

        // Perfect negotiation: detect and resolve offer collisions
        const bool isOffer = (type == "offer");
        bool offerCollision = isOffer && _makingOffer.load(std::memory_order_acquire);

        if (offerCollision) {
            if (!_polite) {
                // Impolite peer ignores the incoming offer
                return Result<void>::ok();
            }
            // Polite peer rolls back its local offer to accept remote offer
            int rollbackResult = rtcSetRemoteDescription(_peerConnectionId, "", "rollback");
            if (rollbackResult < 0) {
                ENTROPY_LOG_WARNING("Rollback failed or unsupported; ignoring incoming offer");
                return Result<void>::ok();
            } else {
                // After a successful rollback we are no longer making an offer
                _makingOffer.store(false, std::memory_order_release);
            }
        }

        // When accepting a remote offer, we are no longer making an offer
        if (isOffer) {
            _makingOffer.store(false, std::memory_order_release);
        }

        int result = rtcSetRemoteDescription(_peerConnectionId, sdp.c_str(), type.c_str());
        if (result < 0) {
            return Result<void>::err(NetworkError::InvalidMessage, "Failed to set remote description");
        }

        return Result<void>::ok();
    }

    Result<void> WebRTCConnection::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_peerConnectionId < 0) {
            return Result<void>::err(NetworkError::InvalidParameter, "Peer connection not initialized");
        }

        int result = rtcAddRemoteCandidate(_peerConnectionId, candidate.c_str(), mid.c_str());
        if (result < 0) {
            return Result<void>::err(NetworkError::InvalidMessage, "Failed to add remote candidate");
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

        _peerConnectionId = rtcCreatePeerConnection(&config);
        if (_peerConnectionId < 0) {
            throw std::runtime_error("Failed to create peer connection");
        }

        rtcSetUserPointer(_peerConnectionId, this);
        rtcSetLocalDescriptionCallback(_peerConnectionId, onLocalDescriptionCallback);
        rtcSetLocalCandidateCallback(_peerConnectionId, onLocalCandidateCallback);
        rtcSetStateChangeCallback(_peerConnectionId, onStateChangeCallback);
        rtcSetDataChannelCallback(_peerConnectionId, onDataChannelCallback);
    }

    void WebRTCConnection::setupDataChannel() {
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
