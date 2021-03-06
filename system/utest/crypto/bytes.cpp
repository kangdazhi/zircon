// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <crypto/bytes.h>
#include <unittest/unittest.h>
#include <zircon/types.h>

#include "utils.h"

namespace crypto {
namespace testing {
namespace {

const size_t kSize = 1024;

bool TestInit(void) {
    BEGIN_TEST;
    Bytes bytes;
    EXPECT_OK(bytes.Init(kSize, 0xff));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());

    EXPECT_ZX(bytes.Init(size_t(-1)), ZX_ERR_NO_MEMORY);
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());

    EXPECT_OK(bytes.Init(kSize, 0xff));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());
    EXPECT_TRUE(AllEqual(bytes.get(), 0xff, 0, kSize));

    EXPECT_OK(bytes.Init(kSize));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());
    EXPECT_TRUE(AllEqual(bytes.get(), 0, 0, kSize));

    EXPECT_OK(bytes.Init(0));
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());
    END_TEST;
}

bool TestResize(void) {
    BEGIN_TEST;
    Bytes bytes;
    EXPECT_OK(bytes.Resize(kSize, 0xff));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());

    EXPECT_ZX(bytes.Resize(size_t(-1)), ZX_ERR_NO_MEMORY);
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());
    EXPECT_TRUE(AllEqual(bytes.get(), 0xff, 0, kSize));

    EXPECT_OK(bytes.Resize(kSize));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());
    EXPECT_TRUE(AllEqual(bytes.get(), 0xff, 0, kSize));

    EXPECT_OK(bytes.Resize(kSize / 2));
    EXPECT_EQ(bytes.len(), kSize / 2);
    EXPECT_NONNULL(bytes.get());
    EXPECT_TRUE(AllEqual(bytes.get(), 0xff, 0, kSize / 2));

    EXPECT_OK(bytes.Resize(kSize));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_NONNULL(bytes.get());
    EXPECT_TRUE(AllEqual(bytes.get(), 0xff, 0, kSize / 2));
    EXPECT_TRUE(AllEqual(bytes.get(), 0, kSize / 2, kSize / 2));

    EXPECT_OK(bytes.Resize(0));
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());
    END_TEST;
}

bool TestCopy(void) {
    BEGIN_TEST;
    Bytes bytes;
    ASSERT_OK(bytes.Init(kSize));

    uint8_t buf[kSize];
    memset(buf, 2, kSize);
    EXPECT_ZX(bytes.Copy(nullptr, kSize, kSize), ZX_ERR_INVALID_ARGS);
    EXPECT_OK(bytes.Copy(buf, 0, kSize * 10));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_TRUE(AllEqual(bytes.get(), 0, 0, kSize));

    EXPECT_OK(bytes.Copy(buf, kSize, kSize));
    EXPECT_TRUE(AllEqual(bytes.get(), 0, 0, kSize));
    EXPECT_TRUE(AllEqual(bytes.get(), 2, kSize, kSize));

    memset(buf, 1, kSize);
    EXPECT_OK(bytes.Copy(buf, kSize / 2, kSize / 2));
    EXPECT_TRUE(AllEqual(bytes.get(), 0, 0, kSize / 2));
    EXPECT_TRUE(AllEqual(bytes.get(), 1, kSize / 2, kSize / 2));
    EXPECT_TRUE(AllEqual(bytes.get(), 2, kSize, kSize));

    bytes.Reset();
    EXPECT_OK(bytes.Copy(buf, kSize));
    EXPECT_EQ(bytes.len(), kSize);
    EXPECT_TRUE(AllEqual(bytes.get(), 1, 0, kSize));
    END_TEST;
}

// This test only checks that the routine basically functions; it does NOT assure anything about the
// quality of the entropy.  That topic is beyond the scope of a deterministic unit test.
bool TestRandomize(void) {
    BEGIN_TEST;
    Bytes bytes;
    EXPECT_OK(bytes.Randomize(0));

    // Test various sizes, doubling as long as it does not exceed the max draw length.
    for (size_t n = 16; n <= ZX_CPRNG_DRAW_MAX_LEN; n *= 2) {
        EXPECT_OK(bytes.Randomize(n));
        EXPECT_FALSE(AllEqual(bytes.get(), 0, 0, n));
    }
    END_TEST;
}

bool TestIncrement(void) {
    BEGIN_TEST;
    Bytes bytes;
    EXPECT_ZX(bytes.Increment(), ZX_ERR_OUT_OF_RANGE);

    ASSERT_OK(bytes.Resize(1));
    EXPECT_OK(bytes.Increment());
    EXPECT_EQ(bytes[0], 1U);
    bytes[0] = 0xFF;
    EXPECT_ZX(bytes.Increment(), ZX_ERR_OUT_OF_RANGE);

    ASSERT_OK(bytes.Resize(2));
    EXPECT_OK(bytes.Increment());
    EXPECT_EQ(bytes[0], 0U);
    EXPECT_EQ(bytes[1], 1U);
    EXPECT_OK(bytes.Increment());
    EXPECT_EQ(bytes[0], 0U);
    EXPECT_EQ(bytes[1], 2U);
    bytes[1] = 0xFF;
    EXPECT_OK(bytes.Increment());
    EXPECT_EQ(bytes[0], 1U);
    EXPECT_EQ(bytes[1], 0U);
    bytes[0] = 0xFF;
    bytes[1] = 0xFF;
    EXPECT_ZX(bytes.Increment(), ZX_ERR_OUT_OF_RANGE);

    ASSERT_OK(bytes.Resize(3));
    bytes[0] = 0;
    bytes[1] = 0;
    bytes[2] = 1;
    EXPECT_OK(bytes.Increment());
    EXPECT_EQ(bytes[0], 0U);
    EXPECT_EQ(bytes[1], 0U);
    EXPECT_EQ(bytes[2], 2U);
    END_TEST;
}

bool TestRelease(void) {
    BEGIN_TEST;
    Bytes bytes;
    size_t len;
    auto buf = bytes.Release(&len);
    EXPECT_NULL(buf.get());
    EXPECT_EQ(len, 0U);
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());

    ASSERT_OK(bytes.Init(kSize, 0xff));
    buf = bytes.Release(&len);
    EXPECT_NONNULL(buf.get());
    EXPECT_EQ(len, kSize);
    EXPECT_TRUE(AllEqual(buf.get(), 0xff, 0, kSize));
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());
    END_TEST;
}

bool TestReset(void) {
    BEGIN_TEST;
    Bytes bytes;
    bytes.Reset();
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());

    ASSERT_OK(bytes.Init(kSize, 0xff));
    bytes.Reset();
    EXPECT_EQ(bytes.len(), 0U);
    EXPECT_NULL(bytes.get());
    END_TEST;
}

bool TestArrayAccess(void) {
    BEGIN_TEST;
    Bytes bytes;
    ASSERT_OK(bytes.Init(kSize, 1));
    for (size_t i = 0; i < kSize; ++i) {
        EXPECT_EQ(bytes[i], 1);
        bytes[i] = 2;
    }
    EXPECT_TRUE(AllEqual(bytes.get(), 2, 0, kSize));
    END_TEST;
}

bool TestComparison(void) {
    BEGIN_TEST;
    Bytes bytes1, bytes2;
    ASSERT_OK(bytes1.Init(kSize, 1));
    ASSERT_OK(bytes2.Copy(bytes1.get(), bytes1.len()));
    EXPECT_TRUE(bytes1 == bytes1);
    EXPECT_TRUE(bytes2 == bytes2);
    EXPECT_FALSE(bytes1 != bytes1);
    EXPECT_FALSE(bytes2 != bytes2);
    EXPECT_TRUE(bytes1 == bytes2);
    EXPECT_TRUE(bytes2 == bytes1);
    EXPECT_FALSE(bytes1 != bytes2);
    EXPECT_FALSE(bytes2 != bytes1);

    ASSERT_OK(bytes2.Init(kSize, 2));
    EXPECT_TRUE(bytes1 == bytes1);
    EXPECT_TRUE(bytes2 == bytes2);
    EXPECT_FALSE(bytes1 != bytes1);
    EXPECT_FALSE(bytes2 != bytes2);
    EXPECT_FALSE(bytes1 == bytes2);
    EXPECT_FALSE(bytes2 == bytes1);
    EXPECT_TRUE(bytes1 != bytes2);
    EXPECT_TRUE(bytes2 != bytes1);
    END_TEST;
}

BEGIN_TEST_CASE(BytesTest)
RUN_TEST(TestInit)
RUN_TEST(TestResize)
RUN_TEST(TestCopy)
RUN_TEST(TestRandomize)
RUN_TEST(TestIncrement)
RUN_TEST(TestRelease)
RUN_TEST(TestReset)
RUN_TEST(TestArrayAccess)
RUN_TEST(TestComparison)
END_TEST_CASE(BytesTest)

} // namespace
} // namespace testing
} // namespace crypto
