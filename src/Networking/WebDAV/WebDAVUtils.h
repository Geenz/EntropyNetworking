/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file WebDAVUtils.h
 * @brief Utility functions for WebDAV URL and date handling
 *
 * This file contains utility functions for percent encoding/decoding,
 * URL normalization, and HTTP date parsing used throughout the WebDAV implementation.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace EntropyEngine::Networking::WebDAV::Utils
{

/**
 * @brief Percent-encodes a string for use in URLs
 *
 * Encodes unsafe characters as %XX. When keepSlashes is true, '/' characters
 * are preserved (useful for path components). Follows RFC 3986.
 *
 * @param s String to encode
 * @param keepSlashes If true, preserves '/' characters unencoded
 * @return Percent-encoded string
 *
 * @code
 * auto encoded = percentEncode("my file.txt");  // "my%20file.txt"
 * auto path = percentEncode("path/to/file", true);  // "path/to/file" (slashes preserved)
 * @endcode
 */
std::string percentEncode(std::string_view s, bool keepSlashes = true);

/**
 * @brief Percent-decodes a URL-encoded string
 *
 * Decodes %XX sequences back to original characters. Returns nullopt
 * if string contains malformed encoding (e.g., %ZZ, incomplete %X).
 *
 * @param s String to decode
 * @return Decoded string, or nullopt if malformed
 *
 * @code
 * auto decoded = percentDecode("my%20file.txt");  // "my file.txt"
 * auto bad = percentDecode("invalid%ZZ");          // nullopt
 * @endcode
 */
std::optional<std::string> percentDecode(std::string_view s);

/**
 * @brief Strips scheme and authority from URL
 *
 * Removes http[s]://host[:port] prefix, leaving only path and beyond.
 * Used to extract path from absolute WebDAV hrefs.
 *
 * @param href URL with scheme and host
 * @return Path portion (e.g., "/dav/file.txt")
 *
 * @code
 * auto path = stripSchemeHost("https://example.com/dav/file.txt");  // "/dav/file.txt"
 * @endcode
 */
std::string stripSchemeHost(std::string_view href);

/**
 * @brief Normalizes href for string comparison
 *
 * Performs multiple normalizations:
 * - Strips scheme and host
 * - Drops query string and fragment
 * - Collapses multiple consecutive '/' to single '/'
 * - Optionally adds trailing '/' for collections
 *
 * Used to match hrefs from WebDAV responses against expected paths.
 *
 * @param href URL to normalize
 * @param ensureTrailingSlashIfCollection If true, adds trailing '/' if not present
 * @return Normalized path for comparison
 *
 * @code
 * auto norm = normalizeHrefForCompare("http://example.com/dav//folder?q=1");  // "/dav/folder"
 * auto dir = normalizeHrefForCompare("/folder", true);  // "/folder/"
 * @endcode
 */
std::string normalizeHrefForCompare(const std::string& href, bool ensureTrailingSlashIfCollection = false);

/**
 * @brief Parses HTTP date header (IMF-fixdate format)
 *
 * Parses dates in IMF-fixdate format per RFC 7231 (e.g., "Mon, 15 Jan 2024 12:30:00 GMT").
 * Used for parsing Last-Modified headers from WebDAV responses.
 *
 * @param s Date string in IMF-fixdate format
 * @return Time point, or nullopt if parsing failed
 *
 * @code
 * auto date = parseHttpDate("Mon, 15 Jan 2024 12:30:00 GMT");
 * if (date) {
 *     auto time_t = std::chrono::system_clock::to_time_t(*date);
 *     // ...
 * }
 * @endcode
 */
std::optional<std::chrono::system_clock::time_point> parseHttpDate(const char* s);

}  // namespace EntropyEngine::Networking::WebDAV::Utils
