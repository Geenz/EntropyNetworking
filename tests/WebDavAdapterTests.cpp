#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "Networking/Protocol/WebDavAdapter.h"
#include <VirtualFileSystem/VirtualFileSystem.h>

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core::IO;

namespace {

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

}

TEST(WebDavAdapter, Propfind_Depth0_File) {
    using EntropyEngine::Core::Concurrency::WorkContractGroup;
    WorkContractGroup group(2000);
    auto vfs = std::make_shared<VirtualFileSystem>(&group);
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

TEST(WebDavAdapter, Propfind_Depth1_Directory) {
    using EntropyEngine::Core::Concurrency::WorkContractGroup;
    WorkContractGroup group(2000);
    auto vfs = std::make_shared<VirtualFileSystem>(&group);
    WebDavAdapter adapter(vfs, "/dav/");

    std::filesystem::path root = std::filesystem::current_path() / "webdav_adapter_tests2";
    std::filesystem::create_directories(root);
    auto childFile = root / "child.txt";
    writeTextFile(childFile, "abc");
    auto childDir = root / "sub";
    std::filesystem::create_directories(childDir);

    HttpRequestLite req;
    req.method = "PROPFIND";
    req.urlPath = std::string("/dav/") + (root.filename().string()) + "/"; // directory URL ends with '/'

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
