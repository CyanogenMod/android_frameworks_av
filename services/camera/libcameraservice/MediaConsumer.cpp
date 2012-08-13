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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <utils/Log.h>

#include "MediaConsumer.h"

#define MC_LOGV(x, ...) ALOGV("[%s] "x, mName.string(), ##__VA_ARGS__)
#define MC_LOGD(x, ...) ALOGD("[%s] "x, mName.string(), ##__VA_ARGS__)
#define MC_LOGI(x, ...) ALOGI("[%s] "x, mName.string(), ##__VA_ARGS__)
#define MC_LOGW(x, ...) ALOGW("[%s] "x, mName.string(), ##__VA_ARGS__)
#define MC_LOGE(x, ...) ALOGE("[%s] "x, mName.string(), ##__VA_ARGS__)

namespace android {

// Get an ID that's unique within this process.
static int32_t createProcessUniqueId() {
    static volatile int32_t globalCounter = 0;
    return android_atomic_inc(&globalCounter);
}

MediaConsumer::MediaConsumer(uint32_t maxLockedBuffers) :
    mMaxLockedBuffers(maxLockedBuffers),
    mCurrentLockedBuffers(0)
{
    mName = String8::format("mc-unnamed-%d-%d", getpid(),
            createProcessUniqueId());

    mBufferQueue = new BufferQueue(true);

    wp<BufferQueue::ConsumerListener> listener;
    sp<BufferQueue::ConsumerListener> proxy;
    listener = static_cast<BufferQueue::ConsumerListener*>(this);
    proxy = new BufferQueue::ProxyConsumerListener(listener);

    status_t err = mBufferQueue->consumerConnect(proxy);
    if (err != NO_ERROR) {
        ALOGE("MediaConsumer: error connecting to BufferQueue: %s (%d)",
                strerror(-err), err);
    } else {
        mBufferQueue->setSynchronousMode(true);
        mBufferQueue->setConsumerUsageBits(GRALLOC_USAGE_HW_VIDEO_ENCODER);
        mBufferQueue->setConsumerName(mName);
    }
}

MediaConsumer::~MediaConsumer()
{
    Mutex::Autolock _l(mMutex);
    for (int i = 0; i < BufferQueue::NUM_BUFFER_SLOTS; i++) {
        freeBufferLocked(i);
    }
    mBufferQueue->consumerDisconnect();
    mBufferQueue.clear();
}

void MediaConsumer::setName(const String8& name) {
    Mutex::Autolock _l(mMutex);
    mName = name;
    mBufferQueue->setConsumerName(name);
}

status_t MediaConsumer::getNextBuffer(buffer_handle_t *buffer, nsecs_t *timestamp) {
    status_t err;

    if (!buffer) return BAD_VALUE;
    if (mCurrentLockedBuffers == mMaxLockedBuffers) {
        MC_LOGV("Too many buffers (max %d)", mCurrentLockedBuffers);
        return INVALID_OPERATION;
    }

    BufferQueue::BufferItem b;

    Mutex::Autolock _l(mMutex);

    err = mBufferQueue->acquireBuffer(&b);
    if (err != OK) {
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            MC_LOGV("No buffer available");
            return BAD_VALUE;
        } else {
            MC_LOGE("Error acquiring buffer: %s (%d)", strerror(err), err);
            return err;
        }
    }

    int buf = b.mBuf;

    if (b.mGraphicBuffer != NULL) {
        mBufferSlot[buf] = b.mGraphicBuffer;
    }

    if (b.mFence.get()) {
        err = b.mFence->wait(Fence::TIMEOUT_NEVER);
        if (err != OK) {
            MC_LOGE("Failed to wait for fence of acquired buffer: %s (%d)",
                    strerror(-err), err);
            return err;
        }
    }

    *buffer = mBufferSlot[buf]->handle;
    *timestamp = b.mTimestamp;

    mCurrentLockedBuffers++;
    MC_LOGV("getNextBuffer: %d buffers in use", mCurrentLockedBuffers);
    return OK;
}

status_t MediaConsumer::freeBuffer(buffer_handle_t buffer) {
    Mutex::Autolock _l(mMutex);
    int buf = 0;
    status_t err;

    for (; buf < BufferQueue::NUM_BUFFER_SLOTS; buf++) {
        if (buffer == mBufferSlot[buf]->handle) break;
    }
    if (buf == BufferQueue::NUM_BUFFER_SLOTS) {
        MC_LOGE("%s: Can't find buffer to free", __FUNCTION__);
        return BAD_VALUE;
    }

    err = mBufferQueue->releaseBuffer(buf, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR,
            Fence::NO_FENCE);
    if (err == BufferQueue::STALE_BUFFER_SLOT) {
        freeBufferLocked(buf);
    } else if (err != OK) {
        MC_LOGE("%s: Unable to release graphic buffer %d to queue", __FUNCTION__,
                buf);
        return err;
    }
    mCurrentLockedBuffers--;
    MC_LOGV("freeBuffer: %d buffers in use", mCurrentLockedBuffers);

    return OK;
}

void MediaConsumer::setFrameAvailableListener(
        const sp<FrameAvailableListener>& listener) {
    MC_LOGV("setFrameAvailableListener");
    Mutex::Autolock lock(mMutex);
    mFrameAvailableListener = listener;
}


void MediaConsumer::onFrameAvailable() {
    MC_LOGV("onFrameAvailable");
    sp<FrameAvailableListener> listener;
    { // scope for the lock
        Mutex::Autolock _l(mMutex);
        listener = mFrameAvailableListener;
    }

    if (listener != NULL) {
        MC_LOGV("actually calling onFrameAvailable");
        listener->onFrameAvailable();
    }
}

void MediaConsumer::onBuffersReleased() {
    MC_LOGV("onBuffersReleased");

    Mutex::Autolock lock(mMutex);

    uint32_t mask = 0;
    mBufferQueue->getReleasedBuffers(&mask);
    for (int i = 0; i < BufferQueue::NUM_BUFFER_SLOTS; i++) {
        if (mask & (1 << i)) {
            freeBufferLocked(i);
        }
    }

}

status_t MediaConsumer::freeBufferLocked(int buf) {
    status_t err = OK;

    mBufferSlot[buf] = NULL;
    return err;
}

} // namespace android
