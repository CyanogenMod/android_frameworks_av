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

#ifndef MPQ_AUDIO_PLAYER_H_

#define MPQ_AUDIO_PLAYER_H_

#include "AudioPlayer.h"

#include <media/IAudioFlinger.h>
#include <utils/threads.h>
#include <utils/List.h>
#include <utils/Vector.h>
#include <pthread.h>
#include <binder/IServiceManager.h>

#include <linux/unistd.h>
//#include <include/linux/msm_audio.h>

#include <include/TimedEventQueue.h>
#include <hardware/audio.h>
#include <binder/BinderService.h>
#include <binder/MemoryDealer.h>
#include <powermanager/IPowerManager.h>

// Pause timeout = 3sec
#define MPQ_AUDIO_PAUSE_TIMEOUT_USEC 3000000
//Wma params configuration
#define WMAPARAMSSIZE       8
#define WMABITRATE          0
#define WMABLOCKALIGN       1
#define WMAENCODEOPTION     2
#define WMAFORMATTAG        3
#define WMABPS              4
#define WMACHANNELMASK      5
#define WMAENCODEOPTION1    6
#define WMAENCODEOPTION2    7

namespace android {
class MPQAudioPlayer : public AudioPlayer {
public:

    MPQAudioPlayer(const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                AwesomePlayer *audioObserver = NULL, bool hasVideo = false);

    virtual ~MPQAudioPlayer();

    // Caller retains ownership of "source".
    virtual void setSource(const sp<MediaSource> &source);

    // Return time in us.
    virtual int64_t getRealTimeUs();

    virtual status_t start(bool sourceAlreadyStarted = false);

    virtual void pause(bool playPendingSamples = false);
    virtual void resume();

    // Returns the timestamp of the last buffer played (in us).
    virtual int64_t getMediaTimeUs();

    // Returns true if a mapping is established, i.e. the MPQ Audio Player
    // has played at least one frame of audio.
    virtual bool getMediaTimeMapping(int64_t *realtime_us, int64_t *mediatime_us);

    virtual status_t seekTo(int64_t time_us);

    virtual bool isSeeking();
    virtual bool reachedEOS(status_t *finalStatus);

    static int getMPQAudioObjectsAlive();

private:

    void* mPlaybackHandle;
    void* mCaptureHandle;
    static int mMPQAudioObjectsAlive;


    enum DecoderType {
        ESoftwareDecoder = 0,
        EHardwareDecoder,
        EMS11Decoder,
    }mDecoderType;

    void clearPowerManager();

    class PMDeathRecipient : public IBinder::DeathRecipient {
        public:
                        PMDeathRecipient(void *obj){parentClass = ( MPQAudioPlayer *)obj;}
            virtual     ~PMDeathRecipient() {}

            // IBinder::DeathRecipient
            virtual     void        binderDied(const wp<IBinder>& who);

        private:
                        MPQAudioPlayer *parentClass;
                        PMDeathRecipient(const PMDeathRecipient&);
                        PMDeathRecipient& operator = (const PMDeathRecipient&);

        friend class  MPQAudioPlayer;
    };

    friend class PMDeathRecipient;

    void        acquireWakeLock();
    void        releaseWakeLock();

    sp<IPowerManager>       mPowerManager;
    sp<IBinder>             mWakeLockToken;
    sp<PMDeathRecipient>    mDeathRecipient;

    void * mLocalBuf;
    //Audio Flinger related variables
    sp<IAudioFlinger> mAudioFlinger;

    //Declare all the threads
    pthread_t mExtractorThread;

    //Kill Thread boolean
    bool mKillExtractorThread;
    bool mKillEventThread;

    //Thread alive boolean
    bool mExtractorThreadAlive;
    bool mEventThreadAlive;

    //Declare the condition Variables and Mutex
    Mutex mExtractorMutex;

    Condition mExtractorCv;

    //global lock for MPQ Audio Player
    Mutex pmLock;
    Mutex mLock;
    Mutex mSeekLock;

    int32_t mChannelMask;

    //Media source - (Parser  for tunnel mode)
    sp<MediaSource> mSource;

    //Buffer related variables
    MediaBuffer *mInputBuffer;
    uint32_t mInputBufferSize;
    int32_t mInputBufferCount;

    //Audio Parameters
    int mSampleRate;
    int32_t mNumChannels;
    String8 mMimeType;
    size_t mFrameSize;
    int64_t mNumFramesPlayed;
    int mAudioFormat;
    int mIsAACFormatAdif;

    //Miscellaneous variables
    //Needed for AV sync
    int64_t mLatencyUs;
    bool mStarted;
    volatile bool mAsyncReset;
    bool mHasVideo;
    bool mFirstEncodedBuffer;

    //Timestamp variable
    int64_t mPositionTimeMediaUs;
    int64_t mPositionTimeRealUs;
    int64_t mTimePaused;
    int64_t mSeekTimeUs;
    int64_t mDurationUs;
    int32_t mTimeout;
    int64_t mPostEOSDelayUs;

    //Seek variables
    bool mSeeking;
    bool mInternalSeeking;

    //EOS variables
    bool mPostedEOS;
    bool mReachedExtractorEOS;
    status_t mFinalStatus;

    //Pause variables
    bool mIsPaused;
    bool mPlayPendingSamples;
    TimedEventQueue mQueue;
    bool mQueueStarted;
    sp<TimedEventQueue::Event>  mPauseEvent;
    bool mPauseEventPending;
    bool mSourcePaused;

    //Routing variables
    bool mAudioSinkOpen;
    bool mIsAudioRouted;

    bool mIsFirstBuffer;
    status_t mFirstBufferResult;
    MediaBuffer *mFirstBuffer;

    sp<MediaPlayerBase::AudioSink> mAudioSink;
    bool mA2DPEnabled;
    AwesomePlayer *mObserver;

    // helper function to obtain AudioFlinger service handle
    void getAudioFlinger();

    size_t fillBuffer(void *data, size_t size);

    int64_t getRealTimeUsLocked();

    void reset();

    void onPauseTimeOut();

    void bufferAlloc(int32_t nSize);

    void bufferDeAlloc();

    // make sure Decoder thread has exited
    void requestAndWaitForExtractorThreadExit();

    //Thread functions
    static void *extractorThreadWrapper(void *me);
    void extractorThreadEntry();

    void createThreads();

    status_t setPlaybackALSAParams();

    status_t configurePCM();
    //Get time stamp from driver
    int64_t getAudioTimeStampUs();

    status_t getDecoderAndFormat();

    status_t seekPlayback();

    status_t pausePlayback(bool bIgnorePendingSamples);

    status_t resumePlayback(int sessionId, bool bIgnorePendingSamples);

    size_t fillBufferfromSoftwareDecoder(void *data, size_t size);
    size_t fillBufferfromParser(void *data, size_t size);
    size_t fillMS11InputBufferfromParser(void *data, size_t size);

    status_t checkForInfoFormatChanged();
    status_t updateMetaDataInformation();

    static size_t postEOS(
        MediaPlayerBase::AudioSink *audioSink,
        void *data, size_t size, void *me);

    MPQAudioPlayer(const MPQAudioPlayer &);
    MPQAudioPlayer &operator=(const MPQAudioPlayer &);
};

struct MPQAudioEvent : public TimedEventQueue::Event {
    MPQAudioEvent(MPQAudioPlayer *player,
               void (MPQAudioPlayer::*method)())
        : mPlayer(player),
          mMethod(method) {
    }

protected:
    virtual ~MPQAudioEvent() {}

    virtual void fire(TimedEventQueue *queue, int64_t /* now_us */) {
        (mPlayer->*mMethod)();
    }

private:
    MPQAudioPlayer *mPlayer;
    void (MPQAudioPlayer::*mMethod)();

    MPQAudioEvent(const MPQAudioEvent &);
    MPQAudioEvent &operator=(const MPQAudioEvent &);
};

}  // namespace android

#endif  // MPQ_AUDIO_PLAYER_H_
