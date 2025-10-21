#include <gtest/gtest.h>
#include <httplib.h>
#include "DavTree.h"
#include "MiniDavServer.h"

TEST(WebDavMiniServer, Smoke) {
    DavTree tree;
    tree.addDir("/");
    tree.addFile("/foo.txt", "foo", "text/plain");

    MiniDavServer srv(tree, "/dav/");
    srv.start();

    httplib::Client cli("127.0.0.1", srv.port());
    auto ropt = cli.Options("/dav/");
    ASSERT_TRUE(ropt);
    EXPECT_EQ(ropt->status, 200);

    auto rhead = cli.Head("/dav/foo.txt");
    ASSERT_TRUE(rhead);
    EXPECT_EQ(rhead->status, 200);
    EXPECT_EQ(rhead->get_header_value("Content-Length"), "3");

    auto rget = cli.Get("/dav/foo.txt");
    ASSERT_TRUE(rget);
    EXPECT_EQ(rget->status, 200);
    EXPECT_EQ(rget->body, "foo");

    // Note: PROPFIND not tested here due to cpp-httplib client lacking custom method send() in this environment.
    // PROPFIND behavior is covered by non-endpoint adapter tests.

    srv.stop();
}
