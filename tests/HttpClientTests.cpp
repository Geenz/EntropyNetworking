#include <gtest/gtest.h>

#include "DavTree.h"
#include "MiniDavServer.h"
#include "Networking/HTTP/HttpClient.h"

using namespace EntropyEngine::Networking::HTTP;

namespace
{

class HttpClientFixture : public ::testing::Test
{
protected:
    void SetUp() override {
        // Build a small DAV tree
        tree.addDir("/");
        tree.addFile("/hello.txt", "hello world", "text/plain");
        tree.addFile("/bin.dat", std::string("abc\0def", 7), "application/octet-stream");

        // Start server mounted at /dav/
        server = std::make_unique<MiniDavServer>(tree, "/dav/");
        server->start();
    }

    void TearDown() override {
        if (server) server->stop();
    }

    std::unique_ptr<MiniDavServer> server;
    DavTree tree;
};

}  // namespace

TEST_F(HttpClientFixture, Get_BasicFile_Aggregated) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";  // Local test server doesn't use TLS
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/hello.txt";

    RequestOptions opts{};  // defaults OK

    auto resp = client.execute(req, opts);
    ASSERT_EQ(resp.statusCode, 200) << resp.statusMessage;
    // Headers should be normalized to lowercase keys
    ASSERT_TRUE(resp.headers.find("content-type") != resp.headers.end());

    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_EQ(body, "hello world");
}

TEST_F(HttpClientFixture, Head_NoBody_ContentLengthPresent) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::HEAD;
    req.scheme = "http";  // Local test server doesn't use TLS
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/hello.txt";

    auto resp = client.execute(req);
    ASSERT_EQ(resp.statusCode, 200) << resp.statusMessage;
    // Body must be empty for HEAD
    EXPECT_TRUE(resp.body.empty());
    // Content-Length header should exist and reflect full resource size (11)
    auto it = resp.headers.find("content-length");
    ASSERT_NE(it, resp.headers.end());
    EXPECT_EQ(it->second, "11");
}

TEST_F(HttpClientFixture, MultipleSequentialGets_Succeed) {
    HttpClient client;

    for (int i = 0; i < 5; ++i) {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.scheme = "http";  // Local test server doesn't use TLS
        req.host = std::string("127.0.0.1:") + std::to_string(server->port());
        req.path = "/dav/hello.txt";
        auto resp = client.execute(req);
        ASSERT_EQ(resp.statusCode, 200) << "iteration " << i << ": " << resp.statusMessage;
        std::string body(resp.body.begin(), resp.body.end());
        EXPECT_EQ(body, "hello world");
    }
}
