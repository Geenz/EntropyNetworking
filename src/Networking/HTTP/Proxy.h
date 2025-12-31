#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace EntropyEngine::Networking::HTTP
{

struct ProxyResult
{
    enum class Type
    {
        Direct,
        Http,
        Socks4,
        Socks5
    };
    Type type = Type::Direct;
    std::string url;  // full proxy URL, e.g., "http://proxy:8080" or "socks5://proxy:1080"
};

// Interface for proxy auto-detection
class ProxyResolver
{
public:
    virtual ~ProxyResolver() = default;
    virtual ProxyResult resolve(const std::string& scheme, const std::string& host, uint16_t port) = 0;
};

// Environment-based resolver honoring NO_PROXY, HTTP(S)_PROXY (and lowercase variants)
class EnvProxyResolver : public ProxyResolver
{
public:
    ProxyResult resolve(const std::string& scheme, const std::string& host, uint16_t port) override;

private:
    static std::optional<std::string> getEnv(const char* name);
    static bool isBypassedByNoProxy(const std::string& hostOnly, const std::string& host, uint16_t port);
};

// Windows system/PAC resolver using WinHTTP. On non-Windows it returns Direct.
class SystemProxyResolver : public ProxyResolver
{
public:
    ProxyResult resolve(const std::string& scheme, const std::string& host, uint16_t port) override;
};

// Default chain: env → system
class DefaultProxyResolver : public ProxyResolver
{
public:
    DefaultProxyResolver();
    // Programmatic policy: if true (default), fall back to system proxy when env yields Direct
    void setUseSystemProxy(bool enabled);
    ProxyResult resolve(const std::string& scheme, const std::string& host, uint16_t port) override;

private:
    EnvProxyResolver _env;
    SystemProxyResolver _sys;

    struct CacheEntry
    {
        ProxyResult res;
        std::chrono::steady_clock::time_point expires;
    };
    std::mutex _m;
    std::unordered_map<std::string, CacheEntry> _cache;  // key: scheme://host:port
    std::atomic<bool> _useSystemProxy{true};
};

}  // namespace EntropyEngine::Networking::HTTP
