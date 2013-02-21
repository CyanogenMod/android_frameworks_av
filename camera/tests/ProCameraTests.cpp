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

#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>

#include <system/camera_metadata.h>
#include <hardware/camera2.h> // for CAMERA2_TEMPLATE_PREVIEW only

namespace android {
namespace camera2 {
namespace tests {
namespace client {

#define CAMERA_ID 0
#define TEST_DEBUGGING 0

#define TEST_LISTENER_TIMEOUT 1000000000 // 1 second listener timeout
#define TEST_FORMAT HAL_PIXEL_FORMAT_RGBA_8888 //TODO: YUY2 instead

#if TEST_DEBUGGING
#define dout std::cerr
#else
#define dout if (0) std::cerr
#endif

#define EXPECT_OK(x) EXPECT_EQ(OK, (x))
#define ASSERT_OK(x) ASSERT_EQ(OK, (x))

class ProCameraTest;

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

        ptr->joinThreadPool();

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

    static void SetUpTestCase() {
        // Binder Thread Pool Initialization
        mTestThread = new ProCameraTestThread();
        mTestThread->run("ProCameraTestThread");
    }

    virtual void SetUp() {
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

    static sp<Thread> mTestThread;

    int mDisplaySecs;
    sp<SurfaceComposerClient> mComposerClient;
    sp<SurfaceControl> mSurfaceControl;

    int getSurfaceWidth() {
        return 512;
    }
    int getSurfaceHeight() {
        return 512;
    }

    void createOnScreenSurface(sp<Surface>& surface) {
        mComposerClient = new SurfaceComposerClient;
        ASSERT_EQ(NO_ERROR, mComposerClient->initCheck());

        mSurfaceControl = mComposerClient->createSurface(
                String8("ProCameraTest StreamingImage Surface"),
                getSurfaceWidth(), getSurfaceHeight(),
                PIXEL_FORMAT_RGB_888, 0);

        ASSERT_TRUE(mSurfaceControl != NULL);
        ASSERT_TRUE(mSurfaceControl->isValid());

        SurfaceComposerClient::openGlobalTransaction();
        ASSERT_EQ(NO_ERROR, mSurfaceControl->setLayer(0x7FFFFFFF));
        ASSERT_EQ(NO_ERROR, mSurfaceControl->show());
        SurfaceComposerClient::closeGlobalTransaction();

        sp<ANativeWindow> window = mSurfaceControl->getSurface();
        surface = mSurfaceControl->getSurface();

        ASSERT_NE((void*)NULL, surface.get());
    }

};

sp<Thread> ProCameraTest::mTestThread;

// test around exclusiveTryLock (immediate locking)
TEST_F(ProCameraTest, LockingImmediate) {

    if (HasFatalFailure()) {
        return;
    }

    EXPECT_FALSE(mCamera->hasExclusiveLock());
    EXPECT_EQ(OK, mCamera->exclusiveTryLock());
    // at this point we definitely have the lock

    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(ACQUIRED, mListener->ReadEvent());

    EXPECT_TRUE(mCamera->hasExclusiveLock());
    EXPECT_EQ(OK, mCamera->exclusiveUnlock());

    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(RELEASED, mListener->ReadEvent());

    EXPECT_FALSE(mCamera->hasExclusiveLock());
}

// test around exclusiveLock (locking at some future point in time)
TEST_F(ProCameraTest, LockingAsynchronous) {

    if (HasFatalFailure()) {
        return;
    }

    // TODO: Add another procamera that has a lock here.
    // then we can be test that the lock wont immediately be acquired

    EXPECT_FALSE(mCamera->hasExclusiveLock());
    EXPECT_EQ(OK, mCamera->exclusiveLock());
    // at this point we may or may not have the lock
    // we cant be sure until we get an ACQUIRED event

    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(ACQUIRED, mListener->ReadEvent());

    EXPECT_TRUE(mCamera->hasExclusiveLock());
    EXPECT_EQ(OK, mCamera->exclusiveUnlock());

    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(RELEASED, mListener->ReadEvent());

    EXPECT_FALSE(mCamera->hasExclusiveLock());
}

// Stream directly to the screen.
TEST_F(ProCameraTest, StreamingImage) {
    if (HasFatalFailure()) {
        return;
    }
    char* displaySecsEnv = getenv("TEST_DISPLAY_SECS");
    if (displaySecsEnv != NULL) {
        mDisplaySecs = atoi(displaySecsEnv);
        if (mDisplaySecs < 0) {
            mDisplaySecs = 0;
        }
    } else {
        mDisplaySecs = 0;
    }

    sp<Surface> surface;
    if (mDisplaySecs > 0) {
        createOnScreenSurface(/*out*/surface);
    }
    int streamId = -1;
    EXPECT_OK(mCamera->createStream(/*width*/640, /*height*/480, TEST_FORMAT,
              surface, &streamId));
    EXPECT_NE(-1, streamId);

    EXPECT_OK(mCamera->exclusiveTryLock());
    /* iterate in a loop submitting requests every frame.
     *  what kind of requests doesnt really matter, just whatever.
     */

    // it would probably be better to use CameraMetadata from camera service.
    camera_metadata_t *request = NULL;
    EXPECT_OK(mCamera->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
              /*out*/&request));
    EXPECT_NE((void*)NULL, request);

    /* FIXME: dont need this later, at which point the above should become an
       ASSERT_NE*/
    if(request == NULL) request = allocate_camera_metadata(10, 100);

    // set the output streams to just this stream ID

    // wow what a verbose API.
    // i would give a loaf of bread for
    //   metadata->updateOrInsert(keys.request.output.streams, streamId);
    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_REQUEST_OUTPUT_STREAMS);
    int find = find_camera_metadata_entry(request, tag, &entry);
    if (find == -ENOENT) {
        if (add_camera_metadata_entry(request, tag, &streamId, /*data_count*/1)
                != OK) {
            camera_metadata_t *tmp = allocate_camera_metadata(1000, 10000);
            ASSERT_OK(append_camera_metadata(tmp, request));
            free_camera_metadata(request);
            request = tmp;

            ASSERT_OK(add_camera_metadata_entry(request, tag, &streamId,
                /*data_count*/1));
        }
    } else {
        ASSERT_OK(update_camera_metadata_entry(request, entry.index, &streamId,
                  /*data_count*/1, &entry));
    }

    EXPECT_OK(mCamera->submitRequest(request, /*streaming*/true));

    dout << "will sleep now for " << mDisplaySecs << std::endl;
    sleep(mDisplaySecs);

    free_camera_metadata(request);
    EXPECT_OK(mCamera->cancelStream(streamId));
    EXPECT_OK(mCamera->exclusiveUnlock());
}

}
}
}
}

