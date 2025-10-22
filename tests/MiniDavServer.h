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
#if defined(SO_NOSIGPIPE)
            // Prevent SIGPIPE on macOS/BSD when client disconnects mid-send
            int set = 1;
            setsockopt(cs, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
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

        // Read request body (supports Content-Length and Transfer-Encoding: chunked)
        size_t contentLen = 0;
        bool isChunked = false;
        if (auto it = headers.find("transfer-encoding"); it != headers.end()) {
            const std::string& te = it->second;
            if (te.find("chunked") != std::string::npos) isChunked = true;
        }
        if (auto it = headers.find("content-length"); it != headers.end()) {
            contentLen = (size_t)std::strtoull(it->second.c_str(), nullptr, 10);
        }

        std::string body;
        if (isChunked) {
            // Decode chunked body from the socket using the already buffered data in `req`
            size_t pos = hdrEnd + 4; // start of body within req buffer
            auto readMore = [&]() -> bool {
#ifdef _WIN32
                int n = ::recv(cs, buf, sizeof(buf), 0);
#else
                ssize_t n = ::recv(cs, buf, sizeof(buf), 0);
#endif
                if (n <= 0) return false;
                req.append(buf, buf + n);
                return true;
            };
            auto readLine = [&]() -> std::string {
                for (;;) {
                    size_t crlf = req.find("\r\n", pos);
                    if (crlf != std::string::npos) {
                        std::string line = req.substr(pos, crlf - pos);
                        pos = crlf + 2;
                        return line;
                    }
                    if (!readMore()) return std::string();
                }
            };
            auto readExact = [&](size_t nbytes) -> bool {
                while (req.size() < pos + nbytes) {
                    if (!readMore()) return false;
                }
                body.append(req.data() + pos, nbytes);
                pos += nbytes;
                return true;
            };

            for (;;) {
                std::string sizeLine = readLine();
                if (sizeLine.empty()) break; // malformed
                // sizeLine may include chunk extensions after ';' — strip them
                auto sc = sizeLine.find(';');
                if (sc != std::string::npos) sizeLine = sizeLine.substr(0, sc);
                size_t chunkSize = (size_t)std::strtoull(sizeLine.c_str(), nullptr, 16);
                if (chunkSize == 0) {
                    // Read the trailing CRLF and optional trailers (ignore trailers)
                    // Consume one blank line after 0-size chunk if present
                    (void)readLine();
                    break;
                }
                if (!readExact(chunkSize)) break;
                // Consume CRLF after the chunk data
                (void)readLine();
            }
        } else {
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
            // Special endpoints for tests: redirect and flaky behavior
            if (path == _base + "redirect") {
                sendSimple(cs, 302, {{"Location", _base + std::string("hello.txt")}});
                return;
            }
            if (path == _base + "flaky") {
                int c = ++_hitCounts[path];
                if (c == 1) {
                    sendSimple(cs, 500);
                    return;
                } else {
                    sendRaw(cs, 200, "text/plain", "ok");
                    return;
                }
            }
            std::string tpath = toTreePath(path);
            auto* n = _tree.find(tpath);
            if (!n || n->isDir) {
                sendSimple(cs, n ? 405 : 404);
                return;
            }

            std::unordered_map<std::string,std::string> extraHeaders;
            extraHeaders["Accept-Ranges"] = "bytes";
            std::string mime = n->mime.empty() ? "application/octet-stream" : n->mime;

            // Simple ETag based on content size (sufficient for tests) and Last-Modified if available
            std::string etag = "\"" + std::to_string(n->content.size()) + "\"";
            extraHeaders["ETag"] = etag;
            if (auto lm = httpDate(n->mtime); !lm.empty()) {
                extraHeaders["Last-Modified"] = lm;
            }

            // Conditional GET/HEAD: If-None-Match → 304 Not Modified
            auto itInm = headers.find("if-none-match");
            if (itInm != headers.end()) {
                // Basic match (no weak validators used)
                if (itInm->second == etag) {
                    sendSimple(cs, 304, {{"ETag", etag}});
                    return;
                }
            }

            // Handle Range requests (return 206 with Content-Range and Content-Length)
            auto itRange = headers.find("range");
            if (method == "GET" && itRange != headers.end()) {
                // Parse "bytes=start-end"
                const std::string& rangeValue = itRange->second;
                if (rangeValue.rfind("bytes=", 0) == 0) {
                    std::string byteRange = rangeValue.substr(6);
                    auto dashPos = byteRange.find('-');
                    if (dashPos != std::string::npos) {
                        size_t start = 0, end = n->content.size() - 1;
                        std::string startStr = byteRange.substr(0, dashPos);
                        std::string endStr = byteRange.substr(dashPos + 1);

                        if (!startStr.empty()) start = std::strtoull(startStr.c_str(), nullptr, 10);
                        if (!endStr.empty()) end = std::strtoull(endStr.c_str(), nullptr, 10);

                        // Clamp to valid range
                        if (start < n->content.size() && end >= start) {
                            end = std::min(end, n->content.size() - 1);
                            size_t length = end - start + 1;
                            std::string bodyOut = n->content.substr(start, length);

                            extraHeaders["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                                            std::to_string(end) + "/" +
                                                            std::to_string(n->content.size());
                            extraHeaders["Content-Length"] = std::to_string(length);
                            sendRaw(cs, 206, mime, bodyOut, extraHeaders);
                            return;
                        }
                    }
                }
            }

            // Full response (no range or range parsing failed)
            if (method == "HEAD") {
                // HEAD: advertise full length, no body
                extraHeaders["Content-Length"] = std::to_string(n->content.size());
                sendRaw(cs, 200, mime, /*body*/"", extraHeaders);
                return;
            }

            // For GET without Range: default to Content-Length full body.
            // Enable chunked streaming only for specific test paths or when explicitly requested.
            bool wantChunked = false;
            // Header-driven toggle
            if (auto it = headers.find("x-stream-chunked"); it != headers.end()) {
                if (it->second == "1" || it->second == "true" || it->second == "on") wantChunked = true;
            }

            std::string bodyOut = n->content;
            if (wantChunked) {
                // Small chunk size to ensure multiple chunks for small files
                size_t chunkSize = 5; // 5-byte chunks work well with tests reading small pieces
                int delayMs = 2; // small inter-chunk delay to allow mid-transfer cancellation tests to observe aborts reliably
                sendChunked(cs, 200, mime, bodyOut, extraHeaders, chunkSize, delayMs);
            } else {
                extraHeaders["Content-Length"] = std::to_string(bodyOut.size());
                sendRaw(cs, 200, mime, bodyOut, extraHeaders);
            }
            return;
        }

        if (method == "PUT") {
            std::string tpath = toTreePath(path);
            auto* n = _tree.find(tpath);
            if (n && n->isDir) {
                sendSimple(cs, 405);
                return;
            }

            // If-Match precondition handling (status-only semantics)
            auto itIfMatch = headers.find("if-match");
            if (itIfMatch != headers.end()) {
                std::string cond = itIfMatch->second;
                // Resource must exist when If-Match is present (unless cond is wildcard and resource exists)
                if (!n) {
                    // RFC would return 412 when resource does not exist and If-Match is present
                    sendSimple(cs, 412);
                    return;
                }
                bool ok = false;
                if (cond == "*") {
                    ok = true; // any current representation is acceptable
                } else {
                    // Compare against our simple ETag for files (size-based)
                    if (!n->isDir) {
                        std::string etag = "\"" + std::to_string(n->content.size()) + "\"";
                        ok = (cond == etag);
                    }
                }
                if (!ok) {
                    sendSimple(cs, 412);
                    return;
                }
            }

            // 201 if created, 204 if replaced
            sendSimple(cs, n ? 204 : 201);
            return;
        }

        if (method == "DELETE") {
            std::string tpath = toTreePath(path);
            auto* n = _tree.find(tpath);
            if (!n) {
                sendSimple(cs, 404);
                return;
            }
            // If-Match precondition handling
            auto itIfMatch = headers.find("if-match");
            if (itIfMatch != headers.end()) {
                std::string cond = itIfMatch->second;
                bool ok = false;
                if (cond == "*") {
                    ok = true; // any current representation is acceptable
                } else {
                    if (!n->isDir) {
                        std::string etag = "\"" + std::to_string(n->content.size()) + "\"";
                        ok = (cond == etag);
                    } else {
                        // For directories, require wildcard to match
                        ok = false;
                    }
                }
                if (!ok) {
                    sendSimple(cs, 412);
                    return;
                }
            }
            sendSimple(cs, 204);
            return;
        }

        if (method == "MKCOL") {
            std::string tpath = toTreePath(path);
            auto* n = _tree.find(tpath);
            // If resource already exists, 405
            if (n) {
                sendSimple(cs, 405);
                return;
            }
            // Check parent existence
            std::string parent = tpath;
            if (parent.size()>1 && parent.back()=='/') parent.pop_back();
            auto slash = parent.find_last_of('/');
            if (slash == std::string::npos) parent = "/";
            else if (slash == 0) parent = "/";
            else parent = parent.substr(0, slash);
            auto* pn = _tree.find(parent);
            if (!pn || !pn->isDir) {
                sendSimple(cs, 409);
                return;
            }
            sendSimple(cs, 201);
            return;
        }

        // MOVE and COPY (status-only)
        if (method == "MOVE" || method == "COPY") {
            // Destination header is required
            auto itDest = headers.find("destination");
            if (itDest == headers.end()) { sendSimple(cs, 400); return; }
            std::string destPath = itDest->second;
            // Extract absolute-path from absolute-URI if necessary
            auto schemePos = destPath.find("://");
            if (schemePos != std::string::npos) {
                auto pathStart = destPath.find('/', schemePos + 3);
                if (pathStart != std::string::npos) destPath = destPath.substr(pathStart);
            }
            // Must be under our mount base
            if (!starts_with(destPath, _base)) { sendSimple(cs, 400); return; }

            bool overwrite = true;
            if (auto itOw = headers.find("overwrite"); itOw != headers.end()) {
                overwrite = !itOw->second.empty() && (itOw->second[0] == 'T' || itOw->second[0] == 't');
            }

            std::string srcTree = toTreePath(path);
            std::string dstTree = toTreePath(destPath);

            const DavNode* srcNode = _tree.find(srcTree);
            if (!srcNode) { sendSimple(cs, 404); return; }

            // Parent of destination must exist
            std::string parent = dstTree;
            if (parent.size()>1 && parent.back()=='/') parent.pop_back();
            auto slash = parent.find_last_of('/');
            if (slash == std::string::npos) parent = "/";
            else if (slash == 0) parent = "/";
            else parent = parent.substr(0, slash);
            const DavNode* pn = _tree.find(parent);
            if (!pn || !pn->isDir) { sendSimple(cs, 409); return; }

            const DavNode* dstNode = _tree.find(dstTree);
            if (dstNode && !overwrite) { sendSimple(cs, 412); return; }

            // Success statuses only (no tree mutation for tests):
            // 201 if created new, 204 if overwrote existing
            sendSimple(cs, dstNode ? 204 : 201);
            return;
        }

        // Method not allowed
        sendSimple(cs, 405, {{"Allow","OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY"}});
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
        headers["Allow"] = "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY";
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
                                  status == 206 ? "Partial Content" :
                                  status == 207 ? "Multi-Status" :
                                  status == 302 ? "Found" :
                                  status == 307 ? "Temporary Redirect" :
                                  status == 404 ? "Not Found" :
                                  status == 405 ? "Method Not Allowed" :
                                  status == 400 ? "Bad Request" :
                                  status == 500 ? "Internal Server Error" : "");
        std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n";
        headers["Content-Type"] = contentType;
        if (headers.find("Content-Length") == headers.end()) {
            headers["Content-Length"] = std::to_string(body.size());
        }
        headers["Connection"] = "close";  // Single request per connection
        for (auto& kv : headers) {
            resp += kv.first + ": " + kv.second + "\r\n";
        }
        resp += "\r\n";
        resp += body;
        sendAll(cs, resp.data(), resp.size());
    }

    // Send a chunked-encoding response in multiple chunks (test-only helper)
    static void sendChunked(
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        cs, int status, const std::string& contentType, const std::string& body,
        std::unordered_map<std::string,std::string> headers = {}, size_t chunkSize = 8192, int delayMs = 0)
    {
        if (chunkSize == 0) chunkSize = 8192;
        std::string statusText = (status == 200 ? "OK" :
                                  status == 206 ? "Partial Content" :
                                  status == 207 ? "Multi-Status" :
                                  status == 302 ? "Found" :
                                  status == 307 ? "Temporary Redirect" :
                                  status == 404 ? "Not Found" :
                                  status == 405 ? "Method Not Allowed" :
                                  status == 400 ? "Bad Request" :
                                  status == 500 ? "Internal Server Error" : "");
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n";
        headers["Content-Type"] = contentType;
        headers["Transfer-Encoding"] = "chunked";
        headers["Connection"] = "close";
        for (auto& kv : headers) {
            head += kv.first + ": " + kv.second + "\r\n";
        }
        head += "\r\n";
        sendAll(cs, head.data(), head.size());

        // write chunks
        size_t off = 0;
        while (off < body.size()) {
            size_t n = std::min(chunkSize, body.size() - off);
            char sizeLine[32];
            int sl = std::snprintf(sizeLine, sizeof(sizeLine), "%zx\r\n", n);
            sendAll(cs, sizeLine, (size_t)sl);
            sendAll(cs, body.data() + off, n);
            sendAll(cs, "\r\n", 2);
            off += n;
            if (delayMs > 0) {
#ifdef _WIN32
                Sleep((DWORD)delayMs);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
#endif
            }
        }
        // final zero-size chunk
        sendAll(cs, "0\r\n\r\n", 5);
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
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            ssize_t n = ::send(cs, data + sent, len - sent, flags);
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
    // Simple per-path hit counters to simulate flaky endpoints
    std::unordered_map<std::string,int> _hitCounts;
};
