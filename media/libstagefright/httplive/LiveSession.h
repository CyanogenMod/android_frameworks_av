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
#include <media/mediaplayer.h>

#include <utils/String8.h>

#include "mpeg2ts/ATSParser.h"

namespace android {

struct ABuffer;
struct AReplyToken;
struct AnotherPacketSource;
struct DataSource;
struct HTTPBase;
struct IMediaHTTPService;
struct LiveDataSource;
struct M3UParser;
struct PlaylistFetcher;
struct HLSTime;

struct LiveSession : public AHandler {
    enum Flags {
        // Don't log any URLs.
        kFlagIncognito = 1,
    };

    enum StreamIndex {
        kAudioIndex    = 0,
        kVideoIndex    = 1,
        kSubtitleIndex = 2,
        kMaxStreams    = 3,
        kMetaDataIndex = 3,
        kNumSources    = 4,
    };

    enum StreamType {
        STREAMTYPE_AUDIO        = 1 << kAudioIndex,
        STREAMTYPE_VIDEO        = 1 << kVideoIndex,
        STREAMTYPE_SUBTITLES    = 1 << kSubtitleIndex,
        STREAMTYPE_METADATA     = 1 << kMetaDataIndex,
    };

    enum SeekMode {
        kSeekModeExactPosition = 0, // used for seeking
        kSeekModeNextSample    = 1, // used for seamless switching
        kSeekModeNextSegment   = 2, // used for seamless switching
    };

    LiveSession(
            const sp<AMessage> &notify,
            uint32_t flags,
            const sp<IMediaHTTPService> &httpService);

    int64_t calculateMediaTimeUs(int64_t firstTimeUs, int64_t timeUs, int32_t discontinuitySeq);
    status_t dequeueAccessUnit(StreamType stream, sp<ABuffer> *accessUnit);

    status_t getStreamFormat(StreamType stream, sp<AMessage> *format);

    sp<HTTPBase> getHTTPDataSource();

    void connectAsync(
            const char *url,
            const KeyedVector<String8, String8> *headers = NULL);

    status_t disconnect();

    // Blocks until seek is complete.
    status_t seekTo(int64_t timeUs);

    status_t getDuration(int64_t *durationUs) const;
    size_t getTrackCount() const;
    sp<AMessage> getTrackInfo(size_t trackIndex) const;
    status_t selectTrack(size_t index, bool select);
    ssize_t getSelectedTrack(media_track_type /* type */) const;

    bool isSeekable() const;
    bool hasDynamicDuration() const;

    static const char *getKeyForStream(StreamType type);
    static const char *getNameForStream(StreamType type);
    static ATSParser::SourceType getSourceTypeForStream(StreamType type);

    enum {
        kWhatStreamsChanged,
        kWhatError,
        kWhatPrepared,
        kWhatPreparationFailed,
        kWhatBufferingStart,
        kWhatBufferingEnd,
        kWhatBufferingUpdate,
        kWhatMetadataDetected,
    };

protected:
    virtual ~LiveSession();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    friend struct PlaylistFetcher;

    enum {
        kWhatConnect                    = 'conn',
        kWhatDisconnect                 = 'disc',
        kWhatSeek                       = 'seek',
        kWhatFetcherNotify              = 'notf',
        kWhatChangeConfiguration        = 'chC0',
        kWhatChangeConfiguration2       = 'chC2',
        kWhatChangeConfiguration3       = 'chC3',
        kWhatFinishDisconnect2          = 'fin2',
        kWhatPollBuffering              = 'poll',
    };

    // Bandwidth Switch Mark Defaults
    static const int64_t kUpSwitchMarkUs;
    static const int64_t kDownSwitchMarkUs;
    static const int64_t kUpSwitchMarginUs;
    static const int64_t kResumeThresholdUs;

    // Buffer Prepare/Ready/Underflow Marks
    static const int64_t kReadyMarkUs;
    static const int64_t kPrepareMarkUs;
    static const int64_t kUnderflowMarkUs;

    struct BandwidthEstimator;
    struct BandwidthItem {
        size_t mPlaylistIndex;
        unsigned long mBandwidth;
    };

    struct FetcherInfo {
        sp<PlaylistFetcher> mFetcher;
        int64_t mDurationUs;
        bool mToBeRemoved;
        bool mToBeResumed;
    };

    struct StreamItem {
        const char *mType;
        AString mUri, mNewUri;
        SeekMode mSeekMode;
        size_t mCurDiscontinuitySeq;
        int64_t mLastDequeuedTimeUs;
        int64_t mLastSampleDurationUs;
        StreamItem()
            : StreamItem("") {}
        StreamItem(const char *type)
            : mType(type),
              mSeekMode(kSeekModeExactPosition),
              mCurDiscontinuitySeq(0),
              mLastDequeuedTimeUs(0),
              mLastSampleDurationUs(0) {}
        AString uriKey() {
            AString key(mType);
            key.append("URI");
            return key;
        }
    };
    StreamItem mStreams[kMaxStreams];

    sp<AMessage> mNotify;
    uint32_t mFlags;
    sp<IMediaHTTPService> mHTTPService;

    bool mBuffering;
    bool mInPreparationPhase;
    int32_t mPollBufferingGeneration;
    int32_t mPrevBufferPercentage;

    sp<HTTPBase> mHTTPDataSource;
    KeyedVector<String8, String8> mExtraHeaders;

    AString mMasterURL;

    Vector<BandwidthItem> mBandwidthItems;
    ssize_t mCurBandwidthIndex;
    ssize_t mOrigBandwidthIndex;
    int32_t mLastBandwidthBps;
    sp<BandwidthEstimator> mBandwidthEstimator;

    sp<M3UParser> mPlaylist;
    int32_t mMaxWidth;
    int32_t mMaxHeight;

    sp<ALooper> mFetcherLooper;
    KeyedVector<AString, FetcherInfo> mFetcherInfos;
    uint32_t mStreamMask;

    // Masks used during reconfiguration:
    // mNewStreamMask: streams in the variant playlist we're switching to;
    // we don't want to immediately overwrite the original value.
    uint32_t mNewStreamMask;

    // mSwapMask: streams that have started to playback content in the new variant playlist;
    // we use this to track reconfiguration progress.
    uint32_t mSwapMask;

    KeyedVector<StreamType, sp<AnotherPacketSource> > mPacketSources;
    // A second set of packet sources that buffer content for the variant we're switching to.
    KeyedVector<StreamType, sp<AnotherPacketSource> > mPacketSources2;

    int32_t mSwitchGeneration;
    int32_t mSubtitleGeneration;

    size_t mContinuationCounter;
    sp<AMessage> mContinuation;
    sp<AMessage> mSeekReply;

    int64_t mLastDequeuedTimeUs;
    int64_t mRealTimeBaseUs;

    bool mReconfigurationInProgress;
    bool mSwitchInProgress;
    int64_t mUpSwitchMark;
    int64_t mDownSwitchMark;
    int64_t mUpSwitchMargin;

    sp<AReplyToken> mDisconnectReplyID;
    sp<AReplyToken> mSeekReplyID;

    bool mFirstTimeUsValid;
    int64_t mFirstTimeUs;
    int64_t mLastSeekTimeUs;
    bool mHasMetadata;

    KeyedVector<size_t, int64_t> mDiscontinuityAbsStartTimesUs;
    KeyedVector<size_t, int64_t> mDiscontinuityOffsetTimesUs;

    sp<PlaylistFetcher> addFetcher(const char *uri);

    void onConnect(const sp<AMessage> &msg);
    void onSeek(const sp<AMessage> &msg);
    void onFinishDisconnect2();

    // If given a non-zero block_size (default 0), it is used to cap the number of
    // bytes read in from the DataSource. If given a non-NULL buffer, new content
    // is read into the end.
    //
    // The DataSource we read from is responsible for signaling error or EOF to help us
    // break out of the read loop. The DataSource can be returned to the caller, so
    // that the caller can reuse it for subsequent fetches (within the initially
    // requested range).
    //
    // For reused HTTP sources, the caller must download a file sequentially without
    // any overlaps or gaps to prevent reconnection.
    ssize_t fetchFile(
            const char *url, sp<ABuffer> *out,
            /* request/open a file starting at range_offset for range_length bytes */
            int64_t range_offset = 0, int64_t range_length = -1,
            /* download block size */
            uint32_t block_size = 0,
            /* reuse DataSource if doing partial fetch */
            sp<DataSource> *source = NULL,
            String8 *actualUrl = NULL,
            /* force connect http even when resuing DataSource */
            bool forceConnectHTTP = false);

    sp<M3UParser> fetchPlaylist(
            const char *url, uint8_t *curPlaylistHash, bool *unchanged);

    bool UriIsSameAsIndex( const AString &uri, int32_t index, bool newUri);
    sp<AnotherPacketSource> getPacketSourceForStreamIndex(size_t trackIndex, bool newUri);
    sp<AnotherPacketSource> getMetadataSource(
            sp<AnotherPacketSource> sources[kNumSources], uint32_t streamMask, bool newUri);

    bool resumeFetcher(
            const AString &uri, uint32_t streamMask,
            int64_t timeUs = -1ll, bool newUri = false);

    float getAbortThreshold(
            ssize_t currentBWIndex, ssize_t targetBWIndex) const;
    void addBandwidthMeasurement(size_t numBytes, int64_t delayUs);
    size_t getBandwidthIndex(int32_t bandwidthBps);
    HLSTime latestMediaSegmentStartTime() const;

    static int SortByBandwidth(const BandwidthItem *, const BandwidthItem *);
    static StreamType indexToType(int idx);
    static ssize_t typeToIndex(int32_t type);

    void changeConfiguration(
            int64_t timeUs, ssize_t bwIndex = -1, bool pickTrack = false);
    void onChangeConfiguration(const sp<AMessage> &msg);
    void onChangeConfiguration2(const sp<AMessage> &msg);
    void onChangeConfiguration3(const sp<AMessage> &msg);

    void swapPacketSource(StreamType stream);
    void tryToFinishBandwidthSwitch(const AString &oldUri);
    void cancelBandwidthSwitch(bool resume = false);
    bool checkSwitchProgress(
            sp<AMessage> &msg, int64_t delayUs, bool *needResumeUntil);

    bool switchBandwidthIfNeeded(bool bufferHigh, bool bufferLow);

    void schedulePollBuffering();
    void cancelPollBuffering();
    void restartPollBuffering();
    void onPollBuffering();
    bool checkBuffering(bool &underflow, bool &ready, bool &down, bool &up);
    void startBufferingIfNecessary();
    void stopBufferingIfNecessary();
    void notifyBufferingUpdate(int32_t percentage);

    void finishDisconnect();

    void postPrepared(status_t err);
    void postError(status_t err);

    DISALLOW_EVIL_CONSTRUCTORS(LiveSession);
};

}  // namespace android

#endif  // LIVE_SESSION_H_
