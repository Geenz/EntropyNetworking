/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "RemoteServer.h"

#include "WebRTCServer.h"

namespace EntropyEngine
{
namespace Networking
{

std::unique_ptr<RemoteServer> createRemoteServer(ConnectionManager* connMgr, uint16_t port) {
    RemoteServerConfig config;
    config.port = port;
    return createRemoteServer(connMgr, config);
}

std::unique_ptr<RemoteServer> createRemoteServer(ConnectionManager* connMgr, const RemoteServerConfig& config) {
    // Currently only WebRTC is supported
    // Future: Check config for protocol selection (WebRTC, QUIC, etc.)
    return std::make_unique<WebRTCServer>(connMgr, config);
}

}  // namespace Networking
}  // namespace EntropyEngine
