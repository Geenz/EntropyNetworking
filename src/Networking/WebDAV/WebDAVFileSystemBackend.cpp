/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "Networking/WebDAV/WebDAVFileSystemBackend.h"
#include "Networking/WebDAV/WebDAVReadStream.h"

#include <stdexcept>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>

#include <VirtualFileSystem/FileStream.h>
#include <VirtualFileSystem/VirtualFileSystem.h>


using namespace EntropyEngine::Core::IO;

namespace EntropyEngine::Networking::WebDAV {

static bool hasParentTraversal(std::string_view p) {
    return p.find("../") != std::string_view::npos || p.find("..\\") != std::string_view::npos;
}

// Map an href from a WebDAV response back into a VFS path under baseUrl
static std::optional<std::string> hrefToVfsPath(const std::string& href, const std::string& baseUrl) {
    std::string path = Utils::stripSchemeHost(href);
    // strip query/fragment
    auto q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);

    std::string base = baseUrl;
    if (!base.empty() && base.back() == '/') base.pop_back();

    // Must be under our base
    if (path.rfind(base, 0) != 0) return std::nullopt;

    std::string rel = path.substr(base.size());
    if (rel.empty()) rel = "/";
    if (!rel.empty() && rel.front() != '/') rel.insert(rel.begin(), '/');

    auto decoded = Utils::percentDecode(rel);
    if (!decoded) return std::nullopt;
    return *decoded;
}

std::string WebDAVFileSystemBackend::buildUrl(const std::string& path) const {
    if (hasParentTraversal(path)) {
        throw std::invalid_argument("Path contains parent directory references: " + path);
    }
    std::string clean = path;
    if (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());

    std::string base = _cfg.baseUrl;
    if (!base.empty() && base.back() == '/') base.pop_back();

    std::string encoded = Utils::percentEncode(clean, /*keepSlashes=*/true);
    return base + "/" + encoded;
}

FileError WebDAVFileSystemBackend::mapHttpStatus(int sc) {
    switch (sc) {
        case 200: case 201: case 204: case 206: case 207: return FileError::None;
        case 401: case 403: return FileError::AccessDenied;
        case 404: return FileError::FileNotFound;
        case 405: return FileError::InvalidPath; // Method not allowed
        case 409: return FileError::Conflict;
        case 413: return FileError::DiskFull;
        case 416: return FileError::InvalidPath; // Range not satisfiable (treat as invalid for now)
        case 423: return FileError::AccessDenied; // Locked
        case 507: return FileError::DiskFull; // Insufficient Storage
        case 500: case 502: case 503: case 504: return FileError::NetworkError;
        default:
            if (sc >= 400 && sc < 500) return FileError::InvalidPath;
            return FileError::NetworkError;
    }
}

FileOperationHandle WebDAVFileSystemBackend::readFile(const std::string& path, ReadOptions options) {
    if (!_vfs) return FileOperationHandle::immediate(FileOpStatus::Failed);

    return _vfs->submit(path, [this, options](FileOperationHandle::OpState& s,
                                              const std::string& p,
                                              const ExecContext& /*ctx*/){
        try {
            const std::string url = buildUrl(p);

            HTTP::HttpRequest req;
            req.method = HTTP::HttpMethod::GET;
            req.scheme = _cfg.scheme;
            req.host = _cfg.host;
            req.path = url;
            if (!_cfg.authHeader.empty()) {
                req.headers["Authorization"] = _cfg.authHeader;
            }

            if (options.offset > 0 || options.length.has_value()) {
                const uint64_t start = options.offset;
                std::string range = "bytes=" + std::to_string(start) + "-";
                if (options.length.has_value()) {
                    const uint64_t end = start + static_cast<uint64_t>(*options.length) - 1;
                    range += std::to_string(end);
                }
                req.headers["Range"] = range;
            }

            auto r = _client.execute(req);
            if (!r.isSuccess()) {
                auto fe = mapHttpStatus(r.statusCode);
                s.setError(fe, "GET failed: " + std::to_string(r.statusCode) + " " + r.statusMessage, p);
                s.complete(FileOpStatus::Failed);
                return;
            }

            s.bytes.assign(reinterpret_cast<const std::byte*>(r.body.data()),
                           reinterpret_cast<const std::byte*>(r.body.data()) + r.body.size());
            s.complete(FileOpStatus::Complete);
        } catch (const std::exception& e) {
            s.setError(FileError::NetworkError, e.what(), p);
            s.complete(FileOpStatus::Failed);
        }
    });
}

FileOperationHandle WebDAVFileSystemBackend::writeFile(const std::string& path,
                                                       std::span<const std::byte> data,
                                                       WriteOptions options) {
    (void)path; (void)data; (void)options;
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle WebDAVFileSystemBackend::deleteFile(const std::string& path) {
    (void)path;
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle WebDAVFileSystemBackend::createFile(const std::string& path) {
    (void)path;
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle WebDAVFileSystemBackend::getMetadata(const std::string& path) {
    if (!_vfs) return FileOperationHandle::immediate(FileOpStatus::Failed);

    return _vfs->submit(path, [this](FileOperationHandle::OpState& s,
                                     const std::string& p,
                                     const ExecContext& /*ctx*/){
        try {
            const std::string url = buildUrl(p);
            const std::string bodyXml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<D:propfind xmlns:D=\"DAV:\">"
                "  <D:prop>"
                "    <D:resourcetype/>"
                "    <D:getcontentlength/>"
                "    <D:getlastmodified/>"
                "    <D:getcontenttype/>"
                "  </D:prop>"
                "</D:propfind>";

            HTTP::HttpRequest req;
            req.method = HTTP::HttpMethod::PROPFIND;
            req.scheme = _cfg.scheme;
            req.host = _cfg.host;
            req.path = url;
            if (!_cfg.authHeader.empty()) {
                req.headers["Authorization"] = _cfg.authHeader;
            }
            req.headers["Depth"] = "0";
            req.headers["Content-Type"] = "application/xml; charset=utf-8";
            req.body.assign(bodyXml.begin(), bodyXml.end());

            auto r = _client.execute(req);
            if (r.statusCode != 200 && r.statusCode != 207) {
                auto fe = mapHttpStatus(r.statusCode);
                if (fe == FileError::FileNotFound) {
                    FileMetadata meta; meta.path = p; meta.exists = false;
                    s.metadata = std::move(meta);
                    s.complete(FileOpStatus::Complete);
                    return;
                }
                s.setError(fe, "PROPFIND d0 failed: " + std::to_string(r.statusCode), p);
                s.complete(FileOpStatus::Failed);
                return;
            }

            auto infos = parsePropfindXml(r.body);
            if (infos.empty()) {
                FileMetadata meta; meta.path = p; meta.exists = false;
                s.metadata = std::move(meta);
                s.complete(FileOpStatus::Complete);
                return;
            }

            const std::string want = Utils::normalizeHrefForCompare(url);
            const DavResourceInfo* self = nullptr;
            for (const auto& ri : infos) {
                if (Utils::normalizeHrefForCompare(ri.href) == want ||
                    Utils::normalizeHrefForCompare(ri.href, /*ensureTrailingSlashIfCollection=*/true) == want) {
                    self = &ri; break;
                }
            }

            if (!self) {
                FileMetadata meta; meta.path = p; meta.exists = false;
                s.metadata = std::move(meta);
                s.complete(FileOpStatus::Complete);
                return;
            }

            FileMetadata meta;
            meta.path = p;
            meta.exists = true;
            meta.isDirectory = self->isCollection;
            meta.isRegularFile = !self->isCollection;
            meta.size = self->contentLength;
            meta.readable = true; meta.writable = false; meta.executable = false;
            meta.lastModified = self->lastModified;
            if (self->contentType) meta.mimeType = *self->contentType;

            s.metadata = std::move(meta);
            s.complete(FileOpStatus::Complete);
        } catch (const std::exception& e) {
            s.setError(FileError::NetworkError, e.what(), p);
            s.complete(FileOpStatus::Failed);
        }
    });
}

bool WebDAVFileSystemBackend::exists(const std::string& path) {
    try {
        const std::string reqPath = buildUrl(path);
        const std::string bodyXml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<D:propfind xmlns:D=\"DAV:\">"
            "  <D:prop>"
            "    <D:resourcetype/>"
            "  </D:prop>"
            "</D:propfind>";

        HTTP::HttpRequest req;
        req.method = HTTP::HttpMethod::PROPFIND;
        req.scheme = _cfg.scheme;
        req.host = _cfg.host;
        req.path = reqPath;
        if (!_cfg.authHeader.empty()) {
            req.headers["Authorization"] = _cfg.authHeader;
        }
        req.headers["Depth"] = "0";
        req.headers["Content-Type"] = "application/xml; charset=utf-8";
        req.body.assign(bodyXml.begin(), bodyXml.end());

        auto r = _client.execute(req);
        if (r.statusCode != 200 && r.statusCode != 207) return false;
        auto infos = parsePropfindXml(r.body);
        if (infos.empty()) return false;
        // Normalize and compare hrefs
        const std::string want = Utils::normalizeHrefForCompare(reqPath);
        for (const auto& info : infos) {
            if (Utils::normalizeHrefForCompare(info.href) == want ||
                Utils::normalizeHrefForCompare(info.href, /*ensureTrailingSlashIfCollection=*/true) == want)
                return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

FileOperationHandle WebDAVFileSystemBackend::listDirectory(const std::string& path, ListDirectoryOptions options) {
    if (!_vfs) return FileOperationHandle::immediate(FileOpStatus::Failed);

    return _vfs->submit(path, [this, options](FileOperationHandle::OpState& s,
                                              const std::string& p,
                                              const ExecContext& /*ctx*/){
        try {
            // Ensure trailing slash for directory
            std::string pathForUrl = p;
            if (pathForUrl.empty() || pathForUrl.back() != '/') pathForUrl.push_back('/');
            const std::string url = buildUrl(pathForUrl);

            const std::string bodyXml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<D:propfind xmlns:D=\"DAV:\">"
                "  <D:prop>"
                "    <D:resourcetype/>"
                "    <D:getcontentlength/>"
                "    <D:getlastmodified/>"
                "    <D:getcontenttype/>"
                "  </D:prop>"
                "</D:propfind>";

            HTTP::HttpRequest req;
            req.method = HTTP::HttpMethod::PROPFIND;
            req.scheme = _cfg.scheme;
            req.host = _cfg.host;
            req.path = url;
            if (!_cfg.authHeader.empty()) {
                req.headers["Authorization"] = _cfg.authHeader;
            }
            req.headers["Depth"] = "1";
            req.headers["Content-Type"] = "application/xml; charset=utf-8";
            req.body.assign(bodyXml.begin(), bodyXml.end());

            auto r = _client.execute(req);
            if (r.statusCode != 200 && r.statusCode != 207) {
                auto fe = mapHttpStatus(r.statusCode);
                s.setError(fe, "PROPFIND d1 failed: " + std::to_string(r.statusCode), p);
                s.complete(FileOpStatus::Failed);
                return;
            }

            auto infos = parsePropfindXml(r.body);
            const std::string selfNorm = Utils::normalizeHrefForCompare(url, /*ensureTrailingSlashIfCollection=*/true);

            std::vector<DirectoryEntry> entries;
            entries.reserve(infos.size());

            for (const auto& ri : infos) {
                std::string norm = Utils::normalizeHrefForCompare(ri.href, /*ensureTrailingSlashIfCollection=*/ri.isCollection);
                if (norm == selfNorm) continue; // skip self

                auto vfsPathOpt = hrefToVfsPath(ri.href, _cfg.baseUrl);
                if (!vfsPathOpt) continue; // out-of-scope
                std::string vfsPath = *vfsPathOpt;

                DirectoryEntry de;
                de.fullPath = vfsPath;
                std::string name = vfsPath;
                if (!name.empty() && name.back()=='/' && ri.isCollection) name.pop_back();
                auto slash = name.find_last_of('/');
                de.name = (slash == std::string::npos) ? name : name.substr(slash + 1);

                FileMetadata meta;
                meta.path = vfsPath;
                meta.exists = true;
                meta.isDirectory   = ri.isCollection;
                meta.isRegularFile = !ri.isCollection;
                meta.size = ri.contentLength;
                meta.readable = true; meta.writable = false; meta.executable = false;
                meta.lastModified = ri.lastModified;
                if (ri.contentType) meta.mimeType = *ri.contentType;
                de.metadata = std::move(meta);

                entries.push_back(std::move(de));
            }

            s.directoryEntries = std::move(entries);
            s.complete(FileOpStatus::Complete);
        } catch (const std::exception& e) {
            s.setError(FileError::NetworkError, e.what(), p);
            s.complete(FileOpStatus::Failed);
        }
    });
}

std::unique_ptr<FileStream> WebDAVFileSystemBackend::openStream(const std::string& path, StreamOptions options) {
    try {
        if (options.mode != StreamOptions::Read) {
            return nullptr;
        }

        const std::string url = buildUrl(path);
        const size_t bufferBytes = options.bufferSize > 0 ? options.bufferSize : (64u * 1024u);

        // Prepare HTTP GET request
        HTTP::HttpRequest req;
        req.method = HTTP::HttpMethod::GET;
        req.scheme = _cfg.scheme;
        req.host = _cfg.host;
        req.path = url;
        if (!_cfg.authHeader.empty()) {
            req.headers["Authorization"] = _cfg.authHeader;
        }

        // Configure streaming options
        HTTP::StreamOptions streamOpts;
        streamOpts.bufferBytes = bufferBytes;
        streamOpts.maxBodyBytes = 0;  // Unlimited for streaming
        streamOpts.connectTimeout = std::chrono::milliseconds(10000);
        streamOpts.totalDeadline = std::chrono::milliseconds(0);  // No total deadline for streaming

        // Execute streaming request
        auto handle = _client.executeStream(req, streamOpts);

        // Create WebDAVReadStream wrapper
        return std::make_unique<WebDAVReadStream>(std::move(handle), url);
    } catch (...) {
        return nullptr;
    }
}

FileOperationHandle WebDAVFileSystemBackend::readLine(const std::string& path, size_t lineNumber) {
    (void)path; (void)lineNumber;
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle WebDAVFileSystemBackend::writeLine(const std::string& path, size_t lineNumber, std::string_view line) {
    (void)path; (void)lineNumber; (void)line;
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

BackendCapabilities WebDAVFileSystemBackend::getCapabilities() const {
    BackendCapabilities caps;
    caps.supportsStreaming = true; // streaming via openStream
    caps.supportsRandomAccess = false;
    caps.supportsDirectories = true;
    caps.supportsMetadata = true;
    caps.supportsAtomicWrites = false;
    caps.supportsWatching = false;
    caps.isRemote = true;
    caps.maxFileSize = SIZE_MAX;
    return caps;
}

} // namespace EntropyEngine::Networking::WebDAV
