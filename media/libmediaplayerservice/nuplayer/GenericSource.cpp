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
#include "../../libstagefright/include/HTTPBase.h"
#include "mediaplayerservice/AVNuExtensions.h"

namespace android {

static int64_t kLowWaterMarkUs = 2000000ll;  // 2secs
static int64_t kHighWaterMarkUs = 5000000ll;  // 5secs
static int64_t kHighWaterMarkRebufferUs = 15000000ll;  // 15secs
static const ssize_t kLowWaterMarkBytes = 40000;
static const ssize_t kHighWaterMarkBytes = 200000;

NuPlayer::GenericSource::GenericSource(
        const sp<AMessage> &notify,
        bool uidValid,
        uid_t uid)
    : Source(notify),
      mAudioTimeUs(0),
      mAudioLastDequeueTimeUs(0),
      mVideoTimeUs(0),
      mVideoLastDequeueTimeUs(0),
      mFetchSubtitleDataGeneration(0),
      mFetchTimedTextDataGeneration(0),
      mDurationUs(-1ll),
      mAudioIsVorbis(false),
      mIsByteMode(false),
      mIsWidevine(false),
      mIsSecure(false),
      mIsStreaming(false),
      mUIDValid(uidValid),
      mUID(uid),
      mFd(-1),
      mDrmManagerClient(NULL),
      mBitrate(-1ll),
      mPendingReadBufferTypes(0) {
    mBufferingMonitor = new BufferingMonitor(notify);
    resetDataSource();
    DataSource::RegisterDefaultSniffers();
}

void NuPlayer::GenericSource::resetDataSource() {
    mHTTPService.clear();
    mHttpSource.clear();
    mUri.clear();
    mUriHeaders.clear();
    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }
    mOffset = 0;
    mLength = 0;
    setDrmPlaybackStatusIfNeeded(Playback::STOP, 0);
    mDecryptHandle = NULL;
    mDrmManagerClient = NULL;
    mStarted = false;
    mStopRead = true;

    if (mBufferingMonitorLooper != NULL) {
        mBufferingMonitorLooper->unregisterHandler(mBufferingMonitor->id());
        mBufferingMonitorLooper->stop();
        mBufferingMonitorLooper = NULL;
    }
    mBufferingMonitor->stop();
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

status_t NuPlayer::GenericSource::setDataSource(const sp<DataSource>& source) {
    resetDataSource();
    mDataSource = source;
    return OK;
}

sp<MetaData> NuPlayer::GenericSource::getFileFormatMeta() const {
    return mFileMeta;
}

status_t NuPlayer::GenericSource::initFromDataSource() {
    sp<IMediaExtractor> extractor;
    String8 mimeType;
    float confidence;
    sp<AMessage> dummy;
    bool isWidevineStreaming = false;

    CHECK(mDataSource != NULL);

    if (mIsWidevine) {
        isWidevineStreaming = SniffWVM(
                mDataSource, &mimeType, &confidence, &dummy);
        if (!isWidevineStreaming ||
                strcasecmp(
                    mimeType.string(), MEDIA_MIMETYPE_CONTAINER_WVM)) {
            ALOGE("unsupported widevine mime: %s", mimeType.string());
            return UNKNOWN_ERROR;
        }
    } else if (mIsStreaming) {
        if (!mDataSource->sniff(&mimeType, &confidence, &dummy)) {
            return UNKNOWN_ERROR;
        }
        isWidevineStreaming = !strcasecmp(
                mimeType.string(), MEDIA_MIMETYPE_CONTAINER_WVM);
    }

    if (isWidevineStreaming) {
        // we don't want cached source for widevine streaming.
        mCachedSource.clear();
        mDataSource = mHttpSource;
        mWVMExtractor = new WVMExtractor(mDataSource);
        mWVMExtractor->setAdaptiveStreamingMode(true);
        if (mUIDValid) {
            mWVMExtractor->setUID(mUID);
        }
        extractor = mWVMExtractor;
    } else {
        extractor = MediaExtractor::Create(mDataSource,
                mimeType.isEmpty() ? NULL : mimeType.string());
    }

    if (extractor == NULL) {
        return UNKNOWN_ERROR;
    }

    if (extractor->getDrmFlag()) {
        checkDrmStatus(mDataSource);
    }

    mFileMeta = extractor->getMetaData();
    if (mFileMeta != NULL) {
        int64_t duration;
        if (mFileMeta->findInt64(kKeyDuration, &duration)) {
            mDurationUs = duration;
        }

        if (!mIsWidevine) {
            // Check mime to see if we actually have a widevine source.
            // If the data source is not URL-type (eg. file source), we
            // won't be able to tell until now.
            const char *fileMime;
            if (mFileMeta->findCString(kKeyMIMEType, &fileMime)
                    && !strncasecmp(fileMime, "video/wvm", 9)) {
                mIsWidevine = true;
            }
        }
    }

    int32_t totalBitrate = 0;

    size_t numtracks = extractor->countTracks();
    if (numtracks == 0) {
        return UNKNOWN_ERROR;
    }

    for (size_t i = 0; i < numtracks; ++i) {
        sp<IMediaSource> track = extractor->getTrack(i);
        if (track == NULL) {
            continue;
        }

        sp<MetaData> meta = extractor->getTrackMetaData(i);
        if (meta == NULL) {
            ALOGE("no metadata for track %zu", i);
            return UNKNOWN_ERROR;
        }

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

                if (AVNuUtils::get()->isByteStreamModeEnabled(meta)) {
                    mIsByteMode = true;
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
                    mIsSecure = true;
                    if (mUIDValid) {
                        extractor->setUID(mUID);
                    }
                }
            }
        }

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

    if (mSources.size() == 0) {
        ALOGE("b/23705695");
        return UNKNOWN_ERROR;
    }

    mBitrate = totalBitrate;

    return OK;
}

status_t NuPlayer::GenericSource::startSources() {
    // Start the selected A/V tracks now before we start buffering.
    // Widevine sources might re-initialize crypto when starting, if we delay
    // this to start(), all data buffered during prepare would be wasted.
    // (We don't actually start reading until start().)
    if (mAudioTrack.mSource != NULL && mAudioTrack.mSource->start() != OK) {
        ALOGE("failed to start audio track!");
        return UNKNOWN_ERROR;
    }

    if (mVideoTrack.mSource != NULL && mVideoTrack.mSource->start() != OK) {
        ALOGE("failed to start video track!");
        return UNKNOWN_ERROR;
    }

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
    if (mIsSecure && !audio && mVideoTrack.mSource != NULL) {
        return mVideoTrack.mSource->setBuffers(buffers);
    }
    return INVALID_OPERATION;
}

bool NuPlayer::GenericSource::isStreaming() const {
    return mIsStreaming;
}

void NuPlayer::GenericSource::setOffloadAudio(bool offload) {
    mBufferingMonitor->setOffloadAudio(offload);
}

NuPlayer::GenericSource::~GenericSource() {
    if (mLooper != NULL) {
        mLooper->unregisterHandler(id());
        mLooper->stop();
    }
    resetDataSource();
}

void NuPlayer::GenericSource::prepareAsync() {
    if (mLooper == NULL) {
        mLooper = new ALooper;
        mLooper->setName("generic");
        mLooper->start();

        mLooper->registerHandler(this);
    }

    sp<AMessage> msg = new AMessage(kWhatPrepareAsync, this);
    msg->post();
}

void NuPlayer::GenericSource::onPrepareAsync() {
    // delayed data source creation
    if (mDataSource == NULL) {
        // set to false first, if the extractor
        // comes back as secure, set it to true then.
        mIsSecure = false;

        if (!mUri.empty()) {
            const char* uri = mUri.c_str();
            String8 contentType;
            mIsWidevine = !strncasecmp(uri, "widevine://", 11);

            if (!strncasecmp("http://", uri, 7)
                    || !strncasecmp("https://", uri, 8)
                    || mIsWidevine) {
                mHttpSource = DataSource::CreateMediaHTTP(mHTTPService);
                if (mHttpSource == NULL) {
                    ALOGE("Failed to create http source!");
                    notifyPreparedAndCleanup(UNKNOWN_ERROR);
                    return;
                }
            }

            mDataSource = DataSource::CreateFromURI(
                   mHTTPService, uri, &mUriHeaders, &contentType,
                   static_cast<HTTPBase *>(mHttpSource.get()));
        } else {
            mIsWidevine = false;

            mDataSource = new FileSource(mFd, mOffset, mLength);
            mFd = -1;
        }

        if (mDataSource == NULL) {
            ALOGE("Failed to create data source!");
            notifyPreparedAndCleanup(UNKNOWN_ERROR);
            return;
        }
    }

    if (mDataSource->flags() & DataSource::kIsCachingDataSource) {
        mCachedSource = static_cast<NuCachedSource2 *>(mDataSource.get());
    }

    // For widevine or other cached streaming cases, we need to wait for
    // enough buffering before reporting prepared.
    // Note that even when URL doesn't start with widevine://, mIsWidevine
    // could still be set to true later, if the streaming or file source
    // is sniffed to be widevine. We don't want to buffer for file source
    // in that case, so must check the flag now.
    mIsStreaming = (mIsWidevine || mCachedSource != NULL);

    // init extractor from data source
    status_t err = initFromDataSource();

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
            (mIsSecure ? FLAG_SECURE : 0)
            | (mDecryptHandle != NULL ? FLAG_PROTECTED : 0)
            | FLAG_CAN_PAUSE
            | FLAG_CAN_SEEK_BACKWARD
            | FLAG_CAN_SEEK_FORWARD
            | FLAG_CAN_SEEK);

    if (mIsSecure) {
        // secure decoders must be instantiated before starting widevine source
        sp<AMessage> reply = new AMessage(kWhatSecureDecodersInstantiated, this);
        notifyInstantiateSecureDecoders(reply);
    } else {
        finishPrepareAsync();
    }
}

void NuPlayer::GenericSource::onSecureDecodersInstantiated(status_t err) {
    if (err != OK) {
        ALOGE("Failed to instantiate secure decoders!");
        notifyPreparedAndCleanup(err);
        return;
    }
    finishPrepareAsync();
}

void NuPlayer::GenericSource::finishPrepareAsync() {
    status_t err = startSources();
    if (err != OK) {
        ALOGE("Failed to init start data source!");
        notifyPreparedAndCleanup(err);
        return;
    }

    if (mIsStreaming) {
        if (mBufferingMonitorLooper == NULL) {
            mBufferingMonitor->prepare(mCachedSource, mWVMExtractor, mDurationUs, mBitrate,
                    mIsStreaming);

            mBufferingMonitorLooper = new ALooper;
            mBufferingMonitorLooper->setName("GSBMonitor");
            mBufferingMonitorLooper->start();
            mBufferingMonitorLooper->registerHandler(mBufferingMonitor);
        }

        mBufferingMonitor->ensureCacheIsFetching();
        mBufferingMonitor->restartPollBuffering();
    } else {
        notifyPrepared();
    }
}

void NuPlayer::GenericSource::notifyPreparedAndCleanup(status_t err) {
    if (err != OK) {
        {
            sp<DataSource> dataSource = mDataSource;
            sp<NuCachedSource2> cachedSource = mCachedSource;
            sp<DataSource> httpSource = mHttpSource;
            {
                Mutex::Autolock _l(mDisconnectLock);
                mDataSource.clear();
                mDecryptHandle = NULL;
                mDrmManagerClient = NULL;
                mCachedSource.clear();
                mHttpSource.clear();
            }
        }
        mBitrate = -1;

        mBufferingMonitor->cancelPollBuffering();
    }
    notifyPrepared(err);
}

void NuPlayer::GenericSource::start() {
    ALOGI("start");

    mStopRead = false;
    if (mAudioTrack.mSource != NULL) {
        postReadBuffer(MEDIA_TRACK_TYPE_AUDIO);
    }

    if (mVideoTrack.mSource != NULL) {
        postReadBuffer(MEDIA_TRACK_TYPE_VIDEO);
    }

    setDrmPlaybackStatusIfNeeded(Playback::START, getLastReadPosition() / 1000);
    mStarted = true;

    (new AMessage(kWhatStart, this))->post();
}

void NuPlayer::GenericSource::stop() {
    // nothing to do, just account for DRM playback status
    setDrmPlaybackStatusIfNeeded(Playback::STOP, 0);
    mStarted = false;
    if (mIsWidevine || mIsSecure) {
        // For widevine or secure sources we need to prevent any further reads.
        sp<AMessage> msg = new AMessage(kWhatStopWidevine, this);
        sp<AMessage> response;
        (void) msg->postAndAwaitResponse(&response);
    }
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

    (new AMessage(kWhatResume, this))->post();
}

void NuPlayer::GenericSource::disconnect() {
    sp<DataSource> dataSource, httpSource;
    {
        Mutex::Autolock _l(mDisconnectLock);
        dataSource = mDataSource;
        httpSource = mHttpSource;
    }

    if (dataSource != NULL) {
        // disconnect data source
        if (dataSource->flags() & DataSource::kIsCachingDataSource) {
            static_cast<NuCachedSource2 *>(dataSource.get())->disconnect();
        }
    } else if (httpSource != NULL) {
        static_cast<HTTPBase *>(httpSource.get())->disconnect();
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

      case kWhatSendGlobalTimedTextData:
      {
          sendGlobalTextData(kWhatTimedTextData, mFetchTimedTextDataGeneration, msg);
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
          const sp<IMediaSource> source = mSources.itemAt(trackIndex);

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

          int64_t timeUs, actualTimeUs;
          const bool formatChange = true;
          if (trackType == MEDIA_TRACK_TYPE_AUDIO) {
              timeUs = mAudioLastDequeueTimeUs;
          } else {
              timeUs = mVideoLastDequeueTimeUs;
          }
          readBuffer(trackType, timeUs, &actualTimeUs, formatChange);
          readBuffer(counterpartType, -1, NULL, formatChange);
          ALOGV("timeUs %lld actualTimeUs %lld", (long long)timeUs, (long long)actualTimeUs);

          break;
      }

      case kWhatStart:
      case kWhatResume:
      {
          mBufferingMonitor->restartPollBuffering();
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

      case kWhatSecureDecodersInstantiated:
      {
          int32_t err;
          CHECK(msg->findInt32("err", &err));
          onSecureDecodersInstantiated(err);
          break;
      }

      case kWhatStopWidevine:
      {
          // mStopRead is only used for Widevine to prevent the video source
          // from being read while the associated video decoder is shutting down.
          mStopRead = true;
          if (mVideoTrack.mSource != NULL) {
              mVideoTrack.mPackets->clear();
          }
          sp<AMessage> response = new AMessage;
          sp<AReplyToken> replyID;
          CHECK(msg->senderAwaitsResponse(&replyID));
          response->postReply(replyID);
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
    sp<AMessage> msg2 = new AMessage(sendWhat, this);
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

void NuPlayer::GenericSource::sendGlobalTextData(
        uint32_t what,
        int32_t curGen,
        sp<AMessage> msg) {
    int32_t msgGeneration;
    CHECK(msg->findInt32("generation", &msgGeneration));
    if (msgGeneration != curGen) {
        // stale
        return;
    }

    uint32_t textType;
    const void *data;
    size_t size = 0;
    if (mTimedTextTrack.mSource->getFormat()->findData(
                    kKeyTextFormatData, &textType, &data, &size)) {
        mGlobalTimedText = new ABuffer(size);
        if (mGlobalTimedText->data()) {
            memcpy(mGlobalTimedText->data(), data, size);
            sp<AMessage> globalMeta = mGlobalTimedText->meta();
            globalMeta->setInt64("timeUs", 0);
            globalMeta->setString("mime", MEDIA_MIMETYPE_TEXT_3GPP);
            globalMeta->setInt32("global", 1);
            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", what);
            notify->setBuffer("buffer", mGlobalTimedText);
            notify->post();
        }
    }
}

sp<MetaData> NuPlayer::GenericSource::getFormatMeta(bool audio) {
    sp<AMessage> msg = new AMessage(kWhatGetFormat, this);
    msg->setInt32("audio", audio);

    sp<AMessage> response;
    sp<RefBase> format;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findObject("format", &format));
        return static_cast<MetaData*>(format.get());
    } else {
        return NULL;
    }
}

void NuPlayer::GenericSource::onGetFormatMeta(sp<AMessage> msg) const {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    sp<AMessage> response = new AMessage;
    sp<MetaData> format = doGetFormatMeta(audio);
    response->setObject("format", format);

    sp<AReplyToken> replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

sp<MetaData> NuPlayer::GenericSource::doGetFormatMeta(bool audio) const {
    sp<IMediaSource> source = audio ? mAudioTrack.mSource : mVideoTrack.mSource;

    if (source == NULL) {
        return NULL;
    }

    return source->getFormat();
}

status_t NuPlayer::GenericSource::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    if (audio && !mStarted) {
        return -EWOULDBLOCK;
    }

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
        if (finalResult == OK) {
            postReadBuffer(
                    audio ? MEDIA_TRACK_TYPE_AUDIO : MEDIA_TRACK_TYPE_VIDEO);
            return -EWOULDBLOCK;
        }
        return finalResult;
    }

    status_t result = track->mPackets->dequeueAccessUnit(accessUnit);

    // start pulling in more buffers if we only have one (or no) buffer left
    // so that decoder has less chance of being starved
    if (track->mPackets->getAvailableBufferCount(&finalResult) < 2) {
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
    if (audio) {
        mAudioLastDequeueTimeUs = timeUs;
        mBufferingMonitor->updateDequeuedBufferTime(timeUs);
    } else {
        mVideoLastDequeueTimeUs = timeUs;
    }

    if (mSubtitleTrack.mSource != NULL
            && !mSubtitleTrack.mPackets->hasBufferAvailable(&eosResult)) {
        sp<AMessage> msg = new AMessage(kWhatFetchSubtitleData, this);
        msg->setInt64("timeUs", timeUs);
        msg->setInt32("generation", mFetchSubtitleDataGeneration);
        msg->post();
    }

    if (mTimedTextTrack.mSource != NULL
            && !mTimedTextTrack.mPackets->hasBufferAvailable(&eosResult)) {
        sp<AMessage> msg = new AMessage(kWhatFetchTimedTextData, this);
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
    if (meta == NULL) {
        ALOGE("no metadata for track %zu", trackIndex);
        return NULL;
    }

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));
    format->setString("mime", mime);

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
    sp<AMessage> msg = new AMessage(kWhatGetSelectedTrack, this);
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

    sp<AReplyToken> replyID;
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

status_t NuPlayer::GenericSource::selectTrack(size_t trackIndex, bool select, int64_t timeUs) {
    ALOGV("%s track: %zu", select ? "select" : "deselect", trackIndex);
    sp<AMessage> msg = new AMessage(kWhatSelectTrack, this);
    msg->setInt32("trackIndex", trackIndex);
    msg->setInt32("select", select);
    msg->setInt64("timeUs", timeUs);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);
    if (err == OK && response != NULL) {
        CHECK(response->findInt32("err", &err));
    }

    return err;
}

void NuPlayer::GenericSource::onSelectTrack(sp<AMessage> msg) {
    int32_t trackIndex, select;
    int64_t timeUs;
    CHECK(msg->findInt32("trackIndex", &trackIndex));
    CHECK(msg->findInt32("select", &select));
    CHECK(msg->findInt64("timeUs", &timeUs));

    sp<AMessage> response = new AMessage;
    status_t err = doSelectTrack(trackIndex, select, timeUs);
    response->setInt32("err", err);

    sp<AReplyToken> replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

status_t NuPlayer::GenericSource::doSelectTrack(size_t trackIndex, bool select, int64_t timeUs) {
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

    const sp<IMediaSource> source = mSources.itemAt(trackIndex);
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

        status_t eosResult; // ignored
        if (mSubtitleTrack.mSource != NULL
                && !mSubtitleTrack.mPackets->hasBufferAvailable(&eosResult)) {
            sp<AMessage> msg = new AMessage(kWhatFetchSubtitleData, this);
            msg->setInt64("timeUs", timeUs);
            msg->setInt32("generation", mFetchSubtitleDataGeneration);
            msg->post();
        }

        sp<AMessage> msg2 = new AMessage(kWhatSendGlobalTimedTextData, this);
        msg2->setInt32("generation", mFetchTimedTextDataGeneration);
        msg2->post();

        if (mTimedTextTrack.mSource != NULL
                && !mTimedTextTrack.mPackets->hasBufferAvailable(&eosResult)) {
            sp<AMessage> msg = new AMessage(kWhatFetchTimedTextData, this);
            msg->setInt64("timeUs", timeUs);
            msg->setInt32("generation", mFetchTimedTextDataGeneration);
            msg->post();
        }

        return OK;
    } else if (!strncasecmp(mime, "audio/", 6) || !strncasecmp(mime, "video/", 6)) {
        bool audio = !strncasecmp(mime, "audio/", 6);
        Track *track = audio ? &mAudioTrack : &mVideoTrack;
        if (track->mSource != NULL && track->mIndex == trackIndex) {
            return OK;
        }

        sp<AMessage> msg = new AMessage(kWhatChangeAVSource, this);
        msg->setInt32("trackIndex", trackIndex);
        msg->post();
        return OK;
    }

    return INVALID_OPERATION;
}

status_t NuPlayer::GenericSource::seekTo(int64_t seekTimeUs) {
    sp<AMessage> msg = new AMessage(kWhatSeek, this);
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

    sp<AReplyToken> replyID;
    CHECK(msg->senderAwaitsResponse(&replyID));
    response->postReply(replyID);
}

status_t NuPlayer::GenericSource::doSeek(int64_t seekTimeUs) {
    mBufferingMonitor->updateDequeuedBufferTime(-1ll);

    // If the Widevine source is stopped, do not attempt to read any
    // more buffers.
    if (mStopRead) {
        return INVALID_OPERATION;
    }
    if (mVideoTrack.mSource != NULL) {
        int64_t actualTimeUs;
        readBuffer(MEDIA_TRACK_TYPE_VIDEO, seekTimeUs, &actualTimeUs);

        seekTimeUs = actualTimeUs;
        mVideoLastDequeueTimeUs = seekTimeUs;
    }

    if (mAudioTrack.mSource != NULL) {
        readBuffer(MEDIA_TRACK_TYPE_AUDIO, seekTimeUs);
        mAudioLastDequeueTimeUs = seekTimeUs;
    }

    setDrmPlaybackStatusIfNeeded(Playback::START, seekTimeUs / 1000);
    if (!mStarted) {
        setDrmPlaybackStatusIfNeeded(Playback::PAUSE, 0);
    }

    // If currently buffering, post kWhatBufferingEnd first, so that
    // NuPlayer resumes. Otherwise, if cache hits high watermark
    // before new polling happens, no one will resume the playback.
    mBufferingMonitor->stopBufferingIfNecessary();
    mBufferingMonitor->restartPollBuffering();

    return OK;
}

sp<ABuffer> NuPlayer::GenericSource::mediaBufferToABuffer(
        MediaBuffer* mb,
        media_track_type trackType,
        int64_t /* seekTimeUs */,
        int64_t *actualTimeUs) {
    bool audio = trackType == MEDIA_TRACK_TYPE_AUDIO;
    size_t outLength = mb->range_length();

    if (audio && mAudioIsVorbis) {
        outLength += sizeof(int32_t);
    }

    sp<ABuffer> ab;
    if (mIsSecure && !audio) {
        // data is already provided in the buffer
        ab = new ABuffer(NULL, mb->range_length());
        mb->add_ref();
        ab->setMediaBufferBase(mb);
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

#if 0
    // Temporarily disable pre-roll till we have a full solution to handle
    // both single seek and continous seek gracefully.
    if (seekTimeUs > timeUs) {
        sp<AMessage> extra = new AMessage;
        extra->setInt64("resume-at-mediaTimeUs", seekTimeUs);
        meta->setMessage("extra", extra);
    }
#endif

    if (trackType == MEDIA_TRACK_TYPE_VIDEO) {
        int32_t layerId;
        if (mb->meta_data()->findInt32(kKeyTemporalLayerId, &layerId)) {
            meta->setInt32("temporal-layer-id", layerId);
        }
    }

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

    uint32_t dataType; // unused
    const void *seiData;
    size_t seiLength;
    if (mb->meta_data()->findData(kKeySEI, &dataType, &seiData, &seiLength)) {
        sp<ABuffer> sei = ABuffer::CreateAsCopy(seiData, seiLength);;
        meta->setBuffer("sei", sei);
    }

    const void *mpegUserDataPointer;
    size_t mpegUserDataLength;
    if (mb->meta_data()->findData(
            kKeyMpegUserData, &dataType, &mpegUserDataPointer, &mpegUserDataLength)) {
        sp<ABuffer> mpegUserData = ABuffer::CreateAsCopy(mpegUserDataPointer, mpegUserDataLength);
        meta->setBuffer("mpegUserData", mpegUserData);
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
        sp<AMessage> msg = new AMessage(kWhatReadBuffer, this);
        msg->setInt32("trackType", trackType);
        msg->post();
    }
}

void NuPlayer::GenericSource::onReadBuffer(sp<AMessage> msg) {
    int32_t tmpType;
    CHECK(msg->findInt32("trackType", &tmpType));
    media_track_type trackType = (media_track_type)tmpType;
    readBuffer(trackType);
    {
        // only protect the variable change, as readBuffer may
        // take considerable time.
        Mutex::Autolock _l(mReadBufferLock);
        mPendingReadBufferTypes &= ~(1 << trackType);
    }
}

void NuPlayer::GenericSource::readBuffer(
        media_track_type trackType, int64_t seekTimeUs, int64_t *actualTimeUs, bool formatChange) {
    // Do not read data if Widevine source is stopped
    if (mStopRead) {
        return;
    }
    Track *track;
    size_t maxBuffers = 1;
    switch (trackType) {
        case MEDIA_TRACK_TYPE_VIDEO:
            track = &mVideoTrack;
            if (mIsWidevine) {
                maxBuffers = 2;
            } else {
                maxBuffers = 4;
            }
            break;
        case MEDIA_TRACK_TYPE_AUDIO:
            track = &mAudioTrack;
            if (mIsWidevine) {
                maxBuffers = 8;
            } else if (mIsByteMode) {
                // byte stream mode is enabled only for mp3 & aac
                // and the parser gives a huge chunk of data per read,
                // so reading one buffer is sufficient.
                maxBuffers = 1;
            } else {
                maxBuffers = 64;
            }
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

    if (mIsWidevine) {
        options.setNonBlocking();
    }

    bool couldReadMultiple = (!mIsWidevine && trackType == MEDIA_TRACK_TYPE_AUDIO);
    for (size_t numBuffers = 0; numBuffers < maxBuffers; ) {
        Vector<MediaBuffer *> mediaBuffers;
        status_t err = NO_ERROR;

        if (!seeking && couldReadMultiple) {
            err = track->mSource->readMultiple(&mediaBuffers, (maxBuffers - numBuffers));
        } else {
            MediaBuffer *mbuf = NULL;
            err = track->mSource->read(&mbuf, &options);
            if (err == OK && mbuf != NULL) {
                mediaBuffers.push_back(mbuf);
            }
        }

        options.clearSeekTo();

        size_t id = 0;
        size_t count = mediaBuffers.size();
        for (; id < count; ++id) {
            int64_t timeUs;
            MediaBuffer *mbuf = mediaBuffers[id];
            if (!mbuf->meta_data()->findInt64(kKeyTime, &timeUs)) {
                mbuf->meta_data()->dumpToLog();
                track->mPackets->signalEOS(ERROR_MALFORMED);
                break;
            }
            if (trackType == MEDIA_TRACK_TYPE_AUDIO) {
                mAudioTimeUs = timeUs;
                mBufferingMonitor->updateQueuedTime(true /* isAudio */, timeUs);
            } else if (trackType == MEDIA_TRACK_TYPE_VIDEO) {
                mVideoTimeUs = timeUs;
                mBufferingMonitor->updateQueuedTime(false /* isAudio */, timeUs);
            }

            queueDiscontinuityIfNeeded(seeking, formatChange, trackType, track);

            sp<ABuffer> buffer = mediaBufferToABuffer(
                    mbuf, trackType, seekTimeUs,
                    numBuffers == 0 ? actualTimeUs : NULL);
            track->mPackets->queueAccessUnit(buffer);
            formatChange = false;
            seeking = false;
            ++numBuffers;
        }
        if (id < count) {
            // Error, some mediaBuffer doesn't have kKeyTime.
            for (; id < count; ++id) {
                mediaBuffers[id]->release();
            }
            break;
        }

        if (err == WOULD_BLOCK) {
            break;
        } else if (err == INFO_FORMAT_CHANGED) {
#if 0
            track->mPackets->queueDiscontinuity(
                    ATSParser::DISCONTINUITY_FORMATCHANGE,
                    NULL,
                    false /* discard */);
#endif
        } else if (err != OK) {
            queueDiscontinuityIfNeeded(seeking, formatChange, trackType, track);
            track->mPackets->signalEOS(err);
            break;
        }
    }
}

void NuPlayer::GenericSource::queueDiscontinuityIfNeeded(
        bool seeking, bool formatChange, media_track_type trackType, Track *track) {
    // formatChange && seeking: track whose source is changed during selection
    // formatChange && !seeking: track whose source is not changed during selection
    // !formatChange: normal seek
    if ((seeking || formatChange)
            && (trackType == MEDIA_TRACK_TYPE_AUDIO
            || trackType == MEDIA_TRACK_TYPE_VIDEO)) {
        ATSParser::DiscontinuityType type = (formatChange && seeking)
                ? ATSParser::DISCONTINUITY_FORMATCHANGE
                : ATSParser::DISCONTINUITY_NONE;
        track->mPackets->queueDiscontinuity(type, NULL /* extra */, true /* discard */);
    }
}

NuPlayer::GenericSource::BufferingMonitor::BufferingMonitor(const sp<AMessage> &notify)
    : mNotify(notify),
      mDurationUs(-1ll),
      mBitrate(-1ll),
      mIsStreaming(false),
      mAudioTimeUs(0),
      mVideoTimeUs(0),
      mPollBufferingGeneration(0),
      mPrepareBuffering(false),
      mBuffering(false),
      mPrevBufferPercentage(-1),
      mOffloadAudio(false),
      mFirstDequeuedBufferRealUs(-1ll),
      mFirstDequeuedBufferMediaUs(-1ll),
      mlastDequeuedBufferMediaUs(-1ll) {
}

NuPlayer::GenericSource::BufferingMonitor::~BufferingMonitor() {
}

void NuPlayer::GenericSource::BufferingMonitor::prepare(
        const sp<NuCachedSource2> &cachedSource,
        const sp<WVMExtractor> &wvmExtractor,
        int64_t durationUs,
        int64_t bitrate,
        bool isStreaming) {
    Mutex::Autolock _l(mLock);
    prepare_l(cachedSource, wvmExtractor, durationUs, bitrate, isStreaming);
}

void NuPlayer::GenericSource::BufferingMonitor::stop() {
    Mutex::Autolock _l(mLock);
    prepare_l(NULL /* cachedSource */, NULL /* wvmExtractor */, -1 /* durationUs */,
            -1 /* bitrate */, false /* isStreaming */);
}

void NuPlayer::GenericSource::BufferingMonitor::cancelPollBuffering() {
    Mutex::Autolock _l(mLock);
    cancelPollBuffering_l();
}

void NuPlayer::GenericSource::BufferingMonitor::restartPollBuffering() {
    Mutex::Autolock _l(mLock);
    if (mIsStreaming) {
        cancelPollBuffering_l();
        onPollBuffering_l();
    }
}

void NuPlayer::GenericSource::BufferingMonitor::stopBufferingIfNecessary() {
    Mutex::Autolock _l(mLock);
    stopBufferingIfNecessary_l();
}

void NuPlayer::GenericSource::BufferingMonitor::ensureCacheIsFetching() {
    Mutex::Autolock _l(mLock);
    ensureCacheIsFetching_l();
}

void NuPlayer::GenericSource::BufferingMonitor::updateQueuedTime(bool isAudio, int64_t timeUs) {
    Mutex::Autolock _l(mLock);
    if (isAudio) {
        mAudioTimeUs = timeUs;
    } else {
        mVideoTimeUs = timeUs;
    }
}

void NuPlayer::GenericSource::BufferingMonitor::setOffloadAudio(bool offload) {
    Mutex::Autolock _l(mLock);
    mOffloadAudio = offload;
}

void NuPlayer::GenericSource::BufferingMonitor::updateDequeuedBufferTime(int64_t mediaUs) {
    Mutex::Autolock _l(mLock);
    if (mediaUs < 0) {
        mFirstDequeuedBufferRealUs = -1ll;
        mFirstDequeuedBufferMediaUs = -1ll;
    } else if (mFirstDequeuedBufferRealUs < 0) {
        mFirstDequeuedBufferRealUs = ALooper::GetNowUs();
        mFirstDequeuedBufferMediaUs = mediaUs;
    }
    mlastDequeuedBufferMediaUs = mediaUs;
}

void NuPlayer::GenericSource::BufferingMonitor::prepare_l(
        const sp<NuCachedSource2> &cachedSource,
        const sp<WVMExtractor> &wvmExtractor,
        int64_t durationUs,
        int64_t bitrate,
        bool isStreaming) {
    ALOGW_IF(wvmExtractor != NULL && cachedSource != NULL,
            "WVMExtractor and NuCachedSource are both present when "
            "BufferingMonitor::prepare_l is called, ignore NuCachedSource");

    mCachedSource = cachedSource;
    mWVMExtractor = wvmExtractor;
    mDurationUs = durationUs;
    mBitrate = bitrate;
    mIsStreaming = isStreaming;
    mAudioTimeUs = 0;
    mVideoTimeUs = 0;
    mPrepareBuffering = (cachedSource != NULL || wvmExtractor != NULL);
    cancelPollBuffering_l();
    mOffloadAudio = false;
    mFirstDequeuedBufferRealUs = -1ll;
    mFirstDequeuedBufferMediaUs = -1ll;
    mlastDequeuedBufferMediaUs = -1ll;
}

void NuPlayer::GenericSource::BufferingMonitor::cancelPollBuffering_l() {
    mBuffering = false;
    ++mPollBufferingGeneration;
    mPrevBufferPercentage = -1;
}

void NuPlayer::GenericSource::BufferingMonitor::notifyBufferingUpdate_l(int32_t percentage) {
    // Buffering percent could go backward as it's estimated from remaining
    // data and last access time. This could cause the buffering position
    // drawn on media control to jitter slightly. Remember previously reported
    // percentage and don't allow it to go backward.
    if (percentage < mPrevBufferPercentage) {
        percentage = mPrevBufferPercentage;
    } else if (percentage > 100) {
        percentage = 100;
    }

    mPrevBufferPercentage = percentage;

    ALOGV("notifyBufferingUpdate_l: buffering %d%%", percentage);

    sp<AMessage> msg = mNotify->dup();
    msg->setInt32("what", kWhatBufferingUpdate);
    msg->setInt32("percentage", percentage);
    msg->post();
}

void NuPlayer::GenericSource::BufferingMonitor::startBufferingIfNecessary_l() {
    if (mPrepareBuffering) {
        return;
    }

    if (!mBuffering) {
        ALOGD("startBufferingIfNecessary_l");

        mBuffering = true;

        ensureCacheIsFetching_l();
        sendCacheStats_l();

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatPauseOnBufferingStart);
        notify->post();
    }
}

void NuPlayer::GenericSource::BufferingMonitor::stopBufferingIfNecessary_l() {
    if (mPrepareBuffering) {
        ALOGD("stopBufferingIfNecessary_l, mBuffering=%d", mBuffering);

        mPrepareBuffering = false;

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatPrepared);
        notify->setInt32("err", OK);
        notify->post();

        return;
    }

    if (mBuffering) {
        ALOGD("stopBufferingIfNecessary_l");
        mBuffering = false;

        sendCacheStats_l();

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatResumeOnBufferingEnd);
        notify->post();
    }
}

void NuPlayer::GenericSource::BufferingMonitor::sendCacheStats_l() {
    int32_t kbps = 0;
    status_t err = UNKNOWN_ERROR;

    if (mWVMExtractor != NULL) {
        err = mWVMExtractor->getEstimatedBandwidthKbps(&kbps);
    } else if (mCachedSource != NULL) {
        err = mCachedSource->getEstimatedBandwidthKbps(&kbps);
    }

    if (err == OK) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatCacheStats);
        notify->setInt32("bandwidth", kbps);
        notify->post();
    }
}

void NuPlayer::GenericSource::BufferingMonitor::ensureCacheIsFetching_l() {
    if (mCachedSource != NULL) {
        mCachedSource->resumeFetchingIfNecessary();
    }
}

void NuPlayer::GenericSource::BufferingMonitor::schedulePollBuffering_l() {
    sp<AMessage> msg = new AMessage(kWhatPollBuffering, this);
    msg->setInt32("generation", mPollBufferingGeneration);
    // Enquires buffering status every second.
    msg->post(1000000ll);
}

int64_t NuPlayer::GenericSource::BufferingMonitor::getLastReadPosition_l() {
    if (mAudioTimeUs > 0) {
        return mAudioTimeUs;
    } else if (mVideoTimeUs > 0) {
        return mVideoTimeUs;
    } else {
        return 0;
    }
}

void NuPlayer::GenericSource::BufferingMonitor::onPollBuffering_l() {
    status_t finalStatus = UNKNOWN_ERROR;
    int64_t cachedDurationUs = -1ll;
    ssize_t cachedDataRemaining = -1;

    if (mWVMExtractor != NULL) {
        cachedDurationUs =
                mWVMExtractor->getCachedDurationUs(&finalStatus);
    } else if (mCachedSource != NULL) {
        cachedDataRemaining =
                mCachedSource->approxDataRemaining(&finalStatus);

        if (finalStatus == OK) {
            off64_t size;
            int64_t bitrate = 0ll;
            if (mDurationUs > 0 && mCachedSource->getSize(&size) == OK) {
                // |bitrate| uses bits/second unit, while size is number of bytes.
                bitrate = size * 8000000ll / mDurationUs;
            } else if (mBitrate > 0) {
                bitrate = mBitrate;
            }
            if (bitrate > 0) {
                cachedDurationUs = cachedDataRemaining * 8000000ll / bitrate;
            }
        }
    }

    if (finalStatus != OK) {
        ALOGV("onPollBuffering_l: EOS (finalStatus = %d)", finalStatus);

        if (finalStatus == ERROR_END_OF_STREAM) {
            notifyBufferingUpdate_l(100);
        }

        stopBufferingIfNecessary_l();
        return;
    } else if (cachedDurationUs >= 0ll) {
        if (mDurationUs > 0ll) {
            int64_t cachedPosUs = getLastReadPosition_l() + cachedDurationUs;
            int percentage = 100.0 * cachedPosUs / mDurationUs;
            if (percentage > 100) {
                percentage = 100;
            }

            notifyBufferingUpdate_l(percentage);
        }

        ALOGV("onPollBuffering_l: cachedDurationUs %.1f sec",
                cachedDurationUs / 1000000.0f);

        if (cachedDurationUs < kLowWaterMarkUs) {
            // Take into account the data cached in downstream components to try to avoid
            // unnecessary pause.
            if (mOffloadAudio && mFirstDequeuedBufferRealUs >= 0) {
                int64_t downStreamCacheUs = mlastDequeuedBufferMediaUs - mFirstDequeuedBufferMediaUs
                        - (ALooper::GetNowUs() - mFirstDequeuedBufferRealUs);
                if (downStreamCacheUs > 0) {
                    cachedDurationUs += downStreamCacheUs;
                }
            }

            if (cachedDurationUs < kLowWaterMarkUs) {
                startBufferingIfNecessary_l();
            }
        } else {
            int64_t highWaterMark = mPrepareBuffering ? kHighWaterMarkUs : kHighWaterMarkRebufferUs;
            if (cachedDurationUs > highWaterMark) {
                stopBufferingIfNecessary_l();
            }
        }
    } else if (cachedDataRemaining >= 0) {
        ALOGV("onPollBuffering_l: cachedDataRemaining %zd bytes",
                cachedDataRemaining);

        if (cachedDataRemaining < kLowWaterMarkBytes) {
            startBufferingIfNecessary_l();
        } else if (cachedDataRemaining > kHighWaterMarkBytes) {
            stopBufferingIfNecessary_l();
        }
    }

    schedulePollBuffering_l();
}

void NuPlayer::GenericSource::BufferingMonitor::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
      case kWhatPollBuffering:
      {
          int32_t generation;
          CHECK(msg->findInt32("generation", &generation));
          Mutex::Autolock _l(mLock);
          if (generation == mPollBufferingGeneration) {
              onPollBuffering_l();
          }
          break;
      }
      default:
          TRESPASS();
          break;
    }
}

}  // namespace android
