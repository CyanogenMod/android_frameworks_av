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
#define TEST_FORMAT HAL_PIXEL_FORMAT_Y16 //TODO: YUY2 instead

#define TEST_FORMAT_MAIN HAL_PIXEL_FORMAT_Y8
#define TEST_FORMAT_DEPTH HAL_PIXEL_FORMAT_Y16

#define TEST_CPU_FRAME_COUNT 2
#define TEST_CPU_HEAP_COUNT 5

#if TEST_DEBUGGING
#define dout std::cerr
#else
#define dout if (0) std::cerr
#endif

#define EXPECT_OK(x) EXPECT_EQ(OK, (x))
#define ASSERT_OK(x) ASSERT_EQ(OK, (x))

class ProCameraTest;

enum ProEvent {
    UNKNOWN,
    ACQUIRED,
    RELEASED,
    STOLEN,
    BUFFER_RECEIVED,
    RESULT_RECEIVED,
};

inline int ProEvent_Mask(ProEvent e) {
    return (1 << static_cast<int>(e));
}

typedef Vector<ProEvent> EventList;

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
    static const int EVENT_MASK_ALL = 0xFFFFFFFF;

    ProCameraTestListener() {
        mEventMask = EVENT_MASK_ALL;
    }

    status_t WaitForEvent() {
        Mutex::Autolock cal(mConditionMutex);

        {
            Mutex::Autolock al(mListenerMutex);

            if (mProEventList.size() > 0) {
                return OK;
            }
        }

        return mListenerCondition.waitRelative(mConditionMutex,
                                               TEST_LISTENER_TIMEOUT);
    }

    /* Read events into out. Existing queue is flushed */
    void ReadEvents(EventList& out) {
        Mutex::Autolock al(mListenerMutex);

        for (size_t i = 0; i < mProEventList.size(); ++i) {
            out.push(mProEventList[i]);
        }

        mProEventList.clear();
    }

    /**
      * Dequeue 1 event from the event queue.
      * Returns UNKNOWN if queue is empty
      */
    ProEvent ReadEvent() {
        Mutex::Autolock al(mListenerMutex);

        if (mProEventList.size() == 0) {
            return UNKNOWN;
        }

        ProEvent ev = mProEventList[0];
        mProEventList.removeAt(0);

        return ev;
    }

    void SetEventMask(int eventMask) {
        Mutex::Autolock al(mListenerMutex);
        mEventMask = eventMask;
    }

private:
    void QueueEvent(ProEvent ev) {
        bool eventAdded = false;
        {
            Mutex::Autolock al(mListenerMutex);

            if (ProEvent_Mask(ev) & mEventMask) {
                mProEventList.push(ev);
                eventAdded = true;
            }
        }

        if (eventAdded) {
            mListenerCondition.broadcast();
        }
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

    virtual void onBufferReceived(int streamId,
                                  const CpuConsumer::LockedBuffer& buf) {

        dout << "Buffer received on streamId = " << streamId <<
                ", dataPtr = " << (void*)buf.data << std::endl;

        QueueEvent(BUFFER_RECEIVED);

    }
    virtual void onResultReceived(int32_t frameId,
                                  camera_metadata* request) {
        dout << "Result received frameId = " << frameId
             << ", requestPtr = " << (void*)request << std::endl;
        QueueEvent(RESULT_RECEIVED);
        free_camera_metadata(request);
    }

    // TODO: remove

    virtual void notify(int32_t , int32_t , int32_t ) {}
    virtual void postData(int32_t , const sp<IMemory>& ,
                          camera_frame_metadata_t *) {}
    virtual void postDataTimestamp(nsecs_t , int32_t , const sp<IMemory>& ) {}


    Vector<ProEvent> mProEventList;
    Mutex             mListenerMutex;
    Mutex             mConditionMutex;
    Condition         mListenerCondition;
    int               mEventMask;
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

    sp<SurfaceComposerClient> mDepthComposerClient;
    sp<SurfaceControl> mDepthSurfaceControl;

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

        mSurfaceControl->setPosition(640, 0);

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

    void createDepthOnScreenSurface(sp<Surface>& surface) {
        mDepthComposerClient = new SurfaceComposerClient;
        ASSERT_EQ(NO_ERROR, mDepthComposerClient->initCheck());

        mDepthSurfaceControl = mDepthComposerClient->createSurface(
                String8("ProCameraTest StreamingImage Surface"),
                getSurfaceWidth(), getSurfaceHeight(),
                PIXEL_FORMAT_RGB_888, 0);

        mDepthSurfaceControl->setPosition(640, 0);

        ASSERT_TRUE(mDepthSurfaceControl != NULL);
        ASSERT_TRUE(mDepthSurfaceControl->isValid());

        SurfaceComposerClient::openGlobalTransaction();
        ASSERT_EQ(NO_ERROR, mDepthSurfaceControl->setLayer(0x7FFFFFFF));
        ASSERT_EQ(NO_ERROR, mDepthSurfaceControl->show());
        SurfaceComposerClient::closeGlobalTransaction();

        sp<ANativeWindow> window = mDepthSurfaceControl->getSurface();
        surface = mDepthSurfaceControl->getSurface();

        ASSERT_NE((void*)NULL, surface.get());
    }

    template <typename T>
    static bool FindItem(T needle, T* array, size_t count) {
        for (int i = 0; i < count; ++i) {
            if (array[i] == needle) {
                return true;
            }
        }
        return false;
    }

};

sp<Thread> ProCameraTest::mTestThread;

TEST_F(ProCameraTest, AvailableFormats) {
    if (HasFatalFailure()) {
        return;
    }

    camera_metadata_t* info = mCamera->getCameraInfo(CAMERA_ID);
    ASSERT_NE((void*)NULL, info);

    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_SCALER_AVAILABLE_FORMATS);
    EXPECT_EQ(OK, find_camera_metadata_entry(info, tag, &entry));

    EXPECT_TRUE(FindItem<int32_t>(HAL_PIXEL_FORMAT_YV12,
                                                  entry.data.i32, entry.count));
    EXPECT_TRUE(FindItem<int32_t>(HAL_PIXEL_FORMAT_YCrCb_420_SP,
                                                  entry.data.i32, entry.count));

    free_camera_metadata(info);
}

// test around exclusiveTryLock (immediate locking)
TEST_F(ProCameraTest, LockingImmediate) {

    if (HasFatalFailure()) {
        return;
    }

    mListener->SetEventMask(ProEvent_Mask(ACQUIRED) |
                            ProEvent_Mask(STOLEN)   |
                            ProEvent_Mask(RELEASED));

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


    mListener->SetEventMask(ProEvent_Mask(ACQUIRED) |
                            ProEvent_Mask(STOLEN)   |
                            ProEvent_Mask(RELEASED));

    // TODO: Add another procamera that has a lock here.
    // then we can be test that the lock wont immediately be acquired

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

// Stream directly to the screen.
TEST_F(ProCameraTest, DISABLED_StreamingImageSingle) {
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

    sp<Surface> depthSurface;
    if (mDisplaySecs > 0) {
        createDepthOnScreenSurface(/*out*/depthSurface);
    }

    int depthStreamId = -1;
    EXPECT_OK(mCamera->createStream(/*width*/320, /*height*/240,
                              TEST_FORMAT_DEPTH, depthSurface, &depthStreamId));
    EXPECT_NE(-1, depthStreamId);

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
    uint8_t allStreams[] = { depthStreamId };
    size_t streamCount = sizeof(allStreams) / sizeof(allStreams[0]);

    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_REQUEST_OUTPUT_STREAMS);
    int find = find_camera_metadata_entry(request, tag, &entry);
    if (find == -ENOENT) {
        if (add_camera_metadata_entry(request, tag, &allStreams,
            /*data_count*/streamCount) != OK) {
            camera_metadata_t *tmp = allocate_camera_metadata(1000, 10000);
            ASSERT_OK(append_camera_metadata(tmp, request));
            free_camera_metadata(request);
            request = tmp;

            ASSERT_OK(add_camera_metadata_entry(request, tag, &allStreams,
                /*data_count*/streamCount));
        }
    } else {
        ASSERT_OK(update_camera_metadata_entry(request, entry.index,
                &allStreams, /*data_count*/streamCount, &entry));
    }

    EXPECT_OK(mCamera->submitRequest(request, /*streaming*/true));

    dout << "will sleep now for " << mDisplaySecs << std::endl;
    sleep(mDisplaySecs);

    free_camera_metadata(request);

    for (int i = 0; i < streamCount; ++i) {
        EXPECT_OK(mCamera->deleteStream(allStreams[i]));
    }
    EXPECT_OK(mCamera->exclusiveUnlock());
}

// Stream directly to the screen.
TEST_F(ProCameraTest, DISABLED_StreamingImageDual) {
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
    sp<Surface> depthSurface;
    if (mDisplaySecs > 0) {
        createOnScreenSurface(/*out*/surface);
        createDepthOnScreenSurface(/*out*/depthSurface);
    }

    int streamId = -1;
    EXPECT_OK(mCamera->createStream(/*width*/1280, /*height*/960,
              TEST_FORMAT_MAIN, surface, &streamId));
    EXPECT_NE(-1, streamId);

    int depthStreamId = -1;
    EXPECT_OK(mCamera->createStream(/*width*/320, /*height*/240,
              TEST_FORMAT_DEPTH, depthSurface, &depthStreamId));
    EXPECT_NE(-1, depthStreamId);

    EXPECT_OK(mCamera->exclusiveTryLock());
    /*
    */
    /* iterate in a loop submitting requests every frame.
     *  what kind of requests doesnt really matter, just whatever.
     */

    // it would probably be better to use CameraMetadata from camera service.
    camera_metadata_t *request = NULL;
    EXPECT_OK(mCamera->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
              /*out*/&request));
    EXPECT_NE((void*)NULL, request);

    /*FIXME: dont need this later, at which point the above should become an
             ASSERT_NE*/
    if(request == NULL) request = allocate_camera_metadata(10, 100);

    // set the output streams to just this stream ID

    // wow what a verbose API.
    uint8_t allStreams[] = { streamId, depthStreamId };
    // IMPORTANT. bad things will happen if its not a uint8.
    size_t streamCount = sizeof(allStreams) / sizeof(allStreams[0]);
    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_REQUEST_OUTPUT_STREAMS);
    int find = find_camera_metadata_entry(request, tag, &entry);
    if (find == -ENOENT) {
        if (add_camera_metadata_entry(request, tag, &allStreams,
                                      /*data_count*/streamCount) != OK) {
            camera_metadata_t *tmp = allocate_camera_metadata(1000, 10000);
            ASSERT_OK(append_camera_metadata(tmp, request));
            free_camera_metadata(request);
            request = tmp;

            ASSERT_OK(add_camera_metadata_entry(request, tag, &allStreams,
                                                /*data_count*/streamCount));
        }
    } else {
        ASSERT_OK(update_camera_metadata_entry(request, entry.index,
                  &allStreams, /*data_count*/streamCount, &entry));
    }

    EXPECT_OK(mCamera->submitRequest(request, /*streaming*/true));

    dout << "will sleep now for " << mDisplaySecs << std::endl;
    sleep(mDisplaySecs);

    free_camera_metadata(request);

    for (int i = 0; i < streamCount; ++i) {
        EXPECT_OK(mCamera->deleteStream(allStreams[i]));
    }
    EXPECT_OK(mCamera->exclusiveUnlock());
}

TEST_F(ProCameraTest, CpuConsumerSingle) {
    if (HasFatalFailure()) {
        return;
    }

    mListener->SetEventMask(ProEvent_Mask(BUFFER_RECEIVED));

    int streamId = -1;
    EXPECT_OK(mCamera->createStreamCpu(/*width*/320, /*height*/240,
        TEST_FORMAT_DEPTH, TEST_CPU_HEAP_COUNT, &streamId));
    EXPECT_NE(-1, streamId);

    EXPECT_OK(mCamera->exclusiveTryLock());
    EXPECT_EQ(OK, mListener->WaitForEvent());
    EXPECT_EQ(ACQUIRED, mListener->ReadEvent());
    /* iterate in a loop submitting requests every frame.
     *  what kind of requests doesnt really matter, just whatever.
     */

    // it would probably be better to use CameraMetadata from camera service.
    camera_metadata_t *request = NULL;
    EXPECT_OK(mCamera->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
        /*out*/&request));
    EXPECT_NE((void*)NULL, request);

    /*FIXME: dont need this later, at which point the above should become an
      ASSERT_NE*/
    if(request == NULL) request = allocate_camera_metadata(10, 100);

    // set the output streams to just this stream ID

    uint8_t allStreams[] = { streamId };
    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_REQUEST_OUTPUT_STREAMS);
    int find = find_camera_metadata_entry(request, tag, &entry);
    if (find == -ENOENT) {
        if (add_camera_metadata_entry(request, tag, &allStreams,
                /*data_count*/1) != OK) {
            camera_metadata_t *tmp = allocate_camera_metadata(1000, 10000);
            ASSERT_OK(append_camera_metadata(tmp, request));
            free_camera_metadata(request);
            request = tmp;

            ASSERT_OK(add_camera_metadata_entry(request, tag, &allStreams,
                /*data_count*/1));
        }
    } else {
        ASSERT_OK(update_camera_metadata_entry(request, entry.index,
            &allStreams, /*data_count*/1, &entry));
    }

    EXPECT_OK(mCamera->submitRequest(request, /*streaming*/true));

    // Consume a couple of frames
    for (int i = 0; i < TEST_CPU_FRAME_COUNT; ++i) {
        EXPECT_EQ(OK, mListener->WaitForEvent());
        EXPECT_EQ(BUFFER_RECEIVED, mListener->ReadEvent());
    }

    // Done: clean up
    free_camera_metadata(request);
    EXPECT_OK(mCamera->deleteStream(streamId));
    EXPECT_OK(mCamera->exclusiveUnlock());
}

TEST_F(ProCameraTest, CpuConsumerDual) {
    if (HasFatalFailure()) {
        return;
    }

    mListener->SetEventMask(ProEvent_Mask(BUFFER_RECEIVED));

    int streamId = -1;
    EXPECT_OK(mCamera->createStreamCpu(/*width*/1280, /*height*/960,
                            TEST_FORMAT_MAIN, TEST_CPU_HEAP_COUNT, &streamId));
    EXPECT_NE(-1, streamId);

    int depthStreamId = -1;
    EXPECT_OK(mCamera->createStreamCpu(/*width*/320, /*height*/240,
                     TEST_FORMAT_DEPTH, TEST_CPU_HEAP_COUNT, &depthStreamId));
    EXPECT_NE(-1, depthStreamId);

    EXPECT_OK(mCamera->exclusiveTryLock());
    /*
    */
    /* iterate in a loop submitting requests every frame.
     *  what kind of requests doesnt really matter, just whatever.
     */

    // it would probably be better to use CameraMetadata from camera service.
    camera_metadata_t *request = NULL;
    EXPECT_OK(mCamera->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
                                            /*out*/&request));
    EXPECT_NE((void*)NULL, request);

    if(request == NULL) request = allocate_camera_metadata(10, 100);

    // set the output streams to just this stream ID

    // wow what a verbose API.
    uint8_t allStreams[] = { streamId, depthStreamId };
    size_t streamCount = 2;
    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_REQUEST_OUTPUT_STREAMS);
    int find = find_camera_metadata_entry(request, tag, &entry);
    if (find == -ENOENT) {
        if (add_camera_metadata_entry(request, tag, &allStreams,
                                      /*data_count*/streamCount) != OK) {
            camera_metadata_t *tmp = allocate_camera_metadata(1000, 10000);
            ASSERT_OK(append_camera_metadata(tmp, request));
            free_camera_metadata(request);
            request = tmp;

            ASSERT_OK(add_camera_metadata_entry(request, tag, &allStreams,
                                                   /*data_count*/streamCount));
        }
    } else {
        ASSERT_OK(update_camera_metadata_entry(request, entry.index,
                              &allStreams, /*data_count*/streamCount, &entry));
    }

    EXPECT_OK(mCamera->submitRequest(request, /*streaming*/true));

    // Consume a couple of frames
    for (int i = 0; i < TEST_CPU_FRAME_COUNT; ++i) {
        // stream id 1
        EXPECT_EQ(OK, mListener->WaitForEvent());
        EXPECT_EQ(BUFFER_RECEIVED, mListener->ReadEvent());

        // stream id 2
        EXPECT_EQ(OK, mListener->WaitForEvent());
        EXPECT_EQ(BUFFER_RECEIVED, mListener->ReadEvent());

        //TODO: events should be a struct with some data like the stream id
    }

    // Done: clean up
    free_camera_metadata(request);
    EXPECT_OK(mCamera->deleteStream(streamId));
    EXPECT_OK(mCamera->exclusiveUnlock());
}

TEST_F(ProCameraTest, ResultReceiver) {
    if (HasFatalFailure()) {
        return;
    }

    mListener->SetEventMask(ProEvent_Mask(RESULT_RECEIVED));
    //FIXME: if this is run right after the previous test we get BUFFER_RECEIVED
    // need to filter out events at read time

    int streamId = -1;
    EXPECT_OK(mCamera->createStreamCpu(/*width*/1280, /*height*/960,
                             TEST_FORMAT_MAIN, TEST_CPU_HEAP_COUNT, &streamId));
    EXPECT_NE(-1, streamId);

    EXPECT_OK(mCamera->exclusiveTryLock());
    /*
    */
    /* iterate in a loop submitting requests every frame.
     *  what kind of requests doesnt really matter, just whatever.
     */

    camera_metadata_t *request = NULL;
    EXPECT_OK(mCamera->createDefaultRequest(CAMERA2_TEMPLATE_PREVIEW,
                                            /*out*/&request));
    EXPECT_NE((void*)NULL, request);

    /*FIXME*/
    if(request == NULL) request = allocate_camera_metadata(10, 100);

    // set the output streams to just this stream ID

    uint8_t allStreams[] = { streamId };
    size_t streamCount = 1;
    camera_metadata_entry_t entry;
    uint32_t tag = static_cast<uint32_t>(ANDROID_REQUEST_OUTPUT_STREAMS);
    int find = find_camera_metadata_entry(request, tag, &entry);
    if (find == -ENOENT) {
        if (add_camera_metadata_entry(request, tag, &allStreams,
                                      /*data_count*/streamCount) != OK) {
            camera_metadata_t *tmp = allocate_camera_metadata(1000, 10000);
            ASSERT_OK(append_camera_metadata(tmp, request));
            free_camera_metadata(request);
            request = tmp;

            ASSERT_OK(add_camera_metadata_entry(request, tag, &allStreams,
                                                /*data_count*/streamCount));
        }
    } else {
        ASSERT_OK(update_camera_metadata_entry(request, entry.index,
                               &allStreams, /*data_count*/streamCount, &entry));
    }

    EXPECT_OK(mCamera->submitRequest(request, /*streaming*/true));

    // Consume a couple of results
    for (int i = 0; i < TEST_CPU_FRAME_COUNT; ++i) {
        EXPECT_EQ(OK, mListener->WaitForEvent());
        EXPECT_EQ(RESULT_RECEIVED, mListener->ReadEvent());
    }

    // Done: clean up
    free_camera_metadata(request);
    EXPECT_OK(mCamera->deleteStream(streamId));
    EXPECT_OK(mCamera->exclusiveUnlock());
}

}
}
}
}

