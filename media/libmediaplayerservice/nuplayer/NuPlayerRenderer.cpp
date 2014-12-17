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
#define LOG_TAG "NuPlayerRenderer"
#include <utils/Log.h>

#include "NuPlayerRenderer.h"

#include <cutils/properties.h>

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include <VideoFrameScheduler.h>

#include <inttypes.h>
#include <ExtendedUtils.h>

namespace android {

// Maximum time in paused state when offloading audio decompression. When elapsed, the AudioSink
// is closed to allow the audio DSP to power down.
static const int64_t kOffloadPauseMaxUs = 60000000ll;

// static
const int64_t NuPlayer::Renderer::kMinPositionUpdateDelayUs = 100000ll;

static bool sFrameAccurateAVsync = false;

static void readProperties() {
    char value[PROPERTY_VALUE_MAX];
    if (property_get("persist.sys.media.avsync", value, NULL)) {
        sFrameAccurateAVsync =
            !strcmp("1", value) || !strcasecmp("true", value);
    }
}

NuPlayer::Renderer::Renderer(
        const sp<MediaPlayerBase::AudioSink> &sink,
        const sp<AMessage> &notify,
        uint32_t flags)
    : mAudioSink(sink),
      mNotify(notify),
      mFlags(flags),
      mNumFramesWritten(0),
      mDrainAudioQueuePending(false),
      mDrainVideoQueuePending(false),
      mAudioQueueGeneration(0),
      mVideoQueueGeneration(0),
      mAudioFirstAnchorTimeMediaUs(-1),
      mAnchorTimeMediaUs(-1),
      mAnchorTimeRealUs(-1),
      mAnchorNumFramesWritten(-1),
      mAnchorMaxMediaUs(-1),
      mVideoLateByUs(0ll),
      mHasAudio(false),
      mHasVideo(false),
      mPauseStartedTimeRealUs(-1),
      mFlushingAudio(false),
      mFlushingVideo(false),
      mSyncQueues(false),
      mPaused(false),
      mVideoSampleReceived(false),
      mVideoRenderingStarted(false),
      mVideoRenderingStartGeneration(0),
      mAudioRenderingStartGeneration(0),
      mAudioOffloadPauseTimeoutGeneration(0),
      mAudioOffloadTornDown(false),
      mCurrentOffloadInfo(AUDIO_INFO_INITIALIZER),
      mTotalBuffersQueued(0),
      mLastAudioBufferDrained(0) {

    readProperties();
    notify->findObject(MEDIA_EXTENDED_STATS, (sp<RefBase>*)&mPlayerExtendedStats);
}

NuPlayer::Renderer::~Renderer() {
    if (offloadingAudio()) {
        mAudioSink->stop();
        mAudioSink->flush();
        mAudioSink->close();
    }
}

void NuPlayer::Renderer::queueBuffer(
        bool audio,
        const sp<ABuffer> &buffer,
        const sp<AMessage> &notifyConsumed) {
    sp<AMessage> msg = new AMessage(kWhatQueueBuffer, id());
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->setBuffer("buffer", buffer);
    msg->setMessage("notifyConsumed", notifyConsumed);
    msg->post();
}

void NuPlayer::Renderer::queueEOS(bool audio, status_t finalResult) {
    CHECK_NE(finalResult, (status_t)OK);

    sp<AMessage> msg = new AMessage(kWhatQueueEOS, id());
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->setInt32("finalResult", finalResult);
    msg->post();
}

void NuPlayer::Renderer::flush(bool audio) {
    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            if (mFlushingAudio) {
                return;
            }
            mFlushingAudio = true;
        } else {
            if (mFlushingVideo) {
                return;
            }
            mFlushingVideo = true;
        }
    }

    sp<AMessage> msg = new AMessage(kWhatFlush, id());
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->post();
}

void NuPlayer::Renderer::signalTimeDiscontinuity() {
    Mutex::Autolock autoLock(mLock);
    // CHECK(mAudioQueue.empty());
    // CHECK(mVideoQueue.empty());
    setAudioFirstAnchorTime(-1);
    setAnchorTime(-1, -1);
    setVideoLateByUs(0);
    mSyncQueues = false;
}

void NuPlayer::Renderer::signalAudioSinkChanged() {
    (new AMessage(kWhatAudioSinkChanged, id()))->post();
}

void NuPlayer::Renderer::signalDisableOffloadAudio() {
    (new AMessage(kWhatDisableOffloadAudio, id()))->post();
}

void NuPlayer::Renderer::pause() {
    (new AMessage(kWhatPause, id()))->post();
}

void NuPlayer::Renderer::resume() {
    (new AMessage(kWhatResume, id()))->post();
}

void NuPlayer::Renderer::setVideoFrameRate(float fps) {
    sp<AMessage> msg = new AMessage(kWhatSetVideoFrameRate, id());
    msg->setFloat("frame-rate", fps);
    msg->post();
}

status_t NuPlayer::Renderer::getCurrentPosition(int64_t *mediaUs) {
    return getCurrentPosition(mediaUs, ALooper::GetNowUs());
}

status_t NuPlayer::Renderer::getCurrentPosition(
        int64_t *mediaUs, int64_t nowUs, bool allowPastQueuedVideo) {
    Mutex::Autolock autoLock(mTimeLock);
    if (!mHasAudio && !mHasVideo) {
        return NO_INIT;
    }

    if (mAnchorTimeMediaUs < 0) {
        return NO_INIT;
    }

    int64_t positionUs = (nowUs - mAnchorTimeRealUs) + mAnchorTimeMediaUs;

    if (mPauseStartedTimeRealUs != -1) {
        positionUs -= (nowUs - mPauseStartedTimeRealUs);
    }

    // limit position to the last queued media time (for video only stream
    // position will be discrete as we don't know how long each frame lasts)
    if (mAnchorMaxMediaUs >= 0 && !allowPastQueuedVideo) {
        if (positionUs > mAnchorMaxMediaUs) {
            positionUs = mAnchorMaxMediaUs;
        }
    }

    if (positionUs < mAudioFirstAnchorTimeMediaUs) {
        positionUs = mAudioFirstAnchorTimeMediaUs;
    }

    *mediaUs = (positionUs <= 0) ? 0 : positionUs;
    return OK;
}

void NuPlayer::Renderer::setHasMedia(bool audio) {
    Mutex::Autolock autoLock(mTimeLock);
    if (audio) {
        mHasAudio = true;
    } else {
        mHasVideo = true;
    }
}

void NuPlayer::Renderer::setAudioFirstAnchorTime(int64_t mediaUs) {
    Mutex::Autolock autoLock(mTimeLock);
    mAudioFirstAnchorTimeMediaUs = mediaUs;
}

void NuPlayer::Renderer::setAudioFirstAnchorTimeIfNeeded(int64_t mediaUs) {
    Mutex::Autolock autoLock(mTimeLock);
    if (mAudioFirstAnchorTimeMediaUs == -1) {
        mAudioFirstAnchorTimeMediaUs = mediaUs;
    }
}

void NuPlayer::Renderer::setAnchorTime(
        int64_t mediaUs, int64_t realUs, int64_t numFramesWritten, bool resume) {
    Mutex::Autolock autoLock(mTimeLock);
    mAnchorTimeMediaUs = mediaUs;
    mAnchorTimeRealUs = realUs;
    mAnchorNumFramesWritten = numFramesWritten;
    if (resume) {
        mPauseStartedTimeRealUs = -1;
    }
}

void NuPlayer::Renderer::setVideoLateByUs(int64_t lateUs) {
    Mutex::Autolock autoLock(mTimeLock);
    mVideoLateByUs = lateUs;
}

int64_t NuPlayer::Renderer::getVideoLateByUs() {
    Mutex::Autolock autoLock(mTimeLock);
    return mVideoLateByUs;
}

void NuPlayer::Renderer::setPauseStartedTimeRealUs(int64_t realUs) {
    Mutex::Autolock autoLock(mTimeLock);
    mPauseStartedTimeRealUs = realUs;
}

bool NuPlayer::Renderer::openAudioSink(
        const sp<AMessage> &format,
        bool offloadOnly,
        bool hasVideo,
        uint32_t flags) {
    sp<AMessage> msg = new AMessage(kWhatOpenAudioSink, id());
    msg->setMessage("format", format);
    msg->setInt32("offload-only", offloadOnly);
    msg->setInt32("has-video", hasVideo);
    msg->setInt32("flags", flags);

    sp<AMessage> response;
    msg->postAndAwaitResponse(&response);

    int32_t offload;
    CHECK(response->findInt32("offload", &offload));
    return (offload != 0);
}

void NuPlayer::Renderer::closeAudioSink() {
    sp<AMessage> msg = new AMessage(kWhatCloseAudioSink, id());

    sp<AMessage> response;
    msg->postAndAwaitResponse(&response);
}

void NuPlayer::Renderer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatOpenAudioSink:
        {
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));

            int32_t offloadOnly;
            CHECK(msg->findInt32("offload-only", &offloadOnly));

            int32_t hasVideo;
            CHECK(msg->findInt32("has-video", &hasVideo));

            uint32_t flags;
            CHECK(msg->findInt32("flags", (int32_t *)&flags));

            bool offload = onOpenAudioSink(format, offloadOnly, hasVideo, flags);

            sp<AMessage> response = new AMessage;
            response->setInt32("offload", offload);

            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));
            response->postReply(replyID);

            break;
        }

        case kWhatCloseAudioSink:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            onCloseAudioSink();

            sp<AMessage> response = new AMessage;
            response->postReply(replyID);
            break;
        }

        case kWhatStopAudioSink:
        {
            mAudioSink->stop();
            break;
        }

        case kWhatDrainAudioQueue:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mAudioQueueGeneration) {
                break;
            }

            mDrainAudioQueuePending = false;

            if (onDrainAudioQueue()) {
                uint32_t numFramesPlayed;
                CHECK_EQ(mAudioSink->getPosition(&numFramesPlayed),
                         (status_t)OK);

                uint32_t numFramesPendingPlayout =
                    mNumFramesWritten - numFramesPlayed;

                // This is how long the audio sink will have data to
                // play back.
                int64_t delayUs =
                    mAudioSink->msecsPerFrame()
                        * numFramesPendingPlayout * 1000ll;

                // Let's give it more data after about half that time
                // has elapsed.
                // kWhatDrainAudioQueue is used for non-offloading mode,
                // and mLock is used only for offloading mode. Therefore,
                // no need to acquire mLock here.
                postDrainAudioQueue_l(delayUs / 2);
            }
            break;
        }

        case kWhatDrainVideoQueue:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mVideoQueueGeneration) {
                break;
            }

            mDrainVideoQueuePending = false;

            onDrainVideoQueue();

            postDrainVideoQueue();
            break;
        }

        case kWhatPostDrainVideoQueue:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mVideoQueueGeneration) {
                break;
            }

            mDrainVideoQueuePending = false;
            postDrainVideoQueue();
            break;
        }

        case kWhatQueueBuffer:
        {
            onQueueBuffer(msg);
            break;
        }

        case kWhatQueueEOS:
        {
            onQueueEOS(msg);
            break;
        }

        case kWhatFlush:
        {
            onFlush(msg);
            break;
        }

        case kWhatAudioSinkChanged:
        {
            onAudioSinkChanged();
            break;
        }

        case kWhatDisableOffloadAudio:
        {
            onDisableOffloadAudio();
            break;
        }

        case kWhatPause:
        {
            onPause();
            break;
        }

        case kWhatResume:
        {
            onResume();
            break;
        }

        case kWhatSetVideoFrameRate:
        {
            float fps;
            CHECK(msg->findFloat("frame-rate", &fps));
            onSetVideoFrameRate(fps);
            break;
        }

        case kWhatAudioOffloadTearDown:
        {
            onAudioOffloadTearDown(kDueToError);
            break;
        }

        case kWhatAudioOffloadPauseTimeout:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation != mAudioOffloadPauseTimeoutGeneration) {
                break;
            }
            ALOGV("Audio Offload tear down due to pause timeout.");
            onAudioOffloadTearDown(kDueToTimeout);
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void NuPlayer::Renderer::postDrainAudioQueue_l(int64_t delayUs) {
    if (mDrainAudioQueuePending || mSyncQueues || mPaused
            || offloadingAudio()) {
        return;
    }

    if (mAudioQueue.empty()) {
        return;
    }

    mDrainAudioQueuePending = true;
    sp<AMessage> msg = new AMessage(kWhatDrainAudioQueue, id());
    msg->setInt32("generation", mAudioQueueGeneration);
    msg->post(delayUs);
}

void NuPlayer::Renderer::prepareForMediaRenderingStart() {
    mAudioRenderingStartGeneration = mAudioQueueGeneration;
    mVideoRenderingStartGeneration = mVideoQueueGeneration;
}

void NuPlayer::Renderer::notifyIfMediaRenderingStarted() {
    if (mVideoRenderingStartGeneration == mVideoQueueGeneration &&
        mAudioRenderingStartGeneration == mAudioQueueGeneration) {
        mVideoRenderingStartGeneration = -1;
        mAudioRenderingStartGeneration = -1;

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatMediaRenderingStart);
        notify->post();
    }
}

// static
size_t NuPlayer::Renderer::AudioSinkCallback(
        MediaPlayerBase::AudioSink * /* audioSink */,
        void *buffer,
        size_t size,
        void *cookie,
        MediaPlayerBase::AudioSink::cb_event_t event) {
    NuPlayer::Renderer *me = (NuPlayer::Renderer *)cookie;

    switch (event) {
        case MediaPlayerBase::AudioSink::CB_EVENT_FILL_BUFFER:
        {
            return me->fillAudioBuffer(buffer, size);
            break;
        }

        case MediaPlayerBase::AudioSink::CB_EVENT_STREAM_END:
        {
            me->notifyEOS(true /* audio */, ERROR_END_OF_STREAM);
            break;
        }

        case MediaPlayerBase::AudioSink::CB_EVENT_TEAR_DOWN:
        {
            me->notifyAudioOffloadTearDown();
            break;
        }
    }

    return 0;
}

size_t NuPlayer::Renderer::fillAudioBuffer(void *buffer, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (!offloadingAudio() || mPaused) {
        return 0;
    }

    bool hasEOS = false;

    size_t sizeCopied = 0;
    bool firstEntry = true;
    while (sizeCopied < size && !mAudioQueue.empty()) {
        QueueEntry *entry = &*mAudioQueue.begin();

        if (entry->mBuffer == NULL) { // EOS
            hasEOS = true;
            mAudioQueue.erase(mAudioQueue.begin());
            entry = NULL;
            break;
        }

        if (firstEntry && entry->mOffset == 0) {
            firstEntry = false;
            int64_t mediaTimeUs;
            CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));
            ALOGV("rendering audio at media time %.2f secs", mediaTimeUs / 1E6);
            setAudioFirstAnchorTimeIfNeeded(mediaTimeUs);
        }

        size_t copy = entry->mBuffer->size() - entry->mOffset;
        size_t sizeRemaining = size - sizeCopied;
        if (copy > sizeRemaining) {
            copy = sizeRemaining;
        }

        memcpy((char *)buffer + sizeCopied,
               entry->mBuffer->data() + entry->mOffset,
               copy);

        entry->mOffset += copy;
        if (entry->mOffset == entry->mBuffer->size()) {
            entry->mNotifyConsumed->post();
            mAudioQueue.erase(mAudioQueue.begin());
            entry = NULL;
        }
        sizeCopied += copy;
        notifyIfMediaRenderingStarted();
    }

    if (mAudioFirstAnchorTimeMediaUs >= 0) {
        int64_t nowUs = ALooper::GetNowUs();
        setAnchorTime(mAudioFirstAnchorTimeMediaUs, nowUs - getPlayedOutAudioDurationUs(nowUs));
    }

    // we don't know how much data we are queueing for offloaded tracks
    mAnchorMaxMediaUs = -1;

    if (hasEOS) {
        (new AMessage(kWhatStopAudioSink, id()))->post();
    }

    return sizeCopied;
}

bool NuPlayer::Renderer::onDrainAudioQueue() {
    uint32_t numFramesPlayed;
    if (mAudioSink->getPosition(&numFramesPlayed) != OK) {
        return false;
    }

    ssize_t numFramesAvailableToWrite =
        mAudioSink->frameCount() - (mNumFramesWritten - numFramesPlayed);

#if 0
    if (numFramesAvailableToWrite == mAudioSink->frameCount()) {
        ALOGI("audio sink underrun");
    } else {
        ALOGV("audio queue has %d frames left to play",
             mAudioSink->frameCount() - numFramesAvailableToWrite);
    }
#endif

    size_t numBytesAvailableToWrite =
        numFramesAvailableToWrite * mAudioSink->frameSize();

    while (numBytesAvailableToWrite > 0 && !mAudioQueue.empty()) {
        QueueEntry *entry = &*mAudioQueue.begin();

        mLastAudioBufferDrained = entry->mBufferOrdinal;

        if (entry->mBuffer == NULL) {
            // EOS
            int64_t postEOSDelayUs = 0;
            if (mAudioSink->needsTrailingPadding()) {
                postEOSDelayUs = getPendingAudioPlayoutDurationUs(ALooper::GetNowUs());
            }
            notifyEOS(true /* audio */, entry->mFinalResult, postEOSDelayUs);

            mAudioQueue.erase(mAudioQueue.begin());
            entry = NULL;
            return false;
        }

        if (entry->mOffset == 0) {
            int64_t mediaTimeUs;
            CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));
            ALOGV("rendering audio at media time %.2f secs", mediaTimeUs / 1E6);
            onNewAudioMediaTime(mediaTimeUs);
        }

        size_t copy = entry->mBuffer->size() - entry->mOffset;
        if (copy > numBytesAvailableToWrite) {
            copy = numBytesAvailableToWrite;
        }

        ssize_t written = mAudioSink->write(entry->mBuffer->data() + entry->mOffset, copy);
        if (written < 0) {
            // An error in AudioSink write is fatal here.
            LOG_ALWAYS_FATAL("AudioSink write error(%zd) when writing %zu bytes", written, copy);
        }

        entry->mOffset += written;
        if (entry->mOffset == entry->mBuffer->size()) {
            entry->mNotifyConsumed->post();
            mAudioQueue.erase(mAudioQueue.begin());

            entry = NULL;
        }

        numBytesAvailableToWrite -= written;
        size_t copiedFrames = written / mAudioSink->frameSize();
        mNumFramesWritten += copiedFrames;

        notifyIfMediaRenderingStarted();

        if (written != (ssize_t)copy) {
            // A short count was received from AudioSink::write()
            //
            // AudioSink write should block until exactly the number of bytes are delivered.
            // But it may return with a short count (without an error) when:
            //
            // 1) Size to be copied is not a multiple of the frame size. We consider this fatal.
            // 2) AudioSink is an AudioCache for data retrieval, and the AudioCache is exceeded.

            // (Case 1)
            // Must be a multiple of the frame size.  If it is not a multiple of a frame size, it
            // needs to fail, as we should not carry over fractional frames between calls.
            CHECK_EQ(copy % mAudioSink->frameSize(), 0);

            // (Case 2)
            // Return early to the caller.
            // Beware of calling immediately again as this may busy-loop if you are not careful.
            ALOGW("AudioSink write short frame count %zd < %zu", written, copy);
            break;
        }
    }
    mAnchorMaxMediaUs =
        mAnchorTimeMediaUs +
                (int64_t)(max((long long)mNumFramesWritten - mAnchorNumFramesWritten, 0LL)
                        * 1000LL * mAudioSink->msecsPerFrame());

    return !mAudioQueue.empty();
}

int64_t NuPlayer::Renderer::getPendingAudioPlayoutDurationUs(int64_t nowUs) {
    int64_t writtenAudioDurationUs =
        mNumFramesWritten * 1000LL * mAudioSink->msecsPerFrame();
    return writtenAudioDurationUs - getPlayedOutAudioDurationUs(nowUs);
}

int64_t NuPlayer::Renderer::getRealTimeUs(int64_t mediaTimeUs, int64_t nowUs) {
    int64_t currentPositionUs;
    if (getCurrentPosition(&currentPositionUs, nowUs, true /* allowPastQueuedVideo */) != OK) {
        // If failed to get current position, e.g. due to audio clock is not ready, then just
        // play out video immediately without delay.
        return nowUs;
    }
    return (mediaTimeUs - currentPositionUs) + nowUs;
}

void NuPlayer::Renderer::onNewAudioMediaTime(int64_t mediaTimeUs) {
    // TRICKY: vorbis decoder generates multiple frames with the same
    // timestamp, so only update on the first frame with a given timestamp
    if (mediaTimeUs == mAnchorTimeMediaUs) {
        return;
    }
    setAudioFirstAnchorTimeIfNeeded(mediaTimeUs);
    int64_t nowUs = ALooper::GetNowUs();
    setAnchorTime(
            mediaTimeUs, nowUs + getPendingAudioPlayoutDurationUs(nowUs), mNumFramesWritten);
}

void NuPlayer::Renderer::postDrainVideoQueue() {
    if (mDrainVideoQueuePending
            || mSyncQueues
            || (mPaused && mVideoSampleReceived)) {
        return;
    }

    if (mVideoQueue.empty()) {
        return;
    }

    QueueEntry &entry = *mVideoQueue.begin();

    sp<AMessage> msg = new AMessage(kWhatDrainVideoQueue, id());
    msg->setInt32("generation", mVideoQueueGeneration);

    if (entry.mBuffer == NULL) {
        // EOS doesn't carry a timestamp.
        msg->post();
        mDrainVideoQueuePending = true;
        return;
    }

    int64_t delayUs;
    int64_t nowUs = ALooper::GetNowUs();
    int64_t realTimeUs;
    if (mFlags & FLAG_REAL_TIME) {
        int64_t mediaTimeUs;
        CHECK(entry.mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));
        realTimeUs = mediaTimeUs;
    } else {
        int64_t mediaTimeUs;
        CHECK(entry.mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

        if (mAnchorTimeMediaUs < 0) {
            setAnchorTime(mediaTimeUs, nowUs);
            realTimeUs = nowUs;
        } else {
            realTimeUs = getRealTimeUs(mediaTimeUs, nowUs);
        }
        if (!mHasAudio) {
            mAnchorMaxMediaUs = mediaTimeUs + 100000; // smooth out videos >= 10fps
        }

        // Heuristics to handle situation when media time changed without a
        // discontinuity. If we have not drained an audio buffer that was
        // received after this buffer, repost in 10 msec. Otherwise repost
        // in 500 msec.
        delayUs = realTimeUs - nowUs;
        if (delayUs > 500000) {
            int64_t postDelayUs = 500000;
            if (mHasAudio && (mLastAudioBufferDrained - entry.mBufferOrdinal) <= 0) {
                postDelayUs = 10000;
            }
            msg->setWhat(kWhatPostDrainVideoQueue);
            msg->post(postDelayUs);
            mVideoScheduler->restart();
            ALOGI("possible video time jump of %dms, retrying in %dms",
                    (int)(delayUs / 1000), (int)(postDelayUs / 1000));
            mDrainVideoQueuePending = true;
            return;
        }
    }

    realTimeUs = mVideoScheduler->schedule(realTimeUs * 1000) / 1000;
    int64_t twoVsyncsUs = 2 * (mVideoScheduler->getVsyncPeriod() / 1000);

    delayUs = realTimeUs - nowUs;

    ALOGW_IF(delayUs > 500000, "unusually high delayUs: %" PRId64, delayUs);
    // post 2 display refreshes before rendering is due
    // FIXME currently this increases power consumption, so unless frame-accurate
    // AV sync is requested, post closer to required render time (at 0.63 vsyncs)
    if (!sFrameAccurateAVsync) {
        twoVsyncsUs >>= 4;
    }
    msg->post(delayUs > twoVsyncsUs ? delayUs - twoVsyncsUs : 0);

    mDrainVideoQueuePending = true;
}

void NuPlayer::Renderer::onDrainVideoQueue() {
    if (mVideoQueue.empty()) {
        return;
    }

    QueueEntry *entry = &*mVideoQueue.begin();

    if (entry->mBuffer == NULL) {
        // EOS

        notifyEOS(false /* audio */, entry->mFinalResult);

        mVideoQueue.erase(mVideoQueue.begin());
        entry = NULL;

        setVideoLateByUs(0);
        return;
    }

    int64_t nowUs = -1;
    int64_t realTimeUs;
    int64_t mediaTimeUs;
    if (mFlags & FLAG_REAL_TIME) {
        CHECK(entry->mBuffer->meta()->findInt64("timeUs", &realTimeUs));
    } else {
        CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

        nowUs = ALooper::GetNowUs();
        realTimeUs = getRealTimeUs(mediaTimeUs, nowUs);
    }

    bool tooLate = false;

    if (!mPaused) {
        if (nowUs == -1) {
            nowUs = ALooper::GetNowUs();
        }
        setVideoLateByUs(nowUs - realTimeUs);
        tooLate = (mVideoLateByUs > 40000);

        if (tooLate) {
            ALOGV("video late by %lld us (%.2f secs)",
                 mVideoLateByUs, mVideoLateByUs / 1E6);
        } else {
            ALOGV("rendering video at media time %.2f secs",
                    (mFlags & FLAG_REAL_TIME ? realTimeUs :
                    (realTimeUs + mAnchorTimeMediaUs - mAnchorTimeRealUs)) / 1E6);
        }
    } else {
        setVideoLateByUs(0);
        if (!mVideoSampleReceived && !mHasAudio) {
            // This will ensure that the first frame after a flush won't be used as anchor
            // when renderer is in paused state, because resume can happen any time after seek.
            setAnchorTime(-1, -1);
        }
    }

    entry->mNotifyConsumed->setInt64("timestampNs", realTimeUs * 1000ll);
    entry->mNotifyConsumed->setInt32("render", !tooLate);
    entry->mNotifyConsumed->post();
    mVideoQueue.erase(mVideoQueue.begin());
    entry = NULL;

    mVideoSampleReceived = true;

    if (!mPaused) {
        if (!mVideoRenderingStarted) {
            mVideoRenderingStarted = true;
            notifyVideoRenderingStart();
        }
        notifyIfMediaRenderingStarted();
    }

    if (tooLate) { //dropped!
        PLAYER_STATS(logFrameDropped);
    } else {
        PLAYER_STATS(logFrameRendered);
    }
}

void NuPlayer::Renderer::notifyVideoRenderingStart() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatVideoRenderingStart);
    notify->post();
}

void NuPlayer::Renderer::notifyEOS(bool audio, status_t finalResult, int64_t delayUs) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatEOS);
    notify->setInt32("audio", static_cast<int32_t>(audio));
    notify->setInt32("finalResult", finalResult);
    notify->post(delayUs);
}

void NuPlayer::Renderer::notifyAudioOffloadTearDown() {
    (new AMessage(kWhatAudioOffloadTearDown, id()))->post();
}

void NuPlayer::Renderer::onQueueBuffer(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    setHasMedia(audio);

    if (mHasVideo) {
        if (mVideoScheduler == NULL) {
            mVideoScheduler = new VideoFrameScheduler();
            mVideoScheduler->init();
        }
    }

    if (dropBufferWhileFlushing(audio, msg)) {
        return;
    }

    sp<ABuffer> buffer;
    CHECK(msg->findBuffer("buffer", &buffer));

    sp<AMessage> notifyConsumed;
    CHECK(msg->findMessage("notifyConsumed", &notifyConsumed));

    QueueEntry entry;
    entry.mBuffer = buffer;
    entry.mNotifyConsumed = notifyConsumed;
    entry.mOffset = 0;
    entry.mFinalResult = OK;
    entry.mBufferOrdinal = ++mTotalBuffersQueued;

    if (audio) {
        Mutex::Autolock autoLock(mLock);
        mAudioQueue.push_back(entry);
        postDrainAudioQueue_l();
    } else {
        mVideoQueue.push_back(entry);
        postDrainVideoQueue();
    }

    Mutex::Autolock autoLock(mLock);
    if (!mSyncQueues || mAudioQueue.empty() || mVideoQueue.empty()) {
        return;
    }

    sp<ABuffer> firstAudioBuffer = (*mAudioQueue.begin()).mBuffer;
    sp<ABuffer> firstVideoBuffer = (*mVideoQueue.begin()).mBuffer;

    if (firstAudioBuffer == NULL || firstVideoBuffer == NULL) {
        // EOS signalled on either queue.
        syncQueuesDone_l();
        return;
    }

    int64_t firstAudioTimeUs;
    int64_t firstVideoTimeUs;
    CHECK(firstAudioBuffer->meta()
            ->findInt64("timeUs", &firstAudioTimeUs));
    CHECK(firstVideoBuffer->meta()
            ->findInt64("timeUs", &firstVideoTimeUs));

    int64_t diff = firstVideoTimeUs - firstAudioTimeUs;

    ALOGV("queueDiff = %.2f secs", diff / 1E6);

    if (diff > 100000ll) {
        // Audio data starts More than 0.1 secs before video.
        // Drop some audio.

        (*mAudioQueue.begin()).mNotifyConsumed->post();
        mAudioQueue.erase(mAudioQueue.begin());
        return;
    }

    syncQueuesDone_l();
}

void NuPlayer::Renderer::syncQueuesDone_l() {
    if (!mSyncQueues) {
        return;
    }

    mSyncQueues = false;

    if (!mAudioQueue.empty()) {
        postDrainAudioQueue_l();
    }

    if (!mVideoQueue.empty()) {
        postDrainVideoQueue();
    }
}

void NuPlayer::Renderer::onQueueEOS(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    if (dropBufferWhileFlushing(audio, msg)) {
        return;
    }

    int32_t finalResult;
    CHECK(msg->findInt32("finalResult", &finalResult));

    QueueEntry entry;
    entry.mOffset = 0;
    entry.mFinalResult = finalResult;

    if (audio) {
        Mutex::Autolock autoLock(mLock);
        if (mAudioQueue.empty() && mSyncQueues) {
            syncQueuesDone_l();
        }
        mAudioQueue.push_back(entry);
        postDrainAudioQueue_l();
    } else {
        if (mVideoQueue.empty() && mSyncQueues) {
            Mutex::Autolock autoLock(mLock);
            syncQueuesDone_l();
        }
        mVideoQueue.push_back(entry);
        postDrainVideoQueue();
    }
}

void NuPlayer::Renderer::onFlush(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            mFlushingAudio = false;
        } else {
            mFlushingVideo = false;
        }
    }

    // If we're currently syncing the queues, i.e. dropping audio while
    // aligning the first audio/video buffer times and only one of the
    // two queues has data, we may starve that queue by not requesting
    // more buffers from the decoder. If the other source then encounters
    // a discontinuity that leads to flushing, we'll never find the
    // corresponding discontinuity on the other queue.
    // Therefore we'll stop syncing the queues if at least one of them
    // is flushed.
    {
         Mutex::Autolock autoLock(mLock);
         syncQueuesDone_l();
         setPauseStartedTimeRealUs(-1);
    }

    ALOGV("flushing %s", audio ? "audio" : "video");
    if (audio) {
        {
            Mutex::Autolock autoLock(mLock);
            flushQueue(&mAudioQueue);

            ++mAudioQueueGeneration;
            prepareForMediaRenderingStart();

            if (offloadingAudio()) {
                setAudioFirstAnchorTime(-1);
            }
        }

        mDrainAudioQueuePending = false;

        if (offloadingAudio()) {
            mAudioSink->pause();
            mAudioSink->flush();
            mAudioSink->start();
        }
    } else {
        flushQueue(&mVideoQueue);

        mDrainVideoQueuePending = false;
        ++mVideoQueueGeneration;

        if (mVideoScheduler != NULL) {
            mVideoScheduler->restart();
        }

        prepareForMediaRenderingStart();
    }

    mVideoSampleReceived = false;
    notifyFlushComplete(audio);
}

void NuPlayer::Renderer::flushQueue(List<QueueEntry> *queue) {
    while (!queue->empty()) {
        QueueEntry *entry = &*queue->begin();

        if (entry->mBuffer != NULL) {
            entry->mNotifyConsumed->post();
        }

        queue->erase(queue->begin());
        entry = NULL;
    }
}

void NuPlayer::Renderer::notifyFlushComplete(bool audio) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushComplete);
    notify->setInt32("audio", static_cast<int32_t>(audio));
    notify->post();
}

bool NuPlayer::Renderer::dropBufferWhileFlushing(
        bool audio, const sp<AMessage> &msg) {
    bool flushing = false;

    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            flushing = mFlushingAudio;
        } else {
            flushing = mFlushingVideo;
        }
    }

    if (!flushing) {
        return false;
    }

    sp<AMessage> notifyConsumed;
    if (msg->findMessage("notifyConsumed", &notifyConsumed)) {
        notifyConsumed->post();
    }

    return true;
}

void NuPlayer::Renderer::onAudioSinkChanged() {
    if (offloadingAudio()) {
        return;
    }
    CHECK(!mDrainAudioQueuePending);
    mNumFramesWritten = 0;
    mAnchorNumFramesWritten = -1;
    uint32_t written;
    if (mAudioSink->getFramesWritten(&written) == OK) {
        mNumFramesWritten = written;
    }
}

void NuPlayer::Renderer::onDisableOffloadAudio() {
    Mutex::Autolock autoLock(mLock);
    mFlags &= ~FLAG_OFFLOAD_AUDIO;
    ++mAudioQueueGeneration;
}

void NuPlayer::Renderer::onPause() {
    if (mPaused) {
        ALOGW("Renderer::onPause() called while already paused!");
        return;
    }
    {
        Mutex::Autolock autoLock(mLock);
        ++mAudioQueueGeneration;
        ++mVideoQueueGeneration;
        prepareForMediaRenderingStart();
        mPaused = true;
        setPauseStartedTimeRealUs(ALooper::GetNowUs());
    }

    mDrainAudioQueuePending = false;
    mDrainVideoQueuePending = false;

    if (mHasAudio) {
        mAudioSink->pause();
        startAudioOffloadPauseTimeout();
    }

    ALOGV("now paused audio queue has %d entries, video has %d entries",
          mAudioQueue.size(), mVideoQueue.size());

    PLAYER_STATS(notifyPause, (ALooper::GetNowUs() - mAnchorTimeRealUs) + mAnchorTimeMediaUs);
}

void NuPlayer::Renderer::onResume() {
    readProperties();

    if (!mPaused) {
        return;
    }

    if (mHasAudio) {
        cancelAudioOffloadPauseTimeout();
        mAudioSink->start();
    }

    Mutex::Autolock autoLock(mLock);
    mPaused = false;
    if (mPauseStartedTimeRealUs != -1) {
        int64_t newAnchorRealUs =
            mAnchorTimeRealUs + ALooper::GetNowUs() - mPauseStartedTimeRealUs;
        setAnchorTime(
                mAnchorTimeMediaUs, newAnchorRealUs, mAnchorNumFramesWritten, true /* resume */);
    }

    if (!mAudioQueue.empty()) {
        postDrainAudioQueue_l();
    }

    if (!mVideoQueue.empty()) {
        postDrainVideoQueue();
    }
}

void NuPlayer::Renderer::onSetVideoFrameRate(float fps) {
    if (mVideoScheduler == NULL) {
        mVideoScheduler = new VideoFrameScheduler();
    }
    mVideoScheduler->init(fps);
}

// TODO: Remove unnecessary calls to getPlayedOutAudioDurationUs()
// as it acquires locks and may query the audio driver.
//
// Some calls could conceivably retrieve extrapolated data instead of
// accessing getTimestamp() or getPosition() every time a data buffer with
// a media time is received.
//
int64_t NuPlayer::Renderer::getPlayedOutAudioDurationUs(int64_t nowUs) {
    uint32_t numFramesPlayed;
    int64_t numFramesPlayedAt;
    AudioTimestamp ts;
    static const int64_t kStaleTimestamp100ms = 100000;

    status_t res = mAudioSink->getTimestamp(ts);
    if (res == OK) {                 // case 1: mixing audio tracks and offloaded tracks.
        numFramesPlayed = ts.mPosition;
        numFramesPlayedAt =
            ts.mTime.tv_sec * 1000000LL + ts.mTime.tv_nsec / 1000;
        const int64_t timestampAge = nowUs - numFramesPlayedAt;
        if (timestampAge > kStaleTimestamp100ms) {
            // This is an audio FIXME.
            // getTimestamp returns a timestamp which may come from audio mixing threads.
            // After pausing, the MixerThread may go idle, thus the mTime estimate may
            // become stale. Assuming that the MixerThread runs 20ms, with FastMixer at 5ms,
            // the max latency should be about 25ms with an average around 12ms (to be verified).
            // For safety we use 100ms.
            ALOGV("getTimestamp: returned stale timestamp nowUs(%lld) numFramesPlayedAt(%lld)",
                    (long long)nowUs, (long long)numFramesPlayedAt);
            numFramesPlayedAt = nowUs - kStaleTimestamp100ms;
        }
        //ALOGD("getTimestamp: OK %d %lld", numFramesPlayed, (long long)numFramesPlayedAt);
    } else if (res == WOULD_BLOCK) { // case 2: transitory state on start of a new track
        numFramesPlayed = 0;
        numFramesPlayedAt = nowUs;
        //ALOGD("getTimestamp: WOULD_BLOCK %d %lld",
        //        numFramesPlayed, (long long)numFramesPlayedAt);
    } else {                         // case 3: transitory at new track or audio fast tracks.
        res = mAudioSink->getPosition(&numFramesPlayed);
        CHECK_EQ(res, (status_t)OK);
        numFramesPlayedAt = nowUs;
        numFramesPlayedAt += 1000LL * mAudioSink->latency() / 2; /* XXX */
        //ALOGD("getPosition: %d %lld", numFramesPlayed, numFramesPlayedAt);
    }

    // TODO: remove the (int32_t) casting below as it may overflow at 12.4 hours.
    //CHECK_EQ(numFramesPlayed & (1 << 31), 0);  // can't be negative until 12.4 hrs, test
    int64_t durationUs = (int64_t)((int32_t)numFramesPlayed * 1000LL * mAudioSink->msecsPerFrame())
            + nowUs - numFramesPlayedAt;
    if (durationUs < 0) {
        // Occurs when numFramesPlayed position is very small and the following:
        // (1) In case 1, the time nowUs is computed before getTimestamp() is called and
        //     numFramesPlayedAt is greater than nowUs by time more than numFramesPlayed.
        // (2) In case 3, using getPosition and adding mAudioSink->latency() to
        //     numFramesPlayedAt, by a time amount greater than numFramesPlayed.
        //
        // Both of these are transitory conditions.
        ALOGV("getPlayedOutAudioDurationUs: negative duration %lld set to zero", (long long)durationUs);
        durationUs = 0;
    }
    ALOGV("getPlayedOutAudioDurationUs(%lld) nowUs(%lld) frames(%u) framesAt(%lld)",
            (long long)durationUs, (long long)nowUs, numFramesPlayed, (long long)numFramesPlayedAt);
    return durationUs;
}

void NuPlayer::Renderer::onAudioOffloadTearDown(AudioOffloadTearDownReason reason) {
    if (mAudioOffloadTornDown) {
        return;
    }
    mAudioOffloadTornDown = true;

    int64_t currentPositionUs;
    if (getCurrentPosition(&currentPositionUs) != OK) {
        currentPositionUs = 0;
    }

    mAudioSink->stop();
    mAudioSink->flush();

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatAudioOffloadTearDown);
    notify->setInt64("positionUs", currentPositionUs);
    notify->setInt32("reason", reason);
    notify->post();
}

void NuPlayer::Renderer::startAudioOffloadPauseTimeout() {
    if (offloadingAudio()) {
        sp<AMessage> msg = new AMessage(kWhatAudioOffloadPauseTimeout, id());
        msg->setInt32("generation", mAudioOffloadPauseTimeoutGeneration);
        msg->post(kOffloadPauseMaxUs);
    }
}

void NuPlayer::Renderer::cancelAudioOffloadPauseTimeout() {
    if (offloadingAudio()) {
        ++mAudioOffloadPauseTimeoutGeneration;
    }
}

bool NuPlayer::Renderer::onOpenAudioSink(
        const sp<AMessage> &format,
        bool offloadOnly,
        bool hasVideo,
        uint32_t flags) {
    ALOGV("openAudioSink: offloadOnly(%d) offloadingAudio(%d)",
            offloadOnly, offloadingAudio());
    bool audioSinkChanged = false;

    int32_t numChannels;
    CHECK(format->findInt32("channel-count", &numChannels));

    int32_t channelMask;
    if (!format->findInt32("channel-mask", &channelMask)) {
        // signal to the AudioSink to derive the mask from count.
        channelMask = CHANNEL_MASK_USE_CHANNEL_ORDER;
    }

    int32_t sampleRate;
    CHECK(format->findInt32("sample-rate", &sampleRate));

    if (offloadingAudio()) {
        audio_format_t audioFormat = AUDIO_FORMAT_PCM_16_BIT;
        AString mime;
        CHECK(format->findString("mime", &mime));
        status_t err = mapMimeToAudioFormat(audioFormat, mime.c_str());

        if (err != OK) {
            ALOGE("Couldn't map mime \"%s\" to a valid "
                    "audio_format", mime.c_str());
            onDisableOffloadAudio();
        } else {
            int32_t bitWidth = 16;
            if (AUDIO_FORMAT_PCM_16_BIT == audioFormat) {
                if ((ExtendedUtils::getPcmSampleBits(format) == 24) &&
                    ExtendedUtils::is24bitPCMOffloadEnabled()) {
                    bitWidth = 24;
                    audioFormat = AUDIO_FORMAT_PCM_24_BIT_OFFLOAD;
                } else if (ExtendedUtils::is16bitPCMOffloadEnabled()) {
                    bitWidth = 16;
                    audioFormat = AUDIO_FORMAT_PCM_16_BIT_OFFLOAD;
                }
            }
            ALOGV("Mime \"%s\" mapped to audio_format 0x%x",
                    mime.c_str(), audioFormat);

            int avgBitRate = -1;
            format->findInt32("bit-rate", &avgBitRate);

            int32_t aacProfile = -1;
            if (audioFormat == AUDIO_FORMAT_AAC
                    && format->findInt32("aac-profile", &aacProfile)) {
                // Redefine AAC format as per aac profile
                mapAACProfileToAudioFormat(
                        audioFormat,
                        aacProfile);
            }

            audio_offload_info_t offloadInfo = AUDIO_INFO_INITIALIZER;
            offloadInfo.duration_us = -1;
            format->findInt64(
                    "durationUs", &offloadInfo.duration_us);
            offloadInfo.sample_rate = sampleRate;
            offloadInfo.channel_mask = channelMask;
            offloadInfo.format = audioFormat;
            offloadInfo.stream_type = AUDIO_STREAM_MUSIC;
            offloadInfo.bit_rate = avgBitRate;
            offloadInfo.has_video = hasVideo;
            offloadInfo.is_streaming = true;
            offloadInfo.use_small_bufs =
                (audioFormat == AUDIO_FORMAT_PCM_16_BIT_OFFLOAD);
            offloadInfo.bit_width = bitWidth;

            if (memcmp(&mCurrentOffloadInfo, &offloadInfo, sizeof(offloadInfo)) == 0) {
                ALOGV("openAudioSink: no change in offload mode");
                // no change from previous configuration, everything ok.
                return offloadingAudio();
            }
            ALOGV("openAudioSink: try to open AudioSink in offload mode");
            flags |= AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
            flags &= ~AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
            audioSinkChanged = true;
            mAudioSink->close();
            err = mAudioSink->open(
                    sampleRate,
                    numChannels,
                    (audio_channel_mask_t)channelMask,
                    audioFormat,
                    8 /* bufferCount */,
                    &NuPlayer::Renderer::AudioSinkCallback,
                    this,
                    (audio_output_flags_t)flags,
                    &offloadInfo);

            if (err == OK) {
                // If the playback is offloaded to h/w, we pass
                // the HAL some metadata information.
                // We don't want to do this for PCM because it
                // will be going through the AudioFlinger mixer
                // before reaching the hardware.
                // TODO
                mCurrentOffloadInfo = offloadInfo;
                err = mAudioSink->start();
                ALOGV_IF(err == OK, "openAudioSink: offload succeeded");
            }
            if (err != OK) {
                // Clean up, fall back to non offload mode.
                mAudioSink->close();
                onDisableOffloadAudio();
                mCurrentOffloadInfo = AUDIO_INFO_INITIALIZER;
                ALOGV("openAudioSink: offload failed");
            }
        }
    }
    if (!offloadOnly && !offloadingAudio()) {
        flags &= ~AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD;
        ALOGV("openAudioSink: open AudioSink in NON-offload mode");

        audioSinkChanged = true;
        mAudioSink->close();
        mCurrentOffloadInfo = AUDIO_INFO_INITIALIZER;
        CHECK_EQ(mAudioSink->open(
                    sampleRate,
                    numChannels,
                    (audio_channel_mask_t)channelMask,
                    AUDIO_FORMAT_PCM_16_BIT,
                    8 /* bufferCount */,
                    NULL,
                    NULL,
                    (audio_output_flags_t)flags),
                 (status_t)OK);
        mAudioSink->start();
    }
    if (audioSinkChanged) {
        onAudioSinkChanged();
    }

    return offloadingAudio();
}

void NuPlayer::Renderer::onCloseAudioSink() {
    mAudioSink->close();
    mCurrentOffloadInfo = AUDIO_INFO_INITIALIZER;
}

}  // namespace android

