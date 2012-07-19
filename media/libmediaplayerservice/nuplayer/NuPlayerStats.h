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

#ifndef NUPLAYER_STATS_H_

#define NUPLAYER_STATS_H_

#include <utils/RefBase.h>
#include <utils/threads.h>

namespace android {

class NuPlayerStats : public RefBase {
  public:
    NuPlayerStats();
    ~NuPlayerStats();

    void setMime(const char* mime);
    void setVeryFirstFrame(bool vff);
    void notifySeek();
    void incrementTotalFrames();
    void incrementDroppedFrames();
    void logStatistics();
    void logPause(int64_t positionUs);
    void logSeek(int64_t seekTimeUs);
    void recordLate(int64_t ts, int64_t clock, int64_t delta, int64_t anchorTime);
    void recordOnTime(int64_t ts, int64_t clock, int64_t delta);
    void logSyncLoss();
    void logFps();
    void logFpsSummary();
    static int64_t getTimeOfDayUs();

  private:
    void logFirstFrame();
    void logCatchUp(int64_t ts, int64_t clock, int64_t delta);
    void logLate(int64_t ts, int64_t clock, int64_t delta);
    void logOnTime(int64_t ts, int64_t clock, int64_t delta);

    mutable Mutex mStatsLock;
    bool mStatistics;
    char* mMIME;
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
    int64_t mLastFrame;
    int64_t mLastFrameUs;
    double mFPSSumUs;
    int64_t mStatisticsFrames;
    bool mVeryFirstFrame;
    bool mSeekPerformed;
    int64_t mTotalTime;
    int64_t mFirstFrameTime;
};

} // namespace android

#endif // NUPLAYER_STATS_H_
