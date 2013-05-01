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

#define LOG_TAG "Camera3-InputStream"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

// This is needed for stdint.h to define INT64_MAX in C++
#define __STDC_LIMIT_MACROS

#include <utils/Log.h>
#include <utils/Trace.h>
#include "Camera3InputStream.h"

namespace android {

namespace camera3 {

Camera3InputStream::Camera3InputStream(int id,
        uint32_t width, uint32_t height, int format) :
        Camera3Stream(id, CAMERA3_STREAM_INPUT, width, height, 0, format),
        mTotalBufferCount(0),
        mDequeuedBufferCount(0),
        mFrameCount(0),
        mLastTimestamp(0) {
    mCombinedFence = new Fence();

    if (format == HAL_PIXEL_FORMAT_BLOB) {
        ALOGE("%s: Bad format, BLOB not supported", __FUNCTION__);
        mState = STATE_ERROR;
    }
}

Camera3InputStream::~Camera3InputStream() {
    disconnectLocked();
}

status_t Camera3InputStream::getInputBufferLocked(
        camera3_stream_buffer *buffer) {
    ATRACE_CALL();
    status_t res;

    // FIXME: will not work in (re-)registration
    if (mState == STATE_IN_CONFIG || mState == STATE_IN_RECONFIG) {
        ALOGE("%s: Stream %d: Buffer registration for input streams"
              " not implemented (state %d)",
              __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Allow acquire during IN_[RE]CONFIG for registration
    if (mState != STATE_CONFIGURED &&
            mState != STATE_IN_CONFIG && mState != STATE_IN_RECONFIG) {
        ALOGE("%s: Stream %d: Can't get buffers in unconfigured state %d",
                __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Only limit acquire amount when fully configured
    if (mState == STATE_CONFIGURED &&
            mDequeuedBufferCount == camera3_stream::max_buffers) {
        ALOGE("%s: Stream %d: Already acquired maximum number of simultaneous"
                " buffers (%d)", __FUNCTION__, mId,
                camera3_stream::max_buffers);
        return INVALID_OPERATION;
    }

    ANativeWindowBuffer* anb;
    int fenceFd;

    assert(mConsumer != 0);

    BufferItem bufferItem;
    res = mConsumer->acquireBuffer(&bufferItem, /*waitForFence*/false);

    if (res != OK) {
        ALOGE("%s: Stream %d: Can't acquire next output buffer: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    anb = bufferItem.mGraphicBuffer->getNativeBuffer();
    assert(anb != NULL);
    fenceFd = bufferItem.mFence->dup();
    /**
     * FenceFD now owned by HAL except in case of error,
     * in which case we reassign it to acquire_fence
     */

    // Handing out a raw pointer to this object. Increment internal refcount.
    incStrong(this);
    buffer->stream = this;
    buffer->buffer = &(anb->handle);
    buffer->acquire_fence = fenceFd;
    buffer->release_fence = -1;
    buffer->status = CAMERA3_BUFFER_STATUS_OK;

    mDequeuedBufferCount++;

    mBuffersInFlight.push_back(bufferItem);

    return OK;
}

status_t Camera3InputStream::returnInputBufferLocked(
        const camera3_stream_buffer &buffer) {
    ATRACE_CALL();
    status_t res;

    // returnBuffer may be called from a raw pointer, not a sp<>, and we'll be
    // decrementing the internal refcount next. In case this is the last ref, we
    // might get destructed on the decStrong(), so keep an sp around until the
    // end of the call - otherwise have to sprinkle the decStrong on all exit
    // points.
    sp<Camera3InputStream> keepAlive(this);
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

    bool bufferFound = false;
    BufferItem bufferItem;
    {
        // Find the buffer we are returning
        Vector<BufferItem>::iterator it, end;
        for (it = mBuffersInFlight.begin(), end = mBuffersInFlight.end();
             it != end;
             ++it) {

            const BufferItem& tmp = *it;
            ANativeWindowBuffer *anb = tmp.mGraphicBuffer->getNativeBuffer();
            if (anb != NULL && &(anb->handle) == buffer.buffer) {
                bufferFound = true;
                bufferItem = tmp;
                mBuffersInFlight.erase(it);
                mDequeuedBufferCount--;
            }
        }
    }
    if (!bufferFound) {
        ALOGE("%s: Stream %d: Can't return buffer that wasn't sent to HAL",
              __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    if (buffer.status == CAMERA3_BUFFER_STATUS_ERROR) {
        if (buffer.release_fence != -1) {
            ALOGE("%s: Stream %d: HAL should not set release_fence(%d) when "
                  "there is an error", __FUNCTION__, mId, buffer.release_fence);
            close(buffer.release_fence);
        }

        /**
         * Reassign release fence as the acquire fence incase of error
         */
        const_cast<camera3_stream_buffer*>(&buffer)->release_fence =
                buffer.acquire_fence;
    }

    /**
     * Unconditionally return buffer to the buffer queue.
     * - Fwk takes over the release_fence ownership
     */
    sp<Fence> releaseFence = new Fence(buffer.release_fence);
    res = mConsumer->releaseBuffer(bufferItem, releaseFence);
    if (res != OK) {
        ALOGE("%s: Stream %d: Error releasing buffer back to buffer queue:"
                " %s (%d)", __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    mCombinedFence = Fence::merge(mName, mCombinedFence, releaseFence);

    mBufferReturnedSignal.signal();

    return OK;

}

bool Camera3InputStream::hasOutstandingBuffersLocked() const {
    nsecs_t signalTime = mCombinedFence->getSignalTime();
    ALOGV("%s: Stream %d: Has %d outstanding buffers,"
            " buffer signal time is %lld",
            __FUNCTION__, mId, mDequeuedBufferCount, signalTime);
    if (mDequeuedBufferCount > 0 || signalTime == INT64_MAX) {
        return true;
    }
    return false;
}

status_t Camera3InputStream::waitUntilIdle(nsecs_t timeout) {
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

size_t Camera3InputStream::getBufferCountLocked() {
    return mTotalBufferCount;
}

status_t Camera3InputStream::disconnectLocked() {
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
        ALOGE("%s: Can't disconnect with %d buffers still acquired!",
                __FUNCTION__, mDequeuedBufferCount);
        return INVALID_OPERATION;
    }

    assert(mBuffersInFlight.size() == 0);

    /**
     *  no-op since we can't disconnect the producer from the consumer-side
     */

    mState = (mState == STATE_IN_RECONFIG) ? STATE_IN_CONFIG : STATE_CONSTRUCTED;
    return OK;
}

sp<IGraphicBufferProducer> Camera3InputStream::getProducerInterface() const {
    return mConsumer->getProducerInterface();
}

void Camera3InputStream::dump(int fd, const Vector<String16> &args) const {
    (void) args;
    String8 lines;
    lines.appendFormat("    Stream[%d]: Input\n", mId);
    lines.appendFormat("      State: %d\n", mState);
    lines.appendFormat("      Dims: %d x %d, format 0x%x\n",
            camera3_stream::width, camera3_stream::height,
            camera3_stream::format);
    lines.appendFormat("      Max size: %d\n", mMaxSize);
    lines.appendFormat("      Usage: %d, max HAL buffers: %d\n",
            camera3_stream::usage, camera3_stream::max_buffers);
    lines.appendFormat("      Frames produced: %d, last timestamp: %lld ns\n",
            mFrameCount, mLastTimestamp);
    lines.appendFormat("      Total buffers: %d, currently acquired: %d\n",
            mTotalBufferCount, mDequeuedBufferCount);
    write(fd, lines.string(), lines.size());
}

status_t Camera3InputStream::configureQueueLocked() {
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

    assert(mMaxSize == 0);
    assert(camera3_stream::format != HAL_PIXEL_FORMAT_BLOB);

    mTotalBufferCount = BufferQueue::MIN_UNDEQUEUED_BUFFERS +
                        camera3_stream::max_buffers;
    mDequeuedBufferCount = 0;
    mFrameCount = 0;

    if (mConsumer.get() == 0) {
        mConsumer = new BufferItemConsumer(camera3_stream::usage,
                                           mTotalBufferCount,
                                           /*synchronousMode*/true);
        mConsumer->setName(String8::format("Camera3-InputStream-%d", mId));
    }

    res = mConsumer->setDefaultBufferSize(camera3_stream::width,
                                          camera3_stream::height);
    if (res != OK) {
        ALOGE("%s: Stream %d: Could not set buffer dimensions %dx%d",
              __FUNCTION__, mId, camera3_stream::width, camera3_stream::height);
        return res;
    }
    res = mConsumer->setDefaultBufferFormat(camera3_stream::format);
    if (res != OK) {
        ALOGE("%s: Stream %d: Could not set buffer format %d",
              __FUNCTION__, mId, camera3_stream::format);
        return res;
    }

    return OK;
}

}; // namespace camera3

}; // namespace android
