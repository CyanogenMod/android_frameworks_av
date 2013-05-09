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

#define LOG_TAG "Camera3-OutputStream"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

// This is needed for stdint.h to define INT64_MAX in C++
#define __STDC_LIMIT_MACROS

#include <utils/Log.h>
#include <utils/Trace.h>
#include "Camera3OutputStream.h"

#ifndef container_of
#define container_of(ptr, type, member) \
    (type *)((char*)(ptr) - offsetof(type, member))
#endif

namespace android {

namespace camera3 {

Camera3OutputStream::Camera3OutputStream(int id,
        sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, int format) :
        Camera3Stream(id, CAMERA3_STREAM_OUTPUT, width, height, 0, format),
        mConsumer(consumer),
        mTransform(0),
        mTotalBufferCount(0),
        mDequeuedBufferCount(0),
        mFrameCount(0),
        mLastTimestamp(0) {

    mCombinedFence = new Fence();
    if (mConsumer == NULL) {
        ALOGE("%s: Consumer is NULL!", __FUNCTION__);
        mState = STATE_ERROR;
    }
}

Camera3OutputStream::Camera3OutputStream(int id,
        sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, size_t maxSize, int format) :
        Camera3Stream(id, CAMERA3_STREAM_OUTPUT,
                width, height, maxSize, format),
        mConsumer(consumer),
        mTransform(0),
        mTotalBufferCount(0),
        mDequeuedBufferCount(0),
        mFrameCount(0),
        mLastTimestamp(0) {

    mCombinedFence = new Fence();

    if (format != HAL_PIXEL_FORMAT_BLOB) {
        ALOGE("%s: Bad format for size-only stream: %d", __FUNCTION__,
                format);
        mState = STATE_ERROR;
    }

    if (mConsumer == NULL) {
        ALOGE("%s: Consumer is NULL!", __FUNCTION__);
        mState = STATE_ERROR;
    }
}

Camera3OutputStream::~Camera3OutputStream() {
    disconnectLocked();
}

status_t Camera3OutputStream::getBufferLocked(camera3_stream_buffer *buffer) {
    ATRACE_CALL();
    status_t res;

    // Allow dequeue during IN_[RE]CONFIG for registration
    if (mState != STATE_CONFIGURED &&
            mState != STATE_IN_CONFIG && mState != STATE_IN_RECONFIG) {
        ALOGE("%s: Stream %d: Can't get buffers in unconfigured state %d",
                __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Only limit dequeue amount when fully configured
    if (mState == STATE_CONFIGURED &&
            mDequeuedBufferCount == camera3_stream::max_buffers) {
        ALOGE("%s: Stream %d: Already dequeued maximum number of simultaneous"
                " buffers (%d)", __FUNCTION__, mId,
                camera3_stream::max_buffers);
        return INVALID_OPERATION;
    }

    ANativeWindowBuffer* anb;
    int fenceFd;

    res = mConsumer->dequeueBuffer(mConsumer.get(), &anb, &fenceFd);
    if (res != OK) {
        ALOGE("%s: Stream %d: Can't dequeue next output buffer: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    // Handing out a raw pointer to this object. Increment internal refcount.
    incStrong(this);
    buffer->stream = this;
    buffer->buffer = &(anb->handle);
    buffer->acquire_fence = fenceFd;
    buffer->release_fence = -1;
    buffer->status = CAMERA3_BUFFER_STATUS_OK;

    mDequeuedBufferCount++;

    return OK;
}

status_t Camera3OutputStream::returnBufferLocked(
        const camera3_stream_buffer &buffer,
        nsecs_t timestamp) {
    ATRACE_CALL();
    status_t res;

    // returnBuffer may be called from a raw pointer, not a sp<>, and we'll be
    // decrementing the internal refcount next. In case this is the last ref, we
    // might get destructed on the decStrong(), so keep an sp around until the
    // end of the call - otherwise have to sprinkle the decStrong on all exit
    // points.
    sp<Camera3OutputStream> keepAlive(this);
    decStrong(this);

    // Allow buffers to be returned in the error state, to allow for disconnect
    // and in the in-config states for registration
    if (mState == STATE_CONSTRUCTED) {
        ALOGE("%s: Stream %d: Can't return buffers in unconfigured state %d",
                __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }
    if (mDequeuedBufferCount == 0) {
        ALOGE("%s: Stream %d: No buffers outstanding to return", __FUNCTION__,
                mId);
        return INVALID_OPERATION;
    }

    sp<Fence> releaseFence;

    /**
     * Fence management - calculate Release Fence
     */
    if (buffer.status == CAMERA3_BUFFER_STATUS_ERROR) {
        if (buffer.release_fence != -1) {
            ALOGE("%s: Stream %d: HAL should not set release_fence(%d) when "
                  "there is an error", __FUNCTION__, mId, buffer.release_fence);
            close(buffer.release_fence);
        }

        /**
         * Reassign release fence as the acquire fence in case of error
         */
        releaseFence = new Fence(buffer.acquire_fence);
    } else {
        res = native_window_set_buffers_timestamp(mConsumer.get(), timestamp);
        if (res != OK) {
            ALOGE("%s: Stream %d: Error setting timestamp: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return res;
        }

        releaseFence = new Fence(buffer.release_fence);
    }

    int anwReleaseFence = releaseFence->dup();

    /**
     * Return buffer back to ANativeWindow
     */
    if (buffer.status == CAMERA3_BUFFER_STATUS_ERROR) {
        // Cancel buffer
        res = mConsumer->cancelBuffer(mConsumer.get(),
                container_of(buffer.buffer, ANativeWindowBuffer, handle),
                anwReleaseFence);
        if (res != OK) {
            ALOGE("%s: Stream %d: Error cancelling buffer to native window:"
                    " %s (%d)", __FUNCTION__, mId, strerror(-res), res);
        }
    } else {
        res = mConsumer->queueBuffer(mConsumer.get(),
                container_of(buffer.buffer, ANativeWindowBuffer, handle),
                anwReleaseFence);
        if (res != OK) {
            ALOGE("%s: Stream %d: Error queueing buffer to native window: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
        }
    }

    if (res != OK) {
        close(anwReleaseFence);
        return res;
    }

    mCombinedFence = Fence::merge(mName, mCombinedFence, releaseFence);

    mDequeuedBufferCount--;
    mBufferReturnedSignal.signal();
    mLastTimestamp = timestamp;

    return OK;
}

bool Camera3OutputStream::hasOutstandingBuffersLocked() const {
    nsecs_t signalTime = mCombinedFence->getSignalTime();
    ALOGV("%s: Stream %d: Has %d outstanding buffers,"
            " buffer signal time is %lld",
            __FUNCTION__, mId, mDequeuedBufferCount, signalTime);
    if (mDequeuedBufferCount > 0 || signalTime == INT64_MAX) {
        return true;
    }
    return false;
}

status_t Camera3OutputStream::waitUntilIdle(nsecs_t timeout) {
    status_t res;
    {
        Mutex::Autolock l(mLock);
        while (mDequeuedBufferCount > 0) {
            if (timeout != TIMEOUT_NEVER) {
                nsecs_t startTime = systemTime();
                res = mBufferReturnedSignal.waitRelative(mLock, timeout);
                if (res == TIMED_OUT) {
                    return res;
                } else if (res != OK) {
                    ALOGE("%s: Error waiting for outstanding buffers: %s (%d)",
                            __FUNCTION__, strerror(-res), res);
                    return res;
                }
                nsecs_t deltaTime = systemTime() - startTime;
                if (timeout <= deltaTime) {
                    timeout = 0;
                } else {
                    timeout -= deltaTime;
                }
            } else {
                res = mBufferReturnedSignal.wait(mLock);
                if (res != OK) {
                    ALOGE("%s: Error waiting for outstanding buffers: %s (%d)",
                            __FUNCTION__, strerror(-res), res);
                    return res;
                }
            }
        }
    }

    // No lock

    unsigned int timeoutMs;
    if (timeout == TIMEOUT_NEVER) {
        timeoutMs = Fence::TIMEOUT_NEVER;
    } else if (timeout == 0) {
        timeoutMs = 0;
    } else {
        // Round up to wait at least 1 ms
        timeoutMs = (timeout + 999999) / 1000000;
    }

    return mCombinedFence->wait(timeoutMs);
}

void Camera3OutputStream::dump(int fd, const Vector<String16> &args) const {
    (void) args;
    String8 lines;
    lines.appendFormat("    Stream[%d]: Output\n", mId);
    lines.appendFormat("      State: %d\n", mState);
    lines.appendFormat("      Dims: %d x %d, format 0x%x\n",
            camera3_stream::width, camera3_stream::height,
            camera3_stream::format);
    lines.appendFormat("      Max size: %d\n", mMaxSize);
    lines.appendFormat("      Usage: %d, max HAL buffers: %d\n",
            camera3_stream::usage, camera3_stream::max_buffers);
    lines.appendFormat("      Frames produced: %d, last timestamp: %lld ns\n",
            mFrameCount, mLastTimestamp);
    lines.appendFormat("      Total buffers: %d, currently dequeued: %d\n",
            mTotalBufferCount, mDequeuedBufferCount);
    write(fd, lines.string(), lines.size());
}

status_t Camera3OutputStream::setTransform(int transform) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    return setTransformLocked(transform);
}

status_t Camera3OutputStream::setTransformLocked(int transform) {
    status_t res = OK;
    if (mState == STATE_ERROR) {
        ALOGE("%s: Stream in error state", __FUNCTION__);
        return INVALID_OPERATION;
    }

    mTransform = transform;
    if (mState == STATE_CONFIGURED) {
        res = native_window_set_buffers_transform(mConsumer.get(),
                transform);
        if (res != OK) {
            ALOGE("%s: Unable to configure stream transform to %x: %s (%d)",
                    __FUNCTION__, transform, strerror(-res), res);
        }
    }
    return res;
}

status_t Camera3OutputStream::configureQueueLocked() {
    status_t res;

    switch (mState) {
        case STATE_IN_RECONFIG:
            res = disconnectLocked();
            if (res != OK) {
                return res;
            }
            break;
        case STATE_IN_CONFIG:
            // OK
            break;
        default:
            ALOGE("%s: Bad state: %d", __FUNCTION__, mState);
            return INVALID_OPERATION;
    }

    // Configure consumer-side ANativeWindow interface
    res = native_window_api_connect(mConsumer.get(),
            NATIVE_WINDOW_API_CAMERA);
    if (res != OK) {
        ALOGE("%s: Unable to connect to native window for stream %d",
                __FUNCTION__, mId);
        return res;
    }

    res = native_window_set_usage(mConsumer.get(), camera3_stream::usage);
    if (res != OK) {
        ALOGE("%s: Unable to configure usage %08x for stream %d",
                __FUNCTION__, camera3_stream::usage, mId);
        return res;
    }

    res = native_window_set_scaling_mode(mConsumer.get(),
            NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (res != OK) {
        ALOGE("%s: Unable to configure stream scaling: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }

    res = setTransformLocked(0);
    if (res != OK) {
        return res;
    }

    if (mMaxSize == 0) {
        // For buffers of known size
        res = native_window_set_buffers_geometry(mConsumer.get(),
                camera3_stream::width, camera3_stream::height,
                camera3_stream::format);
    } else {
        // For buffers with bounded size
        res = native_window_set_buffers_geometry(mConsumer.get(),
                mMaxSize, 1,
                camera3_stream::format);
    }
    if (res != OK) {
        ALOGE("%s: Unable to configure stream buffer geometry"
                " %d x %d, format %x for stream %d",
                __FUNCTION__, camera3_stream::width, camera3_stream::height,
                camera3_stream::format, mId);
        return res;
    }

    int maxConsumerBuffers;
    res = mConsumer->query(mConsumer.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &maxConsumerBuffers);
    if (res != OK) {
        ALOGE("%s: Unable to query consumer undequeued"
                " buffer count for stream %d", __FUNCTION__, mId);
        return res;
    }

    ALOGV("%s: Consumer wants %d buffers", __FUNCTION__,
            maxConsumerBuffers);

    mTotalBufferCount = maxConsumerBuffers + camera3_stream::max_buffers;
    mDequeuedBufferCount = 0;
    mFrameCount = 0;
    mLastTimestamp = 0;

    res = native_window_set_buffer_count(mConsumer.get(),
            mTotalBufferCount);
    if (res != OK) {
        ALOGE("%s: Unable to set buffer count for stream %d",
                __FUNCTION__, mId);
        return res;
    }

    res = native_window_set_buffers_transform(mConsumer.get(),
            mTransform);
    if (res != OK) {
        ALOGE("%s: Unable to configure stream transform to %x: %s (%d)",
                __FUNCTION__, mTransform, strerror(-res), res);
    }

    return OK;
}

size_t Camera3OutputStream::getBufferCountLocked() {
    return mTotalBufferCount;
}

status_t Camera3OutputStream::disconnectLocked() {
    status_t res;

    switch (mState) {
        case STATE_IN_RECONFIG:
        case STATE_CONFIGURED:
            // OK
            break;
        default:
            // No connection, nothing to do
            return OK;
    }

    if (mDequeuedBufferCount > 0) {
        ALOGE("%s: Can't disconnect with %d buffers still dequeued!",
                __FUNCTION__, mDequeuedBufferCount);
        return INVALID_OPERATION;
    }

    res = native_window_api_disconnect(mConsumer.get(), NATIVE_WINDOW_API_CAMERA);

    /**
     * This is not an error. if client calling process dies, the window will
     * also die and all calls to it will return DEAD_OBJECT, thus it's already
     * "disconnected"
     */
    if (res == DEAD_OBJECT) {
        ALOGW("%s: While disconnecting stream %d from native window, the"
                " native window died from under us", __FUNCTION__, mId);
    }
    else if (res != OK) {
        ALOGE("%s: Unable to disconnect stream %d from native window (error %d %s)",
                __FUNCTION__, mId, res, strerror(-res));
        mState = STATE_ERROR;
        return res;
    }

    mState = (mState == STATE_IN_RECONFIG) ? STATE_IN_CONFIG : STATE_CONSTRUCTED;
    return OK;
}

}; // namespace camera3

}; // namespace android
