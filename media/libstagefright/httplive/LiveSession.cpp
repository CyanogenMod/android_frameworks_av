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

LiveSession::LiveSession(
        const sp<AMessage> &notify, uint32_t flags,
        const sp<IMediaHTTPService> &httpService)
    : mNotify(notify),
      mFlags(flags),
      mHTTPService(httpService),
      mInPreparationPhase(true),
      mHTTPDataSource(new MediaHTTP(mHTTPService->makeHTTPConnection())),
      mCurBandwidthIndex(-1),
      mStreamMask(0),
      mNewStreamMask(0),
      mSwapMask(0),
      mCheckBandwidthGeneration(0),
      mSwitchGeneration(0),
      mLastDequeuedTimeUs(0ll),
      mRealTimeBaseUs(0ll),
      mReconfigurationInProgress(false),
      mSwitchInProgress(false),
      mDisconnectReplyID(0),
      mSeekReplyID(0),
      mFirstTimeUsValid(false),
      mFirstTimeUs(0),
      mLastSeekTimeUs(0) {

    mStreams[kAudioIndex] = StreamItem("audio");
    mStreams[kVideoIndex] = StreamItem("video");
    mStreams[kSubtitleIndex] = StreamItem("subtitles");

    for (size_t i = 0; i < kMaxStreams; ++i) {
        mDiscontinuities.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mPacketSources.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mPacketSources2.add(indexToType(i), new AnotherPacketSource(NULL /* meta */));
        mBuffering[i] = false;
    }
}

LiveSession::~LiveSession() {
}

sp<ABuffer> LiveSession::createFormatChangeBuffer(bool swap) {
    ABuffer *discontinuity = new ABuffer(0);
    discontinuity->meta()->setInt32("discontinuity", ATSParser::DISCONTINUITY_FORMATCHANGE);
    discontinuity->meta()->setInt32("swapPacketSource", swap);
    discontinuity->meta()->setInt32("switchGeneration", mSwitchGeneration);
    discontinuity->meta()->setInt64("timeUs", -1);
    return discontinuity;
}

void LiveSession::swapPacketSource(StreamType stream) {
    sp<AnotherPacketSource> &aps = mPacketSources.editValueFor(stream);
    sp<AnotherPacketSource> &aps2 = mPacketSources2.editValueFor(stream);
    sp<AnotherPacketSource> tmp = aps;
    aps = aps2;
    aps2 = tmp;
    aps2->clear();
}

status_t LiveSession::dequeueAccessUnit(
        StreamType stream, sp<ABuffer> *accessUnit) {
    if (!(mStreamMask & stream)) {
        // return -EWOULDBLOCK to avoid halting the decoder
        // when switching between audio/video and audio only.
        return -EWOULDBLOCK;
    }

    status_t finalResult;
    sp<AnotherPacketSource> discontinuityQueue  = mDiscontinuities.valueFor(stream);
    if (discontinuityQueue->hasBufferAvailable(&finalResult)) {
        discontinuityQueue->dequeueAccessUnit(accessUnit);
        // seeking, track switching
        sp<AMessage> extra;
        int64_t timeUs;
        if ((*accessUnit)->meta()->findMessage("extra", &extra)
                && extra != NULL
                && extra->findInt64("timeUs", &timeUs)) {
            // seeking only
            mLastSeekTimeUs = timeUs;
            mDiscontinuityOffsetTimesUs.clear();
            mDiscontinuityAbsStartTimesUs.clear();
        }
        return INFO_DISCONTINUITY;
    }

    sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(stream);

    ssize_t idx = typeToIndex(stream);
    if (!packetSource->hasBufferAvailable(&finalResult)) {
        if (finalResult == OK) {
            mBuffering[idx] = true;
            return -EAGAIN;
        } else {
            return finalResult;
        }
    }

    if (mBuffering[idx]) {
        if (mSwitchInProgress
                || packetSource->isFinished(0)
                || packetSource->getEstimatedDurationUs() > 10000000ll) {
            mBuffering[idx] = false;
        }
    }

    if (mBuffering[idx]) {
        return -EAGAIN;
    }

    // wait for counterpart
    sp<AnotherPacketSource> otherSource;
    uint32_t mask = mNewStreamMask & mStreamMask;
    uint32_t fetchersMask  = 0;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        uint32_t fetcherMask = mFetcherInfos.valueAt(i).mFetcher->getStreamTypeMask();
        fetchersMask |= fetcherMask;
    }
    mask &= fetchersMask;
    if (stream == STREAMTYPE_AUDIO && (mask & STREAMTYPE_VIDEO)) {
        otherSource = mPacketSources.valueFor(STREAMTYPE_VIDEO);
    } else if (stream == STREAMTYPE_VIDEO && (mask & STREAMTYPE_AUDIO)) {
        otherSource = mPacketSources.valueFor(STREAMTYPE_AUDIO);
    }
    if (otherSource != NULL && !otherSource->hasBufferAvailable(&finalResult)) {
        return finalResult == OK ? -EAGAIN : finalResult;
    }

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

        int32_t swap;
        if ((*accessUnit)->meta()->findInt32("swapPacketSource", &swap) && swap) {
            int32_t switchGeneration;
            CHECK((*accessUnit)->meta()->findInt32("switchGeneration", &switchGeneration));
            {
                Mutex::Autolock lock(mSwapMutex);
                if (switchGeneration == mSwitchGeneration) {
                    swapPacketSource(stream);
                    sp<AMessage> msg = new AMessage(kWhatSwapped, id());
                    msg->setInt32("stream", stream);
                    msg->setInt32("switchGeneration", switchGeneration);
                    msg->post();
                }
            }
        } else {
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
        }
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
    // No swapPacketSource race condition; called from the same thread as dequeueAccessUnit.
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

void LiveSession::connectAsync(
        const char *url, const KeyedVector<String8, String8> *headers) {
    sp<AMessage> msg = new AMessage(kWhatConnect, id());
    msg->setString("url", url);

    if (headers != NULL) {
        msg->setPointer(
                "headers",
                new KeyedVector<String8, String8>(*headers));
    }

    msg->post();
}

status_t LiveSession::disconnect() {
    sp<AMessage> msg = new AMessage(kWhatDisconnect, id());

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    return err;
}

status_t LiveSession::seekTo(int64_t timeUs) {
    sp<AMessage> msg = new AMessage(kWhatSeek, id());
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
            uint32_t seekReplyID;
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
                    if (what == PlaylistFetcher::kWhatStopped) {
                        AString uri;
                        CHECK(msg->findString("uri", &uri));
                        if (mFetcherInfos.removeItem(uri) < 0) {
                            // ignore duplicated kWhatStopped messages.
                            break;
                        }

                        if (mSwitchInProgress) {
                            tryToFinishBandwidthSwitch();
                        }
                    }

                    if (mContinuation != NULL) {
                        CHECK_GT(mContinuationCounter, 0);
                        if (--mContinuationCounter == 0) {
                            mContinuation->post();

                            if (mSeekReplyID != 0) {
                                CHECK(mSeekReply != NULL);
                                mSeekReply->setInt32("err", OK);
                                mSeekReply->postReply(mSeekReplyID);
                                mSeekReplyID = 0;
                                mSeekReply.clear();
                            }
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

                    FetcherInfo *info = &mFetcherInfos.editValueFor(uri);
                    info->mDurationUs = durationUs;
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

                case PlaylistFetcher::kWhatTemporarilyDoneFetching:
                {
                    AString uri;
                    CHECK(msg->findString("uri", &uri));

                    FetcherInfo *info = &mFetcherInfos.editValueFor(uri);
                    info->mIsPrepared = true;

                    if (mInPreparationPhase) {
                        bool allFetchersPrepared = true;
                        for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
                            if (!mFetcherInfos.valueAt(i).mIsPrepared) {
                                allFetchersPrepared = false;
                                break;
                            }
                        }

                        if (allFetchersPrepared) {
                            postPrepared(OK);
                        }
                    }
                    break;
                }

                case PlaylistFetcher::kWhatStartedAt:
                {
                    int32_t switchGeneration;
                    CHECK(msg->findInt32("switchGeneration", &switchGeneration));

                    if (switchGeneration != mSwitchGeneration) {
                        break;
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

        case kWhatCheckBandwidth:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mCheckBandwidthGeneration) {
                break;
            }

            onCheckBandwidth(msg);
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

        case kWhatSwapped:
        {
            onSwapped(msg);
            break;
        }

        case kWhatCheckSwitchDown:
        {
            onCheckSwitchDown();
            break;
        }

        case kWhatSwitchDown:
        {
            onSwitchDown();
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

            unsigned long bandwidth;
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
}

void LiveSession::finishDisconnect() {
    // No reconfiguration is currently pending, make sure none will trigger
    // during disconnection either.
    cancelCheckBandwidthEvent();

    // Protect mPacketSources from a swapPacketSource race condition through disconnect.
    // (finishDisconnect, onFinishDisconnect2)
    cancelBandwidthSwitch();

    // cancel switch down monitor
    mSwitchDownMonitor.clear();

    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        mFetcherInfos.valueAt(i).mFetcher->stopAsync();
    }

    sp<AMessage> msg = new AMessage(kWhatFinishDisconnect2, id());

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
    mDisconnectReplyID = 0;
}

sp<PlaylistFetcher> LiveSession::addFetcher(const char *uri) {
    ssize_t index = mFetcherInfos.indexOfKey(uri);

    if (index >= 0) {
        return NULL;
    }

    sp<AMessage> notify = new AMessage(kWhatFetcherNotify, id());
    notify->setString("uri", uri);
    notify->setInt32("switchGeneration", mSwitchGeneration);

    FetcherInfo info;
    info.mFetcher = new PlaylistFetcher(notify, this, uri);
    info.mDurationUs = -1ll;
    info.mIsPrepared = false;
    info.mToBeRemoved = false;
    looper()->registerHandler(info.mFetcher);

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
        String8 *actualUrl) {
    off64_t size;
    sp<DataSource> temp_source;
    if (source == NULL) {
        source = &temp_source;
    }

    if (*source == NULL) {
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
                            StringPrintf(
                                "bytes=%lld-%s",
                                range_offset,
                                range_length < 0
                                    ? "" : StringPrintf("%lld",
                                            range_offset + range_length - 1).c_str()).c_str()));
            }
            status_t err = mHTTPDataSource->connect(url, &headers);

            if (err != OK) {
                return err;
            }

            *source = mHTTPDataSource;
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
            bufferRemaining = 32768;

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

static double uniformRand() {
    return (double)rand() / RAND_MAX;
}

size_t LiveSession::getBandwidthIndex() {
    if (mBandwidthItems.size() == 0) {
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
        int32_t bandwidthBps;
        if (mHTTPDataSource != NULL
                && mHTTPDataSource->estimateBandwidth(&bandwidthBps)) {
            ALOGV("bandwidth estimated at %.2f kbps", bandwidthBps / 1024.0f);
        } else {
            ALOGV("no bandwidth estimate.");
            return 0;  // Pick the lowest bandwidth stream by default.
        }

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
            // consider only 80% of the available bandwidth, but if we are switching up,
            // be even more conservative (70%) to avoid overestimating and immediately
            // switching back.
            size_t adjustedBandwidthBps = bandwidthBps;
            if (index > mCurBandwidthIndex) {
                adjustedBandwidthBps = adjustedBandwidthBps * 7 / 10;
            } else {
                adjustedBandwidthBps = adjustedBandwidthBps * 8 / 10;
            }
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
    int64_t maxDurationUs = 0ll;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        int64_t fetcherDurationUs = mFetcherInfos.valueAt(i).mDurationUs;

        if (fetcherDurationUs >= 0ll && fetcherDurationUs > maxDurationUs) {
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
    status_t err = mPlaylist->selectTrack(index, select);
    if (err == OK) {
        sp<AMessage> msg = new AMessage(kWhatChangeConfiguration, id());
        msg->setInt32("bandwidthIndex", mCurBandwidthIndex);
        msg->setInt32("pickTrack", select);
        msg->post();
    }
    return err;
}

bool LiveSession::canSwitchUp() {
    // Allow upwards bandwidth switch when a stream has buffered at least 10 seconds.
    status_t err = OK;
    for (size_t i = 0; i < mPacketSources.size(); ++i) {
        sp<AnotherPacketSource> source = mPacketSources.valueAt(i);
        int64_t dur = source->getBufferedDurationUs(&err);
        if (err == OK && dur > 10000000) {
            return true;
        }
    }
    return false;
}

void LiveSession::changeConfiguration(
        int64_t timeUs, size_t bandwidthIndex, bool pickTrack) {
    // Protect mPacketSources from a swapPacketSource race condition through reconfiguration.
    // (changeConfiguration, onChangeConfiguration2, onChangeConfiguration3).
    cancelBandwidthSwitch();

    CHECK(!mReconfigurationInProgress);
    mReconfigurationInProgress = true;

    mCurBandwidthIndex = bandwidthIndex;

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

        // If we're seeking all current fetchers are discarded.
        if (timeUs < 0ll) {
            // delay fetcher removal if not picking tracks
            discardFetcher = pickTrack;

            for (size_t j = 0; j < kMaxStreams; ++j) {
                StreamType type = indexToType(j);
                if ((streamMask & type) && uri == URIs[j]) {
                    resumeMask |= type;
                    streamMask &= ~type;
                    discardFetcher = false;
                }
            }
        }

        if (discardFetcher) {
            mFetcherInfos.valueAt(i).mFetcher->stopAsync();
        } else {
            mFetcherInfos.valueAt(i).mFetcher->pauseAsync();
        }
    }

    sp<AMessage> msg;
    if (timeUs < 0ll) {
        // skip onChangeConfiguration2 (decoder destruction) if not seeking.
        msg = new AMessage(kWhatChangeConfiguration3, id());
    } else {
        msg = new AMessage(kWhatChangeConfiguration2, id());
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

        if (mSeekReplyID != 0) {
            CHECK(mSeekReply != NULL);
            mSeekReply->setInt32("err", OK);
            mSeekReply->postReply(mSeekReplyID);
            mSeekReplyID = 0;
            mSeekReply.clear();
        }
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

    uint32_t streamMask, resumeMask;
    CHECK(msg->findInt32("streamMask", (int32_t *)&streamMask));
    CHECK(msg->findInt32("resumeMask", (int32_t *)&resumeMask));

    // currently onChangeConfiguration2 is only called for seeking;
    // remove the following CHECK if using it else where.
    CHECK_EQ(resumeMask, 0);
    streamMask |= resumeMask;

    AString URIs[kMaxStreams];
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (streamMask & indexToType(i)) {
            const AString &uriKey = mStreams[i].uriKey();
            CHECK(msg->findString(uriKey.c_str(), &URIs[i]));
            ALOGV("%s = '%s'", uriKey.c_str(), URIs[i].c_str());
        }
    }

    // Determine which decoders to shutdown on the player side,
    // a decoder has to be shutdown if either
    // 1) its streamtype was active before but now longer isn't.
    // or
    // 2) its streamtype was already active and still is but the URI
    //    has changed.
    uint32_t changedMask = 0;
    for (size_t i = 0; i < kMaxStreams && i != kSubtitleIndex; ++i) {
        if (((mStreamMask & streamMask & indexToType(i))
                && !(URIs[i] == mStreams[i].mUri))
                || (mStreamMask & ~streamMask & indexToType(i))) {
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
    msg->setTarget(id());

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

                if (j != kSubtitleIndex) {
                    ALOGV("queueing dummy discontinuity for stream type %d", indexToType(j));
                    sp<AnotherPacketSource> discontinuityQueue;
                    discontinuityQueue = mDiscontinuities.valueFor(indexToType(j));
                    discontinuityQueue->queueDiscontinuity(
                            ATSParser::DISCONTINUITY_NONE,
                            NULL,
                            true);
                }
            }
        }

        FetcherInfo &info = mFetcherInfos.editValueAt(i);
        if (sources[kAudioIndex] != NULL || sources[kVideoIndex] != NULL
                || sources[kSubtitleIndex] != NULL) {
            info.mFetcher->startAsync(
                    sources[kAudioIndex], sources[kVideoIndex], sources[kSubtitleIndex]);
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

        int32_t latestSeq = -1;
        int64_t startTimeUs = -1;
        int64_t segmentStartTimeUs = -1ll;
        int32_t discontinuitySeq = -1;
        sp<AnotherPacketSource> sources[kMaxStreams];

        // TRICKY: looping from i as earlier streams are already removed from streamMask
        for (size_t j = i; j < kMaxStreams; ++j) {
            const AString &streamUri = switching ? mStreams[j].mNewUri : mStreams[j].mUri;
            if ((streamMask & indexToType(j)) && uri == streamUri) {
                sources[j] = mPacketSources.valueFor(indexToType(j));

                if (timeUs >= 0) {
                    sources[j]->clear();
                    startTimeUs = timeUs;

                    sp<AnotherPacketSource> discontinuityQueue;
                    sp<AMessage> extra = new AMessage;
                    extra->setInt64("timeUs", timeUs);
                    discontinuityQueue = mDiscontinuities.valueFor(indexToType(j));
                    discontinuityQueue->queueDiscontinuity(
                            ATSParser::DISCONTINUITY_SEEK, extra, true);
                } else {
                    int32_t type;
                    int64_t srcSegmentStartTimeUs;
                    sp<AMessage> meta;
                    if (pickTrack) {
                        // selecting
                        meta = sources[j]->getLatestDequeuedMeta();
                    } else {
                        // adapting
                        meta = sources[j]->getLatestEnqueuedMeta();
                    }

                    if (meta != NULL && !meta->findInt32("discontinuity", &type)) {
                        int64_t tmpUs;
                        CHECK(meta->findInt64("timeUs", &tmpUs));
                        if (startTimeUs < 0 || tmpUs < startTimeUs) {
                            startTimeUs = tmpUs;
                        }

                        CHECK(meta->findInt64("segmentStartTimeUs", &tmpUs));
                        if (segmentStartTimeUs < 0 || tmpUs < segmentStartTimeUs) {
                            segmentStartTimeUs = tmpUs;
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
                        sp<AnotherPacketSource> discontinuityQueue;
                        discontinuityQueue = mDiscontinuities.valueFor(indexToType(j));
                        discontinuityQueue->queueDiscontinuity(
                                ATSParser::DISCONTINUITY_FORMATCHANGE, NULL, true);
                    } else {
                        // adapting, queue discontinuities after resume
                        sources[j] = mPacketSources2.valueFor(indexToType(j));
                        sources[j]->clear();
                        uint32_t extraStreams = mNewStreamMask & (~mStreamMask);
                        if (extraStreams & indexToType(j)) {
                            sources[j]->queueAccessUnit(createFormatChangeBuffer(/*swap*/ false));
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
                switching);
    }

    // All fetchers have now been started, the configuration change
    // has completed.

    cancelCheckBandwidthEvent();
    scheduleCheckBandwidthEvent();

    ALOGV("XXX configuration change completed.");
    mReconfigurationInProgress = false;
    if (switching) {
        mSwitchInProgress = true;
    } else {
        mStreamMask = mNewStreamMask;
    }

    if (mDisconnectReplyID != 0) {
        finishDisconnect();
    }
}

void LiveSession::onSwapped(const sp<AMessage> &msg) {
    int32_t switchGeneration;
    CHECK(msg->findInt32("switchGeneration", &switchGeneration));
    if (switchGeneration != mSwitchGeneration) {
        return;
    }

    int32_t stream;
    CHECK(msg->findInt32("stream", &stream));

    ssize_t idx = typeToIndex(stream);
    CHECK(idx >= 0);
    if ((mNewStreamMask & stream) && mStreams[idx].mNewUri.empty()) {
        ALOGW("swapping stream type %d %s to empty stream", stream, mStreams[idx].mUri.c_str());
    }
    mStreams[idx].mUri = mStreams[idx].mNewUri;
    mStreams[idx].mNewUri.clear();

    mSwapMask &= ~stream;
    if (mSwapMask != 0) {
        return;
    }

    // Check if new variant contains extra streams.
    uint32_t extraStreams = mNewStreamMask & (~mStreamMask);
    while (extraStreams) {
        StreamType extraStream = (StreamType) (extraStreams & ~(extraStreams - 1));
        swapPacketSource(extraStream);
        extraStreams &= ~extraStream;

        idx = typeToIndex(extraStream);
        CHECK(idx >= 0);
        if (mStreams[idx].mNewUri.empty()) {
            ALOGW("swapping extra stream type %d %s to empty stream",
                    extraStream, mStreams[idx].mUri.c_str());
        }
        mStreams[idx].mUri = mStreams[idx].mNewUri;
        mStreams[idx].mNewUri.clear();
    }

    tryToFinishBandwidthSwitch();
}

void LiveSession::onCheckSwitchDown() {
    if (mSwitchDownMonitor == NULL) {
        return;
    }

    for (size_t i = 0; i < kMaxStreams; ++i) {
        int32_t targetDuration;
        sp<AnotherPacketSource> packetSource = mPacketSources.valueFor(indexToType(i));
        sp<AMessage> meta = packetSource->getLatestDequeuedMeta();

        if (meta != NULL && meta->findInt32("targetDuration", &targetDuration) ) {
            int64_t bufferedDurationUs = packetSource->getEstimatedDurationUs();
            int64_t targetDurationUs = targetDuration * 1000000ll;

            if (bufferedDurationUs < targetDurationUs / 3) {
                (new AMessage(kWhatSwitchDown, id()))->post();
                break;
            }
        }
    }

    mSwitchDownMonitor->post(1000000ll);
}

void LiveSession::onSwitchDown() {
    if (mReconfigurationInProgress || mSwitchInProgress || mCurBandwidthIndex == 0) {
        return;
    }

    ssize_t bandwidthIndex = getBandwidthIndex();
    if (bandwidthIndex < mCurBandwidthIndex) {
        changeConfiguration(-1, bandwidthIndex, false);
        return;
    }

    changeConfiguration(-1, mCurBandwidthIndex - 1, false);
}

// Mark switch done when:
//   1. all old buffers are swapped out
void LiveSession::tryToFinishBandwidthSwitch() {
    if (!mSwitchInProgress) {
        return;
    }

    bool needToRemoveFetchers = false;
    for (size_t i = 0; i < mFetcherInfos.size(); ++i) {
        if (mFetcherInfos.valueAt(i).mToBeRemoved) {
            needToRemoveFetchers = true;
            break;
        }
    }

    if (!needToRemoveFetchers && mSwapMask == 0) {
        ALOGI("mSwitchInProgress = false");
        mStreamMask = mNewStreamMask;
        mSwitchInProgress = false;
    }
}

void LiveSession::scheduleCheckBandwidthEvent() {
    sp<AMessage> msg = new AMessage(kWhatCheckBandwidth, id());
    msg->setInt32("generation", mCheckBandwidthGeneration);
    msg->post(10000000ll);
}

void LiveSession::cancelCheckBandwidthEvent() {
    ++mCheckBandwidthGeneration;
}

void LiveSession::cancelBandwidthSwitch() {
    Mutex::Autolock lock(mSwapMutex);
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

bool LiveSession::canSwitchBandwidthTo(size_t bandwidthIndex) {
    if (mReconfigurationInProgress || mSwitchInProgress) {
        return false;
    }

    if (mCurBandwidthIndex < 0) {
        return true;
    }

    if (bandwidthIndex == (size_t)mCurBandwidthIndex) {
        return false;
    } else if (bandwidthIndex > (size_t)mCurBandwidthIndex) {
        return canSwitchUp();
    } else {
        return true;
    }
}

void LiveSession::onCheckBandwidth(const sp<AMessage> &msg) {
    size_t bandwidthIndex = getBandwidthIndex();
    if (canSwitchBandwidthTo(bandwidthIndex)) {
        changeConfiguration(-1ll /* timeUs */, bandwidthIndex);
    } else {
        // Come back and check again 10 seconds later in case there is nothing to do now.
        // If we DO change configuration, once that completes it'll schedule a new
        // check bandwidth event with an incremented mCheckBandwidthGeneration.
        msg->post(10000000ll);
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

    mSwitchDownMonitor = new AMessage(kWhatCheckSwitchDown, id());
    mSwitchDownMonitor->post();
}

}  // namespace android

