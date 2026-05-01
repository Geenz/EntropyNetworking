/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "WebDavAdapter.h"

#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <sstream>

using namespace EntropyEngine::Core::IO;

namespace EntropyEngine::Networking
{

static bool iequal_prefix(std::string_view a, std::string_view b) {
    if (b.size() > a.size()) return false;
    for (size_t i = 0; i < b.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool WebDavAdapter::handles(const std::string& urlPath) const {
    // Simple prefix match; compare case-sensitively to keep canonical pathing
    return urlPath.rfind(_mountPrefix, 0) == 0;
}

std::optional<std::string> WebDavAdapter::toVfsPath(const std::string& urlPath) const {
    if (!handles(urlPath)) return std::nullopt;
    auto sub = urlPath.substr(_mountPrefix.size());
    // Avoid going outside mount: reject attempts to traverse above root
    // (Very basic normalization; a production server should apply full canonicalization.)
    if (sub.find("..") != std::string::npos) return std::nullopt;
    return trimLeadingSlash(sub);
}

HttpResponseLite WebDavAdapter::handleOptions(const HttpRequestLite& req) const {
    (void)req;
    HttpResponseLite res;
    res.status = 200;
    res.headers["DAV"] = "1,2";  // advertise class 1 and 2 (even if write ops not implemented yet)
    res.headers["Allow"] = "OPTIONS, PROPFIND, GET, HEAD";
    res.headers["Accept-Ranges"] = "bytes";
    return res;
}

HttpResponseLite WebDavAdapter::handlePropfind(const HttpRequestLite& req, int depth) const {
    HttpResponseLite res;

    // Resolve to VFS path with basic traversal protection
    auto vfsPathOpt = toVfsPath(req.urlPath);
    if (!vfsPathOpt) {
        res.status = 400;
        return res;
    }
    std::string vfsPath = *vfsPathOpt;

    // Determine requested Depth: header (prefer header over parameter)
    int reqDepth = depth;
    for (const auto& kv : req.headers) {
        const std::string& k = kv.first;
        if (k.size() == 5 && (k == "Depth" || k == "depth" || k == "DEPTH")) {
            const std::string& v = kv.second;
            if (v == "0")
                reqDepth = 0;
            else if (v == "1")
                reqDepth = 1;
            else if (v == "infinity" || v == "Infinity" || v == "INFINITY") {
                // Depth: infinity not supported in MVP
                res.status = 400;
                res.headers["Content-Type"] = "application/xml; charset=utf-8";
                res.body = "<error>Depth infinity not supported</error>";
                return res;
            }
            break;
        }
    }

    // Determine if target is a collection (by trailing slash heuristic and metadata)
    bool targetIsDir = !vfsPath.empty() && vfsPath.back() == '/';

    // Prepare XML document
    tinyxml2::XMLDocument doc;
    auto* decl = doc.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\"");
    doc.InsertFirstChild(decl);
    auto* multistatus = doc.NewElement("D:multistatus");
    multistatus->SetAttribute("xmlns:D", "DAV:");
    doc.InsertEndChild(multistatus);

    auto httpDate = [](const std::optional<std::chrono::system_clock::time_point>& tp) -> std::string {
        if (!tp) return {};
        std::time_t t = std::chrono::system_clock::to_time_t(*tp);
        char buf[64]{};
        std::tm g{};
#ifdef _WIN32
        gmtime_s(&g, &t);
#else
        g = *std::gmtime(&t);
#endif
        std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
        return buf;
    };

    auto addResponse = [&](const std::string& href, const std::string& displayName, bool isDir, uint64_t size,
                           const std::optional<std::chrono::system_clock::time_point>& mtime,
                           const std::optional<std::string>& contentType) {
        auto* resp = doc.NewElement("D:response");
        multistatus->InsertEndChild(resp);
        auto* hrefEl = doc.NewElement("D:href");
        hrefEl->SetText(href.c_str());
        resp->InsertEndChild(hrefEl);
        auto* propstat = doc.NewElement("D:propstat");
        resp->InsertEndChild(propstat);
        auto* prop = doc.NewElement("D:prop");
        propstat->InsertEndChild(prop);
        auto* disp = doc.NewElement("D:displayname");
        disp->SetText(displayName.c_str());
        prop->InsertEndChild(disp);
        auto* rt = doc.NewElement("D:resourcetype");
        if (isDir) {
            auto* coll = doc.NewElement("D:collection");
            rt->InsertEndChild(coll);
        }
        prop->InsertEndChild(rt);
        if (!isDir) {
            auto* gcl = doc.NewElement("D:getcontentlength");
            gcl->SetText(std::to_string(size).c_str());
            prop->InsertEndChild(gcl);
        }
        if (mtime.has_value()) {
            auto* glm = doc.NewElement("D:getlastmodified");
            auto d = httpDate(mtime);
            if (!d.empty()) glm->SetText(d.c_str());
            prop->InsertEndChild(glm);
        }
        if (contentType.has_value()) {
            auto* gct = doc.NewElement("D:getcontenttype");
            gct->SetText(contentType->c_str());
            prop->InsertEndChild(gct);
        }
        auto* status = doc.NewElement("D:status");
        status->SetText("HTTP/1.1 200 OK");
        propstat->InsertEndChild(status);
    };

    // Self entry
    auto nameFromPath = [](const std::string& p) -> std::string {
        if (p.empty()) return std::string{};
        std::string s = p;
        if (!s.empty() && s.back() == '/') s.pop_back();
        auto pos = s.find_last_of('/');
        return (pos == std::string::npos) ? s : s.substr(pos + 1);
    };

    // Probe filesystem to refine isDir and attributes
    std::optional<FileMetadata> selfMeta;
    if (targetIsDir) {
        auto dh = _vfs->createDirectoryHandle(vfsPath);
        auto op = dh.getMetadata();
        op.wait();
        if (op.status() == FileOpStatus::Complete && op.metadata().has_value()) {
            selfMeta = *op.metadata();
            targetIsDir = selfMeta->isDirectory;
        }
    } else {
        auto fh = _vfs->createFileHandle(vfsPath);
        // FileHandle::metadata() is a fast snapshot (exists/size)
        auto m = fh.metadata();
        selfMeta = FileMetadata{};
        selfMeta->path = vfsPath;
        selfMeta->exists = m.exists;
        selfMeta->size = m.size;
        selfMeta->isRegularFile = m.exists;
    }

    // Build self href (use incoming URL path as canonical href)
    std::string selfHref = req.urlPath;
    if (targetIsDir && (selfHref.empty() || selfHref.back() != '/')) selfHref.push_back('/');
    std::string selfName = nameFromPath(vfsPath);
    uint64_t selfSize = (selfMeta && selfMeta->isRegularFile) ? static_cast<uint64_t>(selfMeta->size) : 0ull;
    std::optional<std::string> selfType =
        (selfMeta && selfMeta->mimeType.has_value()) ? selfMeta->mimeType : std::optional<std::string>{};
    addResponse(selfHref, selfName, targetIsDir, selfSize,
                selfMeta ? selfMeta->lastModified : std::optional<std::chrono::system_clock::time_point>{}, selfType);

    // Depth: 1 listing for directories
    if (reqDepth >= 1 && targetIsDir) {
        auto dh = _vfs->createDirectoryHandle(vfsPath);
        auto listOp = dh.list({});
        listOp.wait();
        if (listOp.status() == FileOpStatus::Complete) {
            for (const auto& e : listOp.directoryEntries()) {
                std::string href = selfHref + e.name + (e.metadata.isDirectory ? "/" : "");
                std::string disp = e.name;
                uint64_t len = e.metadata.isRegularFile ? static_cast<uint64_t>(e.metadata.size) : 0ull;
                addResponse(href, disp, e.metadata.isDirectory, len, e.metadata.lastModified, e.metadata.mimeType);
            }
        }
    }

    tinyxml2::XMLPrinter printer(nullptr, /*compact*/ false, /*depth*/ 0);
    doc.Print(&printer);

    res.status = 207;
    res.headers["Content-Type"] = "application/xml; charset=utf-8";
    res.body.assign(printer.CStr(), printer.CStrSize() ? printer.CStrSize() - 1 : 0);
    return res;
}

HttpResponseLite WebDavAdapter::handleHead(const HttpRequestLite& req) const {
    HttpResponseLite res;
    auto vfsPathOpt = toVfsPath(req.urlPath);
    if (!vfsPathOpt) {
        res.status = 400;
        return res;
    }
    const std::string& vfsPath = *vfsPathOpt;

    auto fh = _vfs->createFileHandle(vfsPath);
    const auto meta = fh.metadata();
    if (!meta.exists) {
        // If this is a directory (trailing slash), do a minimal dir existence check
        if (!vfsPath.empty() && vfsPath.back() == '/') {
            auto dh = _vfs->createDirectoryHandle(vfsPath);
            auto op = dh.getMetadata();
            op.wait();
            if (op.status() == FileOpStatus::Complete && op.metadata().has_value() && op.metadata()->exists &&
                op.metadata()->isDirectory) {
                res.status = 405;  // Method Not Allowed for collections
                res.headers["Allow"] = "PROPFIND, OPTIONS";
                return res;
            }
        }
        res.status = 404;
        return res;
    }

    res.status = 200;
    res.headers["Content-Length"] = std::to_string(meta.size);
    res.headers["Content-Type"] = guessContentType(vfsPath);
    res.headers["Accept-Ranges"] = "bytes";
    return res;
}

HttpResponseLite WebDavAdapter::handleGet(const HttpRequestLite& req) const {
    HttpResponseLite res;
    auto vfsPathOpt = toVfsPath(req.urlPath);
    if (!vfsPathOpt) {
        res.status = 400;
        return res;
    }
    const std::string& vfsPath = *vfsPathOpt;

    // Directory GET not supported (use PROPFIND)
    if (!vfsPath.empty() && vfsPath.back() == '/') {
        // Check existence for better error mapping
        auto dh = _vfs->createDirectoryHandle(vfsPath);
        auto op = dh.getMetadata();
        op.wait();
        if (op.status() == FileOpStatus::Complete && op.metadata().has_value() && op.metadata()->exists &&
            op.metadata()->isDirectory) {
            res.status = 405;  // Method Not Allowed
            res.headers["Allow"] = "PROPFIND, OPTIONS";
        } else {
            res.status = 404;
        }
        return res;
    }

    auto fh = _vfs->createFileHandle(vfsPath);
    const auto meta = fh.metadata();
    if (!meta.exists) {
        res.status = 404;
        return res;
    }

    auto op = fh.readAll();
    op.wait();
    if (op.status() != FileOpStatus::Complete && op.status() != FileOpStatus::Partial) {
        res.status = 500;
        return res;
    }

    auto bytes = op.contentsBytes();
    res.status = 200;
    res.headers["Content-Type"] = guessContentType(vfsPath);
    res.headers["Content-Length"] = std::to_string(bytes.size());
    // Store as string; this is binary-unsafe but sufficient for a compile-ready adapter skeleton.
    res.body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return res;
}

std::string WebDavAdapter::trimLeadingSlash(std::string s) {
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

std::string WebDavAdapter::guessContentType(std::string_view path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    auto ext = std::string(path.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "txt" || ext == "log" || ext == "csv") return "text/plain; charset=utf-8";
    if (ext == "json") return "application/json";
    if (ext == "xml") return "application/xml";
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "mp4") return "video/mp4";
    if (ext == "wasm") return "application/wasm";
    if (ext == "js") return "application/javascript";
    if (ext == "css") return "text/css; charset=utf-8";
    return "application/octet-stream";
}

std::string WebDavAdapter::buildMinimalMultistatus(const std::string& selfHref) {
    tinyxml2::XMLDocument doc;

    // XML declaration
    auto* decl = doc.NewDeclaration("xml version=\"1.0\" encoding=\"utf-8\"");
    doc.InsertFirstChild(decl);

    // <D:multistatus xmlns:D="DAV:">
    auto* multistatus = doc.NewElement("D:multistatus");
    multistatus->SetAttribute("xmlns:D", "DAV:");
    doc.InsertEndChild(multistatus);

    // <D:response>
    auto* response = doc.NewElement("D:response");
    multistatus->InsertEndChild(response);

    // <D:href>selfHref</D:href>
    auto* href = doc.NewElement("D:href");
    href->SetText(selfHref.c_str());
    response->InsertEndChild(href);

    // <D:propstat>
    auto* propstat = doc.NewElement("D:propstat");
    response->InsertEndChild(propstat);

    // <D:prop>
    auto* prop = doc.NewElement("D:prop");
    propstat->InsertEndChild(prop);

    // <D:resourcetype/>
    auto* resourcetype = doc.NewElement("D:resourcetype");
    prop->InsertEndChild(resourcetype);

    // <D:status>HTTP/1.1 200 OK</D:status>
    auto* status = doc.NewElement("D:status");
    status->SetText("HTTP/1.1 200 OK");
    propstat->InsertEndChild(status);

    tinyxml2::XMLPrinter printer(nullptr, /*compact*/ false, /*depth*/ 0);
    doc.Print(&printer);

    // printer.CStrSize() includes the terminating null
    return std::string(printer.CStr(), printer.CStrSize() - 1);
}

}  // namespace EntropyEngine::Networking
