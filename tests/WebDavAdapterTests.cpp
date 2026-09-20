#include <Concurrency/WorkService.h>
#include <VirtualFileSystem/VirtualFileSystem.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Networking/Protocol/WebDavAdapter.h"

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core::IO;
using namespace EntropyEngine::Core::Concurrency;

namespace
{

static std::string writeTextFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs << text;
    ofs.close();
    return text;
}

static size_t fileSize(const std::filesystem::path& p) {
    return static_cast<size_t>(std::filesystem::file_size(p));
}

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class WebDavAdapterFixture : public ::testing::Test
{
protected:
    void SetUp() override {
        // Create work service and start it
        workService = std::make_unique<WorkService>(WorkService::Config{});
        workService->start();

        // Create work contract group and register with work service
        vfsGroup = std::make_unique<WorkContractGroup>(2000, "VfsGroup");
        workService->addWorkContractGroup(vfsGroup.get());

        // Create VFS with the work contract group
        vfs = std::make_shared<VirtualFileSystem>(vfsGroup.get());
    }

    void TearDown() override {
        // Stop work service FIRST before destroying the work group
        if (workService) {
            workService->stop();
        }

        // Now safe to destroy VFS and work group
        vfs.reset();
        vfsGroup.reset();
        workService.reset();
    }

    std::unique_ptr<WorkService> workService;
    std::unique_ptr<WorkContractGroup> vfsGroup;
    std::shared_ptr<VirtualFileSystem> vfs;
};

}  // namespace

TEST_F(WebDavAdapterFixture, Propfind_Depth0_File) {
    WebDavAdapter adapter(vfs, "/dav/");

    std::filesystem::path root = std::filesystem::current_path() / "webdav_adapter_tests";
    std::filesystem::create_directories(root);
    auto fpath = root / "file.txt";
    std::string contents = writeTextFile(fpath, "hello world!");

    HttpRequestLite req;
    req.method = "PROPFIND";
    req.urlPath = std::string("/dav/") + (root.filename().string()) + "/file.txt";

    auto res = adapter.handlePropfind(req, 0);
    ASSERT_EQ(res.status, 207);

    // Response should not be empty
    ASSERT_FALSE(res.body.empty());

    // Self href is present
    EXPECT_TRUE(contains(res.body, std::string("<D:href>") + req.urlPath + "</D:href>"));

    // For a file, resourcetype must not contain <D:collection/>
    EXPECT_FALSE(contains(res.body, "<D:resourcetype><D:collection/></D:resourcetype>"));

    // Content length tag should be present
    EXPECT_TRUE(contains(res.body, "<D:getcontentlength>"));
}

TEST_F(WebDavAdapterFixture, Propfind_Depth1_Directory) {
    WebDavAdapter adapter(vfs, "/dav/");

    std::filesystem::path root = std::filesystem::current_path() / "webdav_adapter_tests2";
    std::filesystem::create_directories(root);
    auto childFile = root / "child.txt";
    writeTextFile(childFile, "abc");
    auto childDir = root / "sub";
    std::filesystem::create_directories(childDir);

    HttpRequestLite req;
    req.method = "PROPFIND";
    req.urlPath = std::string("/dav/") + (root.filename().string()) + "/";  // directory URL ends with '/'

    auto res = adapter.handlePropfind(req, 1);
    ASSERT_EQ(res.status, 207);

    // Response should not be empty
    ASSERT_FALSE(res.body.empty());
    // Self entry href should be present
    EXPECT_TRUE(contains(res.body, std::string("<D:href>") + req.urlPath + "</D:href>"));

    // Child file should be present with length 3
    EXPECT_TRUE(contains(res.body, std::string("<D:href>") + req.urlPath + "child.txt</D:href>"));
    EXPECT_TRUE(contains(res.body, "<D:getcontentlength>3</D:getcontentlength>"));

    // Child directory should be present with trailing slash
    EXPECT_TRUE(contains(res.body, std::string("<D:href>") + req.urlPath + "sub/</D:href>"));
}
