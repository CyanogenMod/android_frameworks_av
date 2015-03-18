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

//#define LOG_NDEBUG 0
#define LOG_TAG "LiveSession"
#include <utils/Log.h>

#include "LiveSession.h"

#include "M3UParser.h"
#include "PlaylistFetcher.h"

#include "include/HTTPBase.h"
#include "mpeg2ts/AnotherPacketSource.h"

#include <cutils/properties.h>
#include <media/IMediaHTTPConnection.h>
#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaHTTP.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include <utils/Mutex.h>

#include <ctype.h>
#include <inttypes.h>
#include <openssl/aes.h>
#include <openssl/md5.h>

namespace android {

// static
// High water mark to start up switch or report prepared)
const int64_t LiveSession::kHighWaterMark = 8000000ll;
const int64_t LiveSession::kMidWaterMark = 5000000ll;
const int64_t LiveSession::kLowWaterMark = 3000000ll;

struct LiveSession::BandwidthEstimator : public RefBase {
    BandwidthEstimator();

    void addBandwidthMeasurement(size_t numBytes, int64_t delayUs);
    bool estimateBandwidth(int32_t *bandwidth);

private:
    // Bandwidth estimation parameters
    static const int32_t kMaxBandwidthHistoryItems = 20;
    static const int64_t kMaxBandwidthHistoryWindowUs = 3000000ll; // 3 sec

    struct BandwidthEntry {
        int64_t mDelayUs;
        size_t mNumBytes;
    };

    Mutex mLock;
    List<BandwidthEntry> mBandwidthHistory;
    int64_t mTotalTransferTimeUs;
    size_t mTotalTransferBytes;

    DISALLOW_EVIL_CONSTRUCTORS(BandwidthEstimator);
};

LiveSession::BandwidthEstimator::BandwidthEstimator() :
    mTotalTransferTimeUs(0),
    mTotalTransferBytes(0) {
}

void LiveSession::BandwidthEstimator::addBandwidthMeasurement(
        size_t numBytes, int64_t delayUs) {
    AutoMutex autoLock(mLock);

    BandwidthEntry entry;
    entry.mDelayUs = delayUs;
    entry.mNumBytes = numBytes;
    mTotalTransferTimeUs += delayUs;
    mTotalTransferBytes += numBytes;
    mBandwidthHistory.push_back(entry);

    // trim old samples, keeping at least kMaxBandwidthHistoryItems samples,
    // and total transfer time at least kMaxBandwidthHistoryWindowUs.
    while (mBandwidthHistory.size() > kMaxBandwidthHistoryItems) {
        List<BandwidthEntry>::iterator it = mBandwidthHistory.begin();
        if (mTotalTransferTimeUs - it->mDelayUs < kMaxBandwidthHistoryWindowUs) {
            break;
        }
        mTotalTransferTimeUs -= it->mDelayUs;
        mTotalTransferBytes -= it->mNumBytes;
        mBandwidthHistory.erase(mBandwidthHistory.begin());
    }
}

bool LiveSession::BandwidthEstimator::estimateBandwidth(int32_t *bandwidthBps) {
    AutoMutex autoLock(mLock);

    if (mBandwidthHistory.size() < 2) {
        return false;
    }

    *bandwidthBps = ((double)mTotalTransferBytes * 8E6 / mTotalTransferTimeUs);
    return true;
}

LiveSession::LiveSession(
        const sp<AMessage> &notify, uint32_t flags,
        const sp<IMediaHTTPService> &httpService)
    : mNotify(notify),
      mFlags(flags),
      mHTTPService(httpService),
      mInPreparationPhase(true),
      mHTTPDataSource(new MediaHTTP(mHTTPService->makeHTTPConnection())),
      mCurBandwidthIndex(-1),
      mLastBandwidthBps(-1ll),
      mBandwidthEstimator(new BandwidthEstimator()),
      mStreamMask(0),
      mNewStreamMask(0),
      mSwapMask(0),
      mSwitchGeneration(0),
      mSubtitleGeneration(0),
      mLastDequeuedTimeUs(0ll),
      mRealTimeBaseUs(0ll),
      mReconfigurationInProgress(false),
      mSwitchInProgress(false),
      mFirstTimeUsValid(false),
      mFirstTimeUs(0),
      mLastSeekTimeUs(0),
      mPollBufferingGeneration(0) {

    mStreams[kAudioIndex] = StreamItem("audio");
    mStreams[kVideoIndex] = StreamItem("video");
    mStreams[kSubtitleIndex] = StreamItem("subtitles");

    for (size_t i = 0; i < kMaxStreams; ++i) {
        mPacketSources.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mPacketSources2.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
    }
}

LiveSession::~LiveSession() {
    if (mFetcherLooper != NULL) {
        mFetcherLooper->stop();
    }
}

status_t LiveSession::dequeueAccessUnit(
        StreamType stream, sp<ABuffer> *accessUnit) {
    if (!(mStreamMask & stream)) {
        // return -EWOULDBLOCK to avoid halting the decoder
        // when switching between audio/video and audio only.
        return -EWOULDBLOCK;
    }

    status_t finalResult = OK;
    sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(stream);

    ssize_t idx = typeToIndex(stream);
    // Do not let client pull data if we don't have data packets yet.
    // We might only have a format discontinuity queued without data.
    // When NuPlayerDecoder dequeues the format discontinuity, it will
    // immediately try to getFormat. If we return NULL, NuPlayerDecoder
    // thinks it can do seamless change, so will not shutdown decoder.
    // When the actual format arrives, it can't handle it and get stuck.
    if (!packetSource->hasDataBufferAvailable(&finalResult)) {
        if (finalResult == OK) {
            return -EAGAIN;
        } else {
            return finalResult;
        }
    }

    // Let the client dequeue as long as we have buffers available
    // Do not make pause/resume decisions here.

    status_t err = packetSource->dequeueAccessUnit(accessUnit);

    size_t streamIdx;
    const char *streamStr;
    switch (stream) {
        case STREAMTYPE_AUDIO:
            streamIdx = kAudioIndex;
            streamStr = "audio";
            break;
        case STREAMTYPE_VIDEO:
            streamIdx = kVideoIndex;
            streamStr = "video";
            break;
        case STREAMTYPE_SUBTITLES:
            streamIdx = kSubtitleIndex;
            streamStr = "subs";
            break;
        default:
            TRESPASS();
    }

    StreamItem& strm = mStreams[streamIdx];
    if (err == INFO_DISCONTINUITY) {
        // adaptive streaming, discontinuities in the playlist
        int32_t type;
        CHECK((*accessUnit)->meta()->findInt32("discontinuity", &type));

        sp<AMessage> extra;
        if (!(*accessUnit)->meta()->findMessage("extra", &extra)) {
            extra.clear();
        }

        ALOGI("[%s] read discontinuity of type %d, extra = %s",
              streamStr,
              type,
              extra == NULL ? "NULL" : extra->debugString().c_str());

        size_t seq = strm.mCurDiscontinuitySeq;
        int64_t offsetTimeUs;
        if (mDiscontinuityOffsetTimesUs.indexOfKey(seq) >= 0) {
            offsetTimeUs = mDiscontinuityOffsetTimesUs.valueFor(seq);
        } else {
            offsetTimeUs = 0;
        }

        seq += 1;
        if (mDiscontinuityAbsStartTimesUs.indexOfKey(strm.mCurDiscontinuitySeq) >= 0) {
            int64_t firstTimeUs;
            firstTimeUs = mDiscontinuityAbsStartTimesUs.valueFor(strm.mCurDiscontinuitySeq);
            offsetTimeUs += strm.mLastDequeuedTimeUs - firstTimeUs;
            offsetTimeUs += strm.mLastSampleDurationUs;
        } else {
            offsetTimeUs += strm.mLastSampleDurationUs;
        }

        mDiscontinuityOffsetTimesUs.add(seq, offsetTimeUs);
    } else if (err == OK) {

        if (stream == STREAMTYPE_AUDIO || stream == STREAMTYPE_VIDEO) {
            int64_t timeUs;
            int32_t discontinuitySeq = 0;
            CHECK((*accessUnit)->meta()->findInt64("timeUs",  &timeUs));
            (*accessUnit)->meta()->findInt32("discontinuitySeq", &discontinuitySeq);
            strm.mCurDiscontinuitySeq = discontinuitySeq;

            int32_t discard = 0;
            int64_t firstTimeUs;
            if (mDiscontinuityAbsStartTimesUs.indexOfKey(strm.mCurDiscontinuitySeq) >= 0) {
                int64_t durUs; // approximate sample duration
                if (timeUs > strm.mLastDequeuedTimeUs) {
                    durUs = timeUs - strm.mLastDequeuedTimeUs;
                } else {
                    durUs = strm.mLastDequeuedTimeUs - timeUs;
                }
                strm.mLastSampleDurationUs = durUs;
                firstTimeUs = mDiscontinuityAbsStartTimesUs.valueFor(strm.mCurDiscontinuitySeq);
            } else if ((*accessUnit)->meta()->findInt32("discard", &discard) && discard) {
                firstTimeUs = timeUs;
            } else {
                mDiscontinuityAbsStartTimesUs.add(strm.mCurDiscontinuitySeq, timeUs);
                firstTimeUs = timeUs;
            }

            strm.mLastDequeuedTimeUs = timeUs;
            if (timeUs >= firstTimeUs) {
                timeUs -= firstTimeUs;
            } else {
                timeUs = 0;
            }
            timeUs += mLastSeekTimeUs;
            if (mDiscontinuityOffsetTimesUs.indexOfKey(discontinuitySeq) >= 0) {
                timeUs += mDiscontinuityOffsetTimesUs.valueFor(discontinuitySeq);
            }

            ALOGV("[%s] read buffer at time %" PRId64 " us", streamStr, timeUs);
            (*accessUnit)->meta()->setInt64("timeUs",  timeUs);
            mLastDequeuedTimeUs = timeUs;
            mRealTimeBaseUs = ALooper::GetNowUs() - timeUs;
        } else if (stream == STREAMTYPE_SUBTITLES) {
            int32_t subtitleGeneration;
            if ((*accessUnit)->meta()->findInt32("subtitleGeneration", &subtitleGeneration)
                    && subtitleGeneration != mSubtitleGeneration) {
               return -EAGAIN;
            };
            (*accessUnit)->meta()->setInt32(
                    "trackIndex", mPlaylist->getSelectedIndex());
            (*accessUnit)->meta()->setInt64("baseUs", mRealTimeBaseUs);
        }
    } else {
        ALOGI("[%s] encountered error %d", streamStr, err);
    }

    return err;
}

status_t LiveSession::getStreamFormat(StreamType stream, sp<AMessage> *format) {
    if (!(mStreamMask & stream)) {
        return UNKNOWN_ERROR;
    }

    sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(stream);

    sp<MetaData> meta = packetSource->getFormat();

    if (meta == NULL) {
        return -EAGAIN;
    }

    return convertMetaDataToMessage(meta, format);
}

sp<HTTPBase> LiveSession::getHTTPDataSource() {
    return new MediaHTTP(mHTTPService->makeHTTPConnection());
}

void LiveSession::connectAsync(
        const char *url, const KeyedVector<String8, String8> *headers) {
    sp<AMessage> msg = new AMessage(kWhatConnect, this);
    msg->setString("url", url);

    if (headers != NULL) {
        msg->setPointer(
                "headers",
                new KeyedVector<String8, String8>(*headers));
    }

    msg->post();
}

status_t LiveSession::disconnect() {
    sp<AMessage> msg = new AMessage(kWhatDisconnect, this);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    return err;
}

status_t LiveSession::seekTo(int64_t timeUs) {
    sp<AMessage> msg = new AMessage(kWhatSeek, this);
    msg->setInt64("timeUs", timeUs);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    return err;
}

void LiveSession::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatConnect:
        {
            onConnect(msg);
            break;
        }

        case kWhatDisconnect:
        {
            CHECK(msg->senderAwaitsResponse(&mDisconnectReplyID));

            if (mReconfigurationInProgress) {
                break;
            }

            finishDisconnect();
            break;
        }

        case kWhatSeek:
        {
            sp<AReplyToken> seekReplyID;
            CHECK(msg->senderAwaitsResponse(&seekReplyID));
            mSeekReplyID = seekReplyID;
            mSeekReply = new AMessage;

            status_t err = onSeek(msg);

            if (err != OK) {
                msg->post(50000);
            }
            break;
        }

        case kWhatFetcherNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            switch (what) {
                case PlaylistFetcher::kWhatStarted:
                    break;
                case PlaylistFetcher::kWhatPaused:
                case PlaylistFetcher::kWhatStopped:
                {
                    AString uri;
                    CHECK(msg->findString("uri", &uri));
                    ssize_t index = mFetcherInfos.indexOfKey(uri);
                    if (index < 0) {
                        // ignore msgs from fetchers that's already gone
                        break;
                    }

                    if (what == PlaylistFetcher::kWhatStopped) {
                        tryToFinishBandwidthSwitch(uri);

                        mFetcherLooper->unregisterHandler(
                                mFetcherInfos[index].mFetcher->id());
                        mFetcherInfos.removeItemsAt(index);
                    } else if (what == PlaylistFetcher::kWhatPaused) {
                        int32_t seekMode;
                        CHECK(msg->findInt32("seekMode", &seekMode));
                        for (size_t i = 0; i < kMaxStreams; ++i) {
                            if (mStreams[i].mUri == uri) {
                                mStreams[i].mSeekMode = (SeekMode) seekMode;
                            }
                        }
                    }

                    if (mContinuation != NULL) {
                        CHECK_GT(mContinuationCounter, 0);
                        if (--mContinuationCounter == 0) {
                            mContinuation->post();
                        }
                    }
                    break;
                }

                case PlaylistFetcher::kWhatDurationUpdate:
                {
                    AString uri;
                    CHECK(msg->findString("uri", &uri));

                    int64_t durationUs;
                    CHECK(msg->findInt64("durationUs", &durationUs));

                    ssize_t index = mFetcherInfos.indexOfKey(uri);
                    if (index >= 0) {
                        FetcherInfo *info = &mFetcherInfos.editValueFor(uri);
                        info->mDurationUs = durationUs;
                    }
                    break;
                }

                case PlaylistFetcher::kWhatError:
                {
                    status_t err;
                    CHECK(msg->findInt32("err", &err));

                    ALOGE("XXX Received error %d from PlaylistFetcher.", err);

                    // handle EOS on subtitle tracks independently
                    AString uri;
                    if (err == ERROR_END_OF_STREAM && msg->findString("uri", &uri)) {
                        ssize_t i = mFetcherInfos.indexOfKey(uri);
                        if (i >= 0) {
                            const sp<PlaylistFetcher> &fetcher = mFetcherInfos.valueAt(i).mFetcher;
                            if (fetcher != NULL) {
                                uint32_t type = fetcher->getStreamTypeMask();
                                if (type == STREAMTYPE_SUBTITLES) {
                                    mPacketSources.valueFor(
                                            STREAMTYPE_SUBTITLES)->signalEOS(err);;
                                    break;
                                }
                            }
                        }
                    }

                    if (mInPreparationPhase) {
                        postPrepared(err);
                    }

                    cancelBandwidthSwitch();

                    mPacketSources.valueFor(STREAMTYPE_AUDIO)->signalEOS(err);

                    mPacketSources.valueFor(STREAMTYPE_VIDEO)->signalEOS(err);

                    mPacketSources.valueFor(
                            STREAMTYPE_SUBTITLES)->signalEOS(err);

                    sp<AMessage> notify = mNotify->dup();
                    notify->setInt32("what", kWhatError);
                    notify->setInt32("err", err);
                    notify->post();
                    break;
                }

                case PlaylistFetcher::kWhatStartedAt:
                {
                    int32_t switchGeneration;
                    CHECK(msg->findInt32("switchGeneration", &switchGeneration));

                    if (switchGeneration != mSwitchGeneration) {
                        break;
                    }

                    AString uri;
                    CHECK(msg->findString("uri", &uri));
                    ssize_t index = mFetcherInfos.indexOfKey(uri);
                    if (index >= 0) {
                        mFetcherInfos.editValueAt(index).mToBeResumed = true;
                    }

                    // Resume fetcher for the original variant; the resumed fetcher should
                    // continue until the timestamps found in msg, which is stored by the
                    // new fetcher to indicate where the new variant has started buffering.
                    for (size_t i = 0; i < mFetcherInfos.size(); i++) {
                        const FetcherInfo info = mFetcherInfos.valueAt(i);
                        if (info.mToBeRemoved) {
                            info.mFetcher->resumeUntilAsync(msg);
                        }
                    }
                    break;
                }

                default:
                    TRESPASS();
            }

            break;
        }

        case kWhatChangeConfiguration:
        {
            onChangeConfiguration(msg);
            break;
        }

        case kWhatChangeConfiguration2:
        {
            onChangeConfiguration2(msg);
            break;
        }

        case kWhatChangeConfiguration3:
        {
            onChangeConfiguration3(msg);
            break;
        }

        case kWhatFinishDisconnect2:
        {
            onFinishDisconnect2();
            break;
        }

        case kWhatPollBuffering:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));
            if (generation == mPollBufferingGeneration) {
                onPollBuffering();
            }
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

// static
int LiveSession::SortByBandwidth(const BandwidthItem *a, const BandwidthItem *b) {
    if (a->mBandwidth < b->mBandwidth) {
        return -1;
    } else if (a->mBandwidth == b->mBandwidth) {
        return 0;
    }

    return 1;
}

// static
LiveSession::StreamType LiveSession::indexToType(int idx) {
    CHECK(idx >= 0 && idx < kMaxStreams);
    return (StreamType)(1 << idx);
}

// static
ssize_t LiveSession::typeToIndex(int32_t type) {
    switch (type) {
        case STREAMTYPE_AUDIO:
            return 0;
        case STREAMTYPE_VIDEO:
            return 1;
        case STREAMTYPE_SUBTITLES:
            return 2;
        default:
            return -1;
    };
    return -1;
}

void LiveSession::onConnect(const sp<AMessage> &msg) {
    AString url;
    CHECK(msg->findString("url", &url));

    KeyedVector<String8, String8> *headers = NULL;
    if (!msg->findPointer("headers", (void **)&headers)) {
        mExtraHeaders.clear();
    } else {
        mExtraHeaders = *headers;

        delete headers;
        headers = NULL;
    }

    // TODO currently we don't know if we are coming here from incognito mode
    ALOGI("onConnect %s", uriDebugString(url).c_str());

    mMasterURL = url;

    bool dummy;
    mPlaylist = fetchPlaylist(url.c_str(), NULL /* curPlaylistHash */, &dummy);

    if (mPlaylist == NULL) {
        ALOGE("unable to fetch master playlist %s.", uriDebugString(url).c_str());

        postPrepared(ERROR_IO);
        return;
    }

    // create looper for fetchers
    if (mFetcherLooper == NULL) {
        mFetcherLooper = new ALooper();

        mFetcherLooper->setName("Fetcher");
        mFetcherLooper->start(false, false);
    }

    // We trust the content provider to make a reasonable choice of preferred
    // initial bandwidth by listing it first in the variant playlist.
    // At startup we really don't have a good estimate on the available
    // network bandwidth since we haven't tranferred any data yet. Once
    // we have we can make a better informed choice.
    size_t initialBandwidth = 0;
    size_t initialBandwidthIndex = 0;

    if (mPlaylist->isVariantPlaylist()) {
        for (size_t i = 0; i < mPlaylist->size(); ++i) {
            BandwidthItem item;

            item.mPlaylistIndex = i;

            sp<AMessage> meta;
            AString uri;
            mPlaylist->itemAt(i, &uri, &meta);

            CHECK(meta->findInt32("bandwidth", (int32_t *)&item.mBandwidth));

            if (initialBandwidth == 0) {
                initialBandwidth = item.mBandwidth;
            }

            mBandwidthItems.push(item);
        }

        CHECK_GT(mBandwidthItems.size(), 0u);

        mBandwidthItems.sort(SortByBandwidth);

        for (size_t i = 0; i < mBandwidthItems.size(); ++i) {
            if (mBandwidthItems.itemAt(i).mBandwidth == initialBandwidth) {
                initialBandwidthIndex = i;
                break;
            }
        }
    } else {
        // dummy item.
        BandwidthItem item;
        item.mPlaylistIndex = 0;
        item.mBandwidth = 0;
        mBandwidthItems.push(item);
    }

    mPlaylist->pickRandomMediaItems();
    changeConfiguration(
            0ll /* timeUs */, initialBandwidthIndex, false /* pickTrack */);

    schedulePollBuffering();
}

void LiveSession::finishDisconnect() {
    ALOGV("finishDisconnect");

    // No reconfiguration is currently pending, make sure none will trigger
    // during disconnection either.
    cancelBandwidthSwitch();

    // cancel buffer polling
    cancelPollBuffering();

    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        mFetcherInfos.valueAt(i).mFetcher->stopAsync();
    }

    sp<AMessage> msg = new AMessage(kWhatFinishDisconnect2, this);

    mContinuationCounter = mFetcherInfos.size();
    mContinuation = msg;

    if (mContinuationCounter == 0) {
        msg->post();
    }
}

void LiveSession::onFinishDisconnect2() {
    mContinuation.clear();

    mPacketSources.valueFor(STREAMTYPE_AUDIO)->signalEOS(ERROR_END_OF_STREAM);
    mPacketSources.valueFor(STREAMTYPE_VIDEO)->signalEOS(ERROR_END_OF_STREAM);

    mPacketSources.valueFor(
            STREAMTYPE_SUBTITLES)->signalEOS(ERROR_END_OF_STREAM);

    sp<AMessage> response = new AMessage;
    response->setInt32("err", OK);

    response->postReply(mDisconnectReplyID);
    mDisconnectReplyID.clear();
}

sp<PlaylistFetcher> LiveSession::addFetcher(const char *uri) {
    ssize_t index = mFetcherInfos.indexOfKey(uri);

    if (index >= 0) {
        return NULL;
    }

    sp<AMessage> notify = new AMessage(kWhatFetcherNotify, this);
    notify->setString("uri", uri);
    notify->setInt32("switchGeneration", mSwitchGeneration);

    FetcherInfo info;
    info.mFetcher = new PlaylistFetcher(notify, this, uri, mSubtitleGeneration);
    info.mDurationUs = -1ll;
    info.mToBeRemoved = false;
    info.mToBeResumed = false;
    mFetcherLooper->registerHandler(info.mFetcher);

    mFetcherInfos.add(uri, info);

    return info.mFetcher;
}

/*
 * Illustration of parameters:
 *
 * 0      `range_offset`
 * +------------+-------------------------------------------------------+--+--+
 * |            |                                 | next block to fetch |  |  |
 * |            | `source` handle => `out` buffer |                     |  |  |
 * | `url` file |<--------- buffer size --------->|<--- `block_size` -->|  |  |
 * |            |<----------- `range_length` / buffer capacity ----------->|  |
 * |<------------------------------ file_size ------------------------------->|
 *
 * Special parameter values:
 * - range_length == -1 means entire file
 * - block_size == 0 means entire range
 *
 */
ssize_t LiveSession::fetchFile(
        const char *url, sp<ABuffer> *out,
        int64_t range_offset, int64_t range_length,
        uint32_t block_size, /* download block size */
        sp<DataSource> *source, /* to return and reuse source */
        String8 *actualUrl,
        bool forceConnectHTTP /* force connect HTTP when resuing source */) {
    off64_t size;
    sp<DataSource> temp_source;
    if (source == NULL) {
        source = &temp_source;
    }

    if (*source == NULL || forceConnectHTTP) {
        if (!strncasecmp(url, "file://", 7)) {
            *source = new FileSource(url + 7);
        } else if (strncasecmp(url, "http://", 7)
                && strncasecmp(url, "https://", 8)) {
            return ERROR_UNSUPPORTED;
        } else {
            KeyedVector<String8, String8> headers = mExtraHeaders;
            if (range_offset > 0 || range_length >= 0) {
                headers.add(
                        String8("Range"),
                        String8(
                            AStringPrintf(
                                "bytes=%lld-%s",
                                range_offset,
                                range_length < 0
                                    ? "" : AStringPrintf("%lld",
                                            range_offset + range_length - 1).c_str()).c_str()));
            }

            HTTPBase* httpDataSource =
                    (*source == NULL) ? mHTTPDataSource.get() : (HTTPBase*)source->get();
            status_t err = httpDataSource->connect(url, &headers);

            if (err != OK) {
                return err;
            }

            if (*source == NULL) {
                *source = mHTTPDataSource;
            }
        }
    }

    status_t getSizeErr = (*source)->getSize(&size);
    if (getSizeErr != OK) {
        size = 65536;
    }

    sp<ABuffer> buffer = *out != NULL ? *out : new ABuffer(size);
    if (*out == NULL) {
        buffer->setRange(0, 0);
    }

    ssize_t bytesRead = 0;
    // adjust range_length if only reading partial block
    if (block_size > 0 && (range_length == -1 || (int64_t)(buffer->size() + block_size) < range_length)) {
        range_length = buffer->size() + block_size;
    }
    for (;;) {
        // Only resize when we don't know the size.
        size_t bufferRemaining = buffer->capacity() - buffer->size();
        if (bufferRemaining == 0 && getSizeErr != OK) {
            size_t bufferIncrement = buffer->size() / 2;
            if (bufferIncrement < 32768) {
                bufferIncrement = 32768;
            }
            bufferRemaining = bufferIncrement;

            ALOGV("increasing download buffer to %zu bytes",
                 buffer->size() + bufferRemaining);

            sp<ABuffer> copy = new ABuffer(buffer->size() + bufferRemaining);
            memcpy(copy->data(), buffer->data(), buffer->size());
            copy->setRange(0, buffer->size());

            buffer = copy;
        }

        size_t maxBytesToRead = bufferRemaining;
        if (range_length >= 0) {
            int64_t bytesLeftInRange = range_length - buffer->size();
            if (bytesLeftInRange < (int64_t)maxBytesToRead) {
                maxBytesToRead = bytesLeftInRange;

                if (bytesLeftInRange == 0) {
                    break;
                }
            }
        }

        // The DataSource is responsible for informing us of error (n < 0) or eof (n == 0)
        // to help us break out of the loop.
        ssize_t n = (*source)->readAt(
                buffer->size(), buffer->data() + buffer->size(),
                maxBytesToRead);

        if (n < 0) {
            return n;
        }

        if (n == 0) {
            break;
        }

        buffer->setRange(0, buffer->size() + (size_t)n);
        bytesRead += n;
    }

    *out = buffer;
    if (actualUrl != NULL) {
        *actualUrl = (*source)->getUri();
        if (actualUrl->isEmpty()) {
            *actualUrl = url;
        }
    }

    return bytesRead;
}

sp<M3UParser> LiveSession::fetchPlaylist(
        const char *url, uint8_t *curPlaylistHash, bool *unchanged) {
    ALOGV("fetchPlaylist '%s'", url);

    *unchanged = false;

    sp<ABuffer> buffer;
    String8 actualUrl;
    ssize_t  err = fetchFile(url, &buffer, 0, -1, 0, NULL, &actualUrl);

    if (err <= 0) {
        return NULL;
    }

    // MD5 functionality is not available on the simulator, treat all
    // playlists as changed.

#if defined(HAVE_ANDROID_OS)
    uint8_t hash[16];

    MD5_CTX m;
    MD5_Init(&m);
    MD5_Update(&m, buffer->data(), buffer->size());

    MD5_Final(hash, &m);

    if (curPlaylistHash != NULL && !memcmp(hash, curPlaylistHash, 16)) {
        // playlist unchanged
        *unchanged = true;

        return NULL;
    }

    if (curPlaylistHash != NULL) {
        memcpy(curPlaylistHash, hash, sizeof(hash));
    }
#endif

    sp<M3UParser> playlist =
        new M3UParser(actualUrl.string(), buffer->data(), buffer->size());

    if (playlist->initCheck() != OK) {
        ALOGE("failed to parse .m3u8 playlist");

        return NULL;
    }

    return playlist;
}

#if 0
static double uniformRand() {
    return (double)rand() / RAND_MAX;
}
#endif

float LiveSession::getAbortThreshold(
        ssize_t currentBWIndex, ssize_t targetBWIndex) const {
    float abortThreshold = -1.0f;
    if (currentBWIndex > 0 && targetBWIndex < currentBWIndex) {
        /*
           If we're switching down, we need to decide whether to

           1) finish last segment of high-bandwidth variant, or
           2) abort last segment of high-bandwidth variant, and fetch an
              overlapping portion from low-bandwidth variant.

           Here we try to maximize the amount of buffer left when the
           switch point is met. Given the following parameters:

           B: our current buffering level in seconds
           T: target duration in seconds
           X: sample duration in seconds remain to fetch in last segment
           bw0: bandwidth of old variant (as specified in playlist)
           bw1: bandwidth of new variant (as specified in playlist)
           bw: measured bandwidth available

           If we choose 1), when switch happens at the end of current
           segment, our buffering will be
                  B + X - X * bw0 / bw

           If we choose 2), when switch happens where we aborted current
           segment, our buffering will be
                  B - (T - X) * bw1 / bw

           We should only choose 1) if
                  X/T < bw1 / (bw1 + bw0 - bw)
        */

        CHECK(mLastBandwidthBps >= 0);
        abortThreshold =
                (float)mBandwidthItems.itemAt(targetBWIndex).mBandwidth
             / ((float)mBandwidthItems.itemAt(targetBWIndex).mBandwidth
              + (float)mBandwidthItems.itemAt(currentBWIndex).mBandwidth
              - (float)mLastBandwidthBps * 0.7f);
        if (abortThreshold < 0.0f) {
            abortThreshold = -1.0f; // do not abort
        }
        ALOGV("Switching Down: bps %ld => %ld, measured %d, abort ratio %.2f",
                mBandwidthItems.itemAt(currentBWIndex).mBandwidth,
                mBandwidthItems.itemAt(targetBWIndex).mBandwidth,
                mLastBandwidthBps,
                abortThreshold);
    }
    return abortThreshold;
}

void LiveSession::addBandwidthMeasurement(size_t numBytes, int64_t delayUs) {
    mBandwidthEstimator->addBandwidthMeasurement(numBytes, delayUs);
}

size_t LiveSession::getBandwidthIndex(int32_t bandwidthBps) {
    if (mBandwidthItems.size() < 2) {
        // shouldn't be here if we only have 1 bandwidth, check
        // logic to get rid of redundant bandwidth polling
        ALOGW("getBandwidthIndex() called for single bandwidth playlist!");
        return 0;
    }

#if 1
    char value[PROPERTY_VALUE_MAX];
    ssize_t index = -1;
    if (property_get("media.httplive.bw-index", value, NULL)) {
        char *end;
        index = strtol(value, &end, 10);
        CHECK(end > value && *end == '\0');

        if (index >= 0 && (size_t)index >= mBandwidthItems.size()) {
            index = mBandwidthItems.size() - 1;
        }
    }

    if (index < 0) {
        char value[PROPERTY_VALUE_MAX];
        if (property_get("media.httplive.max-bw", value, NULL)) {
            char *end;
            long maxBw = strtoul(value, &end, 10);
            if (end > value && *end == '\0') {
                if (maxBw > 0 && bandwidthBps > maxBw) {
                    ALOGV("bandwidth capped to %ld bps", maxBw);
                    bandwidthBps = maxBw;
                }
            }
        }

        // Pick the highest bandwidth stream below or equal to estimated bandwidth.

        index = mBandwidthItems.size() - 1;
        while (index > 0) {
            // be conservative (70%) to avoid overestimating and immediately
            // switching down again.
            size_t adjustedBandwidthBps = bandwidthBps * 7 / 10;
            if (mBandwidthItems.itemAt(index).mBandwidth <= adjustedBandwidthBps) {
                break;
            }
            --index;
        }
    }
#elif 0
    // Change bandwidth at random()
    size_t index = uniformRand() * mBandwidthItems.size();
#elif 0
    // There's a 50% chance to stay on the current bandwidth and
    // a 50% chance to switch to the next higher bandwidth (wrapping around
    // to lowest)
    const size_t kMinIndex = 0;

    static ssize_t mCurBandwidthIndex = -1;

    size_t index;
    if (mCurBandwidthIndex < 0) {
        index = kMinIndex;
    } else if (uniformRand() < 0.5) {
        index = (size_t)mCurBandwidthIndex;
    } else {
        index = mCurBandwidthIndex + 1;
        if (index == mBandwidthItems.size()) {
            index = kMinIndex;
        }
    }
    mCurBandwidthIndex = index;
#elif 0
    // Pick the highest bandwidth stream below or equal to 1.2 Mbit/sec

    size_t index = mBandwidthItems.size() - 1;
    while (index > 0 && mBandwidthItems.itemAt(index).mBandwidth > 1200000) {
        --index;
    }
#elif 1
    char value[PROPERTY_VALUE_MAX];
    size_t index;
    if (property_get("media.httplive.bw-index", value, NULL)) {
        char *end;
        index = strtoul(value, &end, 10);
        CHECK(end > value && *end == '\0');

        if (index >= mBandwidthItems.size()) {
            index = mBandwidthItems.size() - 1;
        }
    } else {
        index = 0;
    }
#else
    size_t index = mBandwidthItems.size() - 1;  // Highest bandwidth stream
#endif

    CHECK_GE(index, 0);

    return index;
}

int64_t LiveSession::latestMediaSegmentStartTimeUs() {
    sp<AMessage> audioMeta = mPacketSources.valueFor(STREAMTYPE_AUDIO)->getLatestDequeuedMeta();
    int64_t minSegmentStartTimeUs = -1, videoSegmentStartTimeUs = -1;
    if (audioMeta != NULL) {
        audioMeta->findInt64("segmentStartTimeUs", &minSegmentStartTimeUs);
    }

    sp<AMessage> videoMeta = mPacketSources.valueFor(STREAMTYPE_VIDEO)->getLatestDequeuedMeta();
    if (videoMeta != NULL
            && videoMeta->findInt64("segmentStartTimeUs", &videoSegmentStartTimeUs)) {
        if (minSegmentStartTimeUs < 0 || videoSegmentStartTimeUs < minSegmentStartTimeUs) {
            minSegmentStartTimeUs = videoSegmentStartTimeUs;
        }

    }
    return minSegmentStartTimeUs;
}

status_t LiveSession::onSeek(const sp<AMessage> &msg) {
    int64_t timeUs;
    CHECK(msg->findInt64("timeUs", &timeUs));

    if (!mReconfigurationInProgress) {
        changeConfiguration(timeUs, mCurBandwidthIndex);
        return OK;
    } else {
        return -EWOULDBLOCK;
    }
}

status_t LiveSession::getDuration(int64_t *durationUs) const {
    int64_t maxDurationUs = -1ll;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        int64_t fetcherDurationUs = mFetcherInfos.valueAt(i).mDurationUs;

        if (fetcherDurationUs > maxDurationUs) {
            maxDurationUs = fetcherDurationUs;
        }
    }

    *durationUs = maxDurationUs;

    return OK;
}

bool LiveSession::isSeekable() const {
    int64_t durationUs;
    return getDuration(&durationUs) == OK && durationUs >= 0;
}

bool LiveSession::hasDynamicDuration() const {
    return false;
}

size_t LiveSession::getTrackCount() const {
    if (mPlaylist == NULL) {
        return 0;
    } else {
        return mPlaylist->getTrackCount();
    }
}

sp<AMessage> LiveSession::getTrackInfo(size_t trackIndex) const {
    if (mPlaylist == NULL) {
        return NULL;
    } else {
        return mPlaylist->getTrackInfo(trackIndex);
    }
}

status_t LiveSession::selectTrack(size_t index, bool select) {
    if (mPlaylist == NULL) {
        return INVALID_OPERATION;
    }

    ++mSubtitleGeneration;
    status_t err = mPlaylist->selectTrack(index, select);
    if (err == OK) {
        sp<AMessage> msg = new AMessage(kWhatChangeConfiguration, this);
        msg->setInt32("bandwidthIndex", mCurBandwidthIndex);
        msg->setInt32("pickTrack", select);
        msg->post();
    }
    return err;
}

ssize_t LiveSession::getSelectedTrack(media_track_type type) const {
    if (mPlaylist == NULL) {
        return -1;
    } else {
        return mPlaylist->getSelectedTrack(type);
    }
}

void LiveSession::changeConfiguration(
        int64_t timeUs, size_t bandwidthIndex, bool pickTrack) {
    // Protect mPacketSources from a swapPacketSource race condition through reconfiguration.
    // (changeConfiguration, onChangeConfiguration2, onChangeConfiguration3).
    cancelBandwidthSwitch();

    CHECK(!mReconfigurationInProgress);
    mReconfigurationInProgress = true;

    ALOGV("changeConfiguration => timeUs:%" PRId64 " us, bwIndex:%zu, pickTrack:%d",
          timeUs, bandwidthIndex, pickTrack);

    CHECK_LT(bandwidthIndex, mBandwidthItems.size());
    const BandwidthItem &item = mBandwidthItems.itemAt(bandwidthIndex);

    uint32_t streamMask = 0; // streams that should be fetched by the new fetcher
    uint32_t resumeMask = 0; // streams that should be fetched by the original fetcher

    AString URIs[kMaxStreams];
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (mPlaylist->getTypeURI(item.mPlaylistIndex, mStreams[i].mType, &URIs[i])) {
            streamMask |= indexToType(i);
        }
    }

    // Step 1, stop and discard fetchers that are no longer needed.
    // Pause those that we'll reuse.
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        const AString &uri = mFetcherInfos.keyAt(i);

        bool discardFetcher = true;

        if (timeUs < 0ll) {
            // delay fetcher removal if not picking tracks
            discardFetcher = pickTrack;
        }

        for (size_t j = 0; j < kMaxStreams; ++j) {
            StreamType type = indexToType(j);
            if ((streamMask & type) && uri == URIs[j]) {
                resumeMask |= type;
                streamMask &= ~type;
                discardFetcher = false;
            }
        }

        if (discardFetcher) {
            mFetcherInfos.valueAt(i).mFetcher->stopAsync();
        } else {
            float threshold = -1.0f; // always finish fetching by default
            if (timeUs >= 0ll) {
                // seeking, no need to finish fetching
                threshold = 0.0f;
            } else if (!pickTrack) {
                // adapting, abort if remaining of current segment is over threshold
                threshold = getAbortThreshold(
                        mCurBandwidthIndex, bandwidthIndex);
            }

            ALOGV("Pausing with threshold %.3f", threshold);

            mFetcherInfos.valueAt(i).mFetcher->pauseAsync(threshold);
        }
    }

    mCurBandwidthIndex = bandwidthIndex;

    sp<AMessage> msg;
    if (timeUs < 0ll) {
        // skip onChangeConfiguration2 (decoder destruction) if not seeking.
        msg = new AMessage(kWhatChangeConfiguration3, this);
    } else {
        msg = new AMessage(kWhatChangeConfiguration2, this);
    }
    msg->setInt32("streamMask", streamMask);
    msg->setInt32("resumeMask", resumeMask);
    msg->setInt32("pickTrack", pickTrack);
    msg->setInt64("timeUs", timeUs);
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if ((streamMask | resumeMask) & indexToType(i)) {
            msg->setString(mStreams[i].uriKey().c_str(), URIs[i].c_str());
        }
    }

    // Every time a fetcher acknowledges the stopAsync or pauseAsync request
    // we'll decrement mContinuationCounter, once it reaches zero, i.e. all
    // fetchers have completed their asynchronous operation, we'll post
    // mContinuation, which then is handled below in onChangeConfiguration2.
    mContinuationCounter = mFetcherInfos.size();
    mContinuation = msg;

    if (mContinuationCounter == 0) {
        msg->post();
    }
}

void LiveSession::onChangeConfiguration(const sp<AMessage> &msg) {
    if (!mReconfigurationInProgress) {
        int32_t pickTrack = 0, bandwidthIndex = mCurBandwidthIndex;
        msg->findInt32("pickTrack", &pickTrack);
        msg->findInt32("bandwidthIndex", &bandwidthIndex);
        changeConfiguration(-1ll /* timeUs */, bandwidthIndex, pickTrack);
    } else {
        msg->post(1000000ll); // retry in 1 sec
    }
}

void LiveSession::onChangeConfiguration2(const sp<AMessage> &msg) {
    mContinuation.clear();

    // All fetchers are either suspended or have been removed now.

    // If we're seeking, clear all packet sources before we report
    // seek complete, to prevent decoder from pulling stale data.
    int64_t timeUs;
    CHECK(msg->findInt64("timeUs", &timeUs));

    if (timeUs >= 0) {
        mLastSeekTimeUs = timeUs;

        for (size_t i = 0; i < mPacketSources.size(); i++) {
            mPacketSources.editValueAt(i)->clear();
        }

        mDiscontinuityOffsetTimesUs.clear();
        mDiscontinuityAbsStartTimesUs.clear();

        if (mSeekReplyID != NULL) {
            CHECK(mSeekReply != NULL);
            mSeekReply->setInt32("err", OK);
            mSeekReply->postReply(mSeekReplyID);
            mSeekReplyID.clear();
            mSeekReply.clear();
        }
    }

    uint32_t streamMask, resumeMask;
    CHECK(msg->findInt32("streamMask", (int32_t *)&streamMask));
    CHECK(msg->findInt32("resumeMask", (int32_t *)&resumeMask));

    streamMask |= resumeMask;

    AString URIs[kMaxStreams];
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (streamMask & indexToType(i)) {
            const AString &uriKey = mStreams[i].uriKey();
            CHECK(msg->findString(uriKey.c_str(), &URIs[i]));
            ALOGV("%s = '%s'", uriKey.c_str(), URIs[i].c_str());
        }
    }

    uint32_t changedMask = 0;
    for (size_t i = 0; i < kMaxStreams && i != kSubtitleIndex; ++i) {
        // stream URI could change even if onChangeConfiguration2 is only
        // used for seek. Seek could happen during a bw switch, in this
        // case bw switch will be cancelled, but the seekTo position will
        // fetch from the new URI.
        if ((mStreamMask & streamMask & indexToType(i))
                && !mStreams[i].mUri.empty()
                && !(URIs[i] == mStreams[i].mUri)) {
            ALOGV("stream %zu changed: oldURI %s, newURI %s", i,
                    mStreams[i].mUri.c_str(), URIs[i].c_str());
            sp<AnotherPacketSource> source = mPacketSources.valueFor(indexToType(i));
            source->queueDiscontinuity(
                    ATSParser::DISCONTINUITY_FORMATCHANGE, NULL, true);
        }
        // Determine which decoders to shutdown on the player side,
        // a decoder has to be shutdown if its streamtype was active
        // before but now longer isn't.
        if ((mStreamMask & ~streamMask & indexToType(i))) {
            changedMask |= indexToType(i);
        }
    }

    if (changedMask == 0) {
        // If nothing changed as far as the audio/video decoders
        // are concerned we can proceed.
        onChangeConfiguration3(msg);
        return;
    }

    // Something changed, inform the player which will shutdown the
    // corresponding decoders and will post the reply once that's done.
    // Handling the reply will continue executing below in
    // onChangeConfiguration3.
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatStreamsChanged);
    notify->setInt32("changedMask", changedMask);

    msg->setWhat(kWhatChangeConfiguration3);
    msg->setTarget(this);

    notify->setMessage("reply", msg);
    notify->post();
}

void LiveSession::onChangeConfiguration3(const sp<AMessage> &msg) {
    mContinuation.clear();
    // All remaining fetchers are still suspended, the player has shutdown
    // any decoders that needed it.

    uint32_t streamMask, resumeMask;
    CHECK(msg->findInt32("streamMask", (int32_t *)&streamMask));
    CHECK(msg->findInt32("resumeMask", (int32_t *)&resumeMask));

    int64_t timeUs;
    int32_t pickTrack;
    bool switching = false;
    CHECK(msg->findInt64("timeUs", &timeUs));
    CHECK(msg->findInt32("pickTrack", &pickTrack));

    if (timeUs < 0ll) {
        if (!pickTrack) {
            switching = true;
        }
        mRealTimeBaseUs = ALooper::GetNowUs() - mLastDequeuedTimeUs;
    } else {
        mRealTimeBaseUs = ALooper::GetNowUs() - timeUs;
    }

    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (streamMask & indexToType(i)) {
            if (switching) {
                CHECK(msg->findString(mStreams[i].uriKey().c_str(), &mStreams[i].mNewUri));
            } else {
                CHECK(msg->findString(mStreams[i].uriKey().c_str(), &mStreams[i].mUri));
            }
        }
    }

    mNewStreamMask = streamMask | resumeMask;
    if (switching) {
        mSwapMask = mStreamMask & ~resumeMask;
    }

    // Of all existing fetchers:
    // * Resume fetchers that are still needed and assign them original packet sources.
    // * Mark otherwise unneeded fetchers for removal.
    ALOGV("resuming fetchers for mask 0x%08x", resumeMask);
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        const AString &uri = mFetcherInfos.keyAt(i);

        sp<AnotherPacketSource> sources[kMaxStreams];
        for (size_t j = 0; j < kMaxStreams; ++j) {
            if ((resumeMask & indexToType(j)) && uri == mStreams[j].mUri) {
                sources[j] = mPacketSources.valueFor(indexToType(j));
            }
        }
        FetcherInfo &info = mFetcherInfos.editValueAt(i);
        if (sources[kAudioIndex] != NULL || sources[kVideoIndex] != NULL
                || sources[kSubtitleIndex] != NULL) {
            info.mFetcher->startAsync(
                    sources[kAudioIndex], sources[kVideoIndex], sources[kSubtitleIndex], timeUs);
        } else {
            info.mToBeRemoved = true;
        }
    }

    // streamMask now only contains the types that need a new fetcher created.
    if (streamMask != 0) {
        ALOGV("creating new fetchers for mask 0x%08x", streamMask);
    }

    // Find out when the original fetchers have buffered up to and start the new fetchers
    // at a later timestamp.
    for (size_t i = 0; i < kMaxStreams; i++) {
        if (!(indexToType(i) & streamMask)) {
            continue;
        }

        AString uri;
        uri = switching ? mStreams[i].mNewUri : mStreams[i].mUri;

        sp<PlaylistFetcher> fetcher = addFetcher(uri.c_str());
        CHECK(fetcher != NULL);

        int64_t startTimeUs = -1;
        int64_t segmentStartTimeUs = -1ll;
        int32_t discontinuitySeq = -1;
        SeekMode seekMode = kSeekModeExactPosition;
        sp<AnotherPacketSource> sources[kMaxStreams];

        if (i == kSubtitleIndex) {
            segmentStartTimeUs = latestMediaSegmentStartTimeUs();
        }

        // TRICKY: looping from i as earlier streams are already removed from streamMask
        for (size_t j = i; j < kMaxStreams; ++j) {
            const AString &streamUri = switching ? mStreams[j].mNewUri : mStreams[j].mUri;
            if ((streamMask & indexToType(j)) && uri == streamUri) {
                sources[j] = mPacketSources.valueFor(indexToType(j));

                if (timeUs >= 0) {
                    startTimeUs = timeUs;
                } else {
                    int32_t type;
                    sp<AMessage> meta;
                    if (pickTrack) {
                        // selecting

                        // FIXME:
                        // This should only apply to the track that's being picked, we
                        // need a bitmask to indicate that.
                        //
                        // It's possible that selectTrack() gets called during a bandwidth
                        // switch, and we needed to fetch a new variant. The new fetcher
                        // should start from where old fetcher left off, not where decoder
                        // is dequeueing at.

                        meta = sources[j]->getLatestDequeuedMeta();
                    } else {
                        // adapting
                        meta = sources[j]->getLatestEnqueuedMeta();
                    }

                    if (meta != NULL && !meta->findInt32("discontinuity", &type)) {
                        int64_t tmpUs;
                        int64_t tmpSegmentUs;

                        CHECK(meta->findInt64("timeUs", &tmpUs));
                        CHECK(meta->findInt64("segmentStartTimeUs", &tmpSegmentUs));
                        if (startTimeUs < 0 || tmpSegmentUs < segmentStartTimeUs) {
                            startTimeUs = tmpUs;
                            segmentStartTimeUs = tmpSegmentUs;
                        } else if (tmpSegmentUs == segmentStartTimeUs && tmpUs < startTimeUs) {
                            startTimeUs = tmpUs;
                        }

                        int32_t seq;
                        CHECK(meta->findInt32("discontinuitySeq", &seq));
                        if (discontinuitySeq < 0 || seq < discontinuitySeq) {
                            discontinuitySeq = seq;
                        }
                    }

                    if (pickTrack) {
                        // selecting track, queue discontinuities before content
                        sources[j]->clear();
                        if (j == kSubtitleIndex) {
                            break;
                        }

                        ALOGV("stream[%zu]: queue format change", j);

                        sources[j]->queueDiscontinuity(
                                ATSParser::DISCONTINUITY_FORMAT_ONLY, NULL, true);
                    } else {
                        // adapting, queue discontinuities after resume
                        sources[j] = mPacketSources2.valueFor(indexToType(j));
                        sources[j]->clear();
                        uint32_t extraStreams = mNewStreamMask & (~mStreamMask);
                        if (extraStreams & indexToType(j)) {
                            sources[j]->queueDiscontinuity(
                                ATSParser::DISCONTINUITY_FORMAT_ONLY, NULL, true);
                        }
                        // the new fetcher might be providing streams that used to be
                        // provided by two different fetchers,  if one of the fetcher
                        // paused in the middle while the other somehow paused in next
                        // seg, we have to start from next seg.
                        if (seekMode < mStreams[j].mSeekMode) {
                            seekMode = mStreams[j].mSeekMode;
                        }
                    }
                }

                streamMask &= ~indexToType(j);
            }
        }

        fetcher->startAsync(
                sources[kAudioIndex],
                sources[kVideoIndex],
                sources[kSubtitleIndex],
                startTimeUs < 0 ? mLastSeekTimeUs : startTimeUs,
                segmentStartTimeUs,
                discontinuitySeq,
                seekMode);
    }

    // All fetchers have now been started, the configuration change
    // has completed.

    ALOGV("XXX configuration change completed.");
    mReconfigurationInProgress = false;
    if (switching) {
        mSwitchInProgress = true;
    } else {
        mStreamMask = mNewStreamMask;
    }

    if (mDisconnectReplyID != NULL) {
        finishDisconnect();
    }
}

void LiveSession::swapPacketSource(StreamType stream) {
    ALOGV("swapPacketSource: stream = %d", stream);

    // transfer packets from source2 to source
    sp<AnotherPacketSource> &aps = mPacketSources.editValueFor(stream);
    sp<AnotherPacketSource> &aps2 = mPacketSources2.editValueFor(stream);

    // queue discontinuity in mPacketSource
    aps->queueDiscontinuity(ATSParser::DISCONTINUITY_FORMAT_ONLY, NULL, false);

    // queue packets in mPacketSource2 to mPacketSource
    status_t finalResult = OK;
    sp<ABuffer> accessUnit;
    while (aps2->hasBufferAvailable(&finalResult) && finalResult == OK &&
          OK == aps2->dequeueAccessUnit(&accessUnit)) {
        aps->queueAccessUnit(accessUnit);
    }
    aps2->clear();
}

void LiveSession::tryToFinishBandwidthSwitch(const AString &uri) {
    if (!mSwitchInProgress) {
        return;
    }

    ssize_t index = mFetcherInfos.indexOfKey(uri);
    if (index < 0 || !mFetcherInfos[index].mToBeRemoved) {
        return;
    }

    // Swap packet source of streams provided by old variant
    for (size_t idx = 0; idx < kMaxStreams; idx++) {
        if (uri == mStreams[idx].mUri) {
            StreamType stream = indexToType(idx);

            swapPacketSource(stream);

            if ((mNewStreamMask & stream) && mStreams[idx].mNewUri.empty()) {
                ALOGW("swapping stream type %d %s to empty stream",
                        stream, mStreams[idx].mUri.c_str());
            }
            mStreams[idx].mUri = mStreams[idx].mNewUri;
            mStreams[idx].mNewUri.clear();

            mSwapMask &= ~stream;
        }
    }

    mFetcherInfos.editValueAt(index).mToBeRemoved = false;

    ALOGV("tryToFinishBandwidthSwitch: mSwapMask=%x", mSwapMask);
    if (mSwapMask != 0) {
        return;
    }

    // Check if new variant contains extra streams.
    uint32_t extraStreams = mNewStreamMask & (~mStreamMask);
    while (extraStreams) {
        StreamType stream = (StreamType) (extraStreams & ~(extraStreams - 1));
        extraStreams &= ~stream;

        swapPacketSource(stream);

        ssize_t idx = typeToIndex(stream);
        CHECK(idx >= 0);
        if (mStreams[idx].mNewUri.empty()) {
            ALOGW("swapping extra stream type %d %s to empty stream",
                    stream, mStreams[idx].mUri.c_str());
        }
        mStreams[idx].mUri = mStreams[idx].mNewUri;
        mStreams[idx].mNewUri.clear();
    }

    // Restart new fetcher (it was paused after the first 47k block)
    // and let it fetch into mPacketSources (not mPacketSources2)
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        FetcherInfo &info = mFetcherInfos.editValueAt(i);
        if (info.mToBeResumed) {
            const AString &uri = mFetcherInfos.keyAt(i);
            sp<AnotherPacketSource> sources[kMaxStreams];
            for (size_t j = 0; j < kMaxStreams; ++j) {
                if (uri == mStreams[j].mUri) {
                    sources[j] = mPacketSources.valueFor(indexToType(j));
                }
            }
            if (sources[kAudioIndex] != NULL
                    || sources[kVideoIndex] != NULL
                    || sources[kSubtitleIndex] != NULL) {
                ALOGV("resuming fetcher %s", uri.c_str());
                info.mFetcher->startAsync(
                        sources[kAudioIndex],
                        sources[kVideoIndex],
                        sources[kSubtitleIndex]);
            }
            info.mToBeResumed = false;
        }
    }

    mStreamMask = mNewStreamMask;
    mSwitchInProgress = false;

    ALOGI("#### Finished Bandwidth Switch");
}

void LiveSession::schedulePollBuffering() {
    sp<AMessage> msg = new AMessage(kWhatPollBuffering, this);
    msg->setInt32("generation", mPollBufferingGeneration);
    msg->post(1000000ll);
}

void LiveSession::cancelPollBuffering() {
    ++mPollBufferingGeneration;
}

void LiveSession::onPollBuffering() {
    ALOGV("onPollBuffering: mSwitchInProgress %d, mReconfigurationInProgress %d, "
            "mInPreparationPhase %d, mCurBandwidthIndex %zd, mStreamMask 0x%x",
        mSwitchInProgress, mReconfigurationInProgress,
        mInPreparationPhase, mCurBandwidthIndex, mStreamMask);

    bool low, mid, high;
    if (checkBuffering(low, mid, high)) {
        if (mInPreparationPhase && mid) {
            postPrepared(OK);
        }

        // don't switch before we report prepared
        if (!mInPreparationPhase) {
            switchBandwidthIfNeeded(high, !mid);
        }
    }

    schedulePollBuffering();
}

void LiveSession::cancelBandwidthSwitch() {
    ALOGV("cancelBandwidthSwitch: mSwitchGen(%d)++", mSwitchGeneration);

    mSwitchGeneration++;
    mSwitchInProgress = false;
    mSwapMask = 0;

    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        FetcherInfo& info = mFetcherInfos.editValueAt(i);
        if (info.mToBeRemoved) {
            info.mToBeRemoved = false;
        }
    }

    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (!mStreams[i].mNewUri.empty()) {
            ssize_t j = mFetcherInfos.indexOfKey(mStreams[i].mNewUri);
            if (j < 0) {
                mStreams[i].mNewUri.clear();
                continue;
            }

            const FetcherInfo &info = mFetcherInfos.valueAt(j);
            info.mFetcher->stopAsync();
            mFetcherInfos.removeItemsAt(j);
            mStreams[i].mNewUri.clear();
        }
    }
}

bool LiveSession::checkBuffering(bool &low, bool &mid, bool &high) {
    low = mid = high = false;

    if (mSwitchInProgress || mReconfigurationInProgress) {
        ALOGV("Switch/Reconfig in progress, defer buffer polling");
        return false;
    }

    // TODO: Fine tune low/high mark.
    //       We also need to pause playback if buffering is too low.
    //       Currently during underflow, we depend on decoder to starve
    //       to pause, but A/V could have different buffering left,
    //       they're not paused together.
    // TODO: Report buffering level to NuPlayer for BUFFERING_UPDATE

    // Switch down if any of the fetchers are below low mark;
    // Switch up   if all of the fetchers are over high mark.
    size_t activeCount, lowCount, midCount, highCount;
    activeCount = lowCount = midCount = highCount = 0;
    for (size_t i = 0; i < mPacketSources.size(); ++i) {
        // we don't check subtitles for buffering level
        if (!(mStreamMask & mPacketSources.keyAt(i)
                & (STREAMTYPE_AUDIO | STREAMTYPE_VIDEO))) {
            continue;
        }
        // ignore streams that never had any packet queued.
        // (it's possible that the variant only has audio or video)
        sp<AMessage> meta = mPacketSources[i]->getLatestEnqueuedMeta();
        if (meta == NULL) {
            continue;
        }

        ++activeCount;
        int64_t bufferedDurationUs =
                mPacketSources[i]->getEstimatedDurationUs();
        ALOGV("source[%zu]: buffered %lld us", i, (long long)bufferedDurationUs);
        if (bufferedDurationUs < kLowWaterMark) {
            ++lowCount;
            break;
        } else if (bufferedDurationUs > kHighWaterMark) {
            ++midCount;
            ++highCount;
        } else if (bufferedDurationUs > kMidWaterMark) {
            ++midCount;
        }
    }

    if (activeCount > 0) {
        high = (highCount == activeCount);
        mid = (midCount == activeCount);
        low = (lowCount > 0);
        return true;
    }

    return false;
}

void LiveSession::switchBandwidthIfNeeded(bool bufferHigh, bool bufferLow) {
    // no need to check bandwidth if we only have 1 bandwidth settings
    if (mBandwidthItems.size() < 2) {
        return;
    }

    int32_t bandwidthBps;
    if (mBandwidthEstimator->estimateBandwidth(&bandwidthBps)) {
        ALOGV("bandwidth estimated at %.2f kbps", bandwidthBps / 1024.0f);
        mLastBandwidthBps = bandwidthBps;
    } else {
        ALOGV("no bandwidth estimate.");
        return;
    }

    int32_t curBandwidth = mBandwidthItems.itemAt(mCurBandwidthIndex).mBandwidth;
    bool bandwidthLow = bandwidthBps < (int32_t)curBandwidth * 8 / 10;
    bool bandwidthHigh = bandwidthBps > (int32_t)curBandwidth * 12 / 10;

    if ((bufferHigh && bandwidthHigh) || (bufferLow && bandwidthLow)) {
        ssize_t bandwidthIndex = getBandwidthIndex(bandwidthBps);

        if (bandwidthIndex == mCurBandwidthIndex
                || (bufferHigh && bandwidthIndex < mCurBandwidthIndex)
                || (bufferLow && bandwidthIndex > mCurBandwidthIndex)) {
            return;
        }

        ALOGI("#### Starting Bandwidth Switch: %zd => %zd",
                mCurBandwidthIndex, bandwidthIndex);
        changeConfiguration(-1, bandwidthIndex, false);
    }
}

void LiveSession::postPrepared(status_t err) {
    CHECK(mInPreparationPhase);

    sp<AMessage> notify = mNotify->dup();
    if (err == OK || err == ERROR_END_OF_STREAM) {
        notify->setInt32("what", kWhatPrepared);
    } else {
        notify->setInt32("what", kWhatPreparationFailed);
        notify->setInt32("err", err);
    }

    notify->post();

    mInPreparationPhase = false;
}


}  // namespace android

