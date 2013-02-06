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

#ifndef LIVE_SESSION_H_

#define LIVE_SESSION_H_

#include <media/stagefright/foundation/AHandler.h>

#include <utils/String8.h>

namespace android {

struct ABuffer;
struct DataSource;
struct LiveDataSource;
struct M3UParser;
struct HTTPBase;

struct LiveSession : public AHandler {
    enum Flags {
        // Don't log any URLs.
        kFlagIncognito = 1,
    };
    LiveSession(
            const sp<AMessage> &notify,
            uint32_t flags = 0, bool uidValid = false, uid_t uid = 0);

    sp<DataSource> getDataSource();

    void connect(
            const char *url,
            const KeyedVector<String8, String8> *headers = NULL);

    void disconnect();

    // Blocks until seek is complete.
    void seekTo(int64_t timeUs);

    status_t getDuration(int64_t *durationUs) const;

    bool isSeekable() const;
    bool hasDynamicDuration() const;

    // Posted notification's "what" field will carry one of the following:
    enum {
        kWhatPrepared,
        kWhatPreparationFailed,
    };

protected:
    virtual ~LiveSession();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kMaxNumQueuedFragments = 3,
        kMaxNumRetries         = 5,
    };

    enum {
        kWhatConnect        = 'conn',
        kWhatDisconnect     = 'disc',
        kWhatMonitorQueue   = 'moni',
        kWhatSeek           = 'seek',
    };

    struct BandwidthItem {
        AString mURI;
        unsigned long mBandwidth;
    };

    sp<AMessage> mNotify;
    uint32_t mFlags;
    bool mUIDValid;
    uid_t mUID;

    bool mInPreparationPhase;

    sp<LiveDataSource> mDataSource;

    sp<HTTPBase> mHTTPDataSource;

    AString mMasterURL;
    KeyedVector<String8, String8> mExtraHeaders;

    Vector<BandwidthItem> mBandwidthItems;

    KeyedVector<AString, sp<ABuffer> > mAESKeyForURI;

    ssize_t mPrevBandwidthIndex;
    int64_t mLastPlaylistFetchTimeUs;
    sp<M3UParser> mPlaylist;
    int32_t mSeqNumber;
    int64_t mSeekTimeUs;
    int32_t mNumRetries;
    bool mStartOfPlayback;

    mutable Mutex mLock;
    Condition mCondition;
    int64_t mDurationUs;
    bool mDurationFixed;  // Duration has been determined once and for all.
    bool mSeekDone;
    bool mDisconnectPending;

    int32_t mMonitorQueueGeneration;

    enum RefreshState {
        INITIAL_MINIMUM_RELOAD_DELAY,
        FIRST_UNCHANGED_RELOAD_ATTEMPT,
        SECOND_UNCHANGED_RELOAD_ATTEMPT,
        THIRD_UNCHANGED_RELOAD_ATTEMPT
    };
    RefreshState mRefreshState;

    uint8_t mPlaylistHash[16];

    void onConnect(const sp<AMessage> &msg);
    void onDisconnect();
    void onDownloadNext();
    void onMonitorQueue();
    void onSeek(const sp<AMessage> &msg);

    status_t fetchFile(
            const char *url, sp<ABuffer> *out,
            int64_t range_offset = 0, int64_t range_length = -1);

    sp<M3UParser> fetchPlaylist(const char *url, bool *unchanged);
    size_t getBandwidthIndex();

    status_t decryptBuffer(
            size_t playlistIndex, const sp<ABuffer> &buffer);

    void postMonitorQueue(int64_t delayUs = 0);

    bool timeToRefreshPlaylist(int64_t nowUs) const;

    static int SortByBandwidth(const BandwidthItem *, const BandwidthItem *);

    // Returns the media time in us of the segment specified by seqNumber.
    // This is computed by summing the durations of all segments before it.
    int64_t getSegmentStartTimeUs(int32_t seqNumber) const;

    void signalEOS(status_t err);

    DISALLOW_EVIL_CONSTRUCTORS(LiveSession);
};

}  // namespace android

#endif  // LIVE_SESSION_H_
