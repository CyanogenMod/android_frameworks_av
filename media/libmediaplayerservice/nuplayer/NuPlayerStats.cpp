/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
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
#define LOG_TAG "NuPlayerStats"
#include <utils/Log.h>

#include "NuPlayerStats.h"
#include <cutils/properties.h>

#define NO_MIMETYPE_AVAILABLE "N/A"

namespace android {

NuPlayerStats::NuPlayerStats() {
    char value[PROPERTY_VALUE_MAX];
    mStatistics = false;
    property_get("persist.debug.sf.statistics", value, "0");
    if(atoi(value)) mStatistics = true;
    {
        Mutex::Autolock autoLock(mStatsLock);
        mMIME = new char[strlen(NO_MIMETYPE_AVAILABLE)+1];
        strcpy(mMIME,NO_MIMETYPE_AVAILABLE);
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
        mLastFrame = 0;
        mLastFrameUs = 0;
        mStatisticsFrames = 0;
        mFPSSumUs = 0;
        mVeryFirstFrame = true;
        mSeekPerformed = false;
        mTotalTime = 0;
        mFirstFrameTime = 0;
    }
}

NuPlayerStats::~NuPlayerStats() {
    if(mMIME) {
        delete mMIME;
    }
}

void NuPlayerStats::setMime(const char* mime) {
    Mutex::Autolock autoLock(mStatsLock);
    if(mime != NULL) {
        int mimeLen = strlen(mime);
	if(mMIME) {
            delete mMIME;
        }

        mMIME = new char[mimeLen+1];
        strcpy(mMIME,mime);
    }
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

void NuPlayerStats::incrementDroppedFrames() {
    Mutex::Autolock autoLock(mStatsLock);
    mNumVideoFramesDropped++;
}

void NuPlayerStats::logStatistics() {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        ALOGW("=====================================================");
        ALOGW("Mime Type: %s",mMIME);
        ALOGW("Number of frames dropped: %lld",mNumVideoFramesDropped);
        ALOGW("Number of frames rendered: %llu",mTotalFrames);
        ALOGW("=====================================================");
    }
}

void NuPlayerStats::logPause(int64_t positionUs) {
    if(mStatistics) {
        ALOGW("=====================================================");
        ALOGW("Pause position: %lld ms",positionUs/1000);
        ALOGW("=====================================================");
    }
}

void NuPlayerStats::logSeek(int64_t seekTimeUs) {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        ALOGW("=====================================================");
        ALOGW("Seek position: %lld ms",seekTimeUs/1000);
        ALOGW("Seek latency: %lld ms",(getTimeOfDayUs() - mFirstFrameLatencyStartUs)/1000);
        ALOGW("=====================================================");
    }
}

void NuPlayerStats::recordLate(int64_t ts, int64_t clock, int64_t delta, int64_t anchorTime) {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        mNumVideoFramesDropped++;
        mConsecutiveFramesDropped++;
        if (mConsecutiveFramesDropped == 1){
			mCatchupTimeStart = anchorTime;
        }

        logLate(ts,clock,delta);
    }
}

void NuPlayerStats::recordOnTime(int64_t ts, int64_t clock, int64_t delta) {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        mNumVideoFramesDecoded++;
        mConsecutiveFramesDropped = 0;
        logOnTime(ts,clock,delta);
    }
}

void NuPlayerStats::logSyncLoss() {
    if(mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        ALOGW("=====================================================");
        ALOGW("Number of times AV Sync Losses = %u", mNumTimesSyncLoss);
        ALOGW("Max Video Ahead time delta = %u", -mMaxEarlyDelta/1000);
        ALOGW("Max Video Behind time delta = %u", mMaxLateDelta/1000);
        ALOGW("Max Time sync loss = %u",mMaxTimeSyncLoss/1000);
        ALOGW("=====================================================");
    }
}

void NuPlayerStats::logFps() {
    if (mStatistics) {
        Mutex::Autolock autoLock(mStatsLock);
        if(mTotalFrames < 2){
           mLastFrameUs = getTimeOfDayUs();
           mFirstFrameTime = getTimeOfDayUs();
        }

        mTotalTime = getTimeOfDayUs() - mFirstFrameTime;
        int64_t now = getTimeOfDayUs();
        int64_t diff = now - mLastFrameUs;
        if (diff > 250000 && !mVeryFirstFrame) {
             double fps =((mTotalFrames - mLastFrame) * 1E6)/diff;
             if (mStatisticsFrames == 0) {
                 fps =((mTotalFrames - mLastFrame - 1) * 1E6)/diff;
             }
             ALOGW("Frames per second: %.4f, Duration of measurement: %lld", fps,diff);
             mFPSSumUs += fps;
             ++mStatisticsFrames;
             mLastFrameUs = now;
             mLastFrame = mTotalFrames;
         }

        if(mSeekPerformed) {
            mVeryFirstFrame = false;
            mSeekPerformed = false;
        } else if(mVeryFirstFrame) {
            logFirstFrame();
            ALOGW("setting first frame time");
            mLastFrameUs = getTimeOfDayUs();
        }
    }
}

void NuPlayerStats::logFpsSummary() {
    if (mStatistics) {
        logStatistics();
        logSyncLoss();
        {
            Mutex::Autolock autoLock(mStatsLock);
            ALOGW("=========================================================");
            ALOGW("Average Frames Per Second: %.4f", mFPSSumUs/((double)mStatisticsFrames));
            ALOGW("Total Frames / Total Time: %.4f", ((double)(mTotalFrames-1)*1E6)/((double)mTotalTime));
            ALOGW("========================================================");
        }
    }
}

int64_t NuPlayerStats::getTimeOfDayUs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// WARNING: Most private functions are only thread-safe within mStatsLock
inline void NuPlayerStats::logFirstFrame() {
    ALOGW("=====================================================");
    ALOGW("First frame latency: %lld ms",(getTimeOfDayUs()-mFirstFrameLatencyStartUs)/1000);
    ALOGW("=====================================================");
    mVeryFirstFrame = false;
}

inline void NuPlayerStats::logCatchUp(int64_t ts, int64_t clock, int64_t delta) {
    if(mStatistics) {
        if (mConsecutiveFramesDropped > 0) {
            mNumTimesSyncLoss++;
            if (mMaxTimeSyncLoss < (clock - mCatchupTimeStart) && clock > 0 && ts > 0) {
                mMaxTimeSyncLoss = clock - mCatchupTimeStart;
            }
        }
    }
}

inline void NuPlayerStats::logLate(int64_t ts, int64_t clock, int64_t delta) {
    if(mStatistics) {
        if (mMaxLateDelta < delta && clock > 0 && ts > 0) {
            mMaxLateDelta = delta;
        }
    }
}

inline void NuPlayerStats::logOnTime(int64_t ts, int64_t clock, int64_t delta) {
    if(mStatistics) {
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
}

} // namespace android
