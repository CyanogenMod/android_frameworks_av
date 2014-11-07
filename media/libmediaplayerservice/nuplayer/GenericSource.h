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

#ifndef GENERIC_SOURCE_H_

#define GENERIC_SOURCE_H_

#include "NuPlayer.h"
#include "NuPlayerSource.h"

#include "ATSParser.h"

#include <media/mediaplayer.h>

namespace android {

class DecryptHandle;
class DrmManagerClient;
struct AnotherPacketSource;
struct ARTSPController;
struct DataSource;
struct IMediaHTTPService;
struct MediaSource;
class MediaBuffer;
struct NuCachedSource2;
struct WVMExtractor;

struct NuPlayer::GenericSource : public NuPlayer::Source {
    GenericSource(const sp<AMessage> &notify, bool uidValid, uid_t uid);

    status_t setDataSource(
            const sp<IMediaHTTPService> &httpService,
            const char *url,
            const KeyedVector<String8, String8> *headers);

    status_t setDataSource(int fd, int64_t offset, int64_t length);

    virtual void prepareAsync();

    virtual void start();
    virtual void stop();
    virtual void pause();
    virtual void resume();

    virtual void disconnect();

    virtual status_t feedMoreTSData();

    virtual sp<MetaData> getFileFormatMeta() const;

    virtual status_t dequeueAccessUnit(bool audio, sp<ABuffer> *accessUnit);

    virtual status_t getDuration(int64_t *durationUs);
    virtual size_t getTrackCount() const;
    virtual sp<AMessage> getTrackInfo(size_t trackIndex) const;
    virtual ssize_t getSelectedTrack(media_track_type type) const;
    virtual status_t selectTrack(size_t trackIndex, bool select);
    virtual status_t seekTo(int64_t seekTimeUs);

    virtual status_t setBuffers(bool audio, Vector<MediaBuffer *> &buffers);

protected:
    virtual ~GenericSource();

    virtual void onMessageReceived(const sp<AMessage> &msg);

    virtual sp<MetaData> getFormatMeta(bool audio);

private:
    enum {
        kWhatPrepareAsync,
        kWhatFetchSubtitleData,
        kWhatFetchTimedTextData,
        kWhatSendSubtitleData,
        kWhatSendTimedTextData,
        kWhatChangeAVSource,
        kWhatPollBuffering,
        kWhatGetFormat,
        kWhatGetSelectedTrack,
        kWhatSelectTrack,
        kWhatSeek,
        kWhatReadBuffer,
        kWhatStopWidevine,
    };

    Vector<sp<MediaSource> > mSources;

    struct Track {
        size_t mIndex;
        sp<MediaSource> mSource;
        sp<AnotherPacketSource> mPackets;
    };

    Track mAudioTrack;
    int64_t mAudioTimeUs;
    Track mVideoTrack;
    int64_t mVideoTimeUs;
    Track mSubtitleTrack;
    Track mTimedTextTrack;

    int32_t mFetchSubtitleDataGeneration;
    int32_t mFetchTimedTextDataGeneration;
    int64_t mDurationUs;
    bool mAudioIsVorbis;
    bool mIsWidevine;
    bool mUIDValid;
    uid_t mUID;
    sp<IMediaHTTPService> mHTTPService;
    AString mUri;
    KeyedVector<String8, String8> mUriHeaders;
    int mFd;
    int64_t mOffset;
    int64_t mLength;

    sp<DataSource> mDataSource;
    sp<NuCachedSource2> mCachedSource;
    sp<DataSource> mHttpSource;
    sp<WVMExtractor> mWVMExtractor;
    sp<MetaData> mFileMeta;
    DrmManagerClient *mDrmManagerClient;
    sp<DecryptHandle> mDecryptHandle;
    bool mStarted;
    bool mStopRead;
    String8 mContentType;
    AString mSniffedMIME;
    off64_t mMetaDataSize;
    int64_t mBitrate;
    int32_t mPollBufferingGeneration;
    uint32_t mPendingReadBufferTypes;
    mutable Mutex mReadBufferLock;

    sp<ALooper> mLooper;

    void resetDataSource();

    status_t initFromDataSource();
    void checkDrmStatus(const sp<DataSource>& dataSource);
    int64_t getLastReadPosition();
    void setDrmPlaybackStatusIfNeeded(int playbackStatus, int64_t position);

    status_t prefillCacheIfNecessary();

    void notifyPreparedAndCleanup(status_t err);

    void onGetFormatMeta(sp<AMessage> msg) const;
    sp<MetaData> doGetFormatMeta(bool audio) const;

    void onGetSelectedTrack(sp<AMessage> msg) const;
    ssize_t doGetSelectedTrack(media_track_type type) const;

    void onSelectTrack(sp<AMessage> msg);
    status_t doSelectTrack(size_t trackIndex, bool select);

    void onSeek(sp<AMessage> msg);
    status_t doSeek(int64_t seekTimeUs);

    void onPrepareAsync();

    void fetchTextData(
            uint32_t what, media_track_type type,
            int32_t curGen, sp<AnotherPacketSource> packets, sp<AMessage> msg);

    void sendTextData(
            uint32_t what, media_track_type type,
            int32_t curGen, sp<AnotherPacketSource> packets, sp<AMessage> msg);

    sp<ABuffer> mediaBufferToABuffer(
            MediaBuffer *mbuf,
            media_track_type trackType,
            int64_t *actualTimeUs = NULL);

    void postReadBuffer(media_track_type trackType);
    void onReadBuffer(sp<AMessage> msg);
    void readBuffer(
            media_track_type trackType,
            int64_t seekTimeUs = -1ll, int64_t *actualTimeUs = NULL, bool formatChange = false);

    void schedulePollBuffering();
    void cancelPollBuffering();
    void onPollBuffering();
    void notifyBufferingUpdate(int percentage);

    DISALLOW_EVIL_CONSTRUCTORS(GenericSource);
};

}  // namespace android

#endif  // GENERIC_SOURCE_H_
