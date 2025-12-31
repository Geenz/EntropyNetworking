#include "Networking/HTTP/Proxy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "Winhttp.lib")
#endif

#if defined(__APPLE__)
#include <CFNetwork/CFNetwork.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#endif

namespace EntropyEngine::Networking::HTTP
{

static inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static inline std::string hostKey(const std::string& scheme, const std::string& host, uint16_t port) {
    std::ostringstream o;
    o << scheme << "://" << host << ":" << port;
    return o.str();
}

// ---------------- EnvProxyResolver ----------------
std::optional<std::string> EnvProxyResolver::getEnv(const char* name) {
    if (const char* v = std::getenv(name)) return std::string(v);
    return std::nullopt;
}

static std::vector<std::string> splitCommaList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, ',')) {
        // trim
        size_t a = 0, b = cur.size();
        while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
        while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) --b;
        if (b > a) out.push_back(cur.substr(a, b - a));
    }
    return out;
}

static bool matchHostSuffix(const std::string& host, const std::string& pattern) {
    std::string h = toLower(host), p = toLower(pattern);
    if (p == "*") return true;
    if (!p.empty() && p.front() == '.') {
        if (h == p.substr(1)) return true;
        if (h.size() > p.size() && h.rfind(p, h.size() - p.size()) == h.size() - p.size()) return true;
        return false;
    }
    if (h.size() < p.size()) return false;
    return h.compare(h.size() - p.size(), p.size(), p) == 0;
}

bool EnvProxyResolver::isBypassedByNoProxy(const std::string& /*hostOnly*/, const std::string& host,
                                           uint16_t /*port*/) {
    auto noProxy = getEnv("NO_PROXY");
    if (!noProxy) noProxy = getEnv("no_proxy");
    if (!noProxy) return false;
    for (const auto& pat : splitCommaList(*noProxy)) {
        if (pat.empty()) continue;
        if (matchHostSuffix(host, pat)) return true;
    }
    return false;
}

ProxyResult EnvProxyResolver::resolve(const std::string& scheme, const std::string& host, uint16_t port) {
    std::string hostOnly = host;
    auto colon = hostOnly.rfind(':');
    if (colon != std::string::npos && hostOnly.find(':') == colon) hostOnly = hostOnly.substr(0, colon);
    if (isBypassedByNoProxy(hostOnly, hostOnly, port)) return ProxyResult{ProxyResult::Type::Direct, {}};

    std::optional<std::string> proxy;
    if (scheme == "https") {
        proxy = getEnv("HTTPS_PROXY");
        if (!proxy) proxy = getEnv("https_proxy");
        if (!proxy) {
            proxy = getEnv("HTTP_PROXY");
            if (!proxy) proxy = getEnv("http_proxy");
        }
    } else {
        proxy = getEnv("HTTP_PROXY");
        if (!proxy) proxy = getEnv("http_proxy");
    }
    if (!proxy || proxy->empty()) return ProxyResult{ProxyResult::Type::Direct, {}};

    std::string url = *proxy;
    if (url.find("://") == std::string::npos) url = std::string("http://") + url;
    std::string l = toLower(url);
    ProxyResult::Type t = ProxyResult::Type::Http;
    if (l.rfind("socks5://", 0) == 0)
        t = ProxyResult::Type::Socks5;
    else if (l.rfind("socks4://", 0) == 0)
        t = ProxyResult::Type::Socks4;
    else
        t = ProxyResult::Type::Http;
    return ProxyResult{t, url};
}

// ---------------- SystemProxyResolver (Windows/non-Windows) ----------------
ProxyResult SystemProxyResolver::resolve(const std::string& scheme, const std::string& host, uint16_t port) {
#if defined(_WIN32)
    // Compose URL
    std::ostringstream ou;
    ou << scheme << "://" << host;
    // Append port if provided in host is absent and non-default
    bool hostHasPort = (host.rfind(':') != std::string::npos) && (host.find(':') == host.rfind(':'));
    if (!hostHasPort) {
        if (scheme == "http" && port != 80) ou << ":" << port;
        if (scheme == "https" && port != 443) ou << ":" << port;
    }
    std::string url = ou.str();

    // Convert to wide
    int need = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wurl;
    wurl.resize(need ? (need - 1) : 0);
    if (need > 0) MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wurl[0], need);

    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG iecfg{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&iecfg)) {
        return ProxyResult{ProxyResult::Type::Direct, {}};
    }

    HINTERNET hSession = WinHttpOpen(L"EntropyHTTP/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (iecfg.lpszAutoConfigUrl) GlobalFree(iecfg.lpszAutoConfigUrl);
        if (iecfg.lpszProxy) GlobalFree(iecfg.lpszProxy);
        if (iecfg.lpszProxyBypass) GlobalFree(iecfg.lpszProxyBypass);
        return ProxyResult{ProxyResult::Type::Direct, {}};
    }

    WINHTTP_AUTOPROXY_OPTIONS apo{};
    apo.fAutoLogonIfChallenged = TRUE;
    WINHTTP_PROXY_INFO pi{};

    bool success = false;

    if (iecfg.lpszAutoConfigUrl) {
        apo.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
        apo.lpszAutoConfigUrl = iecfg.lpszAutoConfigUrl;
        success = WinHttpGetProxyForUrl(hSession, wurl.c_str(), &apo, &pi);
    }

    if (!success && iecfg.fAutoDetect) {
        ZeroMemory(&apo, sizeof(apo));
        apo.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
        apo.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        success = WinHttpGetProxyForUrl(hSession, wurl.c_str(), &apo, &pi);
    }

    ProxyResult out{ProxyResult::Type::Direct, {}};

    if (success) {
        if (pi.lpszProxy && pi.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY) {
            std::wstring wprox(pi.lpszProxy);
            auto sc = wprox.find(L';');
            if (sc != std::wstring::npos) wprox.resize(sc);
            int need2 = WideCharToMultiByte(CP_UTF8, 0, wprox.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string proxy;
            proxy.resize(need2 ? (need2 - 1) : 0);
            if (need2 > 0) WideCharToMultiByte(CP_UTF8, 0, wprox.c_str(), -1, proxy.data(), need2, nullptr, nullptr);
            if (!proxy.empty()) {
                if (proxy.find("://") == std::string::npos) proxy = std::string("http://") + proxy;
                std::string lp = toLower(proxy);
                if (lp.rfind("socks5://", 0) == 0)
                    out.type = ProxyResult::Type::Socks5;
                else if (lp.rfind("socks4://", 0) == 0)
                    out.type = ProxyResult::Type::Socks4;
                else
                    out.type = ProxyResult::Type::Http;
                out.url = proxy;
            }
        }
        if (pi.lpszProxyBypass) GlobalFree(pi.lpszProxyBypass);
        if (pi.lpszProxy) GlobalFree(pi.lpszProxy);
    } else if (iecfg.lpszProxy) {
        std::wstring wprox(iecfg.lpszProxy);
        int need2 = WideCharToMultiByte(CP_UTF8, 0, wprox.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string proxy;
        proxy.resize(need2 ? (need2 - 1) : 0);
        if (need2 > 0) WideCharToMultiByte(CP_UTF8, 0, wprox.c_str(), -1, proxy.data(), need2, nullptr, nullptr);
        if (!proxy.empty()) {
            if (proxy.find("://") == std::string::npos) proxy = std::string("http://") + proxy;
            std::string lp = toLower(proxy);
            ProxyResult::Type t = ProxyResult::Type::Http;
            if (lp.rfind("socks5://", 0) == 0)
                t = ProxyResult::Type::Socks5;
            else if (lp.rfind("socks4://", 0) == 0)
                t = ProxyResult::Type::Socks4;
            out = ProxyResult{t, proxy};
        }
    }

    WinHttpCloseHandle(hSession);
    if (iecfg.lpszAutoConfigUrl) GlobalFree(iecfg.lpszAutoConfigUrl);
    if (iecfg.lpszProxy) GlobalFree(iecfg.lpszProxy);
    if (iecfg.lpszProxyBypass) GlobalFree(iecfg.lpszProxyBypass);

    return out;
#elif defined(__APPLE__)
    // macOS system proxy resolution using CFNetwork
    // Build a CFURL for the destination
    std::ostringstream ou;
    ou << scheme << "://" << host;
    bool hostHasPort = (host.rfind(':') != std::string::npos) && (host.find(':') == host.rfind(':'));
    if (!hostHasPort) {
        if (scheme == "http" && port != 80) ou << ":" << port;
        if (scheme == "https" && port != 443) ou << ":" << port;
        if (scheme != "http" && scheme != "https" && port != 0) ou << ":" << port;
    }
    std::string url = ou.str();

    auto cfStrFrom = [](const std::string& s) {
        return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()), s.size(),
                                       kCFStringEncodingUTF8, false);
    };

    CFStringRef urlStr = cfStrFrom(url);
    if (!urlStr) return ProxyResult{ProxyResult::Type::Direct, {}};
    CFURLRef cfUrl = CFURLCreateWithString(nullptr, urlStr, nullptr);
    CFRelease(urlStr);
    if (!cfUrl) return ProxyResult{ProxyResult::Type::Direct, {}};

    CFDictionaryRef sys = CFNetworkCopySystemProxySettings();
    if (!sys) {
        CFRelease(cfUrl);
        return ProxyResult{ProxyResult::Type::Direct, {}};
    }

    CFArrayRef arr = CFNetworkCopyProxiesForURL(cfUrl, sys);
    CFRelease(cfUrl);
    CFRelease(sys);
    if (!arr) return ProxyResult{ProxyResult::Type::Direct, {}};

    ProxyResult out{ProxyResult::Type::Direct, {}};
    CFIndex n = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < n; ++i) {
        CFDictionaryRef prx = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        if (!prx) continue;
        CFStringRef typeRef = (CFStringRef)CFDictionaryGetValue(prx, kCFProxyTypeKey);
        if (!typeRef) continue;
        CFStringRef hostRef = (CFStringRef)CFDictionaryGetValue(prx, kCFProxyHostNameKey);
        CFNumberRef portRef = (CFNumberRef)CFDictionaryGetValue(prx, kCFProxyPortNumberKey);

        // Determine type
        if (CFStringCompare(typeRef, kCFProxyTypeNone, 0) == kCFCompareEqualTo) {
            out = ProxyResult{ProxyResult::Type::Direct, {}};
            break;
        } else if (CFStringCompare(typeRef, kCFProxyTypeHTTP, 0) == kCFCompareEqualTo ||
                   CFStringCompare(typeRef, kCFProxyTypeHTTPS, 0) == kCFCompareEqualTo ||
                   CFStringCompare(typeRef, kCFProxyTypeHTTP, kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            if (hostRef && portRef) {
                int p = 0;
                CFNumberGetValue(portRef, kCFNumberIntType, &p);
                // Build URL
                char hostBuf[512] = {0};
                CFStringGetCString(hostRef, hostBuf, sizeof(hostBuf), kCFStringEncodingUTF8);
                std::ostringstream po;
                po << "http://" << hostBuf << ":" << p;
                out = ProxyResult{ProxyResult::Type::Http, po.str()};
                break;
            }
        } else if (CFStringCompare(typeRef, kCFProxyTypeSOCKS, 0) == kCFCompareEqualTo) {
            if (hostRef && portRef) {
                int p = 0;
                CFNumberGetValue(portRef, kCFNumberIntType, &p);
                char hostBuf[512] = {0};
                CFStringGetCString(hostRef, hostBuf, sizeof(hostBuf), kCFStringEncodingUTF8);
                std::ostringstream po;
                po << "socks5://" << hostBuf << ":" << p;
                out = ProxyResult{ProxyResult::Type::Socks5, po.str()};
                break;
            }
        }
    }
    CFRelease(arr);
    return out;
#else
    (void)scheme;
    (void)host;
    (void)port;
    return ProxyResult{ProxyResult::Type::Direct, {}};
#endif
}

// ---------------- DefaultProxyResolver ----------------
DefaultProxyResolver::DefaultProxyResolver() = default;

void DefaultProxyResolver::setUseSystemProxy(bool enabled) {
    _useSystemProxy.store(enabled, std::memory_order_release);
}

ProxyResult DefaultProxyResolver::resolve(const std::string& scheme, const std::string& host, uint16_t port) {
    const std::string key = hostKey(scheme, host, port);
    {
        std::lock_guard<std::mutex> lk(_m);
        auto it = _cache.find(key);
        if (it != _cache.end() && std::chrono::steady_clock::now() < it->second.expires) {
            return it->second.res;
        }
    }

    // 1) Env-based decision (honors NO_PROXY and HTTP[S]_PROXY)
    ProxyResult res = _env.resolve(scheme, host, port);

    // 2) If env said Direct, optionally fall back to system proxy.
    if (res.type == ProxyResult::Type::Direct) {
        bool allowSystem = _useSystemProxy.load(std::memory_order_acquire);

        // Back-compat: ENTROPY_HTTP_USE_SYSTEM_PROXY can force-enable/disable.
        if (const char* env = std::getenv("ENTROPY_HTTP_USE_SYSTEM_PROXY")) {
            std::string v = toLower(std::string(env));
            if (v == "0" || v == "false" || v == "off") {
                allowSystem = false;
            } else if (v == "1" || v == "true" || v == "on") {
                allowSystem = true;
            }
        }

        if (allowSystem) {
            res = _sys.resolve(scheme, host, port);
        }
    }

    {
        std::lock_guard<std::mutex> lk(_m);
        _cache[key] = CacheEntry{res, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
    }
    return res;
}

}  // namespace EntropyEngine::Networking::HTTP
