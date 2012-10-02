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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2_STREAMINGPROCESSOR_H
#define ANDROID_SERVERS_CAMERA_CAMERA2_STREAMINGPROCESSOR_H

#include <utils/Mutex.h>
#include <utils/String16.h>
#include <gui/BufferItemConsumer.h>

#include "Parameters.h"
#include "CameraMetadata.h"

namespace android {

class Camera2Client;
class IMemory;

namespace camera2 {

class Camera2Heap;

/**
 * Management and processing for preview and recording streams
 */
class StreamingProcessor: public BufferItemConsumer::FrameAvailableListener {
  public:
    StreamingProcessor(wp<Camera2Client> client);
    ~StreamingProcessor();

    status_t setPreviewWindow(sp<ANativeWindow> window);

    bool haveValidPreviewWindow() const;

    status_t updatePreviewRequest(const Parameters &params);
    status_t updatePreviewStream(const Parameters &params);
    status_t deletePreviewStream();
    int getPreviewStreamId() const;

    status_t setRecordingBufferCount(size_t count);
    status_t updateRecordingRequest(const Parameters &params);
    status_t updateRecordingStream(const Parameters &params);
    status_t deleteRecordingStream();
    int getRecordingStreamId() const;

    enum StreamType {
        NONE,
        PREVIEW,
        RECORD
    };
    status_t startStream(StreamType type,
            const Vector<uint8_t> &outputStreams);

    status_t stopStream();

    // Returns the request ID for the currently streaming request
    // Returns 0 if there is no active request.
    status_t getActiveRequestId() const;
    status_t incrementStreamingIds();

    // Callback for new recording frames from HAL
    virtual void onFrameAvailable();
    // Callback from stagefright which returns used recording frames
    void releaseRecordingFrame(const sp<IMemory>& mem);

    status_t dump(int fd, const Vector<String16>& args);

  private:
    mutable Mutex mMutex;

    enum {
        NO_STREAM = -1
    };

    wp<Camera2Client> mClient;

    StreamType mActiveRequest;

    // Preview-related members
    int32_t mPreviewRequestId;
    int mPreviewStreamId;
    CameraMetadata mPreviewRequest;
    sp<ANativeWindow> mPreviewWindow;

    // Recording-related members
    int32_t mRecordingRequestId;
    int mRecordingStreamId;
    int mRecordingFrameCount;
    sp<BufferItemConsumer> mRecordingConsumer;
    sp<ANativeWindow>  mRecordingWindow;
    CameraMetadata mRecordingRequest;
    sp<camera2::Camera2Heap> mRecordingHeap;

    static const size_t kDefaultRecordingHeapCount = 8;
    size_t mRecordingHeapCount;
    Vector<BufferItemConsumer::BufferItem> mRecordingBuffers;
    size_t mRecordingHeapHead, mRecordingHeapFree;

};


}; // namespace camera2
}; // namespace android

#endif
