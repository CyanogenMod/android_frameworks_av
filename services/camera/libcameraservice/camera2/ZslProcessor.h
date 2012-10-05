/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2_ZSLPROCESSOR_H
#define ANDROID_SERVERS_CAMERA_CAMERA2_ZSLPROCESSOR_H

#include <utils/Thread.h>
#include <utils/String16.h>
#include <utils/Vector.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>
#include <gui/BufferItemConsumer.h>
#include "Parameters.h"
#include "FrameProcessor.h"
#include "CameraMetadata.h"
#include "Camera2Heap.h"
#include "../Camera2Device.h"

namespace android {

class Camera2Client;

namespace camera2 {

class CaptureSequencer;

/***
 * ZSL queue processing
 */
class ZslProcessor:
            virtual public Thread,
            virtual public BufferItemConsumer::FrameAvailableListener,
            virtual public FrameProcessor::FilteredListener,
            virtual public Camera2Device::BufferReleasedListener {
  public:
    ZslProcessor(wp<Camera2Client> client, wp<CaptureSequencer> sequencer);
    ~ZslProcessor();

    // From mZslConsumer
    virtual void onFrameAvailable();
    // From FrameProcessor
    virtual void onFrameAvailable(int32_t frameId, const CameraMetadata &frame);

    virtual void onBufferReleased(buffer_handle_t *handle);

    status_t updateStream(const Parameters &params);
    status_t deleteStream();
    int getStreamId() const;
    int getReprocessStreamId() const;

    status_t pushToReprocess(int32_t requestId);
    status_t clearZslQueue();

    void dump(int fd, const Vector<String16>& args) const;
  private:
    static const nsecs_t kWaitDuration = 10000000; // 10 ms

    enum {
        RUNNING,
        LOCKED
    } mState;

    wp<Camera2Client> mClient;
    wp<CaptureSequencer> mSequencer;

    mutable Mutex mInputMutex;
    bool mZslBufferAvailable;
    Condition mZslBufferAvailableSignal;

    enum {
        NO_STREAM = -1
    };

    int mZslStreamId;
    int mZslReprocessStreamId;
    sp<BufferItemConsumer> mZslConsumer;
    sp<ANativeWindow>      mZslWindow;

    struct ZslPair {
        BufferItemConsumer::BufferItem buffer;
        CameraMetadata frame;
    };

    static const size_t kZslBufferDepth = 4;
    static const size_t kFrameListDepth = kZslBufferDepth * 2;
    Vector<CameraMetadata> mFrameList;
    size_t mFrameListHead;

    ZslPair mNextPair;

    Vector<ZslPair> mZslQueue;
    size_t mZslQueueHead;
    size_t mZslQueueTail;

    CameraMetadata mLatestCapturedRequest;

    virtual bool threadLoop();

    status_t processNewZslBuffer(sp<Camera2Client> &client);

    // Match up entries from frame list to buffers in ZSL queue
    void findMatchesLocked();

    status_t clearZslQueueLocked();

    void dumpZslQueue(int id) const;
};


}; //namespace camera2
}; //namespace android

#endif
