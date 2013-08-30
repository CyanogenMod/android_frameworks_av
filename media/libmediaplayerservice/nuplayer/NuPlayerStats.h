/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef DASHPLAYER_STATS_H_

#define DASHPLAYER_STATS_H_

#include <utils/RefBase.h>
#include <utils/threads.h>

namespace android {

class NuPlayerStats : public RefBase {
  public:
    NuPlayerStats();
    ~NuPlayerStats();

    void setVeryFirstFrame(bool vff);
    void notifySeek();
    void incrementTotalFrames();
    void incrementDroppedFrames();
    void logStatistics();
    void logPause(int64_t positionUs);
    void logSeek(int64_t seekTimeUs);
    void logEOS(bool audioEOS, bool videoEOS);
    void logPosition(int64_t timeUs);
    void recordLate(int64_t ts, int64_t clock, int64_t delta, int64_t anchorTime);
    void recordOnTime(int64_t ts, int64_t clock, int64_t delta);
    void logSyncLoss();
    void logFps();
    void logFpsSummary(bool bPlaying = true);
    static int64_t getTimeOfDayUs();
    void incrementTotalRenderingFrames();
    void notifyBufferingEvent();
    void setFileDescAndOutputStream(int fd);

  private:
    void logFirstFrame();
    void logCatchUp(int64_t ts, int64_t clock, int64_t delta);
    void logLate(int64_t ts, int64_t clock, int64_t delta);
    void logOnTime(int64_t ts, int64_t clock, int64_t delta);

    mutable Mutex mStatsLock;
    bool mStatistics;
    int64_t mNumVideoFramesDecoded;
    int64_t mNumVideoFramesDropped;
    int64_t mConsecutiveFramesDropped;
    uint32_t mCatchupTimeStart;
    uint32_t mNumTimesSyncLoss;
    uint32_t mMaxEarlyDelta;
    uint32_t mMaxLateDelta;
    uint32_t mMaxTimeSyncLoss;
    uint64_t mTotalFrames;
    int64_t mFirstFrameLatencyStartUs;
    double mFPSSumUs;
    int64_t mStatisticsFrames;
    bool mVeryFirstFrame;
    bool mSeekPerformed;
    int64_t mTotalTime;
    int64_t mFirstFrameTime;
    uint64_t mTotalRenderingFrames;
    bool mBufferingEvent;
    bool mAudioEOS;
    bool mVideoEOS;
    bool mPlaying;
    int64_t mPauseTimeUs;
    int64_t mLastSeekTimeUs;
};

} // namespace android

#endif // DASHPLAYER_STATS_H_
