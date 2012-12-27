/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2009-2012, The Linux Foundation. All rights reserved.
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
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

#define LOG_NDDEBUG 0
//#define LOG_NDEBUG 0
#define LOG_TAG "LPAPlayerALSA"

#include <utils/Log.h>
#include <utils/threads.h>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>

#include <media/stagefright/LPAPlayer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaErrors.h>

#include <hardware_legacy/power.h>

#include <linux/unistd.h>

#include "include/AwesomePlayer.h"
#include <powermanager/PowerManager.h>

static const char   mName[] = "LPAPlayer";

#define MEM_BUFFER_SIZE 262144
#define MEM_BUFFER_COUNT 4

#define PCM_FORMAT 2
#define NUM_FDS 2
namespace android {
int LPAPlayer::objectsAlive = 0;

LPAPlayer::LPAPlayer(
                    const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                    AwesomePlayer *observer)
:AudioPlayer(audioSink,observer),
mPositionTimeMediaUs(-1),
mPositionTimeRealUs(-1),
mInternalSeeking(false),
mStarted(false),
mA2DPEnabled(false),
mSampleRate(0),
mLatencyUs(0),
mFrameSize(0),
mNumFramesPlayed(0),
mNumFramesPlayedSysTimeUs(0),
mInputBuffer(NULL),
mSeeking(false),
mReachedEOS(false),
mReachedOutputEOS(false),
mFinalStatus(OK),
mSeekTimeUs(0),
mPauseTime(0),
mIsFirstBuffer(false),
mFirstBufferResult(OK),
mFirstBuffer(NULL),
mAudioSink(audioSink),
mObserver(observer) {
    ALOGV("LPAPlayer::LPAPlayer() ctor");
    objectsAlive++;
    numChannels =0;
    mPaused = false;
    mIsA2DPEnabled = false;
    mAudioFlinger = NULL;
    AudioFlingerClient = NULL;
    /* Initialize Suspend/Resume related variables */
    mQueue.start();
    mQueueStarted      = true;
    mPauseEvent        = new TimedEvent(this, &LPAPlayer::onPauseTimeOut);
    mPauseEventPending = false;
    getAudioFlinger();
    ALOGV("Registering client with AudioFlinger");
    //mAudioFlinger->registerClient(AudioFlingerClient);

    mIsAudioRouted = false;

    initCheck = true;

}

LPAPlayer::~LPAPlayer() {
    ALOGV("LPAPlayer::~LPAPlayer()");
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();

    //mAudioFlinger->deregisterClient(AudioFlingerClient);
    objectsAlive--;
}

void LPAPlayer::getAudioFlinger() {
    Mutex::Autolock _l(AudioFlingerLock);

    if ( mAudioFlinger.get() == 0 ) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder;
        do {
            binder = sm->getService(String16("media.audio_flinger"));
            if ( binder != 0 )
                break;
            ALOGW("AudioFlinger not published, waiting...");
            usleep(500000); // 0.5 s
        } while ( true );
        if ( AudioFlingerClient == NULL ) {
            AudioFlingerClient = new AudioFlingerLPAdecodeClient(this);
        }

        binder->linkToDeath(AudioFlingerClient);
        mAudioFlinger = interface_cast<IAudioFlinger>(binder);
    }
    ALOGE_IF(mAudioFlinger==0, "no AudioFlinger!?");
}

LPAPlayer::AudioFlingerLPAdecodeClient::AudioFlingerLPAdecodeClient(void *obj)
{
    ALOGV("LPAPlayer::AudioFlingerLPAdecodeClient::AudioFlingerLPAdecodeClient");
    pBaseClass = (LPAPlayer*)obj;
}

void LPAPlayer::AudioFlingerLPAdecodeClient::binderDied(const wp<IBinder>& who) {
    Mutex::Autolock _l(pBaseClass->AudioFlingerLock);

    pBaseClass->mAudioFlinger.clear();
    ALOGW("AudioFlinger server died!");
}

void LPAPlayer::AudioFlingerLPAdecodeClient::ioConfigChanged(int event, audio_io_handle_t ioHandle, const void *param2) {
    ALOGV("ioConfigChanged() event %d", event);
    /*
    if ( event != AudioSystem::A2DP_OUTPUT_STATE &&
         event != AudioSystem::EFFECT_CONFIG_CHANGED) {
        return;
    }

    switch ( event ) {
    case AudioSystem::A2DP_OUTPUT_STATE:
        {
            ALOGV("ioConfigChanged() A2DP_OUTPUT_STATE iohandle is %d with A2DPEnabled in %d", ioHandle, pBaseClass->mIsA2DPEnabled);
            if ( -1 == ioHandle ) {
                if ( pBaseClass->mIsA2DPEnabled ) {
                    pBaseClass->mIsA2DPEnabled = false;
                    if (pBaseClass->mStarted) {
                        pBaseClass->handleA2DPSwitch();
                    }
                    ALOGV("ioConfigChanged:: A2DP Disabled");
                }
            } else {
                if ( !pBaseClass->mIsA2DPEnabled ) {

                    pBaseClass->mIsA2DPEnabled = true;
                    if (pBaseClass->mStarted) {
                        pBaseClass->handleA2DPSwitch();
                    }

                    ALOGV("ioConfigChanged:: A2DP Enabled");
                }
            }
        }
        break;
    }
    ALOGV("ioConfigChanged Out");
    */
}

void LPAPlayer::handleA2DPSwitch() {
    //TODO: Implement
}

void LPAPlayer::setSource(const sp<MediaSource> &source) {
    CHECK(mSource == NULL);
    ALOGV("Setting source from LPA Player");
    mSource = source;
}

status_t LPAPlayer::start(bool sourceAlreadyStarted) {
    CHECK(!mStarted);
    CHECK(mSource != NULL);

    ALOGV("start: sourceAlreadyStarted %d", sourceAlreadyStarted);
    //Check if the source is started, start it
    status_t err;
    if (!sourceAlreadyStarted) {
        err = mSource->start();

        if (err != OK) {
            return err;
        }
    }

    //Create decoder and a2dp notification thread and initialize all the
    //mutexes and coditional variables
    createThreads();
    ALOGV("All Threads Created.");

    // We allow an optional INFO_FORMAT_CHANGED at the very beginning
    // of playback, if there is one, getFormat below will retrieve the
    // updated format, if there isn't, we'll stash away the valid buffer
    // of data to be used on the first audio callback.

    CHECK(mFirstBuffer == NULL);

    MediaSource::ReadOptions options;
    if (mSeeking) {
        options.setSeekTo(mSeekTimeUs);
        mSeeking = false;
    }

    mFirstBufferResult = mSource->read(&mFirstBuffer, &options);
    if (mFirstBufferResult == INFO_FORMAT_CHANGED) {
        ALOGV("INFO_FORMAT_CHANGED!!!");
        CHECK(mFirstBuffer == NULL);
        mFirstBufferResult = OK;
        mIsFirstBuffer = false;
    } else {
        mIsFirstBuffer = true;
    }

    sp<MetaData> format = mSource->getFormat();
    const char *mime;

    bool success = format->findCString(kKeyMIMEType, &mime);
    CHECK(success);
    CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));

    success = format->findInt32(kKeySampleRate, &mSampleRate);
    CHECK(success);

    success = format->findInt32(kKeyChannelCount, &numChannels);
    CHECK(success);

    if(!format->findInt32(kKeyChannelMask, &mChannelMask)) {
        // log only when there's a risk of ambiguity of channel mask selection
        ALOGI_IF(numChannels > 2,
                "source format didn't specify channel mask, using (%d) channel order", numChannels);
        mChannelMask = CHANNEL_MASK_USE_CHANNEL_ORDER;
    }
    audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_LPA |
                                                         AUDIO_OUTPUT_FLAG_DIRECT);
    ALOGV("mAudiosink->open() mSampleRate %d, numChannels %d, mChannelMask %d, flags %d",mSampleRate, numChannels, mChannelMask, flags);
    err = mAudioSink->open(
        mSampleRate, numChannels, mChannelMask, AUDIO_FORMAT_PCM_16_BIT,
        DEFAULT_AUDIOSINK_BUFFERCOUNT,
        &LPAPlayer::AudioSinkCallback,
        this,
        (mA2DPEnabled ? AUDIO_OUTPUT_FLAG_NONE : flags));

    if (err != OK) {
        if (mFirstBuffer != NULL) {
            mFirstBuffer->release();
            mFirstBuffer = NULL;
        }

        if (!sourceAlreadyStarted) {
            mSource->stop();
        }

        ALOGE("Opening a routing session failed");
        return err;
    }

    mIsAudioRouted = true;
    mStarted = true;
    mAudioSink->start();
    ALOGV("Waking up decoder thread");
    pthread_cond_signal(&decoder_cv);

    return OK;
}

status_t LPAPlayer::seekTo(int64_t time_us) {
    Mutex::Autolock autoLock(mLock);
    ALOGV("seekTo: time_us %lld", time_us);
    if ( mReachedEOS ) {
        mReachedEOS = false;
        mReachedOutputEOS = false;
    }
    mSeeking = true;
    mSeekTimeUs = time_us;
    mPauseTime = mSeekTimeUs;
    ALOGV("In seekTo(), mSeekTimeUs %lld",mSeekTimeUs);
    mAudioSink->flush();
    pthread_cond_signal(&decoder_cv);
    return OK;
}

void LPAPlayer::pause(bool playPendingSamples) {
    CHECK(mStarted);
    if (mPaused) {
        return;
    }
    ALOGV("pause: playPendingSamples %d", playPendingSamples);
    mPaused = true;
    A2DPState state;
    if (playPendingSamples) {
        if (!mIsA2DPEnabled) {
            if (!mPauseEventPending) {
                ALOGV("Posting an event for Pause timeout");
                mQueue.postEventWithDelay(mPauseEvent, LPA_PAUSE_TIMEOUT_USEC);
                mPauseEventPending = true;
            }
            mPauseTime = mSeekTimeUs + getTimeStamp(A2DP_DISABLED);
        }
        else {
            mPauseTime = mSeekTimeUs + getTimeStamp(A2DP_ENABLED);
        }
        if (mAudioSink.get() != NULL)
            mAudioSink->pause();
    } else {
        if (!mIsA2DPEnabled) {
            if(!mPauseEventPending) {
                ALOGV("Posting an event for Pause timeout");
                mQueue.postEventWithDelay(mPauseEvent, LPA_PAUSE_TIMEOUT_USEC);
                mPauseEventPending = true;
            }
            mPauseTime = mSeekTimeUs + getTimeStamp(A2DP_DISABLED);
        } else {
            mPauseTime = mSeekTimeUs + getTimeStamp(A2DP_ENABLED);
        }
        if (mAudioSink.get() != NULL) {
            ALOGV("AudioSink pause");
            mAudioSink->pause();
        }
    }
}

void LPAPlayer::resume() {
    ALOGV("resume: mPaused %d",mPaused);
    Mutex::Autolock autoLock(mResumeLock);
    if ( mPaused) {
        CHECK(mStarted);
        if (!mIsA2DPEnabled) {
            if(mPauseEventPending) {
                ALOGV("Resume(): Cancelling the puaseTimeout event");
                mPauseEventPending = false;
                mQueue.cancelEvent(mPauseEvent->eventID());
            }

        }

        if (!mIsAudioRouted) {
            audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_LPA |
                                                                AUDIO_OUTPUT_FLAG_DIRECT);
            status_t err = mAudioSink->open(
                mSampleRate, numChannels, mChannelMask, AUDIO_FORMAT_PCM_16_BIT,
                DEFAULT_AUDIOSINK_BUFFERCOUNT,
                &LPAPlayer::AudioSinkCallback,
                this,
                (mA2DPEnabled ?  AUDIO_OUTPUT_FLAG_NONE : flags ));
            if (err != NO_ERROR) {
                ALOGE("Audio sink open failed.");
            }
            mIsAudioRouted = true;
        }
        mPaused = false;
        mAudioSink->start();
        pthread_cond_signal(&decoder_cv);
    }
}

//static
size_t LPAPlayer::AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie) {
    if (buffer == NULL && size == AudioTrack::EVENT_UNDERRUN) {
        LPAPlayer *me = (LPAPlayer *)cookie;
        me->mReachedEOS = true;
        me->mReachedOutputEOS = true;
        ALOGV("postAudioEOS");
        me->mObserver->postAudioEOS(0);
    }
    return 1;
}

void LPAPlayer::reset() {

    ALOGV("Reset");
    // Close the audiosink after all the threads exited to make sure
    mReachedEOS = true;

    // make sure Decoder thread has exited
    ALOGV("Closing all the threads");
    requestAndWaitForDecoderThreadExit();
    requestAndWaitForA2DPNotificationThreadExit();

    ALOGV("Close the Sink");
    if (mIsAudioRouted) {
	    mAudioSink->stop();
        mAudioSink->close();
        mAudioSink.clear();
    }
    // Make sure to release any buffer we hold onto so that the
    // source is able to stop().
    if (mFirstBuffer != NULL) {
        mFirstBuffer->release();
        mFirstBuffer = NULL;
    }

    if (mInputBuffer != NULL) {
        ALOGV("AudioPlayer releasing input buffer.");
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    mSource->stop();

    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    wp<MediaSource> tmp = mSource;
    mSource.clear();
    while (tmp.promote() != NULL) {
        usleep(1000);
    }

    mPositionTimeMediaUs = -1;
    mPositionTimeRealUs = -1;
    mSeeking = false;
    mReachedEOS = false;
    mReachedOutputEOS = false;
    mFinalStatus = OK;
    mStarted = false;
}


bool LPAPlayer::isSeeking() {
    Mutex::Autolock autoLock(mLock);
    return mSeeking;
}

bool LPAPlayer::reachedEOS(status_t *finalStatus) {
    *finalStatus = OK;
    Mutex::Autolock autoLock(mLock);
    *finalStatus = mFinalStatus;
    return mReachedOutputEOS;
}


void *LPAPlayer::decoderThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->decoderThreadEntry();
    return NULL;
}


void LPAPlayer::decoderThreadEntry() {

    pthread_mutex_lock(&decoder_mutex);

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"LPA DecodeThread", 0, 0, 0);

    ALOGV("decoderThreadEntry wait for signal \n");
    if (!mStarted) {
        pthread_cond_wait(&decoder_cv, &decoder_mutex);
    }
    ALOGV("decoderThreadEntry ready to work \n");
    pthread_mutex_unlock(&decoder_mutex);
    if (killDecoderThread) {
        return;
    }
    void* local_buf = malloc(MEM_BUFFER_SIZE);
    int bytesWritten = 0;
    while (!killDecoderThread) {

        if (mReachedEOS || mPaused || !mIsAudioRouted) {
            pthread_mutex_lock(&decoder_mutex);
            pthread_cond_wait(&decoder_cv, &decoder_mutex);
            pthread_mutex_unlock(&decoder_mutex);
            continue;
        }

        if (!mIsA2DPEnabled) {
            ALOGV("FillBuffer: MemBuffer size %d", MEM_BUFFER_SIZE);
            ALOGV("Fillbuffer started");
            //TODO: Add memset
            bytesWritten = fillBuffer(local_buf, MEM_BUFFER_SIZE);
            ALOGV("FillBuffer completed bytesToWrite %d", bytesWritten);

            if(!killDecoderThread) {
                mAudioSink->write(local_buf, bytesWritten);
            }
        }
    }

    free(local_buf);

    //TODO: Call fillbuffer with different size and write to mAudioSink()
}

void *LPAPlayer::A2DPNotificationThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->A2DPNotificationThreadEntry();
    return NULL;
}

void LPAPlayer::A2DPNotificationThreadEntry() {
    while (1) {
        pthread_mutex_lock(&a2dp_notification_mutex);
        pthread_cond_wait(&a2dp_notification_cv, &a2dp_notification_mutex);
        pthread_mutex_unlock(&a2dp_notification_mutex);
        if (killA2DPNotificationThread) {
            break;
        }

        ALOGV("A2DP notification has come mIsA2DPEnabled: %d", mIsA2DPEnabled);

        if (mIsA2DPEnabled) {
            //TODO:
        }
        else {
        //TODO
        }
    }
    a2dpNotificationThreadAlive = false;
    ALOGV("A2DPNotificationThread is dying");

}

void LPAPlayer::createThreads() {

    //Initialize all the Mutexes and Condition Variables
    pthread_mutex_init(&decoder_mutex, NULL);
    pthread_mutex_init(&a2dp_notification_mutex, NULL);
    pthread_cond_init (&decoder_cv, NULL);
    pthread_cond_init (&a2dp_notification_cv, NULL);

    // Create 4 threads Effect, decoder, event and A2dp
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    killDecoderThread = false;
    killA2DPNotificationThread = false;

    decoderThreadAlive = true;
    a2dpNotificationThreadAlive = true;

    ALOGV("Creating decoder Thread");
    pthread_create(&decoderThread, &attr, decoderThreadWrapper, this);

    ALOGV("Creating A2DP Notification Thread");
    pthread_create(&A2DPNotificationThread, &attr, A2DPNotificationThreadWrapper, this);

    pthread_attr_destroy(&attr);
}

size_t LPAPlayer::fillBuffer(void *data, size_t size) {

    if (mReachedEOS) {
        return 0;
    }

    bool postSeekComplete = false;

    size_t size_done = 0;
    size_t size_remaining = size;
    while (size_remaining > 0) {
        MediaSource::ReadOptions options;

        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking) {
                mInternalSeeking = false;
            }
            if (mSeeking || mInternalSeeking) {
                if (mIsFirstBuffer) {
                    if (mFirstBuffer != NULL) {
                        mFirstBuffer->release();
                        mFirstBuffer = NULL;
                    }
                    mIsFirstBuffer = false;
                }

                options.setSeekTo(mSeekTimeUs);

                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                size_remaining = size;
                size_done = 0;

                mSeeking = false;
                if (mObserver && !mInternalSeeking) {
                    ALOGV("fillBuffer: Posting audio seek complete event");
                    postSeekComplete = true;
                }
                mInternalSeeking = false;
            }
        }

        if (mInputBuffer == NULL) {
            status_t err;

            if (mIsFirstBuffer) {
                mInputBuffer = mFirstBuffer;
                mFirstBuffer = NULL;
                err = mFirstBufferResult;

                mIsFirstBuffer = false;
            } else {
                err = mSource->read(&mInputBuffer, &options);
            }

            CHECK((err == OK && mInputBuffer != NULL)
                   || (err != OK && mInputBuffer == NULL));

            Mutex::Autolock autoLock(mLock);

            if (err != OK) {
                mReachedEOS = true;
                mFinalStatus = err;
                break;
            }

            CHECK(mInputBuffer->meta_data()->findInt64(
                        kKeyTime, &mPositionTimeMediaUs));
            mFrameSize = mAudioSink->frameSize();
            mPositionTimeRealUs =
                ((mNumFramesPlayed + size_done / mFrameSize) * 1000000)
                    / mSampleRate;
        }
        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;

            continue;
        }

        size_t copy = size_remaining;
        if (copy > mInputBuffer->range_length()) {
            copy = mInputBuffer->range_length();
        }

        memcpy((char *)data + size_done,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               copy);

        mInputBuffer->set_range(mInputBuffer->range_offset() + copy,
                                mInputBuffer->range_length() - copy);

        size_done += copy;
        size_remaining -= copy;
    }

    if (postSeekComplete) {
        mObserver->postAudioSeekComplete();
    }

    return size_done;
}

int64_t LPAPlayer::getRealTimeUs() {
    Mutex::Autolock autoLock(mLock);
    return getRealTimeUsLocked();
}


int64_t LPAPlayer::getRealTimeUsLocked(){
    //Used for AV sync: irrelevant API for LPA.
    return 0;
}

int64_t LPAPlayer::getTimeStamp(A2DPState state) {
    uint64_t timestamp = 0;
    switch (state) {
    case A2DP_ENABLED:
    case A2DP_DISCONNECT:
        ALOGV("Get timestamp for A2DP");
        break;
    case A2DP_DISABLED:
    case A2DP_CONNECT: {
        mAudioSink->getTimeStamp(&timestamp);
        break;
    }
    default:
        break;
    }
    ALOGV("timestamp %lld ", timestamp);
    return timestamp;
}

int64_t LPAPlayer::getMediaTimeUs() {
    Mutex::Autolock autoLock(mLock);
    ALOGV("getMediaTimeUs() mPaused %d mSeekTimeUs %lld mPauseTime %lld", mPaused, mSeekTimeUs, mPauseTime);
    if (mPaused) {
        return mPauseTime;
    } else {
        A2DPState state = mIsA2DPEnabled ? A2DP_ENABLED : A2DP_DISABLED;
        return (mSeekTimeUs + getTimeStamp(state));
    }
}

bool LPAPlayer::getMediaTimeMapping(
                                   int64_t *realtime_us, int64_t *mediatime_us) {
    Mutex::Autolock autoLock(mLock);

    *realtime_us = mPositionTimeRealUs;
    *mediatime_us = mPositionTimeMediaUs;

    return mPositionTimeRealUs != -1 && mPositionTimeMediaUs != -1;
}

void LPAPlayer::requestAndWaitForDecoderThreadExit() {

    if (!decoderThreadAlive)
        return;
    killDecoderThread = true;

    /* Flush the audio sink to unblock the decoder thread
       if any write to audio HAL is blocked */
    if (!mReachedOutputEOS && mIsAudioRouted)
        mAudioSink->flush();

    pthread_cond_signal(&decoder_cv);
    pthread_join(decoderThread,NULL);
    ALOGV("decoder thread killed");

}

void LPAPlayer::requestAndWaitForA2DPNotificationThreadExit() {
    if (!a2dpNotificationThreadAlive)
        return;
    killA2DPNotificationThread = true;
    pthread_cond_signal(&a2dp_notification_cv);
    pthread_join(A2DPNotificationThread,NULL);
    ALOGV("a2dp notification thread killed");
}

void LPAPlayer::onPauseTimeOut() {
    ALOGV("onPauseTimeOut");
    Mutex::Autolock autoLock(mResumeLock);
    if (!mPauseEventPending) {
        return;
    }
    mPauseEventPending = false;
    if(!mIsA2DPEnabled) {
        // 1.) Set seek flags
        mReachedEOS = false;
        mReachedOutputEOS = false;
        if(mSeeking == false) {
            mSeekTimeUs += getTimeStamp(A2DP_DISABLED);
            mInternalSeeking = true;
        } else {
            //do not update seek time if user has already seeked
            // to a new position
            // also seek has to be posted back to player,
            // so do not set mInternalSeeking flag
            ALOGV("do not update seek time %lld ", mSeekTimeUs);
        }
        ALOGV("newseek time = %lld ", mSeekTimeUs);
        // 2.) Close routing Session
        mAudioSink->close();
        mIsAudioRouted = false;
    }

}

} //namespace android
