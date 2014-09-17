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

//#define LOG_NDEBUG 0
#define LOG_TAG "PlaylistFetcher"
#include <utils/Log.h>

#include "PlaylistFetcher.h"

#include "LiveDataSource.h"
#include "LiveSession.h"
#include "M3UParser.h"

#include "include/avc_utils.h"
#include "include/HTTPBase.h"
#include "include/ID3.h"
#include "mpeg2ts/AnotherPacketSource.h"

#include <media/IStreamSource.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include <ctype.h>
#include <inttypes.h>
#include <openssl/aes.h>
#include <openssl/md5.h>

namespace android {

// static
const int64_t PlaylistFetcher::kMinBufferedDurationUs = 10000000ll;
const int64_t PlaylistFetcher::kMaxMonitorDelayUs = 3000000ll;
const int32_t PlaylistFetcher::kDownloadBlockSize = 2048;
const int32_t PlaylistFetcher::kNumSkipFrames = 10;

PlaylistFetcher::PlaylistFetcher(
        const sp<AMessage> &notify,
        const sp<LiveSession> &session,
        const char *uri)
    : mNotify(notify),
      mStartTimeUsNotify(notify->dup()),
      mSession(session),
      mURI(uri),
      mStreamTypeMask(0),
      mStartTimeUs(-1ll),
      mSegmentStartTimeUs(-1ll),
      mDiscontinuitySeq(-1ll),
      mStartTimeUsRelative(false),
      mLastPlaylistFetchTimeUs(-1ll),
      mSeqNumber(-1),
      mNumRetries(0),
      mStartup(true),
      mAdaptive(false),
      mPrepared(false),
      mNextPTSTimeUs(-1ll),
      mMonitorQueueGeneration(0),
      mRefreshState(INITIAL_MINIMUM_RELOAD_DELAY),
      mFirstPTSValid(false),
      mAbsoluteTimeAnchorUs(0ll),
      mVideoBuffer(new AnotherPacketSource(NULL)) {
    memset(mPlaylistHash, 0, sizeof(mPlaylistHash));
    mStartTimeUsNotify->setInt32("what", kWhatStartedAt);
    mStartTimeUsNotify->setInt32("streamMask", 0);
}

PlaylistFetcher::~PlaylistFetcher() {
}

int64_t PlaylistFetcher::getSegmentStartTimeUs(int32_t seqNumber) const {
    CHECK(mPlaylist != NULL);

    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }

    int32_t lastSeqNumberInPlaylist =
        firstSeqNumberInPlaylist + (int32_t)mPlaylist->size() - 1;

    CHECK_GE(seqNumber, firstSeqNumberInPlaylist);
    CHECK_LE(seqNumber, lastSeqNumberInPlaylist);

    int64_t segmentStartUs = 0ll;
    for (int32_t index = 0;
            index < seqNumber - firstSeqNumberInPlaylist; ++index) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(
                    index, NULL /* uri */, &itemMeta));

        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));

        segmentStartUs += itemDurationUs;
    }

    return segmentStartUs;
}

int64_t PlaylistFetcher::delayUsToRefreshPlaylist() const {
    int64_t nowUs = ALooper::GetNowUs();

    if (mPlaylist == NULL || mLastPlaylistFetchTimeUs < 0ll) {
        CHECK_EQ((int)mRefreshState, (int)INITIAL_MINIMUM_RELOAD_DELAY);
        return 0ll;
    }

    if (mPlaylist->isComplete()) {
        return (~0llu >> 1);
    }

    int32_t targetDurationSecs;
    CHECK(mPlaylist->meta()->findInt32("target-duration", &targetDurationSecs));

    int64_t targetDurationUs = targetDurationSecs * 1000000ll;

    int64_t minPlaylistAgeUs;

    switch (mRefreshState) {
        case INITIAL_MINIMUM_RELOAD_DELAY:
        {
            size_t n = mPlaylist->size();
            if (n > 0) {
                sp<AMessage> itemMeta;
                CHECK(mPlaylist->itemAt(n - 1, NULL /* uri */, &itemMeta));

                int64_t itemDurationUs;
                CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));

                minPlaylistAgeUs = itemDurationUs;
                break;
            }

            // fall through
        }

        case FIRST_UNCHANGED_RELOAD_ATTEMPT:
        {
            minPlaylistAgeUs = targetDurationUs / 2;
            break;
        }

        case SECOND_UNCHANGED_RELOAD_ATTEMPT:
        {
            minPlaylistAgeUs = (targetDurationUs * 3) / 2;
            break;
        }

        case THIRD_UNCHANGED_RELOAD_ATTEMPT:
        {
            minPlaylistAgeUs = targetDurationUs * 3;
            break;
        }

        default:
            TRESPASS();
            break;
    }

    int64_t delayUs = mLastPlaylistFetchTimeUs + minPlaylistAgeUs - nowUs;
    return delayUs > 0ll ? delayUs : 0ll;
}

status_t PlaylistFetcher::decryptBuffer(
        size_t playlistIndex, const sp<ABuffer> &buffer,
        bool first) {
    sp<AMessage> itemMeta;
    bool found = false;
    AString method;

    for (ssize_t i = playlistIndex; i >= 0; --i) {
        AString uri;
        CHECK(mPlaylist->itemAt(i, &uri, &itemMeta));

        if (itemMeta->findString("cipher-method", &method)) {
            found = true;
            break;
        }
    }

    if (!found) {
        method = "NONE";
    }
    buffer->meta()->setString("cipher-method", method.c_str());

    if (method == "NONE") {
        return OK;
    } else if (!(method == "AES-128")) {
        ALOGE("Unsupported cipher method '%s'", method.c_str());
        return ERROR_UNSUPPORTED;
    }

    AString keyURI;
    if (!itemMeta->findString("cipher-uri", &keyURI)) {
        ALOGE("Missing key uri");
        return ERROR_MALFORMED;
    }

    ssize_t index = mAESKeyForURI.indexOfKey(keyURI);

    sp<ABuffer> key;
    if (index >= 0) {
        key = mAESKeyForURI.valueAt(index);
    } else {
        ssize_t err = mSession->fetchFile(keyURI.c_str(), &key);

        if (err < 0) {
            ALOGE("failed to fetch cipher key from '%s'.", keyURI.c_str());
            return ERROR_IO;
        } else if (key->size() != 16) {
            ALOGE("key file '%s' wasn't 16 bytes in size.", keyURI.c_str());
            return ERROR_MALFORMED;
        }

        mAESKeyForURI.add(keyURI, key);
    }

    AES_KEY aes_key;
    if (AES_set_decrypt_key(key->data(), 128, &aes_key) != 0) {
        ALOGE("failed to set AES decryption key.");
        return UNKNOWN_ERROR;
    }

    size_t n = buffer->size();
    if (!n) {
        return OK;
    }
    CHECK(n % 16 == 0);

    if (first) {
        // If decrypting the first block in a file, read the iv from the manifest
        // or derive the iv from the file's sequence number.

        AString iv;
        if (itemMeta->findString("cipher-iv", &iv)) {
            if ((!iv.startsWith("0x") && !iv.startsWith("0X"))
                    || iv.size() != 16 * 2 + 2) {
                ALOGE("malformed cipher IV '%s'.", iv.c_str());
                return ERROR_MALFORMED;
            }

            memset(mAESInitVec, 0, sizeof(mAESInitVec));
            for (size_t i = 0; i < 16; ++i) {
                char c1 = tolower(iv.c_str()[2 + 2 * i]);
                char c2 = tolower(iv.c_str()[3 + 2 * i]);
                if (!isxdigit(c1) || !isxdigit(c2)) {
                    ALOGE("malformed cipher IV '%s'.", iv.c_str());
                    return ERROR_MALFORMED;
                }
                uint8_t nibble1 = isdigit(c1) ? c1 - '0' : c1 - 'a' + 10;
                uint8_t nibble2 = isdigit(c2) ? c2 - '0' : c2 - 'a' + 10;

                mAESInitVec[i] = nibble1 << 4 | nibble2;
            }
        } else {
            memset(mAESInitVec, 0, sizeof(mAESInitVec));
            mAESInitVec[15] = mSeqNumber & 0xff;
            mAESInitVec[14] = (mSeqNumber >> 8) & 0xff;
            mAESInitVec[13] = (mSeqNumber >> 16) & 0xff;
            mAESInitVec[12] = (mSeqNumber >> 24) & 0xff;
        }
    }

    AES_cbc_encrypt(
            buffer->data(), buffer->data(), buffer->size(),
            &aes_key, mAESInitVec, AES_DECRYPT);

    return OK;
}

status_t PlaylistFetcher::checkDecryptPadding(const sp<ABuffer> &buffer) {
    status_t err;
    AString method;
    CHECK(buffer->meta()->findString("cipher-method", &method));
    if (method == "NONE") {
        return OK;
    }

    uint8_t padding = 0;
    if (buffer->size() > 0) {
        padding = buffer->data()[buffer->size() - 1];
    }

    if (padding > 16) {
        return ERROR_MALFORMED;
    }

    for (size_t i = buffer->size() - padding; i < padding; i++) {
        if (buffer->data()[i] != padding) {
            return ERROR_MALFORMED;
        }
    }

    buffer->setRange(buffer->offset(), buffer->size() - padding);
    return OK;
}

void PlaylistFetcher::postMonitorQueue(int64_t delayUs, int64_t minDelayUs) {
    int64_t maxDelayUs = delayUsToRefreshPlaylist();
    if (maxDelayUs < minDelayUs) {
        maxDelayUs = minDelayUs;
    }
    if (delayUs > maxDelayUs) {
        ALOGV("Need to refresh playlist in %" PRId64 , maxDelayUs);
        delayUs = maxDelayUs;
    }
    sp<AMessage> msg = new AMessage(kWhatMonitorQueue, id());
    msg->setInt32("generation", mMonitorQueueGeneration);
    msg->post(delayUs);
}

void PlaylistFetcher::cancelMonitorQueue() {
    ++mMonitorQueueGeneration;
}

void PlaylistFetcher::startAsync(
        const sp<AnotherPacketSource> &audioSource,
        const sp<AnotherPacketSource> &videoSource,
        const sp<AnotherPacketSource> &subtitleSource,
        int64_t startTimeUs,
        int64_t segmentStartTimeUs,
        int32_t startDiscontinuitySeq,
        bool adaptive) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());

    uint32_t streamTypeMask = 0ul;

    if (audioSource != NULL) {
        msg->setPointer("audioSource", audioSource.get());
        streamTypeMask |= LiveSession::STREAMTYPE_AUDIO;
    }

    if (videoSource != NULL) {
        msg->setPointer("videoSource", videoSource.get());
        streamTypeMask |= LiveSession::STREAMTYPE_VIDEO;
    }

    if (subtitleSource != NULL) {
        msg->setPointer("subtitleSource", subtitleSource.get());
        streamTypeMask |= LiveSession::STREAMTYPE_SUBTITLES;
    }

    msg->setInt32("streamTypeMask", streamTypeMask);
    msg->setInt64("startTimeUs", startTimeUs);
    msg->setInt64("segmentStartTimeUs", segmentStartTimeUs);
    msg->setInt32("startDiscontinuitySeq", startDiscontinuitySeq);
    msg->setInt32("adaptive", adaptive);
    msg->post();
}

void PlaylistFetcher::pauseAsync() {
    (new AMessage(kWhatPause, id()))->post();
}

void PlaylistFetcher::stopAsync(bool clear) {
    sp<AMessage> msg = new AMessage(kWhatStop, id());
    msg->setInt32("clear", clear);
    msg->post();
}

void PlaylistFetcher::resumeUntilAsync(const sp<AMessage> &params) {
    AMessage* msg = new AMessage(kWhatResumeUntil, id());
    msg->setMessage("params", params);
    msg->post();
}

void PlaylistFetcher::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStart:
        {
            status_t err = onStart(msg);

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStarted);
            notify->setInt32("err", err);
            notify->post();
            break;
        }

        case kWhatPause:
        {
            onPause();

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatPaused);
            notify->post();
            break;
        }

        case kWhatStop:
        {
            onStop(msg);

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStopped);
            notify->post();
            break;
        }

        case kWhatMonitorQueue:
        case kWhatDownloadNext:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mMonitorQueueGeneration) {
                // Stale event
                break;
            }

            if (msg->what() == kWhatMonitorQueue) {
                onMonitorQueue();
            } else {
                onDownloadNext();
            }
            break;
        }

        case kWhatResumeUntil:
        {
            onResumeUntil(msg);
            break;
        }

        default:
            TRESPASS();
    }
}

status_t PlaylistFetcher::onStart(const sp<AMessage> &msg) {
    mPacketSources.clear();

    uint32_t streamTypeMask;
    CHECK(msg->findInt32("streamTypeMask", (int32_t *)&streamTypeMask));

    int64_t startTimeUs;
    int64_t segmentStartTimeUs;
    int32_t startDiscontinuitySeq;
    int32_t adaptive;
    CHECK(msg->findInt64("startTimeUs", &startTimeUs));
    CHECK(msg->findInt64("segmentStartTimeUs", &segmentStartTimeUs));
    CHECK(msg->findInt32("startDiscontinuitySeq", &startDiscontinuitySeq));
    CHECK(msg->findInt32("adaptive", &adaptive));

    if (streamTypeMask & LiveSession::STREAMTYPE_AUDIO) {
        void *ptr;
        CHECK(msg->findPointer("audioSource", &ptr));

        mPacketSources.add(
                LiveSession::STREAMTYPE_AUDIO,
                static_cast<AnotherPacketSource *>(ptr));
    }

    if (streamTypeMask & LiveSession::STREAMTYPE_VIDEO) {
        void *ptr;
        CHECK(msg->findPointer("videoSource", &ptr));

        mPacketSources.add(
                LiveSession::STREAMTYPE_VIDEO,
                static_cast<AnotherPacketSource *>(ptr));
    }

    if (streamTypeMask & LiveSession::STREAMTYPE_SUBTITLES) {
        void *ptr;
        CHECK(msg->findPointer("subtitleSource", &ptr));

        mPacketSources.add(
                LiveSession::STREAMTYPE_SUBTITLES,
                static_cast<AnotherPacketSource *>(ptr));
    }

    mStreamTypeMask = streamTypeMask;

    mSegmentStartTimeUs = segmentStartTimeUs;
    mDiscontinuitySeq = startDiscontinuitySeq;

    if (startTimeUs >= 0) {
        mStartTimeUs = startTimeUs;
        mSeqNumber = -1;
        mStartup = true;
        mPrepared = false;
        mAdaptive = adaptive;
    }

    postMonitorQueue();

    return OK;
}

void PlaylistFetcher::onPause() {
    cancelMonitorQueue();
}

void PlaylistFetcher::onStop(const sp<AMessage> &msg) {
    cancelMonitorQueue();

    int32_t clear;
    CHECK(msg->findInt32("clear", &clear));
    if (clear) {
        for (size_t i = 0; i < mPacketSources.size(); i++) {
            sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);
            packetSource->clear();
        }
    }

    mPacketSources.clear();
    mStreamTypeMask = 0;
}

// Resume until we have reached the boundary timestamps listed in `msg`; when
// the remaining time is too short (within a resume threshold) stop immediately
// instead.
status_t PlaylistFetcher::onResumeUntil(const sp<AMessage> &msg) {
    sp<AMessage> params;
    CHECK(msg->findMessage("params", &params));

    bool stop = false;
    for (size_t i = 0; i < mPacketSources.size(); i++) {
        sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);

        const char *stopKey;
        int streamType = mPacketSources.keyAt(i);
        switch (streamType) {
        case LiveSession::STREAMTYPE_VIDEO:
            stopKey = "timeUsVideo";
            break;

        case LiveSession::STREAMTYPE_AUDIO:
            stopKey = "timeUsAudio";
            break;

        case LiveSession::STREAMTYPE_SUBTITLES:
            stopKey = "timeUsSubtitle";
            break;

        default:
            TRESPASS();
        }

        // Don't resume if we would stop within a resume threshold.
        int32_t discontinuitySeq;
        int64_t latestTimeUs = 0, stopTimeUs = 0;
        sp<AMessage> latestMeta = packetSource->getLatestDequeuedMeta();
        if (latestMeta != NULL
                && latestMeta->findInt32("discontinuitySeq", &discontinuitySeq)
                && discontinuitySeq == mDiscontinuitySeq
                && latestMeta->findInt64("timeUs", &latestTimeUs)
                && params->findInt64(stopKey, &stopTimeUs)
                && stopTimeUs - latestTimeUs < resumeThreshold(latestMeta)) {
            stop = true;
        }
    }

    if (stop) {
        for (size_t i = 0; i < mPacketSources.size(); i++) {
            mPacketSources.valueAt(i)->queueAccessUnit(mSession->createFormatChangeBuffer());
        }
        stopAsync(/* clear = */ false);
        return OK;
    }

    mStopParams = params;
    postMonitorQueue();

    return OK;
}

void PlaylistFetcher::notifyError(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

void PlaylistFetcher::queueDiscontinuity(
        ATSParser::DiscontinuityType type, const sp<AMessage> &extra) {
    for (size_t i = 0; i < mPacketSources.size(); ++i) {
        // do not discard buffer upon #EXT-X-DISCONTINUITY tag
        // (seek will discard buffer by abandoning old fetchers)
        mPacketSources.valueAt(i)->queueDiscontinuity(
                type, extra, false /* discard */);
    }
}

void PlaylistFetcher::onMonitorQueue() {
    bool downloadMore = false;
    refreshPlaylist();

    int32_t targetDurationSecs;
    int64_t targetDurationUs = kMinBufferedDurationUs;
    if (mPlaylist != NULL) {
        CHECK(mPlaylist->meta()->findInt32("target-duration", &targetDurationSecs));
        targetDurationUs = targetDurationSecs * 1000000ll;
    }

    // buffer at least 3 times the target duration, or up to 10 seconds
    int64_t durationToBufferUs = targetDurationUs * 3;
    if (durationToBufferUs > kMinBufferedDurationUs)  {
        durationToBufferUs = kMinBufferedDurationUs;
    }

    int64_t bufferedDurationUs = 0ll;
    status_t finalResult = NOT_ENOUGH_DATA;
    if (mStreamTypeMask == LiveSession::STREAMTYPE_SUBTITLES) {
        sp<AnotherPacketSource> packetSource =
            mPacketSources.valueFor(LiveSession::STREAMTYPE_SUBTITLES);

        bufferedDurationUs =
                packetSource->getBufferedDurationUs(&finalResult);
        finalResult = OK;
    } else {
        // Use max stream duration to prevent us from waiting on a non-existent stream;
        // when we cannot make out from the manifest what streams are included in a playlist
        // we might assume extra streams.
        for (size_t i = 0; i < mPacketSources.size(); ++i) {
            if ((mStreamTypeMask & mPacketSources.keyAt(i)) == 0) {
                continue;
            }

            int64_t bufferedStreamDurationUs =
                mPacketSources.valueAt(i)->getBufferedDurationUs(&finalResult);
            ALOGV("buffered %" PRId64 " for stream %d",
                    bufferedStreamDurationUs, mPacketSources.keyAt(i));
            if (bufferedStreamDurationUs > bufferedDurationUs) {
                bufferedDurationUs = bufferedStreamDurationUs;
            }
        }
    }
    downloadMore = (bufferedDurationUs < durationToBufferUs);

    // signal start if buffered up at least the target size
    if (!mPrepared && bufferedDurationUs > targetDurationUs && downloadMore) {
        mPrepared = true;

        ALOGV("prepared, buffered=%" PRId64 " > %" PRId64 "",
                bufferedDurationUs, targetDurationUs);
        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatTemporarilyDoneFetching);
        msg->post();
    }

    if (finalResult == OK && downloadMore) {
        ALOGV("monitoring, buffered=%" PRId64 " < %" PRId64 "",
                bufferedDurationUs, durationToBufferUs);
        // delay the next download slightly; hopefully this gives other concurrent fetchers
        // a better chance to run.
        // onDownloadNext();
        sp<AMessage> msg = new AMessage(kWhatDownloadNext, id());
        msg->setInt32("generation", mMonitorQueueGeneration);
        msg->post(1000l);
    } else {
        // Nothing to do yet, try again in a second.

        sp<AMessage> msg = mNotify->dup();
        msg->setInt32("what", kWhatTemporarilyDoneFetching);
        msg->post();

        int64_t delayUs = mPrepared ? kMaxMonitorDelayUs : targetDurationUs / 2;
        ALOGV("pausing for %" PRId64 ", buffered=%" PRId64 " > %" PRId64 "",
                delayUs, bufferedDurationUs, durationToBufferUs);
        // :TRICKY: need to enforce minimum delay because the delay to
        // refresh the playlist will become 0
        postMonitorQueue(delayUs, mPrepared ? targetDurationUs * 2 : 0);
    }
}

status_t PlaylistFetcher::refreshPlaylist() {
    if (delayUsToRefreshPlaylist() <= 0) {
        bool unchanged;
        sp<M3UParser> playlist = mSession->fetchPlaylist(
                mURI.c_str(), mPlaylistHash, &unchanged);

        if (playlist == NULL) {
            if (unchanged) {
                // We succeeded in fetching the playlist, but it was
                // unchanged from the last time we tried.

                if (mRefreshState != THIRD_UNCHANGED_RELOAD_ATTEMPT) {
                    mRefreshState = (RefreshState)(mRefreshState + 1);
                }
            } else {
                ALOGE("failed to load playlist at url '%s'", mURI.c_str());
                notifyError(ERROR_IO);
                return ERROR_IO;
            }
        } else {
            mRefreshState = INITIAL_MINIMUM_RELOAD_DELAY;
            mPlaylist = playlist;

            if (mPlaylist->isComplete() || mPlaylist->isEvent()) {
                updateDuration();
            }
        }

        mLastPlaylistFetchTimeUs = ALooper::GetNowUs();
    }
    return OK;
}

// static
bool PlaylistFetcher::bufferStartsWithTsSyncByte(const sp<ABuffer>& buffer) {
    return buffer->size() > 0 && buffer->data()[0] == 0x47;
}

void PlaylistFetcher::onDownloadNext() {
    if (refreshPlaylist() != OK) {
        return;
    }

    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }

    bool discontinuity = false;

    const int32_t lastSeqNumberInPlaylist =
        firstSeqNumberInPlaylist + (int32_t)mPlaylist->size() - 1;

    if (mDiscontinuitySeq < 0) {
        mDiscontinuitySeq = mPlaylist->getDiscontinuitySeq();
    }

    if (mSeqNumber < 0) {
        CHECK_GE(mStartTimeUs, 0ll);

        if (mSegmentStartTimeUs < 0) {
            if (!mPlaylist->isComplete() && !mPlaylist->isEvent()) {
                // If this is a live session, start 3 segments from the end on connect
                mSeqNumber = lastSeqNumberInPlaylist - 3;
                if (mSeqNumber < firstSeqNumberInPlaylist) {
                    mSeqNumber = firstSeqNumberInPlaylist;
                }
            } else {
                mSeqNumber = getSeqNumberForTime(mStartTimeUs);
                mStartTimeUs -= getSegmentStartTimeUs(mSeqNumber);
            }
            mStartTimeUsRelative = true;
            ALOGV("Initial sequence number for time %" PRId64 " is %d from (%d .. %d)",
                    mStartTimeUs, mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
        } else {
            mSeqNumber = getSeqNumberForTime(mSegmentStartTimeUs);
            if (mAdaptive) {
                // avoid double fetch/decode
                mSeqNumber += 1;
            }
            ssize_t minSeq = getSeqNumberForDiscontinuity(mDiscontinuitySeq);
            if (mSeqNumber < minSeq) {
                mSeqNumber = minSeq;
            }

            if (mSeqNumber < firstSeqNumberInPlaylist) {
                mSeqNumber = firstSeqNumberInPlaylist;
            }

            if (mSeqNumber > lastSeqNumberInPlaylist) {
                mSeqNumber = lastSeqNumberInPlaylist;
            }
            ALOGV("Initial sequence number for live event %d from (%d .. %d)",
                    mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
        }
    }

    if (mSeqNumber < firstSeqNumberInPlaylist
            || mSeqNumber > lastSeqNumberInPlaylist) {
        if (!mPlaylist->isComplete() && mNumRetries < kMaxNumRetries) {
            ++mNumRetries;

            if (mSeqNumber > lastSeqNumberInPlaylist) {
                // refresh in increasing fraction (1/2, 1/3, ...) of the
                // playlist's target duration or 3 seconds, whichever is less
                int32_t targetDurationSecs;
                CHECK(mPlaylist->meta()->findInt32(
                        "target-duration", &targetDurationSecs));
                int64_t delayUs = mPlaylist->size() * targetDurationSecs *
                        1000000ll / (1 + mNumRetries);
                if (delayUs > kMaxMonitorDelayUs) {
                    delayUs = kMaxMonitorDelayUs;
                }
                ALOGV("sequence number high: %d from (%d .. %d), "
                      "monitor in %" PRId64 " (retry=%d)",
                        mSeqNumber, firstSeqNumberInPlaylist,
                        lastSeqNumberInPlaylist, delayUs, mNumRetries);
                postMonitorQueue(delayUs);
                return;
            }

            // we've missed the boat, let's start from the lowest sequence
            // number available and signal a discontinuity.

            ALOGI("We've missed the boat, restarting playback."
                  "  mStartup=%d, was  looking for %d in %d-%d",
                    mStartup, mSeqNumber, firstSeqNumberInPlaylist,
                    lastSeqNumberInPlaylist);
            mSeqNumber = lastSeqNumberInPlaylist - 3;
            if (mSeqNumber < firstSeqNumberInPlaylist) {
                mSeqNumber = firstSeqNumberInPlaylist;
            }
            discontinuity = true;

            // fall through
        } else {
            ALOGE("Cannot find sequence number %d in playlist "
                 "(contains %d - %d)",
                 mSeqNumber, firstSeqNumberInPlaylist,
                  firstSeqNumberInPlaylist + (int32_t)mPlaylist->size() - 1);

            notifyError(ERROR_END_OF_STREAM);
            return;
        }
    }

    mNumRetries = 0;

    AString uri;
    sp<AMessage> itemMeta;
    CHECK(mPlaylist->itemAt(
                mSeqNumber - firstSeqNumberInPlaylist,
                &uri,
                &itemMeta));

    int32_t val;
    if (itemMeta->findInt32("discontinuity", &val) && val != 0) {
        mDiscontinuitySeq++;
        discontinuity = true;
    }

    int64_t range_offset, range_length;
    if (!itemMeta->findInt64("range-offset", &range_offset)
            || !itemMeta->findInt64("range-length", &range_length)) {
        range_offset = 0;
        range_length = -1;
    }

    ALOGV("fetching segment %d from (%d .. %d)",
          mSeqNumber, firstSeqNumberInPlaylist, lastSeqNumberInPlaylist);

    ALOGV("fetching '%s'", uri.c_str());

    sp<DataSource> source;
    sp<ABuffer> buffer, tsBuffer;
    // decrypt a junk buffer to prefetch key; since a session uses only one http connection,
    // this avoids interleaved connections to the key and segment file.
    {
        sp<ABuffer> junk = new ABuffer(16);
        junk->setRange(0, 16);
        status_t err = decryptBuffer(mSeqNumber - firstSeqNumberInPlaylist, junk,
                true /* first */);
        if (err != OK) {
            notifyError(err);
            return;
        }
    }

    // block-wise download
    bool startup = mStartup;
    ssize_t bytesRead;
    do {
        bytesRead = mSession->fetchFile(
                uri.c_str(), &buffer, range_offset, range_length, kDownloadBlockSize, &source);

        if (bytesRead < 0) {
            status_t err = bytesRead;
            ALOGE("failed to fetch .ts segment at url '%s'", uri.c_str());
            notifyError(err);
            return;
        }

        CHECK(buffer != NULL);

        size_t size = buffer->size();
        // Set decryption range.
        buffer->setRange(size - bytesRead, bytesRead);
        status_t err = decryptBuffer(mSeqNumber - firstSeqNumberInPlaylist, buffer,
                buffer->offset() == 0 /* first */);
        // Unset decryption range.
        buffer->setRange(0, size);

        if (err != OK) {
            ALOGE("decryptBuffer failed w/ error %d", err);

            notifyError(err);
            return;
        }

        if (startup || discontinuity) {
            // Signal discontinuity.

            if (mPlaylist->isComplete() || mPlaylist->isEvent()) {
                // If this was a live event this made no sense since
                // we don't have access to all the segment before the current
                // one.
                mNextPTSTimeUs = getSegmentStartTimeUs(mSeqNumber);
            }

            if (discontinuity) {
                ALOGI("queueing discontinuity (explicit=%d)", discontinuity);

                queueDiscontinuity(
                        ATSParser::DISCONTINUITY_FORMATCHANGE,
                        NULL /* extra */);

                discontinuity = false;
            }

            startup = false;
        }

        err = OK;
        if (bufferStartsWithTsSyncByte(buffer)) {
            // Incremental extraction is only supported for MPEG2 transport streams.
            if (tsBuffer == NULL) {
                tsBuffer = new ABuffer(buffer->data(), buffer->capacity());
                tsBuffer->setRange(0, 0);
            } else if (tsBuffer->capacity() != buffer->capacity()) {
                size_t tsOff = tsBuffer->offset(), tsSize = tsBuffer->size();
                tsBuffer = new ABuffer(buffer->data(), buffer->capacity());
                tsBuffer->setRange(tsOff, tsSize);
            }
            tsBuffer->setRange(tsBuffer->offset(), tsBuffer->size() + bytesRead);

            err = extractAndQueueAccessUnitsFromTs(tsBuffer);
        }

        if (err == -EAGAIN) {
            // starting sequence number too low/high
            mTSParser.clear();
            postMonitorQueue();
            return;
        } else if (err == ERROR_OUT_OF_RANGE) {
            // reached stopping point
            stopAsync(/* clear = */ false);
            return;
        } else if (err != OK) {
            notifyError(err);
            return;
        }

    } while (bytesRead != 0);

    if (bufferStartsWithTsSyncByte(buffer)) {
        // If we still don't see a stream after fetching a full ts segment mark it as
        // nonexistent.
        const size_t kNumTypes = ATSParser::NUM_SOURCE_TYPES;
        ATSParser::SourceType srcTypes[kNumTypes] =
                { ATSParser::VIDEO, ATSParser::AUDIO };
        LiveSession::StreamType streamTypes[kNumTypes] =
                { LiveSession::STREAMTYPE_VIDEO, LiveSession::STREAMTYPE_AUDIO };

        for (size_t i = 0; i < kNumTypes; i++) {
            ATSParser::SourceType srcType = srcTypes[i];
            LiveSession::StreamType streamType = streamTypes[i];

            sp<AnotherPacketSource> source =
                static_cast<AnotherPacketSource *>(
                    mTSParser->getSource(srcType).get());

            if (source == NULL) {
                ALOGW("MPEG2 Transport stream does not contain %s data.",
                      srcType == ATSParser::VIDEO ? "video" : "audio");

                mStreamTypeMask &= ~streamType;
                mPacketSources.removeItem(streamType);
            }
        }

    }

    if (checkDecryptPadding(buffer) != OK) {
        ALOGE("Incorrect padding bytes after decryption.");
        notifyError(ERROR_MALFORMED);
        return;
    }

    status_t err = OK;
    if (tsBuffer != NULL) {
        AString method;
        CHECK(buffer->meta()->findString("cipher-method", &method));
        if ((tsBuffer->size() > 0 && method == "NONE")
                || tsBuffer->size() > 16) {
            ALOGE("MPEG2 transport stream is not an even multiple of 188 "
                    "bytes in length.");
            notifyError(ERROR_MALFORMED);
            return;
        }
    }

    // bulk extract non-ts files
    if (tsBuffer == NULL) {
        err = extractAndQueueAccessUnits(buffer, itemMeta);
        if (err == -EAGAIN) {
            // starting sequence number too low/high
            postMonitorQueue();
            return;
        } else if (err == ERROR_OUT_OF_RANGE) {
            // reached stopping point
            stopAsync(/* clear = */false);
            return;
        }
    }

    if (err != OK) {
        notifyError(err);
        return;
    }

    ++mSeqNumber;

    postMonitorQueue();
}

int32_t PlaylistFetcher::getSeqNumberWithAnchorTime(int64_t anchorTimeUs) const {
    int32_t firstSeqNumberInPlaylist, lastSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL
            || !mPlaylist->meta()->findInt32("media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }
    lastSeqNumberInPlaylist = firstSeqNumberInPlaylist + mPlaylist->size() - 1;

    int32_t index = mSeqNumber - firstSeqNumberInPlaylist - 1;
    while (index >= 0 && anchorTimeUs > mStartTimeUs) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(index, NULL /* uri */, &itemMeta));

        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));

        anchorTimeUs -= itemDurationUs;
        --index;
    }

    int32_t newSeqNumber = firstSeqNumberInPlaylist + index + 1;
    if (newSeqNumber <= lastSeqNumberInPlaylist) {
        return newSeqNumber;
    } else {
        return lastSeqNumberInPlaylist;
    }
}

int32_t PlaylistFetcher::getSeqNumberForDiscontinuity(size_t discontinuitySeq) const {
    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL
            || !mPlaylist->meta()->findInt32("media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }

    size_t curDiscontinuitySeq = mPlaylist->getDiscontinuitySeq();
    if (discontinuitySeq < curDiscontinuitySeq) {
        return firstSeqNumberInPlaylist <= 0 ? 0 : (firstSeqNumberInPlaylist - 1);
    }

    size_t index = 0;
    while (index < mPlaylist->size()) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt( index, NULL /* uri */, &itemMeta));

        int64_t discontinuity;
        if (itemMeta->findInt64("discontinuity", &discontinuity)) {
            curDiscontinuitySeq++;
        }

        if (curDiscontinuitySeq == discontinuitySeq) {
            return firstSeqNumberInPlaylist + index;
        }

        ++index;
    }

    return firstSeqNumberInPlaylist + mPlaylist->size();
}

int32_t PlaylistFetcher::getSeqNumberForTime(int64_t timeUs) const {
    int32_t firstSeqNumberInPlaylist;
    if (mPlaylist->meta() == NULL || !mPlaylist->meta()->findInt32(
                "media-sequence", &firstSeqNumberInPlaylist)) {
        firstSeqNumberInPlaylist = 0;
    }

    size_t index = 0;
    int64_t segmentStartUs = 0;
    while (index < mPlaylist->size()) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(
                    index, NULL /* uri */, &itemMeta));

        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));

        if (timeUs < segmentStartUs + itemDurationUs) {
            break;
        }

        segmentStartUs += itemDurationUs;
        ++index;
    }

    if (index >= mPlaylist->size()) {
        index = mPlaylist->size() - 1;
    }

    return firstSeqNumberInPlaylist + index;
}

const sp<ABuffer> &PlaylistFetcher::setAccessUnitProperties(
        const sp<ABuffer> &accessUnit, const sp<AnotherPacketSource> &source, bool discard) {
    sp<MetaData> format = source->getFormat();
    if (format != NULL) {
        // for simplicity, store a reference to the format in each unit
        accessUnit->meta()->setObject("format", format);
    }

    if (discard) {
        accessUnit->meta()->setInt32("discard", discard);
    }

    accessUnit->meta()->setInt32("discontinuitySeq", mDiscontinuitySeq);
    accessUnit->meta()->setInt64("segmentStartTimeUs", getSegmentStartTimeUs(mSeqNumber));
    return accessUnit;
}

status_t PlaylistFetcher::extractAndQueueAccessUnitsFromTs(const sp<ABuffer> &buffer) {
    if (mTSParser == NULL) {
        // Use TS_TIMESTAMPS_ARE_ABSOLUTE so pts carry over between fetchers.
        mTSParser = new ATSParser(ATSParser::TS_TIMESTAMPS_ARE_ABSOLUTE);
    }

    if (mNextPTSTimeUs >= 0ll) {
        sp<AMessage> extra = new AMessage;
        // Since we are using absolute timestamps, signal an offset of 0 to prevent
        // ATSParser from skewing the timestamps of access units.
        extra->setInt64(IStreamListener::kKeyMediaTimeUs, 0);

        mTSParser->signalDiscontinuity(
                ATSParser::DISCONTINUITY_SEEK, extra);

        mAbsoluteTimeAnchorUs = mNextPTSTimeUs;
        mNextPTSTimeUs = -1ll;
        mFirstPTSValid = false;
    }

    size_t offset = 0;
    while (offset + 188 <= buffer->size()) {
        status_t err = mTSParser->feedTSPacket(buffer->data() + offset, 188);

        if (err != OK) {
            return err;
        }

        offset += 188;
    }
    // setRange to indicate consumed bytes.
    buffer->setRange(buffer->offset() + offset, buffer->size() - offset);

    status_t err = OK;
    for (size_t i = mPacketSources.size(); i-- > 0;) {
        sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);

        const char *key;
        ATSParser::SourceType type;
        const LiveSession::StreamType stream = mPacketSources.keyAt(i);
        switch (stream) {
            case LiveSession::STREAMTYPE_VIDEO:
                type = ATSParser::VIDEO;
                key = "timeUsVideo";
                break;

            case LiveSession::STREAMTYPE_AUDIO:
                type = ATSParser::AUDIO;
                key = "timeUsAudio";
                break;

            case LiveSession::STREAMTYPE_SUBTITLES:
            {
                ALOGE("MPEG2 Transport streams do not contain subtitles.");
                return ERROR_MALFORMED;
                break;
            }

            default:
                TRESPASS();
        }

        sp<AnotherPacketSource> source =
            static_cast<AnotherPacketSource *>(
                    mTSParser->getSource(type).get());

        if (source == NULL) {
            continue;
        }

        int64_t timeUs;
        sp<ABuffer> accessUnit;
        status_t finalResult;
        while (source->hasBufferAvailable(&finalResult)
                && source->dequeueAccessUnit(&accessUnit) == OK) {

            CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

            if (mStartup) {
                if (!mFirstPTSValid) {
                    mFirstTimeUs = timeUs;
                    mFirstPTSValid = true;
                }
                if (mStartTimeUsRelative) {
                    timeUs -= mFirstTimeUs;
                    if (timeUs < 0) {
                        timeUs = 0;
                    }
                }

                if (timeUs < mStartTimeUs) {
                    // buffer up to the closest preceding IDR frame
                    ALOGV("timeUs %" PRId64 " us < mStartTimeUs %" PRId64 " us",
                            timeUs, mStartTimeUs);
                    const char *mime;
                    sp<MetaData> format  = source->getFormat();
                    bool isAvc = false;
                    if (format != NULL && format->findCString(kKeyMIMEType, &mime)
                            && !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
                        isAvc = true;
                    }
                    if (isAvc && IsIDR(accessUnit)) {
                        mVideoBuffer->clear();
                    }
                    if (isAvc) {
                        mVideoBuffer->queueAccessUnit(accessUnit);
                    }

                    continue;
                }
            }

            CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));
            if (mStartTimeUsNotify != NULL && timeUs > mStartTimeUs) {

                int32_t targetDurationSecs;
                CHECK(mPlaylist->meta()->findInt32("target-duration", &targetDurationSecs));
                int64_t targetDurationUs = targetDurationSecs * 1000000ll;
                // mStartup
                //   mStartup is true until we have queued a packet for all the streams
                //   we are fetching. We queue packets whose timestamps are greater than
                //   mStartTimeUs.
                // mSegmentStartTimeUs >= 0
                //   mSegmentStartTimeUs is non-negative when adapting or switching tracks
                // timeUs - mStartTimeUs > targetDurationUs:
                //   This and the 2 above conditions should only happen when adapting in a live
                //   stream; the old fetcher has already fetched to mStartTimeUs; the new fetcher
                //   would start fetching after timeUs, which should be greater than mStartTimeUs;
                //   the old fetcher would then continue fetching data until timeUs. We don't want
                //   timeUs to be too far ahead of mStartTimeUs because we want the old fetcher to
                //   stop as early as possible. The definition of being "too far ahead" is
                //   arbitrary; here we use targetDurationUs as threshold.
                if (mStartup && mSegmentStartTimeUs >= 0
                        && timeUs - mStartTimeUs > targetDurationUs) {
                    // we just guessed a starting timestamp that is too high when adapting in a
                    // live stream; re-adjust based on the actual timestamp extracted from the
                    // media segment; if we didn't move backward after the re-adjustment
                    // (newSeqNumber), start at least 1 segment prior.
                    int32_t newSeqNumber = getSeqNumberWithAnchorTime(timeUs);
                    if (newSeqNumber >= mSeqNumber) {
                        --mSeqNumber;
                    } else {
                        mSeqNumber = newSeqNumber;
                    }
                    mStartTimeUsNotify = mNotify->dup();
                    mStartTimeUsNotify->setInt32("what", kWhatStartedAt);
                    return -EAGAIN;
                }

                int32_t seq;
                if (!mStartTimeUsNotify->findInt32("discontinuitySeq", &seq)) {
                    mStartTimeUsNotify->setInt32("discontinuitySeq", mDiscontinuitySeq);
                }
                int64_t startTimeUs;
                if (!mStartTimeUsNotify->findInt64(key, &startTimeUs)) {
                    mStartTimeUsNotify->setInt64(key, timeUs);

                    uint32_t streamMask = 0;
                    mStartTimeUsNotify->findInt32("streamMask", (int32_t *) &streamMask);
                    streamMask |= mPacketSources.keyAt(i);
                    mStartTimeUsNotify->setInt32("streamMask", streamMask);

                    if (streamMask == mStreamTypeMask) {
                        mStartup = false;
                        mStartTimeUsNotify->post();
                        mStartTimeUsNotify.clear();
                    }
                }
            }

            if (mStopParams != NULL) {
                // Queue discontinuity in original stream.
                int32_t discontinuitySeq;
                int64_t stopTimeUs;
                if (!mStopParams->findInt32("discontinuitySeq", &discontinuitySeq)
                        || discontinuitySeq > mDiscontinuitySeq
                        || !mStopParams->findInt64(key, &stopTimeUs)
                        || (discontinuitySeq == mDiscontinuitySeq
                                && timeUs >= stopTimeUs)) {
                    packetSource->queueAccessUnit(mSession->createFormatChangeBuffer());
                    mStreamTypeMask &= ~stream;
                    mPacketSources.removeItemsAt(i);
                    break;
                }
            }

            // Note that we do NOT dequeue any discontinuities except for format change.
            if (stream == LiveSession::STREAMTYPE_VIDEO) {
                const bool discard = true;
                status_t status;
                while (mVideoBuffer->hasBufferAvailable(&status)) {
                    sp<ABuffer> videoBuffer;
                    mVideoBuffer->dequeueAccessUnit(&videoBuffer);
                    setAccessUnitProperties(videoBuffer, source, discard);
                    packetSource->queueAccessUnit(videoBuffer);
                }
            }

            setAccessUnitProperties(accessUnit, source);
            packetSource->queueAccessUnit(accessUnit);
        }

        if (err != OK) {
            break;
        }
    }

    if (err != OK) {
        for (size_t i = mPacketSources.size(); i-- > 0;) {
            sp<AnotherPacketSource> packetSource = mPacketSources.valueAt(i);
            packetSource->clear();
        }
        return err;
    }

    if (!mStreamTypeMask) {
        // Signal gap is filled between original and new stream.
        ALOGV("ERROR OUT OF RANGE");
        return ERROR_OUT_OF_RANGE;
    }

    return OK;
}

/* static */
bool PlaylistFetcher::bufferStartsWithWebVTTMagicSequence(
        const sp<ABuffer> &buffer) {
    size_t pos = 0;

    // skip possible BOM
    if (buffer->size() >= pos + 3 &&
            !memcmp("\xef\xbb\xbf", buffer->data() + pos, 3)) {
        pos += 3;
    }

    // accept WEBVTT followed by SPACE, TAB or (CR) LF
    if (buffer->size() < pos + 6 ||
            memcmp("WEBVTT", buffer->data() + pos, 6)) {
        return false;
    }
    pos += 6;

    if (buffer->size() == pos) {
        return true;
    }

    uint8_t sep = buffer->data()[pos];
    return sep == ' ' || sep == '\t' || sep == '\n' || sep == '\r';
}

status_t PlaylistFetcher::extractAndQueueAccessUnits(
        const sp<ABuffer> &buffer, const sp<AMessage> &itemMeta) {
    if (bufferStartsWithWebVTTMagicSequence(buffer)) {
        if (mStreamTypeMask != LiveSession::STREAMTYPE_SUBTITLES) {
            ALOGE("This stream only contains subtitles.");
            return ERROR_MALFORMED;
        }

        const sp<AnotherPacketSource> packetSource =
            mPacketSources.valueFor(LiveSession::STREAMTYPE_SUBTITLES);

        int64_t durationUs;
        CHECK(itemMeta->findInt64("durationUs", &durationUs));
        buffer->meta()->setInt64("timeUs", getSegmentStartTimeUs(mSeqNumber));
        buffer->meta()->setInt64("durationUs", durationUs);
        buffer->meta()->setInt64("segmentStartTimeUs", getSegmentStartTimeUs(mSeqNumber));
        buffer->meta()->setInt32("discontinuitySeq", mDiscontinuitySeq);

        packetSource->queueAccessUnit(buffer);
        return OK;
    }

    if (mNextPTSTimeUs >= 0ll) {
        mFirstPTSValid = false;
        mAbsoluteTimeAnchorUs = mNextPTSTimeUs;
        mNextPTSTimeUs = -1ll;
    }

    // This better be an ISO 13818-7 (AAC) or ISO 13818-1 (MPEG) audio
    // stream prefixed by an ID3 tag.

    bool firstID3Tag = true;
    uint64_t PTS = 0;

    for (;;) {
        // Make sure to skip all ID3 tags preceding the audio data.
        // At least one must be present to provide the PTS timestamp.

        ID3 id3(buffer->data(), buffer->size(), true /* ignoreV1 */);
        if (!id3.isValid()) {
            if (firstID3Tag) {
                ALOGE("Unable to parse ID3 tag.");
                return ERROR_MALFORMED;
            } else {
                break;
            }
        }

        if (firstID3Tag) {
            bool found = false;

            ID3::Iterator it(id3, "PRIV");
            while (!it.done()) {
                size_t length;
                const uint8_t *data = it.getData(&length);

                static const char *kMatchName =
                    "com.apple.streaming.transportStreamTimestamp";
                static const size_t kMatchNameLen = strlen(kMatchName);

                if (length == kMatchNameLen + 1 + 8
                        && !strncmp((const char *)data, kMatchName, kMatchNameLen)) {
                    found = true;
                    PTS = U64_AT(&data[kMatchNameLen + 1]);
                }

                it.next();
            }

            if (!found) {
                ALOGE("Unable to extract transportStreamTimestamp from ID3 tag.");
                return ERROR_MALFORMED;
            }
        }

        // skip the ID3 tag
        buffer->setRange(
                buffer->offset() + id3.rawSize(), buffer->size() - id3.rawSize());

        firstID3Tag = false;
    }

    if (mStreamTypeMask != LiveSession::STREAMTYPE_AUDIO) {
        ALOGW("This stream only contains audio data!");

        mStreamTypeMask &= LiveSession::STREAMTYPE_AUDIO;

        if (mStreamTypeMask == 0) {
            return OK;
        }
    }

    sp<AnotherPacketSource> packetSource =
        mPacketSources.valueFor(LiveSession::STREAMTYPE_AUDIO);

    if (packetSource->getFormat() == NULL && buffer->size() >= 7) {
        ABitReader bits(buffer->data(), buffer->size());

        // adts_fixed_header

        CHECK_EQ(bits.getBits(12), 0xfffu);
        bits.skipBits(3);  // ID, layer
        bool protection_absent = bits.getBits(1) != 0;

        unsigned profile = bits.getBits(2);
        CHECK_NE(profile, 3u);
        unsigned sampling_freq_index = bits.getBits(4);
        bits.getBits(1);  // private_bit
        unsigned channel_configuration = bits.getBits(3);
        CHECK_NE(channel_configuration, 0u);
        bits.skipBits(2);  // original_copy, home

        sp<MetaData> meta = MakeAACCodecSpecificData(
                profile, sampling_freq_index, channel_configuration);

        meta->setInt32(kKeyIsADTS, true);

        packetSource->setFormat(meta);
    }

    int64_t numSamples = 0ll;
    int32_t sampleRate;
    CHECK(packetSource->getFormat()->findInt32(kKeySampleRate, &sampleRate));

    int64_t timeUs = (PTS * 100ll) / 9ll;
    if (!mFirstPTSValid) {
        mFirstPTSValid = true;
        mFirstTimeUs = timeUs;
    }

    size_t offset = 0;
    while (offset < buffer->size()) {
        const uint8_t *adtsHeader = buffer->data() + offset;
        CHECK_LT(offset + 5, buffer->size());

        unsigned aac_frame_length =
            ((adtsHeader[3] & 3) << 11)
            | (adtsHeader[4] << 3)
            | (adtsHeader[5] >> 5);

        if (aac_frame_length == 0) {
            const uint8_t *id3Header = adtsHeader;
            if (!memcmp(id3Header, "ID3", 3)) {
                ID3 id3(id3Header, buffer->size() - offset, true);
                if (id3.isValid()) {
                    offset += id3.rawSize();
                    continue;
                };
            }
            return ERROR_MALFORMED;
        }

        CHECK_LE(offset + aac_frame_length, buffer->size());

        int64_t unitTimeUs = timeUs + numSamples * 1000000ll / sampleRate;
        offset += aac_frame_length;

        // Each AAC frame encodes 1024 samples.
        numSamples += 1024;

        if (mStartup) {
            int64_t startTimeUs = unitTimeUs;
            if (mStartTimeUsRelative) {
                startTimeUs -= mFirstTimeUs;
                if (startTimeUs  < 0) {
                    startTimeUs = 0;
                }
            }
            if (startTimeUs < mStartTimeUs) {
                continue;
            }

            if (mStartTimeUsNotify != NULL) {
                int32_t targetDurationSecs;
                CHECK(mPlaylist->meta()->findInt32("target-duration", &targetDurationSecs));
                int64_t targetDurationUs = targetDurationSecs * 1000000ll;

                // Duplicated logic from how we handle .ts playlists.
                if (mStartup && mSegmentStartTimeUs >= 0
                        && timeUs - mStartTimeUs > targetDurationUs) {
                    int32_t newSeqNumber = getSeqNumberWithAnchorTime(timeUs);
                    if (newSeqNumber >= mSeqNumber) {
                        --mSeqNumber;
                    } else {
                        mSeqNumber = newSeqNumber;
                    }
                    return -EAGAIN;
                }

                mStartTimeUsNotify->setInt64("timeUsAudio", timeUs);
                mStartTimeUsNotify->setInt32("discontinuitySeq", mDiscontinuitySeq);
                mStartTimeUsNotify->setInt32("streamMask", LiveSession::STREAMTYPE_AUDIO);
                mStartTimeUsNotify->post();
                mStartTimeUsNotify.clear();
            }
        }

        if (mStopParams != NULL) {
            // Queue discontinuity in original stream.
            int32_t discontinuitySeq;
            int64_t stopTimeUs;
            if (!mStopParams->findInt32("discontinuitySeq", &discontinuitySeq)
                    || discontinuitySeq > mDiscontinuitySeq
                    || !mStopParams->findInt64("timeUsAudio", &stopTimeUs)
                    || (discontinuitySeq == mDiscontinuitySeq && unitTimeUs >= stopTimeUs)) {
                packetSource->queueAccessUnit(mSession->createFormatChangeBuffer());
                mStreamTypeMask = 0;
                mPacketSources.clear();
                return ERROR_OUT_OF_RANGE;
            }
        }

        sp<ABuffer> unit = new ABuffer(aac_frame_length);
        memcpy(unit->data(), adtsHeader, aac_frame_length);

        unit->meta()->setInt64("timeUs", unitTimeUs);
        setAccessUnitProperties(unit, packetSource);
        packetSource->queueAccessUnit(unit);
    }

    return OK;
}

void PlaylistFetcher::updateDuration() {
    int64_t durationUs = 0ll;
    for (size_t index = 0; index < mPlaylist->size(); ++index) {
        sp<AMessage> itemMeta;
        CHECK(mPlaylist->itemAt(
                    index, NULL /* uri */, &itemMeta));

        int64_t itemDurationUs;
        CHECK(itemMeta->findInt64("durationUs", &itemDurationUs));

        durationUs += itemDurationUs;
    }

    sp<AMessage> msg = mNotify->dup();
    msg->setInt32("what", kWhatDurationUpdate);
    msg->setInt64("durationUs", durationUs);
    msg->post();
}

int64_t PlaylistFetcher::resumeThreshold(const sp<AMessage> &msg) {
    int64_t durationUs, threshold;
    if (msg->findInt64("durationUs", &durationUs)) {
        return kNumSkipFrames * durationUs;
    }

    sp<RefBase> obj;
    msg->findObject("format", &obj);
    MetaData *format = static_cast<MetaData *>(obj.get());

    const char *mime;
    CHECK(format->findCString(kKeyMIMEType, &mime));
    bool audio = !strncasecmp(mime, "audio/", 6);
    if (audio) {
        // Assumes 1000 samples per frame.
        int32_t sampleRate;
        CHECK(format->findInt32(kKeySampleRate, &sampleRate));
        return kNumSkipFrames  /* frames */ * 1000 /* samples */
                * (1000000 / sampleRate) /* sample duration (us) */;
    } else {
        int32_t frameRate;
        if (format->findInt32(kKeyFrameRate, &frameRate) && frameRate > 0) {
            return kNumSkipFrames * (1000000 / frameRate);
        }
    }

    return 500000ll;
}

}  // namespace android
