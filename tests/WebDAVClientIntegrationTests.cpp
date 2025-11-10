#include <gtest/gtest.h>
#include "DavTree.h"
#include "MiniDavServer.h"
#include "Networking/WebDAV/WebDAVFileSystemBackend.h"
#include "Networking/WebDAV/WebDAVReadStream.h"
#include "Networking/HTTP/HttpClient.h"
#include <Concurrency/WorkService.h>
#include <VirtualFileSystem/VirtualFileSystem.h>
#include <algorithm>
#include <array>
#include <span>
#include <cstring>

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::WebDAV;
using namespace EntropyEngine::Core::IO;
using namespace EntropyEngine::Core::Concurrency;

// Integration tests for WebDAV client (WebDAVConnection + WebDAVFileSystemBackend)
// against a real HTTP server (MiniDavServer)

namespace {

class WebDAVClientIntegrationFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Build test tree
        tree.addDir("/");
        tree.addFile("/hello.txt", "hello world", "text/plain");
        tree.addFile("/binary.bin", std::string("abc\0def", 7), "application/octet-stream");
        tree.addDir("/sub/");
        tree.addFile("/sub/child.txt", "child content", "text/plain");
        tree.addFile("/sub/data.bin", std::string("\x01\x02\x03\x04", 4), "application/octet-stream");

        // Start server
        server = std::make_unique<MiniDavServer>(tree, "/dav/");
        server->start();

        // Create work service and VFS group
        workService = std::make_unique<WorkService>(WorkService::Config{});
        workService->start();

        vfsGroup = std::make_unique<WorkContractGroup>(2000, "VfsGroup");
        workService->addWorkContractGroup(vfsGroup.get());

        // Create VFS
        vfs = std::make_unique<VirtualFileSystem>(vfsGroup.get());

        // Create backend with HttpClient
        WebDAVFileSystemBackend::Config bcfg;
        bcfg.scheme = "http";  // Local test server uses HTTP
        bcfg.host = "127.0.0.1:" + std::to_string(server->port());
        bcfg.baseUrl = "/dav";
        backend = std::make_shared<WebDAVFileSystemBackend>(bcfg);
        backend->setVirtualFileSystem(vfs.get());
    }

    void TearDown() override {
        backend.reset();
        vfs.reset();
        vfsGroup.reset();

        if (workService) {
            workService->stop();
        }
        workService.reset();

        if (server) server->stop();
    }

    DavTree tree;
    std::unique_ptr<MiniDavServer> server;
    std::unique_ptr<WorkService> workService;
    std::unique_ptr<WorkContractGroup> vfsGroup;
    std::unique_ptr<VirtualFileSystem> vfs;
    std::shared_ptr<WebDAVFileSystemBackend> backend;
};

} // namespace

// ============================================================================
// WebDAVFileSystemBackend Integration Tests (against MiniDavServer)
// ============================================================================
// Note: We skip direct WebDAVConnection tests since we can't easily create
// a NetworkConnection that talks HTTP to localhost. The backend tests below
// cover the full stack through the VFS API.

TEST_F(WebDAVClientIntegrationFixture, ServerSmokeTest) {
    EXPECT_GT(server->port(), 0);
}

// Disabled: WebDAVConnection removed in favor of HttpClient
// TEST_F(WebDAVClientIntegrationFixture, Connection_Get_SimpleFile) {
//     auto resp = davConn->get("/dav/hello.txt");
//     ASSERT_TRUE(resp.isSuccess());
//     EXPECT_EQ(resp.statusCode, 200);
//     std::string body(resp.body.begin(), resp.body.end());
//     EXPECT_EQ(body, "hello world");
// }

TEST_F(WebDAVClientIntegrationFixture, Backend_ReadFile_Complete) {
    auto handle = backend->readFile("/hello.txt");
    handle.wait();
    ASSERT_EQ(handle.status(), FileOpStatus::Complete);

    auto bytes = handle.contentsBytes();
    std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    EXPECT_EQ(content, "hello world");
}

TEST_F(WebDAVClientIntegrationFixture, Backend_ConcurrentReads) {
    std::vector<FileOperationHandle> handles;

    handles.push_back(backend->readFile("/hello.txt"));
    handles.push_back(backend->readFile("/binary.bin"));
    handles.push_back(backend->readFile("/sub/child.txt"));

    for (auto& h : handles) {
        h.wait();
        EXPECT_EQ(h.status(), FileOpStatus::Complete);
    }

    std::string content0(reinterpret_cast<const char*>(handles[0].contentsBytes().data()), handles[0].contentsBytes().size());
    EXPECT_EQ(content0, "hello world");
}


TEST_F(WebDAVClientIntegrationFixture, Backend_Exists) {
    EXPECT_TRUE(backend->exists("/hello.txt"));
    EXPECT_FALSE(backend->exists("/missing.txt"));
}

TEST_F(WebDAVClientIntegrationFixture, Backend_GetMetadata) {
    auto h = backend->getMetadata("/hello.txt");
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    ASSERT_TRUE(h.metadata().has_value());
    auto md = *h.metadata();
    EXPECT_TRUE(md.exists);
    EXPECT_FALSE(md.isDirectory);
    EXPECT_EQ(md.size, 11u);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_ListDirectory) {
    auto h = backend->listDirectory("/sub/");
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    const auto& entries = h.directoryEntries();
    // Expect at least the two known children
    bool hasChildTxt = false, hasDataBin = false;
    for (const auto& e : entries) {
        if (e.name == "child.txt") hasChildTxt = true;
        if (e.name == "data.bin") hasDataBin = true;
    }
    EXPECT_TRUE(hasChildTxt);
    EXPECT_TRUE(hasDataBin);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_WriteFile_StatusOnly) {
    // Existing file: server returns 204 No Content
    const char* payload = "new content";
    std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
    auto h1 = backend->writeFile("/hello.txt", data);
    h1.wait();
    EXPECT_EQ(h1.status(), FileOpStatus::Complete);

    // New file: server returns 201 Created
    auto h2 = backend->writeFile("/sub/new.bin", data);
    h2.wait();
    EXPECT_EQ(h2.status(), FileOpStatus::Complete);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_CreateFile_StatusOnly) {
    auto h = backend->createFile("/empty.bin");
    h.wait();
    EXPECT_EQ(h.status(), FileOpStatus::Complete);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_DeleteFile_StatusOnly) {
    // Existing file
    auto h1 = backend->deleteFile("/binary.bin");
    h1.wait();
    EXPECT_EQ(h1.status(), FileOpStatus::Complete);

    // Missing file should fail (404)
    auto h2 = backend->deleteFile("/does_not_exist.bin");
    h2.wait();
    EXPECT_EQ(h2.status(), FileOpStatus::Failed);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_CreateDirectory_MKCOL_StatusMapping) {
    // Case 1: create new directory (status-only server returns 201; no persistence)
    auto h1 = backend->createDirectory("/newdir/");
    h1.wait();
    EXPECT_EQ(h1.status(), FileOpStatus::Complete);

    // Case 2: create existing directory ("/sub/") → 405 → failure
    auto h2 = backend->createDirectory("/sub/");
    h2.wait();
    EXPECT_EQ(h2.status(), FileOpStatus::Failed);

    // Case 3: missing parent ("/missing/child") → 409 → failure
    auto h3 = backend->createDirectory("/missing/child");
    h3.wait();
    EXPECT_EQ(h3.status(), FileOpStatus::Failed);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_MoveCopy_StatusMapping) {
    // COPY to a new destination → success (201)
    auto c1 = backend->copy("/hello.txt", "/sub/hello_copy.txt", /*overwrite=*/true);
    c1.wait();
    EXPECT_EQ(c1.status(), FileOpStatus::Complete);

    // COPY to an existing destination with Overwrite: F → 412 → failure
    auto c2 = backend->copy("/hello.txt", "/sub/child.txt", /*overwrite=*/false);
    c2.wait();
    EXPECT_EQ(c2.status(), FileOpStatus::Failed);

    // MOVE to a new destination → success (201)
    auto m1 = backend->move("/hello.txt", "/sub/moved.txt", /*overwrite=*/true);
    m1.wait();
    EXPECT_EQ(m1.status(), FileOpStatus::Complete);

    // MOVE with missing parent → 409 → failure
    auto m2 = backend->move("/hello.txt", "/missing/child.txt", /*overwrite=*/true);
    m2.wait();
    EXPECT_EQ(m2.status(), FileOpStatus::Failed);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_StreamRead_SmallChunks) {
    StreamOptions so; so.mode = StreamOptions::Read; so.buffered = false; so.bufferSize = 1024;
    auto stream = backend->openStream("/hello.txt", so);
    ASSERT_NE(stream, nullptr);

    std::string accum;
    accum.reserve(16);
    std::array<uint8_t, 5> chunk{};
    for (;;) {
        auto r = stream->read(chunk);
        if (r.bytesTransferred > 0) {
            accum.append(reinterpret_cast<const char*>(chunk.data()), r.bytesTransferred);
        }
        if (r.complete) break;
        // Avoid infinite loop if nothing read and not complete (should not happen)
        if (r.bytesTransferred == 0 && stream->fail()) {
            break;
        }
    }
    EXPECT_FALSE(stream->fail());
    EXPECT_EQ(accum, std::string("hello world"));
    stream->close();
}

TEST_F(WebDAVClientIntegrationFixture, Backend_ReadFile_Conditional_IfNoneMatch304) {
    // Acquire current ETag using HttpClient, then ask backend helper with If-None-Match
    HTTP::HttpClient hc;
    HTTP::HttpRequest req; req.method = HTTP::HttpMethod::GET; req.scheme = "http";
    req.host = std::string("127.0.0.1:") + std::to_string(server->port());
    req.path = "/dav/hello.txt";
    auto resp = hc.execute(req);
    ASSERT_EQ(resp.statusCode, 200);
    auto it = resp.headers.find("etag");
    ASSERT_NE(it, resp.headers.end());
    std::string etag = it->second;

    auto h = backend->readFileIfNoneMatch("/hello.txt", etag);
    h.wait();
    // For 304 path we surface Complete with empty bytes as a cache hit indicator
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    EXPECT_EQ(h.contentsBytes().size(), 0u);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_StreamRead_HeaderHelpers) {
    StreamOptions so; so.mode = StreamOptions::Read; so.buffered = false; so.bufferSize = 1024;
    auto streamBase = backend->openStream("/binary.bin", so);
    ASSERT_NE(streamBase, nullptr);

    // Downcast to WebDAVReadStream to access helper accessors
    auto* davStream = dynamic_cast<EntropyEngine::Networking::WebDAV::WebDAVReadStream*>(streamBase.get());
    ASSERT_NE(davStream, nullptr);

    // Wait until headers are ready by attempting a tiny read
    std::array<uint8_t, 1> tmp{}; (void)streamBase->read(tmp);

    auto cl = davStream->contentLength();
    auto et = davStream->etag();
    auto ct = davStream->contentType();

    ASSERT_TRUE(cl.has_value());
    EXPECT_EQ(*cl, 7u);
    ASSERT_TRUE(et.has_value());
    EXPECT_FALSE(et->empty());
    ASSERT_TRUE(ct.has_value());
    EXPECT_NE(ct->find("application/octet-stream"), std::string::npos);

    streamBase->close();
}


TEST_F(WebDAVClientIntegrationFixture, Backend_WriteFile_IfMatch_Precondition) {
    const char* payload = "abc";
    std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(payload), 3);

    // Wrong ETag should fail with 412 mapping → Failed status
    auto hBad = backend->writeFile("/hello.txt", data, "\"999\"");
    hBad.wait();
    EXPECT_EQ(hBad.status(), FileOpStatus::Failed);

    // Correct ETag for hello.txt (size 11) should succeed
    auto hOk = backend->writeFile("/hello.txt", data, "\"11\"");
    hOk.wait();
    EXPECT_EQ(hOk.status(), FileOpStatus::Complete);
}

TEST_F(WebDAVClientIntegrationFixture, Backend_DeleteFile_IfMatch_Precondition) {
    // Wrong ETag should fail
    auto hBad = backend->deleteFileIfMatch("/hello.txt", "\"does-not-match\"");
    hBad.wait();
    EXPECT_EQ(hBad.status(), FileOpStatus::Failed);

    // Without precondition should succeed
    auto hOk = backend->deleteFile("/hello.txt");
    hOk.wait();
    EXPECT_EQ(hOk.status(), FileOpStatus::Complete);
}
