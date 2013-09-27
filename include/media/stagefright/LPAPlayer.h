/*
 * Copyright (c) 2009-2012, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef LPA_PLAYER_H_

#define LPA_PLAYER_H_

#include "AudioPlayer.h"
#include <utils/threads.h>
#include <utils/List.h>
#include <utils/Vector.h>
#include <fcntl.h>
#include <pthread.h>
#include <include/TimedEventQueue.h>

// Pause timeout = 3sec
#define LPA_PAUSE_TIMEOUT_USEC 3000000

namespace android {

class LPAPlayer : public AudioPlayer  {
public:
    enum {
        REACHED_EOS,
        SEEK_COMPLETE
    };

    enum {
        TRACK_DIRECT,
        TRACK_REGULAR,
        TRACK_NONE
    };

    LPAPlayer(const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                AwesomePlayer *audioObserver = NULL);

    virtual ~LPAPlayer();

    // Caller retains ownership of "source".
    virtual void setSource(const sp<MediaSource> &source);

    // Return time in us.
    virtual int64_t getRealTimeUs();

    virtual status_t start(bool sourceAlreadyStarted = false);

    virtual void pause(bool playPendingSamples = false);
    virtual status_t resume();

    // Returns the timestamp of the last buffer played (in us).
    virtual int64_t getMediaTimeUs();

    // Returns true iff a mapping is established, i.e. the LPAPlayer
    // has played at least one frame of audio.
    virtual bool getMediaTimeMapping(int64_t *realtime_us, int64_t *mediatime_us);

    virtual status_t seekTo(int64_t time_us);

    virtual bool isSeeking();
    virtual bool reachedEOS(status_t *finalStatus);

    static int mObjectsAlive;
private:
    int64_t mPositionTimeMediaUs;
    int64_t mPositionTimeRealUs;
    bool mInternalSeeking;
    bool mIsAudioRouted;
    bool mStarted;
    bool mPaused;
    int32_t mChannelMask;
    int32_t mNumOutputChannels;
    int32_t mNumInputChannels;
    int32_t mSampleRate;
    int64_t mLatencyUs;
    size_t mFrameSize;
    int64_t mTimeStarted;
    int64_t mTimePlayed;
    int64_t mNumFramesPlayed;
    int64_t mNumFramesPlayedSysTimeUs;

    pthread_t mDecoderThread;

    //Kill Thread boolean
    bool mKillDecoderThread;

    //Thread alive boolean
    bool mDecoderThreadAlive;

    //Declare the condition Variables and Mutex

    Mutex mDecoderMutex;

    Condition mDecoderCv;

    // make sure Decoder thread has exited
    void requestAndWaitForDecoderThreadExit_l();

    static void *decoderThreadWrapper(void *me);
    void decoderThreadEntry();

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
    int mTrackType;

    static size_t AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *data, size_t size, void *me,
        MediaPlayerBase::AudioSink::cb_event_t event);

    int64_t getTimeStamp();

    size_t fillBuffer(void *data, size_t size);

    int64_t getRealTimeUsLocked();

    void reset();

    status_t setupAudioSink();
    static size_t AudioCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie);
    size_t AudioCallback(void *cookie, void *data, size_t size);
    int64_t getMediaTimeUs_l();
    bool seekTooClose(int64_t);

    void convertMonoToStereo(int16_t *data, size_t size);

    LPAPlayer(const LPAPlayer &);
    LPAPlayer &operator=(const LPAPlayer &);
};

struct TimedEvent : public TimedEventQueue::Event {
    TimedEvent(LPAPlayer *player,
               void (LPAPlayer::*method)())
        : mPlayer(player),
          mMethod(method) {
    }

protected:
    virtual ~TimedEvent() {}

    virtual void fire(TimedEventQueue *queue, int64_t /* now_us */) {
        (mPlayer->*mMethod)();
    }

private:
    LPAPlayer *mPlayer;
    void (LPAPlayer::*mMethod)();

    TimedEvent(const TimedEvent &);
    TimedEvent &operator=(const TimedEvent &);
};

}  // namespace android

#endif  // LPA_PLAYER_H_

