/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
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

//#define LOG_NDEBUG 0
//#define LOG_NDDEBUG 0
#define LOG_TAG "TunnelPlayer"
#include <utils/Log.h>
#include <utils/threads.h>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>

#include <media/stagefright/TunnelPlayer.h>
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

static const char   mName[] = "TunnelPlayer";
#define MEM_METADATA_SIZE 64
#define MEM_PADDING 64
/*
 * We need to reserve some space in the
 * ion buffer (used in HAL) to save the
 * metadata. so read from the extractor
 * a somewhat smaller number of bytes.
 * ideally this number should be bufer_size - sizeof(struct output_metadata_t)
 */
#define MEM_BUFFER_SIZE (240*1024 - MEM_METADATA_SIZE)
#define MEM_BUFFER_COUNT 4
#define TUNNEL_BUFFER_TIME 1500000

namespace android {
int TunnelPlayer::mTunnelObjectsAlive = 0;

TunnelPlayer::TunnelPlayer(
                    const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                    AwesomePlayer *observer, bool hasVideo)
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
    ALOGD("TunnelPlayer::TunnelPlayer()");
    mTunnelObjectsAlive++;
    numChannels = 0;
    mPaused = false;
    mIsA2DPEnabled = false;
    mAudioFlinger = NULL;
    mAudioFlingerClient = NULL;
    mFormat = AUDIO_FORMAT_MP3;
    mQueue.start();
    mQueueStarted      = true;
    mPauseEvent        = new TunnelEvent(this, &TunnelPlayer::onPauseTimeOut);
    mPauseEventPending = false;

    //getAudioFlinger();
    //ALOGD("Registering client with AudioFlinger");
    //mAudioFlinger->registerClient(mAudioFlingerClient);

    mSeekTimeUs = 0;
    mIsAudioRouted = false;

    mHasVideo = hasVideo;
    initCheck = true;

    //mDeathRecipient = new PMDeathRecipient(this);
}
void TunnelPlayer::acquireWakeLock()
{
    /*Mutex::Autolock _l(pmLock);

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
    }*/
}

void TunnelPlayer::releaseWakeLock()
{
    /*Mutex::Autolock _l(pmLock);

    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock() %s", mName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLock(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }*/
}

void TunnelPlayer::clearPowerManager()
{
    Mutex::Autolock _l(pmLock);
    releaseWakeLock();
    mPowerManager.clear();
}

void TunnelPlayer::PMDeathRecipient::binderDied(const wp<IBinder>& who)
{
    parentClass->clearPowerManager();
    ALOGW("power manager service died !!!");
}

TunnelPlayer::~TunnelPlayer() {
    ALOGD("TunnelPlayer::~TunnelPlayer()");
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();
    //mAudioFlinger->deregisterClient(mAudioFlingerClient);
    mTunnelObjectsAlive--;

    releaseWakeLock();
    if (mPowerManager != 0) {
        sp<IBinder> binder = mPowerManager->asBinder();
        binder->unlinkToDeath(mDeathRecipient);
    }


}

void TunnelPlayer::getAudioFlinger() {
/*    Mutex::Autolock _l(mAudioFlingerLock);

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
        if ( mAudioFlingerClient == NULL ) {
            mAudioFlingerClient = new AudioFlingerTunnelDecodeClient(this);
        }

        binder->linkToDeath(mAudioFlingerClient);
        mAudioFlinger = interface_cast<IAudioFlinger>(binder);
    }
    ALOGE_IF(mAudioFlinger==0, "no AudioFlinger!?");*/
}

/*TunnelPlayer::AudioFlingerTunnelDecodeClient::AudioFlingerTunnelDecodeClient(void *obj)
{
    ALOGD("TunnelPlayer::AudioFlingerTunnelDecodeClient - Constructor");
    pBaseClass = (TunnelPlayer*)obj;
}

void TunnelPlayer::AudioFlingerTunnelDecodeClient::binderDied(const wp<IBinder>& who) {
    Mutex::Autolock _l(pBaseClass->mAudioFlingerLock);

    pBaseClass->mAudioFlinger.clear();
    ALOGW("AudioFlinger server died!");
}*/

/*void TunnelPlayer::AudioFlingerTunnelDecodeClient::ioConfigChanged(int event, int ioHandle, void *param2) {
    ALOGV("ioConfigChanged() event %d", event);


    if (event != AudioSystem::A2DP_OUTPUT_STATE) {
        return;
    }

    switch ( event ) {
    case AudioSystem::A2DP_OUTPUT_STATE:
        {
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
    default:
        break;
    }
    ALOGV("ioConfigChanged Out");
}*/

/*void TunnelPlayer::handleA2DPSwitch() {
    //TODO: Implement
}
*/

void TunnelPlayer::setSource(const sp<MediaSource> &source) {
    CHECK(mSource == NULL);
    ALOGD("Setting source from Tunnel Player");
    mSource = source;
}

status_t TunnelPlayer::start(bool sourceAlreadyStarted) {
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
    if (!strcasecmp(mime,MEDIA_MIMETYPE_AUDIO_AAC)) {
        mFormat = AUDIO_FORMAT_AAC;
    }
    if (!strcasecmp(mime,MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        mFormat = AUDIO_FORMAT_AMR_WB;
        ALOGV("TunnelPlayer::start AUDIO_FORMAT_AMR_WB");
    }
    if (!strcasecmp(mime,MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS)) {
        mFormat = AUDIO_FORMAT_AMR_WB_PLUS;
        ALOGV("TunnelPlayer::start AUDIO_FORMAT_AMR_WB_PLUS");
    }

    CHECK(success);

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
    audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_TUNNEL |
                                                         AUDIO_OUTPUT_FLAG_DIRECT);
    ALOGV("mAudiosink->open() mSampleRate %d, numChannels %d, mChannelMask %d, flags %d",mSampleRate, numChannels, mChannelMask, flags);
    err = mAudioSink->open(
        mSampleRate, numChannels, mChannelMask, mFormat,
        DEFAULT_AUDIOSINK_BUFFERCOUNT,
        &TunnelPlayer::AudioSinkCallback,
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

    if (!mIsA2DPEnabled) {
        acquireWakeLock();
    }

    mIsAudioRouted = true;
    mStarted = true;
    mAudioSink->start();
    ALOGV("Waking up extractor thread");
    pthread_cond_signal(&extractor_cv);

    return OK;
}

status_t TunnelPlayer::seekTo(int64_t time_us) {

    ALOGD("seekTo: time_us %lld", time_us);

    Mutex::Autolock _l(mLock); //to sync w/ onpausetimeout

    //This can happen if the client calls seek
    //without ever calling getPosition
    if (mPositionTimeRealUs == -1) {
        getOffsetRealTime_l(&mPositionTimeRealUs);
    }

    if (mPositionTimeRealUs > 0) {
      //check for return conditions only if seektime
      // is set
      bool postSeekComplete = false;

      if (time_us > mPositionTimeRealUs){
           if ((time_us - mPositionTimeRealUs) < TUNNEL_BUFFER_TIME){
             ALOGV("In seekTo(), ignoring time_us %lld mSeekTimeUs %lld", time_us, mSeekTimeUs);
             postSeekComplete = true;
           }
      } else {
           if ((mPositionTimeRealUs - time_us) < TUNNEL_BUFFER_TIME){
               ALOGV("In seekTo(), ignoring time_us %lld mSeekTimeUs %lld", time_us, mSeekTimeUs);
               postSeekComplete = true;
           }
      }

      if (postSeekComplete) {
          mLock.unlock(); //unlock and post
          mObserver->postAudioSeekComplete();
          mLock.lock();
          return OK;
      }
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
            ALOGV("Going to signal extractor thread since playback is already going on ");
            pthread_cond_signal(&extractor_cv);
            ALOGV("Signalled extractor thread.");
        }
    }
    ALOGV("seek done.");
    return OK;
}
void TunnelPlayer::pause(bool playPendingSamples) {
    CHECK(mStarted);
    if (mPaused) {
        return;
    }
    Mutex::Autolock autoLock(mLock);
    ALOGV("pause: playPendingSamples %d", playPendingSamples);
    mPaused = true;
    int64_t playedTime = 0;
    if(!mPauseEventPending) {
        ALOGV("Posting an event for Pause timeout");
        mQueue.postEventWithDelay(mPauseEvent, TUNNEL_PAUSE_TIMEOUT_USEC);
        mPauseEventPending = true;
    }
    getPlayedTimeFromDSP_l(&playedTime);
    mPauseTime = mSeekTimeUs + playedTime;
    if (mAudioSink.get() != NULL) {
        ALOGV("AudioSink pause");
        mAudioSink->pause();
    }
}

void TunnelPlayer::resume() {
    Mutex::Autolock autoLock(mLock);
    ALOGV("resume: mPaused %d",mPaused);
    if ( mPaused) {
        CHECK(mStarted);
        if (!mIsA2DPEnabled) {
            if(mPauseEventPending) {
                ALOGV("Resume(): Cancelling the puaseTimeout event");
                mPauseEventPending = false;
                mQueue.cancelEvent(mPauseEvent->eventID());
            }

        }
        audio_format_t format;

        if (!mIsAudioRouted) {
            audio_output_flags_t flags = (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_TUNNEL |
                                                                AUDIO_OUTPUT_FLAG_DIRECT);
            status_t err = mAudioSink->open(
                mSampleRate, numChannels, mChannelMask, mFormat,
                DEFAULT_AUDIOSINK_BUFFERCOUNT,
                &TunnelPlayer::AudioSinkCallback,
                this,
                (mA2DPEnabled ?  AUDIO_OUTPUT_FLAG_NONE : flags ));
            if (err != NO_ERROR) {
                ALOGE("Audio sink open failed.");
            }
            mIsAudioRouted = true;
        }
        mPaused = false;
        ALOGV("Audio sink open succeeded.");
        mAudioSink->start();
        ALOGV("Audio sink start succeeded.");
        pthread_cond_signal(&extractor_cv);
        ALOGV("Audio signalling extractor thread.");
    }
}

//static
size_t TunnelPlayer::AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie) {
    if (buffer == NULL && size == AudioTrack::EVENT_UNDERRUN) {
        TunnelPlayer *me = (TunnelPlayer *)cookie;
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

void TunnelPlayer::reset() {
    ALOGV("Reset");

    Mutex::Autolock _l(mLock); //to sync w/ onpausetimeout

    //cancel any pending onpause timeout events
    //doesnt matter if the event is really present or not
    mPauseEventPending = false;
    mQueue.cancelEvent(mPauseEvent->eventID());

    mReachedEOS = true;

    // make sure Decoder thread has exited
    requestAndWaitForExtractorThreadExit();

    // Close the audiosink after all the threads exited to make sure
    if (mIsAudioRouted) {
        mAudioSink->stop();
        mAudioSink->close();
        mIsAudioRouted = false;
    }
    //TODO: Release Wake lock

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

    if (mStarted)
        mSource->stop();

    mSource.clear();

    mPositionTimeMediaUs = -1;
    mPositionTimeRealUs = -1;
    mSeeking = false;
    mReachedEOS = false;
    mReachedOutputEOS = false;
    mFinalStatus = OK;
    mStarted = false;
}


bool TunnelPlayer::isSeeking() {
    Mutex::Autolock autoLock(mLock);
    return mSeeking;
}

bool TunnelPlayer::reachedEOS(status_t *finalStatus) {
    *finalStatus = OK;
    Mutex::Autolock autoLock(mLock);
    *finalStatus = mFinalStatus;
    return mReachedOutputEOS;
}


void *TunnelPlayer::extractorThreadWrapper(void *me) {
    static_cast<TunnelPlayer *>(me)->extractorThreadEntry();
    return NULL;
}


void TunnelPlayer::extractorThreadEntry() {

    pthread_mutex_lock(&extractor_mutex);
    uint32_t BufferSizeToUse = MEM_BUFFER_SIZE;

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, mHasVideo ? ANDROID_PRIORITY_NORMAL :
                                              ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"Extractor Thread", 0, 0, 0);

    ALOGV("extractorThreadEntry wait for signal \n");
    if (!mStarted) {
        pthread_cond_wait(&extractor_cv, &extractor_mutex);
    }
    ALOGV("extractorThreadEntry ready to work \n");
    pthread_mutex_unlock(&extractor_mutex);
    if (killExtractorThread) {
        return;
    }
    if(mSource != NULL) {
        sp<MetaData> format = mSource->getFormat();
        const char *mime;
        bool success = format->findCString(kKeyMIMEType, &mime);
    }
    void* local_buf = malloc(BufferSizeToUse);
    int *lptr = ((int*)local_buf);
    int bytesWritten = 0;
    bool lSeeking = false;
    bool lPaused = false;
    while (!killExtractorThread) {

        if (mReachedEOS || mPaused || !mIsAudioRouted) {
            ALOGV("Going to sleep before write since "
                  "mReachedEOS %d, mPaused %d, mIsAudioRouted %d",
                  mReachedEOS, mPaused, mIsAudioRouted);
            pthread_mutex_lock(&extractor_mutex);
            pthread_cond_wait(&extractor_cv, &extractor_mutex);
            pthread_mutex_unlock(&extractor_mutex);
            ALOGV("Woke up from sleep before write since "
                  "mReachedEOS %d, mPaused %d, mIsAudioRouted %d",
                  mReachedEOS, mPaused, mIsAudioRouted);
            continue;
        }

        if (!mIsA2DPEnabled) {
            ALOGV("FillBuffer: MemBuffer size %d", BufferSizeToUse);
            ALOGV("Fillbuffer started");
            bytesWritten = fillBuffer(local_buf, BufferSizeToUse);
            ALOGV("FillBuffer completed bytesToWrite %d", bytesWritten);
            if(!killExtractorThread) {
                mLock.lock();
                lPaused = mPaused;
                mLock.unlock();

                if(lPaused == true) {
                    //write only if player is not in paused state. Sleep on lock
                    // resume is called
                    ALOGV("Going to sleep in decodethreadiwrite since sink is paused");
                    pthread_mutex_lock(&extractor_mutex);
                    pthread_cond_wait(&extractor_cv, &extractor_mutex);
                    ALOGV("Going to unlock n decodethreadwrite since sink "
                          "resumed mPaused %d, mIsAudioRouted %d, mReachedEOS %d",
                          mPaused, mIsAudioRouted, mReachedEOS);
                    pthread_mutex_unlock(&extractor_mutex);
                }
                mLock.lock();
                lSeeking = mSeeking||mInternalSeeking;
                mLock.unlock();

                if(lSeeking == false && (killExtractorThread == false)){
                    //if we are seeking, ignore write, otherwise write
                  ALOGV("Fillbuffer before seek flag %d", mSeeking);
                    int lWrittenBytes = mAudioSink->write(local_buf, bytesWritten);
                    ALOGV("Fillbuffer after write, written bytes %d and seek flag %d", lWrittenBytes, mSeeking);
                    if(lWrittenBytes > 0) {
                        //send EOS only if write was successful, if is_buffer_available
                        // is flushed out (which returns 0 do not SEND EOS
                        ALOGV("Fillbuffer after write and seek flag %d", mSeeking);
                        mLock.lock();
                        lSeeking = mSeeking||mInternalSeeking;
                        mLock.unlock();
                        //ignore posting zero length buffer is seeking is set
                        if(mReachedEOS && bytesWritten && !lSeeking && (killExtractorThread == false)) {
                            ALOGV("Fillbuffer after write sent EOS flag %d", lSeeking);
                            mAudioSink->write(local_buf, 0);
                        } else {
                            ALOGV("Not sending EOS buffer sent since seeking %d, "
                                  "kill %d and mReachedEOS %d",         \
                                  lSeeking, killExtractorThread, mReachedEOS);
                        }
                    } else {
                        ALOGV("write exited because of flush %d", mSeeking);
                    }
                } else {
                    ALOGV("Fillbuffer ignored since we seeked after fillBuffer was set %d", mSeeking);
                }
            }
        }

    }

    free(local_buf);

    //TODO: Call fillbuffer with different size and write to mAudioSink()
}
void TunnelPlayer::createThreads() {

    //Initialize all the Mutexes and Condition Variables
    pthread_mutex_init(&extractor_mutex, NULL);
    pthread_cond_init (&extractor_cv, NULL);

    // Create 4 threads Effect, decoder, event and A2dp
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    killExtractorThread = false;

    extractorThreadAlive = true;

    ALOGV("Creating Extractor Thread");
    pthread_create(&extractorThread, &attr, extractorThreadWrapper, this);

    pthread_attr_destroy(&attr);
}
size_t TunnelPlayer::fillBuffer(void *data, size_t size) {

    if (mReachedEOS) {
        return 0;
    }

    bool postSeekComplete = false;

    size_t size_done = 0;
    size_t size_remaining = size;
    //clear the flag since we dont know whether we are seeking or not, yet
    ALOGV("fillBuffer: Clearing seek flag in fill buffer");

    bool yield = !mIsFirstBuffer;

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
                if (mObserver && !mInternalSeeking) {
                    ALOGV("fillBuffer: Posting audio seek complete event");
                    postSeekComplete = true;
                }
                mInternalSeeking = false;
                ALOGV("fillBuffer: Setting seek flag in fill buffer");
                //set the flag since we know that this buffer is the new positions buffer
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

        if (mHasVideo && yield) {
            sched_yield();
        }
    }
    if(mReachedEOS)
        memset((char *)data + size_done, 0x0, size_remaining);
    ALOGV("fill buffer size_done = %d",size_done);

    if (postSeekComplete) {
        mObserver->postAudioSeekComplete();
    }

    return size_done;
}

int64_t TunnelPlayer::getRealTimeUs() {
    Mutex::Autolock autoLock(mLock);
    getOffsetRealTime_l(&mPositionTimeRealUs);
    //update media time too
    mPositionTimeMediaUs = mPositionTimeRealUs;
    return mPositionTimeRealUs;
}

void TunnelPlayer::getPlayedTimeFromDSP_l(int64_t* timeStamp ) {
    ALOGV("going to query timestamp");
    mAudioSink->getTimeStamp((uint64_t*)timeStamp);
    ALOGV("timestamp returned from DSP %lld ", (*timeStamp));
    return;
}

//offset with pause and seek time
void TunnelPlayer::getOffsetRealTime_l(int64_t* offsetTime) {
    if (mPaused) {
        *offsetTime = mPauseTime;
        ALOGV("getMediaTimeUs() mPaused %d mSeekTimeUs %lld mPauseTime %lld", mPaused, mSeekTimeUs, mPauseTime );
    } else {
        getPlayedTimeFromDSP_l(offsetTime);
        ALOGV("getMediaTimeUs() mPaused %d mSeekTimeUs %lld mPauseTime %lld, timeStamp %lld", mPaused, mSeekTimeUs, mPauseTime, *offsetTime);
        *offsetTime = mSeekTimeUs + *offsetTime;
    }
}

int64_t TunnelPlayer::getMediaTimeUs() {
    //essentially there is only one time, the real time
    return getRealTimeUs();
}

bool TunnelPlayer::getMediaTimeMapping(
                                   int64_t *realtime_us, int64_t *mediatime_us) {
    Mutex::Autolock autoLock(mLock);

    *realtime_us = mPositionTimeRealUs;
    *mediatime_us = mPositionTimeMediaUs;

    return mPositionTimeRealUs != -1 && mPositionTimeMediaUs != -1;
}

//lock has been taken in reset() to sync with onpausetimeout
void TunnelPlayer::requestAndWaitForExtractorThreadExit() {
    ALOGV("requestAndWaitForExtractorThreadExit -1");

    if (!extractorThreadAlive)
        return;

    killExtractorThread = true;

    ALOGV("requestAndWaitForExtractorThreadExit +0");
    if (mIsAudioRouted && !mReachedOutputEOS) {
        mAudioSink->flush();
    }

    ALOGV("requestAndWaitForExtractorThreadExit +1");
    pthread_cond_signal(&extractor_cv);
    ALOGV("requestAndWaitForExtractorThreadExit +2");
    mLock.unlock();
    pthread_join(extractorThread,NULL);
    mLock.lock();
    ALOGV("requestAndWaitForExtractorThreadExit +3");

    ALOGV("Extractor thread killed");
}

void TunnelPlayer::onPauseTimeOut() {
    Mutex::Autolock autoLock(mLock);
    int64_t playedTime = 0;
    ALOGV("onPauseTimeOut");
    if (!mPauseEventPending) {
        return;
    }
    mPauseEventPending = false;
    if(!mIsA2DPEnabled) {
        // 1.) Set seek flags
        mReachedEOS = false;
        mReachedOutputEOS = false;

        if(mSeeking == false) {
            ALOGV("onPauseTimeOut +2");
            mInternalSeeking = true;
            ALOGV("onPauseTimeOut +3");
            getPlayedTimeFromDSP_l(&playedTime);
            mSeekTimeUs += playedTime;
        } else {
            ALOGV("Do not update seek time if it was seeked before onpause timeout");
        }

        // 2.) Close routing Session
        ALOGV("onPauseTimeOut +4");
        mAudioSink->flush();
        ALOGV("onPauseTimeOut +5");
        mAudioSink->stop();
        ALOGV("onPauseTimeOut +6");
        mAudioSink->close();
        ALOGV("onPauseTimeOut +7");
        mIsAudioRouted = false;

        // 3.) Release Wake Lock
        releaseWakeLock();
    }

}

} //namespace android
