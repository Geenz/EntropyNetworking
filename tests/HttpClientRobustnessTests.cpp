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

// Test timeout on slow server
TEST_F(HttpClientRobustnessTests, TimeoutHandling) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "httpbin.org";
    req.path = "/delay/10"; // 10 second delay

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(2000); // 2 second timeout

    auto start = std::chrono::steady_clock::now();
    auto resp = client.execute(req, opts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Should timeout
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

// Test POST with body
TEST_F(HttpClientRobustnessTests, PostWithJsonBody) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.host = "httpbin.org";
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

// Test custom headers
TEST_F(HttpClientRobustnessTests, CustomHeaders) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "httpbin.org";
    req.path = "/headers";
    req.headers["x-custom-header"] = "test-value-12345";
    req.headers["x-another-header"] = "another-value";
    req.headers["user-agent"] = "HttpClient-Test/1.0";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess());

    // httpbin echoes headers back
    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_NE(body.find("X-Custom-Header"), std::string::npos);
    EXPECT_NE(body.find("test-value-12345"), std::string::npos);
    EXPECT_NE(body.find("X-Another-Header"), std::string::npos);
}

// Test connection reuse (same host multiple requests)
TEST_F(HttpClientRobustnessTests, ConnectionReuse) {
    // Make multiple requests to same host
    for (int i = 0; i < 5; i++) {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.host = "httpbin.org";
        req.path = "/get?request=" + std::to_string(i);

        RequestOptions opts;
        opts.totalDeadline = std::chrono::milliseconds(30000);

        auto resp = client.execute(req, opts);
        EXPECT_TRUE(resp.isSuccess()) << "Request " << i << " failed";
    }
}

// Test different HTTP methods
TEST_F(HttpClientRobustnessTests, HttpMethods) {
    struct TestCase {
        HttpMethod method;
        std::string path;
        int expectedStatus;
    };

    std::vector<TestCase> cases = {
        {HttpMethod::GET, "/get", 200},
        {HttpMethod::POST, "/post", 200},
        {HttpMethod::PUT, "/put", 200}, // httpbin.org currently returns 200 for PUT
        {HttpMethod::DELETE_, "/delete", 200},
        {HttpMethod::PATCH, "/patch", 200},
    };

    for (const auto& tc : cases) {
        HttpRequest req;
        req.method = tc.method;
        req.host = "httpbin.org";
        req.path = tc.path;

        if (tc.method != HttpMethod::GET && tc.method != HttpMethod::HEAD) {
            req.body = {'t', 'e', 's', 't'};
        }

        RequestOptions opts;
        opts.totalDeadline = std::chrono::milliseconds(30000);

        auto resp = client.execute(req, opts);
        EXPECT_EQ(resp.statusCode, tc.expectedStatus)
            << "Method failed: " << tc.path;
    }
}

// Test response size limit
TEST_F(HttpClientRobustnessTests, ResponseSizeLimit) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "httpbin.org";
    req.path = "/bytes/2048"; // Request 2KB

    RequestOptions opts;
    opts.maxResponseBytes = 1024; // Limit to 1KB
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    // Should fail due to size limit
    EXPECT_EQ(resp.statusCode, 0);
    EXPECT_NE(resp.statusMessage.find("cURL error"), std::string::npos);
}

// Test empty response
TEST_F(HttpClientRobustnessTests, EmptyResponse) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "httpbin.org";
    req.path = "/bytes/0"; // Request 0 bytes

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess());
    EXPECT_TRUE(resp.body.empty());
}

// Test HTTP status codes
TEST_F(HttpClientRobustnessTests, HttpStatusCodes) {
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
        req.host = "httpbin.org";
        req.path = test.path;

        RequestOptions opts;
        opts.totalDeadline = std::chrono::milliseconds(30000);

        auto resp = client.execute(req, opts);
        EXPECT_EQ(resp.statusCode, test.expectedStatus)
            << "Path: " << test.path;
    }
}

// Test concurrent requests
TEST_F(HttpClientRobustnessTests, ConcurrentRequests) {
    const int numThreads = 4;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([this, i, &successCount]() {
            HttpRequest req;
            req.method = HttpMethod::GET;
            req.host = "httpbin.org";
            req.path = "/get?thread=" + std::to_string(i);

            RequestOptions opts;
            opts.totalDeadline = std::chrono::milliseconds(30000);

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

// Test header case normalization
TEST_F(HttpClientRobustnessTests, HeaderCaseNormalization) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "httpbin.org";
    req.path = "/response-headers?X-Test-Header=TestValue";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp = client.execute(req, opts);

    EXPECT_TRUE(resp.isSuccess());

    // Headers should be lowercase
    EXPECT_NE(resp.headers.find("content-type"), resp.headers.end());
    EXPECT_EQ(resp.headers.find("Content-Type"), resp.headers.end());
}

// Test closeIdle functionality
TEST_F(HttpClientRobustnessTests, CloseIdleConnections) {
    // Make first request
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.host = "httpbin.org";
    req.path = "/get";

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(30000);

    auto resp1 = client.execute(req, opts);
    EXPECT_TRUE(resp1.isSuccess());

    // Close idle connections
    client.closeIdle();

    // Make second request - should still work
    auto resp2 = client.execute(req, opts);
    EXPECT_TRUE(resp2.isSuccess());
}
