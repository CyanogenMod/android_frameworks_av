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

#define LOG_TAG "Camera3-Stream"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>
#include "device3/Camera3Stream.h"
#include "device3/StatusTracker.h"

#include <cutils/properties.h>

namespace android {

namespace camera3 {

Camera3Stream::~Camera3Stream() {
    sp<StatusTracker> statusTracker = mStatusTracker.promote();
    if (statusTracker != 0 && mStatusId != StatusTracker::NO_STATUS_ID) {
        statusTracker->removeComponent(mStatusId);
    }
}

Camera3Stream* Camera3Stream::cast(camera3_stream *stream) {
    return static_cast<Camera3Stream*>(stream);
}

const Camera3Stream* Camera3Stream::cast(const camera3_stream *stream) {
    return static_cast<const Camera3Stream*>(stream);
}

Camera3Stream::Camera3Stream(int id,
        camera3_stream_type type,
        uint32_t width, uint32_t height, size_t maxSize, int format,
        android_dataspace dataSpace, camera3_stream_rotation_t rotation, int setId) :
    camera3_stream(),
    mId(id),
    mSetId(setId),
    mName(String8::format("Camera3Stream[%d]", id)),
    mMaxSize(maxSize),
    mState(STATE_CONSTRUCTED),
    mStatusId(StatusTracker::NO_STATUS_ID),
    mStreamUnpreparable(true),
    mOldUsage(0),
    mOldMaxBuffers(0),
    mPrepared(false),
    mPreparedBufferIdx(0),
    mLastMaxCount(Camera3StreamInterface::ALLOCATE_PIPELINE_MAX) {

    camera3_stream::stream_type = type;
    camera3_stream::width = width;
    camera3_stream::height = height;
    camera3_stream::format = format;
    camera3_stream::data_space = dataSpace;
    camera3_stream::rotation = rotation;
    camera3_stream::usage = 0;
    camera3_stream::max_buffers = 0;
    camera3_stream::priv = NULL;

    if ((format == HAL_PIXEL_FORMAT_BLOB || format == HAL_PIXEL_FORMAT_RAW_OPAQUE) &&
            maxSize == 0) {
        ALOGE("%s: BLOB or RAW_OPAQUE format with size == 0", __FUNCTION__);
        mState = STATE_ERROR;
    }
}

int Camera3Stream::getId() const {
    return mId;
}

int Camera3Stream::getStreamSetId() const {
    return mSetId;
}

uint32_t Camera3Stream::getWidth() const {
    return camera3_stream::width;
}

uint32_t Camera3Stream::getHeight() const {
    return camera3_stream::height;
}

int Camera3Stream::getFormat() const {
    return camera3_stream::format;
}

android_dataspace Camera3Stream::getDataSpace() const {
    return camera3_stream::data_space;
}

camera3_stream* Camera3Stream::startConfiguration() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    status_t res;

    switch (mState) {
        case STATE_ERROR:
            ALOGE("%s: In error state", __FUNCTION__);
            return NULL;
        case STATE_CONSTRUCTED:
            // OK
            break;
        case STATE_IN_CONFIG:
        case STATE_IN_RECONFIG:
            // Can start config again with no trouble; but don't redo
            // mOldUsage/mOldMaxBuffers
            return this;
        case STATE_CONFIGURED:
            if (hasOutstandingBuffersLocked()) {
                ALOGE("%s: Cannot configure stream; has outstanding buffers",
                        __FUNCTION__);
                return NULL;
            }
            break;
        default:
            ALOGE("%s: Unknown state %d", __FUNCTION__, mState);
            return NULL;
    }

    mOldUsage = camera3_stream::usage;
    mOldMaxBuffers = camera3_stream::max_buffers;

    res = getEndpointUsage(&(camera3_stream::usage));
    if (res != OK) {
        ALOGE("%s: Cannot query consumer endpoint usage!",
                __FUNCTION__);
        return NULL;
    }

    // Stop tracking if currently doing so
    if (mStatusId != StatusTracker::NO_STATUS_ID) {
        sp<StatusTracker> statusTracker = mStatusTracker.promote();
        if (statusTracker != 0) {
            statusTracker->removeComponent(mStatusId);
        }
        mStatusId = StatusTracker::NO_STATUS_ID;
    }

    if (mState == STATE_CONSTRUCTED) {
        mState = STATE_IN_CONFIG;
    } else { // mState == STATE_CONFIGURED
        LOG_ALWAYS_FATAL_IF(mState != STATE_CONFIGURED, "Invalid state: 0x%x", mState);
        mState = STATE_IN_RECONFIG;
    }

    return this;
}

bool Camera3Stream::isConfiguring() const {
    Mutex::Autolock l(mLock);
    return (mState == STATE_IN_CONFIG) || (mState == STATE_IN_RECONFIG);
}

status_t Camera3Stream::finishConfiguration(camera3_device *hal3Device) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    switch (mState) {
        case STATE_ERROR:
            ALOGE("%s: In error state", __FUNCTION__);
            return INVALID_OPERATION;
        case STATE_IN_CONFIG:
        case STATE_IN_RECONFIG:
            // OK
            break;
        case STATE_CONSTRUCTED:
        case STATE_CONFIGURED:
            ALOGE("%s: Cannot finish configuration that hasn't been started",
                    __FUNCTION__);
            return INVALID_OPERATION;
        default:
            ALOGE("%s: Unknown state", __FUNCTION__);
            return INVALID_OPERATION;
    }

    // Register for idle tracking
    sp<StatusTracker> statusTracker = mStatusTracker.promote();
    if (statusTracker != 0) {
        mStatusId = statusTracker->addComponent();
    }

    // Check if the stream configuration is unchanged, and skip reallocation if
    // so. As documented in hardware/camera3.h:configure_streams().
    if (mState == STATE_IN_RECONFIG &&
            mOldUsage == camera3_stream::usage &&
            mOldMaxBuffers == camera3_stream::max_buffers) {
        mState = STATE_CONFIGURED;
        return OK;
    }

    // Reset prepared state, since buffer config has changed, and existing
    // allocations are no longer valid
    mPrepared = false;
    mStreamUnpreparable = false;

    status_t res;
    res = configureQueueLocked();
    if (res != OK) {
        ALOGE("%s: Unable to configure stream %d queue: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        mState = STATE_ERROR;
        return res;
    }

    res = registerBuffersLocked(hal3Device);
    if (res != OK) {
        ALOGE("%s: Unable to register stream buffers with HAL: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        mState = STATE_ERROR;
        return res;
    }

    mState = STATE_CONFIGURED;

    return res;
}

status_t Camera3Stream::cancelConfiguration() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    switch (mState) {
        case STATE_ERROR:
            ALOGE("%s: In error state", __FUNCTION__);
            return INVALID_OPERATION;
        case STATE_IN_CONFIG:
        case STATE_IN_RECONFIG:
            // OK
            break;
        case STATE_CONSTRUCTED:
        case STATE_CONFIGURED:
            ALOGE("%s: Cannot cancel configuration that hasn't been started",
                    __FUNCTION__);
            return INVALID_OPERATION;
        default:
            ALOGE("%s: Unknown state", __FUNCTION__);
            return INVALID_OPERATION;
    }

    camera3_stream::usage = mOldUsage;
    camera3_stream::max_buffers = mOldMaxBuffers;

    mState = (mState == STATE_IN_RECONFIG) ? STATE_CONFIGURED : STATE_CONSTRUCTED;
    return OK;
}

bool Camera3Stream::isUnpreparable() {
    ATRACE_CALL();

    Mutex::Autolock l(mLock);
    return mStreamUnpreparable;
}

status_t Camera3Stream::startPrepare(int maxCount) {
    ATRACE_CALL();

    Mutex::Autolock l(mLock);

    if (maxCount < 0) {
        ALOGE("%s: Stream %d: Can't prepare stream if max buffer count (%d) is < 0",
                __FUNCTION__, mId, maxCount);
        return BAD_VALUE;
    }

    // This function should be only called when the stream is configured already.
    if (mState != STATE_CONFIGURED) {
        ALOGE("%s: Stream %d: Can't prepare stream if stream is not in CONFIGURED "
                "state %d", __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // This function can't be called if the stream has already received filled
    // buffers
    if (mStreamUnpreparable) {
        ALOGE("%s: Stream %d: Can't prepare stream that's already in use",
                __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    if (getHandoutOutputBufferCountLocked() > 0) {
        ALOGE("%s: Stream %d: Can't prepare stream that has outstanding buffers",
                __FUNCTION__, mId);
        return INVALID_OPERATION;
    }



    size_t pipelineMax = getBufferCountLocked();
    size_t clampedCount = (pipelineMax < static_cast<size_t>(maxCount)) ?
            pipelineMax : static_cast<size_t>(maxCount);
    size_t bufferCount = (maxCount == Camera3StreamInterface::ALLOCATE_PIPELINE_MAX) ?
            pipelineMax : clampedCount;

    mPrepared = bufferCount <= mLastMaxCount;

    if (mPrepared) return OK;

    mLastMaxCount = bufferCount;

    mPreparedBuffers.insertAt(camera3_stream_buffer_t(), /*index*/0, bufferCount);
    mPreparedBufferIdx = 0;

    mState = STATE_PREPARING;

    return NOT_ENOUGH_DATA;
}

bool Camera3Stream::isPreparing() const {
    Mutex::Autolock l(mLock);
    return mState == STATE_PREPARING;
}

bool Camera3Stream::isAbandoned() const {
    Mutex::Autolock l(mLock);
    return mState == STATE_ABANDONED;
}

status_t Camera3Stream::prepareNextBuffer() {
    ATRACE_CALL();

    Mutex::Autolock l(mLock);
    status_t res = OK;

    // This function should be only called when the stream is preparing
    if (mState != STATE_PREPARING) {
        ALOGE("%s: Stream %d: Can't prepare buffer if stream is not in PREPARING "
                "state %d", __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Get next buffer - this may allocate, and take a while for large buffers
    res = getBufferLocked( &mPreparedBuffers.editItemAt(mPreparedBufferIdx) );
    if (res != OK) {
        ALOGE("%s: Stream %d: Unable to allocate buffer %zu during preparation",
                __FUNCTION__, mId, mPreparedBufferIdx);
        return NO_INIT;
    }

    mPreparedBufferIdx++;

    // Check if we still have buffers left to allocate
    if (mPreparedBufferIdx < mPreparedBuffers.size()) {
        return NOT_ENOUGH_DATA;
    }

    // Done with prepare - mark stream as such, and return all buffers
    // via cancelPrepare
    mPrepared = true;

    return cancelPrepareLocked();
}

status_t Camera3Stream::cancelPrepare() {
    ATRACE_CALL();

    Mutex::Autolock l(mLock);

    return cancelPrepareLocked();
}

status_t Camera3Stream::cancelPrepareLocked() {
    status_t res = OK;

    // This function should be only called when the stream is mid-preparing.
    if (mState != STATE_PREPARING) {
        ALOGE("%s: Stream %d: Can't cancel prepare stream if stream is not in "
                "PREPARING state %d", __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Return all valid buffers to stream, in ERROR state to indicate
    // they weren't filled.
    for (size_t i = 0; i < mPreparedBufferIdx; i++) {
        mPreparedBuffers.editItemAt(i).release_fence = -1;
        mPreparedBuffers.editItemAt(i).status = CAMERA3_BUFFER_STATUS_ERROR;
        returnBufferLocked(mPreparedBuffers[i], 0);
    }
    mPreparedBuffers.clear();
    mPreparedBufferIdx = 0;

    mState = STATE_CONFIGURED;

    return res;
}

status_t Camera3Stream::tearDown() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    status_t res = OK;

    // This function should be only called when the stream is configured.
    if (mState != STATE_CONFIGURED) {
        ALOGE("%s: Stream %d: Can't tear down stream if stream is not in "
                "CONFIGURED state %d", __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // If any buffers have been handed to the HAL, the stream cannot be torn down.
    if (getHandoutOutputBufferCountLocked() > 0) {
        ALOGE("%s: Stream %d: Can't tear down a stream that has outstanding buffers",
                __FUNCTION__, mId);
        return INVALID_OPERATION;
    }

    // Free buffers by disconnecting and then reconnecting to the buffer queue
    // Only unused buffers will be dropped immediately; buffers that have been filled
    // and are waiting to be acquired by the consumer and buffers that are currently
    // acquired will be freed once they are released by the consumer.

    res = disconnectLocked();
    if (res != OK) {
        if (res == -ENOTCONN) {
            // queue has been disconnected, nothing left to do, so exit with success
            return OK;
        }
        ALOGE("%s: Stream %d: Unable to disconnect to tear down buffers: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        return res;
    }

    mState = STATE_IN_CONFIG;

    res = configureQueueLocked();
    if (res != OK) {
        ALOGE("%s: Unable to configure stream %d queue: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        mState = STATE_ERROR;
        return res;
    }

    // Reset prepared state, since we've reconnected to the queue and can prepare again.
    mPrepared = false;
    mStreamUnpreparable = false;

    mState = STATE_CONFIGURED;

    return OK;
}

status_t Camera3Stream::getBuffer(camera3_stream_buffer *buffer) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    status_t res = OK;

    // This function should be only called when the stream is configured already.
    if (mState != STATE_CONFIGURED) {
        ALOGE("%s: Stream %d: Can't get buffers if stream is not in CONFIGURED state %d",
                __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Wait for new buffer returned back if we are running into the limit.
    if (getHandoutOutputBufferCountLocked() == camera3_stream::max_buffers) {
        ALOGV("%s: Already dequeued max output buffers (%d), wait for next returned one.",
                __FUNCTION__, camera3_stream::max_buffers);
        res = mOutputBufferReturnedSignal.waitRelative(mLock, kWaitForBufferDuration);
        if (res != OK) {
            if (res == TIMED_OUT) {
                ALOGE("%s: wait for output buffer return timed out after %lldms (max_buffers %d)",
                        __FUNCTION__, kWaitForBufferDuration / 1000000LL,
                        camera3_stream::max_buffers);
            }
            return res;
        }
    }

    res = getBufferLocked(buffer);
    if (res == OK) {
        fireBufferListenersLocked(*buffer, /*acquired*/true, /*output*/true);
        if (buffer->buffer) {
            mOutstandingBuffers.push_back(*buffer->buffer);
        }
    }

    return res;
}

bool Camera3Stream::isOutstandingBuffer(const camera3_stream_buffer &buffer) {
    if (buffer.buffer == nullptr) {
        return false;
    }

    for (auto b : mOutstandingBuffers) {
        if (b == *buffer.buffer) {
            return true;
        }
    }
    return false;
}

void Camera3Stream::removeOutstandingBuffer(const camera3_stream_buffer &buffer) {
    if (buffer.buffer == nullptr) {
        return;
    }

    for (auto b = mOutstandingBuffers.begin(); b != mOutstandingBuffers.end(); b++) {
        if (*b == *buffer.buffer) {
            mOutstandingBuffers.erase(b);
            return;
        }
    }
}

status_t Camera3Stream::returnBuffer(const camera3_stream_buffer &buffer,
        nsecs_t timestamp) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    // Check if this buffer is outstanding.
    if (!isOutstandingBuffer(buffer)) {
        ALOGE("%s: Stream %d: Returning an unknown buffer.", __FUNCTION__, mId);
        return BAD_VALUE;
    }

    /**
     * TODO: Check that the state is valid first.
     *
     * <HAL3.2 IN_CONFIG and IN_RECONFIG in addition to CONFIGURED.
     * >= HAL3.2 CONFIGURED only
     *
     * Do this for getBuffer as well.
     */
    status_t res = returnBufferLocked(buffer, timestamp);
    if (res == OK) {
        fireBufferListenersLocked(buffer, /*acquired*/false, /*output*/true);
    }

    // Even if returning the buffer failed, we still want to signal whoever is waiting for the
    // buffer to be returned.
    mOutputBufferReturnedSignal.signal();

    removeOutstandingBuffer(buffer);
    return res;
}

status_t Camera3Stream::getInputBuffer(camera3_stream_buffer *buffer) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    status_t res = OK;

    // This function should be only called when the stream is configured already.
    if (mState != STATE_CONFIGURED) {
        ALOGE("%s: Stream %d: Can't get input buffers if stream is not in CONFIGURED state %d",
                __FUNCTION__, mId, mState);
        return INVALID_OPERATION;
    }

    // Wait for new buffer returned back if we are running into the limit.
    if (getHandoutInputBufferCountLocked() == camera3_stream::max_buffers) {
        ALOGV("%s: Already dequeued max input buffers (%d), wait for next returned one.",
                __FUNCTION__, camera3_stream::max_buffers);
        res = mInputBufferReturnedSignal.waitRelative(mLock, kWaitForBufferDuration);
        if (res != OK) {
            if (res == TIMED_OUT) {
                ALOGE("%s: wait for input buffer return timed out after %lldms", __FUNCTION__,
                        kWaitForBufferDuration / 1000000LL);
            }
            return res;
        }
    }

    res = getInputBufferLocked(buffer);
    if (res == OK) {
        fireBufferListenersLocked(*buffer, /*acquired*/true, /*output*/false);
        if (buffer->buffer) {
            mOutstandingBuffers.push_back(*buffer->buffer);
        }
    }

    return res;
}

status_t Camera3Stream::returnInputBuffer(const camera3_stream_buffer &buffer) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    // Check if this buffer is outstanding.
    if (!isOutstandingBuffer(buffer)) {
        ALOGE("%s: Stream %d: Returning an unknown buffer.", __FUNCTION__, mId);
        return BAD_VALUE;
    }

    status_t res = returnInputBufferLocked(buffer);
    if (res == OK) {
        fireBufferListenersLocked(buffer, /*acquired*/false, /*output*/false);
        mInputBufferReturnedSignal.signal();
    }

    removeOutstandingBuffer(buffer);
    return res;
}

status_t Camera3Stream::getInputBufferProducer(sp<IGraphicBufferProducer> *producer) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    return getInputBufferProducerLocked(producer);
}

void Camera3Stream::fireBufferListenersLocked(
        const camera3_stream_buffer& buffer, bool acquired, bool output) {
    List<wp<Camera3StreamBufferListener> >::iterator it, end;

    // TODO: finish implementing

    Camera3StreamBufferListener::BufferInfo info =
        Camera3StreamBufferListener::BufferInfo();
    info.mOutput = output;
    info.mError = (buffer.status == CAMERA3_BUFFER_STATUS_ERROR);
    // TODO: rest of fields

    for (it = mBufferListenerList.begin(), end = mBufferListenerList.end();
         it != end;
         ++it) {

        sp<Camera3StreamBufferListener> listener = it->promote();
        if (listener != 0) {
            if (acquired) {
                listener->onBufferAcquired(info);
            } else {
                listener->onBufferReleased(info);
            }
        }
    }
}

bool Camera3Stream::hasOutstandingBuffers() const {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    return hasOutstandingBuffersLocked();
}

status_t Camera3Stream::setStatusTracker(sp<StatusTracker> statusTracker) {
    Mutex::Autolock l(mLock);
    sp<StatusTracker> oldTracker = mStatusTracker.promote();
    if (oldTracker != 0 && mStatusId != StatusTracker::NO_STATUS_ID) {
        oldTracker->removeComponent(mStatusId);
    }
    mStatusId = StatusTracker::NO_STATUS_ID;
    mStatusTracker = statusTracker;

    return OK;
}

status_t Camera3Stream::disconnect() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    ALOGV("%s: Stream %d: Disconnecting...", __FUNCTION__, mId);
    status_t res = disconnectLocked();

    if (res == -ENOTCONN) {
        // "Already disconnected" -- not an error
        return OK;
    } else {
        return res;
    }
}

status_t Camera3Stream::registerBuffersLocked(camera3_device *hal3Device) {
    ATRACE_CALL();

    /**
     * >= CAMERA_DEVICE_API_VERSION_3_2:
     *
     * camera3_device_t->ops->register_stream_buffers() is not called and must
     * be NULL.
     */
    if (hal3Device->common.version >= CAMERA_DEVICE_API_VERSION_3_2) {
        ALOGV("%s: register_stream_buffers unused as of HAL3.2", __FUNCTION__);

        if (hal3Device->ops->register_stream_buffers != NULL) {
            ALOGE("%s: register_stream_buffers is deprecated in HAL3.2; "
                    "must be set to NULL in camera3_device::ops", __FUNCTION__);
            return INVALID_OPERATION;
        }

        return OK;
    }

    ALOGV("%s: register_stream_buffers using deprecated code path", __FUNCTION__);

    status_t res;

    size_t bufferCount = getBufferCountLocked();

    Vector<buffer_handle_t*> buffers;
    buffers.insertAt(/*prototype_item*/NULL, /*index*/0, bufferCount);

    camera3_stream_buffer_set bufferSet = camera3_stream_buffer_set();
    bufferSet.stream = this;
    bufferSet.num_buffers = bufferCount;
    bufferSet.buffers = buffers.editArray();

    Vector<camera3_stream_buffer_t> streamBuffers;
    streamBuffers.insertAt(camera3_stream_buffer_t(), /*index*/0, bufferCount);

    // Register all buffers with the HAL. This means getting all the buffers
    // from the stream, providing them to the HAL with the
    // register_stream_buffers() method, and then returning them back to the
    // stream in the error state, since they won't have valid data.
    //
    // Only registered buffers can be sent to the HAL.

    uint32_t bufferIdx = 0;
    for (; bufferIdx < bufferCount; bufferIdx++) {
        res = getBufferLocked( &streamBuffers.editItemAt(bufferIdx) );
        if (res != OK) {
            ALOGE("%s: Unable to get buffer %d for registration with HAL",
                    __FUNCTION__, bufferIdx);
            // Skip registering, go straight to cleanup
            break;
        }

        sp<Fence> fence = new Fence(streamBuffers[bufferIdx].acquire_fence);
        fence->waitForever("Camera3Stream::registerBuffers");

        buffers.editItemAt(bufferIdx) = streamBuffers[bufferIdx].buffer;
    }
    if (bufferIdx == bufferCount) {
        // Got all buffers, register with HAL
        ALOGV("%s: Registering %zu buffers with camera HAL",
                __FUNCTION__, bufferCount);
        ATRACE_BEGIN("camera3->register_stream_buffers");
        res = hal3Device->ops->register_stream_buffers(hal3Device,
                &bufferSet);
        ATRACE_END();
    }

    // Return all valid buffers to stream, in ERROR state to indicate
    // they weren't filled.
    for (size_t i = 0; i < bufferIdx; i++) {
        streamBuffers.editItemAt(i).release_fence = -1;
        streamBuffers.editItemAt(i).status = CAMERA3_BUFFER_STATUS_ERROR;
        returnBufferLocked(streamBuffers[i], 0);
    }

    mPrepared = true;

    return res;
}

status_t Camera3Stream::getBufferLocked(camera3_stream_buffer *) {
    ALOGE("%s: This type of stream does not support output", __FUNCTION__);
    return INVALID_OPERATION;
}
status_t Camera3Stream::returnBufferLocked(const camera3_stream_buffer &,
                                           nsecs_t) {
    ALOGE("%s: This type of stream does not support output", __FUNCTION__);
    return INVALID_OPERATION;
}
status_t Camera3Stream::getInputBufferLocked(camera3_stream_buffer *) {
    ALOGE("%s: This type of stream does not support input", __FUNCTION__);
    return INVALID_OPERATION;
}
status_t Camera3Stream::returnInputBufferLocked(
        const camera3_stream_buffer &) {
    ALOGE("%s: This type of stream does not support input", __FUNCTION__);
    return INVALID_OPERATION;
}
status_t Camera3Stream::getInputBufferProducerLocked(sp<IGraphicBufferProducer>*) {
    ALOGE("%s: This type of stream does not support input", __FUNCTION__);
    return INVALID_OPERATION;
}

void Camera3Stream::addBufferListener(
        wp<Camera3StreamBufferListener> listener) {
    Mutex::Autolock l(mLock);

    List<wp<Camera3StreamBufferListener> >::iterator it, end;
    for (it = mBufferListenerList.begin(), end = mBufferListenerList.end();
         it != end;
         ) {
        if (*it == listener) {
            ALOGE("%s: Try to add the same listener twice, ignoring...", __FUNCTION__);
            return;
        }
        it++;
    }

    mBufferListenerList.push_back(listener);
}

void Camera3Stream::removeBufferListener(
        const sp<Camera3StreamBufferListener>& listener) {
    Mutex::Autolock l(mLock);

    bool erased = true;
    List<wp<Camera3StreamBufferListener> >::iterator it, end;
    for (it = mBufferListenerList.begin(), end = mBufferListenerList.end();
         it != end;
         ) {

        if (*it == listener) {
            it = mBufferListenerList.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }

    if (!erased) {
        ALOGW("%s: Could not find listener to remove, already removed",
              __FUNCTION__);
    }
}

}; // namespace camera3

}; // namespace android
