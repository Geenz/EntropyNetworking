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
#include <optional>
#include <chrono>

namespace EntropyEngine::Networking::WebDAV::Utils {

// Percent-encode a path, preserving '/' when keepSlashes=true
std::string percentEncode(std::string_view s, bool keepSlashes = true);

// Percent-decode; returns empty optional on malformed sequences
std::optional<std::string> percentDecode(std::string_view s);

// Strip scheme and authority (http[s]://host[:port]) from href
std::string stripSchemeHost(std::string_view href);

// Normalize href for comparison: strip scheme/host, drop query/fragment, collapse multiple '/'
std::string normalizeHrefForCompare(const std::string& href, bool ensureTrailingSlashIfCollection = false);

// Parse IMF-fixdate (RFC 7231); returns nullopt on failure
std::optional<std::chrono::system_clock::time_point> parseHttpDate(const char* s);

} // namespace EntropyEngine::Networking::WebDAV::Utils
