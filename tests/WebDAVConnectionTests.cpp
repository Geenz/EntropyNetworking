// DISABLED: WebDAVConnection has been removed in favor of HttpClient.
// WebDAV functionality is tested via WebDAVClientIntegrationTests using real HTTP server.
#if 0

#include <gtest/gtest.h>
#include "MockHttpConnection.h"
#include "Networking/WebDAV/WebDAVConnection.h"

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::WebDAV;
using namespace EntropyEngine::Networking::Tests;

static std::string httpOkWithLen(const std::string& body, const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o;
    o << "HTTP/1.1 200 OK\r\n";
    o << "Content-Length: " << body.size() << "\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n" << body;
    return o.str();
}

static std::string httpOkChunked(const std::string& body, const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o;
    o << "HTTP/1.1 200 OK\r\n";
    o << "Transfer-Encoding: chunked\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n";
    std::ostringstream chunk;
    chunk << std::hex << body.size();
    o << chunk.str() << "\r\n" << body << "\r\n0\r\n\r\n";
    return o.str();
}

static std::string davDepth0FileXml(const std::string& href) {
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    x << "<D:multistatus xmlns:D=\"DAV:\">\n";
    x << "  <D:response>\n";
    x << "    <D:href>" << href << "</D:href>\n";
    x << "    <D:propstat><D:prop><D:getcontentlength>12</D:getcontentlength></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
    x << "  </D:response>\n";
    x << "</D:multistatus>\n";
    return x.str();
}

TEST(WebDAVConnection, GetWithContentLength) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    WebDAVConnection client(mock, cfg);

    std::string body = "Hello World!";
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkWithLen(body));
    mock->enqueueResponse(std::move(resp));

    auto r = client.get("/dav/file.bin");
    ASSERT_EQ(r.statusCode, 200);
    ASSERT_EQ(std::string(r.body.begin(), r.body.end()), body);
}

TEST(WebDAVConnection, GetChunked) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    WebDAVConnection client(mock, cfg);

    std::string body = "Chunk me!";
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkChunked(body));
    mock->enqueueResponse(std::move(resp));

    auto r = client.get("/dav/c.bin");
    ASSERT_EQ(r.statusCode, 200);
    ASSERT_EQ(std::string(r.body.begin(), r.body.end()), body);
}

TEST(WebDAVConnection, PropfindDepth0) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    WebDAVConnection client(mock, cfg);

    std::string xml = davDepth0FileXml("/dav/file.txt");
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkWithLen(xml, {{"Content-Type","application/xml; charset=utf-8"}}));
    mock->enqueueResponse(std::move(resp));

    std::string bodyXml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:propfind xmlns:D=\"DAV:\"><D:prop><D:getcontentlength/></D:prop></D:propfind>";
    auto r = client.propfind("/dav/file.txt", 0, bodyXml);
    ASSERT_EQ(r.statusCode, 200);
    ASSERT_FALSE(r.body.empty());
}

TEST(WebDAVConnection, GetTimeout) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com", .requestTimeout = std::chrono::milliseconds(10) };
    WebDAVConnection client(mock, cfg);

    // Do not enqueue any response to simulate timeout
    auto r = client.get("/dav/never.reply");
    ASSERT_EQ(r.statusCode, 0);
    ASSERT_EQ(r.statusMessage, "timeout");
}

TEST(WebDAVConnection, GetBodySizeExceeded) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com", .requestTimeout = std::chrono::milliseconds(1000), .maxBodyBytes = 5 };
    WebDAVConnection client(mock, cfg);

    std::string body = "1234567890"; // 10 bytes > 5 cap

    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkWithLen(body));
    mock->enqueueResponse(std::move(resp));

    auto r = client.get("/dav/too.big");
    ASSERT_EQ(r.statusCode, 0);
    // Failure message propagated from parser; presence is enough
    ASSERT_FALSE(r.statusMessage.empty());
}

#endif // Disabled WebDAVConnectionTests
