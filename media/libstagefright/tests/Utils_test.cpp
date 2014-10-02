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

    ASSERT_EQ(periodicError(124, 100), 24);
    ASSERT_EQ(periodicError(288, 100), 12);
    ASSERT_EQ(periodicError(-345, 100), 45);
    ASSERT_EQ(periodicError(-493, 100), 7);
    ASSERT_EQ(periodicError(-550, 100), 50);
    ASSERT_EQ(periodicError(-600, 100), 0);
}

} // namespace android
