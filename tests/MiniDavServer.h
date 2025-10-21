#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <optional>
#include <chrono>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#include "DavTree.h"

/**
 * Minimal in-process WebDAV test server using raw sockets.
 * Supports OPTIONS, PROPFIND (Depth 0/1), GET, HEAD over an in-memory DavTree.
 *
 * This is a test-only server implemented with raw sockets to avoid cpp-httplib
 * limitations with custom HTTP methods like PROPFIND. Cross-platform and suitable
 * for local integration tests.
 */
class MiniDavServer {
public:
    explicit MiniDavServer(DavTree& tree, std::string mount = "/dav/")
        : _tree(tree), _base(std::move(mount)) {}

    ~MiniDavServer() { stop(); }

    void start() {
#ifdef _WIN32
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
            throw std::runtime_error("WSAStartup failed");
#endif
        _running.store(true);
        _listenSock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (_listenSock < 0) throw std::runtime_error("socket failed");

        int on = 1;
#ifdef _WIN32
        setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#else
        setsockopt(_listenSock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0); // any available port
#ifdef _WIN32
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
#else
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
        if (::bind(_listenSock, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(_listenSock, 8) < 0)
            throw std::runtime_error("listen failed");

        // Get bound port
        socklen_t len = sizeof(addr);
        if (getsockname(_listenSock, (sockaddr*)&addr, &len) == 0) {
            _port = ntohs(addr.sin_port);
        }

        _thr = std::thread([this]{ this->acceptLoop(); });
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void stop() {
        bool exp = true;
        if (_running.compare_exchange_strong(exp, false)) {
#ifdef _WIN32
            shutdown(_listenSock, SD_BOTH);
            closesocket(_listenSock);
            _listenSock = -1;
            // Wake up accept() by connecting
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
            if (s != INVALID_SOCKET) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(_port);
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                ::connect(s, (sockaddr*)&addr, sizeof(addr));
                closesocket(s);
            }
#else
            shutdown(_listenSock, SHUT_RDWR);
            close(_listenSock);
            _listenSock = -1;
#endif
            if (_thr.joinable()) _thr.join();
#ifdef _WIN32
            WSACleanup();
#endif
        }
    }

    uint16_t port() const { return _port; }

private:
    void acceptLoop() {
        while (_running.load()) {
#ifdef _WIN32
            SOCKET cs = ::accept(_listenSock, nullptr, nullptr);
            if (cs == INVALID_SOCKET) break;
#else
            int cs = ::accept(_listenSock, nullptr, nullptr);
            if (cs < 0) break;
#endif
            handleClient(cs);
#ifdef _WIN32
            closesocket(cs);
#else
            close(cs);
#endif
        }
    }

    static bool starts_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
    }

    void handleClient(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs)
    {
        std::string req;
        char buf[4096];
        // Read until header end or buffer limit
        for (;;) {
#ifdef _WIN32
            int n = ::recv(cs, buf, sizeof(buf), 0);
#else
            ssize_t n = ::recv(cs, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            req.append(buf, buf + n);
            if (req.find("\r\n\r\n") != std::string::npos) break;
            if (req.size() > 64 * 1024) break; // header cap
        }
        if (req.empty()) return;

        // Parse request line
        auto sp1 = req.find(' ');
        auto sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            sendSimple(cs, 400);
            return;
        }
        std::string method = req.substr(0, sp1);
        std::string path = req.substr(sp1 + 1, sp2 - (sp1 + 1));

        // Parse headers
        std::unordered_map<std::string,std::string> headers;
        auto hdrStart = req.find("\r\n") + 2;
        auto hdrEnd = req.find("\r\n\r\n");
        size_t i = hdrStart;
        while (i < hdrEnd) {
            auto lineEnd = req.find("\r\n", i);
            if (lineEnd == std::string::npos || lineEnd > hdrEnd) break;
            auto line = req.substr(i, lineEnd - i);
            auto col = line.find(':');
            if (col != std::string::npos) {
                std::string k = line.substr(0, col);
                for (auto& c : k) c = (char)tolower((unsigned char)c);
                std::string v = line.substr(col + 1);
                // trim whitespace
                size_t p = 0;
                while (p < v.size() && (v[p] == ' ' || v[p] == '\t')) ++p;
                v.erase(0, p);
                headers[k] = v;
            }
            i = lineEnd + 2;
        }

        size_t contentLen = 0;
        if (auto it = headers.find("content-length"); it != headers.end()) {
            contentLen = (size_t)std::strtoull(it->second.c_str(), nullptr, 10);
        }
        std::string body;
        body.reserve(contentLen);
        auto have = req.size() - (hdrEnd + 4);
        if (have > 0) {
            size_t copy = std::min(have, contentLen);
            body.append(req.data() + hdrEnd + 4, copy);
        }
        while (body.size() < contentLen) {
#ifdef _WIN32
            int n = ::recv(cs, buf, sizeof(buf), 0);
#else
            ssize_t n = ::recv(cs, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            size_t remain = contentLen - body.size();
            size_t copy = (size_t)n > remain ? remain : (size_t)n;
            body.append(buf, buf + copy);
        }

        if (!starts_with(path, _base)) {
            sendSimple(cs, 404);
            return;
        }
        if (path.find("..") != std::string::npos) {
            sendSimple(cs, 400);
            return;
        }

        // Route methods
        if (method == "OPTIONS") {
            handleOptions(cs);
            return;
        }

        if (method == "PROPFIND") {
            int depth = 0;
            if (auto it = headers.find("depth"); it != headers.end()) {
                if (it->second == "1") depth = 1;
                else if (it->second == "infinity") {
                    sendRaw(cs, 400, "application/xml; charset=utf-8",
                           "<error>Depth infinity not supported</error>");
                    return;
                }
            }
            handlePropfind(cs, path, depth);
            return;
        }

        if (method == "HEAD" || method == "GET") {
            std::string tpath = toTreePath(path);
            auto* n = _tree.find(tpath);
            if (!n || n->isDir) {
                sendSimple(cs, n ? 405 : 404);
                return;
            }
            std::string bodyOut = (method == "GET") ? n->content : std::string();
            std::string mime = n->mime.empty() ? "application/octet-stream" : n->mime;

            std::unordered_map<std::string,std::string> extraHeaders;
            extraHeaders["Accept-Ranges"] = "bytes";
            extraHeaders["Content-Length"] = std::to_string(n->content.size());
            sendRaw(cs, 200, mime, bodyOut, extraHeaders);
            return;
        }

        // Method not allowed
        sendSimple(cs, 405, {{"Allow","OPTIONS, PROPFIND, GET, HEAD"}});
    }

    std::string toTreePath(const std::string& reqPath) const {
        // Keep leading '/'
        std::string p = reqPath.substr(_base.size() - 1);
        return p;
    }

    static std::string httpDate(const std::optional<std::chrono::system_clock::time_point>& tp) {
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
    }

    void handleOptions(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs) const
    {
        std::unordered_map<std::string,std::string> headers;
        headers["DAV"] = "1,2";
        headers["Allow"] = "OPTIONS, PROPFIND, GET, HEAD";
        headers["Accept-Ranges"] = "bytes";
        sendRaw(cs, 200, "text/plain", "", headers);
    }

    void handlePropfind(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs, const std::string& path, int depth)
    {
        std::string tpath = toTreePath(path);
        const DavNode* n = _tree.find(tpath);
        if (!n) {
            sendSimple(cs, 404);
            return;
        }

        // Build minimal multistatus XML
        std::string xml;
        xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        xml += "<D:multistatus xmlns:D=\"DAV:\">\n";

        auto appendResponse = [&](const std::string& href, const DavNode& node) {
            xml += "  <D:response>\n";
            xml += "    <D:href>" + href + "</D:href>\n";
            xml += "    <D:propstat><D:prop>";
            xml += "<D:displayname>" + node.name + "</D:displayname>";
            xml += "<D:resourcetype>";
            if (node.isDir) xml += "<D:collection/>";
            xml += "</D:resourcetype>";
            if (!node.isDir) {
                xml += "<D:getcontentlength>" + std::to_string(node.content.size()) + "</D:getcontentlength>";
            }
            if (auto d = httpDate(node.mtime); !d.empty()) {
                xml += "<D:getlastmodified>" + d + "</D:getlastmodified>";
            }
            if (!node.isDir && !node.mime.empty()) {
                xml += "<D:getcontenttype>" + node.mime + "</D:getcontenttype>";
            }
            xml += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
            xml += "  </D:response>\n";
        };

        std::string selfHref = path;
        if (n->isDir && !selfHref.empty() && selfHref.back() != '/')
            selfHref.push_back('/');
        appendResponse(selfHref, *n);

        if (depth >= 1 && n->isDir) {
            for (auto& kv : n->children) {
                const auto& c = *kv.second;
                std::string href = selfHref + c.name + (c.isDir ? "/" : "");
                appendResponse(href, c);
            }
        }
        xml += "</D:multistatus>\n";
        sendRaw(cs, 207, "application/xml; charset=utf-8", xml);
    }

    static void sendSimple(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs, int status, const std::unordered_map<std::string,std::string>& extraHeaders = {})
    {
        sendRaw(cs, status, "text/plain", "", extraHeaders);
    }

    static void sendRaw(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs, int status, const std::string& contentType, const std::string& body,
        std::unordered_map<std::string,std::string> headers = {})
    {
        std::string statusText = (status == 200 ? "OK" :
                                  status == 207 ? "Multi-Status" :
                                  status == 404 ? "Not Found" :
                                  status == 405 ? "Method Not Allowed" :
                                  status == 400 ? "Bad Request" : "");
        std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n";
        headers["Content-Type"] = contentType;
        if (headers.find("Content-Length") == headers.end()) {
            headers["Content-Length"] = std::to_string(body.size());
        }
        headers["Connection"] = "keep-alive";
        for (auto& kv : headers) {
            resp += kv.first + ": " + kv.second + "\r\n";
        }
        resp += "\r\n";
        resp += body;
        sendAll(cs, resp.data(), resp.size());
    }

    static void sendAll(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs, const char* data, size_t len)
    {
        size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            int n = ::send(cs, data + sent, (int)(len - sent), 0);
#else
            ssize_t n = ::send(cs, data + sent, len - sent, 0);
#endif
            if (n <= 0) break;
            sent += (size_t)n;
        }
    }

    DavTree& _tree;
    std::string _base;
    std::atomic<bool> _running{false};
#ifdef _WIN32
    SOCKET _listenSock = INVALID_SOCKET;
#else
    int _listenSock = -1;
#endif
    std::thread _thr;
    uint16_t _port = 0;
};
