/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef PLAYLIST_FETCHER_H_

#define PLAYLIST_FETCHER_H_

#include <media/stagefright/foundation/AHandler.h>

#include "mpeg2ts/ATSParser.h"
#include "LiveSession.h"

namespace android {

struct ABuffer;
struct AnotherPacketSource;
struct DataSource;
struct HTTPBase;
struct LiveDataSource;
struct M3UParser;
struct String8;

struct PlaylistFetcher : public AHandler {
    enum {
        kWhatStarted,
        kWhatPaused,
        kWhatStopped,
        kWhatError,
        kWhatDurationUpdate,
        kWhatTemporarilyDoneFetching,
        kWhatPrepared,
        kWhatPreparationFailed,
        kWhatStartedAt,
    };

    PlaylistFetcher(
            const sp<AMessage> &notify,
            const sp<LiveSession> &session,
            const char *uri);

    sp<DataSource> getDataSource();

    void startAsync(
            const sp<AnotherPacketSource> &audioSource,
            const sp<AnotherPacketSource> &videoSource,
            const sp<AnotherPacketSource> &subtitleSource,
            int64_t startTimeUs = -1ll,
            int64_t minStartTimeUs = 0ll /* start after this timestamp */,
            int32_t startSeqNumberHint = -1 /* try starting at this sequence number */);

    void pauseAsync();

    void stopAsync(bool selfTriggered = false);

    void resumeUntilAsync(const sp<AMessage> &params);

protected:
    virtual ~PlaylistFetcher();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kMaxNumRetries         = 5,
    };

    enum {
        kWhatStart          = 'strt',
        kWhatPause          = 'paus',
        kWhatStop           = 'stop',
        kWhatMonitorQueue   = 'moni',
        kWhatResumeUntil    = 'rsme',
        kWhatDownloadNext   = 'dlnx',
    };

    static const int64_t kMinBufferedDurationUs;
    static const int64_t kMaxMonitorDelayUs;
    static const int32_t kDownloadBlockSize;
    static const int32_t kNumSkipFrames;

    static bool bufferStartsWithTsSyncByte(const sp<ABuffer>& buffer);

    // notifications to mSession
    sp<AMessage> mNotify;
    sp<AMessage> mStartTimeUsNotify;

    sp<LiveSession> mSession;
    AString mURI;
    AString mVideoMime;

    uint32_t mStreamTypeMask;
    int64_t mStartTimeUs;
    int64_t mMinStartTimeUs; // start fetching no earlier than this value
    sp<AMessage> mStopParams; // message containing the latest timestamps we should fetch.

    KeyedVector<LiveSession::StreamType, sp<AnotherPacketSource> >
        mPacketSources;

    KeyedVector<AString, sp<ABuffer> > mAESKeyForURI;

    int64_t mLastPlaylistFetchTimeUs;
    sp<M3UParser> mPlaylist;
    int32_t mSeqNumber;
    int32_t mNumRetries;
    bool mStartup;
    bool mPrepared;
    bool mSkipToFirstIDRAfterConnect;
    int64_t mNextPTSTimeUs;

    int32_t mMonitorQueueGeneration;

    enum RefreshState {
        INITIAL_MINIMUM_RELOAD_DELAY,
        FIRST_UNCHANGED_RELOAD_ATTEMPT,
        SECOND_UNCHANGED_RELOAD_ATTEMPT,
        THIRD_UNCHANGED_RELOAD_ATTEMPT
    };
    RefreshState mRefreshState;

    uint8_t mPlaylistHash[16];

    sp<ATSParser> mTSParser;

    bool mFirstPTSValid;
    uint64_t mFirstPTS;
    int64_t mAbsoluteTimeAnchorUs;

    // Stores the initialization vector to decrypt the next block of cipher text, which can
    // either be derived from the sequence number, read from the manifest, or copied from
    // the last block of cipher text (cipher-block chaining).
    unsigned char mAESInitVec[16];

    // Set first to true if decrypting the first segment of a playlist segment. When
    // first is true, reset the initialization vector based on the available
    // information in the manifest; otherwise, use the initialization vector as
    // updated by the last call to AES_cbc_encrypt.
    //
    // For the input to decrypt correctly, decryptBuffer must be called on
    // consecutive byte ranges on block boundaries, e.g. 0..15, 16..47, 48..63,
    // and so on.
    status_t decryptBuffer(
            size_t playlistIndex, const sp<ABuffer> &buffer,
            bool first = true);
    status_t checkDecryptPadding(const sp<ABuffer> &buffer);

    void postMonitorQueue(int64_t delayUs = 0, int64_t minDelayUs = 0);
    void cancelMonitorQueue();

    int64_t delayUsToRefreshPlaylist() const;
    status_t refreshPlaylist();

    // Returns the media time in us of the segment specified by seqNumber.
    // This is computed by summing the durations of all segments before it.
    int64_t getSegmentStartTimeUs(int32_t seqNumber) const;

    status_t onStart(const sp<AMessage> &msg);
    void onPause();
    void onStop(const sp<AMessage> &msg);
    void onMonitorQueue();
    void onDownloadNext();

    // Resume a fetcher to continue until the stopping point stored in msg.
    status_t onResumeUntil(const sp<AMessage> &msg);

    status_t extractAndQueueAccessUnitsFromTs(const sp<ABuffer> &buffer);

    status_t extractAndQueueAccessUnits(
            const sp<ABuffer> &buffer, const sp<AMessage> &itemMeta);

    void notifyError(status_t err);

    void queueDiscontinuity(
            ATSParser::DiscontinuityType type, const sp<AMessage> &extra);

    int32_t getSeqNumberForTime(int64_t timeUs) const;

    void updateDuration();

    // Before resuming a fetcher in onResume, check the remaining duration is longer than that
    // returned by resumeThreshold.
    int64_t resumeThreshold(const sp<AMessage> &msg);

    DISALLOW_EVIL_CONSTRUCTORS(PlaylistFetcher);
};

}  // namespace android

#endif  // PLAYLIST_FETCHER_H_

