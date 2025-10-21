#include <gtest/gtest.h>
#include "MiniDavServer.h"
#include "DavTree.h"
#include "Networking/HTTP/HttpClient.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <random>

using namespace EntropyEngine::Networking::HTTP;

class HttpClientStreamingFixture : public ::testing::Test {
protected:
    void SetUp() override {
        tree.addDir("/");
        tree.addFile("/hello.txt", "hello world", "text/plain");
        server = std::make_unique<MiniDavServer>(tree, "/dav/");
        server->start();
    }
    void TearDown() override {
        if (server) server->stop();
    }

    DavTree tree;
    std::unique_ptr<MiniDavServer> server;
};

// Ensure pause/resume works: tiny consumer buffer and tiny stream buffer to force CURL_WRITEFUNC_PAUSE
TEST_F(HttpClientStreamingFixture, StreamingBackpressure_PauseResume) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/hello.txt";
    req.headers["X-Stream-Chunked"] = "1"; // force chunked streaming from server

    StreamOptions opts;
    opts.bufferBytes = 4; // extremely small to force pause
    opts.connectTimeout = std::chrono::milliseconds(5000);

    auto handle = client.executeStream(req, opts);

    std::string accum;
    std::vector<uint8_t> chunk(2);

    // Read until done
    while (!handle.isDone() && !handle.failed()) {
        size_t n = handle.read(chunk.data(), chunk.size());
        if (n > 0) {
            accum.append(reinterpret_cast<const char*>(chunk.data()), n);
        } else if (handle.failed()) {
            break;
        } else if (handle.isDone()) {
            break;
        }
    }

    EXPECT_FALSE(handle.failed()) << handle.getFailureReason();
    EXPECT_EQ(accum, std::string("hello world"));
}

// Verify headers are available early and 404 status is surfaced quickly for missing resource
TEST_F(HttpClientStreamingFixture, Streaming_EarlyError_HeadersReady404) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/missing.txt"; // server returns 404 with empty body

    StreamOptions opts; opts.bufferBytes = 64;
    auto handle = client.executeStream(req, opts);

    // Should get headers quickly
    bool ready = handle.waitForHeaders(std::chrono::milliseconds(2000));
    ASSERT_TRUE(ready) << "headers not ready in time";

    EXPECT_EQ(handle.getStatusCode(), 404);

    // Should complete quickly with no data
    std::vector<uint8_t> buf(16);
    size_t n = handle.read(buf.data(), buf.size());
    (void)n; // may be 0
    // Either done or failed (404 is not treated as failure in low-level stream)
    EXPECT_TRUE(handle.isDone());
}

// Ensure we can cancel an in-flight streaming transfer and it aborts promptly
TEST_F(HttpClientStreamingFixture, Streaming_Cancel_MidTransfer) {
    HttpClient client;

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/hello.txt";
    req.headers["X-Stream-Chunked"] = "1";

    StreamOptions opts; opts.bufferBytes = 64;
    auto handle = client.executeStream(req, opts);

    // Wait for headers then cancel
    handle.waitForHeaders(std::chrono::milliseconds(2000));
    handle.cancel();

    // Attempt to read and expect failure soon
    std::vector<uint8_t> buf(8);
    // Give libcurl some time to process cancellation
    auto start = std::chrono::steady_clock::now();
    bool failed = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        (void)handle.read(buf.data(), buf.size());
        if (handle.failed()) { failed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(failed) << "stream did not fail after cancel";
}

// Streaming uploads: PUT with known content length via uploadRead callback
TEST_F(HttpClientStreamingFixture, StreamingUpload_Put_KnownLength_StatusCodes) {
    HttpClient client;

    const size_t totalSize = 64 * 1024; // 64KB
    std::vector<uint8_t> data(totalSize);
    // Fill with deterministic pattern
    for (size_t i = 0; i < totalSize; ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    // Prepare a read cursor that serves small chunks to exercise callback multiple times
    struct Cursor { const uint8_t* p; size_t n; size_t off = 0; } cur{data.data(), data.size(), 0};
    std::atomic<int> calls{0};

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(10000);
    opts.contentLength = totalSize;
    opts.uploadRead = [&cur, &calls](char* dst, size_t max) -> size_t {
        ++calls;
        size_t remain = cur.n - cur.off;
        if (remain == 0) return 0; // EOF
        size_t take = std::min(max, remain);
        std::memcpy(dst, cur.p + cur.off, take);
        cur.off += take;
        // Simulate slow source a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return take;
    };

    HttpRequest req;
    req.method = HttpMethod::PUT;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/upload.bin";

    auto resp1 = client.execute(req, opts);
    EXPECT_TRUE(resp1.statusCode == 201 || resp1.statusCode == 204);

    // Second PUT to same path should be 204 (overwrite)
    // Reset cursor
    cur.off = 0; calls = 0;
    auto resp2 = client.execute(req, opts);
    EXPECT_TRUE(resp2.statusCode == 201 || resp2.statusCode == 204);

    EXPECT_GT(calls.load(), 1) << "uploadRead should be called multiple times";
}


// Aggregated GET follows redirect by default (302 -> 200)
TEST_F(HttpClientStreamingFixture, AggregatedGet_FollowsRedirectByDefault) {
    HttpClient client;
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/redirect";

    RequestOptions opts; // defaults: redirects enabled for safe methods
    auto resp = client.execute(req, opts);
    EXPECT_EQ(resp.statusCode, 200);
    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_EQ(body, std::string("hello world"));
}

// Conditional GET with If-None-Match should return 304 Not Modified
TEST_F(HttpClientStreamingFixture, AggregatedGet_Conditional_IfNoneMatch304) {
    HttpClient client;
    HttpRequest req1;
    req1.method = HttpMethod::GET;
    req1.scheme = "http";
    req1.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req1.path = "/dav/hello.txt";

    RequestOptions opts;
    auto resp1 = client.execute(req1, opts);
    ASSERT_EQ(resp1.statusCode, 200);
    auto it = resp1.headers.find("etag");
    ASSERT_NE(it, resp1.headers.end());
    std::string etag = it->second;

    HttpRequest req2 = req1;
    req2.headers["If-None-Match"] = etag;
    auto resp2 = client.execute(req2, opts);
    EXPECT_EQ(resp2.statusCode, 304);
    EXPECT_TRUE(resp2.body.empty());
}

// Streaming GET follows redirect by default
TEST_F(HttpClientStreamingFixture, StreamingGet_FollowsRedirectByDefault) {
    HttpClient client;
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/redirect";

    StreamOptions sopts; sopts.bufferBytes = 64; // small buffer ok
    auto handle = client.executeStream(req, sopts);
    ASSERT_TRUE(handle.waitForHeaders(std::chrono::milliseconds(2000)));
    EXPECT_EQ(handle.getStatusCode(), 200);

    std::vector<uint8_t> buf(64);
    std::string accum;
    while (!handle.isDone() && !handle.failed()) {
        size_t n = handle.read(buf.data(), buf.size());
        if (n > 0) accum.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    EXPECT_FALSE(handle.failed()) << handle.getFailureReason();
    EXPECT_EQ(accum, std::string("hello world"));
}

// Retry policy: flaky endpoint returns 500 then 200; expect success
TEST_F(HttpClientStreamingFixture, AggregatedGet_RetryOnTransientFailure) {
    HttpClient client;
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/flaky";

    RequestOptions opts; // retries enabled by default for idempotent
    opts.totalDeadline = std::chrono::milliseconds(10000);
    auto resp = client.execute(req, opts);
    EXPECT_EQ(resp.statusCode, 200);
    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_EQ(body, std::string("ok"));
}


// Additional happy-path streaming test with a normal (64 KiB) buffer
TEST_F(HttpClientStreamingFixture, StreamingHappyPath_Normal64KiBBuffer) {
    // Arrange: add a larger file (~256 KiB) to the DavTree
    std::string big(256 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = (char)(i & 0xFF);
    tree.addFile("/big.bin", big, "application/octet-stream");

    HttpClient client;
    HttpRequest req; req.method = HttpMethod::GET; req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/big.bin";

    StreamOptions sopts; sopts.bufferBytes = 64 * 1024; // 64 KiB normal buffer
    auto handle = client.executeStream(req, sopts);
    ASSERT_TRUE(handle.waitForHeaders(std::chrono::milliseconds(2000)));
    EXPECT_EQ(handle.getStatusCode(), 200);

    std::vector<uint8_t> buf(32 * 1024);
    std::string accum; accum.reserve(big.size());
    while (!handle.isDone() && !handle.failed()) {
        size_t n = handle.read(buf.data(), buf.size());
        if (n > 0) accum.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    EXPECT_FALSE(handle.failed()) << handle.getFailureReason();
    EXPECT_EQ(accum.size(), big.size());
    EXPECT_EQ(accum, big);
}


// Unknown-length (chunked) streaming upload: PUT without contentLength
TEST_F(HttpClientStreamingFixture, StreamingUpload_Put_UnknownLength_Chunked) {
    HttpClient client;

    const size_t totalSize = 64 * 1024; // 64KB
    std::vector<uint8_t> data(totalSize);
    for (size_t i = 0; i < totalSize; ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    struct Cursor { const uint8_t* p; size_t n; size_t off = 0; } cur{data.data(), data.size(), 0};
    std::atomic<int> calls{0};

    RequestOptions opts;
    opts.totalDeadline = std::chrono::milliseconds(10000);
    // No contentLength to force Transfer-Encoding: chunked
    opts.uploadRead = [&cur, &calls](char* dst, size_t max) -> size_t {
        ++calls;
        size_t remain = cur.n - cur.off;
        if (remain == 0) return 0; // EOF
        size_t take = std::min<size_t>(remain, std::min<size_t>(max, 4096)); // up to 4KiB per call
        std::memcpy(dst, cur.p + cur.off, take);
        cur.off += take;
        return take;
    };

    HttpRequest req;
    req.method = HttpMethod::PUT;
    req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/upload_chunked.bin";

    auto resp1 = client.execute(req, opts);
    EXPECT_TRUE(resp1.statusCode == 201 || resp1.statusCode == 204) << "status=" << resp1.statusCode;

    // Second PUT should typically be 204 (overwrite) from status-only server
    cur.off = 0; calls = 0;
    auto resp2 = client.execute(req, opts);
    EXPECT_TRUE(resp2.statusCode == 201 || resp2.statusCode == 204) << "status=" << resp2.statusCode;
    EXPECT_GT(calls.load(), 1);
}


// If-Match precondition tests (server returns 412 on mismatch)
TEST_F(HttpClientStreamingFixture, AggregatedPut_IfMatch_PreconditionFailed) {
    HttpClient client;

    // First, fetch current ETag of existing resource
    HttpRequest getReq; getReq.method = HttpMethod::GET; getReq.scheme = "http";
    getReq.host = std::string("127.0.0.1:") + std::to_string(server->port());
    getReq.path = "/dav/hello.txt";
    auto getResp = client.execute(getReq);
    ASSERT_EQ(getResp.statusCode, 200);
    auto it = getResp.headers.find("etag");
    ASSERT_NE(it, getResp.headers.end());
    std::string goodETag = it->second;

    // Prepare PUT to same path with mismatched If-Match
    HttpRequest putReq = getReq; putReq.method = HttpMethod::PUT;
    putReq.headers["If-Match"] = "\"999\""; // deliberately wrong
    std::string payload = "payload";
    putReq.headers["Content-Type"] = "application/octet-stream";
    putReq.body.assign(payload.begin(), payload.end());

    auto badResp = client.execute(putReq);
    EXPECT_EQ(badResp.statusCode, 412);

    // Now with correct ETag, expect normal overwrite (204)
    putReq.headers["If-Match"] = goodETag;
    auto okResp = client.execute(putReq);
    EXPECT_TRUE(okResp.statusCode == 201 || okResp.statusCode == 204);
}

TEST_F(HttpClientStreamingFixture, AggregatedDelete_IfMatch_PreconditionFailed) {
    HttpClient client;

    // Obtain ETag of existing resource
    HttpRequest getReq; getReq.method = HttpMethod::GET; getReq.scheme = "http";
    getReq.host = std::string("127.0.0.1:") + std::to_string(server->port());
    getReq.path = "/dav/hello.txt";
    auto getResp = client.execute(getReq);
    ASSERT_EQ(getResp.statusCode, 200);

    // DELETE with mismatched If-Match should return 412
    HttpRequest delReq = getReq; delReq.method = HttpMethod::DELETE_;
    delReq.path = "/dav/hello.txt"; // delete existing file from this fixture
    delReq.headers["If-Match"] = "\"does-not-match\"";
    auto delResp = client.execute(delReq);
    EXPECT_EQ(delResp.statusCode, 412);

    // DELETE without If-Match on existing file should succeed (204)
    delReq.headers.erase("If-Match");
    auto delOk = client.execute(delReq);
    EXPECT_EQ(delOk.statusCode, 204);
}
