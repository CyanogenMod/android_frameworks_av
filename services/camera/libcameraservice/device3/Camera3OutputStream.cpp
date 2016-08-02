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
        sp<Surface> consumer,
        uint32_t width, uint32_t height, int format,
        android_dataspace dataSpace, camera3_stream_rotation_t rotation,
        nsecs_t timestampOffset, int setId) :
        Camera3IOStreamBase(id, CAMERA3_STREAM_OUTPUT, width, height,
                            /*maxSize*/0, format, dataSpace, rotation, setId),
        mConsumer(consumer),
        mTransform(0),
        mTraceFirstBuffer(true),
        mUseBufferManager(false),
        mTimestampOffset(timestampOffset),
        mConsumerUsage(0) {

    if (mConsumer == NULL) {
        ALOGE("%s: Consumer is NULL!", __FUNCTION__);
        mState = STATE_ERROR;
    }

    if (setId > CAMERA3_STREAM_SET_ID_INVALID) {
        mBufferReleasedListener = new BufferReleasedListener(this);
    }
}

Camera3OutputStream::Camera3OutputStream(int id,
        sp<Surface> consumer,
        uint32_t width, uint32_t height, size_t maxSize, int format,
        android_dataspace dataSpace, camera3_stream_rotation_t rotation,
        nsecs_t timestampOffset, int setId) :
        Camera3IOStreamBase(id, CAMERA3_STREAM_OUTPUT, width, height, maxSize,
                            format, dataSpace, rotation, setId),
        mConsumer(consumer),
        mTransform(0),
        mTraceFirstBuffer(true),
        mUseMonoTimestamp(false),
        mUseBufferManager(false),
        mTimestampOffset(timestampOffset),
        mConsumerUsage(0) {

    if (format != HAL_PIXEL_FORMAT_BLOB && format != HAL_PIXEL_FORMAT_RAW_OPAQUE) {
        ALOGE("%s: Bad format for size-only stream: %d", __FUNCTION__,
                format);
        mState = STATE_ERROR;
    }

    if (mConsumer == NULL) {
        ALOGE("%s: Consumer is NULL!", __FUNCTION__);
        mState = STATE_ERROR;
    }

    if (setId > CAMERA3_STREAM_SET_ID_INVALID) {
        mBufferReleasedListener = new BufferReleasedListener(this);
    }
}

Camera3OutputStream::Camera3OutputStream(int id,
        uint32_t width, uint32_t height, int format,
        uint32_t consumerUsage, android_dataspace dataSpace,
        camera3_stream_rotation_t rotation, nsecs_t timestampOffset, int setId) :
        Camera3IOStreamBase(id, CAMERA3_STREAM_OUTPUT, width, height,
                            /*maxSize*/0, format, dataSpace, rotation, setId),
        mConsumer(nullptr),
        mTransform(0),
        mTraceFirstBuffer(true),
        mUseBufferManager(false),
        mTimestampOffset(timestampOffset),
        mConsumerUsage(consumerUsage) {
    // Deferred consumer only support preview surface format now.
    if (format != HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        ALOGE("%s: Deferred consumer only supports IMPLEMENTATION_DEFINED format now!",
                __FUNCTION__);
        mState = STATE_ERROR;
    }

    // Sanity check for the consumer usage flag.
    if ((consumerUsage & GraphicBuffer::USAGE_HW_TEXTURE) == 0 &&
            (consumerUsage & GraphicBuffer::USAGE_HW_COMPOSER) == 0) {
        ALOGE("%s: Deferred consumer usage flag is illegal (0x%x)!", __FUNCTION__, consumerUsage);
        mState = STATE_ERROR;
    }

    mConsumerName = String8("Deferred");
    if (setId > CAMERA3_STREAM_SET_ID_INVALID) {
        mBufferReleasedListener = new BufferReleasedListener(this);
    }

}

Camera3OutputStream::Camera3OutputStream(int id, camera3_stream_type_t type,
                                         uint32_t width, uint32_t height,
                                         int format,
                                         android_dataspace dataSpace,
                                         camera3_stream_rotation_t rotation,
                                         int setId) :
        Camera3IOStreamBase(id, type, width, height,
                            /*maxSize*/0,
                            format, dataSpace, rotation, setId),
        mTransform(0),
        mTraceFirstBuffer(true),
        mUseMonoTimestamp(false),
        mUseBufferManager(false),
        mConsumerUsage(0) {

    if (setId > CAMERA3_STREAM_SET_ID_INVALID) {
        mBufferReleasedListener = new BufferReleasedListener(this);
    }

    // Subclasses expected to initialize mConsumer themselves
}


Camera3OutputStream::~Camera3OutputStream() {
    disconnectLocked();
}

status_t Camera3OutputStream::getBufferLocked(camera3_stream_buffer *buffer) {
    ATRACE_CALL();
    status_t res;

    if ((res = getBufferPreconditionCheckLocked()) != OK) {
        return res;
    }

    ANativeWindowBuffer* anb;
    int fenceFd = -1;
    bool gotBufferFromManager = false;

    if (mUseBufferManager) {
        sp<GraphicBuffer> gb;
        res = mBufferManager->getBufferForStream(getId(), getStreamSetId(), &gb, &fenceFd);
        if (res == OK) {
            // Attach this buffer to the bufferQueue: the buffer will be in dequeue state after a
            // successful return.
            anb = gb.get();
            res = mConsumer->attachBuffer(anb);
            if (res != OK) {
                ALOGE("%s: Stream %d: Can't attach the output buffer to this surface: %s (%d)",
                        __FUNCTION__, mId, strerror(-res), res);
                return res;
            }
            gotBufferFromManager = true;
            ALOGV("Stream %d: Attached new buffer", getId());
        } else if (res == ALREADY_EXISTS) {
            // Have sufficient free buffers already attached, can just
            // dequeue from buffer queue
            ALOGV("Stream %d: Reusing attached buffer", getId());
            gotBufferFromManager = false;
        } else if (res != OK) {
            ALOGE("%s: Stream %d: Can't get next output buffer from buffer manager: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    }
    if (!gotBufferFromManager) {
        /**
         * Release the lock briefly to avoid deadlock for below scenario:
         * Thread 1: StreamingProcessor::startStream -> Camera3Stream::isConfiguring().
         * This thread acquired StreamingProcessor lock and try to lock Camera3Stream lock.
         * Thread 2: Camera3Stream::returnBuffer->StreamingProcessor::onFrameAvailable().
         * This thread acquired Camera3Stream lock and bufferQueue lock, and try to lock
         * StreamingProcessor lock.
         * Thread 3: Camera3Stream::getBuffer(). This thread acquired Camera3Stream lock
         * and try to lock bufferQueue lock.
         * Then there is circular locking dependency.
         */
        sp<ANativeWindow> currentConsumer = mConsumer;
        mLock.unlock();

        res = currentConsumer->dequeueBuffer(currentConsumer.get(), &anb, &fenceFd);
        mLock.lock();
        if (res != OK) {
            ALOGE("%s: Stream %d: Can't dequeue next output buffer: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);

            // Only transition to STATE_ABANDONED from STATE_CONFIGURED. (If it is STATE_PREPARING,
            // let prepareNextBuffer handle the error.)
            if (res == NO_INIT && mState == STATE_CONFIGURED) {
                mState = STATE_ABANDONED;
            }

            return res;
        }
    }

    /**
     * FenceFD now owned by HAL except in case of error,
     * in which case we reassign it to acquire_fence
     */
    handoutBufferLocked(*buffer, &(anb->handle), /*acquireFence*/fenceFd,
                        /*releaseFence*/-1, CAMERA3_BUFFER_STATUS_OK, /*output*/true);

    return OK;
}

status_t Camera3OutputStream::returnBufferLocked(
        const camera3_stream_buffer &buffer,
        nsecs_t timestamp) {
    ATRACE_CALL();

    status_t res = returnAnyBufferLocked(buffer, timestamp, /*output*/true);

    if (res != OK) {
        return res;
    }

    mLastTimestamp = timestamp;

    return OK;
}

status_t Camera3OutputStream::returnBufferCheckedLocked(
            const camera3_stream_buffer &buffer,
            nsecs_t timestamp,
            bool output,
            /*out*/
            sp<Fence> *releaseFenceOut) {

    (void)output;
    ALOG_ASSERT(output, "Expected output to be true");

    status_t res;

    // Fence management - always honor release fence from HAL
    sp<Fence> releaseFence = new Fence(buffer.release_fence);
    int anwReleaseFence = releaseFence->dup();

    /**
     * Release the lock briefly to avoid deadlock with
     * StreamingProcessor::startStream -> Camera3Stream::isConfiguring (this
     * thread will go into StreamingProcessor::onFrameAvailable) during
     * queueBuffer
     */
    sp<ANativeWindow> currentConsumer = mConsumer;
    mLock.unlock();

    /**
     * Return buffer back to ANativeWindow
     */
    if (buffer.status == CAMERA3_BUFFER_STATUS_ERROR) {
        // Cancel buffer
        ALOGW("A frame is dropped for stream %d", mId);
        res = currentConsumer->cancelBuffer(currentConsumer.get(),
                container_of(buffer.buffer, ANativeWindowBuffer, handle),
                anwReleaseFence);
        if (res != OK) {
            ALOGE("%s: Stream %d: Error cancelling buffer to native window:"
                  " %s (%d)", __FUNCTION__, mId, strerror(-res), res);
        }

        if (mUseBufferManager) {
            // Return this buffer back to buffer manager.
            mBufferReleasedListener->onBufferReleased();
        }
    } else {
        if (mTraceFirstBuffer && (stream_type == CAMERA3_STREAM_OUTPUT)) {
            {
                char traceLog[48];
                snprintf(traceLog, sizeof(traceLog), "Stream %d: first full buffer\n", mId);
                ATRACE_NAME(traceLog);
            }
            mTraceFirstBuffer = false;
        }

        /* Certain consumers (such as AudioSource or HardwareComposer) use
         * MONOTONIC time, causing time misalignment if camera timestamp is
         * in BOOTTIME. Do the conversion if necessary. */
        res = native_window_set_buffers_timestamp(mConsumer.get(),
                mUseMonoTimestamp ? timestamp - mTimestampOffset : timestamp);
        if (res != OK) {
            ALOGE("%s: Stream %d: Error setting timestamp: %s (%d)",
                  __FUNCTION__, mId, strerror(-res), res);
            return res;
        }

        res = currentConsumer->queueBuffer(currentConsumer.get(),
                container_of(buffer.buffer, ANativeWindowBuffer, handle),
                anwReleaseFence);
        if (res != OK) {
            ALOGE("%s: Stream %d: Error queueing buffer to native window: "
                  "%s (%d)", __FUNCTION__, mId, strerror(-res), res);
        }
    }
    mLock.lock();

    // Once a valid buffer has been returned to the queue, can no longer
    // dequeue all buffers for preallocation.
    if (buffer.status != CAMERA3_BUFFER_STATUS_ERROR) {
        mStreamUnpreparable = true;
    }

    if (res != OK) {
        close(anwReleaseFence);
    }

    *releaseFenceOut = releaseFence;

    return res;
}

void Camera3OutputStream::dump(int fd, const Vector<String16> &args) const {
    (void) args;
    String8 lines;
    lines.appendFormat("    Stream[%d]: Output\n", mId);
    lines.appendFormat("      Consumer name: %s\n", mConsumerName.string());
    write(fd, lines.string(), lines.size());

    Camera3IOStreamBase::dump(fd, args);
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

    mTraceFirstBuffer = true;
    if ((res = Camera3IOStreamBase::configureQueueLocked()) != OK) {
        return res;
    }

    ALOG_ASSERT(mConsumer != 0, "mConsumer should never be NULL");

    // Configure consumer-side ANativeWindow interface. The listener may be used
    // to notify buffer manager (if it is used) of the returned buffers.
    res = mConsumer->connect(NATIVE_WINDOW_API_CAMERA, /*listener*/mBufferReleasedListener);
    if (res != OK) {
        ALOGE("%s: Unable to connect to native window for stream %d",
                __FUNCTION__, mId);
        return res;
    }

    mConsumerName = mConsumer->getConsumerName();

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

    if (mMaxSize == 0) {
        // For buffers of known size
        res = native_window_set_buffers_dimensions(mConsumer.get(),
                camera3_stream::width, camera3_stream::height);
    } else {
        // For buffers with bounded size
        res = native_window_set_buffers_dimensions(mConsumer.get(),
                mMaxSize, 1);
    }
    if (res != OK) {
        ALOGE("%s: Unable to configure stream buffer dimensions"
                " %d x %d (maxSize %zu) for stream %d",
                __FUNCTION__, camera3_stream::width, camera3_stream::height,
                mMaxSize, mId);
        return res;
    }
    res = native_window_set_buffers_format(mConsumer.get(),
            camera3_stream::format);
    if (res != OK) {
        ALOGE("%s: Unable to configure stream buffer format %#x for stream %d",
                __FUNCTION__, camera3_stream::format, mId);
        return res;
    }

    res = native_window_set_buffers_data_space(mConsumer.get(),
            camera3_stream::data_space);
    if (res != OK) {
        ALOGE("%s: Unable to configure stream dataspace %#x for stream %d",
                __FUNCTION__, camera3_stream::data_space, mId);
        return res;
    }

    int maxConsumerBuffers;
    res = static_cast<ANativeWindow*>(mConsumer.get())->query(
            mConsumer.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &maxConsumerBuffers);
    if (res != OK) {
        ALOGE("%s: Unable to query consumer undequeued"
                " buffer count for stream %d", __FUNCTION__, mId);
        return res;
    }

    ALOGV("%s: Consumer wants %d buffers, HAL wants %d", __FUNCTION__,
            maxConsumerBuffers, camera3_stream::max_buffers);
    if (camera3_stream::max_buffers == 0) {
        ALOGE("%s: Camera HAL requested max_buffer count: %d, requires at least 1",
                __FUNCTION__, camera3_stream::max_buffers);
        return INVALID_OPERATION;
    }

    mTotalBufferCount = maxConsumerBuffers + camera3_stream::max_buffers;
    mHandoutTotalBufferCount = 0;
    mFrameCount = 0;
    mLastTimestamp = 0;
    mUseMonoTimestamp = (isConsumedByHWComposer() | isVideoStream());

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

    // Set dequeueBuffer/attachBuffer timeout if the consumer is not hw composer or hw texture.
    // We need skip these cases as timeout will disable the non-blocking (async) mode.
    if (!(isConsumedByHWComposer() || isConsumedByHWTexture())) {
        mConsumer->setDequeueTimeout(kDequeueBufferTimeout);
    }

    /**
     * Camera3 Buffer manager is only supported by HAL3.3 onwards, as the older HALs requires
     * buffers to be statically allocated for internal static buffer registration, while the
     * buffers provided by buffer manager are really dynamically allocated. Camera3Device only
     * sets the mBufferManager if device version is > HAL3.2, which guarantees that the buffer
     * manager setup is skipped in below code. Note that HAL3.2 is also excluded here, as some
     * HAL3.2 devices may not support the dynamic buffer registeration.
     */
    if (mBufferManager != 0 && mSetId > CAMERA3_STREAM_SET_ID_INVALID) {
        uint32_t consumerUsage = 0;
        getEndpointUsage(&consumerUsage);
        StreamInfo streamInfo(
                getId(), getStreamSetId(), getWidth(), getHeight(), getFormat(), getDataSpace(),
                camera3_stream::usage | consumerUsage, mTotalBufferCount,
                /*isConfigured*/true);
        wp<Camera3OutputStream> weakThis(this);
        res = mBufferManager->registerStream(weakThis,
                streamInfo);
        if (res == OK) {
            // Disable buffer allocation for this BufferQueue, buffer manager will take over
            // the buffer allocation responsibility.
            mConsumer->getIGraphicBufferProducer()->allowAllocation(false);
            mUseBufferManager = true;
        } else {
            ALOGE("%s: Unable to register stream %d to camera3 buffer manager, "
                  "(error %d %s), fall back to BufferQueue for buffer management!",
                  __FUNCTION__, mId, res, strerror(-res));
        }
    }

    return OK;
}

status_t Camera3OutputStream::disconnectLocked() {
    status_t res;

    if ((res = Camera3IOStreamBase::disconnectLocked()) != OK) {
        return res;
    }

    // Stream configuration was not finished (can only be in STATE_IN_CONFIG or STATE_CONSTRUCTED
    // state), don't need change the stream state, return OK.
    if (mConsumer == nullptr) {
        return OK;
    }

    ALOGV("%s: disconnecting stream %d from native window", __FUNCTION__, getId());

    res = native_window_api_disconnect(mConsumer.get(),
                                       NATIVE_WINDOW_API_CAMERA);
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
        ALOGE("%s: Unable to disconnect stream %d from native window "
              "(error %d %s)",
              __FUNCTION__, mId, res, strerror(-res));
        mState = STATE_ERROR;
        return res;
    }

    // Since device is already idle, there is no getBuffer call to buffer manager, unregister the
    // stream at this point should be safe.
    if (mUseBufferManager) {
        res = mBufferManager->unregisterStream(getId(), getStreamSetId());
        if (res != OK) {
            ALOGE("%s: Unable to unregister stream %d from buffer manager "
                    "(error %d %s)", __FUNCTION__, mId, res, strerror(-res));
            mState = STATE_ERROR;
            return res;
        }
        // Note that, to make prepare/teardown case work, we must not mBufferManager.clear(), as
        // the stream is still in usable state after this call.
        mUseBufferManager = false;
    }

    mState = (mState == STATE_IN_RECONFIG) ? STATE_IN_CONFIG
                                           : STATE_CONSTRUCTED;
    return OK;
}

status_t Camera3OutputStream::getEndpointUsage(uint32_t *usage) const {

    status_t res;
    int32_t u = 0;
    if (mConsumer == nullptr) {
        // mConsumerUsage was sanitized before the Camera3OutputStream was constructed.
        *usage = mConsumerUsage;
        return OK;
    }

    res = static_cast<ANativeWindow*>(mConsumer.get())->query(mConsumer.get(),
            NATIVE_WINDOW_CONSUMER_USAGE_BITS, &u);

    // If an opaque output stream's endpoint is ImageReader, add
    // GRALLOC_USAGE_HW_CAMERA_ZSL to the usage so HAL knows it will be used
    // for the ZSL use case.
    // Assume it's for ImageReader if the consumer usage doesn't have any of these bits set:
    //     1. GRALLOC_USAGE_HW_TEXTURE
    //     2. GRALLOC_USAGE_HW_RENDER
    //     3. GRALLOC_USAGE_HW_COMPOSER
    //     4. GRALLOC_USAGE_HW_VIDEO_ENCODER
    if (camera3_stream::format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
            (u & (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER |
            GRALLOC_USAGE_HW_VIDEO_ENCODER)) == 0) {
        u |= GRALLOC_USAGE_HW_CAMERA_ZSL;
    }

    *usage = u;
    return res;
}

bool Camera3OutputStream::isVideoStream() const {
    uint32_t usage = 0;
    status_t res = getEndpointUsage(&usage);
    if (res != OK) {
        ALOGE("%s: getting end point usage failed: %s (%d).", __FUNCTION__, strerror(-res), res);
        return false;
    }

    return (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) != 0;
}

status_t Camera3OutputStream::setBufferManager(sp<Camera3BufferManager> bufferManager) {
    Mutex::Autolock l(mLock);
    if (mState != STATE_CONSTRUCTED) {
        ALOGE("%s: this method can only be called when stream in CONSTRUCTED state.",
                __FUNCTION__);
        return INVALID_OPERATION;
    }
    mBufferManager = bufferManager;

    return OK;
}

void Camera3OutputStream::BufferReleasedListener::onBufferReleased() {
    sp<Camera3OutputStream> stream = mParent.promote();
    if (stream == nullptr) {
        ALOGV("%s: Parent camera3 output stream was destroyed", __FUNCTION__);
        return;
    }

    Mutex::Autolock l(stream->mLock);
    if (!(stream->mUseBufferManager)) {
        return;
    }

    ALOGV("Stream %d: Buffer released", stream->getId());
    status_t res = stream->mBufferManager->onBufferReleased(
        stream->getId(), stream->getStreamSetId());
    if (res != OK) {
        ALOGE("%s: signaling buffer release to buffer manager failed: %s (%d).", __FUNCTION__,
                strerror(-res), res);
        stream->mState = STATE_ERROR;
    }
}

status_t Camera3OutputStream::detachBuffer(sp<GraphicBuffer>* buffer, int* fenceFd) {
    Mutex::Autolock l(mLock);

    ALOGV("Stream %d: detachBuffer", getId());
    if (buffer == nullptr) {
        return BAD_VALUE;
    }

    sp<Fence> fence;
    status_t res = mConsumer->detachNextBuffer(buffer, &fence);
    if (res == NO_MEMORY) {
        // This may rarely happen, which indicates that the released buffer was freed by other
        // call (e.g., attachBuffer, dequeueBuffer etc.) before reaching here. We should notify the
        // buffer manager that this buffer has been freed. It's not fatal, but should be avoided,
        // therefore log a warning.
        *buffer = 0;
        ALOGW("%s: the released buffer has already been freed by the buffer queue!", __FUNCTION__);
    } else if (res != OK) {
        // Treat other errors as abandonment
        ALOGE("%s: detach next buffer failed: %s (%d).", __FUNCTION__, strerror(-res), res);
        mState = STATE_ABANDONED;
        return res;
    }

    if (fenceFd != nullptr) {
        if (fence!= 0 && fence->isValid()) {
            *fenceFd = fence->dup();
        } else {
            *fenceFd = -1;
        }
    }

    return OK;
}

bool Camera3OutputStream::isConsumerConfigurationDeferred() const {
    Mutex::Autolock l(mLock);
    return mConsumer == nullptr;
}

status_t Camera3OutputStream::setConsumer(sp<Surface> consumer) {
    if (consumer == nullptr) {
        ALOGE("%s: it's illegal to set a null consumer surface!", __FUNCTION__);
        return INVALID_OPERATION;
    }

    if (mConsumer != nullptr) {
        ALOGE("%s: consumer surface was already set!", __FUNCTION__);
        return INVALID_OPERATION;
    }

    mConsumer = consumer;
    return OK;
}

bool Camera3OutputStream::isConsumedByHWComposer() const {
    uint32_t usage = 0;
    status_t res = getEndpointUsage(&usage);
    if (res != OK) {
        ALOGE("%s: getting end point usage failed: %s (%d).", __FUNCTION__, strerror(-res), res);
        return false;
    }

    return (usage & GRALLOC_USAGE_HW_COMPOSER) != 0;
}

bool Camera3OutputStream::isConsumedByHWTexture() const {
    uint32_t usage = 0;
    status_t res = getEndpointUsage(&usage);
    if (res != OK) {
        ALOGE("%s: getting end point usage failed: %s (%d).", __FUNCTION__, strerror(-res), res);
        return false;
    }

    return (usage & GRALLOC_USAGE_HW_TEXTURE) != 0;
}

}; // namespace camera3

}; // namespace android
