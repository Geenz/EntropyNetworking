#include <gtest/gtest.h>
#include "DavTree.h"
#include "MiniDavServer.h"
#include "Networking/WebDAV/WebDAVConnection.h"
#include "Networking/WebDAV/WebDAVFileSystemBackend.h"
#include "HttpNetworkConnection.h"
#include <Concurrency/WorkService.h>
#include <VirtualFileSystem/VirtualFileSystem.h>
#include <algorithm>

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::WebDAV;
using namespace EntropyEngine::Networking::Tests;
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

        // Create work service and groups
        workService = std::make_unique<WorkService>(WorkService::Config{});
        workService->start();

        httpGroup = std::make_unique<WorkContractGroup>(1000, "HttpGroup");
        vfsGroup = std::make_unique<WorkContractGroup>(2000, "VfsGroup");

        workService->addWorkContractGroup(httpGroup.get());
        workService->addWorkContractGroup(vfsGroup.get());

        // Create VFS
        vfs = std::make_unique<VirtualFileSystem>(vfsGroup.get());

        // Create HTTP network connection using work group
        httpConn = std::make_shared<HttpNetworkConnection>("127.0.0.1", server->port(), httpGroup.get());
        httpConn->connect();

        // Create WebDAV connection
        WebDAVConnection::Config davCfg;
        davCfg.host = "127.0.0.1:" + std::to_string(server->port());
        davConn = std::make_shared<WebDAVConnection>(httpConn, davCfg);

        // Create backend
        WebDAVFileSystemBackend::Config bcfg;
        bcfg.baseUrl = "/dav";
        backend = std::make_shared<WebDAVFileSystemBackend>(davConn, bcfg);
        backend->setVirtualFileSystem(vfs.get());

        // Create connection pool (3 connections) to avoid contention
        std::vector<std::shared_ptr<WebDAVConnection>> pool;
        for (int i = 0; i < 3; i++) {
            auto http = std::make_shared<HttpNetworkConnection>("127.0.0.1", server->port(), httpGroup.get());
            http->connect();
            auto dav = std::make_shared<WebDAVConnection>(http, davCfg);
            pool.push_back(dav);
        }
        backend->setAggregateConnections(pool);
    }

    void TearDown() override {
        backend.reset();
        davConn.reset();
        if (httpConn) {
            httpConn->disconnect();
        }
        httpConn.reset();

        vfs.reset();
        vfsGroup.reset();
        httpGroup.reset();

        if (workService) {
            workService->stop();
        }
        workService.reset();

        if (server) server->stop();
    }

    DavTree tree;
    std::unique_ptr<MiniDavServer> server;
    std::unique_ptr<WorkService> workService;
    std::unique_ptr<WorkContractGroup> httpGroup;
    std::unique_ptr<WorkContractGroup> vfsGroup;
    std::unique_ptr<VirtualFileSystem> vfs;
    std::shared_ptr<HttpNetworkConnection> httpConn;
    std::shared_ptr<WebDAVConnection> davConn;
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

TEST_F(WebDAVClientIntegrationFixture, Connection_Get_SimpleFile) {
    auto resp = davConn->get("/dav/hello.txt");
    ASSERT_TRUE(resp.isSuccess());
    EXPECT_EQ(resp.statusCode, 200);
    std::string body(resp.body.begin(), resp.body.end());
    EXPECT_EQ(body, "hello world");
}

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
