/* Copyright (c) 2013 - 2014, The Linux Foundation. All rights reserved.
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
#ifndef EXTENDED_STATS_H_
#define EXTENDED_STATS_H_

#include <inttypes.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/KeyedVector.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

#define MEDIA_EXTENDED_STATS "MediaExtendedStats"

#define STATS_PROFILE_START_LATENCY "Total startup latency"
#define STATS_PROFILE_ALLOCATE_NODE(isVideo) (isVideo != 0 ? "\tAllocate node (video)" : "\tAllocate node (audio)")
#define STATS_PROFILE_ALLOCATE_INPUT(isVideo) (isVideo != 0 ? "\tAllocate input buffer (video)" : "\tAllocate input buffer (audio)")
#define STATS_PROFILE_ALLOCATE_OUTPUT(isVideo) (isVideo != 0 ? "\tAllocate output buffer (video)" : "\tAllocate output buffer (audio)")
#define STATS_PROFILE_CONFIGURE_CODEC(isVideo) (isVideo != 0 ? "\tConfigure codec (video)" : "\tConfigure codec (audio)")
#define STATS_PROFILE_FIRST_BUFFER(isVideo) (isVideo != 0 ? "Time to process first buffer (video)" : "Time to process first buffer (audio)")
#define STATS_PROFILE_PREPARE "Prepare"
#define STATS_PROFILE_SET_DATA_SOURCE "Set data source"
#define STATS_PROFILE_PAUSE "Pause"
#define STATS_PROFILE_SEEK "Seek"
#define STATS_PROFILE_RESUME "Resume"

#define STATS_PROFILE_SET_CAMERA_SOURCE "Set camera source"
#define STATS_PROFILE_SET_ENCODER(isVideo) (isVideo != 0 ? "Set video encoder" : "Set audio encoder")
#define STATS_PROFILE_STOP "Stop"
#define STATS_BITRATE "Video Bitrate"
#define STATS_PROFILE_SF_RECORDER_START_LATENCY "\tStagefrightRecorder start latency"
#define STATS_PROFILE_CAMERA_SOURCE_START_LATENCY "\tCamera source start latency"
#define STATS_PROFILE_RECONFIGURE "\tReconfigure latency"

namespace android {

/*
 * This class provides support for profiling events and dumping aggregate
 * statistics. It may be used to profile latencies at startup, seek, resume
 * and to report dropped frames etc.
 */
typedef int64_t statsDataType;
class MediaExtendedStats;

class ExtendedStats : public RefBase {

public:

    enum {
        MEDIA_STATS_FLAG = 'MeSt',
    };

    explicit ExtendedStats(const char* id, pid_t tid);

    // Evaluative item; associated with an operation
    struct LogEntry : public RefBase {
        LogEntry();
        virtual ~LogEntry() { mData = 0;}
        virtual void insert(statsDataType) { }
        virtual void dump(const char* label) const;
        virtual void reset() {mData = 0;}
        inline statsDataType data() const { return mData; }
    protected:
        statsDataType mData;
    };

    // Supported type of MediaExtendedStats
    enum StatsType {
        PLAYER,
        RECORDER,
    };

    // Supported evaluations (and hence possible variants of 'LogEntry's)
    enum LogType {
        AVERAGE = 1 << 0,
        MOVING_AVERAGE = 1 << 1,
        PROFILE = 1 << 2,
    };

    enum {
        PROFILE_START = 1,
        PROFILE_START_ONCE,
        PROFILE_STOP,
    };

    static const size_t kMaxStringLength = 1024;
    static const int32_t kMaxWindowSize = 120;

    struct AutoProfile {
        AutoProfile(const char* eventName, sp<MediaExtendedStats> mediaExtendedStats = NULL,
                bool condition = true, bool profileOnce = false);
        ~AutoProfile();

        private:
            AString mEventName;
            sp<ExtendedStats::LogEntry> mLog;
            sp<ExtendedStats> mStats;
            bool mCondition;
    };

    ~ExtendedStats();
    void log(LogType type, const char* key, statsDataType value, bool condition = true);
    virtual void dump(const char* key = NULL);
    virtual void reset(const char* key);
    virtual void clear();

    static int64_t getSystemTime() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1E6 + tv.tv_usec;
    }

    static sp<LogEntry> createLogEntry(LogType type, int32_t windowSize);

    //only profile once, as opposed to up to kMaxOccurrences
    inline void profileStartOnce(const char* name, bool condition = true) {
        log(PROFILE, name, PROFILE_START_ONCE, condition);
    }

    //wrapper function to start profiling latency
    inline void profileStart(const char* name, bool condition = true) {
        log(PROFILE, name, PROFILE_START, condition);
    }

    //wrapper function to stop profiling. Name must match the name from profileStart
    inline void profileStop(const char* name) {
        log(PROFILE, name, PROFILE_STOP);
    }

    static MediaExtendedStats* Create(enum StatsType statsType, const char* name, pid_t tid);

    //wrapper function to set window size.
    inline void setWindowSize(int32_t windowSize) {
        mWindowSize = windowSize;
    }

private:
    sp<LogEntry> getLogEntry(const char *key, LogType type);

protected:
    KeyedVector<AString, sp<LogEntry> > mLogEntry;
    Mutex mLock;

    ExtendedStats(const ExtendedStats&) {}
    AString mName;
    pid_t mTid;

    int32_t mWindowSize;
};

inline ExtendedStats::LogType operator| (ExtendedStats::LogType a, ExtendedStats::LogType b) {
    return static_cast<ExtendedStats::LogType>(static_cast<int>(a) | static_cast<int>(b));
}

/**************************** MediaExtendedStats *********************/

class MediaExtendedStats : public RefBase {
public:
    explicit MediaExtendedStats(const char* name, pid_t tid);

    void logFrameDropped();
    void logDimensions(int32_t width, int32_t height);
    void logBitRate(int64_t frameSize, int64_t timestamp);

    //only profile once, as opposed to up to kMaxOccurrences
    inline void profileStartOnce(const char* name, bool condition = true) {
        mProfileTimes->profileStartOnce(name, condition);
    }

    //wrapper function to start profiling latency
    inline void profileStart(const char* name, bool condition = true) {
        mProfileTimes->profileStart(name, condition);
    }

    //wrapper function to stop profiling. Name must match the name from profileStart
    inline void profileStop(const char* name) {
        mProfileTimes->profileStop(name);
    }

    sp<ExtendedStats> getProfileTimes() {
        return mProfileTimes;
    }
    virtual void reset();

    virtual void notifyPause(int64_t pauseTimeUs) = 0;
    virtual void dump() = 0;

    int32_t setFrameRate(int32_t frameRate) {
        mFrameRate = frameRate;
        mProfileTimes->setWindowSize(mFrameRate);
    }

protected:
    AString mName;
    pid_t mTid;

    int64_t mCurrentConsecutiveFramesDropped;
    int64_t mMaxConsecutiveFramesDropped;
    int64_t mNumChainedDrops;
    int64_t mFramesDropped;

    int64_t mLastPauseTime;

    Vector<int32_t> mWidthDimensions;
    Vector<int32_t> mHeightDimensions;

    sp<ExtendedStats> mProfileTimes;
    int32_t mFrameRate;
    Mutex mLock;

    /* helper functions */
    void resetConsecutiveFramesDropped();

    virtual ~MediaExtendedStats();
};

/************************* PlayerExtendedStats *************************/

class PlayerExtendedStats : public MediaExtendedStats {

public:
    explicit PlayerExtendedStats(const char* name, pid_t tid);

    void logFrameRendered();

    //functions to alert the logger of discontinuities in playback
    void notifyPlaying(bool isPlaying);
    void notifySeek(int64_t seekTimeUs);
    void notifySeekDone();
    void notifyEOS();

    virtual void reset();
    virtual void dump();
    virtual void notifyPause(int64_t pauseTimeUs);

private:
    int64_t mFramesRendered;

    int64_t mTotalPlayingTime;
    int64_t mStartPlayingTime;
    int64_t mLastSeekTime;

    bool mEOS;
    bool mPlaying;
    bool mPaused; //used as a flag for seeking while paused

    void updateTotalPlayingTime(bool wasPlaying);
};

class RecorderExtendedStats : public MediaExtendedStats {
public:
    explicit RecorderExtendedStats(const char* name, pid_t tid);

    void logFrameEncoded();
    void logRecordingDuration(int64_t duration);

    virtual void reset();
    virtual void dump();
    virtual void notifyPause(int64_t pauseTimeUs);

private:
    int64_t mFramesEncoded;
    int64_t mTotalRecordingTime;
};

}
#endif  //EXTENDED_STATS_H_
