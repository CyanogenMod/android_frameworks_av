/*Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "PrefetchSource"
#include <utils/Log.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>

#include <binder/IPCThreadState.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include <sys/atomics.h>
#include <sys/prctl.h>
#include "include/ExtendedPrefetchSource.h"

#define ATRACE_TAG ATRACE_TAG_ALWAYS
#include <utils/Trace.h>

namespace android {

struct AutoTrace {
    AutoTrace(const char *msg) {
        mMsg[0] = '\0';
        if (msg != NULL) {
            strncpy(mMsg, msg, sizeof(mMsg) - 1);
            mMsg[sizeof(mMsg) - 1] = '\0';
        }
        ATRACE_BEGIN(mMsg);
    }
    ~AutoTrace() {
        ATRACE_END();
    }
    private:
    char mMsg[32];
};

PrefetchSource::PrefetchSource(
        sp<MediaSource> source, uint32_t mode, const char *id)
    : mSource(source),
      mSourceStarted(false),
      mBuffer(0),
      mRemnantOffset(0),
      mAvailBufferQueue(0),
      mFilledBufferQueue(0),
      mMode(mode),
      mState(STATE_STOPPED),
      mSeekTimeUs(-1),
      mSeekMode(ReadOptions::SEEK_CLOSEST_SYNC),
      mReachedEos(false) {
    int32_t bufCount, bufSize;

    if (mMode == MODE_AGGREGATE) {
        bufSize = kDefaultAudioPrefetchBufferSize;
        bufCount = kNumAudioPrefetchBuffers;
    } else {
        bufSize = kDefaultVideoPrefetchBufferSize;
        sp<MetaData> meta = mSource->getFormat();
        meta->findInt32(kKeyMaxInputSize, &bufSize);
        bufCount = kNumVideoPrefetchBuffers;
    }

    mAvailBufferQueue = new SyncQueue(bufCount);
    mFilledBufferQueue = new SyncQueue(bufCount);
    mAvailBufferQueue->setName("allocAvailQ");
    mFilledBufferQueue->setName("allocFilledQ");
    for (int i=0; i < bufCount; ++i) {
        MediaBuffer *buf = new MediaBuffer(bufSize);
        mAvailBufferQueue->add(buf);
    }

    if (id != NULL) {
        strncpy(mId, id, sizeof(mId) - 1);
        mId[sizeof(mId) - 1] = '\0';
    } else {
        strncpy(mId, "PrefetchSource", 15);
    }
    ALOGV("Created %s", mId);
}

PrefetchSource::~PrefetchSource() {
    stop();

    if (mAvailBufferQueue) {
        delete mAvailBufferQueue;
    }

    if (mFilledBufferQueue) {
        delete mFilledBufferQueue;
    }

    ALOGV("Destroyed %s", mId);
}

bool PrefetchSource::isPrefetchEnabled() {
    char enablePrefetch[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.mm.enable.prefetch", enablePrefetch, "0");
    if (!strncmp(enablePrefetch, "true", 4) || atoi(enablePrefetch)) {
        return true;
    }

    return false;
}

status_t PrefetchSource::start(MetaData *params) {
    ALOGD("Starting %s", mId);
    status_t err = mSource->start(params);
    if (err != OK) {
        return err;
    }

    mSourceStarted = true;
    startThread();
    return OK;
}

status_t PrefetchSource::stop() {
    ALOGD("Stopping %s", mId);
    stopThread();
    flushFilledBuffers();
    if (mSourceStarted) {
        mSource->stop();
        mSourceStarted = false;
    }
    ALOGD("%s has stopped", mId);
    return OK;
}

sp<MetaData> PrefetchSource::getFormat() {
    return mSource->getFormat();
}

status_t PrefetchSource::read(MediaBuffer **buffer,
        const ReadOptions *options) {
    ATRACE_CALL();
    ALOGV("%s read", mId);

    *buffer = NULL;

    // Start prefetching thread if it is not already running
    if (mState != STATE_RUNNING && !mReachedEos) {
        startThread();
    }

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        ALOGI("Seek requested in %s", mId);
        stopThread();
        flushFilledBuffers();
        mSeekMode = mode;
        mSeekTimeUs = seekTimeUs;
        startThread();
    }

    ALOGV("%s filled queue size : %d", mId, mFilledBufferQueue->count());
    status_t err = mFilledBufferQueue->get(buffer);
    if (err == OK) {
        ALOGV("Found filled buffer %p", *buffer);
        (*buffer)->add_ref();
    } else {
        if (mReachedEos) {
            ALOGI("%s reached EOS", mId);
            return ERROR_END_OF_STREAM;
        }
        ALOGE("Error %d getting filled buffer in %s", err, mId);
    }

    return err;
}

status_t PrefetchSource::setBuffers(const Vector<MediaBuffer *> &buffers) {
    stop();
    if (mAvailBufferQueue) {
        delete mAvailBufferQueue;
        mAvailBufferQueue = new SyncQueue(buffers.size());
        mAvailBufferQueue->setName("useAvailQ");
    }

    if (mFilledBufferQueue) {
        delete mFilledBufferQueue;
        mFilledBufferQueue = new SyncQueue(buffers.size());
        mFilledBufferQueue->setName("useFilledQ");
    }

    for (size_t i = 0; i < buffers.size(); ++i) {
        mAvailBufferQueue->add(buffers.itemAt(i));
    }

    start();
    ALOGI("Using codec-supplied buffers");
    return OK;
}

status_t PrefetchSource::readInternal(MediaBuffer **buffer) {
    ATRACE_CALL();
    ALOGV("%s readInternal", mId);
    if (mReachedEos) {
        *buffer = NULL;
        ALOGD("%s handling deferred EOS", mId);
        // Main thread may be blocked waiting for data
        mFilledBufferQueue->setBlocking(false);
        mFilledBufferQueue->wake();
        return ERROR_END_OF_STREAM;
    }

    status_t err;
    MediaBuffer *dstBuffer = NULL;
    bool isFirstRead = true;
    size_t filledLen = 0;

    ALOGV("%s avail queue size : %d", mId, mAvailBufferQueue->count());
    err = mAvailBufferQueue->get(&dstBuffer);
    // Woken by flush
    if (mState == STATE_STOPPING) {
        // If a buffer was returned, requeue it as available
        if (dstBuffer != NULL) {
            mAvailBufferQueue->put(dstBuffer);
        }
        return ERROR_END_OF_STREAM;
    }

    CHECK((err == OK) && (dstBuffer != NULL));

    size_t capacity = dstBuffer->size();
    dstBuffer->set_range(0, 0);
    dstBuffer->meta_data()->clear();

    ALOGV("%s got avail buffer %p", mId, dstBuffer);
    for(;mState != STATE_STOPPING;) {
        AutoTrace aTrace("source-read");
        err = OK;
        if (mBuffer == NULL) {
            MediaSource::ReadOptions options;
            options.setSeekTo(mSeekTimeUs, mSeekMode);
            if (mSeekTimeUs >= 0) {
                ALOGI("Seeking source to %lld", mSeekTimeUs);
            }

            err = mSource->read(&mBuffer, mSeekTimeUs >= 0 ? &options : NULL);
            mSeekTimeUs = -1;
            mSeekMode = ReadOptions::SEEK_CLOSEST_SYNC;
            if (mState == STATE_STOPPING) {
                ALOGD("%s readInternal: stop requested", mId);
                break;
            } else {
                usleep(0); // yield
            }
        }

        if (err == OK) {
            CHECK(mBuffer);
            if (mBuffer->range_length() == 0) {
                mBuffer->release();
                mBuffer = NULL;
                continue;
            }

            if (isFirstRead) {
                copyMetaData(mBuffer, dstBuffer);
                isFirstRead = false;
            }

            // Offset into the source buffer at which to start reading
            size_t srcOffset = mBuffer->range_offset() + mRemnantOffset;
            // Bytes that still need to be copied from the source buffer
            size_t bytesToCopy = mBuffer->range_length() - mRemnantOffset;
            if ((filledLen + bytesToCopy) <= capacity) {
                memcpy((uint8_t*)dstBuffer->data() + filledLen,
                        (uint8_t*)mBuffer->data() + srcOffset,
                        bytesToCopy);
                filledLen += bytesToCopy;
                mRemnantOffset = 0;
                mBuffer->release();
                mBuffer = NULL;
                if (mMode == MODE_FRAME_BY_FRAME || filledLen == capacity) {
                    break;
                }
            } else { // The current buffer will be filled by this call
                CHECK(mMode != MODE_FRAME_BY_FRAME);
                // Free space in the destination buffer
                size_t copySize = capacity - filledLen;
                memcpy((uint8_t*)dstBuffer->data() + filledLen,
                        (uint8_t*)mBuffer->data() + srcOffset,
                        copySize);
                filledLen += copySize;
                mRemnantOffset += copySize;
                *buffer = dstBuffer;
                break;
            }
        } else {
            // No more input; reached EOS
            ALOGI("%s saw source EOS (status %d)", mId, err);
            mReachedEos = true;
            break;
        }
    }

    if (mState == STATE_STOPPING ||
            ((dstBuffer == NULL || filledLen == 0) && mReachedEos)) {
        if (dstBuffer) {
            *buffer = NULL;
            ALOGD("Releasing buf %p as %s is stopping", dstBuffer, mId);
            mAvailBufferQueue->put(dstBuffer);
        }

        if (mReachedEos) {
            // Main thread may be blocked waiting for data
            mFilledBufferQueue->setBlocking(false);
            ALOGV("%s waking blocked filled-buf readers on EOS", mId);
            mFilledBufferQueue->wake();
        }
        return ERROR_END_OF_STREAM;
    }

    if (filledLen && dstBuffer) {
        dstBuffer->set_range(0, filledLen);
        *buffer = dstBuffer;
        err = OK;
    } else {
        *buffer = NULL;
    }

    return err;
}

void PrefetchSource::flushFilledBuffers() {
    ATRACE_CALL();
    ALOGD("Flush called");
    MediaBuffer *buffer;
    status_t err;
    while(!mFilledBufferQueue->empty()) {
        buffer = NULL;
        err = mFilledBufferQueue->get(&buffer);
        if (buffer) {
            ALOGV("Freeing filled buffer %p", buffer);
            mAvailBufferQueue->put(buffer);
        } else {
            ALOGW("Failed to flush filled buffer (%d)", err);
        }
    }

    mRemnantOffset = 0;
    if (mBuffer) {
        mBuffer->release();
        mBuffer = NULL;
    }

    ALOGD("After flush, avail buffer queue size is %d",
            mAvailBufferQueue->count());

    /* These assertions do not always hold in useBuffer mode as OMXCodec's
     * flush/free behavior does not fully honor buffer ownership. Otherwise,
     * they should always hold. */
    //CHECK(mFilledBufferQueue->count() == 0);
    //CHECK(mAvailBufferQueue->full());
}

void PrefetchSource::copyMetaData(MediaBuffer *srcBuffer,
        MediaBuffer *dstBuffer) {
    // NOTE: Copy any other useful keys here
    int64_t time = 0;
    if (mBuffer->meta_data()->findInt64(kKeyTime, &time) != 0) {
        ALOGV("%s TS = %lld", mId, time);
        dstBuffer->meta_data()->setInt64(kKeyTime, time);
    }
}

void *PrefetchSource::ThreadWrapper(void *me) {
    pid_t tid = androidGetTid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_BACKGROUND);
    ALOGD("Prefetch thread tid=%d, prio=%d", (int)tid,
            androidGetThreadPriority(tid));
    PrefetchSource *source = static_cast<PrefetchSource*>(me);
    source->prefetchThread();
    return NULL;
}

void PrefetchSource::prefetchThread() {
    int result;
    prctl(PR_SET_NAME, (unsigned long)mId, 0, 0, 0);
    result = __atomic_cmpxchg((int)STATE_STARTING,(int)STATE_RUNNING, &mState);
    if (result) {
        ALOGD("Main thread signaled stop before prefetch started");
    }

    while(mState != STATE_STOPPING) {
        MediaBuffer *buf = NULL;
        status_t err = readInternal(&buf);
        if (err == OK) {
            CHECK(buf);
            mFilledBufferQueue->put(buf);
        } else {
            if(buf) {
                mAvailBufferQueue->put(buf);
            }
            ALOGD("Prefetch thread stopping with status %d", err);
            break;
        }
    }

    result = __atomic_swap((int)STATE_STOPPED, &mState);
    ALOGI("Prefetch thread stopped from state %d", result);
}

void PrefetchSource::startThread() {
    ATRACE_CALL();
    if (mState == STATE_STOPPED) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        mState = (int)STATE_STARTING;
        mFilledBufferQueue->setBlocking(true);
        mReachedEos = false;
        ALOGV("Creating prefetch thread");
        pthread_create(&mThread, &attr, ThreadWrapper, this);
        pthread_attr_destroy(&attr);
    }
}

void PrefetchSource::stopThread() {
    ATRACE_CALL();
    ALOGV("Stopping prefetch thread");
    if (mState == STATE_STOPPED) {
        return;
    }

    /* Need to raise priority so this finishes with minimal latency,
     * particularly when seeking. Otherwise, the main thread may be
     * blocked waiting on this to finish. */
    androidSetThreadPriority(androidGetTid(), ANDROID_PRIORITY_NORMAL);
    int result =
            __atomic_cmpxchg((int)STATE_STARTING, (int)STATE_STOPPING,&mState);
    if (result) {
        result =
            __atomic_cmpxchg((int)STATE_RUNNING, (int)STATE_STOPPING, &mState);
    }

    if (result) {
        ALOGV("Signaled stop but prefetch thread was already stopped");
    }

    mAvailBufferQueue->setBlocking(false);
    ALOGV("Waking blocked avail-buf readers");
    mAvailBufferQueue->wake();

    // Block until thread is stopped
    pthread_join(mThread, NULL);
    mAvailBufferQueue->setBlocking(true);
}

PrefetchSource::SyncQueue::SyncQueue(int size)
    : mSize(size),
      mCount(0),
      mReadIndex(0),
      mWriteIndex(0),
      mBlocking(true) {
    if (size > 0) {
        mList = new MediaBuffer*[size];
    } else {
        mList = NULL;
    }

    setName("SyncQueue");
}

PrefetchSource::SyncQueue::~SyncQueue() {
    MediaBuffer *buf;
    for (; mCount > 0; mCount--) {
        buf = mList[mReadIndex];
        mReadIndex = (mReadIndex + 1) % mSize;
        if (buf != NULL) {
            buf->setObserver(NULL);
            buf->release();
        }
    }
}

status_t PrefetchSource::SyncQueue::add(MediaBuffer *buf) {
    if (buf == NULL) {
        return INVALID_OPERATION;
    }

    ALOGV("%s registered buf: %p len=%zu", mName, buf, buf->size());
    buf->setObserver(this);
    return put(buf);
}

status_t PrefetchSource::SyncQueue::put(MediaBuffer *buf) {
    ATRACE_CALL();
    ALOGV("%s put() (mCount = %d)", mName, mCount);
    if (mList == NULL || mCount >= mSize) {
        return INVALID_OPERATION;
    }

    mList[mWriteIndex] = buf;
    mWriteIndex = (mWriteIndex + 1) % mSize;

    int prevCount = __atomic_inc(&mCount);
    if (prevCount == -1) { // There appears to be a buffer free now
        Mutex::Autolock autoLock(mLock);
        if (mCount >= 0) {
            mCondition.broadcast();
            ALOGV("%s woke any blocked readers", mName);
        }
    }

    return OK;
}

status_t PrefetchSource::SyncQueue::get(MediaBuffer **buf) {
    ATRACE_CALL();
    ALOGV("%s get() (mCount = %d)", mName, mCount);
    if (mList == NULL || buf == NULL) {
        return INVALID_OPERATION;
    }

    int prevCount = __atomic_dec(&mCount);
    if (prevCount == 0) { // There appear to be no buffers available
        AutoTrace aTrace("SyncQueue wait");
        Mutex::Autolock autoLock(mLock);
        // If a buffer just became available, there is no need to block
        ALOGV("%s has no buffers and is %s", mName,
                mBlocking ? "blocking" : "not blocking");
        while (mBlocking && mCount < 0) {
            // mLock ensures that no wakeup can arrive until after calling wait
            status_t err = mCondition.waitRelative(mLock,TIMEOUT_NS);
            if (err == OK) {
                ALOGV("Buffer available - %s woke up", mName);
                break;
            } else if (err == TIMED_OUT) {
                ALOGD("%s wait timeout (mCount = %d)", mName, mCount);
            } else {
                ALOGE("Error %d in %s get()", err, mName);
            }
        }
    }

    /* This condition alone is not sufficient to detect wakeups from flush.
     * It is possible that mCount is actually >= 0 if a wakeup was sent but
     * flush arrived before the thread actually woke up. */
    if (mCount < 0) {
        // Release the buffer reference as a buffer will not be returned
        __atomic_inc(&mCount);
        return NOT_ENOUGH_DATA;
    }

    *buf = mList[mReadIndex];
    mReadIndex = (mReadIndex + 1) % mSize;

    return OK;
}

void PrefetchSource::SyncQueue::wake() {
    ATRACE_CALL();
    Mutex::Autolock autoLock(mLock);
    mCondition.broadcast();
    ALOGV("%s woke any blocked readers", mName);
}

void PrefetchSource::SyncQueue::setBlocking(bool blocking) {
    Mutex::Autolock autoLock(mLock);
    mBlocking = blocking;
}

void PrefetchSource::SyncQueue::setName(const char *name) {
    if (name == NULL) {
        return;
    }

    strncpy(mName, name, sizeof(mName) - 1);
    mName[sizeof(mName) - 1] = '\0';
}
}
