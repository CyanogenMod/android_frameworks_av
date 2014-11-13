/*Copyright (c) 2013 - 2014, The Linux Foundation. All rights reserved.
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
#define LOG_TAG "ExtendedStats"
#include <ctype.h>
#include <inttypes.h>
#include <media/stagefright/ExtendedStats.h>
#include <media/stagefright/foundation/ADebug.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <cutils/properties.h>

namespace android {

/* constructors and destructors */
ExtendedStats::ExtendedStats(const char *id, pid_t tid) {
    mLogEntry.clear();
    mName.setTo(id);
    mTid = tid;
}

ExtendedStats::~ExtendedStats() {
    mLogEntry.clear();
}

ExtendedStats::LogEntry::LogEntry()
    : mData(0) {
}

ExtendedStats::StatsFrameInfoWrapper::StatsFrameInfoWrapper(const StatsFrameInfoWrapper& copy) {
    infoPtr = copy.infoPtr;
}

ExtendedStats::StatsFrameInfoWrapper::StatsFrameInfoWrapper(StatsFrameInfo* oInfoPtr) : infoPtr(oInfoPtr) { }

ExtendedStats::StatsFrameInfo::StatsFrameInfo() {
    size = 0;
    timestamp = 0;
}

ExtendedStats::TimeBoundVector::TimeBoundVector(StatsFrameInfoPool& infoPool) : mFrameInfoPool(infoPool) {
    mCurrBoundedSum = 0;
    mMaxBoundedAvg = 0;
    mTotalNumBuffers = 0;
    mTotalSizeSum = 0;
}


void ExtendedStats::LogEntry::dump(const char* label) const {
    ALOGI("%s : %" PRId64 "", label, mData);
}

struct Min : public ExtendedStats::LogEntry {
    Min() {
        mData = INT64_MAX;
    }
    void insert(statsDataType value) {
        if (value < mData)
            mData = value;
    }
};

struct Max : public ExtendedStats::LogEntry {
    void insert(statsDataType value) {
        if (value > mData)
            mData = value;
    }
};

// Accumulates inserted values
struct Total : public ExtendedStats::LogEntry {
    Total() {}
    ~Total() {}
    void insert(statsDataType value) {
        mData += value;
    }
};

// Accumulates running-average of inserted values
struct Average : public ExtendedStats::LogEntry {
    void insert(statsDataType value) {
        mN++;
        mSum += value;
        mData = mSum / mN;
        mRem = mSum % mN;
    }
    Average() {
        mN = 0;
        mSum = 0;
        mRem = 0;
    }
    int32_t mN;
    int32_t mSum;
    int32_t mRem;
};

// Saves inserted values in a bound array
struct Archive : public ExtendedStats::LogEntry {

    Archive() : mIndex(0), mCache() /* zeroes out mCache */ { }

    static const int kMaxOccurrences = 8;

    virtual void insert(statsDataType value) {
        if (mIndex >= kMaxOccurrences)
            mIndex = kMaxOccurrences - 1;
        mCache[mIndex++] = value;
    }
    void dump(const char* label) const {
        if (mIndex != 0) {
            char temp[ExtendedStats::kMaxStringLength] = {0};

            for(int i = 0; i < mIndex; ++i) {
                snprintf(temp + strlen(temp), ExtendedStats::kMaxStringLength, "\t%" PRId64 "", mCache[i]);
            }

            ALOGI("%s: %s", label, temp);
        }
    }
protected:
    int32_t mIndex;
    statsDataType mCache[kMaxOccurrences];
};

// Profiles and saves the delay between insertion of START and STOP
struct TimeProfile : Archive {
    TimeProfile(): mStartingTimeIndex(0), mStartTimesCache() {}
    ~TimeProfile() {}

    virtual void insert(statsDataType value) {
        if (value == ExtendedStats::PROFILE_START) {
            if (mStartingTimeIndex >= kMaxOccurrences) {
                mStartingTimeIndex = kMaxOccurrences - 1;
            }
            mStartTimesCache[mStartingTimeIndex++] = ExtendedStats::getSystemTime();
        } else if (value == ExtendedStats::PROFILE_START_ONCE) {
            //only profile first occurrence
            if (mStartingTimeIndex == 0)  {
                mStartTimesCache[mStartingTimeIndex++] = ExtendedStats::getSystemTime();
            }
        } else if (value == ExtendedStats::PROFILE_STOP) {
            if (mIndex >= kMaxOccurrences) {
                mIndex = kMaxOccurrences - 1;
            }
            if (mStartTimesCache[mIndex] != 0) { //i.e. we've called START before
                mCache[mIndex] = ExtendedStats::getSystemTime() - mStartTimesCache[mIndex];
                mIndex++;
            }
        }
    }
    void dump(const char* label) const {
        if (mIndex != 0) {
            char temp[ExtendedStats::kMaxStringLength] = {0};

            for(int i = 0; i < mIndex; ++i) {
                snprintf(temp + strlen(temp),
                    ExtendedStats::kMaxStringLength, "\t%0.2f", mCache[i] / 1E3);
            }

            ALOGI("%s (ms): %s", label, temp);
        }
    }

private:
    int32_t mStartingTimeIndex;
    statsDataType mStartTimesCache[kMaxOccurrences];
};

//static
// LogEntry factory
sp<ExtendedStats::LogEntry> ExtendedStats::createLogEntry(LogType type) {
    switch(type) {
        case TOTAL:
            return new Total();
        case AVERAGE:
            return new Average();
        case PROFILE:
            return new TimeProfile();
        case MAX:
            return new Max();
        case MIN:
            return new Min();
        default:
           return new LogEntry();
    }
}

sp<ExtendedStats::LogEntry> ExtendedStats::getLogEntry(const char *key,
        LogType type) {
    if (!key)
        return NULL;

    ssize_t idx = mLogEntry.indexOfKey(key);

    /* if this entry doesn't exist, add it in the log and return it */
    if (idx < 0) {
        /* keep write access to the KeyedVector thread-safe */
        Mutex::Autolock autoLock(mLock);
        sp<LogEntry> logEntry = createLogEntry(type);
        mLogEntry.add(key, logEntry);
        return logEntry;
    } else {
        return mLogEntry.valueAt(idx);
    }
}

void ExtendedStats::log(LogType type, const char* key, statsDataType value, bool condition) {
    if ( !condition || !key)
        return;

    getLogEntry(key, type)->insert(value);
}

void ExtendedStats::dump(const char* key) const {
    // If no key is provided, print all
    // TBD: print label and sentinels
    if (key) {
        ssize_t idx = mLogEntry.indexOfKey(key);
        if (idx >= 0) {
            mLogEntry.valueAt(idx)->dump(key);
        }
    } else {
        ALOGI("----------------------------------------------------");
        ALOGI(" %s ", mName.c_str());
        for (size_t i = 0; i < mLogEntry.size(); ++i) {
            mLogEntry.valueAt(i)->dump(mLogEntry.keyAt(i).c_str());
        }
        ALOGI("----------------------------------------------------");
    }
}


/* FrameInfoPool methods */
ExtendedStats::StatsFrameInfo* ExtendedStats::StatsFrameInfoPool::get() {
    if (pool.empty()) {
        return new StatsFrameInfo();
    } else {
        StatsFrameInfo* info = pool.editTop();
        pool.pop();
        return info;
    }
}

void ExtendedStats::StatsFrameInfoPool::add(StatsFrameInfo* info) {
    pool.add(info);
}

void ExtendedStats::StatsFrameInfoPool::clear() {
    for (uint32_t i = 0; i < pool.size(); i++)
    {
        delete pool.editItemAt(i);
        pool.editItemAt(i) = 0;
    }
    pool.clear();
}

ExtendedStats::StatsFrameInfoPool::~StatsFrameInfoPool() {
    clear();
}

/* TimeBoundVector methods */
void ExtendedStats::TimeBoundVector::add(StatsFrameInfoWrapper item) {
    Mutex::Autolock lock(mLock);
    mList.add(item);
    mCurrBoundedSum += (item.infoPtr)->size;
    mTotalSizeSum += (item.infoPtr)->size;
    mTotalNumBuffers++;

    StatsFrameInfo* first = mList.itemAt(0).infoPtr;
    StatsFrameInfo* last = mList.top().infoPtr;

    // remove as many as needed to get within range
    while (last->timestamp - first->timestamp > MAX_TIME_US) {
        mCurrBoundedSum -= first->size;

        // reclaim it in the pool
        mFrameInfoPool.add(mList.editItemAt(0).infoPtr);
        mList.removeAt(0);
        first = mList.itemAt(0).infoPtr;
    }

    // calculate average if we are within the window range
    if (last->timestamp - first->timestamp > MIN_TIME_US) {
        int64_t avg = mCurrBoundedSum / mList.size();

        if (mMaxBoundedAvg < avg) {
            mMaxBoundedAvg = avg;
        }
    }
}

void ExtendedStats::TimeBoundVector::clear() {
    Mutex::Autolock lock(mLock);
    for (uint32_t i = 0; i < mList.size(); i++) {
        delete mList.editItemAt(i).infoPtr;
        mList.editItemAt(i).infoPtr = 0;
    }
    mList.clear();
    mCurrBoundedSum = 0;
}

ExtendedStats::TimeBoundVector::~TimeBoundVector() {
    clear();
}

ExtendedStats::AutoProfile::AutoProfile(
        const char* name, sp<MediaExtendedStats> mediaExtendedStats,
        bool condition, bool profileOnce)
    : mEventName(name),
      mStats(NULL),
      mCondition(condition) {

    if (mediaExtendedStats != NULL) {
        mStats = mediaExtendedStats->getProfileTimes();
    }

    if (condition && name && mStats != NULL) {
        if (profileOnce)
            mStats->profileStartOnce(name);
        else
            mStats->profileStart(name);
    }
}

ExtendedStats::AutoProfile::~AutoProfile() {
    if (mCondition && mStats != NULL) {
        mStats->profileStop(mEventName.c_str());
    }
}

MediaExtendedStats* ExtendedStats::Create(
        enum StatsType statsType, const char* name, pid_t tid) {

    char value[PROPERTY_VALUE_MAX];
    property_get("persist.debug.sf.extendedstats", value, "0");
    if (atoi(value)) {
        switch (statsType) {
            case PLAYER:
                return new PlayerExtendedStats(name, tid);
            case RECORDER:
                return new RecorderExtendedStats(name, tid);
        }
    }
    return NULL;
}


/***************************** MediaExtendedStats ************************/

MediaExtendedStats::MediaExtendedStats(const char* name, pid_t tid) :
    mBitRateVector(mFrameInfoPool) {

    mName = name;
    mTid = tid;

    reset();
}
/** helper methods **/

void MediaExtendedStats::resetConsecutiveFramesDropped() {
    if (mCurrentConsecutiveFramesDropped > mMaxConsecutiveFramesDropped)
        mMaxConsecutiveFramesDropped = mCurrentConsecutiveFramesDropped;

    mCurrentConsecutiveFramesDropped = 0;
}

/** MediaExtendedStats methods **/

void MediaExtendedStats::reset() {
    mCurrentConsecutiveFramesDropped = 0;
    mMaxConsecutiveFramesDropped = 0;
    mNumChainedDrops = 0;
    mFramesDropped = 0;
    mLastPauseTime = 0;

    mWidthDimensions.clear();
    mHeightDimensions.clear();

    mFrameInfoPool.clear();
    mBitRateVector.clear();

    mProfileTimes = new ExtendedStats(mName.c_str(), mTid);
}


void MediaExtendedStats::logFrameDropped() {
    mFramesDropped++;
    mCurrentConsecutiveFramesDropped++;
}

void MediaExtendedStats::logDimensions(int32_t width, int32_t height) {
    if (mWidthDimensions.empty() || mWidthDimensions.top() != width ||
        mHeightDimensions.empty() || mHeightDimensions.top() != height) {
        mWidthDimensions.push(width);
        mHeightDimensions.push(height);
    }
}

void MediaExtendedStats::logBitRate(int64_t size, int64_t timestamp) {
    ExtendedStats::StatsFrameInfo* sfInfo = mFrameInfoPool.get();
    sfInfo->size = size;
    sfInfo->timestamp = timestamp;
    mBitRateVector.add(ExtendedStats::StatsFrameInfoWrapper(sfInfo));
}

MediaExtendedStats::~MediaExtendedStats() {
    mBitRateVector.clear();
    mFrameInfoPool.clear();
}

/***************************** PlayerExtendedStats ************************/

PlayerExtendedStats::PlayerExtendedStats(const char* name, pid_t tid) :
    MediaExtendedStats(name, tid) {

    reset();
}

/** helper methods **/

void PlayerExtendedStats::updateTotalPlayingTime(bool wasPlaying) {
    /* only accumulate total playing time if we were playing */
    if (wasPlaying) {
        uint64_t currentTime = ExtendedStats::getSystemTime();
        mTotalPlayingTime += (currentTime - mStartPlayingTime);

        //reset this in case of repeated calls without intervening stops
        mStartPlayingTime = currentTime;
    }
}

/** PlayerExtendedStats methods **/

void PlayerExtendedStats::reset() {
    MediaExtendedStats::reset();

    mFramesRendered = 0;

    mPlaying = false;
    mPaused = false;
    mEOS = false;

    mTotalPlayingTime = 0;
    mStartPlayingTime = 0;

    mLastSeekTime = 0;
}

void PlayerExtendedStats::logFrameRendered() {
    /* we've just rendered a frame
     * if we had been dropping consecutive frames
     * before this, update their counts
     */
    if (mCurrentConsecutiveFramesDropped > 1)
        mNumChainedDrops++;

    resetConsecutiveFramesDropped();

    mFramesRendered++;
}

void PlayerExtendedStats::notifyPlaying(bool isNowPlaying) {
    if (isNowPlaying) {
        mStartPlayingTime = ExtendedStats::getSystemTime();
        mPaused = false;
    } else { //we've stopped playing.
        resetConsecutiveFramesDropped();

        /* explicitly passing in mPlaying b/c we depend on this old value.
         * The alternative (using mPlaying value within the function) can lead to
         * hard-to-find bug if mPlaying is updated before this call. Updating
         * mPlaying after this avoids race condition.
         */
        updateTotalPlayingTime(mPlaying);
    }

    // don't move this before updateTotalPlayingTime
    mPlaying = isNowPlaying;
}

void PlayerExtendedStats::notifyPause(int64_t pauseTimeUs) {
    notifyPlaying(false);
    mLastPauseTime = pauseTimeUs;
    mPaused = true;
}

void PlayerExtendedStats::notifySeek(int64_t seekTimeUs) {
    mBitRateVector.clear();
    notifyPlaying(false);
    mLastSeekTime = seekTimeUs;
}

void PlayerExtendedStats::notifySeekDone() {
    //if we're not seeking while paused
    if (!mPaused) {
        notifyPlaying(true);
    }
}

void PlayerExtendedStats::notifyEOS() {
    updateTotalPlayingTime(mPlaying);
    mEOS = true;
    mPlaying = false;
}

void PlayerExtendedStats::dump() {
    updateTotalPlayingTime(mPlaying);

    int64_t totalFrames = mFramesDropped + mFramesRendered;

    /* If we didn't process any video frames, don't print anything at all.
     * This takes care of problem in encoder profiling whereby the sound of the
     * recorder button triggers a PlayerExtendedStats instance and logs become
     * interleaved.
     */
    if (!totalFrames)
        return;

    double percentDropped = (double)mFramesDropped / totalFrames;

    ALOGI("-------------------Begin PlayerExtendedStats----------------------");

    ALOGI("%s stats (tid %d):", mName.c_str(), mTid);
    ALOGI("Video dimensions:");
    for (uint32_t i = 0; i < mWidthDimensions.size(); i++) {
        ALOGI("\t\t%d x %d", mWidthDimensions[i], mHeightDimensions[i]);
    }
    ALOGI("Total frames decoded: %"PRId64"", totalFrames);
    ALOGI("Frames dropped: %"PRId64" out of %"PRId64" (%0.2f%%)", mFramesDropped, totalFrames, percentDropped * 100);
    ALOGI("Frames rendered: %"PRId64" out of %"PRId64" (%0.2f%%)", mFramesRendered, totalFrames, (1-percentDropped) * 100);
    ALOGI("Total playback duration: %"PRId64"ms", mTotalPlayingTime / 1000);
    ALOGI("Max frames dropped consecutively: %"PRId64"", mMaxConsecutiveFramesDropped);
    ALOGI("Num occurrences of consecutive drops: %"PRId64"", mNumChainedDrops);

    ALOGI("Last seek to time: %"PRId64" ms", mLastSeekTime / 1000);
    ALOGI("Last pause time: %"PRId64" ms", mLastPauseTime/1000);

    ALOGI("Average FPS: %0.2f", mTotalPlayingTime == 0 ? 0 : mFramesRendered /(mTotalPlayingTime / 1E6));
    ALOGI("Peak bitrate: %"PRId64"", mBitRateVector.mMaxBoundedAvg);
    ALOGI("Average bitrate: %"PRId64"", mBitRateVector.mTotalNumBuffers == 0 ? 0 :
                                        mBitRateVector.mTotalSizeSum / mBitRateVector.mTotalNumBuffers);

    ALOGI("EOS(%d)", mEOS ? 1 : 0);
    ALOGI("PLAYING(%d)", mPlaying ? 1 : 0);

    ALOGI("------- Profile Latencies --------");
    bool video = true;
    bool audio = !video;
    mProfileTimes->dump(STATS_PROFILE_PAUSE);
    mProfileTimes->dump(STATS_PROFILE_RESUME);
    mProfileTimes->dump(STATS_PROFILE_SEEK);

    if (mEOS) {
        ALOGI("---------- KPI -----------");
        mProfileTimes->dump(STATS_PROFILE_SET_DATA_SOURCE);
        mProfileTimes->dump(STATS_PROFILE_PREPARE);
        mProfileTimes->dump(STATS_PROFILE_ALLOCATE_NODE(video));
        mProfileTimes->dump(STATS_PROFILE_ALLOCATE_NODE(audio));
        mProfileTimes->dump(STATS_PROFILE_CONFIGURE_CODEC(video));
        mProfileTimes->dump(STATS_PROFILE_ALLOCATE_INPUT(video));
        mProfileTimes->dump(STATS_PROFILE_ALLOCATE_OUTPUT(video));
        mProfileTimes->dump(STATS_PROFILE_CONFIGURE_CODEC(audio));
        mProfileTimes->dump(STATS_PROFILE_ALLOCATE_INPUT(audio));
        mProfileTimes->dump(STATS_PROFILE_ALLOCATE_OUTPUT(audio));
        mProfileTimes->dump(STATS_PROFILE_FIRST_BUFFER(video));
        mProfileTimes->dump(STATS_PROFILE_FIRST_BUFFER(audio));
        mProfileTimes->dump(STATS_PROFILE_START_LATENCY);
    }

    ALOGI("-------------------End PlayerExtendedStats----------------------");
}

/************************************** RecorderExtendedStats *********************************/

RecorderExtendedStats::RecorderExtendedStats(const char* name, pid_t tid) :
    MediaExtendedStats(name, tid) {

    reset();
}

void RecorderExtendedStats::reset() {
    MediaExtendedStats::reset();
    mFramesEncoded = 0;
    mTotalRecordingTime = 0;
}

void RecorderExtendedStats::notifyPause(int64_t pauseTimeUs) {
    mLastPauseTime = pauseTimeUs;
    resetConsecutiveFramesDropped();
}

void RecorderExtendedStats::logFrameEncoded() {
    /* we've just rendered a frame
     * if we had been dropping consecutive frames
     * before this, update their counts
     */
    if (mCurrentConsecutiveFramesDropped > 1)
        mNumChainedDrops++;

    resetConsecutiveFramesDropped();

    mFramesEncoded++;
}

void RecorderExtendedStats::logRecordingDuration(int64_t duration) {
    mTotalRecordingTime = duration;
}

void RecorderExtendedStats::dump() {

    int64_t totalFrames = mFramesDropped + mFramesEncoded;
    double percentDropped = totalFrames == 0 ? 0 : (double)mFramesDropped/totalFrames;

    ALOGI("-------------------Begin RecorderExtendedStats----------------------");

    ALOGI("%s stats (tid %d):",mName.c_str(), mTid);
    ALOGI("Video dimensions:");
    for (uint32_t i = 0; i < mWidthDimensions.size(); i++)
    {
        ALOGI("\t\t%d x %d", mWidthDimensions[i], mHeightDimensions[i]);
    }
    ALOGI("Total frames: %"PRId64"", totalFrames);
    ALOGI("Frames dropped: %"PRId64" out of %"PRId64" (%0.2f%%)", mFramesDropped, totalFrames, percentDropped * 100);
    ALOGI("Frames encoded: %"PRId64" out of %"PRId64" (%0.2f%%)", mFramesEncoded, totalFrames, (1-percentDropped) * 100);
    ALOGI("Max frames dropped consecutively: %"PRId64"", mMaxConsecutiveFramesDropped);
    ALOGI("Num occurrences of consecutive drops: %"PRId64"", mNumChainedDrops);

    ALOGI("Total recording duration: %"PRId64" ms", mTotalRecordingTime/1000);
    ALOGI("Last pause time: %"PRId64" ms", mLastPauseTime/1000);
    ALOGI("Input frame rate: %0.2f", mTotalRecordingTime == 0 ? 0 : mFramesEncoded/(mTotalRecordingTime/1E6));
    ALOGI("Peak bitrate: %"PRId64"", mBitRateVector.mMaxBoundedAvg);
    ALOGI("Average bitrate: %"PRId64"", mBitRateVector.mTotalNumBuffers == 0 ? 0 :
                                        mBitRateVector.mTotalSizeSum/mBitRateVector.mTotalNumBuffers);

    ALOGI("------- Profile Latencies --------");

    bool video = true;
    bool audio = !video;
    mProfileTimes->dump(STATS_PROFILE_PAUSE);

    ALOGI("---------- KPI -----------");
    mProfileTimes->dump(STATS_PROFILE_SET_CAMERA_SOURCE);
    mProfileTimes->dump(STATS_PROFILE_ALLOCATE_NODE(video));
    mProfileTimes->dump(STATS_PROFILE_ALLOCATE_NODE(audio));
    mProfileTimes->dump(STATS_PROFILE_SET_ENCODER(video));
    mProfileTimes->dump(STATS_PROFILE_CONFIGURE_CODEC(video));
    mProfileTimes->dump(STATS_PROFILE_ALLOCATE_INPUT(video));
    mProfileTimes->dump(STATS_PROFILE_ALLOCATE_OUTPUT(video));
    mProfileTimes->dump(STATS_PROFILE_SET_ENCODER(audio));
    mProfileTimes->dump(STATS_PROFILE_CONFIGURE_CODEC(audio));
    mProfileTimes->dump(STATS_PROFILE_ALLOCATE_INPUT(audio));
    mProfileTimes->dump(STATS_PROFILE_ALLOCATE_OUTPUT(audio));
    mProfileTimes->dump(STATS_PROFILE_FIRST_BUFFER(video));
    mProfileTimes->dump(STATS_PROFILE_FIRST_BUFFER(audio));
    mProfileTimes->dump(STATS_PROFILE_START_LATENCY);
    mProfileTimes->dump(STATS_PROFILE_STOP);

    ALOGI("-------------------End RecorderExtendedStats----------------------");
}

}
