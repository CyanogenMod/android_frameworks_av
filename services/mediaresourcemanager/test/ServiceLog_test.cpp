/*
 * Copyright 2015 The Android Open Source Project
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
#define LOG_TAG "ServiceLog_test"
#include <utils/Log.h>

#include <gtest/gtest.h>

#include "ServiceLog.h"

namespace android {

class ServiceLogTest : public ::testing::Test {
public:
    ServiceLogTest() : mServiceLog(new ServiceLog(3)) {
    }

protected:
    sp<ServiceLog> mServiceLog;
};

TEST_F(ServiceLogTest, addThenToString) {
    mServiceLog->add(String8("log1"));
    EXPECT_TRUE(mServiceLog->toString().contains("log1"));
    ALOGV("toString:\n%s", mServiceLog->toString().string());

    mServiceLog->add(String8("log2"));
    EXPECT_TRUE(mServiceLog->toString().contains("log1"));
    EXPECT_TRUE(mServiceLog->toString().contains("log2"));
    ALOGV("toString:\n%s", mServiceLog->toString().string());

    mServiceLog->add(String8("log3"));
    EXPECT_TRUE(mServiceLog->toString().contains("log1"));
    EXPECT_TRUE(mServiceLog->toString().contains("log2"));
    EXPECT_TRUE(mServiceLog->toString().contains("log3"));
    ALOGV("toString:\n%s", mServiceLog->toString().string());

    mServiceLog->add(String8("log4"));
    EXPECT_FALSE(mServiceLog->toString().contains("log1"));
    EXPECT_TRUE(mServiceLog->toString().contains("log2"));
    EXPECT_TRUE(mServiceLog->toString().contains("log3"));
    EXPECT_TRUE(mServiceLog->toString().contains("log4"));
    ALOGV("toString:\n%s", mServiceLog->toString().string());

    mServiceLog->add(String8("log5"));
    EXPECT_FALSE(mServiceLog->toString().contains("log1"));
    EXPECT_FALSE(mServiceLog->toString().contains("log2"));
    EXPECT_TRUE(mServiceLog->toString().contains("log3"));
    EXPECT_TRUE(mServiceLog->toString().contains("log4"));
    EXPECT_TRUE(mServiceLog->toString().contains("log5"));
    ALOGV("toString:\n%s", mServiceLog->toString().string());
}

} // namespace android
