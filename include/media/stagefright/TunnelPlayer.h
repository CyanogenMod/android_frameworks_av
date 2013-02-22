/*
 * Copyright (C) 2009 The Android Open Source Project
 *
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

#ifndef TUNNEL_PLAYER_H_

#define TUNNEL_PLAYER_H_

#include "AudioPlayer.h"
#include <media/IAudioFlinger.h>
#include <utils/threads.h>
#include <utils/List.h>
#include <utils/Vector.h>
#include <fcntl.h>
#include <pthread.h>
#include <binder/IServiceManager.h>
#include <linux/unistd.h>
#include <include/TimedEventQueue.h>
#include <binder/BinderService.h>
#include <binder/MemoryDealer.h>
#include <powermanager/IPowerManager.h>

// Pause timeout = 3sec
#define TUNNEL_PAUSE_TIMEOUT_USEC 3000000
namespace android {

class TunnelPlayer : public AudioPlayer  {
public:
    enum {
        REACHED_EOS,
        SEEK_COMPLETE
    };

    TunnelPlayer(const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                AwesomePlayer *audioObserver = NULL, bool hasVideo = false);

    virtual ~TunnelPlayer();

    // Caller retains ownership of "source".
    virtual void setSource(const sp<MediaSource> &source);

    // Return time in us.
    virtual int64_t getRealTimeUs();

    virtual status_t start(bool sourceAlreadyStarted = false);

    virtual void pause(bool playPendingSamples = false);
    virtual void resume();

    // Returns the timestamp of the last buffer played (in us).
    virtual int64_t getMediaTimeUs();

    // Returns true iff a mapping is established, i.e. the TunnelPlayer
    // has played at least one frame of audio.
    virtual bool getMediaTimeMapping(int64_t *realtime_us, int64_t *mediatime_us);

    virtual status_t seekTo(int64_t time_us);

    virtual bool isSeeking();
    virtual bool reachedEOS(status_t *finalStatus);


    static int mTunnelObjectsAlive;
private:
    int64_t mPositionTimeMediaUs;
    int64_t mPositionTimeRealUs;
    bool mInternalSeeking;
    bool mIsAudioRouted;
    bool mStarted;
    bool mPaused;
    bool mA2DPEnabled;
    int32_t mChannelMask;
    int32_t numChannels;
    int32_t mSampleRate;
    int64_t mLatencyUs;
    size_t mFrameSize;
    int64_t mNumFramesPlayed;
    int64_t mNumFramesPlayedSysTimeUs;
    audio_format_t mFormat;
    bool mHasVideo;
    void clearPowerManager();

    class PMDeathRecipient : public IBinder::DeathRecipient {
        public:
                        PMDeathRecipient(void *obj){parentClass = (TunnelPlayer *)obj;}
            virtual     ~PMDeathRecipient() {}

            // IBinder::DeathRecipient
            virtual     void        binderDied(const wp<IBinder>& who);

        private:
                        TunnelPlayer *parentClass;
                        PMDeathRecipient(const PMDeathRecipient&);
                        PMDeathRecipient& operator = (const PMDeathRecipient&);

        friend class TunnelPlayer;
    };

    friend class PMDeathRecipient;

    void        acquireWakeLock();
    void        releaseWakeLock();

    sp<IPowerManager>       mPowerManager;
    sp<IBinder>             mWakeLockToken;
    sp<PMDeathRecipient>    mDeathRecipient;

    pthread_t extractorThread;

    //Kill Thread boolean
    bool killExtractorThread;

    //Thread alive boolean
    bool extractorThreadAlive;


    //Declare the condition Variables and Mutex

    Condition mExtractorCV;


    // make sure Decoder thread has exited
    void requestAndWaitForExtractorThreadExit();


    static void *extractorThreadWrapper(void *me);
    void extractorThreadEntry();

    void createThreads();

    volatile bool mIsA2DPEnabled;

    //Structure to recieve the BT notification from the flinger.
    class AudioFlingerTunneldecodeClient: public IBinder::DeathRecipient, public BnAudioFlingerClient {
    public:
        AudioFlingerTunneldecodeClient(void *obj);

        TunnelPlayer *pBaseClass;
        // DeathRecipient
        virtual void binderDied(const wp<IBinder>& who);

        // IAudioFlingerClient

        // indicate a change in the configuration of an output or input: keeps the cached
        // values for output/input parameters upto date in client process
        virtual void ioConfigChanged(int event, audio_io_handle_t ioHandle, const void *param2);

        friend class TunnelPlayer;
    };

    sp<IAudioFlinger> mAudioFlinger;

    // helper function to obtain AudioFlinger service handle
    void getAudioFlinger();
    void onPauseTimeOut();

    sp<AudioFlingerTunneldecodeClient> mAudioFlingerClient;
    friend class AudioFlingerTunneldecodeClient;
    Mutex mAudioFlingerLock;
    sp<MediaSource> mSource;

    MediaBuffer *mInputBuffer;

    Mutex pmLock;
    Mutex mLock;

    bool mSeeking;
    bool mReachedEOS;
    bool mReachedOutputEOS;
    status_t mFinalStatus;
    int64_t mSeekTimeUs;
    int64_t mPauseTime;


    bool mIsFirstBuffer;
    status_t mFirstBufferResult;
    MediaBuffer *mFirstBuffer;
    TimedEventQueue mQueue;
    bool            mQueueStarted;
    sp<TimedEventQueue::Event>  mPauseEvent;
    bool                        mPauseEventPending;

    typedef enum {
      NCREATED = -1,
      INITIALIZED,
      RUNNING,
      SLEEPING,
      EXITING,
    } ThreadState;

    sp<MediaPlayerBase::AudioSink> mAudioSink;
    AwesomePlayer *mObserver;
    ThreadState mThreadState;
    bool mStopSinkPending;

    static size_t AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *data, size_t size, void *me);

    enum A2DPState {
        A2DP_ENABLED,
        A2DP_DISABLED,
        A2DP_CONNECT,
        A2DP_DISCONNECT
    };

    void getPlayedTimeFromDSP_l(int64_t *timeStamp);
    void getOffsetRealTime_l(int64_t *offsetTime);

    size_t fillBuffer(void *data, size_t size);

    void reset();
    status_t schedPauseTimeOut();
    status_t stopAudioSink();

    TunnelPlayer(const TunnelPlayer &);
    TunnelPlayer &operator=(const TunnelPlayer &);
};

struct TunnelEvent : public TimedEventQueue::Event {
    TunnelEvent(TunnelPlayer *player,
               void (TunnelPlayer::*method)())
        : mPlayer(player),
          mMethod(method) {
    }

protected:
    virtual ~TunnelEvent() {}

    virtual void fire(TimedEventQueue *queue, int64_t /* now_us */) {
        (mPlayer->*mMethod)();
    }

private:
    TunnelPlayer *mPlayer;
    void (TunnelPlayer::*mMethod)();

    TunnelEvent(const TunnelEvent &);
    TunnelEvent &operator=(const TunnelEvent &);
};

}  // namespace android

#endif  // LPA_PLAYER_H_
