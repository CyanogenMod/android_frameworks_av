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
#define LOG_TAG "GenericSource"

#include "GenericSource.h"

#include "AnotherPacketSource.h"

#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include "../../libstagefright/include/DRMExtractor.h"
#include "../../libstagefright/include/NuCachedSource2.h"
#include "../../libstagefright/include/WVMExtractor.h"

namespace android {

NuPlayer::GenericSource::GenericSource(
        const sp<AMessage> &notify,
        bool uidValid,
        uid_t uid)
    : Source(notify),
      mFetchSubtitleDataGeneration(0),
      mFetchTimedTextDataGeneration(0),
      mDurationUs(0ll),
      mAudioIsVorbis(false),
      mIsWidevine(false),
      mUIDValid(uidValid),
      mUID(uid),
      mDrmManagerClient(NULL),
      mMetaDataSize(-1ll),
      mBitrate(-1ll),
      mPollBufferingGeneration(0),
      mPendingReadBufferTypes(0) {
    resetDataSource();
    DataSource::RegisterDefaultSniffers();
}

void NuPlayer::GenericSource::resetDataSource() {
    mAudioTimeUs = 0;
    mVideoTimeUs = 0;
    mHTTPService.clear();
    mUri.clear();
    mUriHeaders.clear();
    mFd = -1;
    mOffset = 0;
    mLength = 0;
    setDrmPlaybackStatusIfNeeded(Playback::STOP, 0);
    mDecryptHandle = NULL;
    mDrmManagerClient = NULL;
    mStarted = false;
}

status_t NuPlayer::GenericSource::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers) {
    resetDataSource();

    mHTTPService = httpService;
    mUri = url;

    if (headers) {
        mUriHeaders = *headers;
    }

    // delay data source creation to prepareAsync() to avoid blocking
    // the calling thread in setDataSource for any significant time.
    return OK;
}

status_t NuPlayer::GenericSource::setDataSource(
        int fd, int64_t offset, int64_t length) {
    resetDataSource();

    mFd = dup(fd);
    mOffset = offset;
    mLength = length;

    // delay data source creation to prepareAsync() to avoid blocking
    // the calling thread in setDataSource for any significant time.
    return OK;
}

status_t NuPlayer::GenericSource::initFromDataSource() {
    sp<MediaExtractor> extractor;

    CHECK(mDataSource != NULL);

    if (mIsWidevine) {
        String8 mimeType;
        float confidence;
        sp<AMessage> dummy;
        bool success;

        success = SniffWVM(mDataSource, &mimeType, &confidence, &dummy);
        if (!success
                || strcasecmp(
                    mimeType.string(), MEDIA_MIMETYPE_CONTAINER_WVM)) {
            ALOGE("unsupported widevine mime: %s", mimeType.string());
            return UNKNOWN_ERROR;
        }

        mWVMExtractor = new WVMExtractor(mDataSource);
        mWVMExtractor->setAdaptiveStreamingMode(true);
        if (mUIDValid) {
            mWVMExtractor->setUID(mUID);
        }
        extractor = mWVMExtractor;
    } else {
        extractor = MediaExtractor::Create(mDataSource,
                mSniffedMIME.empty() ? NULL: mSniffedMIME.c_str());
    }

    if (extractor == NULL) {
        return UNKNOWN_ERROR;
    }

    if (extractor->getDrmFlag()) {
        checkDrmStatus(mDataSource);
    }

    sp<MetaData> fileMeta = extractor->getMetaData();
    if (fileMeta != NULL) {
        int64_t duration;
        if (fileMeta->findInt64(kKeyDuration, &duration)) {
            mDurationUs = duration;
        }
    }

    int32_t totalBitrate = 0;

    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MediaSource> track = extractor->getTrack(i);

        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        // Do the string compare immediately with "mime",
        // we can't assume "mime" would stay valid after another
        // extractor operation, some extractors might modify meta
        // during getTrack() and make it invalid.
        if (!strncasecmp(mime, "audio/", 6)) {
            if (mAudioTrack.mSource == NULL) {
                mAudioTrack.mIndex = i;
                mAudioTrack.mSource = track;
                mAudioTrack.mPackets =
                    new AnotherPacketSource(mAudioTrack.mSource->getFormat());

                if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)) {
                    mAudioIsVorbis = true;
                } else {
                    mAudioIsVorbis = false;
                }
            }
        } else if (!strncasecmp(mime, "video/", 6)) {
            if (mVideoTrack.mSource == NULL) {
                mVideoTrack.mIndex = i;
                mVideoTrack.mSource = track;
                mVideoTrack.mPackets =
                    new AnotherPacketSource(mVideoTrack.mSource->getFormat());

                // check if the source requires secure buffers
                int32_t secure;
                if (meta->findInt32(kKeyRequiresSecureBuffers, &secure)
                        && secure) {
                    mIsWidevine = true;
                    if (mUIDValid) {
                        extractor->setUID(mUID);
                    }
                }
            }
        }

        if (track != NULL) {
            mSources.push(track);
            int64_t durationUs;
            if (meta->findInt64(kKeyDuration, &durationUs)) {
                if (durationUs > mDurationUs) {
                    mDurationUs = durationUs;
                }
            }

            int32_t bitrate;
            if (totalBitrate >= 0 && meta->findInt32(kKeyBitRate, &bitrate)) {
                totalBitrate += bitrate;
            } else {
                totalBitrate = -1;
            }
        }
    }

    mBitrate = totalBitrate;

    return OK;
}

void NuPlayer::GenericSource::checkDrmStatus(const sp<DataSource>& dataSource) {
    dataSource->getDrmInfo(mDecryptHandle, &mDrmManagerClient);
    if (mDecryptHandle != NULL) {
        CHECK(mDrmManagerClient);
        if (RightsStatus::RIGHTS_VALID != mDecryptHandle->status) {
            sp<AMessage> msg = dupNotify();
            msg->setInt32("what", kWhatDrmNoLicense);
            msg->post();
        }
    }
}

int64_t NuPlayer::GenericSource::getLastReadPosition() {
    if (mAudioTrack.mSource != NULL) {
        return mAudioTimeUs;
    } else if (mVideoTrack.mSource != NULL) {
        return mVideoTimeUs;
    } else {
        return 0;
    }
}

status_t NuPlayer::GenericSource::setBuffers(
        bool audio, Vector<MediaBuffer *> &buffers) {
    if (mIsWidevine && !audio) {
        return mVideoTrack.mSource->setBuffers(buffers);
    }
    return INVALID_OPERATION;
}

NuPlayer::GenericSource::~GenericSource() {
    if (mLooper != NULL) {
        mLooper->unregisterHandler(id());
        mLooper->stop();
    }
}

void NuPlayer::GenericSource::prepareAsync() {
    if (mLooper == NULL) {
        mLooper = new ALooper;
        mLooper->setName("generic");
        mLooper->start();

        mLooper->registerHandler(this);
    }

    sp<AMessage> msg = new AMessage(kWhatPrepareAsync, id());
    msg->post();
}

void NuPlayer::GenericSource::onPrepareAsync() {
    // delayed data source creation
    if (mDataSource == NULL) {
        if (!mUri.empty()) {
            mIsWidevine = !strncasecmp(mUri.c_str(), "widevine://", 11);

            mDataSource = DataSource::CreateFromURI(
                   mHTTPService, mUri.c_str(), &mUriHeaders, &mContentType);
        } else {
            // set to false first, if the extractor
            // comes back as secure, set it to true then.
            mIsWidevine = false;

            mDataSource = new FileSource(mFd, mOffset, mLength);
        }

        if (mDataSource == NULL) {
            ALOGE("Failed to create data source!");
            notifyPreparedAndCleanup(UNKNOWN_ERROR);
            return;
        }

        if (mDataSource->flags() & DataSource::kIsCachingDataSource) {
            mCachedSource = static_cast<NuCachedSource2 *>(mDataSource.get());
        }

        if (mIsWidevine || mCachedSource != NULL) {
            schedulePollBuffering();
        }
    }

    // check initial caching status
    status_t err = prefillCacheIfNecessary();
    if (err != OK) {
        if (err == -EAGAIN) {
            (new AMessage(kWhatPrepareAsync, id()))->post(200000);
        } else {
            ALOGE("Failed to prefill data cache!");
            notifyPreparedAndCleanup(UNKNOWN_ERROR);
        }
        return;
    }

    // init extrator from data source
    err = initFromDataSource();

    if (err != OK) {
        ALOGE("Failed to init from data source!");
        notifyPreparedAndCleanup(err);
        return;
    }

    if (mVideoTrack.mSource != NULL) {
        sp<MetaData> meta = doGetFormatMeta(false /* audio */);
        sp<AMessage> msg = new AMessage;
        err = convertMetaDataToMessage(meta, &msg);
        if(err != OK) {
            notifyPreparedAndCleanup(err);
            return;
        }
        notifyVideoSizeChanged(msg);
    }

    notifyFlagsChanged(
            (mIsWidevine ? FLAG_SECURE : 0)
            | FLAG_CAN_PAUSE
            | FLAG_CAN_SEEK_BACKWARD
            | FLAG_CAN_SEEK_FORWARD
            | FLAG_CAN_SEEK);

    notifyPrepared();
}

void NuPlayer::GenericSource::notifyPreparedAndCleanup(status_t err) {
    if (err != OK) {
        mMetaDataSize = -1ll;
        mContentType = "";
        mSniffedMIME = "";
        mDataSource.clear();
        mCachedSource.clear();

        cancelPollBuffering();
    }
    notifyPrepared(err);
}

status_t NuPlayer::GenericSource::prefillCacheIfNecessary() {
    CHECK(mDataSource != NULL);

    if (mCachedSource == NULL) {
        // no prefill if the data source is not cached
        return OK;
    }

    // We're not doing this for streams that appear to be audio-only
    // streams to ensure that even low bandwidth streams start
    // playing back fairly instantly.
    if (!strncasecmp(mContentType.string(), "audio/", 6)) {
        return OK;
    }

    // We're going to prefill the cache before trying to instantiate
    // the extractor below, as the latter is an operation that otherwise
    // could block on the datasource for a significant amount of time.
    // During that time we'd be unable to abort the preparation phase
    // without this prefill.

    // Initially make sure we have at least 192 KB for the sniff
    // to complete without blocking.
    static const size_t kMinBytesForSniffing = 192 * 1024;
    static const size_t kDefaultMetaSize = 200000;

    status_t finalStatus;

    size_t cachedDataRemaining =
            mCachedSource->approxDataRemaining(&finalStatus);

    if (finalStatus != OK || (mMetaDataSize >= 0
            && (off64_t)cachedDataRemaining >= mMetaDataSize)) {
        ALOGV("stop caching, status %d, "
                "metaDataSize %lld, cachedDataRemaining %zu",
                finalStatus, mMetaDataSize, cachedDataRemaining);
        return OK;
    }

    ALOGV("now cached %zu bytes of data", cachedDataRemaining);

    if (mMetaDataSize < 0
            && cachedDataRemaining >= kMinBytesForSniffing) {
        String8 tmp;
        float confidence;
        sp<AMessage> meta;
        if (!mCachedSource->sniff(&tmp, &confidence, &meta)) {
            return UNKNOWN_ERROR;
        }

        // We successfully identified the file's extractor to
        // be, remember this mime type so we don't have to
        // sniff it again when we call MediaExtractor::Create()
        mSniffedMIME = tmp.string();

        if (meta == NULL
                || !meta->findInt64("meta-data-size",
                        reinterpret_cast<int64_t*>(&mMetaDataSize))) {
            mMetaDataSize = kDefaultMetaSize;
        }

        if (mMetaDataSize < 0ll) {
            ALOGE("invalid metaDataSize = %lld bytes", mMetaDataSize);
            return UNKNOWN_ERROR;
        }
    }

    return -EAGAIN;
}

void NuPlayer::GenericSource::start() {
    ALOGI("start");

    if (mAudioTrack.mSource != NULL) {
        CHECK_EQ(mAudioTrack.mSource->start(), (status_t)OK);

        postReadBuffer(MEDIA_TRACK_TYPE_AUDIO);
    }

    if (mVideoTrack.mSource != NULL) {
        CHECK_EQ(mVideoTrack.mSource->start(), (status_t)OK);

        postReadBuffer(MEDIA_TRACK_TYPE_VIDEO);
    }

    setDrmPlaybackStatusIfNeeded(Playback::START, getLastReadPosition() / 1000);
    mStarted = true;
}

void NuPlayer::GenericSource::stop() {
    // nothing to do, just account for DRM playback status
    setDrmPlaybackStatusIfNeeded(Playback::STOP, 0);
    mStarted = false;
}

void NuPlayer::GenericSource::pause() {
    // nothing to do, just account for DRM playback status
    setDrmPlaybackStatusIfNeeded(Playback::PAUSE, 0);
    mStarted = false;
}

void NuPlayer::GenericSource::resume() {
    // nothing to do, just account for DRM playback status
    setDrmPlaybackStatusIfNeeded(Playback::START, getLastReadPosition() / 1000);
    mStarted = true;
}

void NuPlayer::GenericSource::disconnect() {
    if (mDataSource != NULL) {
        // disconnect data source
        if (mDataSource->flags() & DataSource::kIsCachingDataSource) {
            static_cast<NuCachedSource2 *>(mDataSource.get())->disconnect();
        }
    }
}

void NuPlayer::GenericSource::setDrmPlaybackStatusIfNeeded(int playbackStatus, int64_t position) {
    if (mDecryptHandle != NULL) {
        mDrmManagerClient->setPlaybackStatus(mDecryptHandle, playbackStatus, position);
    }
    mSubtitleTrack.mPackets = new AnotherPacketSource(NULL);
    mTimedTextTrack.mPackets = new AnotherPacketSource(NULL);
}

status_t NuPlayer::GenericSource::feedMoreTSData() {
    return OK;
}

void NuPlayer::GenericSource::schedulePollBuffering() {
    sp<AMessage> msg = new AMessage(kWhatPollBuffering, id());
    msg->setInt32("generation", mPollBufferingGeneration);
    msg->post(1000000ll);
}

void NuPlayer::GenericSource::cancelPollBuffering() {
    ++mPollBufferingGeneration;
}

void NuPlayer::GenericSource::notifyBufferingUpdate(int percentage) {
    sp<AMessage> msg = dupNotify();
    msg->setInt32("what", kWhatBufferingUpdate);
    msg->setInt32("percentage", percentage);
    msg->post();
}

void NuPlayer::GenericSource::onPollBuffering() {
    status_t finalStatus = UNKNOWN_ERROR;
    int64_t cachedDurationUs = 0ll;

    if (mCachedSource != NULL) {
        size_t cachedDataRemaining =
                mCachedSource->approxDataRemaining(&finalStatus);

        if (finalStatus == OK) {
            off64_t size;
            int64_t bitrate = 0ll;
            if (mDurationUs > 0 && mCachedSource->getSize(&size) == OK) {
                bitrate = size * 8000000ll / mDurationUs;
            } else if (mBitrate > 0) {
                bitrate = mBitrate;
            }
            if (bitrate > 0) {
                cachedDurationUs = cachedDataRemaining * 8000000ll / bitrate;
            }
        }
    } else if (mWVMExtractor != NULL) {
        cachedDurationUs
            = mWVMExtractor->getCachedDurationUs(&finalStatus);
    }

    if (finalStatus == ERROR_END_OF_STREAM) {
        notifyBufferingUpdate(100);
        cancelPollBuffering();
        return;
    } else if (cachedDurationUs > 0ll && mDurationUs > 0ll) {
        int percentage = 100.0 * cachedDurationUs / mDurationUs;
        if (percentage > 100) {
            percentage = 100;
        }

        notifyBufferingUpdate(percentage);
    }

    schedulePollBuffering();
}


void NuPlayer::GenericSource::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
      case kWhatPrepareAsync:
      {
          onPrepareAsync();
          break;
      }
      case kWhatFetchSubtitleData:
      {
          fetchTextData(kWhatSendSubtitleData, MEDIA_TRACK_TYPE_SUBTITLE,
                  mFetchSubtitleDataGeneration, mSubtitleTrack.mPackets, msg);
          break;
      }

      case kWhatFetchTimedTextData:
      {
          fetchTextData(kWhatSendTimedTextData, MEDIA_TRACK_TYPE_TIMEDTEXT,
                  mFetchTimedTextDataGeneration, mTimedTextTrack.mPackets, msg);
          break;
      }

      case kWhatSendSubtitleData:
      {
          sendTextData(kWhatSubtitleData, MEDIA_TRACK_TYPE_SUBTITLE,
                  mFetchSubtitleDataGeneration, mSubtitleTrack.mPackets, msg);
          break;
      }

      case kWhatSendTimedTextData:
      {
          sendTextData(kWhatTimedTextData, MEDIA_TRACK_TYPE_TIMEDTEXT,
                  mFetchTimedTextDataGeneration, mTimedTextTrack.mPackets, msg);
          break;
      }

      case kWhatChangeAVSource:
      {
          int32_t trackIndex;
          CHECK(msg->findInt32("trackIndex", &trackIndex));
          const sp<MediaSource> source = mSources.itemAt(trackIndex);

          Track* track;
          const char *mime;
          media_track_type trackType, counterpartType;
          sp<MetaData> meta = source->getFormat();
          meta->findCString(kKeyMIMEType, &mime);
          if (!strncasecmp(mime, "audio/", 6)) {
              track = &mAudioTrack;
              trackType = MEDIA_TRACK_TYPE_AUDIO;
              counterpartType = MEDIA_TRACK_TYPE_VIDEO;;
          } else {
              CHECK(!strncasecmp(mime, "video/", 6));
              track = &mVideoTrack;
              trackType = MEDIA_TRACK_TYPE_VIDEO;
              counterpartType = MEDIA_TRACK_TYPE_AUDIO;;
          }


          if (track->mSource != NULL) {
              track->mSource->stop();
          }
          track->mSource = source;
          track->mSource->start();
          track->mIndex = trackIndex;

          status_t avail;
          if (!track->mPackets->hasBufferAvailable(&avail)) {
              // sync from other source
              TRESPASS();
              break;
          }

          int64_t timeUs, actualTimeUs;
          const bool formatChange = true;
          sp<AMessage> latestMeta = track->mPackets->getLatestEnqueuedMeta();
          CHECK(latestMeta != NULL && latestMeta->findInt64("timeUs", &timeUs));
          readBuffer(trackType, timeUs, &actualTimeUs, formatChange);
          readBuffer(counterpartType, -1, NULL, formatChange);
          ALOGV("timeUs %lld actualTimeUs %lld", timeUs, actualTimeUs);

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

      case kWhatGetFormat:
      {
          onGetFormatMeta(msg);
          break;
      }

      case kWhatGetSelectedTrack:
      {
          onGetSelectedTrack(msg);
          break;
      }

      case kWhatSelectTrack:
      {
          onSelectTrack(msg);
          break;
      }

      case kWhatSeek:
      {
          onSeek(msg);
          break;
      }

      case kWhatReadBuffer:
      {
          onReadBuffer(msg);
          break;
      }

      default:
          Source::onMessageReceived(msg);
          break;
    }
}

void NuPlayer::GenericSource::fetchTextData(
        uint32_t sendWhat,
        media_track_type type,
        int32_t curGen,
        sp<AnotherPacketSource> packets,
        sp<AMessage> msg) {
    int32_t msgGeneration;
    CHECK(msg->findInt32("generation", &msgGeneration));
    if (msgGeneration != curGen) {
        // stale
        return;
    }

    int32_t avail;
    if (packets->hasBufferAvailable(&avail)) {
        return;
    }

    int64_t timeUs;
    CHECK(msg->findInt64("timeUs", &timeUs));

    int64_t subTimeUs;
    readBuffer(type, timeUs, &subTimeUs);

    int64_t delayUs = subTimeUs - timeUs;
    if (msg->what() == kWhatFetchSubtitleData) {
        const int64_t oneSecUs = 1000000ll;
        delayUs -= oneSecUs;
    }
    sp<AMessage> msg2 = new AMessage(sendWhat, id());
    msg2->setInt32("generation", msgGeneration);
    msg2->post(delayUs < 0 ? 0 : delayUs);
}

void NuPlayer::GenericSource::sendTextData(
        uint32_t what,
        media_track_type type,
        int32_t curGen,
        sp<AnotherPacketSource> packets,
        sp<AMessage> msg) {
    int32_t msgGeneration;
    CHECK(msg->findInt32("generation", &msgGeneration));
    if (msgGeneration != curGen) {
        // stale
        return;
    }

    int64_t subTimeUs;
    if (packets->nextBufferTime(&subTimeUs) != OK) {
        return;
    }

    int64_t nextSubTimeUs;
    readBuffer(type, -1, &nextSubTimeUs);

    sp<ABuffer> buffer;
    status_t dequeueStatus = packets->dequeueAccessUnit(&buffer);
    if (dequeueStatus == OK) {
        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", what);
        notify->setBuffer("buffer", buffer);
        notify->post();

        const int64_t delayUs = nextSubTimeUs - subTimeUs;
        msg->post(delayUs < 0 ? 0 : delayUs);
    }
}

sp<MetaData> NuPlayer::GenericSource::getFormatMeta(bool audio) {
    sp<AMessage> msg = new AMessage(kWhatGetFormat, id());
    msg->setInt32("audio", audio);

    sp<AMessage> response;
    void *format;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findPointer("format", &format));
        return (MetaData *)format;
    } else {
        return NULL;
    }
}

void NuPlayer::GenericSource::onGetFormatMeta(sp<AMessage> msg) const {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    sp<AMessage> response = new AMessage;
    sp<MetaData> format = doGetFormatMeta(audio);
    response->setPointer("format", format.get());

    uint32_t replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

sp<MetaData> NuPlayer::GenericSource::doGetFormatMeta(bool audio) const {
    sp<MediaSource> source = audio ? mAudioTrack.mSource : mVideoTrack.mSource;

    if (source == NULL) {
        return NULL;
    }

    return source->getFormat();
}

status_t NuPlayer::GenericSource::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    Track *track = audio ? &mAudioTrack : &mVideoTrack;

    if (track->mSource == NULL) {
        return -EWOULDBLOCK;
    }

    if (mIsWidevine && !audio) {
        // try to read a buffer as we may not have been able to the last time
        postReadBuffer(MEDIA_TRACK_TYPE_VIDEO);
    }

    status_t finalResult;
    if (!track->mPackets->hasBufferAvailable(&finalResult)) {
        return (finalResult == OK ? -EWOULDBLOCK : finalResult);
    }

    status_t result = track->mPackets->dequeueAccessUnit(accessUnit);

    if (!track->mPackets->hasBufferAvailable(&finalResult)) {
        postReadBuffer(audio? MEDIA_TRACK_TYPE_AUDIO : MEDIA_TRACK_TYPE_VIDEO);
    }

    if (result != OK) {
        if (mSubtitleTrack.mSource != NULL) {
            mSubtitleTrack.mPackets->clear();
            mFetchSubtitleDataGeneration++;
        }
        if (mTimedTextTrack.mSource != NULL) {
            mTimedTextTrack.mPackets->clear();
            mFetchTimedTextDataGeneration++;
        }
        return result;
    }

    int64_t timeUs;
    status_t eosResult; // ignored
    CHECK((*accessUnit)->meta()->findInt64("timeUs", &timeUs));

    if (mSubtitleTrack.mSource != NULL
            && !mSubtitleTrack.mPackets->hasBufferAvailable(&eosResult)) {
        sp<AMessage> msg = new AMessage(kWhatFetchSubtitleData, id());
        msg->setInt64("timeUs", timeUs);
        msg->setInt32("generation", mFetchSubtitleDataGeneration);
        msg->post();
    }

    if (mTimedTextTrack.mSource != NULL
            && !mTimedTextTrack.mPackets->hasBufferAvailable(&eosResult)) {
        sp<AMessage> msg = new AMessage(kWhatFetchTimedTextData, id());
        msg->setInt64("timeUs", timeUs);
        msg->setInt32("generation", mFetchTimedTextDataGeneration);
        msg->post();
    }

    return result;
}

status_t NuPlayer::GenericSource::getDuration(int64_t *durationUs) {
    *durationUs = mDurationUs;
    return OK;
}

size_t NuPlayer::GenericSource::getTrackCount() const {
    return mSources.size();
}

sp<AMessage> NuPlayer::GenericSource::getTrackInfo(size_t trackIndex) const {
    size_t trackCount = mSources.size();
    if (trackIndex >= trackCount) {
        return NULL;
    }

    sp<AMessage> format = new AMessage();
    sp<MetaData> meta = mSources.itemAt(trackIndex)->getFormat();

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    int32_t trackType;
    if (!strncasecmp(mime, "video/", 6)) {
        trackType = MEDIA_TRACK_TYPE_VIDEO;
    } else if (!strncasecmp(mime, "audio/", 6)) {
        trackType = MEDIA_TRACK_TYPE_AUDIO;
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP)) {
        trackType = MEDIA_TRACK_TYPE_TIMEDTEXT;
    } else {
        trackType = MEDIA_TRACK_TYPE_UNKNOWN;
    }
    format->setInt32("type", trackType);

    const char *lang;
    if (!meta->findCString(kKeyMediaLanguage, &lang)) {
        lang = "und";
    }
    format->setString("language", lang);

    if (trackType == MEDIA_TRACK_TYPE_SUBTITLE) {
        format->setString("mime", mime);

        int32_t isAutoselect = 1, isDefault = 0, isForced = 0;
        meta->findInt32(kKeyTrackIsAutoselect, &isAutoselect);
        meta->findInt32(kKeyTrackIsDefault, &isDefault);
        meta->findInt32(kKeyTrackIsForced, &isForced);

        format->setInt32("auto", !!isAutoselect);
        format->setInt32("default", !!isDefault);
        format->setInt32("forced", !!isForced);
    }

    return format;
}

ssize_t NuPlayer::GenericSource::getSelectedTrack(media_track_type type) const {
    sp<AMessage> msg = new AMessage(kWhatGetSelectedTrack, id());
    msg->setInt32("type", type);

    sp<AMessage> response;
    int32_t index;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findInt32("index", &index));
        return index;
    } else {
        return -1;
    }
}

void NuPlayer::GenericSource::onGetSelectedTrack(sp<AMessage> msg) const {
    int32_t tmpType;
    CHECK(msg->findInt32("type", &tmpType));
    media_track_type type = (media_track_type)tmpType;

    sp<AMessage> response = new AMessage;
    ssize_t index = doGetSelectedTrack(type);
    response->setInt32("index", index);

    uint32_t replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

ssize_t NuPlayer::GenericSource::doGetSelectedTrack(media_track_type type) const {
    const Track *track = NULL;
    switch (type) {
    case MEDIA_TRACK_TYPE_VIDEO:
        track = &mVideoTrack;
        break;
    case MEDIA_TRACK_TYPE_AUDIO:
        track = &mAudioTrack;
        break;
    case MEDIA_TRACK_TYPE_TIMEDTEXT:
        track = &mTimedTextTrack;
        break;
    case MEDIA_TRACK_TYPE_SUBTITLE:
        track = &mSubtitleTrack;
        break;
    default:
        break;
    }

    if (track != NULL && track->mSource != NULL) {
        return track->mIndex;
    }

    return -1;
}

status_t NuPlayer::GenericSource::selectTrack(size_t trackIndex, bool select) {
    ALOGV("%s track: %zu", select ? "select" : "deselect", trackIndex);
    sp<AMessage> msg = new AMessage(kWhatSelectTrack, id());
    msg->setInt32("trackIndex", trackIndex);
    msg->setInt32("select", trackIndex);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findInt32("err", &err));
    }

    return err;
}

void NuPlayer::GenericSource::onSelectTrack(sp<AMessage> msg) {
    int32_t trackIndex, select;
    CHECK(msg->findInt32("trackIndex", &trackIndex));
    CHECK(msg->findInt32("select", &select));

    sp<AMessage> response = new AMessage;
    status_t err = doSelectTrack(trackIndex, select);
    response->setInt32("err", err);

    uint32_t replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

status_t NuPlayer::GenericSource::doSelectTrack(size_t trackIndex, bool select) {
    if (trackIndex >= mSources.size()) {
        return BAD_INDEX;
    }

    if (!select) {
        Track* track = NULL;
        if (mSubtitleTrack.mSource != NULL && trackIndex == mSubtitleTrack.mIndex) {
            track = &mSubtitleTrack;
            mFetchSubtitleDataGeneration++;
        } else if (mTimedTextTrack.mSource != NULL && trackIndex == mTimedTextTrack.mIndex) {
            track = &mTimedTextTrack;
            mFetchTimedTextDataGeneration++;
        }
        if (track == NULL) {
            return INVALID_OPERATION;
        }
        track->mSource->stop();
        track->mSource = NULL;
        track->mPackets->clear();
        return OK;
    }

    const sp<MediaSource> source = mSources.itemAt(trackIndex);
    sp<MetaData> meta = source->getFormat();
    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));
    if (!strncasecmp(mime, "text/", 5)) {
        bool isSubtitle = strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP);
        Track *track = isSubtitle ? &mSubtitleTrack : &mTimedTextTrack;
        if (track->mSource != NULL && track->mIndex == trackIndex) {
            return OK;
        }
        track->mIndex = trackIndex;
        if (track->mSource != NULL) {
            track->mSource->stop();
        }
        track->mSource = mSources.itemAt(trackIndex);
        track->mSource->start();
        if (track->mPackets == NULL) {
            track->mPackets = new AnotherPacketSource(track->mSource->getFormat());
        } else {
            track->mPackets->clear();
            track->mPackets->setFormat(track->mSource->getFormat());

        }

        if (isSubtitle) {
            mFetchSubtitleDataGeneration++;
        } else {
            mFetchTimedTextDataGeneration++;
        }

        return OK;
    } else if (!strncasecmp(mime, "audio/", 6) || !strncasecmp(mime, "video/", 6)) {
        bool audio = !strncasecmp(mime, "audio/", 6);
        Track *track = audio ? &mAudioTrack : &mVideoTrack;
        if (track->mSource != NULL && track->mIndex == trackIndex) {
            return OK;
        }

        sp<AMessage> msg = new AMessage(kWhatChangeAVSource, id());
        msg->setInt32("trackIndex", trackIndex);
        msg->post();
        return OK;
    }

    return INVALID_OPERATION;
}

status_t NuPlayer::GenericSource::seekTo(int64_t seekTimeUs) {
    sp<AMessage> msg = new AMessage(kWhatSeek, id());
    msg->setInt64("seekTimeUs", seekTimeUs);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findInt32("err", &err));
    }

    return err;
}

void NuPlayer::GenericSource::onSeek(sp<AMessage> msg) {
    int64_t seekTimeUs;
    CHECK(msg->findInt64("seekTimeUs", &seekTimeUs));

    sp<AMessage> response = new AMessage;
    status_t err = doSeek(seekTimeUs);
    response->setInt32("err", err);

    uint32_t replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

status_t NuPlayer::GenericSource::doSeek(int64_t seekTimeUs) {
    if (mVideoTrack.mSource != NULL) {
        int64_t actualTimeUs;
        readBuffer(MEDIA_TRACK_TYPE_VIDEO, seekTimeUs, &actualTimeUs);

        seekTimeUs = actualTimeUs;
    }

    if (mAudioTrack.mSource != NULL) {
        readBuffer(MEDIA_TRACK_TYPE_AUDIO, seekTimeUs);
    }

    setDrmPlaybackStatusIfNeeded(Playback::START, seekTimeUs / 1000);
    if (!mStarted) {
        setDrmPlaybackStatusIfNeeded(Playback::PAUSE, 0);
    }
    return OK;
}

sp<ABuffer> NuPlayer::GenericSource::mediaBufferToABuffer(
        MediaBuffer* mb,
        media_track_type trackType,
        int64_t *actualTimeUs) {
    bool audio = trackType == MEDIA_TRACK_TYPE_AUDIO;
    size_t outLength = mb->range_length();

    if (audio && mAudioIsVorbis) {
        outLength += sizeof(int32_t);
    }

    sp<ABuffer> ab;
    if (mIsWidevine && !audio) {
        // data is already provided in the buffer
        ab = new ABuffer(NULL, mb->range_length());
        ab->meta()->setPointer("mediaBuffer", mb);
        mb->add_ref();
    } else {
        ab = new ABuffer(outLength);
        memcpy(ab->data(),
               (const uint8_t *)mb->data() + mb->range_offset(),
               mb->range_length());
    }

    if (audio && mAudioIsVorbis) {
        int32_t numPageSamples;
        if (!mb->meta_data()->findInt32(kKeyValidSamples, &numPageSamples)) {
            numPageSamples = -1;
        }

        uint8_t* abEnd = ab->data() + mb->range_length();
        memcpy(abEnd, &numPageSamples, sizeof(numPageSamples));
    }

    sp<AMessage> meta = ab->meta();

    int64_t timeUs;
    CHECK(mb->meta_data()->findInt64(kKeyTime, &timeUs));
    meta->setInt64("timeUs", timeUs);

    if (trackType == MEDIA_TRACK_TYPE_TIMEDTEXT) {
        const char *mime;
        CHECK(mTimedTextTrack.mSource != NULL
                && mTimedTextTrack.mSource->getFormat()->findCString(kKeyMIMEType, &mime));
        meta->setString("mime", mime);
    }

    int64_t durationUs;
    if (mb->meta_data()->findInt64(kKeyDuration, &durationUs)) {
        meta->setInt64("durationUs", durationUs);
    }

    if (trackType == MEDIA_TRACK_TYPE_SUBTITLE) {
        meta->setInt32("trackIndex", mSubtitleTrack.mIndex);
    }

    if (actualTimeUs) {
        *actualTimeUs = timeUs;
    }

    mb->release();
    mb = NULL;

    return ab;
}

void NuPlayer::GenericSource::postReadBuffer(media_track_type trackType) {
    Mutex::Autolock _l(mReadBufferLock);

    if ((mPendingReadBufferTypes & (1 << trackType)) == 0) {
        mPendingReadBufferTypes |= (1 << trackType);
        sp<AMessage> msg = new AMessage(kWhatReadBuffer, id());
        msg->setInt32("trackType", trackType);
        msg->post();
    }
}

void NuPlayer::GenericSource::onReadBuffer(sp<AMessage> msg) {
    int32_t tmpType;
    CHECK(msg->findInt32("trackType", &tmpType));
    media_track_type trackType = (media_track_type)tmpType;
    {
        // only protect the variable change, as readBuffer may
        // take considerable time.  This may result in one extra
        // read being processed, but that is benign.
        Mutex::Autolock _l(mReadBufferLock);
        mPendingReadBufferTypes &= ~(1 << trackType);
    }
    readBuffer(trackType);
}

void NuPlayer::GenericSource::readBuffer(
        media_track_type trackType, int64_t seekTimeUs, int64_t *actualTimeUs, bool formatChange) {
    Track *track;
    switch (trackType) {
        case MEDIA_TRACK_TYPE_VIDEO:
            track = &mVideoTrack;
            break;
        case MEDIA_TRACK_TYPE_AUDIO:
            track = &mAudioTrack;
            break;
        case MEDIA_TRACK_TYPE_SUBTITLE:
            track = &mSubtitleTrack;
            break;
        case MEDIA_TRACK_TYPE_TIMEDTEXT:
            track = &mTimedTextTrack;
            break;
        default:
            TRESPASS();
    }

    if (track->mSource == NULL) {
        return;
    }

    if (actualTimeUs) {
        *actualTimeUs = seekTimeUs;
    }

    MediaSource::ReadOptions options;

    bool seeking = false;

    if (seekTimeUs >= 0) {
        options.setSeekTo(seekTimeUs, MediaSource::ReadOptions::SEEK_PREVIOUS_SYNC);
        seeking = true;
    }

    if (mIsWidevine && trackType != MEDIA_TRACK_TYPE_AUDIO) {
        options.setNonBlocking();
    }

    for (;;) {
        MediaBuffer *mbuf;
        status_t err = track->mSource->read(&mbuf, &options);

        options.clearSeekTo();

        if (err == OK) {
            int64_t timeUs;
            CHECK(mbuf->meta_data()->findInt64(kKeyTime, &timeUs));
            if (trackType == MEDIA_TRACK_TYPE_AUDIO) {
                mAudioTimeUs = timeUs;
            } else if (trackType == MEDIA_TRACK_TYPE_VIDEO) {
                mVideoTimeUs = timeUs;
            }

            // formatChange && seeking: track whose source is changed during selection
            // formatChange && !seeking: track whose source is not changed during selection
            // !formatChange: normal seek
            if ((seeking || formatChange)
                    && (trackType == MEDIA_TRACK_TYPE_AUDIO
                    || trackType == MEDIA_TRACK_TYPE_VIDEO)) {
                ATSParser::DiscontinuityType type = formatChange
                        ? (seeking
                                ? ATSParser::DISCONTINUITY_FORMATCHANGE
                                : ATSParser::DISCONTINUITY_NONE)
                        : ATSParser::DISCONTINUITY_SEEK;
                track->mPackets->queueDiscontinuity( type, NULL, true /* discard */);
            }

            sp<ABuffer> buffer = mediaBufferToABuffer(mbuf, trackType, actualTimeUs);
            track->mPackets->queueAccessUnit(buffer);
            break;
        } else if (err == WOULD_BLOCK) {
            break;
        } else if (err == INFO_FORMAT_CHANGED) {
#if 0
            track->mPackets->queueDiscontinuity(
                    ATSParser::DISCONTINUITY_FORMATCHANGE,
                    NULL,
                    false /* discard */);
#endif
        } else {
            track->mPackets->signalEOS(err);
            break;
        }
    }
}

}  // namespace android
