/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
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

// Extremely small HTTP types so we don't introduce a hard dependency on an HTTP library here.
struct HttpRequestLite {
    std::string method;                 // e.g. "GET", "HEAD", "PROPFIND", "OPTIONS"
    std::string urlPath;                // decoded server-relative path, e.g. "/dav/assets/foo.bin"
    std::unordered_map<std::string, std::string> headers; // case-insensitive not enforced here
};

struct HttpResponseLite {
    int status = 500; // HTTP status code
    std::unordered_map<std::string, std::string> headers;
    std::string body; // For HEAD, keep empty body; for GET, fill bytes (binary-safe storage not modeled here)
};

/**
 * WebDavAdapter (MVP skeleton)
 *
 * Purpose: Adapt EntropyCore's VirtualFileSystem (VFS) to WebDAV semantics without
 * binding to a specific HTTP server implementation. This class offers minimal
 * handler methods you can wire into any HTTP library.
 *
 * Scope (Phase 1 MVP):
 *  - OPTIONS, GET, HEAD, PROPFIND (Depth 0 and 1 minimal)
 *  - Read-only. No MKCOL/PUT/DELETE/etc.
 *
 * This is a compile-ready skeleton. It provides working GET/HEAD using VFS and
 * placeholder-but-valid OPTIONS/PROPFIND responses. It is intentionally light to
 * avoid adding new third-party dependencies in this repository.
 */
class WebDavAdapter {
public:
    explicit WebDavAdapter(std::shared_ptr<EntropyEngine::Core::IO::VirtualFileSystem> vfs,
                           std::string mountPrefix = "/dav/")
        : _vfs(std::move(vfs)), _mountPrefix(std::move(mountPrefix)) {}

    // Returns true if the path is under the configured WebDAV mount prefix
    bool handles(const std::string& urlPath) const;

    // Translate a request path like "/dav/foo/bar.txt" -> "foo/bar.txt" (VFS path)
    std::optional<std::string> toVfsPath(const std::string& urlPath) const;

    // Handlers
    HttpResponseLite handleOptions(const HttpRequestLite& req) const;
    HttpResponseLite handlePropfind(const HttpRequestLite& req, int depth = 0) const;
    HttpResponseLite handleHead(const HttpRequestLite& req) const;
    HttpResponseLite handleGet(const HttpRequestLite& req) const;

private:
    using Vfs = EntropyEngine::Core::IO::VirtualFileSystem;
    using FileHandle = EntropyEngine::Core::IO::FileHandle;
    using DirectoryHandle = EntropyEngine::Core::IO::DirectoryHandle;
    using FileOpStatus = EntropyEngine::Core::IO::FileOpStatus;

    static std::string trimLeadingSlash(std::string s);
    static std::string guessContentType(std::string_view path);

    // Very small XML builder for minimal multistatus
    static std::string buildMinimalMultistatus(const std::string& selfHref);

    std::shared_ptr<Vfs> _vfs;
    std::string _mountPrefix; // e.g. "/dav/"
};

} // namespace EntropyEngine::Networking
