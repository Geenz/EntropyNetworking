/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <gtest/gtest.h>

#include <thread>

#include "../src/Networking/Transport/ConnectionManager.h"

using namespace EntropyEngine::Networking;

// Basic lifecycle tests that don't require a live backend
TEST(ConnectionManagerLifecycle, HandleValidityAndCloseFreesSlot) {
    ConnectionManager mgr(2);

    auto h = mgr.openLocalConnection("/tmp/entropy_lifecycle_test.sock");
    ASSERT_TRUE(h.valid());
    EXPECT_EQ(mgr.activeCount(), 1u);

    // Copy the handle and then close the original; copied handle should also become invalid afterwards
    auto copy = h;
    auto res = h.close();
    ASSERT_TRUE(res.success()) << res.errorMessage;

    EXPECT_FALSE(h.valid());
    EXPECT_FALSE(copy.valid());
    EXPECT_EQ(mgr.activeCount(), 0u);

    // Reopen should succeed and produce a valid handle again
    auto h2 = mgr.openLocalConnection("/tmp/entropy_lifecycle_test.sock");
    ASSERT_TRUE(h2.valid());
    EXPECT_EQ(mgr.activeCount(), 1u);
}
