/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "AnotherPacketSource"

#include "AnotherPacketSource.h"

#include "include/avc_utils.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/Vector.h>

#include <inttypes.h>

namespace android {

const int64_t kNearEOSMarkUs = 2000000ll; // 2 secs

AnotherPacketSource::AnotherPacketSource(const sp<MetaData> &meta)
    : mIsAudio(false),
      mIsVideo(false),
      mEnabled(true),
      mFormat(NULL),
      mLastQueuedTimeUs(0),
      mEOSResult(OK),
      mLatestEnqueuedMeta(NULL),
      mLatestDequeuedMeta(NULL),
      mQueuedDiscontinuityCount(0) {
    setFormat(meta);
}

void AnotherPacketSource::setFormat(const sp<MetaData> &meta) {
    if (mFormat != NULL) {
        // Only allowed to be set once. Requires explicit clear to reset.
        return;
    }

    mIsAudio = false;
    mIsVideo = false;

    if (meta == NULL) {
        return;
    }

    mFormat = meta;
    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if (!strncasecmp("audio/", mime, 6)) {
        mIsAudio = true;
    } else  if (!strncasecmp("video/", mime, 6)) {
        mIsVideo = true;
    } else {
        CHECK(!strncasecmp("text/", mime, 5));
    }
}

AnotherPacketSource::~AnotherPacketSource() {
}

status_t AnotherPacketSource::start(MetaData * /* params */) {
    return OK;
}

status_t AnotherPacketSource::stop() {
    return OK;
}

sp<MetaData> AnotherPacketSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    if (mFormat != NULL) {
        return mFormat;
    }

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    while (it != mBuffers.end()) {
        sp<ABuffer> buffer = *it;
        int32_t discontinuity;
        if (!buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            sp<RefBase> object;
            if (buffer->meta()->findObject("format", &object)) {
                setFormat(static_cast<MetaData*>(object.get()));
                return mFormat;
            }
        }

        ++it;
    }
    return NULL;
}

status_t AnotherPacketSource::dequeueAccessUnit(sp<ABuffer> *buffer) {
    buffer->clear();

    Mutex::Autolock autoLock(mLock);
    while (mEOSResult == OK && mBuffers.empty()) {
        mCondition.wait(mLock);
    }

    if (!mBuffers.empty()) {
        *buffer = *mBuffers.begin();
        mBuffers.erase(mBuffers.begin());

        int32_t discontinuity;
        if ((*buffer)->meta()->findInt32("discontinuity", &discontinuity)) {
            if (wasFormatChange(discontinuity)) {
                mFormat.clear();
            }

            --mQueuedDiscontinuityCount;
            return INFO_DISCONTINUITY;
        }

        mLatestDequeuedMeta = (*buffer)->meta()->dup();

        sp<RefBase> object;
        if ((*buffer)->meta()->findObject("format", &object)) {
            setFormat(static_cast<MetaData*>(object.get()));
        }

        return OK;
    }

    return mEOSResult;
}

status_t AnotherPacketSource::read(
        MediaBuffer **out, const ReadOptions *) {
    *out = NULL;

    Mutex::Autolock autoLock(mLock);
    while (mEOSResult == OK && mBuffers.empty()) {
        mCondition.wait(mLock);
    }

    if (!mBuffers.empty()) {

        const sp<ABuffer> buffer = *mBuffers.begin();
        mBuffers.erase(mBuffers.begin());

        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            if (wasFormatChange(discontinuity)) {
                mFormat.clear();
            }

            return INFO_DISCONTINUITY;
        }

        mLatestDequeuedMeta = buffer->meta()->dup();

        sp<RefBase> object;
        if (buffer->meta()->findObject("format", &object)) {
            setFormat(static_cast<MetaData*>(object.get()));
        }

        int64_t timeUs;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        MediaBuffer *mediaBuffer = new MediaBuffer(buffer);

        mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);

        *out = mediaBuffer;
        return OK;
    }

    return mEOSResult;
}

bool AnotherPacketSource::wasFormatChange(
        int32_t discontinuityType) const {
    if (mIsAudio) {
        return (discontinuityType & ATSParser::DISCONTINUITY_AUDIO_FORMAT) != 0;
    }

    if (mIsVideo) {
        return (discontinuityType & ATSParser::DISCONTINUITY_VIDEO_FORMAT) != 0;
    }

    return false;
}

void AnotherPacketSource::queueAccessUnit(const sp<ABuffer> &buffer) {
    int32_t damaged;
    if (buffer->meta()->findInt32("damaged", &damaged) && damaged) {
        // LOG(VERBOSE) << "discarding damaged AU";
        return;
    }

    Mutex::Autolock autoLock(mLock);
    mBuffers.push_back(buffer);
    mCondition.signal();

    int32_t discontinuity;
    if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
        // discontinuity handling needs to be consistent with queueDiscontinuity()
        ++mQueuedDiscontinuityCount;
        mLastQueuedTimeUs = 0ll;
        mEOSResult = OK;
        mLatestEnqueuedMeta = NULL;
        return;
    }

    int64_t lastQueuedTimeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &lastQueuedTimeUs));
    mLastQueuedTimeUs = lastQueuedTimeUs;
    ALOGV("queueAccessUnit timeUs=%" PRIi64 " us (%.2f secs)",
            mLastQueuedTimeUs, mLastQueuedTimeUs / 1E6);

    if (mLatestEnqueuedMeta == NULL) {
        mLatestEnqueuedMeta = buffer->meta()->dup();
    } else {
        int64_t latestTimeUs = 0;
        int64_t frameDeltaUs = 0;
        CHECK(mLatestEnqueuedMeta->findInt64("timeUs", &latestTimeUs));
        if (lastQueuedTimeUs > latestTimeUs) {
            mLatestEnqueuedMeta = buffer->meta()->dup();
            frameDeltaUs = lastQueuedTimeUs - latestTimeUs;
            mLatestEnqueuedMeta->setInt64("durationUs", frameDeltaUs);
        } else if (!mLatestEnqueuedMeta->findInt64("durationUs", &frameDeltaUs)) {
            // For B frames
            frameDeltaUs = latestTimeUs - lastQueuedTimeUs;
            mLatestEnqueuedMeta->setInt64("durationUs", frameDeltaUs);
        }
    }
}

void AnotherPacketSource::clear() {
    Mutex::Autolock autoLock(mLock);

    mBuffers.clear();
    mEOSResult = OK;
    mQueuedDiscontinuityCount = 0;

    mFormat = NULL;
    mLatestEnqueuedMeta = NULL;
}

void AnotherPacketSource::queueDiscontinuity(
        ATSParser::DiscontinuityType type,
        const sp<AMessage> &extra,
        bool discard) {
    Mutex::Autolock autoLock(mLock);

    if (discard) {
        // Leave only discontinuities in the queue.
        List<sp<ABuffer> >::iterator it = mBuffers.begin();
        while (it != mBuffers.end()) {
            sp<ABuffer> oldBuffer = *it;

            int32_t oldDiscontinuityType;
            if (!oldBuffer->meta()->findInt32(
                        "discontinuity", &oldDiscontinuityType)) {
                it = mBuffers.erase(it);
                continue;
            }

            ++it;
        }
    }

    mEOSResult = OK;
    mLastQueuedTimeUs = 0;
    mLatestEnqueuedMeta = NULL;

    if (type == ATSParser::DISCONTINUITY_NONE) {
        return;
    }

    ++mQueuedDiscontinuityCount;
    sp<ABuffer> buffer = new ABuffer(0);
    buffer->meta()->setInt32("discontinuity", static_cast<int32_t>(type));
    buffer->meta()->setMessage("extra", extra);

    mBuffers.push_back(buffer);
    mCondition.signal();
}

void AnotherPacketSource::signalEOS(status_t result) {
    CHECK(result != OK);

    Mutex::Autolock autoLock(mLock);
    mEOSResult = result;
    mCondition.signal();
}

bool AnotherPacketSource::hasBufferAvailable(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);
    *finalResult = OK;
    if (!mEnabled) {
        return false;
    }
    if (!mBuffers.empty()) {
        return true;
    }

    *finalResult = mEOSResult;
    return false;
}

bool AnotherPacketSource::hasDataBufferAvailable(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);
    *finalResult = OK;
    if (!mEnabled) {
        return false;
    }
    List<sp<ABuffer> >::iterator it;
    for (it = mBuffers.begin(); it != mBuffers.end(); it++) {
        int32_t discontinuity;
        if (!(*it)->meta()->findInt32("discontinuity", &discontinuity)) {
            return true;
        }
    }

    *finalResult = mEOSResult;
    return false;
}

int64_t AnotherPacketSource::getBufferedDurationUs(status_t *finalResult) {
    Mutex::Autolock autoLock(mLock);
    return getBufferedDurationUs_l(finalResult);
}

int64_t AnotherPacketSource::getBufferedDurationUs_l(status_t *finalResult) {
    *finalResult = mEOSResult;

    if (mBuffers.empty()) {
        return 0;
    }

    int64_t time1 = -1;
    int64_t time2 = -1;
    int64_t durationUs = 0;

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    while (it != mBuffers.end()) {
        const sp<ABuffer> &buffer = *it;

        int64_t timeUs;
        if (buffer->meta()->findInt64("timeUs", &timeUs)) {
            if (time1 < 0 || timeUs < time1) {
                time1 = timeUs;
            }

            if (time2 < 0 || timeUs > time2) {
                time2 = timeUs;
            }
        } else {
            // This is a discontinuity, reset everything.
            durationUs += time2 - time1;
            time1 = time2 = -1;
        }

        ++it;
    }

    return durationUs + (time2 - time1);
}

// A cheaper but less precise version of getBufferedDurationUs that we would like to use in
// LiveSession::dequeueAccessUnit to trigger downwards adaptation.
int64_t AnotherPacketSource::getEstimatedDurationUs() {
    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return 0;
    }

    if (mQueuedDiscontinuityCount > 0) {
        status_t finalResult;
        return getBufferedDurationUs_l(&finalResult);
    }

    List<sp<ABuffer> >::iterator it = mBuffers.begin();
    sp<ABuffer> buffer = *it;

    int64_t startTimeUs;
    buffer->meta()->findInt64("timeUs", &startTimeUs);
    if (startTimeUs < 0) {
        return 0;
    }

    it = mBuffers.end();
    --it;
    buffer = *it;

    int64_t endTimeUs;
    buffer->meta()->findInt64("timeUs", &endTimeUs);
    if (endTimeUs < 0) {
        return 0;
    }

    int64_t diffUs;
    if (endTimeUs > startTimeUs) {
        diffUs = endTimeUs - startTimeUs;
    } else {
        diffUs = startTimeUs - endTimeUs;
    }
    return diffUs;
}

status_t AnotherPacketSource::nextBufferTime(int64_t *timeUs) {
    *timeUs = 0;

    Mutex::Autolock autoLock(mLock);

    if (mBuffers.empty()) {
        return mEOSResult != OK ? mEOSResult : -EWOULDBLOCK;
    }

    sp<ABuffer> buffer = *mBuffers.begin();
    CHECK(buffer->meta()->findInt64("timeUs", timeUs));

    return OK;
}

bool AnotherPacketSource::isFinished(int64_t duration) const {
    if (duration > 0) {
        int64_t diff = duration - mLastQueuedTimeUs;
        if (diff < kNearEOSMarkUs && diff > -kNearEOSMarkUs) {
            ALOGV("Detecting EOS due to near end");
            return true;
        }
    }
    return (mEOSResult != OK);
}

sp<AMessage> AnotherPacketSource::getLatestEnqueuedMeta() {
    Mutex::Autolock autoLock(mLock);
    return mLatestEnqueuedMeta;
}

sp<AMessage> AnotherPacketSource::getLatestDequeuedMeta() {
    Mutex::Autolock autoLock(mLock);
    return mLatestDequeuedMeta;
}

void AnotherPacketSource::enable(bool enable) {
    Mutex::Autolock autoLock(mLock);
    mEnabled = enable;
}

/*
 * returns the sample meta that's delayUs after queue head
 * (NULL if such sample is unavailable)
 */
sp<AMessage> AnotherPacketSource::getMetaAfterLastDequeued(int64_t delayUs) {
    Mutex::Autolock autoLock(mLock);
    int64_t firstUs = -1;
    int64_t lastUs = -1;
    int64_t durationUs = 0;

    List<sp<ABuffer> >::iterator it;
    for (it = mBuffers.begin(); it != mBuffers.end(); ++it) {
        const sp<ABuffer> &buffer = *it;
        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            durationUs += lastUs - firstUs;
            firstUs = -1;
            lastUs = -1;
            continue;
        }
        int64_t timeUs;
        if (buffer->meta()->findInt64("timeUs", &timeUs)) {
            if (firstUs < 0) {
                firstUs = timeUs;
            }
            if (lastUs < 0 || timeUs > lastUs) {
                lastUs = timeUs;
            }
            if (durationUs + (lastUs - firstUs) >= delayUs) {
                return buffer->meta();
            }
        }
    }
    return NULL;
}

/*
 * removes samples with time equal or after meta
 */
void AnotherPacketSource::trimBuffersAfterMeta(
        const sp<AMessage> &meta) {
    if (meta == NULL) {
        ALOGW("trimming with NULL meta, ignoring");
        return;
    }

    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return;
    }

    HLSTime stopTime(meta);
    ALOGV("trimBuffersAfterMeta: discontinuitySeq %d, timeUs %lld",
            stopTime.mSeq, (long long)stopTime.mTimeUs);

    List<sp<ABuffer> >::iterator it;
    sp<AMessage> newLatestEnqueuedMeta = NULL;
    int64_t newLastQueuedTimeUs = 0;
    size_t newDiscontinuityCount = 0;
    for (it = mBuffers.begin(); it != mBuffers.end(); ++it) {
        const sp<ABuffer> &buffer = *it;
        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            newDiscontinuityCount++;
            continue;
        }

        HLSTime curTime(buffer->meta());
        if (!(curTime < stopTime)) {
            ALOGV("trimming from %lld (inclusive) to end",
                    (long long)curTime.mTimeUs);
            break;
        }
        newLatestEnqueuedMeta = buffer->meta();
        newLastQueuedTimeUs = curTime.mTimeUs;
    }
    mBuffers.erase(it, mBuffers.end());
    mLatestEnqueuedMeta = newLatestEnqueuedMeta;
    mLastQueuedTimeUs = newLastQueuedTimeUs;
    mQueuedDiscontinuityCount = newDiscontinuityCount;
}

/*
 * removes samples with time equal or before meta;
 * returns first sample left in the queue.
 *
 * (for AVC, if trim happens, the samples left will always start
 * at next IDR.)
 */
sp<AMessage> AnotherPacketSource::trimBuffersBeforeMeta(
        const sp<AMessage> &meta) {
    HLSTime startTime(meta);
    ALOGV("trimBuffersBeforeMeta: discontinuitySeq %d, timeUs %lld",
            startTime.mSeq, (long long)startTime.mTimeUs);

    sp<AMessage> firstMeta;
    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return NULL;
    }

    sp<MetaData> format;
    bool isAvc = false;

    List<sp<ABuffer> >::iterator it;
    size_t discontinuityCount = 0;
    for (it = mBuffers.begin(); it != mBuffers.end(); ++it) {
        const sp<ABuffer> &buffer = *it;
        int32_t discontinuity;
        if (buffer->meta()->findInt32("discontinuity", &discontinuity)) {
            format = NULL;
            isAvc = false;
            discontinuityCount++;
            continue;
        }
        if (format == NULL) {
            sp<RefBase> object;
            if (buffer->meta()->findObject("format", &object)) {
                const char* mime;
                format = static_cast<MetaData*>(object.get());
                isAvc = format != NULL
                        && format->findCString(kKeyMIMEType, &mime)
                        && !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC);
            }
        }
        if (isAvc && !IsIDR(buffer)) {
            continue;
        }

        HLSTime curTime(buffer->meta());
        if (startTime < curTime) {
            ALOGV("trimming from beginning to %lld (not inclusive)",
                    (long long)curTime.mTimeUs);
            firstMeta = buffer->meta();
            break;
        }
    }
    mBuffers.erase(mBuffers.begin(), it);
    mQueuedDiscontinuityCount -= discontinuityCount;
    mLatestDequeuedMeta = NULL;
    return firstMeta;
}

}  // namespace android
