/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "Serializer"
#include <utils/Log.h>

#include "Serializer.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>

namespace android {

struct Serializer::Track : public RefBase {
    Track(const sp<MediaSource> &source);

    status_t start();
    status_t stop();

    void readBufferIfNecessary();

    bool reachedEOS() const;
    int64_t bufferTimeUs() const;

    sp<ABuffer> drainBuffer();

protected:
    virtual ~Track();

private:
    sp<MediaSource> mSource;
    AString mMIME;

    bool mStarted;
    status_t mFinalResult;
    MediaBuffer *mBuffer;
    int64_t mBufferTimeUs;

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

Serializer::Track::Track(const sp<MediaSource> &source)
    : mSource(source),
      mStarted(false),
      mFinalResult(OK),
      mBuffer(NULL),
      mBufferTimeUs(-1ll) {
    const char *mime;
    sp<MetaData> meta = mSource->getFormat();
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    mMIME = mime;
}

Serializer::Track::~Track() {
    stop();
}

status_t Serializer::Track::start() {
    if (mStarted) {
        return OK;
    }

    status_t err = mSource->start();

    if (err == OK) {
        mStarted = true;
    }

    return err;
}

status_t Serializer::Track::stop() {
    if (!mStarted) {
        return OK;
    }

    if (mBuffer != NULL) {
        mBuffer->release();
        mBuffer = NULL;

        mBufferTimeUs = -1ll;
    }

    status_t err = mSource->stop();

    mStarted = false;

    return err;
}

void Serializer::Track::readBufferIfNecessary() {
    if (mBuffer != NULL) {
        return;
    }

    int64_t nowUs = ALooper::GetNowUs();
    mFinalResult = mSource->read(&mBuffer);
    int64_t delayUs = ALooper::GetNowUs() - nowUs;

    ALOGV("read on track %s took %lld us, got %d bytes",
          mMIME.c_str(), delayUs, mBuffer->range_length());

    if (mFinalResult != OK) {
        ALOGI("read failed w/ err %d", mFinalResult);
        return;
    }

    CHECK(mBuffer->meta_data()->findInt64(kKeyTime, &mBufferTimeUs));
}

bool Serializer::Track::reachedEOS() const {
    return mFinalResult != OK;
}

int64_t Serializer::Track::bufferTimeUs() const {
    return mBufferTimeUs;
}

sp<ABuffer> Serializer::Track::drainBuffer() {
    sp<ABuffer> accessUnit = new ABuffer(mBuffer->range_length());

    memcpy(accessUnit->data(),
           (const uint8_t *)mBuffer->data() + mBuffer->range_offset(),
           mBuffer->range_length());

    accessUnit->meta()->setInt64("timeUs", mBufferTimeUs);
    accessUnit->meta()->setPointer("mediaBuffer", mBuffer);

    mBuffer = NULL;
    mBufferTimeUs = -1ll;

    return accessUnit;
}

////////////////////////////////////////////////////////////////////////////////

Serializer::Serializer(bool throttle, const sp<AMessage> &notify)
    : mThrottle(throttle),
      mNotify(notify),
      mPollGeneration(0),
      mStartTimeUs(-1ll) {
}

Serializer::~Serializer() {
}

status_t Serializer::postSynchronouslyAndReturnError(
        const sp<AMessage> &msg) {
    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err != OK) {
        return err;
    }

    if (!response->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

ssize_t Serializer::addSource(const sp<MediaSource> &source) {
    sp<AMessage> msg = new AMessage(kWhatAddSource, id());
    msg->setPointer("source", source.get());

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err != OK) {
        return err;
    }

    if (!response->findInt32("err", &err)) {
        size_t index;
        CHECK(response->findSize("index", &index));

        return index;
    }

    return err;
}

status_t Serializer::start() {
    return postSynchronouslyAndReturnError(new AMessage(kWhatStart, id()));
}

status_t Serializer::stop() {
    return postSynchronouslyAndReturnError(new AMessage(kWhatStop, id()));
}

void Serializer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatAddSource:
        {
            ssize_t index = onAddSource(msg);

            sp<AMessage> response = new AMessage;

            if (index < 0) {
                response->setInt32("err", index);
            } else {
                response->setSize("index", index);
            }

            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            response->postReply(replyID);
            break;
        }

        case kWhatStart:
        case kWhatStop:
        {
            status_t err = (msg->what() == kWhatStart) ? onStart() : onStop();

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);

            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            response->postReply(replyID);
            break;
        }

        case kWhatPoll:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mPollGeneration) {
                break;
            }

            int64_t delayUs = onPoll();
            if (delayUs >= 0ll) {
                schedulePoll(delayUs);
            }
            break;
        }

        default:
            TRESPASS();
    }
}

ssize_t Serializer::onAddSource(const sp<AMessage> &msg) {
    void *obj;
    CHECK(msg->findPointer("source", &obj));

    sp<MediaSource> source = static_cast<MediaSource *>(obj);

    sp<Track> track = new Track(source);
    return mTracks.add(track);
}

status_t Serializer::onStart() {
    status_t err = OK;
    for (size_t i = 0; i < mTracks.size(); ++i) {
        err = mTracks.itemAt(i)->start();

        if (err != OK) {
            break;
        }
    }

    if (err == OK) {
        schedulePoll();
    }

    return err;
}

status_t Serializer::onStop() {
    for (size_t i = 0; i < mTracks.size(); ++i) {
        mTracks.itemAt(i)->stop();
    }

    cancelPoll();

    return OK;
}

int64_t Serializer::onPoll() {
    int64_t minTimeUs = -1ll;
    ssize_t minTrackIndex = -1;

    for (size_t i = 0; i < mTracks.size(); ++i) {
        const sp<Track> &track = mTracks.itemAt(i);

        track->readBufferIfNecessary();

        if (!track->reachedEOS()) {
            int64_t timeUs = track->bufferTimeUs();

            if (minTrackIndex < 0 || timeUs < minTimeUs) {
                minTimeUs = timeUs;
                minTrackIndex = i;
            }
        }
    }

    if (minTrackIndex < 0) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatEOS);
        notify->post();

        return -1ll;
    }

    if (mThrottle) {
        int64_t nowUs = ALooper::GetNowUs();

        if (mStartTimeUs < 0ll) {
            mStartTimeUs = nowUs;
        }

        int64_t lateByUs = nowUs - (minTimeUs + mStartTimeUs);

        if (lateByUs < 0ll) {
            // Too early
            return -lateByUs;
        }
    }

    sp<AMessage> notify = mNotify->dup();

    notify->setInt32("what", kWhatAccessUnit);
    notify->setSize("trackIndex", minTrackIndex);

    notify->setBuffer(
            "accessUnit", mTracks.itemAt(minTrackIndex)->drainBuffer());

    notify->post();

    return 0ll;
}

void Serializer::schedulePoll(int64_t delayUs) {
    sp<AMessage> msg = new AMessage(kWhatPoll, id());
    msg->setInt32("generation", mPollGeneration);
    msg->post(delayUs);
}

void Serializer::cancelPoll() {
    ++mPollGeneration;
}

}  // namespace android
