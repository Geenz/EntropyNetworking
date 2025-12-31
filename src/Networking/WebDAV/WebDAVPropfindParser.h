/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file WebDAVPropfindParser.h
 * @brief Parser for WebDAV PROPFIND responses
 *
 * This file contains utilities for parsing WebDAV PROPFIND (RFC 4918) XML responses
 * into structured resource information.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace tinyxml2
{
class XMLElement;
}

namespace EntropyEngine::Networking::WebDAV
{

/**
 * @brief Resource information from WebDAV PROPFIND response
 *
 * Represents a single resource (file or directory) returned by PROPFIND.
 * Extracted from DAV:response elements in the XML.
 */
struct DavResourceInfo
{
    std::string href;            ///< Raw href from response (may be absolute or relative)
    bool isCollection = false;   ///< true if DAV:resourcetype contains DAV:collection
    uint64_t contentLength = 0;  ///< File size (0 for collections or when property missing)
    std::optional<std::chrono::system_clock::time_point> lastModified;  ///< Last modification time (nullopt if missing)
    std::optional<std::string> contentType;                             ///< MIME type (nullopt if missing)
};

/**
 * @brief Parses WebDAV PROPFIND response body (207 Multistatus)
 *
 * Parses RFC 4918 compliant WebDAV PROPFIND XML responses. Extracts resource
 * properties from DAV:response elements. Namespace-tolerant (matches by local
 * name, ignores prefixes). Prefers propstat with HTTP 200 status.
 *
 * @param xmlBytes Raw XML response body from HTTP 207 response
 * @return Vector of resource info in document order, or empty on parse error
 *
 * @code
 * auto response = conn->propfind("/dav/folder", 1, propfindXml);
 * if (response.statusCode == 207) {
 *     auto resources = parsePropfindXml(response.body);
 *     for (const auto& res : resources) {
 *         if (res.isCollection) {
 *             processDirectory(res.href);
 *         } else {
 *             processFile(res.href, res.contentLength);
 *         }
 *     }
 * }
 * @endcode
 */
std::vector<DavResourceInfo> parsePropfindXml(const std::vector<uint8_t>& xmlBytes);

}  // namespace EntropyEngine::Networking::WebDAV
