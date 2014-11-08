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

#ifndef NUPLAYER_RENDERER_H_

#define NUPLAYER_RENDERER_H_

#include "NuPlayer.h"

namespace android {

struct ABuffer;
struct VideoFrameScheduler;

struct NuPlayer::Renderer : public AHandler {
    enum Flags {
        FLAG_REAL_TIME = 1,
        FLAG_OFFLOAD_AUDIO = 2,
    };
    Renderer(const sp<MediaPlayerBase::AudioSink> &sink,
             const sp<AMessage> &notify,
             uint32_t flags = 0);

    static size_t AudioSinkCallback(
            MediaPlayerBase::AudioSink *audioSink,
            void *data, size_t size, void *me,
            MediaPlayerBase::AudioSink::cb_event_t event);

    void queueBuffer(
            bool audio,
            const sp<ABuffer> &buffer,
            const sp<AMessage> &notifyConsumed);

    void queueEOS(bool audio, status_t finalResult);

    void flush(bool audio);

    void signalTimeDiscontinuity();

    void signalAudioSinkChanged();

    void signalDisableOffloadAudio();

    void pause();
    void resume();

    void setVideoFrameRate(float fps);

    // Following setters and getters are protected by mTimeLock.
    status_t getCurrentPosition(int64_t *mediaUs);
    status_t getCurrentPosition(
            int64_t *mediaUs, int64_t nowUs, bool allowPastQueuedVideo = false);
    void setHasMedia(bool audio);
    void setAudioFirstAnchorTime(int64_t mediaUs);
    void setAudioFirstAnchorTimeIfNeeded(int64_t mediaUs);
    void setAnchorTime(
            int64_t mediaUs, int64_t realUs, int64_t numFramesWritten = -1, bool resume = false);
    void setVideoLateByUs(int64_t lateUs);
    int64_t getVideoLateByUs();
    void setPauseStartedTimeRealUs(int64_t realUs);

    bool openAudioSink(
            const sp<AMessage> &format,
            bool offloadOnly,
            bool hasVideo,
            uint32_t flags);
    void closeAudioSink();

    enum {
        kWhatEOS                 = 'eos ',
        kWhatFlushComplete       = 'fluC',
        kWhatPosition            = 'posi',
        kWhatVideoRenderingStart = 'vdrd',
        kWhatMediaRenderingStart = 'mdrd',
        kWhatAudioOffloadTearDown = 'aOTD',
        kWhatAudioOffloadPauseTimeout = 'aOPT',
    };

    enum AudioOffloadTearDownReason {
        kDueToError = 0,
        kDueToTimeout,
    };

protected:
    virtual ~Renderer();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatDrainAudioQueue     = 'draA',
        kWhatDrainVideoQueue     = 'draV',
        kWhatPostDrainVideoQueue = 'pDVQ',
        kWhatQueueBuffer         = 'queB',
        kWhatQueueEOS            = 'qEOS',
        kWhatFlush               = 'flus',
        kWhatAudioSinkChanged    = 'auSC',
        kWhatPause               = 'paus',
        kWhatResume              = 'resm',
        kWhatOpenAudioSink       = 'opnA',
        kWhatCloseAudioSink      = 'clsA',
        kWhatStopAudioSink       = 'stpA',
        kWhatDisableOffloadAudio = 'noOA',
        kWhatSetVideoFrameRate   = 'sVFR',
    };

    struct QueueEntry {
        sp<ABuffer> mBuffer;
        sp<AMessage> mNotifyConsumed;
        size_t mOffset;
        status_t mFinalResult;
        int32_t mBufferOrdinal;
    };

    static const int64_t kMinPositionUpdateDelayUs;

    sp<PlayerExtendedStats> mPlayerExtendedStats;
    sp<MediaPlayerBase::AudioSink> mAudioSink;
    sp<AMessage> mNotify;
    Mutex mLock;
    uint32_t mFlags;
    List<QueueEntry> mAudioQueue;
    List<QueueEntry> mVideoQueue;
    uint32_t mNumFramesWritten;
    sp<VideoFrameScheduler> mVideoScheduler;

    bool mDrainAudioQueuePending;
    bool mDrainVideoQueuePending;
    int32_t mAudioQueueGeneration;
    int32_t mVideoQueueGeneration;

    Mutex mTimeLock;
    // |mTimeLock| protects the following 7 member vars that are related to time.
    // Note: those members are only written on Renderer thread, so reading on Renderer thread
    // doesn't need to be protected. Otherwise accessing those members must be protected by
    // |mTimeLock|.
    // TODO: move those members to a seperated media clock class.
    int64_t mAudioFirstAnchorTimeMediaUs;
    int64_t mAnchorTimeMediaUs;
    int64_t mAnchorTimeRealUs;
    int64_t mAnchorNumFramesWritten;
    int64_t mAnchorMaxMediaUs;
    int64_t mVideoLateByUs;
    bool mHasAudio;
    bool mHasVideo;
    int64_t mPauseStartedTimeRealUs;

    Mutex mFlushLock;  // protects the following 2 member vars.
    bool mFlushingAudio;
    bool mFlushingVideo;

    bool mSyncQueues;

    bool mPaused;
    bool mVideoSampleReceived;
    bool mVideoRenderingStarted;
    int32_t mVideoRenderingStartGeneration;
    int32_t mAudioRenderingStartGeneration;

    int64_t mLastPositionUpdateUs;

    int32_t mAudioOffloadPauseTimeoutGeneration;
    bool mAudioOffloadTornDown;
    audio_offload_info_t mCurrentOffloadInfo;

    int32_t mTotalBuffersQueued;
    int32_t mLastAudioBufferDrained;


    size_t fillAudioBuffer(void *buffer, size_t size);

    bool onDrainAudioQueue();
    int64_t getPendingAudioPlayoutDurationUs(int64_t nowUs);
    int64_t getPlayedOutAudioDurationUs(int64_t nowUs);
    void postDrainAudioQueue_l(int64_t delayUs = 0);

    void onNewAudioMediaTime(int64_t mediaTimeUs);
    int64_t getRealTimeUs(int64_t mediaTimeUs, int64_t nowUs);

    void onDrainVideoQueue();
    void postDrainVideoQueue();

    void prepareForMediaRenderingStart();
    void notifyIfMediaRenderingStarted();

    void onQueueBuffer(const sp<AMessage> &msg);
    void onQueueEOS(const sp<AMessage> &msg);
    void onFlush(const sp<AMessage> &msg);
    void onAudioSinkChanged();
    void onDisableOffloadAudio();
    void onPause();
    void onResume();
    void onSetVideoFrameRate(float fps);
    void onAudioOffloadTearDown(AudioOffloadTearDownReason reason);
    bool onOpenAudioSink(
            const sp<AMessage> &format,
            bool offloadOnly,
            bool hasVideo,
            uint32_t flags);
    void onCloseAudioSink();

    void notifyEOS(bool audio, status_t finalResult, int64_t delayUs = 0);
    void notifyFlushComplete(bool audio);
    void notifyPosition();
    void notifyVideoLateBy(int64_t lateByUs);
    void notifyVideoRenderingStart();
    void notifyAudioOffloadTearDown();

    void flushQueue(List<QueueEntry> *queue);
    bool dropBufferWhileFlushing(bool audio, const sp<AMessage> &msg);
    void syncQueuesDone_l();

    bool offloadingAudio() const { return (mFlags & FLAG_OFFLOAD_AUDIO) != 0; }

    void startAudioOffloadPauseTimeout();
    void cancelAudioOffloadPauseTimeout();

    DISALLOW_EVIL_CONSTRUCTORS(Renderer);
};

}  // namespace android

#endif  // NUPLAYER_RENDERER_H_
