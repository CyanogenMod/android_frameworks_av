/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 * Not a Contribution
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
#include <utils/threads.h>
#include <utils/List.h>
#include <utils/Vector.h>
#include <fcntl.h>
#include <pthread.h>
#include <include/TimedEventQueue.h>

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
    static const int getTunnelObjectsAliveMax();
private:
    int64_t mPositionTimeMediaUs;
    int64_t mPositionTimeRealUs;
    bool mInternalSeeking;
    bool mIsAudioRouted;
    bool mStarted;
    bool mPaused;
    int32_t mChannelMask;
    int32_t numChannels;
    int32_t mSampleRate;
    int64_t mLatencyUs;
    size_t mFrameSize;
    int64_t mNumFramesPlayed;
    int64_t mNumFramesPlayedSysTimeUs;
    audio_format_t mFormat;
    bool mHasVideo;

    pthread_t mExtractorThread;

    //Kill Thread boolean
    bool mKillExtractorThread;

    //Thread alive boolean
    bool mExtractorThreadAlive;

    //Declare the condition Variables and Mutex
    Mutex mExtractorMutex;
    Condition mExtractorCv;


    // make sure Decoder thread has exited
    void requestAndWaitForExtractorThreadExit_l();


    static void *extractorThreadWrapper(void *me);
    void extractorThreadEntry();

    void createThreads();

    void onPauseTimeOut();

    sp<MediaSource> mSource;

    MediaBuffer *mInputBuffer;

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

    sp<MediaPlayerBase::AudioSink> mAudioSink;
    AwesomePlayer *mObserver;

    static size_t AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *data, size_t size, void *me);

    void getPlayedTimeFromDSP_l(int64_t *timeStamp);
    void getOffsetRealTime_l(int64_t *offsetTime);

    size_t fillBuffer(void *data, size_t size);

    void reset();
    bool seekTooClose(int64_t time_us);

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
