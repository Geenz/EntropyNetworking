// DISABLED: These tests use WebDAVConnection which has been removed in favor of HttpClient.
// TODO: Rewrite these tests to use HttpClient streaming or integrate with MiniDavServer.
#if 0

#include <gtest/gtest.h>
#include "MockHttpConnection.h"
#include "Networking/WebDAV/WebDAVConnection.h"
#include "Networking/WebDAV/WebDAVFileSystemBackend.h"
#include <VirtualFileSystem/VirtualFileSystem.h>
#include <array>

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::WebDAV;
using namespace EntropyEngine::Networking::Tests;
using namespace EntropyEngine::Core::IO;

static std::string httpOkWithLen(const std::string& body, const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o; o << "HTTP/1.1 200 OK\r\n";
    o << "Content-Length: " << body.size() << "\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n" << body; return o.str();
}

static std::string httpOkChunked(const std::string& body, const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o; o << "HTTP/1.1 200 OK\r\n";
    o << "Transfer-Encoding: chunked\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n";
    std::ostringstream chunk; chunk << std::hex << body.size();
    o << chunk.str() << "\r\n" << body << "\r\n0\r\n\r\n";
    return o.str();
}

TEST(WebDAVStreaming, Stream_ReadsIncrementally_ContentLength) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    EntropyEngine::Core::Concurrency::WorkContractGroup group(2000);
    VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    // Script response delivered in two chunks
    std::string body = "HelloWorld";
    MockHttpConnection::ScriptedResponse resp;
    std::string http = httpOkWithLen(body);
    // Split the HTTP response into headers and body as two chunks
    auto splitPos = http.find("\r\n\r\n");
    ASSERT_NE(splitPos, std::string::npos);
    splitPos += 4; // include delimiter
    resp.chunks.push_back(http.substr(0, splitPos));
    resp.chunks.push_back(http.substr(splitPos));
    mock->enqueueResponse(std::move(resp));

    StreamOptions so; so.mode = StreamOptions::Read; so.buffered = false; so.bufferSize = 1024;
    auto stream = backend->openStream("/assets/stream.bin", so);
    ASSERT_NE(stream, nullptr);

    std::array<std::byte, 5> buf{};
    auto r1 = stream->read(buf);
    ASSERT_TRUE(r1.success());
    ASSERT_EQ(r1.bytesTransferred, 5u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf.data()), 5), std::string("Hello"));

    auto r2 = stream->read(buf);
    ASSERT_TRUE(r2.success());
    ASSERT_EQ(r2.bytesTransferred, 5u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf.data()), 5), std::string("World"));

    // EOF on next read
    auto r3 = stream->read(buf);
    EXPECT_TRUE(r3.complete);
}

TEST(WebDAVStreaming, Stream_ReadsIncrementally_Chunked) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    EntropyEngine::Core::Concurrency::WorkContractGroup group(2000);
    VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    std::string body = "ABCDEFGHIJ";
    MockHttpConnection::ScriptedResponse resp;
    std::string http = httpOkChunked(body);
    // deliver in several small chunks to exercise parser
    resp.chunks.push_back(http.substr(0, 20));
    resp.chunks.push_back(http.substr(20, 30));
    resp.chunks.push_back(http.substr(50));
    mock->enqueueResponse(std::move(resp));

    StreamOptions so; so.mode = StreamOptions::Read; so.buffered = false; so.bufferSize = 1024;
    auto stream = backend->openStream("/assets/chunked.bin", so);
    ASSERT_NE(stream, nullptr);

    std::vector<std::byte> accum(10);
    size_t off = 0;
    while (off < accum.size()) {
        std::array<std::byte, 3> small{};
        auto r = stream->read(small);
        ASSERT_TRUE(r.success());
        if (r.bytesTransferred == 0 && r.complete) break;
        std::memcpy(accum.data() + off, small.data(), r.bytesTransferred);
        off += r.bytesTransferred;
    }
    EXPECT_EQ(std::string(reinterpret_cast<char*>(accum.data()), 10), body);
}

#endif // Disabled WebDAVStreamingTests
