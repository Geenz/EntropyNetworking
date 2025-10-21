#pragma once

#include "Networking/Transport/NetworkConnection.h"
#include <Concurrency/WorkContractGroup.h>
#include <llhttp.h>
#include <mutex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace EntropyEngine::Networking::Tests {

/**
 * @brief NetworkConnection implementation that speaks HTTP to a server
 *
 * Uses raw TCP sockets and llhttp for both request parsing (incoming) and
 * response parsing (from server). No dependency on httplib for client side.
 *
 * IMPORTANT: Delivers responses asynchronously via WorkContractGroup to avoid
 * deadlocks with WebDAVConnection's mutex locking strategy.
 */
class HttpNetworkConnection : public NetworkConnection {
private:
    struct ParsedRequest {
        std::string method;
        std::string url;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
        bool complete = false;

        std::string curHeaderField;
        std::string curHeaderValue;
    };

    struct ParsedResponse {
        int statusCode = 0;
        std::string statusMessage;
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body;
        bool headersComplete = false;
        bool complete = false;

        std::string curHeaderField;
        std::string curHeaderValue;
    };

    // Request parsing callbacks
    static int on_url_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* req = static_cast<ParsedRequest*>(parser->data);
        req->url.append(at, len);
        return 0;
    }

    static int on_req_header_field_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* req = static_cast<ParsedRequest*>(parser->data);
        if (!req->curHeaderField.empty() && !req->curHeaderValue.empty()) {
            req->headers[req->curHeaderField] = req->curHeaderValue;
            req->curHeaderField.clear();
            req->curHeaderValue.clear();
        }
        req->curHeaderField.append(at, len);
        return 0;
    }

    static int on_req_header_value_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* req = static_cast<ParsedRequest*>(parser->data);
        req->curHeaderValue.append(at, len);
        return 0;
    }

    static int on_req_headers_complete_cb(llhttp_t* parser) {
        auto* req = static_cast<ParsedRequest*>(parser->data);
        if (!req->curHeaderField.empty()) {
            req->headers[req->curHeaderField] = req->curHeaderValue;
            req->curHeaderField.clear();
            req->curHeaderValue.clear();
        }
        req->method = llhttp_method_name(static_cast<llhttp_method>(parser->method));
        return 0;
    }

    static int on_req_body_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* req = static_cast<ParsedRequest*>(parser->data);
        req->body.append(at, len);
        return 0;
    }

    static int on_req_message_complete_cb(llhttp_t* parser) {
        auto* req = static_cast<ParsedRequest*>(parser->data);
        req->complete = true;
        return 0;
    }

    // Response parsing callbacks
    static int on_resp_header_field_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* resp = static_cast<ParsedResponse*>(parser->data);
        if (!resp->curHeaderField.empty() && !resp->curHeaderValue.empty()) {
            resp->headers[resp->curHeaderField] = resp->curHeaderValue;
            resp->curHeaderField.clear();
            resp->curHeaderValue.clear();
        }
        resp->curHeaderField.append(at, len);
        return 0;
    }

    static int on_resp_header_value_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* resp = static_cast<ParsedResponse*>(parser->data);
        resp->curHeaderValue.append(at, len);
        return 0;
    }

    static int on_resp_headers_complete_cb(llhttp_t* parser) {
        auto* resp = static_cast<ParsedResponse*>(parser->data);
        if (!resp->curHeaderField.empty()) {
            resp->headers[resp->curHeaderField] = resp->curHeaderValue;
            resp->curHeaderField.clear();
            resp->curHeaderValue.clear();
        }
        resp->statusCode = parser->status_code;
        resp->headersComplete = true;
        return 0;
    }

    static int on_resp_body_cb(llhttp_t* parser, const char* at, size_t len) {
        auto* resp = static_cast<ParsedResponse*>(parser->data);
        resp->body.insert(resp->body.end(),
            reinterpret_cast<const uint8_t*>(at),
            reinterpret_cast<const uint8_t*>(at) + len);
        return 0;
    }

    static int on_resp_message_complete_cb(llhttp_t* parser) {
        auto* resp = static_cast<ParsedResponse*>(parser->data);
        resp->complete = true;
        return 0;
    }

public:
    HttpNetworkConnection(std::string host, uint16_t port, Core::Concurrency::WorkContractGroup* workGroup)
        : _host(std::move(host)), _port(port), _workGroup(workGroup) {

        // Setup llhttp settings for request parsing
        llhttp_settings_init(&_reqSettings);
        _reqSettings.on_url = on_url_cb;
        _reqSettings.on_header_field = on_req_header_field_cb;
        _reqSettings.on_header_value = on_req_header_value_cb;
        _reqSettings.on_headers_complete = on_req_headers_complete_cb;
        _reqSettings.on_body = on_req_body_cb;
        _reqSettings.on_message_complete = on_req_message_complete_cb;

        // Setup llhttp settings for response parsing
        llhttp_settings_init(&_respSettings);
        _respSettings.on_header_field = on_resp_header_field_cb;
        _respSettings.on_header_value = on_resp_header_value_cb;
        _respSettings.on_headers_complete = on_resp_headers_complete_cb;
        _respSettings.on_body = on_resp_body_cb;
        _respSettings.on_message_complete = on_resp_message_complete_cb;

#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }

    ~HttpNetworkConnection() override {
        shutdownCallbacks();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    Result<void> connect() override {
        _connected = true;
        return Result<void>::ok();
    }

    Result<void> disconnect() override {
        _connected = false;
        return Result<void>::ok();
    }

    bool isConnected() const override {
        return _connected;
    }

    Result<void> send(const std::vector<uint8_t>& data) override {
        if (!_connected) {
            return Result<void>::err(NetworkError::ConnectionClosed, "Not connected");
        }

        // Parse HTTP request using llhttp
        ParsedRequest req;
        llhttp_t parser;
        llhttp_init(&parser, HTTP_REQUEST, &_reqSettings);
        parser.data = &req;

        auto err = llhttp_execute(&parser, reinterpret_cast<const char*>(data.data()), data.size());
        if (err != HPE_OK || !req.complete) {
            return Result<void>::err(NetworkError::InvalidParameter,
                std::string("Failed to parse HTTP request: ") + llhttp_errno_name(err));
        }

        // Execute HTTP request on work contract thread
        auto work = _workGroup->createContract([this, requestBytes=data]() {
            // Create TCP socket
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return;

            // Connect to server
            sockaddr_in serverAddr{};
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(_port);
            inet_pton(AF_INET, _host.c_str(), &serverAddr.sin_addr);

            if (::connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return;
            }

            // Set a socket receive timeout as a safety net (5 seconds)
#ifdef _WIN32
            {
                DWORD timeoutMs = 5000;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
            }
#else
            {
                timeval tv{}; tv.tv_sec = 5; tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            }
#endif

            // Send HTTP request
            ::send(sock, reinterpret_cast<const char*>(requestBytes.data()), (int)requestBytes.size(), 0);

            // Use llhttp to parse response and detect completion
            // Accumulate raw bytes separately to preserve exact framing
            ParsedResponse resp;
            llhttp_t respParser;
            llhttp_init(&respParser, HTTP_RESPONSE, &_respSettings);
            respParser.data = &resp;

            std::vector<uint8_t> buffer(4096);
            std::vector<uint8_t> raw;
            constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024; // 16 MiB safety limit

            while (!resp.complete) {
                int bytesRead = recv(sock, reinterpret_cast<char*>(buffer.data()), (int)buffer.size(), 0);
                if (bytesRead <= 0) {
                    // Timeout or connection closed - deliver what we have
                    break;
                }

                // Accumulate raw bytes
                raw.insert(raw.end(), buffer.begin(), buffer.begin() + bytesRead);

                // Safety limit
                if (raw.size() > kMaxResponseBytes) break;

                // Feed to llhttp for RFC-compliant parsing
                auto err = llhttp_execute(&respParser,
                    reinterpret_cast<const char*>(buffer.data()), bytesRead);
                if (err != HPE_OK && err != HPE_PAUSED) {
                    // Parse error - deliver what we have
                    break;
                }
            }
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif

            if (raw.empty()) return;

            // Deliver raw HTTP response bytes via callback (on work contract thread)
            onMessageReceived(raw);
        });

        work.schedule();

        return Result<void>::ok();
    }

    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override {
        return send(data);
    }

    ConnectionState getState() const override {
        return _connected ? ConnectionState::Connected : ConnectionState::Disconnected;
    }

    ConnectionType getType() const override {
        return ConnectionType::Remote;
    }

    ConnectionStats getStats() const override {
        return ConnectionStats{};
    }

private:
    std::string _host;
    uint16_t _port;
    Core::Concurrency::WorkContractGroup* _workGroup;
    llhttp_settings_t _reqSettings{};
    llhttp_settings_t _respSettings{};
    bool _connected = false;
};

} // namespace EntropyEngine::Networking::Tests
