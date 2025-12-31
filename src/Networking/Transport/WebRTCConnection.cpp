/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "WebRTCConnection.h"

#include <EntropyCore.h>
#include <Logging/Logger.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace EntropyEngine::Networking
{

// Debug helpers for readable logs
static const char* rtcStateToString(rtcState s) {
    switch (s) {
        case RTC_NEW:
            return "RTC_NEW";
        case RTC_CONNECTING:
            return "RTC_CONNECTING";
        case RTC_CONNECTED:
            return "RTC_CONNECTED";
        case RTC_DISCONNECTED:
            return "RTC_DISCONNECTED";
        case RTC_FAILED:
            return "RTC_FAILED";
        case RTC_CLOSED:
            return "RTC_CLOSED";
    }
    return "RTC_?";
}

// Binary envelope framing for signaling messages
// Format: [Type:1B][Length:4B big-endian][Payload]
// Type: 0x01=SDP, 0x02=ICE
// Payload: For SDP: type\0sdp, For ICE: candidate\0mid

enum class SignalingMessageType : uint8_t
{
    SDP = 0x01,
    ICE = 0x02
};

// Encode uint32_t to big-endian bytes
static void encodeU32BigEndian(uint32_t value, uint8_t* dest) {
    dest[0] = (value >> 24) & 0xFF;
    dest[1] = (value >> 16) & 0xFF;
    dest[2] = (value >> 8) & 0xFF;
    dest[3] = value & 0xFF;
}

// Decode big-endian bytes to uint32_t
static uint32_t decodeU32BigEndian(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

// Encode SDP description into binary envelope
static std::vector<uint8_t> encodeSDPMessage(const std::string& type, const std::string& sdp) {
    // Payload: type\0sdp
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), type.begin(), type.end());
    payload.push_back(0);  // null terminator
    payload.insert(payload.end(), sdp.begin(), sdp.end());

    // Envelope: [Type:1B][Length:4B][Payload]
    std::vector<uint8_t> message;
    message.reserve(5 + payload.size());
    message.push_back(static_cast<uint8_t>(SignalingMessageType::SDP));

    uint8_t lengthBytes[4];
    encodeU32BigEndian(static_cast<uint32_t>(payload.size()), lengthBytes);
    message.insert(message.end(), lengthBytes, lengthBytes + 4);
    message.insert(message.end(), payload.begin(), payload.end());

    return message;
}

// Encode ICE candidate into binary envelope
static std::vector<uint8_t> encodeICEMessage(const std::string& candidate, const std::string& mid) {
    // Payload: candidate\0mid
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), candidate.begin(), candidate.end());
    payload.push_back(0);  // null terminator
    payload.insert(payload.end(), mid.begin(), mid.end());

    // Envelope: [Type:1B][Length:4B][Payload]
    std::vector<uint8_t> message;
    message.reserve(5 + payload.size());
    message.push_back(static_cast<uint8_t>(SignalingMessageType::ICE));

    uint8_t lengthBytes[4];
    encodeU32BigEndian(static_cast<uint32_t>(payload.size()), lengthBytes);
    message.insert(message.end(), lengthBytes, lengthBytes + 4);
    message.insert(message.end(), payload.begin(), payload.end());

    return message;
}

// Decode binary envelope message
// Returns: {type, str1, str2} where for SDP: str1=type, str2=sdp; for ICE: str1=candidate, str2=mid
static std::optional<std::tuple<SignalingMessageType, std::string, std::string>> decodeSignalingMessage(
    const std::vector<uint8_t>& data) {
    // Minimum size: Type(1) + Length(4) = 5 bytes
    if (data.size() < 5) {
        ENTROPY_LOG_ERROR("Signaling message too short");
        return std::nullopt;
    }

    SignalingMessageType msgType = static_cast<SignalingMessageType>(data[0]);
    uint32_t payloadLength = decodeU32BigEndian(&data[1]);

    // Verify payload length
    if (data.size() != 5 + payloadLength) {
        ENTROPY_LOG_ERROR(
            std::format("Signaling message length mismatch: expected {}, got {}", 5 + payloadLength, data.size()));
        return std::nullopt;
    }

    // Extract payload
    const uint8_t* payload = &data[5];

    // Find null terminator
    const uint8_t* nullPos = static_cast<const uint8_t*>(std::memchr(payload, 0, payloadLength));
    if (!nullPos) {
        ENTROPY_LOG_ERROR("Signaling message payload missing null terminator");
        return std::nullopt;
    }

    size_t firstStringLen = nullPos - payload;
    size_t secondStringStart = firstStringLen + 1;

    if (secondStringStart > payloadLength) {
        ENTROPY_LOG_ERROR("Signaling message payload truncated");
        return std::nullopt;
    }

    std::string str1(reinterpret_cast<const char*>(payload), firstStringLen);
    std::string str2(reinterpret_cast<const char*>(payload + secondStringStart), payloadLength - secondStringStart);

    return std::make_tuple(msgType, str1, str2);
}

static const char* connStateToString(ConnectionState s) {
    switch (s) {
        case ConnectionState::Disconnected:
            return "Disconnected";
        case ConnectionState::Disconnecting:
            return "Disconnecting";
        case ConnectionState::Connecting:
            return "Connecting";
        case ConnectionState::Connected:
            return "Connected";
        case ConnectionState::Failed:
            return "Failed";
    }
    return "?";
}

WebRTCConnection::WebRTCConnection(WebRTCConfig config, SignalingCallbacks signalingCallbacks, std::string signalingUrl,
                                   std::string dataChannelLabel)
    : _callbackContext(new CallbackContext{this, true})  // Intentionally leaked
      ,
      _config(std::move(config)),
      _signalingCallbacks(std::move(signalingCallbacks)),
      _signalingUrl(std::move(signalingUrl)),
      _dataChannelLabel(std::move(dataChannelLabel)),
      _unreliableChannelLabel(_dataChannelLabel + "-unreliable") {
    _polite = _config.polite;
}

WebRTCConnection::~WebRTCConnection() {
    // CRITICAL ORDER (understanding libdatachannel mechanics):
    // 1. Invalidate context (so callbacks that are ALREADY executing will early-return)
    // 2. Set user pointers to nullptr (prevents NEW callbacks from getting our pointer)
    // 3. Delete resources (closes connections, stops new callbacks from being queued)
    // 4. Wait for active callbacks to complete (callbacks that got pointer before step 2)
    // 5. Delete context (safe - no more references)

    // Step 1: Invalidate callback context
    _callbackContext->valid.store(false, std::memory_order_release);

    // Clear user callbacks
    setMessageCallback(nullptr);
    setStateCallback(nullptr);

    disconnect();

    // Set shutdown flag (for NetworkConnection base class tracking)
    shutdownCallbacks();

    // Step 2: Clear user pointers in libdatachannel
    // This prevents NEW callbacks from getting our _callbackContext pointer
    // because callbacks check: if (auto ptr = getUserPointer(pc)) before using it
    if (_peerConnectionId >= 0) {
        rtcSetUserPointer(_peerConnectionId, nullptr);
    }
    if (_dataChannelId >= 0) {
        rtcSetUserPointer(_dataChannelId, nullptr);
    }
    if (_unreliableDataChannelId >= 0) {
        rtcSetUserPointer(_unreliableDataChannelId, nullptr);
    }

    // Step 3: Delete resources (closes connections, cleans up libdatachannel state)
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

    // Brief sleep to allow in-flight callbacks to drain
    // libdatachannel doesn't guarantee instant callback termination after rtcDelete*
    // Callbacks can still be executing on background threads
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step 4: Wait for all active callbacks to complete
    // These are callbacks that got the pointer BEFORE we cleared it in step 2
    // They're using CallbackGuard which tracks activeCallbacks
    int waitCount = 0;
    while (_callbackContext->activeCallbacks.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (++waitCount % 10000 == 0) {  // Log every second
            ENTROPY_LOG_WARNING("Waiting for " +
                                std::to_string(_callbackContext->activeCallbacks.load(std::memory_order_acquire)) +
                                " active callbacks to complete");
        }
    }

    // Step 5: Delete context - safe now, no more references
    delete _callbackContext;
    _callbackContext = nullptr;
}

// C API callback adapters
void WebRTCConnection::onLocalDescriptionCallback(int, const char* sdp, const char* type, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

    ENTROPY_LOG_DEBUG(std::string("onLocalDescription: type=") + (type ? type : "(null)") +
                      ", polite=" + (self->_polite ? "true" : "false") +
                      ", sdpLen=" + std::to_string(sdp ? (int)std::strlen(sdp) : 0));

    // Note: We do NOT clear _makingOffer here - it's cleared when we receive
    // a remote answer or when we accept a remote offer (see setRemoteDescription)

    if (self->_signalingCallbacks.onLocalDescription) {
        self->_signalingCallbacks.onLocalDescription(type, sdp);
    }
}

void WebRTCConnection::onLocalCandidateCallback(int, const char* cand, const char* mid, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

    ENTROPY_LOG_DEBUG(std::string("onLocalCandidate: mid=") + (mid ? mid : "(null)") +
                      ", len=" + std::to_string(cand ? (int)std::strlen(cand) : 0));
    if (self->_signalingCallbacks.onLocalCandidate) {
        self->_signalingCallbacks.onLocalCandidate(cand, mid);
    }
}

void WebRTCConnection::onStateChangeCallback(int, rtcState state, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

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
            ENTROPY_LOG_INFO(std::string("PeerConnection state change: ") + rtcStateToString(state) + " => " +
                             connStateToString(newState));
            self->_state = newState;
            pending = newState;
        } else {
            ENTROPY_LOG_DEBUG(std::string("PeerConnection state change (no mapped transition): ") +
                              rtcStateToString(state));
        }
    }

    if (pending && guard.isValid()) {
        self->onStateChanged(*pending);
    }
}

void WebRTCConnection::onSignalingStateChangeCallback(int, rtcSignalingState state, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

    self->_signalingState.store(state, std::memory_order_release);

    // Defensive cleanup to avoid stale negotiation flags between episodes
    if (state == RTC_SIGNALING_STABLE) {
        self->_ignoreOffer.store(false, std::memory_order_release);
        self->_isSettingRemoteAnswerPending.store(false, std::memory_order_release);
        // Note: We intentionally do not force-clear _makingOffer here; it's managed by offer/answer paths
    }

    const char* stateStr = "unknown";
    switch (state) {
        case RTC_SIGNALING_STABLE:
            stateStr = "stable";
            break;
        case RTC_SIGNALING_HAVE_LOCAL_OFFER:
            stateStr = "have-local-offer";
            break;
        case RTC_SIGNALING_HAVE_REMOTE_OFFER:
            stateStr = "have-remote-offer";
            break;
        case RTC_SIGNALING_HAVE_LOCAL_PRANSWER:
            stateStr = "have-local-pranswer";
            break;
        case RTC_SIGNALING_HAVE_REMOTE_PRANSWER:
            stateStr = "have-remote-pranswer";
            break;
    }
    ENTROPY_LOG_DEBUG(std::string("Signaling state changed to: ") + stateStr);
}

void WebRTCConnection::onDataChannelCallback(int, int dc, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

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
            ENTROPY_LOG_DEBUG(
                std::format("Switching _dataChannelId from {} to REMOTE channel {}", self->_dataChannelId, dc));
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
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

    char label[256];
    if (rtcGetDataChannelLabel(id, label, sizeof(label)) < 0) {
        return;
    }

    ENTROPY_LOG_INFO(std::string("Data channel opened: ") + label + ", id=" + std::to_string(id));

    std::string_view labelView(label);
    std::optional<ConnectionState> pending;

    {
        std::lock_guard<std::mutex> lock(self->_mutex);

        // Update cached channel open state for both reliable and unreliable channels
        if (id == self->_dataChannelId) {
            self->_dataChannelOpen.store(true, std::memory_order_release);
        } else if (id == self->_unreliableDataChannelId) {
            self->_unreliableDataChannelOpen.store(true, std::memory_order_release);
        }

        // Only transition to Connected for reliable channel
        if (labelView != self->_dataChannelLabel) return;

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
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            self->_stats.connectTime.store(timestamp, std::memory_order_relaxed);
            self->_stats.lastActivityTime.store(timestamp, std::memory_order_relaxed);
            pending = ConnectionState::Connected;
        }
    }

    if (pending && guard.isValid()) {
        self->onStateChanged(*pending);
    }
}

void WebRTCConnection::onClosedCallback(int id, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

    char label[256];
    if (rtcGetDataChannelLabel(id, label, sizeof(label)) < 0) {
        return;
    }

    ENTROPY_LOG_INFO(std::string("Data channel closed: ") + label + ", id=" + std::to_string(id));

    std::string_view labelView(label);
    std::optional<ConnectionState> pending;

    {
        std::lock_guard<std::mutex> lock(self->_mutex);

        // Update cached channel open state for both reliable and unreliable channels
        if (id == self->_dataChannelId) {
            self->_dataChannelOpen.store(false, std::memory_order_release);
        } else if (id == self->_unreliableDataChannelId) {
            self->_unreliableDataChannelOpen.store(false, std::memory_order_release);
        }

        // Only transition to Disconnected for reliable channel
        if (labelView != self->_dataChannelLabel) return;

        if (self->_state != ConnectionState::Disconnected && self->_state != ConnectionState::Failed) {
            self->_state = ConnectionState::Disconnected;
            pending = ConnectionState::Disconnected;
        }
    }

    if (pending && guard.isValid()) {
        self->onStateChanged(*pending);
    }
}

void WebRTCConnection::onMessageCallback(int id, const char* message, int size, void* user) {
    CallbackGuard guard(static_cast<CallbackContext*>(user));
    if (!guard.isValid()) return;
    auto* self = guard.getConnection();

    // Hot path: removed per-message DEBUG logging (was killing throughput at scale)
    // Defensive: drop empty/erroneous frames to avoid huge allocations on negative sizes
    if (size <= 0) return;

    // Performance note: This creates a vector (heap allocation) per message.
    // For high message rates (>10K msg/s), consider:
    // - Buffer pool with reusable vectors
    // - Callback API accepting std::span<const uint8_t> (breaking change)
    // Current approach prioritizes API simplicity over allocation efficiency.
    std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(message),
                              reinterpret_cast<const uint8_t*>(message) + size);

    // Update stats atomically without lock
    self->_stats.bytesReceived.fetch_add(size, std::memory_order_relaxed);
    self->_stats.messagesReceived.fetch_add(1, std::memory_order_relaxed);
    self->_stats.lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    if (!self->isShuttingDown()) {
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
        // Set up internal WebSocket if in client mode
        if (!_signalingUrl.empty() && !_signalingCallbacks.onLocalDescription) {
            setupInternalWebSocket();
        }

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
            return Result<void>::err(NetworkError::InvalidParameter, "Cannot reconnect - connection destroyed");
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
        return Result<void>::err(NetworkError::ConnectionClosed, "Failed to trigger ICE restart");
    }

    // Use cached channel state (lock-free, no deadlock, no race condition)
    bool channelStillOpen = wasEstablished && _dataChannelOpen.load(std::memory_order_acquire);

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
    if (pending && !isShuttingDown()) {
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

    if (pending && !isShuttingDown()) {
        onStateChanged(*pending);
    }

    _makingOffer.store(false, std::memory_order_release);

    return Result<void>::ok();
}

bool WebRTCConnection::isConnected() const {
    return _state == ConnectionState::Connected;
}

Result<void> WebRTCConnection::send(const std::vector<uint8_t>& data) {
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Connection not established");
    }

    // Snapshot both id and open state atomically under same lock to avoid races during channel swaps
    int id;
    bool open;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        id = _dataChannelId;
        open = _dataChannelOpen.load(std::memory_order_relaxed);  // relaxed OK under mutex
    }

    if (!open || id < 0) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Data channel not open");
    }

    // Hot path: removed per-message DEBUG logging (was killing throughput at scale)
    int result = rtcSendMessage(id, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    if (result < 0) {
        ENTROPY_LOG_ERROR(std::format("rtcSendMessage failed with code {}", result));
        return Result<void>::err(NetworkError::InvalidMessage, "Failed to send data");
    }

    // Update stats atomically without lock
    _stats.bytesSent.fetch_add(data.size(), std::memory_order_relaxed);
    _stats.messagesSent.fetch_add(1, std::memory_order_relaxed);
    _stats.lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    return Result<void>::ok();
}

Result<void> WebRTCConnection::trySend(const std::vector<uint8_t>& data) {
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Connection not established");
    }

    // Snapshot both id and open state atomically under same lock to avoid races during channel swaps
    int id;
    bool open;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        id = _dataChannelId;
        open = _dataChannelOpen.load(std::memory_order_relaxed);  // relaxed OK under mutex
    }

    if (!open || id < 0) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Data channel not open");
    }

    int buffered = rtcGetBufferedAmount(id);
    if (buffered > 0) {
        return Result<void>::err(NetworkError::WouldBlock, "WebRTC data channel backpressured");
    }

    int result = rtcSendMessage(id, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    if (result < 0) {
        return Result<void>::err(NetworkError::InvalidMessage, "Failed to trySend data");
    }

    // Update stats atomically without lock
    _stats.bytesSent.fetch_add(data.size(), std::memory_order_relaxed);
    _stats.messagesSent.fetch_add(1, std::memory_order_relaxed);
    _stats.lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    return Result<void>::ok();
}

Result<void> WebRTCConnection::sendUnreliable(const std::vector<uint8_t>& data) {
    if (_state != ConnectionState::Connected) {
        return Result<void>::err(NetworkError::ConnectionClosed, "Connection not established");
    }

    // Snapshot both id and open state atomically under same lock to avoid races during channel swaps
    int id;
    bool open;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        id = _unreliableDataChannelId;
        open = _unreliableDataChannelOpen.load(std::memory_order_relaxed);  // relaxed OK under mutex
    }

    if (!open || id < 0) {
        // Fallback to reliable channel
        return send(data);
    }

    int result = rtcSendMessage(id, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    if (result < 0) {
        return Result<void>::err(NetworkError::InvalidMessage, "Failed to send unreliable data");
    }

    // Update stats atomically without lock
    _stats.bytesSent.fetch_add(data.size(), std::memory_order_relaxed);
    _stats.messagesSent.fetch_add(1, std::memory_order_relaxed);
    _stats.lastActivityTime.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    return Result<void>::ok();
}

ConnectionState WebRTCConnection::getState() const {
    return _state;
}

ConnectionStats WebRTCConnection::getStats() const {
    // No lock needed - ConnectionStats copy constructor handles atomic loads
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

        ENTROPY_LOG_DEBUG(
            std::string("setRemoteDescription: type=") + type + ", sdpLen=" + std::to_string((int)sdp.size()) +
            ", offerCollision=" + (offerCollision ? "true" : "false") + ", polite=" + (_polite ? "true" : "false"));

        // Remember pc id for out-of-lock RTC calls
        pcId = _peerConnectionId;
    }

    // Set remote description
    // Note: When a polite peer accepts a remote offer during an ongoing local negotiation,
    // libdatachannel's C API performs an implicit rollback of the local offer. We rely on
    // this behavior here (tested against libdatachannel version bundled via vcpkg). If you
    // observe anomalies, add diagnostics around signaling transitions to verify stability.
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
            // End of this glare/offer episode: ensure ignoreOffer is cleared
            _ignoreOffer.store(false, std::memory_order_release);
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
            ENTROPY_LOG_WARNING(std::string("Failed to add queued remote candidate (mid=") + mid +
                                ", len=" + std::to_string(candidate.size()) + ")");
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

    ENTROPY_LOG_DEBUG(std::string("addRemoteCandidate: mid=") + mid + ", len=" + std::to_string((int)candidate.size()));

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
                     ", bindAddress=" + (_config.bindAddress.empty() ? "" : _config.bindAddress) + ")");
    _peerConnectionId = rtcCreatePeerConnection(&config);
    if (_peerConnectionId < 0) {
        throw std::runtime_error("Failed to create peer connection");
    }

    rtcSetUserPointer(_peerConnectionId, _callbackContext);
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

    ENTROPY_LOG_DEBUG(std::format("Created LOCAL reliable channel with id={}", _dataChannelId));
    setupDataChannelCallbacks(_dataChannelId, true);
}

void WebRTCConnection::setupUnreliableDataChannel() {
    rtcDataChannelInit init;
    std::memset(&init, 0, sizeof(init));
    init.reliability.unordered = true;
    init.reliability.maxRetransmits = 0;

    ENTROPY_LOG_INFO("Creating unreliable data channel: label=" + _unreliableChannelLabel);
    _unreliableDataChannelId = rtcCreateDataChannelEx(_peerConnectionId, _unreliableChannelLabel.c_str(), &init);
    if (_unreliableDataChannelId < 0) {
        throw std::runtime_error("Failed to create unreliable data channel");
    }

    setupDataChannelCallbacks(_unreliableDataChannelId, false);
}

void WebRTCConnection::setupDataChannelCallbacks(int channelId, bool isReliable) {
    rtcSetUserPointer(channelId, _callbackContext);
    rtcSetOpenCallback(channelId, onOpenCallback);
    rtcSetClosedCallback(channelId, onClosedCallback);
    rtcSetMessageCallback(channelId, onMessageCallback);
}

void WebRTCConnection::setupInternalWebSocket() {
    // Client mode: Create and manage WebSocket for signaling
    _webSocket = std::make_shared<rtc::WebSocket>();

    // Set up signaling callbacks to bridge between WebSocket and WebRTCConnection
    // Uses binary envelope framing for robustness
    _signalingCallbacks.onLocalDescription = [webSocket = _webSocket](const std::string& type, const std::string& sdp) {
        if (webSocket && webSocket->isOpen()) {
            auto encoded = encodeSDPMessage(type, sdp);
            webSocket->send(reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
        }
    };

    _signalingCallbacks.onLocalCandidate = [webSocket = _webSocket](const std::string& candidate,
                                                                    const std::string& mid) {
        if (webSocket && webSocket->isOpen()) {
            auto encoded = encodeICEMessage(candidate, mid);
            webSocket->send(reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
        }
    };

    // Set up WebSocket message handler for incoming signaling
    // Expects binary envelope format
    _webSocket->onMessage([this](auto data) {
        if (std::holds_alternative<rtc::binary>(data)) {
            const auto& binaryData = std::get<rtc::binary>(data);
            // Convert std::byte to uint8_t
            std::vector<uint8_t> msg;
            msg.reserve(binaryData.size());
            for (const auto& byte : binaryData) {
                msg.push_back(static_cast<uint8_t>(byte));
            }

            auto decoded = decodeSignalingMessage(msg);
            if (!decoded) {
                ENTROPY_LOG_ERROR("Failed to decode signaling message");
                return;
            }

            auto [msgType, str1, str2] = *decoded;
            if (msgType == SignalingMessageType::SDP) {
                // str1=type, str2=sdp
                setRemoteDescription(str1, str2);
            } else if (msgType == SignalingMessageType::ICE) {
                // str1=candidate, str2=mid
                addRemoteCandidate(str1, str2);
            }
        }
    });

    _webSocket->onError([](std::string error) { ENTROPY_LOG_ERROR(std::format("WebRTC signaling error: {}", error)); });

    // Open WebSocket connection
    _webSocket->open(_signalingUrl);

    ENTROPY_LOG_INFO(std::format("WebRTC client connecting to signaling server: {}", _signalingUrl));
}

}  // namespace EntropyEngine::Networking
