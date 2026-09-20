/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */
#include "Networking/WebDAV/WebDAVUtils.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace EntropyEngine::Networking::WebDAV::Utils
{

std::string percentEncode(std::string_view s, bool keepSlashes) {
    auto isUnreserved = [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~'; };
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isUnreserved(c) || (keepSlashes && c == '/')) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02X", c);
            out.push_back('%');
            out.append(buf, buf + 2);
        }
    }
    return out;
}

std::optional<std::string> percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == '%') {
            if (i + 2 >= s.size()) return std::nullopt;
            auto h1 = s[i + 1];
            auto h2 = s[i + 2];
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                return -1;
            };
            int a = hex(h1), b = hex(h2);
            if (a < 0 || b < 0) return std::nullopt;
            out.push_back(static_cast<char>((a << 4) | b));
            i += 2;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string stripSchemeHost(std::string_view href) {
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) {
        auto pos = href.find('/', href.find("//") + 2);
        if (pos == std::string_view::npos) return std::string("/");
        return std::string(href.substr(pos));
    }
    return std::string(href);
}

std::string normalizeHrefForCompare(const std::string& href, bool ensureTrailingSlashIfCollection) {
    auto path = stripSchemeHost(href);
    // strip query/fragment
    auto q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);
    // Collapse multiple '/'
    std::string norm;
    norm.reserve(path.size());
    bool prevSlash = false;
    for (char c : path) {
        if (c == '/') {
            if (!prevSlash) norm.push_back(c);
            prevSlash = true;
        } else {
            norm.push_back(c);
            prevSlash = false;
        }
    }
    if (ensureTrailingSlashIfCollection && !norm.empty() && norm.back() != '/') norm.push_back('/');
    return norm;
}

std::optional<std::chrono::system_clock::time_point> parseHttpDate(const char* s) {
    if (!s) return std::nullopt;
    // IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
    std::tm tm{};
    char wk[4]{};
    char mon[4]{};
    char tz[8]{};
    int d = 0, y = 0, H = 0, M = 0, S = 0;
    if (std::sscanf(s, "%3s, %d %3s %d %d:%d:%d %7s", wk, &d, mon, &y, &H, &M, &S, tz) != 8) return std::nullopt;
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* m = std::strstr(months, mon);
    if (!m) return std::nullopt;
    int mi = int((m - months) / 3);
    tm.tm_mday = d;
    tm.tm_year = y - 1900;
    tm.tm_hour = H;
    tm.tm_min = M;
    tm.tm_sec = S;
    tm.tm_mon = mi;
    tm.tm_isdst = 0;
#ifdef _WIN32
    time_t tt = _mkgmtime(&tm);
#else
    time_t tt = timegm(&tm);
#endif
    if (tt == -1) return std::nullopt;
    return std::chrono::system_clock::from_time_t(tt);
}

}  // namespace EntropyEngine::Networking::WebDAV::Utils
