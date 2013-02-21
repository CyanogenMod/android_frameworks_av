/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <gtest/gtest.h>
#include <iostream>

#include "Camera.h"
#include "ProCamera.h"

namespace android {
namespace camera2 {
namespace tests {
namespace client {

#define CAMERA_ID 0
#define TEST_DEBUGGING 0

#if TEST_DEBUGGING
#define dout std::cerr
#else
#define dout if (0) std::cerr
#endif

class ProCameraTest : public ::testing::Test {

    virtual void SetUp() {
        mCamera = ProCamera::connect(CAMERA_ID);
        ASSERT_NE((void*)NULL, mCamera.get());
    }

    virtual void TearDown() {
        ASSERT_NE((void*)NULL, mCamera.get());
        mCamera->disconnect();
    }

protected:
    sp<ProCamera> mCamera;
};

TEST_F(ProCameraTest, Locking) {

    if (HasFatalFailure()) {
        return;
    }

    status_t res = mCamera->exclusiveTryLock();

    EXPECT_EQ(OK, res);
}

}
}
}
}

