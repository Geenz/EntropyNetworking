/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>
#include "Networking/HTTP/HttpClient.h"
#include "MiniDavServer.h"
#include "DavTree.h"
#include <thread>
#include <chrono>

using namespace EntropyEngine::Networking::HTTP;

class HttpClientRobustnessTests : public ::testing::Test {
protected:
    HttpClient client;
};

// Test DNS resolution failure
TEST_F(HttpClientRobustnessTests, DnsResolutionFailure) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "this-domain-absolutely-does-not-exist-12345.invalid";
    req.path = "/";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(5000);

    auto resp = client.execute(req, opts);

    // Should fail with status 0 and error message
    EXPECT_EQ(resp.statusCode, 0);
    EXPECT_FALSE(resp.statusMessage.empty());
    EXPECT_NE(resp.statusMessage.find("cURL error"), std::string::npos);
}

// Test connection refused (invalid port)
TEST_F(HttpClientRobustnessTests, ConnectionRefused) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = "127.0.0.1:1"; // Port 1 should be refused
    req.path = "/";

    RequestOptions opts;
    opts.connectTimeout = std::chrono::milliseconds(2000);
    opts.totalDeadline = std::chrono::milliseconds(5000);

    auto resp = client.execute(req, opts);

    EXPECT_EQ(resp.statusCode, 0);
    EXPECT_FALSE(resp.statusMessage.empty());
}

// Test timeout on slow server (local harness)
TEST_F(HttpClientRobustnessTests, TimeoutHandling) {
    // Local server with delay endpoint to avoid external dependency
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = host;
    req.path = "/delay/10"; // 10 second delay

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(2000); // 2 second timeout

    auto start = std::chrono::steady_clock::now();
    auto resp = client.execute(req, opts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Should timeout (statusCode==0) in ~2 seconds
    EXPECT_EQ(resp.statusCode, 0);
    EXPECT_NE(resp.statusMessage.find("cURL error"), std::string::npos);

    // Should timeout around 2 seconds, not wait 10
    EXPECT_LT(elapsed.count(), 4000);
}

// Test HTTPS with real server
TEST_F(HttpClientRobustnessTests, HttpsRealServer) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "https"; // Explicit HTTPS
    req.host = "httpbin.org";
    req.path = "/get";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess()) << "Status: " << resp.statusCode
                                   << ", Message: " << resp.statusMessage;
    EXPECT_FALSE(resp.body.empty());
}

// Test POST with body (local harness)
TEST_F(HttpClientRobustnessTests, PostWithJsonBody) {
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    HttpRequest req;
    req.method = HttpMethod::POST;
    req.scheme = "http";
    req.host = host;
    req.path = "/post";

    std::string jsonBody = R"({"test": "data", "number": 42, "nested": {"key": "value"}})";
    req.body.assign(jsonBody.begin(), jsonBody.end());
    req.headers["content-type"] = "application/json";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess());
    EXPECT_FALSE(resp.body.empty());

    // Response should echo back our JSON
    std::string respBody(resp.body.begin(), resp.body.end());
    EXPECT_NE(respBody.find("test"), std::string::npos);
    EXPECT_NE(respBody.find("data"), std::string::npos);
}

// Test custom headers (local harness)
TEST_F(HttpClientRobustnessTests, CustomHeaders) {
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = host;
    req.path = "/headers";
    req.headers["x-custom-header"] = "test-value-12345";
    req.headers["x-another-header"] = "another-value";
    req.headers["user-agent"] = "HttpClient-Test/1.0";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess());

    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_NE(body.find("X-Custom-Header"), std::string::npos);
    EXPECT_NE(body.find("test-value-12345"), std::string::npos);
    EXPECT_NE(body.find("X-Another-Header"), std::string::npos);
}

// Test connection reuse (same host multiple requests) - local harness
TEST_F(HttpClientRobustnessTests, ConnectionReuse) {
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    // Make multiple requests to same host
    for (int i = 0; i < 5; i++) {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.scheme = "http";
        req.host = host;
        req.path = "/get?request=" + std::to_string(i);

        RequestOptions opts;
        opts.totalDeadline = std::chrono::milliseconds(30000);

        auto resp = client.execute(req, opts);
        EXPECT_TRUE(resp.isSuccess()) << "Request " << i << " failed";
    }
}

// Test different HTTP methods
TEST_F(HttpClientRobustnessTests, HttpMethods) {
    // Use local MiniDavServer so this test always runs and is deterministic
    DavTree tree; tree.addDir("/"); tree.addFile("/hello.txt", "hello", "text/plain");
    MiniDavServer server(tree, "/dav/");
    server.start();

    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    struct TestCase {
        HttpMethod method;
        std::string path;
        int expectedStatus;
        std::vector<uint8_t> body;
    };

    std::vector<TestCase> cases = {
        {HttpMethod::GET, "/dav/hello.txt", 200, {}},
        {HttpMethod::HEAD, "/dav/hello.txt", 200, {}},
        {HttpMethod::PUT, "/dav/newfile.bin", 201, {'t','e','s','t'}},
        {HttpMethod::DELETE_, "/dav/hello.txt", 204, {}},
        {HttpMethod::OPTIONS, "/dav/", 200, {}},
        {HttpMethod::PROPFIND, "/dav/", 207, {}} // Depth may default; server returns 207
    };

    for (const auto& tc : cases) {
        HttpRequest req;
        req.method = tc.method;
        req.scheme = "http";
        req.host = host;
        req.path = tc.path;
        if (!tc.body.empty()) {
            req.body = tc.body;
            req.headers["Content-Type"] = "application/octet-stream";
        }
        if (tc.method == HttpMethod::PROPFIND) {
            req.headers["Depth"] = "0";
            const char* bodyXml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                "<D:propfind xmlns:D=\"DAV:\">"
                "  <D:prop><D:resourcetype/></D:prop>"
                "</D:propfind>";
            req.body.assign(bodyXml, bodyXml + std::strlen(bodyXml));
            req.headers["Content-Type"] = "application/xml; charset=utf-8";
        }

        RequestOptions opts;
        opts.totalDeadline = std::chrono::milliseconds(10000);

        auto resp = client.execute(req, opts);
        // For PUT on an existing resource or server semantics, allow 201 or 204
        if (tc.method == HttpMethod::PUT) {
            EXPECT_TRUE(resp.statusCode == 201 || resp.statusCode == 204)
                << "PUT returned: " << resp.statusCode;
        } else {
            EXPECT_EQ(resp.statusCode, tc.expectedStatus)
                << "Method failed: " << tc.path << ", got " << resp.statusCode;
        }
    }
}

// Test response size limit (local harness)
TEST_F(HttpClientRobustnessTests, ResponseSizeLimit) {
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = host;
    req.path = "/bytes/2048"; // Request 2KB

    RequestOptions opts;
    opts.maxResponseBytes = 1024; // Limit to 1KB
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    // Should fail due to size limit
    EXPECT_EQ(resp.statusCode, 0);
    EXPECT_NE(resp.statusMessage.find("cURL error"), std::string::npos);
}

// Test empty response (local harness)
TEST_F(HttpClientRobustnessTests, EmptyResponse) {
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = host;
    req.path = "/bytes/0"; // Request 0 bytes

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess());
    EXPECT_TRUE(resp.body.empty());
}

// Test HTTP status codes (local harness)
TEST_F(HttpClientRobustnessTests, HttpStatusCodes) {
    DavTree tree; tree.addDir("/");
    MiniDavServer server(tree, "/dav/");
    server.start();
    auto host = std::string("127.0.0.1:") + std::to_string(server.port());

    struct StatusTest {
        std::string path;
        int expectedStatus;
    };

    std::vector<StatusTest> tests = {
        {"/status/200", 200},
        {"/status/201", 201},
        {"/status/204", 204},
        {"/status/400", 400},
        {"/status/404", 404},
        {"/status/500", 500},
    };

    for (const auto& test : tests) {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.scheme = "http";
        req.host = host;
        req.path = test.path;

        RequestOptions opts;
        opts.totalDeadline = std::chrono::milliseconds(30000);

        auto resp = client.execute(req, opts);
        EXPECT_EQ(resp.statusCode, test.expectedStatus)
            << "Path: " << test.path << ", got status: " << resp.statusCode;
    }
}

// Test concurrent requests
TEST_F(HttpClientRobustnessTests, ConcurrentRequests) {
    // Always-on local test for concurrent requests
    DavTree tree; tree.addDir("/");
    // Add multiple files to fetch concurrently
    tree.addFile("/f0.txt", "zero", "text/plain");
    tree.addFile("/f1.txt", "one", "text/plain");
    tree.addFile("/f2.txt", "two", "text/plain");
    tree.addFile("/f3.txt", "three", "text/plain");
    MiniDavServer server(tree, "/dav/");
    server.start();

    const int numThreads = 4;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([this, i, &successCount, &server]() {
            HttpRequest req;
            req.method = HttpMethod::GET;
            req.scheme = "http";
            req.host = std::string("127.0.0.1:") + std::to_string(server.port());
            req.path = "/dav/f" + std::to_string(i) + ".txt";

            RequestOptions opts;
            opts.totalDeadline = std::chrono::milliseconds(10000);

            auto resp = client.execute(req, opts);
            if (resp.isSuccess()) {
                successCount++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successCount.load(), numThreads);
}

// Test header case normalization (local MiniDavServer, always-on)
TEST_F(HttpClientRobustnessTests, HeaderCaseNormalization) {
    // Spin up a tiny local DAV server
    DavTree tree; tree.addDir("/"); tree.addFile("/hello.txt", "hello", "text/plain");
    MiniDavServer server(tree, "/dav/");
    server.start();

    HttpClient localClient;
    HttpRequest req; req.method = HttpMethod::GET; req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server.port());
    req.path = "/dav/hello.txt";

    RequestOptions opts; opts.totalDeadline = std::chrono::milliseconds(10000);
    auto resp = localClient.execute(req, opts);

    ASSERT_EQ(resp.statusCode, 200);

    // All response header keys must be lowercase in our API
    EXPECT_NE(resp.headers.find("content-type"), resp.headers.end());
    EXPECT_NE(resp.headers.find("content-length"), resp.headers.end());
    // Our MiniDavServer also sets ETag for files
    EXPECT_NE(resp.headers.find("etag"), resp.headers.end());

    // Mixed/Title case keys should not be present
    EXPECT_EQ(resp.headers.find("Content-Type"), resp.headers.end());
    EXPECT_EQ(resp.headers.find("Content-Length"), resp.headers.end());
    EXPECT_EQ(resp.headers.find("ETag"), resp.headers.end());
}

// Test closeIdle functionality
TEST_F(HttpClientRobustnessTests, CloseIdleConnections) {
    // Always-on local test for closeIdle()
    DavTree tree; tree.addDir("/"); tree.addFile("/hello.txt", "hello", "text/plain");
    MiniDavServer server(tree, "/dav/");
    server.start();

    // Make first request
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server.port());
    req.path = "/dav/hello.txt";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(10000);

    auto resp1 = client.execute(req, opts);
    EXPECT_TRUE(resp1.isSuccess());

    // Close idle connections
    client.closeIdle();

    // Make second request - should still work
    auto resp2 = client.execute(req, opts);
    EXPECT_TRUE(resp2.isSuccess());
}
