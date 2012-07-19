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

#ifndef ANDROID_SERVERS_CAMERA_MEDIACONSUMER_H
#define ANDROID_SERVERS_CAMERA_MEDIACONSUMER_H

#include <gui/BufferQueue.h>

#include <ui/GraphicBuffer.h>

#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/threads.h>

#define ANDROID_GRAPHICS_MEDIACONSUMER_JNI_ID "mMediaConsumer"

namespace android {

/**
 * MediaConsumer is a BufferQueue consumer endpoint that makes it
 * straightforward to bridge Camera 2 to the existing media recording framework.
 * This queue is synchronous by default.
 *
 * TODO: This is a temporary replacement for the full camera->media recording
 * path using SurfaceMediaEncoder or equivalent.
 */

class MediaConsumer: public virtual RefBase,
                     protected BufferQueue::ConsumerListener
{
  public:
    struct FrameAvailableListener : public virtual RefBase {
        // onFrameAvailable() is called each time an additional frame becomes
        // available for consumption. A new frame queued will always trigger the
        // callback, whether the queue is empty or not.
        //
        // This is called without any lock held and can be called concurrently
        // by multiple threads.
        virtual void onFrameAvailable() = 0;
    };

    // Create a new media consumer. The maxBuffers parameter specifies
    // how many buffers can be locked for user access at the same time.
    MediaConsumer(uint32_t maxBuffers);

    virtual ~MediaConsumer();

    // set the name of the MediaConsumer that will be used to identify it in
    // log messages.
    void setName(const String8& name);

    // Gets the next graphics buffer from the producer. Returns BAD_VALUE if no
    // new buffer is available, and INVALID_OPERATION if the maximum number of
    // buffers is already in use.
    //
    // Only a fixed number of buffers can be available at a time, determined by
    // the construction-time maxBuffers parameter. If INVALID_OPERATION is
    // returned by getNextBuffer, then old buffers must be returned to the
    // queue by calling freeBuffer before more buffers can be acquired.
    status_t getNextBuffer(buffer_handle_t *buffer, nsecs_t *timestamp);

    // Returns a buffer to the queue, allowing it to be reused. Since
    // only a fixed number of buffers may be locked at a time, old buffers must
    // be released by calling unlockBuffer to ensure new buffers can be acquired by
    // lockNextBuffer.
    status_t freeBuffer(buffer_handle_t buffer);

    // setFrameAvailableListener sets the listener object that will be notified
    // when a new frame becomes available.
    void setFrameAvailableListener(const sp<FrameAvailableListener>& listener);

    sp<ISurfaceTexture> getProducerInterface() const { return mBufferQueue; }
  protected:

    // Implementation of the BufferQueue::ConsumerListener interface.  These
    // calls are used to notify the MediaConsumer of asynchronous events in the
    // BufferQueue.
    virtual void onFrameAvailable();
    virtual void onBuffersReleased();

  private:
    // Free local buffer state
    status_t freeBufferLocked(int buf);

    // Maximum number of buffers that can be locked at a time
    uint32_t mMaxLockedBuffers;

    // mName is a string used to identify the SurfaceTexture in log messages.
    // It can be set by the setName method.
    String8 mName;

    // mFrameAvailableListener is the listener object that will be called when a
    // new frame becomes available. If it is not NULL it will be called from
    // queueBuffer.
    sp<FrameAvailableListener> mFrameAvailableListener;

    // Underlying buffer queue
    sp<BufferQueue> mBufferQueue;

    // Array for caching buffers from the buffer queue
    sp<GraphicBuffer> mBufferSlot[BufferQueue::NUM_BUFFER_SLOTS];
    // Count of currently outstanding buffers
    uint32_t mCurrentLockedBuffers;

    // mMutex is the mutex used to prevent concurrent access to the member
    // variables of MediaConsumer objects. It must be locked whenever the
    // member variables are accessed.
    mutable Mutex mMutex;
};

} // namespace android

#endif // ANDROID_SERVERS_CAMERA_MEDIACONSUMER_H
