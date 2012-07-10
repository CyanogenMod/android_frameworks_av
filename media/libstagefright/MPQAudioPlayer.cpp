/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
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

#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#define LOG_TAG "MPQAudioPlayer"
#include <utils/Log.h>
#include <utils/threads.h>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/eventfd.h>

#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>
#include <system/audio.h>
#include "include/ESDS.h"

extern "C" {
    #include <asound.h>
    #include "alsa_audio.h"
    #include <compress_params.h>
    #include <compress_offload.h>
}
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MPQAudioPlayer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <hardware_legacy/power.h>

#include <linux/unistd.h>
//#include <include/linux/msm_audio.h>

#include "include/AwesomePlayer.h"
#include <powermanager/PowerManager.h>
static const char   mName[] = "MPQAudioPlayer";


//Session Id to diff playback
#define MPQ_AUDIO_SESSION_ID 3
#define TUNNEL_SESSION_ID 2

#define RENDER_LATENCY 24000
#define AAC_AC3_BUFFER_SIZE 32768

namespace android {

int MPQAudioPlayer::mMPQAudioObjectsAlive = 0;

int MPQAudioPlayer::getMPQAudioObjectsAlive(/*bool hasVideo*/) {
    ALOGV("getMPQAudioObjectsAlive = %d",mMPQAudioObjectsAlive);
    return mMPQAudioObjectsAlive;
}

MPQAudioPlayer::MPQAudioPlayer(
                    const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                    AwesomePlayer *observer, bool hasVideo)
:AudioPlayer(audioSink,observer),
mInputBuffer(NULL),
mSampleRate(0),
mNumChannels(0),
mFrameSize(0),
mNumFramesPlayed(0),
mIsAACFormatAdif(0),
mLatencyUs(0),
mStarted(false),
mAsyncReset(false),
mPositionTimeMediaUs(-1),
mPositionTimeRealUs(-1),
mSeeking(false),
mInternalSeeking(false),
mPostedEOS(false),
mReachedExtractorEOS(false),
mFinalStatus(OK),
mIsPaused(false),
mPlayPendingSamples(false),
mSourcePaused(false),
mAudioSinkOpen(false),
mIsAudioRouted(false),
mIsFirstBuffer(false),
mFirstBufferResult(OK),
mFirstBuffer(NULL),
mAudioSink(audioSink),
mA2DPEnabled(false),
mObserver(observer) {
    ALOGV("MPQAudioPlayer::MPQAudioPlayer()");

    mAudioFlinger = NULL;
    if(mAudioSink->getSessionId())
        mMPQAudioObjectsAlive++;

    mQueue.start();
    mQueueStarted      = true;
    mPauseEvent        = new MPQAudioEvent(this, &MPQAudioPlayer::onPauseTimeOut);
    mPauseEventPending = false;
    mSourcePaused = false;

    getAudioFlinger();
    ALOGD("Registering client with AudioFlinger");

    mMimeType.setTo("");

    mTimePaused  = 0;
    mDurationUs = 0;
    mSeekTimeUs = 0;
    mTimeout = -1;
    mPostEOSDelayUs = 0;

    mLocalBuf = NULL;
    mInputBufferSize =  0;

    mFirstEncodedBuffer = false;
    mHasVideo = hasVideo;
    initCheck = true;
    mDeathRecipient = new PMDeathRecipient(this);
}

void MPQAudioPlayer::acquireWakeLock()
{
/*    Mutex::Autolock _l(pmLock);

    if (mPowerManager == 0) {
        // use checkService() to avoid blocking if power service is not up yet
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            ALOGW("Thread %s cannot connect to the power manager service", mName);
        } else {
            mPowerManager = interface_cast<IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
    if (mPowerManager != 0 && mWakeLockToken == 0) {
        sp<IBinder> binder = new BBinder();
        status_t status = mPowerManager->acquireWakeLock(POWERMANAGER_PARTIAL_WAKE_LOCK,
                                                         binder,
                                                         String16(mName));
        if (status == NO_ERROR) {
            mWakeLockToken = binder;
        }
        ALOGV("acquireWakeLock() %s status %d", mName, status);
    }
    */
}

void MPQAudioPlayer::releaseWakeLock()
{
/*
    Mutex::Autolock _l(pmLock);

    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock() %s", mName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLock(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
*/
}

void MPQAudioPlayer::clearPowerManager()
{
    Mutex::Autolock _l(pmLock);
    releaseWakeLock();
    mPowerManager.clear();
}

void MPQAudioPlayer::PMDeathRecipient::binderDied(const wp<IBinder>& who)
{
    parentClass->clearPowerManager();
    ALOGW("power manager service died !!!");
}

MPQAudioPlayer::~MPQAudioPlayer() {
    ALOGD("MPQAudioPlayer::~MPQAudioPlayer()");
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();
    if(mMPQAudioObjectsAlive > 0)
        mMPQAudioObjectsAlive--;

    releaseWakeLock();
    if (mPowerManager != 0) {
        sp<IBinder> binder = mPowerManager->asBinder();
        binder->unlinkToDeath(mDeathRecipient);
    }
}

void MPQAudioPlayer::getAudioFlinger() {

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
        mAudioFlinger = interface_cast<IAudioFlinger>(binder);
    }
    ALOGE_IF(mAudioFlinger==0, "no AudioFlinger!?");
}

void MPQAudioPlayer::setSource(const sp<MediaSource> &source) {
    CHECK(mSource == NULL);
    ALOGD("Setting source from MPQ Audio Player");
    mSource = source;
}

status_t MPQAudioPlayer::start(bool sourceAlreadyStarted) {
    Mutex::Autolock autoLock(mLock);
    CHECK(!mStarted);
    CHECK(mSource != NULL);

    ALOGV("start: sourceAlreadyStarted %d", sourceAlreadyStarted);
    //Check if the source is started, start it
    status_t err = OK;
    if (!sourceAlreadyStarted) {
        err = mSource->start();
        if (err != OK) {
            return err;
        }
    }

    err = updateMetaDataInformation();
    if(err != OK) {
        ALOGE("updateMetaDataInformation = %d", err);
        return err;
    }

    err = getDecoderAndFormat();
    if(err != OK) {
        ALOGV("getDecoderAndFormat return err = %d",err);
        return err;
    }

    //Create event, extractor and initialize all the
    //mutexes and coditional variables
    ALOGV("Creat threads ++");
    createThreads();
    ALOGV("All Threads Created.");

    sp<MetaData> format = mSource->getFormat();
    if(!format->findInt32(kKeyChannelMask, &mChannelMask)) {
        // log only when there's a risk of ambiguity of channel mask selection
        ALOGI_IF(mNumChannels > 2,
                "source format didn't specify channel mask, using (%d) channel order", mNumChannels);
        mChannelMask = CHANNEL_MASK_USE_CHANNEL_ORDER;
    }
    ALOGD("Opening a routing session for audio playback:\
            mSampleRate = %d mNumChannels =  %d",\
            mSampleRate, mNumChannels);
    audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_LPA |
                                                        AUDIO_OUTPUT_FLAG_DIRECT);
    err = mAudioSink->open(
        mSampleRate, mNumChannels, mChannelMask, (audio_format_t)mAudioFormat,
        DEFAULT_AUDIOSINK_BUFFERCOUNT,
        &MPQAudioPlayer::postEOS,
        this,
        (mA2DPEnabled ?  AUDIO_OUTPUT_FLAG_NONE : flags ));
    if (err != OK) {
        //TODO: Release first buffer if it is being handled
        if (!sourceAlreadyStarted) {
            mSource->stop();
        }
        ALOGE("Opening a routing session failed");
        return err;
    }
    acquireWakeLock();
    mIsAudioRouted = true;
    err = configurePCM();
    if (err) {
        ALOGE("Error Configuring PCM");
        return err;
    }
    mAudioSink->start();
    ALOGD(" MPQ Audio Driver Started");
    mStarted = true;

    ALOGD("Waking up extractor thread");
    mExtractorCv.signal();

    return OK;
}


status_t MPQAudioPlayer::seekTo(int64_t time_us) {
    Mutex::Autolock autoLock1(mSeekLock);
    Mutex::Autolock autoLock(mLock);

    status_t err = OK;
    ALOGD("seekTo: time_us %lld", time_us);
    if (mReachedExtractorEOS) {
        mReachedExtractorEOS = false;
        mPostedEOS = false;
        mTimeout = -1;
    }
    mSeeking = true;
    mSeekTimeUs = time_us;

    err = seekPlayback();

    if(err)
        ALOGE("seek returned error = %d",err);
    return err;
}

status_t MPQAudioPlayer::seekPlayback() {

    status_t err = OK;
    //Just give the buffer from new location
    mPositionTimeRealUs = mPositionTimeMediaUs = -1;
    mNumFramesPlayed = 0;
    mTimePaused = 0;
    if (mStarted) {
        if(!mIsAACFormatAdif) {
            mAudioSink->flush();
            if(err != OK) {
                ALOGE("flush returned error =%d",err);
                return err;
            }
            if (!mIsPaused)
                mExtractorCv.signal();
        }
    }
    return OK;
}

void MPQAudioPlayer::pause(bool playPendingSamples) {

    Mutex::Autolock autolock(mLock);
    status_t err = OK;
    CHECK(mStarted);

    ALOGD("Pause: playPendingSamples %d", playPendingSamples);
    mPlayPendingSamples = playPendingSamples;
    mIsPaused = true;
    bool bIgnorePendingSamples = false;
    switch(mDecoderType) {

        case ESoftwareDecoder:
            if(mAudioSink->getSessionId()) {
                err = pausePlayback(bIgnorePendingSamples);
                CHECK(mSource != NULL);
                if ((mSource->pause()) == OK)
                    mSourcePaused = true;
            }
        break;

        case EMS11Decoder:
            err = pausePlayback(bIgnorePendingSamples);
        break;

        case EHardwareDecoder:
            mTimeout  = -1;
            bIgnorePendingSamples = true;
            err = pausePlayback(bIgnorePendingSamples);
        break;

        default:
            ALOGE("Invalid Decoder Type - postEOS ");
            err =  BAD_VALUE;
        break;
    }

    if(err != OK) {
        ALOGE("pause returnded err = %d",err);
        mFinalStatus = BAD_VALUE;
        if(mObserver) {
            mObserver->postAudioEOS();
        }
    }
}

status_t MPQAudioPlayer::pausePlayback(bool bIgnorePendingSamples) {

    status_t err = OK;
    if (mPlayPendingSamples && !bIgnorePendingSamples) {
        //Should be stop ideally
        //No pausing the driver. Allow the playback
        mNumFramesPlayed = 0;
    }
    else {
        mAudioSink->pause();
        if(err != OK) {
            ALOGE("Pause returned error =%d",err);
            return err;
        }

    }
    mTimePaused = mSeekTimeUs + getAudioTimeStampUs();
    return err;
}


void MPQAudioPlayer::resume() {

    Mutex::Autolock autoLock(mLock);
    status_t err = OK;
    CHECK(mStarted);
    CHECK(mSource != NULL);

    ALOGD("Resume: mIsPaused %d",mIsPaused);

    if (!mIsPaused)
        return;

    bool bIgnorePendingSamples = false;
    switch(mDecoderType) {

        case ESoftwareDecoder:
            if(mAudioSink->getSessionId()) {
                err = resumePlayback(MPQ_AUDIO_SESSION_ID, bIgnorePendingSamples);
                if(mSourcePaused)
                    mSource->start();
            }
        break;

        case EMS11Decoder:
            err = resumePlayback(MPQ_AUDIO_SESSION_ID, bIgnorePendingSamples);
        break;

        case EHardwareDecoder:
            bIgnorePendingSamples = true;
            err = resumePlayback(TUNNEL_SESSION_ID, bIgnorePendingSamples);
        break;

        default:
            ALOGE("Invalid Decoder Type - postEOS ");
            err =  BAD_VALUE;
        break;
    }

    if(err != OK) {
        ALOGE("resume returnded err = %d",err);
        mFinalStatus = BAD_VALUE;
        if(mObserver) {
            mObserver->postAudioEOS();
        }
        return;
    }

    mIsPaused = false;
    mExtractorCv.signal();

}

status_t MPQAudioPlayer::resumePlayback(int sessionId, bool bIgnorePendingSamples) {

    status_t err = OK;
    if (!mIsAudioRouted) {
        ALOGV("Opening a session for MPQ Audio playback - Software Decoder");
        audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_LPA |
                                                            AUDIO_OUTPUT_FLAG_DIRECT);
        status_t err = mAudioSink->open(
            mSampleRate, mNumChannels, mChannelMask, (audio_format_t)mAudioFormat,
            DEFAULT_AUDIOSINK_BUFFERCOUNT,
            &MPQAudioPlayer::postEOS,
            this,
            (mA2DPEnabled ?  AUDIO_OUTPUT_FLAG_NONE : flags ));
        if(err != OK) {
            ALOGE("openSession - resume = %d",err);
            return err;
        }
        acquireWakeLock();
        mIsAudioRouted = true;
    }

	mAudioSink->start();

    return err;
}

void MPQAudioPlayer::reset() {

    ALOGD("Reset called!!!!!");
    mAsyncReset = true;

    if (mAudioSink.get()) {
        mAudioSink->pause();
        ALOGV("Close the PCM Stream");
        mAudioSink->stop();
    }
    mAudioSink = NULL;

    // make sure Extractor thread has exited
    requestAndWaitForExtractorThreadExit();
    ALOGV("Extractor Thread killed");
    // make sure the event thread also has exited

    if(mAudioSink.get()) {
        mAudioSink->pause();
        ALOGV("Close the PCM Stream");
        mAudioSink->stop();
        mAudioSink = NULL;
    }

    // Close the audiosink after all the threads exited to make sure
    // there is no thread writing data to audio sink or applying effect
    if(mAudioSink.get() != NULL) {
        ALOGV("close session ++");
        mAudioSink->close();
        ALOGV("close session --");
        mIsAudioRouted =  false;
    }
    mAudioSink.clear();

    if (mFirstBuffer != NULL) {
        mFirstBuffer->release();
        mFirstBuffer = NULL;
    }

    if (mInputBuffer != NULL) {
        ALOGV("MPQ Audio Player releasing input buffer.");
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    mSource->stop();

    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    if(mDecoderType == ESoftwareDecoder) {
        wp<MediaSource> tmp = mSource;
        mSource.clear();
        while (tmp.promote() != NULL) {
           ALOGV("reset-sleep");
           usleep(1000);
        }
    }
    else {
        mSource.clear();
    }
    bufferDeAlloc();
    ALOGD("Buffer Deallocation complete!");
    mPositionTimeMediaUs = -1;
    mPositionTimeRealUs = -1;

    mSeeking = false;
    mInternalSeeking = false;

    mPostedEOS = false;
    mReachedExtractorEOS = false;
    mFinalStatus = OK;

    mIsPaused = false;
    mPauseEventPending = false;
    mPlayPendingSamples = false;

    mTimePaused  = 0;
    mDurationUs = 0;
    mSeekTimeUs = 0;
    mTimeout = -1;

    mNumChannels = 0;
    mMimeType.setTo("");
    mInputBuffer = NULL;

    mStarted = false;
}

bool MPQAudioPlayer::isSeeking() {
    Mutex::Autolock autoLock(mLock);
    return mSeeking;
}

bool MPQAudioPlayer::reachedEOS(status_t *finalStatus) {
    *finalStatus = OK;
    Mutex::Autolock autoLock(mLock);
    *finalStatus = mFinalStatus;
    return mPostedEOS;
}


void *MPQAudioPlayer::extractorThreadWrapper(void *me) {
    static_cast<MPQAudioPlayer *>(me)->extractorThreadEntry();
    return NULL;
}


void MPQAudioPlayer::extractorThreadEntry() {
    mExtractorMutex.lock();
    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"MPQ Audio DecodeThread", 0, 0, 0);
    ALOGV("extractorThreadEntry wait for signal \n");

    while (!mStarted && !mKillExtractorThread) {
        mExtractorCv.wait(mExtractorMutex);
    }

    ALOGV("extractorThreadEntry ready to work \n");
    mExtractorMutex.unlock();

    while (!mKillExtractorThread) {
        if (mDecoderType ==ESoftwareDecoder || mDecoderType == EMS11Decoder) {

            mExtractorMutex.lock();
            if(mPostedEOS || mIsPaused || mAsyncReset) {
                ALOGV("extractorThreadEntry: waiting on mExtractorCv");
                mExtractorCv.wait(mExtractorMutex);
                //TODO: Guess this should be removed plz verify
                mExtractorMutex.unlock();
                ALOGV("extractorThreadEntry: received a signal to wake upi - extractor mutex");
                continue;
            }
            mExtractorMutex.unlock();
            int bytesToWrite = 0;
            if(mDecoderType == EMS11Decoder)
                bytesToWrite = fillBuffer(mLocalBuf, AAC_AC3_BUFFER_SIZE);
            else
                bytesToWrite = fillBuffer(mLocalBuf, mInputBufferSize);
            ALOGV("fillBuffer returned size %d",bytesToWrite);
            if(mSeeking || mIsPaused)
                continue;
            //TODO : What if bytesWritetn is zero
            ALOGV("write - pcm  ++");
            if(mAudioSink.get() != NULL && bytesToWrite) {
                ssize_t bytesWritten = 0;
                if(mAudioFormat == AUDIO_FORMAT_AC3 ||
                            mAudioFormat == AUDIO_FORMAT_AAC ||
                            mAudioFormat == AUDIO_FORMAT_AAC_ADIF ||
                            mAudioFormat == AUDIO_FORMAT_EAC3) {
                    bytesWritten = mAudioSink->write(mLocalBuf, bytesToWrite);
                }
                else {
                     bytesWritten = mAudioSink->write(mLocalBuf, mInputBufferSize);
                }
                ALOGV("bytesWritten = %d",(int)bytesWritten);
            }
            else if(!mAudioSink->getSessionId()) {
                ALOGV("bytesToWrite = %d, mInputBufferSize = %d",\
                        bytesToWrite,mInputBufferSize);
                mAudioSink->write(mLocalBuf, bytesToWrite);
            }
            if(mObserver && mReachedExtractorEOS) {
                ALOGV("Posting EOS event..zero byte buffer ");
                //TODO : make it POST EOS to amke sense for  Software
                if(!mPostedEOS) {
                    if( mDecoderType == EMS11Decoder) {
                        memset(mLocalBuf, 0x0, AAC_AC3_BUFFER_SIZE);
                        mAudioSink->write(mLocalBuf, 0);
                    }
                    mObserver->postAudioEOS( mPostEOSDelayUs);
                    mPostedEOS = true;
                }
            }
            continue;
        }
        else if (mDecoderType == EHardwareDecoder) {
            mExtractorMutex.lock();
            if (mReachedExtractorEOS || mIsPaused || mAsyncReset ) {
                ALOGV("extractorThreadEntry: mIsPaused %d\
                        mReachedExtractorEOS %d mAsyncReset %d ",\
                        mIsPaused, mReachedExtractorEOS, mAsyncReset);
                ALOGV("extractorThreadEntry: waiting on mExtractorCv");
                mExtractorCv.wait(mExtractorMutex);
                //TODO: Guess this should be removed plz verify
                mExtractorMutex.unlock();
                ALOGV("extractorThreadEntry: received a signal to wake up");
                continue;
            }

            mExtractorMutex.unlock();
            ALOGV("Calling fillBuffer for size %d", mInputBufferSize);
            int bytesToWrite = fillBuffer(mLocalBuf, mInputBufferSize);
            ALOGV("fillBuffer returned size %d", bytesToWrite);
            if (mSeeking || mIsPaused ) {
                continue;
            }
            mAudioSink->write(mLocalBuf, bytesToWrite);
            if (!mInputBufferSize) {
                mInputBufferSize = mAudioSink->frameCount();
                ALOGD("mInputBufferSize = %d",mInputBufferSize);
                bufferAlloc(mInputBufferSize);
            }
            if (bytesToWrite <= 0)
                continue;
        }
        else  {
           ALOGE("Invalid Decoder Type in extractor thread");
           break;
        }
    }
    mExtractorThreadAlive = false;
    ALOGD("Extractor Thread is dying");

}
//static
size_t MPQAudioPlayer::postEOS(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie) {
    if (buffer == NULL && size == AudioTrack::EVENT_UNDERRUN) {
        MPQAudioPlayer *me = (MPQAudioPlayer *)cookie;
        if(me->mObserver && me->mReachedExtractorEOS ) {
            me->mPostedEOS = true;
            ALOGV("postAudioEOS");
            me->mObserver->postAudioEOS(0);
        }
    }
    return 1;
}

void MPQAudioPlayer::bufferAlloc(int32_t nSize) {

    mLocalBuf = malloc(nSize);
    if (NULL == mLocalBuf)
        ALOGE("Allocate Buffer for Software decoder failed ");
}

void MPQAudioPlayer::bufferDeAlloc() {

    if(mLocalBuf) {
        free(mLocalBuf);
        mLocalBuf = NULL;
    }
}

void MPQAudioPlayer::createThreads() {

    //Initialize all the Mutexes and Condition Variables
    // Create the extractor thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    mKillExtractorThread = false;
    mExtractorThreadAlive = true;

    ALOGD("Creating Extractor Thread");
    pthread_create(&mExtractorThread, &attr, extractorThreadWrapper, this);


    pthread_attr_destroy(&attr);
}

size_t MPQAudioPlayer::fillBuffer(void *data, size_t size) {

    switch(mDecoderType) {

        case EHardwareDecoder:
            return fillBufferfromParser(data, size);
        break;

        case ESoftwareDecoder:
            return fillBufferfromSoftwareDecoder(data, size);
        break;

        case EMS11Decoder:
                ALOGE("get AAC/ AC3 dat from parser");
                return fillMS11InputBufferfromParser(data, size);
        break;

        default:
         ALOGE("Fill Buffer - Invalid Decoder");
         //Returning zero size
         return 0;
    }
}

size_t MPQAudioPlayer::fillBufferfromSoftwareDecoder(void *data, size_t size) {

    ALOGE("fillBufferfromSoftwareDecoder");
    if (mReachedExtractorEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining = size;

    while (size_remaining > 0) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking) {

                if (mIsFirstBuffer) {
                    if (mFirstBuffer != NULL) {
                        mFirstBuffer->release();
                        mFirstBuffer = NULL;
                    }
                    mIsFirstBuffer = false;
                }

                MediaSource::ReadOptions::SeekMode seekMode;
                seekMode = MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
                options.setSeekTo(mSeekTimeUs, seekMode );
                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                size_remaining = size;

                mSeeking = false;
                if (mObserver && !mAsyncReset) {
                    ALOGD("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
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
                    if (mObserver && !mReachedExtractorEOS) {
                        if(mAudioSink->getSessionId()) {
                            mPostEOSDelayUs = mLatencyUs;
                            ALOGV("mPostEOSDelayUs = %lld", mPostEOSDelayUs);
                        }
                        else {
                            uint32_t numFramesPlayedOut, numFramesPendingPlayout;
                            status_t err = mAudioSink->getPosition(&numFramesPlayedOut);
                            if (err != OK || mNumFramesPlayed < numFramesPlayedOut) {
                                numFramesPendingPlayout = 0;
                            }
                            else {
                                numFramesPendingPlayout =
                                        mNumFramesPlayed - numFramesPlayedOut;
                            }

                            mFrameSize = mAudioSink->frameSize();
                            uint32_t numAdditionalFrames = size_done / mFrameSize;

                            numFramesPendingPlayout += numAdditionalFrames;

                            int64_t timeToCompletionUs =
                                (1000000ll * numFramesPendingPlayout) / mSampleRate;

                            ALOGV("total number of frames played: %lld (%lld us)",
                                   (mNumFramesPlayed + numAdditionalFrames),
                                   1000000ll * (mNumFramesPlayed + numAdditionalFrames)
                                    / mSampleRate);
                            ALOGV("%d frames left to play, %lld us (%.2f secs)",
                                numFramesPendingPlayout,
                               timeToCompletionUs, timeToCompletionUs / 1E6);
                            mPostEOSDelayUs = mLatencyUs + timeToCompletionUs;
                            ALOGV("mPostEOSDelayUs = %lld", mPostEOSDelayUs);

                        }
                    }
                    ALOGD("fill buffer - reached eos true");
                    mReachedExtractorEOS = true;
                    mFinalStatus = err;
                    break;
                }
            }
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

    {
        Mutex::Autolock autoLock(mLock);
        if(mFrameSize != 0)
            mNumFramesPlayed += size_done / mFrameSize;
    }


    ALOGV("fill buffer size_done = %d",size_done);
    return size_done;
}

size_t MPQAudioPlayer::fillMS11InputBufferfromParser(void *data, size_t size) {

    ALOGV("fillAC3BufferfromParser");
    if (mReachedExtractorEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining =  0;

    while (1) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking || mInternalSeeking) {

                MediaSource::ReadOptions::SeekMode seekMode;
                seekMode = MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
                options.setSeekTo(mSeekTimeUs, seekMode );
                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                //size_remaining = size;

                mSeeking = false;
                if (mObserver && !mAsyncReset) {
                    ALOGD("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
            }
        }
        if (mInputBuffer == NULL) {

            status_t err = OK;

            if(!mFirstEncodedBuffer && ((mAudioFormat == AUDIO_FORMAT_AAC) || (mAudioFormat == AUDIO_FORMAT_AAC_ADIF))) {
                uint32_t type;
                const void *configData;
                size_t configSize = 0;
                   const void *codec_specific_data;
                   size_t codec_specific_data_size = 0;
                if (mSource->getFormat()->findData(kKeyESDS, &type, &configData, &configSize)) {
                    ALOGV("GET ESDS - ");
                    ESDS esds((const char *)configData, configSize);
                    CHECK_EQ(esds.InitCheck(), (status_t)OK);

                   esds.getCodecSpecificInfo(
                        &codec_specific_data, &codec_specific_data_size);

                   memcpy((char *)data,(const char *)codec_specific_data, codec_specific_data_size);
                size_done = codec_specific_data_size;
                ALOGV("size_done = %d",size_done);
                const char *configarr = (const char *)codec_specific_data;
                for(size_t k = 0; k < size_done ; k++) {
                ALOGV("Config data is 0x%x",configarr[k]);
                }

                }
                else if (mSource->getFormat()->findData(kKeyAacCodecSpecificData, &type, &configData, &configSize)) {
                    ALOGI("AAC");
                    memcpy((char *)data,(const char *)configData, configSize);
                size_done = configSize;
                ALOGV("size_done = %d",size_done);
                const char *configarr = (const char *)configData;
                for(size_t k = 0; k < size_done ; k++) {
                ALOGV("Config data is 0x%x",configarr[k]);
                }

                }
                mFirstEncodedBuffer = true;
                //TODO: Handle Error case if config data is zero
                break;
            }

            err = mSource->read(&mInputBuffer, &options);

            CHECK((err == OK && mInputBuffer != NULL)
                  || (err != OK && mInputBuffer == NULL));
            {
                Mutex::Autolock autoLock(mLock);

                if (err != OK) {
                    ALOGD("fill buffer - reached eos true");
                    mReachedExtractorEOS = true;
                    mFinalStatus = err;
                    mFinalStatus = err;
                    break;
                }
            }
        }


        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;
            continue;
        }

        memcpy((char *)data,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               mInputBuffer->range_length());

        size_done =  mInputBuffer->range_length();

        mInputBuffer->set_range(mInputBuffer->range_offset(),
                                mInputBuffer->range_length() - mInputBuffer->range_length());

        break;
    }

    ALOGV("fill buffer size_done = %d",size_done);
    return size_done;
}


size_t MPQAudioPlayer::fillBufferfromParser(void *data, size_t size) {

    ALOGV("fillBufferfromParser");
    if (mReachedExtractorEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining = size;
    if (!mFirstEncodedBuffer && (mAudioFormat == AUDIO_FORMAT_WMA || mAudioFormat == AUDIO_FORMAT_WMA_PRO)) {
        uint32_t type;
        int configData[WMAPARAMSSIZE];
        size_t configSize = WMAPARAMSSIZE * sizeof(int);
        int value;
        sp<MetaData> format = mSource->getFormat();
        ALOGV("Extracting the WMA params");

        if (format->findInt32(kKeyBitRate, &value))
            configData[WMABITRATE] = value;
        else configData[WMABITRATE] = 0;
        if (format->findInt32(kKeyWMABlockAlign, &value))
            configData[WMABLOCKALIGN] = value;
        else configData[WMABLOCKALIGN] = 0;
        if (format->findInt32(kKeyWMAEncodeOpt, &value))
            configData[WMAENCODEOPTION] = value;
        else configData[WMAENCODEOPTION] = 0;

        if (format->findInt32(kKeyWMAFormatTag, &value))
            configData[WMAFORMATTAG] = value;
        else configData[WMAFORMATTAG] = 0;

        if (format->findInt32(kKeyWMABitspersample, &value))
            configData[WMABPS] = value;
        else configData[WMABPS] = 0;

        if (format->findInt32(kKeyWMAChannelMask, &value))
            configData[WMACHANNELMASK] = value;
        else configData[WMACHANNELMASK] = 0;

        if (format->findInt32(kKeyWMAAdvEncOpt1, &value))
            configData[WMAENCODEOPTION1] = value;
        else configData[WMAENCODEOPTION1] = 0;

        if (format->findInt32(kKeyWMAAdvEncOpt2, &value))
            configData[WMAENCODEOPTION2] = value;
        else configData[WMAENCODEOPTION2] = 0;

        memcpy((char *)data,(const char *)configData, configSize);
        size_done = configSize;
        ALOGV("size_done = %d",size_done);
        mFirstEncodedBuffer = true;
        return size_done;

    }

    while (size_remaining > 0) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking || mInternalSeeking) {

                MediaSource::ReadOptions::SeekMode seekMode;
                seekMode = MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
                options.setSeekTo(mSeekTimeUs, seekMode );
                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                size_remaining = size;

                mSeeking = false;
                if (mObserver && !mAsyncReset && !mInternalSeeking) {
                    ALOGD("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
                mInternalSeeking = false;
            }
        }
        if (mInputBuffer == NULL) {
            status_t err;
            err = mSource->read(&mInputBuffer, &options);

            CHECK((err == OK && mInputBuffer != NULL)
                  || (err != OK && mInputBuffer == NULL));
            {
                Mutex::Autolock autoLock(mLock);

                if (err != OK) {
                    ALOGD("fill buffer - reached eos true");
                    mReachedExtractorEOS = true;
                    mFinalStatus = err;
                    break;
                }
            }
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

    if(mReachedExtractorEOS) {
        memset((char *)data + size_done, 0x0, size_remaining);
    }
    ALOGV("fill buffer size_done = %d",size_done);
    return size_done;
}

int64_t MPQAudioPlayer::getRealTimeUs() {

    Mutex::Autolock autoLock(mLock);
    CHECK(mStarted);

    switch(mDecoderType) {

        case EHardwareDecoder:
            mPositionTimeRealUs = mSeekTimeUs + mPositionTimeMediaUs;
            break;
        case EMS11Decoder:
        case ESoftwareDecoder:
            mPositionTimeRealUs =  -mLatencyUs + mSeekTimeUs + mPositionTimeMediaUs;
            break;
        default:
            ALOGV(" Invalide Decoder return zero time");
            mPositionTimeRealUs = 0;
            break;
    }
    return mPositionTimeRealUs;
}

int64_t MPQAudioPlayer::getMediaTimeUs() {

    Mutex::Autolock autoLock(mLock);

    mPositionTimeMediaUs = mSeekTimeUs + getAudioTimeStampUs();
    if (mIsPaused) {
        ALOGV("getMediaTimeUs - paused = %lld",mTimePaused);
        return mTimePaused;
    } else {
        ALOGV("getMediaTimeUs - mSeekTimeUs = %lld", mSeekTimeUs);
        return mPositionTimeMediaUs;
    }
}

bool MPQAudioPlayer::getMediaTimeMapping(
                                   int64_t *realtime_us, int64_t *mediatime_us) {
    Mutex::Autolock autoLock(mLock);

    mPositionTimeMediaUs = (mSeekTimeUs + getAudioTimeStampUs());

    *realtime_us = mPositionTimeRealUs;
    *mediatime_us = mPositionTimeMediaUs;

    return mPositionTimeRealUs != -1 && mPositionTimeMediaUs != -1;
}

void MPQAudioPlayer::requestAndWaitForExtractorThreadExit() {

    if (!mExtractorThreadAlive)
        return;
    ALOGD("mKillExtractorThread true");
    mKillExtractorThread = true;
    mExtractorCv.signal();
    pthread_join(mExtractorThread,NULL);
    ALOGD("Extractor thread killed");
}

void MPQAudioPlayer::onPauseTimeOut() {
    Mutex::Autolock autoLock(mLock);
    //TODO  : Need to call standby on the the stream here
}


int64_t MPQAudioPlayer::getAudioTimeStampUs() {

    uint64_t tstamp = 0;
    ALOGV("MPQ Player: getAudioTimeStampUs");
    if (mAudioSink->getTimeStamp(&tstamp)) {
        ALOGE("MPQ Player: failed SNDRV_COMPRESS_TSTAMP\n");
        return 0;
    } else {
        ALOGV("timestamp = %lld\n", tstamp);
        return (tstamp + RENDER_LATENCY);
    }
    return 0;
}

status_t MPQAudioPlayer::configurePCM() {

    int err = 0;
    char *mpqAudioDevice = (char *)"";
    int flags = 0;
    ALOGV("configurePCM");
    //AudioEventObserver *aeObv;
    switch (mDecoderType) {
        case ESoftwareDecoder:
        case EMS11Decoder:
             ALOGV("getOutputSession = %d", mAudioSink->getSessionId());
             if(mAudioSink->getSessionId()) {
                if(mAudioSink == NULL) {
                   ALOGE("PCM stream invalid");
                   return BAD_VALUE;
                }
                mInputBufferSize = mAudioSink->frameCount();
                ALOGV("mInputBufferSize = %d",mInputBufferSize);
                mLatencyUs = (int64_t)mAudioSink->latency() * 1000;
                ALOGV("mLatencyUs = %lld",mLatencyUs);
             }
             else {
                mInputBufferSize = mAudioSink->bufferSize();
                ALOGV("get sink buffer size = %d",mInputBufferSize);
                mLatencyUs = (int64_t)mAudioSink->latency() * 1000;
                ALOGV("Sink -mLatencyUs = %lld",mLatencyUs);
             }
             if(mDecoderType == EMS11Decoder) {
                 bufferAlloc(AAC_AC3_BUFFER_SIZE);
                 if (NULL == mLocalBuf) {
                     ALOGE("Allocate Buffer for Software decoder failed ");
                     //TODO : Return No memory Error
                    return BAD_VALUE;
                 }
                 break;
             }
             bufferAlloc(mInputBufferSize);
             if (NULL == mLocalBuf) {
                 ALOGE("Allocate Buffer for Software decoder failed ");
                 //TODO : Return No memory Error
                 return BAD_VALUE;
             }
            break;
        case EHardwareDecoder:
             ALOGV("getOutputSession = ");
//             CHECK(mAudioSink.get() == NULL);
             ALOGV("getOutputSession-- ");

             if (mAudioFormat != AUDIO_FORMAT_WMA && mAudioFormat != AUDIO_FORMAT_WMA_PRO) {
                 mInputBufferSize = mAudioSink->frameCount();
                 ALOGD("mInputBufferSize = %d",mInputBufferSize);
                 bufferAlloc(mInputBufferSize);
             } else {
                 bufferAlloc(WMAPARAMSSIZE*sizeof(int));
             }
             ALOGV("Hardware break");
           break;

        default:
            //TODO : err - debug message
            ALOGE("default case - invalid case ");
            break;
    }
    return err;
}
status_t MPQAudioPlayer::getDecoderAndFormat() {

    status_t err = OK;
    if (   !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_RAW) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_QCELP) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_EVRC) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AMR_NB) ||
           !strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AMR_WB) ||
           !strcasecmp(mMimeType.string(),  MEDIA_MIMETYPE_AUDIO_VORBIS) ||
           !strcasecmp(mMimeType.string(),  MEDIA_MIMETYPE_AUDIO_FLAC)) {
        ALOGW("Sw Decoder");
        mAudioFormat = AUDIO_FORMAT_PCM_16_BIT;
        mDecoderType = ESoftwareDecoder;
        err = checkForInfoFormatChanged();
        if(err != OK) {
           ALOGE("checkForInfoFormatChanged err = %d", err);
           return err;
        }
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AC3)) {
        ALOGW("MS11 AC3");
       mDecoderType = EMS11Decoder;
       mAudioFormat = AUDIO_FORMAT_AC3;
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_EAC3)) {
        ALOGW("MS11 EAC3");
       mDecoderType = EMS11Decoder;
       mAudioFormat = AUDIO_FORMAT_EAC3;
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_AAC)) {
        ALOGW("MS11 AAC");
        mDecoderType = EMS11Decoder;
        mAudioFormat = AUDIO_FORMAT_AAC;
        if(mIsAACFormatAdif)
            mAudioFormat = AUDIO_FORMAT_AAC_ADIF;
    }
    else if (!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_WMA)) {
        ALOGW("Hw Decoder - WMA");
        mAudioFormat = AUDIO_FORMAT_WMA;
        mDecoderType = EHardwareDecoder;
        sp<MetaData> format = mSource->getFormat();
        int version = -1;
        CHECK(format->findInt32(kKeyWMAVersion, &version));
        if(version==kTypeWMAPro || version==kTypeWMALossLess)
            mAudioFormat = AUDIO_FORMAT_WMA_PRO;
    }
    else if(!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_DTS)) {
        ALOGE("### Hw Decoder - DTS");
        mAudioFormat = AUDIO_FORMAT_DTS;
        mDecoderType = EHardwareDecoder;
    }
    else if(!strcasecmp(mMimeType.string(), MEDIA_MIMETYPE_AUDIO_MPEG)) {
        ALOGW("Hw Decoder - MP3");
        mAudioFormat = AUDIO_FORMAT_MP3;
        mDecoderType = EHardwareDecoder;
    }
    else {

       ALOGW("invalid format ");
       err =  BAD_VALUE;
    }
     return err;
}

status_t MPQAudioPlayer::checkForInfoFormatChanged() {

    /* Check for an INFO format change for formats
    *  that use software decoder. Update the format
    *  accordingly
    */
    status_t err = OK;
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
    } else if(mFirstBufferResult != OK) {
        mReachedExtractorEOS = true;
        mFinalStatus = mFirstBufferResult;
        return mFirstBufferResult;
    } else {
        mIsFirstBuffer = true;
    }

    err = updateMetaDataInformation();
    if(err != OK) {
        ALOGE("updateMetaDataInformation = %d", err);
    }
    return err;
}

status_t MPQAudioPlayer::updateMetaDataInformation() {

    sp<MetaData> format = mSource->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    mMimeType = mime;
    CHECK(success);

    success = format->findInt32(kKeySampleRate, &mSampleRate);
    CHECK(success);

    success = format->findInt32(kKeyChannelCount, &mNumChannels);
    CHECK(success);

    if(!mNumChannels)
        mNumChannels = 2;

    success = format->findInt32(kkeyAacFormatAdif, &mIsAACFormatAdif);
    //CHECK(success);

    success = format->findInt64(kKeyDuration, &mDurationUs);
    ALOGV("mDurationUs = %lld, %s",mDurationUs,mMimeType.string());
    return OK;
}

} //namespace android
