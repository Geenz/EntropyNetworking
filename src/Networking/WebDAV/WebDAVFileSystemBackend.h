/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#pragma once

#include <memory>
#include <string>
#include <span>
#include <vector>
#include <optional>

// EntropyCore VFS (via vcpkg dependency)
#include <VirtualFileSystem/IFileSystemBackend.h>

// Our WebDAV client pieces
#include "Networking/WebDAV/WebDAVConnection.h"
#include "Networking/WebDAV/WebDAVPropfindParser.h"
#include "Networking/WebDAV/WebDAVUtils.h"

namespace EntropyEngine::Networking::WebDAV {

/**
 * WebDAVFileSystemBackend (read-only MVP skeleton)
 *
 * Notes:
 * - EntropyCore's VirtualFileSystem constructs FileOperationHandle state internally.
 *   Third-party backends cannot currently create OpState directly (friend-restricted).
 * - This skeleton provides a compiling backend that implements exists() and capability
 *   reporting, while read/list/metadata return immediate failed handles until
 *   EntropyCore exposes a generic submit mechanism or adds this backend to friends.
 */
class WebDAVFileSystemBackend : public Core::IO::IFileSystemBackend {
public:
    struct Config {
        std::string baseUrl; // e.g., "/dav/assets"
    };

    WebDAVFileSystemBackend(std::shared_ptr<WebDAVConnection> conn, Config cfg)
        : _conn(std::move(conn)), _cfg(std::move(cfg)) {}

    // Core file operations (read-only MVP: currently stubbed)
    Core::IO::FileOperationHandle readFile(const std::string& path,
                                           Core::IO::ReadOptions options = {}) override;

    Core::IO::FileOperationHandle writeFile(const std::string& path,
                                            std::span<const std::byte> data,
                                            Core::IO::WriteOptions options = {}) override;

    Core::IO::FileOperationHandle deleteFile(const std::string& path) override;
    Core::IO::FileOperationHandle createFile(const std::string& path) override;

    // Metadata operations
    Core::IO::FileOperationHandle getMetadata(const std::string& path) override;
    bool exists(const std::string& path) override;

    // Directory operations
    Core::IO::FileOperationHandle listDirectory(const std::string& path,
                                                Core::IO::ListDirectoryOptions options = {}) override;

    // Stream support (not supported in MVP)
    std::unique_ptr<Core::IO::FileStream> openStream(const std::string& path,
                                                     Core::IO::StreamOptions options = {}) override;

    // Line ops (not supported in MVP)
    Core::IO::FileOperationHandle readLine(const std::string& path, size_t lineNumber) override;
    Core::IO::FileOperationHandle writeLine(const std::string& path, size_t lineNumber, std::string_view line) override;

    // Backend info
    Core::IO::BackendCapabilities getCapabilities() const override;
    std::string getBackendType() const override { return "WebDAV"; }

    // Path normalization for identity/locking (pass-through)
    std::string normalizeKey(const std::string& path) const override { return path; }

private:
    std::shared_ptr<WebDAVConnection> _conn;
    Config _cfg;

    std::string buildUrl(const std::string& path) const;

    static Core::IO::FileError mapHttpStatus(int statusCode);
};

} // namespace EntropyEngine::Networking::WebDAV
