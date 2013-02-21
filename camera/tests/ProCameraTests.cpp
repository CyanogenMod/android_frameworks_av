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

#include <binder/IPCThreadState.h>
#include <utils/Thread.h>

#include "Camera.h"
#include "ProCamera.h"
#include <utils/Vector.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>

namespace android {
namespace camera2 {
namespace tests {
namespace client {

#define CAMERA_ID 0
#define TEST_DEBUGGING 0

#define TEST_LISTENER_TIMEOUT 2000000000 // 2 second listener timeout

#if TEST_DEBUGGING
#define dout std::cerr
#else
#define dout if (0) std::cerr
#endif

enum LockEvent {
    UNKNOWN,
    ACQUIRED,
    RELEASED,
    STOLEN
};

typedef Vector<LockEvent> EventList;

class ProCameraTestThread : public Thread
{
public:
    ProCameraTestThread() {
    }

    virtual bool threadLoop() {
        mProc = ProcessState::self();
        mProc->startThreadPool();

        IPCThreadState *ptr = IPCThreadState::self();

        dout << "will join thread pool" << std::endl;
        ptr->joinThreadPool();
        dout << "joined thread pool (done)" << std::endl;

        return false;
    }

    sp<ProcessState> mProc;
};

class ProCameraTestListener : public ProCameraListener {

public:
    status_t WaitForEvent() {
        Mutex::Autolock cal(mConditionMutex);

        {
            Mutex::Autolock al(mListenerMutex);

            if (mLockEventList.size() > 0) {
                return OK;
            }
        }

        return mListenerCondition.waitRelative(mConditionMutex,
                                               TEST_LISTENER_TIMEOUT);
    }

    /* Read events into out. Existing queue is flushed */
    void ReadEvents(EventList& out) {
        Mutex::Autolock al(mListenerMutex);

        for (size_t i = 0; i < mLockEventList.size(); ++i) {
            out.push(mLockEventList[i]);
        }

        mLockEventList.clear();
    }

    /**
      * Dequeue 1 event from the event queue.
      * Returns UNKNOWN if queue is empty
      */
    LockEvent ReadEvent() {
        Mutex::Autolock al(mListenerMutex);

        if (mLockEventList.size() == 0) {
            return UNKNOWN;
        }

        LockEvent ev = mLockEventList[0];
        mLockEventList.removeAt(0);

        return ev;
    }

private:
    void QueueEvent(LockEvent ev) {
        {
            Mutex::Autolock al(mListenerMutex);
            mLockEventList.push(ev);
        }


        mListenerCondition.broadcast();
    }

protected:

    //////////////////////////////////////////////////
    ///////// ProCameraListener //////////////////////
    //////////////////////////////////////////////////


    // Lock has been acquired. Write operations now available.
    virtual void onLockAcquired() {
        QueueEvent(ACQUIRED);
    }
    // Lock has been released with exclusiveUnlock
    virtual void onLockReleased() {
        QueueEvent(RELEASED);
    }

    // Lock has been stolen by another client.
    virtual void onLockStolen() {
        QueueEvent(STOLEN);
    }

    // Lock free.
    virtual void onTriggerNotify(int32_t ext1, int32_t ext2, int32_t ext3) {

        dout << "Trigger notify: " << ext1 << " " << ext2
             << " " << ext3 << std::endl;
    }

    // TODO: remove

    virtual void notify(int32_t , int32_t , int32_t ) {}
    virtual void postData(int32_t , const sp<IMemory>& ,
                          camera_frame_metadata_t *) {}
    virtual void postDataTimestamp(nsecs_t , int32_t , const sp<IMemory>& ) {}


    Vector<LockEvent> mLockEventList;
    Mutex             mListenerMutex;
    Mutex             mConditionMutex;
    Condition         mListenerCondition;
};

class ProCameraTest : public ::testing::Test {

public:
    ProCameraTest() {
    }

    virtual void SetUp() {
        mTestThread = new ProCameraTestThread();
        mTestThread->run("ProCameraTestThread");

        mCamera = ProCamera::connect(CAMERA_ID);
        ASSERT_NE((void*)NULL, mCamera.get());

        mListener = new ProCameraTestListener();
        mCamera->setListener(mListener);
    }

    virtual void TearDown() {
        ASSERT_NE((void*)NULL, mCamera.get());
        mCamera->disconnect();
    }

protected:
    sp<ProCamera> mCamera;
    sp<ProCameraTestListener> mListener;

    sp<Thread> mTestThread;

};

TEST_F(ProCameraTest, LockingImmediate) {

    if (HasFatalFailure()) {
        return;
    }


    EXPECT_FALSE(mCamera->hasExclusiveLock());
    EXPECT_EQ(OK, mCamera->exclusiveTryLock());

    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(ACQUIRED, mListener->ReadEvent());

    EXPECT_TRUE(mCamera->hasExclusiveLock());
    EXPECT_EQ(OK, mCamera->exclusiveUnlock());

    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(RELEASED, mListener->ReadEvent());

    EXPECT_FALSE(mCamera->hasExclusiveLock());
}

}
}
}
}

