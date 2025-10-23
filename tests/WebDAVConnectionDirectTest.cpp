// DISABLED: WebDAVConnection has been removed in favor of HttpClient.
// WebDAV functionality is tested via WebDAVClientIntegrationTests using real HTTP server.
#if 0

#include <gtest/gtest.h>
#include "HttpNetworkConnection.h"
#include "MiniDavServer.h"
#include "DavTree.h"
#include "Networking/WebDAV/WebDAVConnection.h"
#include <Concurrency/WorkService.h>
#include <iostream>

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::WebDAV;
using namespace EntropyEngine::Networking::Tests;
using namespace EntropyEngine::Core::Concurrency;

TEST(WebDAVConnectionDirectTest, SimpleGet) {
    // Setup server
    DavTree tree;
    tree.addDir("/");
    tree.addFile("/hello.txt", "hello world", "text/plain");

    MiniDavServer server(tree, "/dav/");
    server.start();

    std::cout << "Server started on port " << server.port() << std::endl;

    // Setup work service
    WorkService workService(WorkService::Config{});
    workService.start();

    WorkContractGroup httpGroup(1000, "HttpGroup");
    workService.addWorkContractGroup(&httpGroup);

    // Create HTTP connection
    auto httpConn = std::make_shared<HttpNetworkConnection>("127.0.0.1", server.port(), &httpGroup);
    httpConn->connect();

    std::cout << "HttpNetworkConnection created and connected" << std::endl;

    // Create WebDAV connection
    WebDAVConnection::Config davCfg;
    davCfg.host = "127.0.0.1:" + std::to_string(server.port());
    auto davConn = std::make_shared<WebDAVConnection>(httpConn, davCfg);

    std::cout << "WebDAVConnection created, attempting GET /dav/hello.txt" << std::endl;

    // Perform GET
    auto resp = davConn->get("/dav/hello.txt");

    std::cout << "Response received: statusCode=" << resp.statusCode
              << " statusMessage=" << resp.statusMessage
              << " bodySize=" << resp.body.size() << std::endl;

    ASSERT_TRUE(resp.isSuccess()) << "Status: " << resp.statusCode << " Message: " << resp.statusMessage;
    EXPECT_EQ(resp.statusCode, 200);

    std::string body(resp.body.begin(), resp.body.end());
    std::cout << "Body: " << body << std::endl;
    EXPECT_EQ(body, "hello world");

    workService.stop();
    server.stop();
}

#endif // Disabled WebDAVConnectionDirectTest
