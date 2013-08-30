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

//#define LOG_NDEBUG 0
#define LOG_TAG "NuPlayerStats"

#include <utils/Log.h>
#include <cutils/properties.h>
#include "NuPlayerStats.h"

#define NO_MIMETYPE_AVAILABLE "N/A"

namespace android {

NuPlayerStats::NuPlayerStats() {
    char value[PROPERTY_VALUE_MAX];
    mStatistics = false;
    property_get("persist.debug.sf.statistics", value, "0");
    if(atoi(value))
    {
      Mutex::Autolock autoLock(mStatsLock);
      mStatistics = true;
      mNumVideoFramesDecoded = 0;
      mNumVideoFramesDropped = 0;
      mConsecutiveFramesDropped = 0;
      mCatchupTimeStart = 0;
      mNumTimesSyncLoss = 0;
      mMaxEarlyDelta = 0;
      mMaxLateDelta = 0;
      mMaxTimeSyncLoss = 0;
      mTotalFrames = 0;
      mFirstFrameLatencyStartUs = getTimeOfDayUs();
      mStatisticsFrames = 0;
      mFPSSumUs = 0;
      mVeryFirstFrame = true;
      mSeekPerformed = false;
      mTotalTime = 0;
      mFirstFrameTime = 0;
      mTotalRenderingFrames = 0;
      mVideoEOS = false;
      mAudioEOS = false;
      mPlaying = true;
      mLastSeekTimeUs = 0;
      mPauseTimeUs = 0;
  }
}

NuPlayerStats::~NuPlayerStats() {
    Mutex::Autolock autoLock(mStatsLock);
    mStatistics = false;
}

void NuPlayerStats::setVeryFirstFrame(bool vff) {
    Mutex::Autolock autoLock(mStatsLock);
    mVeryFirstFrame = true;
}

void NuPlayerStats::notifySeek() {
    Mutex::Autolock autoLock(mStatsLock);
    mFirstFrameLatencyStartUs = getTimeOfDayUs();
    mSeekPerformed = true;
}

void NuPlayerStats::incrementTotalFrames() {
    Mutex::Autolock autoLock(mStatsLock);
    mTotalFrames++;
}

void NuPlayerStats::incrementTotalRenderingFrames() {
    Mutex::Autolock autoLock(mStatsLock);
    mTotalRenderingFrames++;
}

void NuPlayerStats::incrementDroppedFrames() {
    Mutex::Autolock autoLock(mStatsLock);
    mNumVideoFramesDropped++;
}

void NuPlayerStats::logStatistics() {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);

        ALOGW("Total Video Frames Decoded(%llu)",mTotalFrames);
        ALOGW("Total Video Frames Rendered(%lld)", mTotalRenderingFrames);
        ALOGW("numVideoFramesDropped(%llu)", mNumVideoFramesDropped);
    }
}

void NuPlayerStats::logPause(int64_t positionUs) {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        mPauseTimeUs = positionUs;
    }
}

void NuPlayerStats::logSeek(int64_t seekTimeUs) {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        mLastSeekTimeUs = seekTimeUs;
    }
}

void NuPlayerStats::logEOS(bool audioEOS, bool videoEOS) {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        mAudioEOS = audioEOS;
        mVideoEOS = videoEOS;
    }
}

void NuPlayerStats::recordLate(int64_t ts, int64_t clock, int64_t delta, int64_t anchorTime) {
    Mutex::Autolock autoLock(mStatsLock);
    mNumVideoFramesDropped++;
    mConsecutiveFramesDropped++;
    if (mConsecutiveFramesDropped == 1){
      mCatchupTimeStart = anchorTime;
    }

    logLate(ts,clock,delta);
}

void NuPlayerStats::recordOnTime(int64_t ts, int64_t clock, int64_t delta) {
    Mutex::Autolock autoLock(mStatsLock);
    mNumVideoFramesDecoded++;
    mConsecutiveFramesDropped = 0;
    logOnTime(ts,clock,delta);
}

void NuPlayerStats::logSyncLoss() {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        ALOGW("Number of times AV Sync Lost(%u)", mNumTimesSyncLoss);
        ALOGW("Max Video Ahead Time Delta(%u)", -mMaxEarlyDelta/1000);
        ALOGW("Max Video Behind Time Delta(%u)", mMaxLateDelta/1000);
        ALOGW("Max Time Sync Loss(%u)",mMaxTimeSyncLoss/1000);
    }
}

void NuPlayerStats::logFps() {
    if (mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        if(mTotalRenderingFrames < 2){
           mFirstFrameTime = getTimeOfDayUs();
        }

        mTotalTime = getTimeOfDayUs() - mFirstFrameTime;

        if(mSeekPerformed) {
            mVeryFirstFrame = false;
            mSeekPerformed = false;
        } else if(mVeryFirstFrame) {
            logFirstFrame();
            ALOGW("setting first frame time");
        }
    }
}

void NuPlayerStats::logFpsSummary(bool bPlaying) {
    if (mStatistics) {
        mPlaying = bPlaying;
        ALOGW("=========================================================\n");
        logStatistics();
        {
            Mutex::Autolock autoLock(mStatsLock);
            ALOGW("Average Frames Per Second(%.4f)", (mTotalTime == 0)? 0.0 : ((double)(mTotalRenderingFrames-1)*1E6)/((double)mTotalTime));
        }
        ALOGW("Last Seek To Time(%lld ms)", mLastSeekTimeUs/1000);
        ALOGW("Last Paused Time(%lld ms)", mPauseTimeUs/1000);
        logSyncLoss();
        ALOGW("EOS(%d)", mVideoEOS);
        ALOGW("PLAYING(%d)", mPlaying);
        ALOGW("=========================================================\n");
    }
}

int64_t NuPlayerStats::getTimeOfDayUs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// WARNING: Most private functions are only thread-safe within mStatsLock
inline void NuPlayerStats::logFirstFrame() {
    ALOGW("=====================================================\n");
    ALOGW("First frame latency: %lld ms\n",(getTimeOfDayUs()-mFirstFrameLatencyStartUs)/1000);
    ALOGW("=====================================================\n");
    mVeryFirstFrame = false;
}

inline void NuPlayerStats::logCatchUp(int64_t ts, int64_t clock, int64_t delta) {
    if (mConsecutiveFramesDropped > 0) {
        mNumTimesSyncLoss++;
        if (mMaxTimeSyncLoss < (clock - mCatchupTimeStart) && clock > 0 && ts > 0) {
            mMaxTimeSyncLoss = clock - mCatchupTimeStart;
        }
    }
}

inline void NuPlayerStats::logLate(int64_t ts, int64_t clock, int64_t delta) {
    if (mMaxLateDelta < delta && clock > 0 && ts > 0) {
        mMaxLateDelta = delta;
    }
}

inline void NuPlayerStats::logOnTime(int64_t ts, int64_t clock, int64_t delta) {
    bool needLogLate = false;
    logCatchUp(ts, clock, delta);
    if (delta <= 0) {
        if ((-delta) > (-mMaxEarlyDelta) && clock > 0 && ts > 0) {
            mMaxEarlyDelta = delta;
        }
    }
    else {
        needLogLate = true;
    }

    if(needLogLate) logLate(ts, clock, delta);
}

} // namespace android
