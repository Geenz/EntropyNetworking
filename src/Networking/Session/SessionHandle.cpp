/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "SessionHandle.h"
#include "SessionManager.h"
#include <format>

namespace EntropyEngine::Networking {

SessionManager* SessionHandle::manager() const {
    return static_cast<SessionManager*>(const_cast<void*>(handleOwner()));
}

Result<void> SessionHandle::sendEntityCreated(
    uint64_t entityId,
    const std::string& appId,
    const std::string& typeName,
    uint64_t parentId
) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendEntityCreated(*this, entityId, appId, typeName, parentId);
}

Result<void> SessionHandle::sendEntityDestroyed(uint64_t entityId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendEntityDestroyed(*this, entityId);
}

Result<void> SessionHandle::sendPropertyUpdate(
    PropertyHash hash,
    PropertyType type,
    const PropertyValue& value
) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendPropertyUpdate(*this, hash, type, value);
}

Result<void> SessionHandle::sendPropertyUpdateBatch(const std::vector<uint8_t>& batchData) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendPropertyUpdateBatch(*this, batchData);
}

Result<void> SessionHandle::sendSceneSnapshot(const std::vector<uint8_t>& snapshotData) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendSceneSnapshot(*this, snapshotData);
}

bool SessionHandle::isConnected() const {
    auto* mgr = manager();
    if (!mgr) return false;
    return mgr->isConnected(*this);
}

ConnectionState SessionHandle::getConnectionState() const {
    auto* mgr = manager();
    if (!mgr) return ConnectionState::Disconnected;
    return mgr->getConnectionState(*this);
}

ConnectionStats SessionHandle::getConnectionStats() const {
    auto* mgr = manager();
    if (!mgr) return ConnectionStats{};
    return mgr->getConnectionStats(*this);
}

ConnectionHandle SessionHandle::getConnection() const {
    auto* mgr = manager();
    if (!mgr) return ConnectionHandle();
    return mgr->getConnection(*this);
}

PropertyRegistry& SessionHandle::getPropertyRegistry() {
    auto* mgr = manager();
    if (!mgr) {
        // This is dangerous - we shouldn't return a reference to nothing
        // But the API requires it. In practice, valid() should be checked first.
        static PropertyRegistry dummy;
        return dummy;
    }
    return mgr->getPropertyRegistry(*this);
}

const PropertyRegistry& SessionHandle::getPropertyRegistry() const {
    auto* mgr = manager();
    if (!mgr) {
        static PropertyRegistry dummy;
        return dummy;
    }
    return mgr->getPropertyRegistry(*this);
}

bool SessionHandle::valid() const {
    auto* mgr = manager();
    if (!mgr) return false;
    return mgr->isValidHandle(*this);
}

uint64_t SessionHandle::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(
        Core::TypeSystem::createTypeId<SessionHandle>().id
    );
    return hash;
}

std::string SessionHandle::toString() const {
    if (!hasHandle()) {
        return std::format("{}@{}(invalid)", className(), static_cast<const void*>(this));
    }
    return std::format("{}@{}(owner={}, idx={}, gen={})",
                       className(),
                       static_cast<const void*>(this),
                       handleOwner(),
                       handleIndex(),
                       handleGeneration());
}

} // namespace EntropyEngine::Networking
