// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "NetworkConnection.h"
#include <rtc/rtc.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

namespace EntropyEngine::Networking {

    /**
     * @brief Configuration for WebRTC connection
     */
    struct WebRTCConfig {
        std::vector<std::string> iceServers;
        std::string proxyServer;
        std::string bindAddress;
        uint16_t portRangeBegin = 0;
        uint16_t portRangeEnd = 0;
        int maxMessageSize = 256 * 1024; // 256 KB default
        bool enableIceTcp = false;
    };

    /**
     * @brief Signaling callbacks for WebRTC connection
     *
     * The application must provide these callbacks to handle signaling.
     * Typically, these would send data over WebSocket or another signaling channel.
     */
    struct SignalingCallbacks {
        using LocalDescriptionCallback = std::function<void(const std::string& type, const std::string& sdp)>;
        using LocalCandidateCallback = std::function<void(const std::string& candidate, const std::string& mid)>;

        LocalDescriptionCallback onLocalDescription;
        LocalCandidateCallback onLocalCandidate;
    };

    /**
     * @brief WebRTC-based network connection using data channels
     *
     * Implements NetworkConnection interface using libdatachannel for WebRTC data channels.
     * Supports reliable and unreliable data transfer over WebRTC.
     *
     * Signaling must be handled externally via the SignalingCallbacks.
     */
    class WebRTCConnection : public NetworkConnection {
    public:
        /**
         * @brief Construct a new WebRTC connection
         * @param config WebRTC configuration (ICE servers, etc.)
         * @param signalingCallbacks Callbacks for sending signaling messages
         * @param dataChannelLabel Label for the data channel (default: "entropy-data")
         */
        WebRTCConnection(
            WebRTCConfig config,
            SignalingCallbacks signalingCallbacks,
            std::string dataChannelLabel = "entropy-data"
        );

        ~WebRTCConnection() override;

        // NetworkConnection interface
        Result<void> connect() override;
        Result<void> disconnect() override;
        bool isConnected() const override;

        Result<void> send(const std::vector<uint8_t>& data) override;
        Result<void> sendUnreliable(const std::vector<uint8_t>& data) override;

        ConnectionState getState() const override;
        ConnectionType getType() const override { return ConnectionType::WebRTC; }
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
        void setupPeerConnection();
        void setupDataChannel();
        void setupUnreliableDataChannel();
        void setupDataChannelCallbacks(std::shared_ptr<rtc::DataChannel> channel);
        void updateConnectionState(rtc::PeerConnection::State state);

        WebRTCConfig _config;
        SignalingCallbacks _signalingCallbacks;
        std::string _dataChannelLabel;
        std::string _unreliableChannelLabel;

        std::shared_ptr<rtc::PeerConnection> _peerConnection;
        std::shared_ptr<rtc::DataChannel> _dataChannel;
        std::shared_ptr<rtc::DataChannel> _unreliableDataChannel;

        std::atomic<ConnectionState> _state{ConnectionState::Disconnected};
        mutable std::mutex _mutex;
        ConnectionStats _stats;
    };

} // namespace EntropyEngine::Networking
