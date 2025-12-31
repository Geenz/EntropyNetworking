/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "DavTree.h"
#include "MiniDavServer.h"
#include "Networking/HTTP/HttpClient.h"

using namespace EntropyEngine::Networking::HTTP;

/**
 * Integration tests for HttpClient against MiniDavServer
 *
 * These tests verify that HttpClient can successfully communicate with
 * a local WebDAV server (MiniDavServer) for WebDAV operations.
 */
class HttpClientWebDAVIntegration : public ::testing::Test
{
protected:
    void SetUp() override {
        // Build test tree with various file types
        tree.addDir("/");
        tree.addFile("/hello.txt", "hello world", "text/plain");
        tree.addFile("/data.json", R"({"test": "value"})", "application/json");
        tree.addDir("/subdir/");
        tree.addFile("/subdir/file.txt", "nested content", "text/plain");

        // Start MiniDavServer on random port
        server = std::make_unique<MiniDavServer>(tree, "/dav/");
        server->start();
    }

    void TearDown() override {
        if (server) server->stop();
    }

    DavTree tree;
    std::unique_ptr<MiniDavServer> server;
};

TEST_F(HttpClientWebDAVIntegration, SimpleGet) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = "127.0.0.1:" + std::to_string(server->port());
    req.path = "/dav/hello.txt";

    auto resp = client.execute(req);

    ASSERT_TRUE(resp.isSuccess()) << "Status: " << resp.statusCode << ", Message: " << resp.statusMessage;
    EXPECT_EQ(resp.statusCode, 200);

    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_EQ(body, "hello world");
}

TEST_F(HttpClientWebDAVIntegration, GetNonExistent) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = "127.0.0.1:" + std::to_string(server->port());
    req.path = "/dav/missing.txt";

    auto resp = client.execute(req);

    EXPECT_EQ(resp.statusCode, 404);
}

TEST_F(HttpClientWebDAVIntegration, PropfindDepth0) {
    HttpClient client;

    std::string propfindBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "  <D:prop>"
        "    <D:resourcetype/>"
        "    <D:getcontentlength/>"
        "  </D:prop>"
        "</D:propfind>";

    HttpRequest req;
    req.method = HttpMethod::PROPFIND;
    req.scheme = "http";
    req.host = "127.0.0.1:" + std::to_string(server->port());
    req.path = "/dav/hello.txt";
    req.headers["Depth"] = "0";
    req.headers["Content-Type"] = "application/xml; charset=utf-8";
    req.body.assign(propfindBody.begin(), propfindBody.end());

    auto resp = client.execute(req);

    ASSERT_TRUE(resp.statusCode == 200 || resp.statusCode == 207)
        << "Status: " << resp.statusCode << ", Message: " << resp.statusMessage;

    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_NE(body.find("hello.txt"), std::string::npos);
}

TEST_F(HttpClientWebDAVIntegration, PropfindDepth1Directory) {
    HttpClient client;

    std::string propfindBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "  <D:prop>"
        "    <D:resourcetype/>"
        "  </D:prop>"
        "</D:propfind>";

    HttpRequest req;
    req.method = HttpMethod::PROPFIND;
    req.scheme = "http";
    req.host = "127.0.0.1:" + std::to_string(server->port());
    req.path = "/dav/";
    req.headers["Depth"] = "1";
    req.headers["Content-Type"] = "application/xml; charset=utf-8";
    req.body.assign(propfindBody.begin(), propfindBody.end());

    auto resp = client.execute(req);

    ASSERT_TRUE(resp.statusCode == 200 || resp.statusCode == 207)
        << "Status: " << resp.statusCode << ", Message: " << resp.statusMessage;

    std::string body(resp.body.begin(), resp.body.end());
    // Should list hello.txt, data.json, and subdir
    EXPECT_NE(body.find("hello.txt"), std::string::npos);
    EXPECT_NE(body.find("data.json"), std::string::npos);
}

TEST_F(HttpClientWebDAVIntegration, ConcurrentRequests) {
    HttpClient client;

    // Make 3 concurrent requests to different resources
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < 3; i++) {
        threads.emplace_back([&client, this, &successCount]() {
            HttpRequest req;
            req.method = HttpMethod::GET;
            req.scheme = "http";
            req.host = "127.0.0.1:" + std::to_string(server->port());
            req.path = "/dav/hello.txt";

            auto resp = client.execute(req);
            if (resp.isSuccess()) {
                successCount++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successCount.load(), 3);
}

TEST_F(HttpClientWebDAVIntegration, RangeRequest) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = "127.0.0.1:" + std::to_string(server->port());
    req.path = "/dav/hello.txt";
    req.headers["Range"] = "bytes=0-4";  // Request first 5 bytes

    auto resp = client.execute(req);

    // MiniDavServer now supports Range requests
    ASSERT_EQ(resp.statusCode, 206) << "Status: " << resp.statusCode << ", Message: " << resp.statusMessage;

    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_EQ(body, "hello");  // "hello world" -> first 5 bytes = "hello"

    // Verify Content-Range header
    auto itRange = resp.headers.find("content-range");
    ASSERT_NE(itRange, resp.headers.end());
    EXPECT_EQ(itRange->second, "bytes 0-4/11");
}
