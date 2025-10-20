#include <gtest/gtest.h>
#include "Networking/WebDAV/WebDAVPropfindParser.h"
#include "Networking/WebDAV/WebDAVUtils.h"
#include <sstream>

using namespace EntropyEngine::Networking::WebDAV;

static std::string davDepth1Body(const std::string& selfHref) {
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    x << "<D:multistatus xmlns:D=\"DAV:\">\n";
    x << "  <D:response>\n";
    x << "    <D:href>" << selfHref << "</D:href>\n";
    x << "    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
    x << "  </D:response>\n";
    x << "  <D:response>\n";
    x << "    <D:href>" << selfHref << "file1.bin</D:href>\n";
    x << "    <D:propstat><D:prop><D:getcontentlength>123</D:getcontentlength><D:getcontenttype>application/octet-stream</D:getcontenttype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
    x << "  </D:response>\n";
    x << "  <D:response>\n";
    x << "    <D:href>" << selfHref << "subdir/</D:href>\n";
    x << "    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
    x << "  </D:response>\n";
    x << "</D:multistatus>\n";
    return x.str();
}

TEST(WebDAVPropfindParser, Depth0File) {
    std::string xml;
    {
        std::ostringstream x;
        x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        x << "<D:multistatus xmlns:D=\"DAV:\">\n";
        x << "  <D:response>\n";
        x << "    <D:href>/dav/a.txt</D:href>\n";
        x << "    <D:propstat><D:prop><D:getcontentlength>42</D:getcontentlength><D:getlastmodified>Sun, 06 Nov 1994 08:49:37 GMT</D:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
        x << "  </D:response>\n";
        x << "</D:multistatus>\n";
        xml = x.str();
    }

    std::vector<uint8_t> bytes(xml.begin(), xml.end());
    auto resources = parsePropfindXml(bytes);
    ASSERT_EQ(resources.size(), 1);
    EXPECT_EQ(resources[0].href, "/dav/a.txt");
    EXPECT_FALSE(resources[0].isCollection);
    EXPECT_EQ(resources[0].contentLength, 42u);
    EXPECT_TRUE(resources[0].lastModified.has_value());
}

TEST(WebDAVPropfindParser, Depth1List) {
    auto xml = davDepth1Body("/dav/assets/");
    std::vector<uint8_t> bytes(xml.begin(), xml.end());
    auto resources = parsePropfindXml(bytes);
    ASSERT_EQ(resources.size(), 3);

    // Self
    EXPECT_EQ(resources[0].href, "/dav/assets/");
    EXPECT_TRUE(resources[0].isCollection);

    // Child file
    EXPECT_EQ(resources[1].href, "/dav/assets/file1.bin");
    EXPECT_FALSE(resources[1].isCollection);
    EXPECT_EQ(resources[1].contentLength, 123u);
    ASSERT_TRUE(resources[1].contentType.has_value());
    EXPECT_EQ(*resources[1].contentType, "application/octet-stream");

    // Child dir
    EXPECT_EQ(resources[2].href, "/dav/assets/subdir/");
    EXPECT_TRUE(resources[2].isCollection);
}

TEST(WebDAVPropfindParser, AbsoluteHref) {
    // Ensure absolute url is accepted and preserved in href
    std::string xml;
    {
        std::ostringstream x;
        x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        x << "<D:multistatus xmlns:D=\"DAV:\">\n";
        x << "  <D:response>\n";
        x << "    <D:href>https://example.com/dav/root/</D:href>\n";
        x << "    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n";
        x << "  </D:response>\n";
        x << "</D:multistatus>\n";
        xml = x.str();
    }
    std::vector<uint8_t> bytes(xml.begin(), xml.end());
    auto resources = parsePropfindXml(bytes);
    ASSERT_EQ(resources.size(), 1);
    EXPECT_EQ(resources[0].href, "https://example.com/dav/root/");
}
