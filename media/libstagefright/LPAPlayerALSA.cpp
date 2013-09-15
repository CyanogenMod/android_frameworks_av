/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
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

//#define LOG_NDDEBUG 0
//#define LOG_NDEBUG 0
#define LOG_TAG "LPAPlayerALSA"

#include <utils/Log.h>
#include <utils/threads.h>

#include <signal.h>
#include <sys/prctl.h>
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
#include "include/AwesomePlayer.h"

static const char   mName[] = "LPAPlayer";

#define MEM_METADATA_SIZE 64
#ifndef LPA_DEFAULT_BUFFER_SIZE
#define LPA_DEFAULT_BUFFER_SIZE 256
#endif
#define MEM_BUFFER_SIZE ((LPA_DEFAULT_BUFFER_SIZE*1024) - MEM_METADATA_SIZE)
#define MEM_BUFFER_COUNT 4
#define LPA_BUFFER_TIME 1500000

namespace android {
int LPAPlayer::mObjectsAlive = 0;

LPAPlayer::LPAPlayer(
                    const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                    AwesomePlayer *observer)
:AudioPlayer(audioSink,observer),
mPositionTimeMediaUs(-1),
mPositionTimeRealUs(-1),
mInternalSeeking(false),
mStarted(false),
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
    mObjectsAlive++;
    mNumOutputChannels =0;
    mNumInputChannels = 0;
    mPaused = false;
    /* Initialize Suspend/Resume related variables */
    mQueue.start();
    mQueueStarted      = true;
    mPauseEvent        = new TimedEvent(this, &LPAPlayer::onPauseTimeOut);
    mPauseEventPending = false;
    ALOGV("Registering client with AudioFlinger");

    mIsAudioRouted = false;

    initCheck = true;

}

LPAPlayer::~LPAPlayer() {
    ALOGV("LPAPlayer::~LPAPlayer()");
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();

    mObjectsAlive--;
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

    //Create decoder thread and initialize all the
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

    success = format->findInt32(kKeyChannelCount, &mNumInputChannels);
    CHECK(success);

    // Always produce stereo output
    mNumOutputChannels = 2;

    if(!format->findInt32(kKeyChannelMask, &mChannelMask)) {
        // log only when there's a risk of ambiguity of channel mask selection
        ALOGI_IF(mNumInputChannels > 2,
                "source format didn't specify channel mask, using (%d) channel order", mNumInputChannels);
        mChannelMask = CHANNEL_MASK_USE_CHANNEL_ORDER;
    }
    audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_LPA |
                                                         AUDIO_OUTPUT_FLAG_DIRECT);
    ALOGV("mAudiosink->open() mSampleRate %d, numOutputChannels %d, mChannelMask %d, flags %d",mSampleRate,
          mNumOutputChannels, mChannelMask, flags);
    err = mAudioSink->open(
        mSampleRate, mNumOutputChannels, mChannelMask, AUDIO_FORMAT_PCM_16_BIT,
        DEFAULT_AUDIOSINK_BUFFERCOUNT,
        &LPAPlayer::AudioSinkCallback,
        this,
        flags);

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
    mLock.lock();
    mDecoderCv.signal();
    mLock.unlock();

    return OK;
}

status_t LPAPlayer::seekTo(int64_t time_us) {
    Mutex::Autolock autoLock(mLock);
    ALOGV("seekTo: time_us %lld", time_us);

    if (seekTooClose(time_us)) {
        mLock.unlock();
        mObserver->postAudioSeekComplete();
        mLock.lock();
        return OK;
    }

    mSeeking = true;
    mSeekTimeUs = time_us;
    mPauseTime = mSeekTimeUs;
    ALOGV("In seekTo(), mSeekTimeUs %lld",mSeekTimeUs);

    if (mIsAudioRouted) {
        mAudioSink->flush();
    }

    if (mReachedEOS) {
        mReachedEOS = false;
        mReachedOutputEOS = false;
        if(mPaused == false) {
            ALOGV("Going to signal decoder thread since playback is already going on ");
            mDecoderCv.signal();
            ALOGV("Signalled extractor thread.");
        }
    }
    ALOGV("seek done.");
    return OK;
}

void LPAPlayer::pause(bool playPendingSamples) {
    CHECK(mStarted);
    if (mPaused) {
        return;
    }
    ALOGV("pause: playPendingSamples %d", playPendingSamples);
    mPaused = true;
    if (!mPauseEventPending) {
        ALOGV("Posting an event for Pause timeout");
        mQueue.postEventWithDelay(mPauseEvent, LPA_PAUSE_TIMEOUT_USEC);
        mPauseEventPending = true;
    }
    mPauseTime = mSeekTimeUs + getTimeStamp();

    if (mAudioSink.get() != NULL) {
        ALOGV("AudioSink pause");
        mAudioSink->pause();
    }
}

void LPAPlayer::resume() {
    ALOGV("resume: mPaused %d",mPaused);
    Mutex::Autolock autoLock(mLock);
    if ( mPaused) {
        CHECK(mStarted);
        if(mPauseEventPending) {
            ALOGV("Resume(): Cancelling the puaseTimeout event");
            mPauseEventPending = false;
            mQueue.cancelEvent(mPauseEvent->eventID());
        }

        if (!mIsAudioRouted) {
            audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_LPA |
                                                                AUDIO_OUTPUT_FLAG_DIRECT);
            status_t err = mAudioSink->open(
                mSampleRate, mNumOutputChannels, mChannelMask, AUDIO_FORMAT_PCM_16_BIT,
                DEFAULT_AUDIOSINK_BUFFERCOUNT,
                &LPAPlayer::AudioSinkCallback,
                this,
                flags);
            if (err != NO_ERROR) {
                ALOGE("Audio sink open failed.");
            }
            mIsAudioRouted = true;
        }
        mPaused = false;
        mAudioSink->start();
        mDecoderCv.signal();
    }
}

//static
size_t LPAPlayer::AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie) {
    if (buffer == NULL && size == AudioTrack::EVENT_UNDERRUN) {
        LPAPlayer *me = (LPAPlayer *)cookie;
        if(me->mReachedEOS == true) {
            //in the case of seek all these flags will be reset
            me->mReachedOutputEOS = true;
            ALOGV("postAudioEOS mSeeking %d", me->mSeeking);
            me->mObserver->postAudioEOS(0);
        }else {
            ALOGV("postAudioEOS ignored since %d", me->mSeeking);
        }
    }
    return 1;
}

void LPAPlayer::reset() {
    ALOGD("Reset");

    Mutex::Autolock _l(mLock); //to sync w/ onpausetimeout

    //cancel any pending onpause timeout events
    //doesnt matter if the event is really present or not
    mPauseEventPending = false;
    mQueue.cancelEvent(mPauseEvent->eventID());

    // Close the audiosink after all the threads exited to make sure
    mReachedEOS = true;

    // make sure Decoder thread has exited
    ALOGD("Closing all the threads");
    requestAndWaitForDecoderThreadExit_l();

    ALOGD("Close the Sink");
    if (mIsAudioRouted) {
        mAudioSink->stop();
        mAudioSink->close();
        mAudioSink.clear();
        mIsAudioRouted = false;
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

    mLock.lock();
    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"LPA DecodeThread", 0, 0, 0);

    ALOGV("mDecoderThreadEntry wait for signal \n");
    if (!mStarted) {
        mDecoderCv.wait(mLock);
    }
    ALOGV("decoderThreadEntry ready to work \n");
    mLock.unlock();
    if (mKillDecoderThread) {
        return;
    }
    void* local_buf = malloc(MEM_BUFFER_SIZE);
    if(local_buf == (void*) NULL) {
        mKillDecoderThread = true;
        ALOGE("Malloc failed");
        return;
    }
    int *lptr = ((int*)local_buf);
    int bytesWritten = 0;

    if (!local_buf) {
      ALOGE("Failed to allocate temporary buffer for mDecoderThread");
      return;
    }

    bool lSeeking = false;
    bool lPaused = false;

    while (!mKillDecoderThread) {

        if (mReachedEOS || mPaused || !mIsAudioRouted) {
            ALOGV("Going to sleep before write since "
                  "mReachedEOS %d, mPaused %d, mIsAudioRouted %d",
                  mReachedEOS, mPaused, mIsAudioRouted);
            mDecoderMutex.lock();
            mDecoderCv.wait(mDecoderMutex);
            mDecoderMutex.unlock();
            ALOGV("Woke up from sleep before write since "
                  "mReachedEOS %d, mPaused %d, mIsAudioRouted %d",
                  mReachedEOS, mPaused, mIsAudioRouted);
            continue;
        }

        ALOGV("FillBuffer: MemBuffer size %d", MEM_BUFFER_SIZE);
        ALOGV("Fillbuffer started");
        if (mNumInputChannels == 1) {
            bytesWritten = fillBuffer(local_buf, MEM_BUFFER_SIZE/2);
            CHECK(bytesWritten <= MEM_BUFFER_SIZE/2);

            convertMonoToStereo((int16_t*)local_buf, bytesWritten);
            bytesWritten *= 2;
        } else {
            bytesWritten = fillBuffer(local_buf, MEM_BUFFER_SIZE);
            CHECK(bytesWritten <= MEM_BUFFER_SIZE);
        }

        ALOGV("FillBuffer completed bytesToWrite %d", bytesWritten);
        if(!mKillDecoderThread) {
            mLock.lock();
            lPaused = mPaused;
            mLock.unlock();

            if(lPaused == true) {
                //write only if player is not in paused state. Sleep on lock
                // resume is called
                ALOGV("Going to sleep in decodethreadiwrite since sink is paused");
                mDecoderMutex.lock();
                mDecoderCv.wait(mDecoderMutex);
                ALOGV("Going to unlock n decodethreadwrite since sink "
                      "resumed mPaused %d, mIsAudioRouted %d, mReachedEOS %d",
                      mPaused, mIsAudioRouted, mReachedEOS);
                mDecoderMutex.unlock();
            }
            mLock.lock();
            lSeeking = mSeeking||mInternalSeeking;
            mLock.unlock();

            if(lSeeking == false && (mKillDecoderThread == false)){
                //if we are seeking, ignore write, otherwise write
                ALOGV("Fillbuffer before seeling flag %d", mSeeking);
                int lWrittenBytes = mAudioSink->write(local_buf, bytesWritten);
                ALOGV("Fillbuffer after write, written bytes %d and seek flag %d", lWrittenBytes, mSeeking);
            } else {
                ALOGV("Fillbuffer ignored since we seeked after fillBuffer was set %d", mSeeking);
            }
        }
    }

    free(local_buf);

    //TODO: Call fillbuffer with different size and write to mAudioSink()
}

void LPAPlayer::createThreads() {

    // Create 4 threads Effect, decoder, event and A2dp
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    mKillDecoderThread = false;

    mDecoderThreadAlive = true;

    ALOGV("Creating decoder Thread");
    pthread_create(&mDecoderThread, &attr, decoderThreadWrapper, this);

    pthread_attr_destroy(&attr);
}

size_t LPAPlayer::fillBuffer(void *data, size_t size) {

    if (mReachedEOS) {
        return 0;
    }

    if ((data == (void*) NULL) || size > MEM_BUFFER_SIZE) {
        ALOGE("fillBuffer given wrong buffer");
        return 0;
    }

    bool postSeekComplete = false;

    size_t size_done = 0;
    size_t size_remaining = size;
    ALOGV("fillBuffer: Clearing seek flag in fill buffer");

    while (size_remaining > 0) {
        MediaSource::ReadOptions options;

        {
            Mutex::Autolock autoLock(mLock);

            if(mSeeking) {
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

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                size_remaining = size;

                mSeeking = false;
                if (mObserver && !mInternalSeeking) {
                    ALOGV("fillBuffer: Posting audio seek complete event");
                    postSeekComplete = true;
                }
                mInternalSeeking = false;
                ALOGV("fillBuffer: Setting seek flag in fill buffer");
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
            {
                Mutex::Autolock autoLock(mLock);

                if (err != OK) {
                    ALOGD("fill buffer - reached eos true");
                    mReachedEOS = true;
                    mFinalStatus = err;
                    break;
                }
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
        } //is size_remaining < range_length impossible?

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

int64_t LPAPlayer::getTimeStamp() {
    uint64_t timestamp = 0;
    mAudioSink->getTimeStamp(&timestamp);
    ALOGV("timestamp %lld ", timestamp);
    return timestamp;
}

int64_t LPAPlayer::getMediaTimeUs_l() {
    if (mPaused) {
        return mPauseTime;
    } else {
        return (mSeekTimeUs + getTimeStamp());
    }
}

int64_t LPAPlayer::getMediaTimeUs() {
    Mutex::Autolock autoLock(mLock);
    ALOGV("getMediaTimeUs() mPaused %d mSeekTimeUs %lld mPauseTime %lld", mPaused, mSeekTimeUs, mPauseTime);
    return getMediaTimeUs_l();
}

bool LPAPlayer::getMediaTimeMapping(
                                   int64_t *realtime_us, int64_t *mediatime_us) {
    ALOGE("getMediaTimeMapping is invalid for LPA Player");
    *realtime_us = -1;
    *mediatime_us = -1;
    return false;
}

bool LPAPlayer::seekTooClose(int64_t time_us) {
    int64_t t1 = getMediaTimeUs_l();
    /*
     * empirical
     * -----------
     * This constant signifies how much data (in Us) has been rendered by the
     * DSP in the interval between the moment flush is issued on AudioSink to
     * after ioctl(PAUSE) returns in Audio HAL. (flush triggers an implicit
     * pause in audio HAL)
     *
     */
     const int64_t kDeltaUs = 60000LL; /* 60-70ms on msm8974, must be measured for other targets */
     t1 += kDeltaUs;
     return (time_us > t1) && ((time_us - t1) <= LPA_BUFFER_TIME);
}

//lock taken in reset()
void LPAPlayer::requestAndWaitForDecoderThreadExit_l() {

    if (!mDecoderThreadAlive)
        return;
    mKillDecoderThread = true;

    /* Flush the audio sink to unblock the decoder thread
       if any write to audio HAL is blocked */
    if (!mReachedOutputEOS && mIsAudioRouted)
        mAudioSink->flush();

    mDecoderCv.signal();
    mLock.unlock();
    pthread_join(mDecoderThread,NULL);
    mLock.lock();
    ALOGD("decoder thread killed");

}

void LPAPlayer::onPauseTimeOut() {
    ALOGV("onPauseTimeOut");
    Mutex::Autolock autoLock(mLock);
    if (!mPauseEventPending) {
        return;
    }
    mPauseEventPending = false;
    // 1.) Set seek flags
    mReachedEOS = false;
    mReachedOutputEOS = false;
    if(mSeeking == false) {
        mSeekTimeUs += getTimeStamp();
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
    mAudioSink->flush();
    mAudioSink->stop();
    mAudioSink->close();
    mIsAudioRouted = false;
}

//dup each mono frame
void LPAPlayer::convertMonoToStereo(int16_t *data, size_t size)
{
    int i =0;
    int16_t *start_pointer = data;
    int monoFrameCount = (size) / (sizeof(int16_t));

    for (i = monoFrameCount; i > 0 ; i--) {
      int16_t temp_sample = *(start_pointer + i - 1);
      *(start_pointer + (i*2) - 1) = temp_sample;
      *(start_pointer + (i*2) - 2) = temp_sample;
    }
}

} //namespace android
