/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "Utils_test"

#include <gtest/gtest.h>
#include <utils/String8.h>
#include <utils/Errors.h>
#include <fcntl.h>
#include <unistd.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/Utils.h>

namespace android {

class UtilsTest : public ::testing::Test {
};

TEST_F(UtilsTest, TestFourCC) {
    ASSERT_EQ(FOURCC('s', 't', 'm' , 'u'), 'stmu');
}

TEST_F(UtilsTest, TestMathTemplates) {
    ASSERT_EQ(divRound(-10, -4), 3);
    ASSERT_EQ(divRound(-11, -4), 3);
    ASSERT_EQ(divRound(-12, -4), 3);
    ASSERT_EQ(divRound(-13, -4), 3);
    ASSERT_EQ(divRound(-14, -4), 4);

    ASSERT_EQ(divRound(10, -4), -3);
    ASSERT_EQ(divRound(11, -4), -3);
    ASSERT_EQ(divRound(12, -4), -3);
    ASSERT_EQ(divRound(13, -4), -3);
    ASSERT_EQ(divRound(14, -4), -4);

    ASSERT_EQ(divRound(-10, 4), -3);
    ASSERT_EQ(divRound(-11, 4), -3);
    ASSERT_EQ(divRound(-12, 4), -3);
    ASSERT_EQ(divRound(-13, 4), -3);
    ASSERT_EQ(divRound(-14, 4), -4);

    ASSERT_EQ(divRound(10, 4), 3);
    ASSERT_EQ(divRound(11, 4), 3);
    ASSERT_EQ(divRound(12, 4), 3);
    ASSERT_EQ(divRound(13, 4), 3);
    ASSERT_EQ(divRound(14, 4), 4);

    ASSERT_EQ(divUp(-11, -4), 3);
    ASSERT_EQ(divUp(-12, -4), 3);
    ASSERT_EQ(divUp(-13, -4), 4);

    ASSERT_EQ(divUp(11, -4), -2);
    ASSERT_EQ(divUp(12, -4), -3);
    ASSERT_EQ(divUp(13, -4), -3);

    ASSERT_EQ(divUp(-11, 4), -2);
    ASSERT_EQ(divUp(-12, 4), -3);
    ASSERT_EQ(divUp(-13, 4), -3);

    ASSERT_EQ(divUp(11, 4), 3);
    ASSERT_EQ(divUp(12, 4), 3);
    ASSERT_EQ(divUp(13, 4), 4);

    ASSERT_EQ(abs(5L), 5L);
    ASSERT_EQ(abs(-25), 25);

    ASSERT_EQ(min(5.6f, 6.0f), 5.6f);
    ASSERT_EQ(min(6.0f, 5.6f), 5.6f);
    ASSERT_EQ(min(-4.3, 8.6), -4.3);
    ASSERT_EQ(min(8.6, -4.3), -4.3);

    ASSERT_EQ(max(5.6f, 6.0f), 6.0f);
    ASSERT_EQ(max(6.0f, 5.6f), 6.0f);
    ASSERT_EQ(max(-4.3, 8.6), 8.6);
    ASSERT_EQ(max(8.6, -4.3), 8.6);

    ASSERT_FALSE(isInRange(-43, 86u, -44));
    ASSERT_TRUE(isInRange(-43, 87u, -43));
    ASSERT_TRUE(isInRange(-43, 88u, -1));
    ASSERT_TRUE(isInRange(-43, 89u, 0));
    ASSERT_TRUE(isInRange(-43, 90u, 46));
    ASSERT_FALSE(isInRange(-43, 91u, 48));
    ASSERT_FALSE(isInRange(-43, 92u, 50));

    ASSERT_FALSE(isInRange(43, 86u, 42));
    ASSERT_TRUE(isInRange(43, 87u, 43));
    ASSERT_TRUE(isInRange(43, 88u, 44));
    ASSERT_TRUE(isInRange(43, 89u, 131));
    ASSERT_FALSE(isInRange(43, 90u, 133));
    ASSERT_FALSE(isInRange(43, 91u, 135));

    ASSERT_FALSE(isInRange(43u, 86u, 42u));
    ASSERT_TRUE(isInRange(43u, 85u, 43u));
    ASSERT_TRUE(isInRange(43u, 84u, 44u));
    ASSERT_TRUE(isInRange(43u, 83u, 125u));
    ASSERT_FALSE(isInRange(43u, 82u, 125u));
    ASSERT_FALSE(isInRange(43u, 81u, 125u));

    ASSERT_FALSE(isInRange(-43, ~0u, 43));
    ASSERT_FALSE(isInRange(-43, ~0u, 44));
    ASSERT_FALSE(isInRange(-43, ~0u, ~0));
    ASSERT_FALSE(isInRange(-43, ~0u, 41));
    ASSERT_FALSE(isInRange(-43, ~0u, 40));

    ASSERT_FALSE(isInRange(43u, ~0u, 43u));
    ASSERT_FALSE(isInRange(43u, ~0u, 41u));
    ASSERT_FALSE(isInRange(43u, ~0u, 40u));
    ASSERT_FALSE(isInRange(43u, ~0u, ~0u));

    ASSERT_FALSE(isInRange(-43, 86u, -44, 0u));
    ASSERT_FALSE(isInRange(-43, 86u, -44, 1u));
    ASSERT_FALSE(isInRange(-43, 86u, -44, 2u));
    ASSERT_FALSE(isInRange(-43, 86u, -44, ~0u));
    ASSERT_TRUE(isInRange(-43, 87u, -43, 0u));
    ASSERT_TRUE(isInRange(-43, 87u, -43, 1u));
    ASSERT_TRUE(isInRange(-43, 87u, -43, 86u));
    ASSERT_TRUE(isInRange(-43, 87u, -43, 87u));
    ASSERT_FALSE(isInRange(-43, 87u, -43, 88u));
    ASSERT_FALSE(isInRange(-43, 87u, -43, ~0u));
    ASSERT_TRUE(isInRange(-43, 88u, -1, 0u));
    ASSERT_TRUE(isInRange(-43, 88u, -1, 45u));
    ASSERT_TRUE(isInRange(-43, 88u, -1, 46u));
    ASSERT_FALSE(isInRange(-43, 88u, -1, 47u));
    ASSERT_FALSE(isInRange(-43, 88u, -1, ~3u));
    ASSERT_TRUE(isInRange(-43, 90u, 46, 0u));
    ASSERT_TRUE(isInRange(-43, 90u, 46, 1u));
    ASSERT_FALSE(isInRange(-43, 90u, 46, 2u));
    ASSERT_FALSE(isInRange(-43, 91u, 48, 0u));
    ASSERT_FALSE(isInRange(-43, 91u, 48, 2u));
    ASSERT_FALSE(isInRange(-43, 91u, 48, ~6u));
    ASSERT_FALSE(isInRange(-43, 92u, 50, 0u));
    ASSERT_FALSE(isInRange(-43, 92u, 50, 1u));

    ASSERT_FALSE(isInRange(43u, 86u, 42u, 0u));
    ASSERT_FALSE(isInRange(43u, 86u, 42u, 1u));
    ASSERT_FALSE(isInRange(43u, 86u, 42u, 2u));
    ASSERT_FALSE(isInRange(43u, 86u, 42u, ~0u));
    ASSERT_TRUE(isInRange(43u, 87u, 43u, 0u));
    ASSERT_TRUE(isInRange(43u, 87u, 43u, 1u));
    ASSERT_TRUE(isInRange(43u, 87u, 43u, 86u));
    ASSERT_TRUE(isInRange(43u, 87u, 43u, 87u));
    ASSERT_FALSE(isInRange(43u, 87u, 43u, 88u));
    ASSERT_FALSE(isInRange(43u, 87u, 43u, ~0u));
    ASSERT_TRUE(isInRange(43u, 88u, 60u, 0u));
    ASSERT_TRUE(isInRange(43u, 88u, 60u, 70u));
    ASSERT_TRUE(isInRange(43u, 88u, 60u, 71u));
    ASSERT_FALSE(isInRange(43u, 88u, 60u, 72u));
    ASSERT_FALSE(isInRange(43u, 88u, 60u, ~3u));
    ASSERT_TRUE(isInRange(43u, 90u, 132u, 0u));
    ASSERT_TRUE(isInRange(43u, 90u, 132u, 1u));
    ASSERT_FALSE(isInRange(43u, 90u, 132u, 2u));
    ASSERT_FALSE(isInRange(43u, 91u, 134u, 0u));
    ASSERT_FALSE(isInRange(43u, 91u, 134u, 2u));
    ASSERT_FALSE(isInRange(43u, 91u, 134u, ~6u));
    ASSERT_FALSE(isInRange(43u, 92u, 136u, 0u));
    ASSERT_FALSE(isInRange(43u, 92u, 136u, 1u));

    ASSERT_EQ(periodicError(124, 100), 24);
    ASSERT_EQ(periodicError(288, 100), 12);
    ASSERT_EQ(periodicError(-345, 100), 45);
    ASSERT_EQ(periodicError(-493, 100), 7);
    ASSERT_EQ(periodicError(-550, 100), 50);
    ASSERT_EQ(periodicError(-600, 100), 0);
}

} // namespace android
