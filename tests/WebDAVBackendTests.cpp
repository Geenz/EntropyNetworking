#include <gtest/gtest.h>
#include "MockHttpConnection.h"
#include "Networking/WebDAV/WebDAVConnection.h"
#include "Networking/WebDAV/WebDAVFileSystemBackend.h"

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Networking::WebDAV;
using namespace EntropyEngine::Networking::Tests;
using namespace EntropyEngine::Core::IO;

static std::string httpOkWithLen(const std::string& body, const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o; o << "HTTP/1.1 200 OK\r\n";
    o << "Content-Length: " << body.size() << "\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n" << body; return o.str();
}

static std::string davDepth0Body(const std::string& href, bool collection) {
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    x << "<D:multistatus xmlns:D=\"DAV:\">\n";
    x << "  <D:response>\n";
    x << "    <D:href>" << href << "</D:href>\n";
    x << "    <D:propstat><D:prop>";
    if (collection) x << "<D:resourcetype><D:collection/></D:resourcetype>";
    else x << "<D:getcontentlength>123</D:getcontentlength>";
    x << "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
    x << "  </D:response>\n";
    x << "</D:multistatus>\n";
    return x.str();
}

TEST(WebDAVBackend, ExistsFileRelativeHref) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    WebDAVFileSystemBackend backend(client, bcfg);

    // Script a PROPFIND Depth 0 response containing the requested href
    std::string xml = davDepth0Body("/dav/assets/file.bin", false);
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkWithLen(xml, {{"Content-Type","application/xml; charset=utf-8"}}));
    mock->enqueueResponse(std::move(resp));

    bool ex = backend.exists("/assets/file.bin");
    EXPECT_TRUE(ex);
}

TEST(WebDAVBackend, ExistsDirAbsoluteHref) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "assets.internal" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    WebDAVFileSystemBackend backend(client, bcfg);

    // Absolute href in response should be matched after normalization
    std::string xml = davDepth0Body("https://assets.internal/dav/assets/", true);
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkWithLen(xml, {{"Content-Type","application/xml; charset=utf-8"}}));
    mock->enqueueResponse(std::move(resp));

    bool ex = backend.exists("/assets/");
    EXPECT_TRUE(ex);
}

TEST(WebDAVBackend, ExistsNotFound) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    WebDAVFileSystemBackend backend(client, bcfg);

    // 404 response
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    mock->enqueueResponse(std::move(resp));

    bool ex = backend.exists("/assets/missing.bin");
    EXPECT_FALSE(ex);
}


#include <VirtualFileSystem/VirtualFileSystem.h>

using EntropyEngine::Core::Concurrency::WorkContractGroup;

static std::string httpPartialWithLen(const std::string& body,
                                      const std::string& contentRange,
                                      const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o; o << "HTTP/1.1 206 Partial Content\r\n";
    o << "Content-Length: " << body.size() << "\r\n";
    o << "Content-Range: " << contentRange << "\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n" << body; return o.str();
}

static std::string httpOkChunked(const std::string& body,
                                 const std::vector<std::pair<std::string,std::string>>& headers = {}) {
    std::ostringstream o; o << "HTTP/1.1 200 OK\r\n";
    o << "Transfer-Encoding: chunked\r\n";
    for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
    o << "\r\n";
    std::ostringstream chunk; chunk << std::hex << body.size();
    o << chunk.str() << "\r\n" << body << "\r\n0\r\n\r\n";
    return o.str();
}

static std::string davDepth1ListBody(const std::string& selfHref,
                                     const std::vector<std::pair<std::string,bool>>& children) {
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    x << "<D:multistatus xmlns:D=\"DAV:\">\n";
    x << "  <D:response>\n";
    x << "    <D:href>" << selfHref << "</D:href>\n";
    x << "    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
    x << "  </D:response>\n";
    for (auto& ch : children) {
        x << "  <D:response>\n";
        x << "    <D:href>" << ch.first << (ch.second?"/":"") << "</D:href>\n";
        x << "    <D:propstat><D:prop>";
        if (ch.second) x << "<D:resourcetype><D:collection/></D:resourcetype>";
        else x << "<D:getcontentlength>10</D:getcontentlength>";
        x << "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
        x << "  </D:response>\n";
    }
    x << "</D:multistatus>";
    return x.str();
}

TEST(WebDAVBackend, ReadFile_ContentLength) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    WorkContractGroup group(2000);
    EntropyEngine::Core::IO::VirtualFileSystem vfs(&group);
    // Attach backend to VFS without mounting
    backend->setVirtualFileSystem(&vfs);

    std::string body = "abcdefghij";
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkWithLen(body));
    mock->enqueueResponse(std::move(resp));

    auto h = backend->readFile("/assets/file.bin");
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    auto bytes = h.contentsBytes();
    ASSERT_EQ(bytes.size(), body.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), body);
}

TEST(WebDAVBackend, ReadFile_Chunked) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    WorkContractGroup group(2000);
    EntropyEngine::Core::IO::VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    std::string body = "chunked-body";
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(httpOkChunked(body));
    mock->enqueueResponse(std::move(resp));

    auto h = backend->readFile("/assets/file2.bin");
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    auto bytes = h.contentsBytes();
    ASSERT_EQ(bytes.size(), body.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), body);
}

TEST(WebDAVBackend, ReadFile_Range206) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    WorkContractGroup group(2000);
    EntropyEngine::Core::IO::VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    std::string part = "56789";
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back(
        httpPartialWithLen(part, "bytes 5-9/10")
    );
    mock->enqueueResponse(std::move(resp));

    ReadOptions ropt; ropt.offset = 5; ropt.length = 5;
    auto h = backend->readFile("/assets/range.bin", ropt);
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    auto bytes = h.contentsBytes();
    ASSERT_EQ(bytes.size(), part.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), part);
}

TEST(WebDAVBackend, GetMetadata_FileAndDir) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    WorkContractGroup group(2000);
    EntropyEngine::Core::IO::VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    // File metadata
    std::string xmlFile;
    {
        std::ostringstream x;
        x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        x << "<D:multistatus xmlns:D=\"DAV:\">\n";
        x << "  <D:response>\n";
        x << "    <D:href>/dav/assets/meta.txt</D:href>\n";
        x << "    <D:propstat><D:prop><D:getcontentlength>321</D:getcontentlength><D:getlastmodified>Sun, 06 Nov 1994 08:49:37 GMT</D:getlastmodified><D:getcontenttype>text/plain</D:getcontenttype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
        x << "  </D:response>\n";
        x << "</D:multistatus>\n";
        xmlFile = x.str();
    }
    MockHttpConnection::ScriptedResponse r1; r1.chunks.push_back(httpOkWithLen(xmlFile, {{"Content-Type","application/xml; charset=utf-8"}}));
    mock->enqueueResponse(std::move(r1));

    auto m1 = backend->getMetadata("/assets/meta.txt");
    m1.wait();
    ASSERT_EQ(m1.status(), FileOpStatus::Complete);
    ASSERT_TRUE(m1.metadata().has_value());
    auto md = *m1.metadata();
    EXPECT_TRUE(md.exists);
    EXPECT_FALSE(md.isDirectory);
    EXPECT_EQ(md.size, 321u);
    ASSERT_TRUE(md.mimeType.has_value());
    EXPECT_EQ(*md.mimeType, std::string("text/plain"));

    // Directory metadata
    std::string xmlDir;
    {
        std::ostringstream x;
        x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        x << "<D:multistatus xmlns:D=\"DAV:\">\n";
        x << "  <D:response>\n";
        x << "    <D:href>/dav/assets/</D:href>\n";
        x << "    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
        x << "  </D:response>\n";
        x << "</D:multistatus>\n";
        xmlDir = x.str();
    }
    MockHttpConnection::ScriptedResponse r2; r2.chunks.push_back(httpOkWithLen(xmlDir, {{"Content-Type","application/xml; charset=utf-8"}}));
    mock->enqueueResponse(std::move(r2));

    auto m2 = backend->getMetadata("/assets/");
    m2.wait();
    ASSERT_EQ(m2.status(), FileOpStatus::Complete);
    ASSERT_TRUE(m2.metadata().has_value());
    auto md2 = *m2.metadata();
    EXPECT_TRUE(md2.exists);
    EXPECT_TRUE(md2.isDirectory);
}

TEST(WebDAVBackend, ListDirectory_SkipSelf_AbsoluteHref) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    WorkContractGroup group(2000);
    EntropyEngine::Core::IO::VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    std::string xml = davDepth1ListBody("https://example.com/dav/assets/", {
        {"https://example.com/dav/assets/file1.bin", false},
        {"https://example.com/dav/assets/sub", true}
    });

    MockHttpConnection::ScriptedResponse r; r.chunks.push_back(httpOkWithLen(xml, {{"Content-Type","application/xml; charset=utf-8"}}));
    mock->enqueueResponse(std::move(r));

    auto h = backend->listDirectory("/assets/");
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Complete);
    const auto& entries = h.directoryEntries();
    ASSERT_EQ(entries.size(), 2u); // self skipped
    EXPECT_EQ(entries[0].metadata.isRegularFile, true);
    EXPECT_EQ(entries[1].metadata.isDirectory, true);
}

TEST(WebDAVBackend, ErrorMapping_404_ReadFile) {
    auto mock = std::make_shared<MockHttpConnection>();
    WebDAVConnection::Config cfg{ .host = "example.com" };
    auto client = std::make_shared<WebDAVConnection>(mock, cfg);

    WebDAVFileSystemBackend::Config bcfg{ .baseUrl = "/dav" };
    auto backend = std::make_shared<WebDAVFileSystemBackend>(client, bcfg);

    WorkContractGroup group(2000);
    EntropyEngine::Core::IO::VirtualFileSystem vfs(&group);
    backend->setVirtualFileSystem(&vfs);

    // 404 for GET
    MockHttpConnection::ScriptedResponse resp; resp.chunks.push_back("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    mock->enqueueResponse(std::move(resp));

    auto h = backend->readFile("/assets/missing.bin");
    h.wait();
    ASSERT_EQ(h.status(), FileOpStatus::Failed);
    EXPECT_EQ(h.errorInfo().code, FileError::FileNotFound);
}
