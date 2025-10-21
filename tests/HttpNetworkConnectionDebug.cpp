#include <gtest/gtest.h>
#include "HttpNetworkConnection.h"
#include "MiniDavServer.h"
#include "DavTree.h"
#include <Concurrency/WorkService.h>
#include <thread>
#include <chrono>

using namespace EntropyEngine::Networking::Tests;
using namespace EntropyEngine::Core::Concurrency;

TEST(HttpNetworkConnectionDebug, BasicSendReceive) {
    // Setup server
    DavTree tree;
    tree.addDir("/");
    tree.addFile("/test.txt", "test content", "text/plain");

    MiniDavServer server(tree, "/dav/");
    server.start();

    // Setup work service
    WorkService workService(WorkService::Config{});
    workService.start();

    WorkContractGroup httpGroup(1000, "HttpGroup");
    workService.addWorkContractGroup(&httpGroup);

    // Create connection
    HttpNetworkConnection conn("127.0.0.1", server.port(), &httpGroup);
    conn.connect();

    // Track if callback was invoked
    bool callbackInvoked = false;
    std::vector<uint8_t> receivedData;
    std::mutex callbackMutex;
    std::condition_variable callbackCV;

    conn.setMessageCallback([&](const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lk(callbackMutex);
        callbackInvoked = true;
        receivedData = data;
        callbackCV.notify_one();
    });

    // Send simple HTTP GET
    std::string request = "GET /dav/test.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::vector<uint8_t> requestBytes(request.begin(), request.end());

    auto sendResult = conn.send(requestBytes);
    ASSERT_TRUE(sendResult.success()) << sendResult.errorMessage;

    // Wait for callback
    std::unique_lock<std::mutex> lk(callbackMutex);
    bool gotResponse = callbackCV.wait_for(lk, std::chrono::seconds(5), [&]{ return callbackInvoked; });

    ASSERT_TRUE(gotResponse) << "Callback was not invoked within timeout";
    ASSERT_GT(receivedData.size(), 0) << "Received data is empty";

    std::string response(reinterpret_cast<const char*>(receivedData.data()), receivedData.size());
    EXPECT_TRUE(response.find("HTTP/1.1 200") != std::string::npos) << "Response: " << response;
    EXPECT_TRUE(response.find("test content") != std::string::npos) << "Response: " << response;

    workService.stop();
    server.stop();
}
