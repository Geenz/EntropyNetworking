/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file WebDavAdapter.h
 * @brief VFS-to-WebDAV adapter for HTTP server integration
 *
 * This file contains WebDavAdapter, a lightweight adapter that exposes
 * EntropyCore's VirtualFileSystem over WebDAV (RFC 4918) without binding
 * to a specific HTTP server implementation.
 */

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>

// VFS (from EntropyCore)
#include <VirtualFileSystem/VirtualFileSystem.h>
#include <VirtualFileSystem/FileHandle.h>
#include <VirtualFileSystem/DirectoryHandle.h>

namespace EntropyEngine::Networking {

/**
 * @brief Minimal HTTP request representation for WebDAV operations
 *
 * Lightweight HTTP request type avoiding dependencies on external HTTP libraries.
 */
struct HttpRequestLite {
    std::string method;                 ///< HTTP method (e.g. "GET", "HEAD", "PROPFIND", "OPTIONS")
    std::string urlPath;                ///< Decoded server-relative path (e.g. "/dav/assets/foo.bin")
    std::unordered_map<std::string, std::string> headers; ///< HTTP headers (case-insensitive matching not enforced)
    std::string body;                   ///< Optional request body (e.g., PROPFIND XML)
};

/**
 * @brief Minimal HTTP response representation for WebDAV operations
 *
 * Lightweight HTTP response type avoiding dependencies on external HTTP libraries.
 */
struct HttpResponseLite {
    int status = 500;                   ///< HTTP status code (default 500 Internal Server Error)
    std::unordered_map<std::string, std::string> headers; ///< HTTP response headers
    std::string body;                   ///< Response body (empty for HEAD, populated for GET/PROPFIND)
};

/**
 * @brief VFS-to-WebDAV adapter for HTTP server integration
 *
 * WebDavAdapter bridges EntropyCore's VirtualFileSystem to WebDAV (RFC 4918) semantics
 * without binding to a specific HTTP server implementation. Provides minimal handler
 * methods that can be wired into any HTTP library (e.g., Boost.Beast, cpp-httplib).
 *
 * Features (Phase 1 MVP):
 * - OPTIONS: Returns DAV compliance class (Class 1, read-only)
 * - GET: Streams VFS file contents with Content-Type detection
 * - HEAD: Returns file metadata without body
 * - PROPFIND: Returns XML multistatus with resource properties (Depth 0/1)
 * - Path mapping: Translates WebDAV URLs to VFS paths via configurable mount prefix
 * - Read-only: No MKCOL/PUT/DELETE/COPY/MOVE support (future extension)
 *
 * Thread Safety: All handler methods are const and thread-safe if the underlying VFS
 * is thread-safe. Can handle concurrent requests from HTTP server threads.
 *
 * Usage Pattern: Integrate with HTTP server request routing to delegate WebDAV
 * requests to appropriate handlers based on method and path.
 *
 * @code
 * // Setup VFS with multiple backends
 * auto vfs = std::make_shared<VirtualFileSystem>();
 * vfs->mount("/assets", assetBackend);
 * vfs->mount("/config", configBackend);
 *
 * // Create WebDAV adapter with /dav mount prefix
 * WebDavAdapter adapter(vfs, "/dav/");
 *
 * // Integrate with HTTP server (pseudo-code)
 * httpServer.onRequest([&adapter](const auto& rawReq) -> auto {
 *     HttpRequestLite req{
 *         .method = rawReq.method,
 *         .urlPath = rawReq.path,
 *         .headers = rawReq.headers
 *     };
 *
 *     if (!adapter.handles(req.urlPath)) {
 *         return HttpResponse{404, "Not Found"};
 *     }
 *
 *     HttpResponseLite resp;
 *     if (req.method == "OPTIONS") {
 *         resp = adapter.handleOptions(req);
 *     } else if (req.method == "PROPFIND") {
 *         int depth = parseDepthHeader(req.headers["Depth"]); // 0, 1, or infinity
 *         resp = adapter.handlePropfind(req, depth);
 *     } else if (req.method == "HEAD") {
 *         resp = adapter.handleHead(req);
 *     } else if (req.method == "GET") {
 *         resp = adapter.handleGet(req);
 *     } else {
 *         resp.status = 405; // Method Not Allowed
 *     }
 *
 *     return convertToServerResponse(resp);
 * });
 *
 * // Client can now access VFS over WebDAV:
 * // GET http://localhost/dav/assets/texture.png  -> reads from assetBackend
 * // PROPFIND http://localhost/dav/config/ Depth:1 -> lists config directory
 * @endcode
 */
class WebDavAdapter {
public:
    /**
     * @brief Constructs WebDAV adapter with VFS and mount prefix
     * @param vfs Shared VirtualFileSystem instance to expose over WebDAV
     * @param mountPrefix URL path prefix for WebDAV operations (e.g., "/dav/")
     */
    explicit WebDavAdapter(std::shared_ptr<EntropyEngine::Core::IO::VirtualFileSystem> vfs,
                           std::string mountPrefix = "/dav/")
        : _vfs(std::move(vfs)), _mountPrefix(std::move(mountPrefix)) {}

    /**
     * @brief Checks if URL path is under WebDAV mount prefix
     * @param urlPath Server-relative URL path (e.g., "/dav/assets/foo.bin")
     * @return true if path starts with mount prefix
     */
    bool handles(const std::string& urlPath) const;

    /**
     * @brief Translates WebDAV URL path to VFS path
     *
     * Strips mount prefix from URL path to produce VFS-relative path.
     * Example: "/dav/foo/bar.txt" -> "foo/bar.txt"
     *
     * @param urlPath Server-relative URL path
     * @return VFS path if under mount prefix, nullopt otherwise
     */
    std::optional<std::string> toVfsPath(const std::string& urlPath) const;

    /**
     * @brief Handles OPTIONS request (WebDAV capability discovery)
     *
     * Returns DAV compliance class and supported methods.
     *
     * @param req HTTP request
     * @return Response with DAV: Class 1 header and Allow: GET,HEAD,PROPFIND,OPTIONS
     */
    HttpResponseLite handleOptions(const HttpRequestLite& req) const;

    /**
     * @brief Handles PROPFIND request (resource property query)
     *
     * Returns WebDAV multistatus (207) with resource properties.
     * Depth 0: Single resource. Depth 1: Resource + children.
     *
     * @param req HTTP request
     * @param depth PROPFIND depth (0 or 1, infinity not supported)
     * @return 207 Multistatus with XML body, or 404 if resource not found
     */
    HttpResponseLite handlePropfind(const HttpRequestLite& req, int depth = 0) const;

    /**
     * @brief Handles HEAD request (metadata only, no body)
     *
     * Returns file metadata (Content-Length, Content-Type, Last-Modified)
     * without response body.
     *
     * @param req HTTP request
     * @return 200 OK with headers, or 404 if file not found
     */
    HttpResponseLite handleHead(const HttpRequestLite& req) const;

    /**
     * @brief Handles GET request (file content retrieval)
     *
     * Reads file from VFS and returns contents with Content-Type detection.
     *
     * @param req HTTP request
     * @return 200 OK with file body, or 404 if file not found
     */
    HttpResponseLite handleGet(const HttpRequestLite& req) const;

private:
    using Vfs = EntropyEngine::Core::IO::VirtualFileSystem;
    using FileHandle = EntropyEngine::Core::IO::FileHandle;
    using DirectoryHandle = EntropyEngine::Core::IO::DirectoryHandle;
    using FileOpStatus = EntropyEngine::Core::IO::FileOpStatus;

    /**
     * @brief Removes leading slash from path
     * @param s Path string
     * @return Path without leading slash
     */
    static std::string trimLeadingSlash(std::string s);

    /**
     * @brief Guesses MIME type from file extension
     *
     * Simple extension-based content type detection (e.g., ".png" -> "image/png").
     * Falls back to "application/octet-stream" for unknown types.
     *
     * @param path File path with extension
     * @return MIME type string
     */
    static std::string guessContentType(std::string_view path);

    /**
     * @brief Builds minimal WebDAV multistatus XML response
     *
     * Constructs RFC 4918 compliant XML for PROPFIND responses.
     *
     * @param selfHref Resource href for the response element
     * @return XML string with multistatus structure
     */
    static std::string buildMinimalMultistatus(const std::string& selfHref);

    std::shared_ptr<Vfs> _vfs;          ///< VirtualFileSystem instance to expose over WebDAV
    std::string _mountPrefix;           ///< URL path prefix for WebDAV operations (e.g., "/dav/")
};

} // namespace EntropyEngine::Networking
