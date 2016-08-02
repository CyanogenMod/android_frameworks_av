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

#ifndef ANDROID_SERVERS_CAMERA3_OUTPUT_STREAM_H
#define ANDROID_SERVERS_CAMERA3_OUTPUT_STREAM_H

#include <utils/RefBase.h>
#include <gui/IProducerListener.h>
#include <gui/Surface.h>

#include "Camera3Stream.h"
#include "Camera3IOStreamBase.h"
#include "Camera3OutputStreamInterface.h"
#include "Camera3BufferManager.h"

namespace android {

namespace camera3 {

class Camera3BufferManager;

/**
 * Stream info structure that holds the necessary stream info for buffer manager to use for
 * buffer allocation and management.
 */
struct StreamInfo {
    int streamId;
    int streamSetId;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    android_dataspace dataSpace;
    uint32_t combinedUsage;
    size_t totalBufferCount;
    bool isConfigured;
    StreamInfo(int id = CAMERA3_STREAM_ID_INVALID,
            int setId = CAMERA3_STREAM_SET_ID_INVALID,
            uint32_t w = 0,
            uint32_t h = 0,
            uint32_t fmt = 0,
            android_dataspace ds = HAL_DATASPACE_UNKNOWN,
            uint32_t usage = 0,
            size_t bufferCount = 0,
            bool configured = false) :
                streamId(id),
                streamSetId(setId),
                width(w),
                height(h),
                format(fmt),
                dataSpace(ds),
                combinedUsage(usage),
                totalBufferCount(bufferCount),
                isConfigured(configured){}
};

/**
 * A class for managing a single stream of output data from the camera device.
 */
class Camera3OutputStream :
        public Camera3IOStreamBase,
        public Camera3OutputStreamInterface {
  public:
    /**
     * Set up a stream for formats that have 2 dimensions, such as RAW and YUV.
     * A valid stream set id needs to be set to support buffer sharing between multiple
     * streams.
     */
    Camera3OutputStream(int id, sp<Surface> consumer,
            uint32_t width, uint32_t height, int format,
            android_dataspace dataSpace, camera3_stream_rotation_t rotation,
            nsecs_t timestampOffset, int setId = CAMERA3_STREAM_SET_ID_INVALID);

    /**
     * Set up a stream for formats that have a variable buffer size for the same
     * dimensions, such as compressed JPEG.
     * A valid stream set id needs to be set to support buffer sharing between multiple
     * streams.
     */
    Camera3OutputStream(int id, sp<Surface> consumer,
            uint32_t width, uint32_t height, size_t maxSize, int format,
            android_dataspace dataSpace, camera3_stream_rotation_t rotation,
            nsecs_t timestampOffset, int setId = CAMERA3_STREAM_SET_ID_INVALID);

    virtual ~Camera3OutputStream();

    /**
     * Camera3Stream interface
     */

    virtual void     dump(int fd, const Vector<String16> &args) const;

    /**
     * Set the transform on the output stream; one of the
     * HAL_TRANSFORM_* / NATIVE_WINDOW_TRANSFORM_* constants.
     */
    status_t         setTransform(int transform);

    /**
     * Return if this output stream is for video encoding.
     */
    bool isVideoStream() const;
    /**
     * Return if this output stream is consumed by hardware composer.
     */
    bool isConsumedByHWComposer() const;

    /**
     * Return if this output stream is consumed by hardware texture.
     */
    bool isConsumedByHWTexture() const;

    class BufferReleasedListener : public BnProducerListener {
        public:
          BufferReleasedListener(wp<Camera3OutputStream> parent) : mParent(parent) {}

          /**
          * Implementation of IProducerListener, used to notify this stream that the consumer
          * has returned a buffer and it is ready to return to Camera3BufferManager for reuse.
          */
          virtual void onBufferReleased();

        private:
          wp<Camera3OutputStream> mParent;
    };

    virtual status_t detachBuffer(sp<GraphicBuffer>* buffer, int* fenceFd);

    /**
     * Set the graphic buffer manager to get/return the stream buffers.
     *
     * It is only legal to call this method when stream is in STATE_CONSTRUCTED state.
     */
    status_t setBufferManager(sp<Camera3BufferManager> bufferManager);

  protected:
    Camera3OutputStream(int id, camera3_stream_type_t type,
            uint32_t width, uint32_t height, int format,
            android_dataspace dataSpace, camera3_stream_rotation_t rotation,
            int setId = CAMERA3_STREAM_SET_ID_INVALID);

    /**
     * Note that we release the lock briefly in this function
     */
    virtual status_t returnBufferCheckedLocked(
            const camera3_stream_buffer &buffer,
            nsecs_t timestamp,
            bool output,
            /*out*/
            sp<Fence> *releaseFenceOut);

    virtual status_t disconnectLocked();

    sp<Surface> mConsumer;
  private:

    static const nsecs_t       kDequeueBufferTimeout   = 1000000000; // 1 sec

    int               mTransform;

    virtual status_t  setTransformLocked(int transform);

    bool mTraceFirstBuffer;

    // Name of Surface consumer
    String8           mConsumerName;

    // Whether consumer assumes MONOTONIC timestamp
    bool mUseMonoTimestamp;

    /**
     * GraphicBuffer manager this stream is registered to. Used to replace the buffer
     * allocation/deallocation role of BufferQueue.
     */
    sp<Camera3BufferManager> mBufferManager;

    /**
     * Buffer released listener, used to notify the buffer manager that a buffer is released
     * from consumer side.
     */
    sp<BufferReleasedListener> mBufferReleasedListener;

    /**
     * Flag indicating if the buffer manager is used to allocate the stream buffers
     */
    bool mUseBufferManager;

    /**
     * Timestamp offset for video and hardware composer consumed streams
     */
    nsecs_t mTimestampOffset;

    /**
     * Internal Camera3Stream interface
     */
    virtual status_t getBufferLocked(camera3_stream_buffer *buffer);
    virtual status_t returnBufferLocked(
            const camera3_stream_buffer &buffer,
            nsecs_t timestamp);

    virtual status_t configureQueueLocked();

    virtual status_t getEndpointUsage(uint32_t *usage) const;

}; // class Camera3OutputStream

} // namespace camera3

} // namespace android

#endif
