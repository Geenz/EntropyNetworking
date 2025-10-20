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
#include <vector>
#include <optional>
#include <chrono>

namespace tinyxml2 { class XMLElement; }

namespace EntropyEngine::Networking::WebDAV {

struct DavResourceInfo {
    std::string href;                                     // Raw href from response
    bool isCollection = false;                            // true if DAV:resourcetype has <collection/>
    uint64_t contentLength = 0;                           // 0 for collections or when missing
    std::optional<std::chrono::system_clock::time_point> lastModified; // nullopt if missing
    std::optional<std::string> contentType;               // nullopt if missing
};

// Parse a WebDAV PROPFIND response body (XML) and return all resources in document order.
// - Namespace tolerant: matches by local-name, ignores prefixes.
// - Prefers propstat with HTTP 200 status; falls back to first <prop> if needed.
// Returns empty vector on parse errors.
std::vector<DavResourceInfo> parsePropfindXml(const std::vector<uint8_t>& xmlBytes);

} // namespace EntropyEngine::Networking::WebDAV
