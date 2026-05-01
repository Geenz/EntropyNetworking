/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "ConnectionHandle.h"

#include <format>

#include "ConnectionManager.h"

namespace EntropyEngine::Networking
{

ConnectionManager* ConnectionHandle::manager() const {
    return static_cast<ConnectionManager*>(const_cast<void*>(handleOwner()));
}

Result<void> ConnectionHandle::connect() {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid handle");
    }
    return mgr->connect(*this);
}

Result<void> ConnectionHandle::disconnect() {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid handle");
    }
    return mgr->disconnect(*this);
}

Result<void> ConnectionHandle::close() {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid handle");
    }
    return mgr->closeConnection(*this);
}

Result<void> ConnectionHandle::send(const std::vector<uint8_t>& data) {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid handle");
    }
    return mgr->send(*this, data);
}

Result<void> ConnectionHandle::trySend(const std::vector<uint8_t>& data) {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid handle");
    }
    return mgr->trySend(*this, data);
}

Result<void> ConnectionHandle::sendUnreliable(const std::vector<uint8_t>& data) {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid handle");
    }
    return mgr->sendUnreliable(*this, data);
}

bool ConnectionHandle::isConnected() const {
    auto* mgr = manager();
    if (!mgr) return false;
    return mgr->isConnected(*this);
}

ConnectionState ConnectionHandle::getState() const {
    auto* mgr = manager();
    if (!mgr) return ConnectionState::Disconnected;
    return mgr->getState(*this);
}

ConnectionStats ConnectionHandle::getStats() const {
    auto* mgr = manager();
    if (!mgr) return ConnectionStats{};
    return mgr->getStats(*this);
}

ConnectionType ConnectionHandle::getType() const {
    auto* mgr = manager();
    if (!mgr) return ConnectionType::Local;  // Default fallback
    return mgr->getConnectionType(*this);
}

bool ConnectionHandle::valid() const {
    auto* mgr = manager();
    if (!mgr) return false;
    return mgr->isValidHandle(*this);
}

void ConnectionHandle::setMessageCallback(std::function<void(const std::vector<uint8_t>&)> callback) noexcept {
    auto* mgr = manager();
    if (mgr) {
        mgr->setMessageCallback(*this, std::move(callback));
    }
}

void ConnectionHandle::setStateCallback(std::function<void(ConnectionState)> callback) noexcept {
    auto* mgr = manager();
    if (mgr) {
        mgr->setStateCallback(*this, std::move(callback));
    }
}

uint64_t ConnectionHandle::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(Core::TypeSystem::createTypeId<ConnectionHandle>().id);
    return hash;
}

std::string ConnectionHandle::toString() const {
    if (!hasHandle()) {
        return std::format("{}@{}(invalid)", className(), static_cast<const void*>(this));
    }
    return std::format("{}@{}(owner={}, idx={}, gen={})", className(), static_cast<const void*>(this), handleOwner(),
                       handleIndex(), handleGeneration());
}

}  // namespace EntropyEngine::Networking
