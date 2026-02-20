/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include "SessionHandle.h"

#include <format>

#include "SessionManager.h"

namespace EntropyEngine::Networking
{

SessionManager* SessionHandle::manager() const {
    return static_cast<SessionManager*>(const_cast<void*>(handleOwner()));
}

Result<void> SessionHandle::sendEntityCreated(uint64_t entityId, const std::string& appId, const std::string& typeName,
                                              uint64_t parentId,
                                              const std::vector<NetworkSession::ComponentGroupData>& components,
                                              uint64_t targetSceneId, const std::string& entityName) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendEntityCreated(*this, entityId, appId, typeName, parentId, components, targetSceneId, entityName);
}

Result<void> SessionHandle::sendEntityDestroyed(uint64_t entityId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendEntityDestroyed(*this, entityId);
}

Result<void> SessionHandle::sendComponentAdded(uint64_t entityId,
                                               const NetworkSession::ComponentGroupData& component) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendComponentAdded(*this, entityId, component);
}

Result<void> SessionHandle::sendComponentRemoved(uint64_t entityId, ComponentTypeHash typeHash) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendComponentRemoved(*this, entityId, typeHash);
}

Result<void> SessionHandle::sendPropertyUpdate(PropertyHash hash, PropertyType type, const PropertyValue& value) const {
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

Result<void> SessionHandle::sendHeartbeat() const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendHeartbeat(*this);
}

// Asset protocol operations

Result<void> SessionHandle::sendAssetAdvertise(const std::string& appId,
                                               const std::vector<NetworkSession::AssetEntryData>& entries,
                                               uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetAdvertise(*this, appId, entries, requestId);
}

Result<void> SessionHandle::sendAssetWithdraw(const std::vector<std::array<uint8_t, 32>>& assetIds,
                                              uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetWithdraw(*this, assetIds, requestId);
}

Result<void> SessionHandle::sendAssetWithdrawAll(const std::string& appId, uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetWithdrawAll(*this, appId, requestId);
}

Result<void> SessionHandle::sendAssetResolve(const std::array<uint8_t, 32>& assetId, uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetResolve(*this, assetId, requestId);
}

Result<void> SessionHandle::sendAssetResolveBatch(const std::vector<std::array<uint8_t, 32>>& assetIds,
                                                  uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetResolveBatch(*this, assetIds, requestId);
}

Result<void> SessionHandle::sendAssetProvideKey(const std::array<uint8_t, 32>& assetId,
                                                const std::array<uint8_t, 32>& key, uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetProvideKey(*this, assetId, key, requestId);
}

Result<void> SessionHandle::sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data,
                                            uint8_t contentType, bool persistent, uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetUpload(*this, appId, data, contentType, persistent, requestId);
}

Result<void> SessionHandle::sendAssetUpload(const std::string& appId, const std::vector<uint8_t>& data,
                                            uint8_t contentType, bool persistent, uint64_t requestId,
                                            const NetworkSession::AssetMetadataData& metadata) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetUpload(*this, appId, data, contentType, persistent, requestId, metadata);
}

Result<void> SessionHandle::sendAssetFetch(const std::array<uint8_t, 32>& assetId, uint64_t requestId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetFetch(*this, assetId, requestId);
}

Result<void> SessionHandle::sendAssetUploadBegin(const NetworkSession::AssetUploadBeginData& data) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetUploadBegin(*this, data);
}

Result<void> SessionHandle::sendAssetUploadChunk(const NetworkSession::AssetUploadChunkData& data) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetUploadChunk(*this, data);
}

Result<void> SessionHandle::sendAssetUploadComplete(const NetworkSession::AssetUploadCompleteData& data) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetUploadComplete(*this, data);
}

Result<void> SessionHandle::sendAssetUploadCancel(const std::array<uint8_t, 16>& uploadId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendAssetUploadCancel(*this, uploadId);
}

bool SessionHandle::supportsMultipleChannels() const {
    auto* mgr = manager();
    if (!mgr) {
        return false;
    }
    return mgr->supportsMultipleChannels(*this);
}

Result<void> SessionHandle::openChannel(const std::string& channel) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->openChannel(*this, channel);
}

// Permission system operations

Result<void> SessionHandle::sendPermissionRequest(uint64_t requestId, uint64_t requestingSessionId,
                                                  const std::string& appId, PermissionKey key,
                                                  const std::string& reason) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendPermissionRequest(*this, requestId, requestingSessionId, appId, key, reason);
}

Result<void> SessionHandle::sendHeadPosePermissionResponse(uint64_t requestId, uint64_t respondingSessionId,
                                                           bool granted, uint64_t grantToken, bool isNewGrant) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendHeadPosePermissionResponse(*this, requestId, respondingSessionId, granted, grantToken, isNewGrant);
}

Result<void> SessionHandle::sendIdentityPermissionResponse(uint64_t requestId, uint64_t respondingSessionId,
                                                           bool granted, uint64_t grantToken, bool isNewGrant,
                                                           const std::string& identityHash) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendIdentityPermissionResponse(*this, requestId, respondingSessionId, granted, grantToken, isNewGrant,
                                               identityHash);
}

Result<void> SessionHandle::sendUsernamePermissionResponse(uint64_t requestId, uint64_t respondingSessionId,
                                                           bool granted, uint64_t grantToken, bool isNewGrant,
                                                           const std::string& username) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendUsernamePermissionResponse(*this, requestId, respondingSessionId, granted, grantToken, isNewGrant,
                                               username);
}

Result<void> SessionHandle::sendHostnamePermissionResponse(uint64_t requestId, uint64_t respondingSessionId,
                                                           bool granted, uint64_t grantToken, bool isNewGrant,
                                                           const std::string& hostname) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendHostnamePermissionResponse(*this, requestId, respondingSessionId, granted, grantToken, isNewGrant,
                                               hostname);
}

Result<void> SessionHandle::sendPermissionRevoked(uint64_t portalSessionId, PermissionKey key) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendPermissionRevoked(*this, portalSessionId, key);
}

Result<void> SessionHandle::sendPermissionRequestCancelled(uint64_t requestId, PermissionKey key) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->sendPermissionRequestCancelled(*this, requestId, key);
}

Result<void> SessionHandle::performHandshake(const std::string& clientType, const std::string& clientId) const {
    auto* mgr = manager();
    if (!mgr) {
        return Result<void>::err(NetworkError::InvalidParameter, "Invalid session handle");
    }
    return mgr->performHandshake(*this, clientType, clientId);
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
    static const uint64_t hash = static_cast<uint64_t>(Core::TypeSystem::createTypeId<SessionHandle>().id);
    return hash;
}

std::string SessionHandle::toString() const {
    if (!hasHandle()) {
        return std::format("{}@{}(invalid)", className(), static_cast<const void*>(this));
    }
    return std::format("{}@{}(owner={}, idx={}, gen={})", className(), static_cast<const void*>(this), handleOwner(),
                       handleIndex(), handleGeneration());
}

}  // namespace EntropyEngine::Networking
