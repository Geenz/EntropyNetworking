// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "WebRTCConnection.h"
#include <stdexcept>
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
    }

    WebRTCConnection::~WebRTCConnection() {
        disconnect();
    }

    Result<void> WebRTCConnection::connect() {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != ConnectionState::Disconnected) {
            return Result<void>::err(
                NetworkError::InvalidParameter,
                "Connection already active"
            );
        }

        try {
            _state = ConnectionState::Connecting;
            setupPeerConnection();
            setupDataChannel();
            setupUnreliableDataChannel();
            return Result<void>::ok();
        } catch (const std::exception& e) {
            _state = ConnectionState::Disconnected;
            return Result<void>::err(
                NetworkError::ConnectionClosed,
                std::string("Failed to create peer connection: ") + e.what()
            );
        }
    }

    Result<void> WebRTCConnection::disconnect() {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state == ConnectionState::Disconnected) {
            return Result<void>::ok();
        }

        _state = ConnectionState::Disconnected;

        if (_dataChannel) {
            _dataChannel->close();
            _dataChannel.reset();
        }

        if (_unreliableDataChannel) {
            _unreliableDataChannel->close();
            _unreliableDataChannel.reset();
        }

        if (_peerConnection) {
            _peerConnection->close();
            _peerConnection.reset();
        }

        onStateChanged(ConnectionState::Disconnected);

        return Result<void>::ok();
    }

    bool WebRTCConnection::isConnected() const {
        return _state == ConnectionState::Connected;
    }

    Result<void> WebRTCConnection::send(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != ConnectionState::Connected) {
            return Result<void>::err(
                NetworkError::ConnectionClosed,
                "Connection not established"
            );
        }

        if (!_dataChannel || !_dataChannel->isOpen()) {
            return Result<void>::err(
                NetworkError::ConnectionClosed,
                "Data channel not open"
            );
        }

        try {
            _dataChannel->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
            _stats.bytesSent += data.size();
            _stats.messagesSent++;
            _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return Result<void>::ok();
        } catch (const std::exception& e) {
            return Result<void>::err(
                NetworkError::InvalidMessage,
                std::string("Failed to send data: ") + e.what()
            );
        }
    }

    Result<void> WebRTCConnection::trySend(const std::vector<uint8_t>& data) {
            std::lock_guard<std::mutex> lock(_mutex);

            if (_state != ConnectionState::Connected) {
                return Result<void>::err(
                    NetworkError::ConnectionClosed,
                    "Connection not established"
                );
            }

            if (!_dataChannel || !_dataChannel->isOpen()) {
                return Result<void>::err(
                    NetworkError::ConnectionClosed,
                    "Data channel not open"
                );
            }

            // Backpressure: if there's anything buffered, report WouldBlock
            // (could be refined with a threshold in configuration in future PRs)
            if (_dataChannel->bufferedAmount() > 0) {
                return Result<void>::err(
                    NetworkError::WouldBlock,
                    "WebRTC data channel backpressured"
                );
            }

            try {
                _dataChannel->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
                _stats.bytesSent += data.size();
                _stats.messagesSent++;
                _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                return Result<void>::ok();
            } catch (const std::exception& e) {
                return Result<void>::err(
                    NetworkError::InvalidMessage,
                    std::string("Failed to trySend data: ") + e.what()
                );
            }
        }

        Result<void> WebRTCConnection::sendUnreliable(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_state != ConnectionState::Connected) {
            return Result<void>::err(
                NetworkError::ConnectionClosed,
                "Connection not established"
            );
        }

        if (!_unreliableDataChannel || !_unreliableDataChannel->isOpen()) {
            // Fall back to reliable channel if unreliable is not available
            return send(data);
        }

        try {
            _unreliableDataChannel->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
            _stats.bytesSent += data.size();
            _stats.messagesSent++;
            _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return Result<void>::ok();
        } catch (const std::exception& e) {
            return Result<void>::err(
                NetworkError::InvalidMessage,
                std::string("Failed to send unreliable data: ") + e.what()
            );
        }
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

        if (!_peerConnection) {
            return Result<void>::err(
                NetworkError::InvalidParameter,
                "Peer connection not initialized"
            );
        }

        try {
            rtc::Description description(sdp, type);
            _peerConnection->setRemoteDescription(description);
            return Result<void>::ok();
        } catch (const std::exception& e) {
            return Result<void>::err(
                NetworkError::InvalidMessage,
                std::string("Failed to set remote description: ") + e.what()
            );
        }
    }

    Result<void> WebRTCConnection::addRemoteCandidate(const std::string& candidate, const std::string& mid) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (!_peerConnection) {
            return Result<void>::err(
                NetworkError::InvalidParameter,
                "Peer connection not initialized"
            );
        }

        try {
            rtc::Candidate rtcCandidate(candidate, mid);
            _peerConnection->addRemoteCandidate(rtcCandidate);
            return Result<void>::ok();
        } catch (const std::exception& e) {
            return Result<void>::err(
                NetworkError::InvalidMessage,
                std::string("Failed to add remote candidate: ") + e.what()
            );
        }
    }

    bool WebRTCConnection::isReady() const {
        return _peerConnection != nullptr;
    }

    void WebRTCConnection::setupPeerConnection() {
        rtc::Configuration rtcConfig;

        // Add ICE servers
        for (const auto& server : _config.iceServers) {
            rtcConfig.iceServers.emplace_back(server);
        }

        // Set optional parameters
        if (!_config.proxyServer.empty()) {
            rtcConfig.proxyServer = _config.proxyServer;
        }

        if (!_config.bindAddress.empty()) {
            rtcConfig.bindAddress = _config.bindAddress;
        }

        if (_config.portRangeBegin > 0 && _config.portRangeEnd > 0) {
            rtcConfig.portRangeBegin = _config.portRangeBegin;
            rtcConfig.portRangeEnd = _config.portRangeEnd;
        }

        rtcConfig.enableIceTcp = _config.enableIceTcp;
        rtcConfig.maxMessageSize = _config.maxMessageSize;

        _peerConnection = std::make_shared<rtc::PeerConnection>(rtcConfig);

        // Set up local description callback
        _peerConnection->onLocalDescription([this](rtc::Description description) {
            if (_signalingCallbacks.onLocalDescription) {
                _signalingCallbacks.onLocalDescription(
                    description.typeString(),
                    std::string(description)
                );
            }
        });

        // Set up local candidate callback
        _peerConnection->onLocalCandidate([this](rtc::Candidate candidate) {
            if (_signalingCallbacks.onLocalCandidate) {
                _signalingCallbacks.onLocalCandidate(
                    candidate.candidate(),
                    candidate.mid()
                );
            }
        });

        // Set up state change callback
        _peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
            updateConnectionState(state);
        });

        // Set up gathering state change callback
        _peerConnection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            // Optional: could expose this to the application
        });

        // Set up data channel handler for receiving channels from remote peer
        _peerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            ENTROPY_LOG_DEBUG(std::string("onDataChannel called: ") + dc->label());
            std::lock_guard<std::mutex> lock(_mutex);

            // Check if this is the reliable or unreliable channel
            if (dc->label() == _dataChannelLabel) {
                ENTROPY_LOG_DEBUG("Setting as reliable channel");
                _dataChannel = dc;
                setupDataChannelCallbacks(_dataChannel);
            } else if (dc->label() == _unreliableChannelLabel) {
                ENTROPY_LOG_DEBUG("Setting as unreliable channel");
                _unreliableDataChannel = dc;
                setupDataChannelCallbacks(_unreliableDataChannel);
            }
        });
    }

    void WebRTCConnection::setupDataChannel() {
        rtc::DataChannelInit init;
        init.reliability.unordered = false;

        _dataChannel = _peerConnection->createDataChannel(_dataChannelLabel, init);
        setupDataChannelCallbacks(_dataChannel);
    }

    void WebRTCConnection::setupUnreliableDataChannel() {
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0; // No retransmissions

        _unreliableDataChannel = _peerConnection->createDataChannel(_unreliableChannelLabel, init);
        setupDataChannelCallbacks(_unreliableDataChannel);
    }

    void WebRTCConnection::setupDataChannelCallbacks(std::shared_ptr<rtc::DataChannel> channel) {
        if (!channel) return;

        // Determine if this is the reliable channel
        bool isReliableChannel = (channel->label() == _dataChannelLabel);

        channel->onOpen([this, isReliableChannel, channel]() {
            // Debug output
            ENTROPY_LOG_INFO(std::string("Data channel opened: ") + channel->label());

            if (isReliableChannel) {
                std::lock_guard<std::mutex> lock(_mutex);
                // Only transition to Connected if we're not already there
                // This is the definitive source for Connected state
                if (_state != ConnectionState::Connected) {
                    _state = ConnectionState::Connected;

                    // Record connection time
                    auto now = std::chrono::system_clock::now();
                    _stats.connectTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()
                    ).count();
                    _stats.lastActivityTime = _stats.connectTime;

                    onStateChanged(ConnectionState::Connected);
                }
            }
        });

        channel->onClosed([this, isReliableChannel]() {
            if (isReliableChannel) {
                std::lock_guard<std::mutex> lock(_mutex);
                // Only transition to Disconnected if we're not already there
                if (_state != ConnectionState::Disconnected && _state != ConnectionState::Failed) {
                    _state = ConnectionState::Disconnected;
                    onStateChanged(ConnectionState::Disconnected);
                }
            }
        });

        channel->onMessage([this](auto data) {
            std::vector<uint8_t> message;

            if (std::holds_alternative<std::string>(data)) {
                const auto& str = std::get<std::string>(data);
                message.assign(str.begin(), str.end());
            } else {
                const auto& binary = std::get<rtc::binary>(data);
                message.resize(binary.size());
                for (size_t i = 0; i < binary.size(); ++i) {
                    message[i] = static_cast<uint8_t>(binary[i]);
                }
            }

            {
                std::lock_guard<std::mutex> lock(_mutex);
                _stats.bytesReceived += message.size();
                _stats.messagesReceived++;
                _stats.lastActivityTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }

            onMessageReceived(message);
        });

        channel->onError([](std::string error) {
            // Optional: could expose error callback to application
        });
    }

    void WebRTCConnection::updateConnectionState(rtc::PeerConnection::State state) {
        std::lock_guard<std::mutex> lock(_mutex);

        ConnectionState newState = _state;  // Track if state actually changes

        switch (state) {
            case rtc::PeerConnection::State::New:
            case rtc::PeerConnection::State::Connecting:
                // Only update to Connecting if we haven't reached Connected yet
                if (_state != ConnectionState::Connected) {
                    newState = ConnectionState::Connecting;
                }
                break;

            case rtc::PeerConnection::State::Connected:
                // Don't set Connected here - wait for data channel to open
                // Data channel opening is the definitive signal that we're ready
                break;

            case rtc::PeerConnection::State::Disconnected:
                // Peer connection disconnected - mark as such
                newState = ConnectionState::Disconnected;
                break;

            case rtc::PeerConnection::State::Failed:
                // Connection failed - mark as failed
                newState = ConnectionState::Failed;
                break;

            case rtc::PeerConnection::State::Closed:
                // Connection closed - mark as disconnected
                newState = ConnectionState::Disconnected;
                break;
        }

        // Only fire state change event if state actually changed
        if (newState != _state) {
            _state = newState;
            onStateChanged(_state);
        }
    }

} // namespace EntropyEngine::Networking
