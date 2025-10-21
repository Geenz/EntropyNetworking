/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file WebDAVFileSystemBackend.h
 * @brief VirtualFileSystem backend for WebDAV resources
 *
 * This file contains WebDAVFileSystemBackend, a read-only IFileSystemBackend
 * implementation that exposes remote WebDAV servers as VFS mount points.
 */

#pragma once

#include <memory>
#include <string>
#include <span>
#include <vector>
#include <optional>

#include <VirtualFileSystem/IFileSystemBackend.h>

#include "Networking/HTTP/HttpClient.h"
#include "Networking/WebDAV/WebDAVPropfindParser.h"
#include "Networking/WebDAV/WebDAVUtils.h"

namespace EntropyEngine::Networking::WebDAV {

/**
 * @brief Read-only VFS backend for WebDAV servers
 *
 * Implements IFileSystemBackend for remote WebDAV resources. Uses VirtualFileSystem::submit()
 * to schedule async operations through the VFS WorkContractGroup (no custom threads).
 * Supports connection pooling for concurrent HTTP requests.
 *
 * Read Operations:
 * - readFile() - HTTP GET with optional Range header
 * - getMetadata() - WebDAV PROPFIND Depth:0
 * - listDirectory() - WebDAV PROPFIND Depth:1
 * - exists() - WebDAV PROPFIND Depth:0
 * - openStream() - Streaming GET for large files
 *
 * Thread Safety: All methods are thread-safe. Concurrent operations are handled by the VFS
 * WorkContractGroup and connection pool (if configured).
 *
 * @code
 * // Create backend
 * WebDAVFileSystemBackend::Config cfg{
 *     .scheme = "https",
 *     .host = "example.com",
 *     .baseUrl = "/dav"
 * };
 * auto backend = std::make_shared<WebDAVFileSystemBackend>(cfg);
 *
 * // Mount in VFS
 * vfs->mount("/remote", backend);
 *
 * // Use via VFS
 * auto fileHandle = vfs->createFileHandle("/remote/data.bin");
 * auto readOp = fileHandle.read();
 * readOp.wait();
 * processData(readOp.bytes());
 * @endcode
 */
class WebDAVFileSystemBackend : public Core::IO::IFileSystemBackend {
public:
    /**
     * @brief Backend configuration
     */
    struct Config {
        std::string scheme = "https";  ///< HTTP scheme ("http" or "https", defaults to "https")
        std::string host;              ///< Server hostname with optional port (e.g., "example.com:8080")
        std::string baseUrl;           ///< Base URL path on server (e.g., "/dav/assets")
        std::string authHeader;        ///< Optional Authorization header value (e.g., "Bearer ..." or "Basic ...")
    };

    /**
     * @brief Constructs WebDAV backend with HttpClient
     * @param cfg Backend configuration (scheme, host, base URL)
     */
    explicit WebDAVFileSystemBackend(Config cfg)
        : _client(), _cfg(std::move(cfg)) {}

    /**
     * @brief Reads file via HTTP GET (supports Range header)
     * @param path VFS path (mapped to baseUrl + path)
     * @param options Read options (offset, length for partial reads)
     * @return FileOperationHandle that completes with file bytes
     */
    Core::IO::FileOperationHandle readFile(const std::string& path,
                                           Core::IO::ReadOptions options = {}) override;

    /**
     * @brief Write operations not supported (read-only backend)
     * @return Immediate failure handle
     */
    Core::IO::FileOperationHandle writeFile(const std::string& path,
                                            std::span<const std::byte> data,
                                            Core::IO::WriteOptions options = {}) override;

    /**
     * @brief Delete operations not supported (read-only backend)
     * @return Immediate failure handle
     */
    Core::IO::FileOperationHandle deleteFile(const std::string& path) override;

    /**
     * @brief Create operations not supported (read-only backend)
     * @return Immediate failure handle
     */
    Core::IO::FileOperationHandle createFile(const std::string& path) override;

    /**
     * @brief Gets file/directory metadata via PROPFIND Depth:0
     * @param path VFS path
     * @return FileOperationHandle that completes with FileMetadata
     */
    Core::IO::FileOperationHandle getMetadata(const std::string& path) override;

    /**
     * @brief Checks if resource exists via PROPFIND Depth:0
     * @param path VFS path
     * @return true if resource exists and is accessible
     */
    bool exists(const std::string& path) override;

    /**
     * @brief Lists directory contents via PROPFIND Depth:1
     * @param path VFS path to directory
     * @param options Listing options (unused in current implementation)
     * @return FileOperationHandle that completes with DirectoryEntry vector
     */
    Core::IO::FileOperationHandle listDirectory(const std::string& path,
                                                Core::IO::ListDirectoryOptions options = {}) override;

    /**
     * @brief Opens streaming read for large files
     * @param path VFS path
     * @param options Stream options (mode must be Read, bufferSize configurable)
     * @return FileStream for incremental reads, or nullptr if mode is not Read
     */
    std::unique_ptr<Core::IO::FileStream> openStream(const std::string& path,
                                                     Core::IO::StreamOptions options = {}) override;

    /**
     * @brief Line operations not supported
     * @return Immediate failure handle
     */
    Core::IO::FileOperationHandle readLine(const std::string& path, size_t lineNumber) override;

    /**
     * @brief Line operations not supported
     * @return Immediate failure handle
     */
    Core::IO::FileOperationHandle writeLine(const std::string& path, size_t lineNumber, std::string_view line) override;

    /**
     * @brief Gets backend capabilities
     * @return Capabilities indicating read-only, streaming, remote backend
     */
    Core::IO::BackendCapabilities getCapabilities() const override;

    /**
     * @brief Gets backend type identifier
     * @return "WebDAV"
     */
    std::string getBackendType() const override { return "WebDAV"; }

    /**
     * @brief Path normalization for VFS locking (pass-through)
     * @param path Path to normalize
     * @return Path unchanged (WebDAV paths are used as-is)
     */
    std::string normalizeKey(const std::string& path) const override { return path; }

private:
    HTTP::HttpClient _client;  ///< HTTP client for WebDAV operations
    Config _cfg;               ///< Backend configuration

    /**
     * @brief Builds full URL from VFS path
     * @param path VFS path
     * @return Full URL (baseUrl + encoded path)
     * @throws std::invalid_argument if path contains parent directory references
     */
    std::string buildUrl(const std::string& path) const;

    /**
     * @brief Maps HTTP status code to FileError
     * @param statusCode HTTP status code
     * @return Corresponding FileError
     */
    static Core::IO::FileError mapHttpStatus(int statusCode);
};

} // namespace EntropyEngine::Networking::WebDAV
