/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
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

#undef DEBUG_HDCP

//#define LOG_NDEBUG 0
#define LOG_TAG "AwesomePlayer"
#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Log.h>
#include <utils/Trace.h>

#include <dlfcn.h>

#include "include/AwesomePlayer.h"
#include "include/DRMExtractor.h"
#include "include/SoftwareRenderer.h"
#include "include/NuCachedSource2.h"
#include "include/ThrottledSource.h"
#include "include/MPEG2TSExtractor.h"
#include "include/WVMExtractor.h"

#ifdef ENABLE_AV_ENHANCEMENTS
#include <QCMediaDefs.h>
#endif

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/timedtext/TimedTextDriver.h>
#include <media/stagefright/AudioPlayer.h>
#ifdef QCOM_DIRECTTRACK
#ifdef LEGACY_LPA
#include <media/stagefright/LPAPlayerLegacy.h>
#else
#include <media/stagefright/LPAPlayer.h>
#endif
#ifdef USE_TUNNEL_MODE
#include <media/stagefright/TunnelPlayer.h>
#endif
#endif
#include <media/stagefright/ClockEstimator.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/Utils.h>
#ifdef QCOM_HARDWARE
#include "include/ExtendedUtils.h"
#endif


#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>

#include <media/stagefright/foundation/AMessage.h>

#include <cutils/properties.h>

#define USE_SURFACE_ALLOC 1
#define FRAME_DROP_FREQ 0
#ifdef QCOM_HARDWARE
#define LPA_MIN_DURATION_USEC_ALLOWED 30000000
#define LPA_MIN_DURATION_USEC_DEFAULT 60000000
#endif

namespace android {

static int64_t kLowWaterMarkUs = 2000000ll;  // 2secs
static int64_t kHighWaterMarkUs = 5000000ll;  // 5secs
static const size_t kLowWaterMarkBytes = 40000;
static const size_t kHighWaterMarkBytes = 200000;
#ifdef QCOM_DIRECTTRACK
int AwesomePlayer::mTunnelAliveAP = 0;
#endif

// maximum time in paused state when offloading audio decompression. When elapsed, the AudioPlayer
// is destroyed to allow the audio DSP to power down.
static int64_t kOffloadPauseMaxUs = 60000000ll;


struct AwesomeEvent : public TimedEventQueue::Event {
    AwesomeEvent(
            AwesomePlayer *player,
            void (AwesomePlayer::*method)())
        : mPlayer(player),
          mMethod(method) {
    }

protected:
    virtual ~AwesomeEvent() {}

    virtual void fire(TimedEventQueue *queue, int64_t /* now_us */) {
        (mPlayer->*mMethod)();
    }

private:
    AwesomePlayer *mPlayer;
    void (AwesomePlayer::*mMethod)();

    AwesomeEvent(const AwesomeEvent &);
    AwesomeEvent &operator=(const AwesomeEvent &);
};

struct AwesomeLocalRenderer : public AwesomeRenderer {
    AwesomeLocalRenderer(
            const sp<ANativeWindow> &nativeWindow, const sp<MetaData> &meta)
        : mTarget(new SoftwareRenderer(nativeWindow, meta)) {
    }

    virtual void render(MediaBuffer *buffer) {
        render((const uint8_t *)buffer->data() + buffer->range_offset(),
               buffer->range_length());
    }

    void render(const void *data, size_t size) {
        mTarget->render(data, size, NULL);
    }

protected:
    virtual ~AwesomeLocalRenderer() {
        delete mTarget;
        mTarget = NULL;
    }

private:
    SoftwareRenderer *mTarget;

    AwesomeLocalRenderer(const AwesomeLocalRenderer &);
    AwesomeLocalRenderer &operator=(const AwesomeLocalRenderer &);;
};

struct AwesomeNativeWindowRenderer : public AwesomeRenderer {
    AwesomeNativeWindowRenderer(
            const sp<ANativeWindow> &nativeWindow,
            int32_t rotationDegrees)
        : mNativeWindow(nativeWindow) {
        applyRotation(rotationDegrees);
    }

    virtual void render(MediaBuffer *buffer) {
        ATRACE_CALL();
        int64_t timeUs;
        CHECK(buffer->meta_data()->findInt64(kKeyTime, &timeUs));
        native_window_set_buffers_timestamp(mNativeWindow.get(), timeUs * 1000);
        status_t err = mNativeWindow->queueBuffer(
                mNativeWindow.get(), buffer->graphicBuffer().get(), -1);
        if (err != 0) {
            ALOGE("queueBuffer failed with error %s (%d)", strerror(-err),
                    -err);
            return;
        }

        sp<MetaData> metaData = buffer->meta_data();
        metaData->setInt32(kKeyRendered, 1);
    }

protected:
    virtual ~AwesomeNativeWindowRenderer() {}

private:
    sp<ANativeWindow> mNativeWindow;

    void applyRotation(int32_t rotationDegrees) {
        uint32_t transform;
        switch (rotationDegrees) {
            case 0: transform = 0; break;
            case 90: transform = HAL_TRANSFORM_ROT_90; break;
            case 180: transform = HAL_TRANSFORM_ROT_180; break;
            case 270: transform = HAL_TRANSFORM_ROT_270; break;
            default: transform = 0; break;
        }

        if (transform) {
            CHECK_EQ(0, native_window_set_buffers_transform(
                        mNativeWindow.get(), transform));
        }
    }

    AwesomeNativeWindowRenderer(const AwesomeNativeWindowRenderer &);
    AwesomeNativeWindowRenderer &operator=(
            const AwesomeNativeWindowRenderer &);
};

// To collect the decoder usage
void addBatteryData(uint32_t params) {
    sp<IBinder> binder =
        defaultServiceManager()->getService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    CHECK(service.get() != NULL);

    service->addBatteryData(params);
}

////////////////////////////////////////////////////////////////////////////////
AwesomePlayer::AwesomePlayer()
    : mQueueStarted(false),
      mUIDValid(false),
      mTimeSource(NULL),
      mVideoRenderingStarted(false),
      mVideoRendererIsPreview(false),
      mMediaRenderingStartGeneration(0),
      mStartGeneration(0),
      mAudioPlayer(NULL),
      mDisplayWidth(0),
      mDisplayHeight(0),
      mVideoScalingMode(NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW),
      mFlags(0),
      mExtractorFlags(0),
      mVideoBuffer(NULL),
      mDecryptHandle(NULL),
      mLastVideoTimeUs(-1),
      mTextDriver(NULL),
      mOffloadAudio(false),
      mAudioTearDown(false),
      mReadRetry(false),
      mIsFirstFrameAfterResume(false),
      mCustomAVSync(false),
      mVSyncLocker(NULL) {
    CHECK_EQ(mClient.connect(), (status_t)OK);

    DataSource::RegisterDefaultSniffers();

    mVideoEvent = new AwesomeEvent(this, &AwesomePlayer::onVideoEvent);
    mVideoEventPending = false;
    mStreamDoneEvent = new AwesomeEvent(this, &AwesomePlayer::onStreamDone);
    mStreamDoneEventPending = false;
    mBufferingEvent = new AwesomeEvent(this, &AwesomePlayer::onBufferingUpdate);
    mBufferingEventPending = false;
    mVideoLagEvent = new AwesomeEvent(this, &AwesomePlayer::onVideoLagUpdate);
    mVideoLagEventPending = false;

    mCheckAudioStatusEvent = new AwesomeEvent(
            this, &AwesomePlayer::onCheckAudioStatus);

    mAudioStatusEventPending = false;

    mAudioTearDownEvent = new AwesomeEvent(this,
                              &AwesomePlayer::onAudioTearDownEvent);
    mAudioTearDownEventPending = false;

    mDurationUs = -1;
    mAudioTearDownPosition = 0;

    mClockEstimator = new WindowedLinearFitEstimator();

    reset();
#ifdef QCOM_DIRECTTRACK
    mIsTunnelAudio = false;
#endif

#ifdef QCOM_HARDWARE
    mLateAVSyncMargin = ExtendedUtils::ShellProp::getMaxAVSyncLateMargin();
    mCustomAVSync = ExtendedUtils::ShellProp::isCustomAVSyncEnabled();
#else
    mLateAVSyncMargin = 40000;
#endif
}

AwesomePlayer::~AwesomePlayer() {
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();

#ifdef QCOM_DIRECTTRACK
    // Disable Tunnel Mode Audio
    if (mIsTunnelAudio) {
        if (mTunnelAliveAP > 0) {
            mTunnelAliveAP--;
            ALOGV("mTunnelAliveAP = %d", mTunnelAliveAP);
        }
    }
    mIsTunnelAudio = false;
#endif
    mClient.disconnect();
#ifdef ENABLE_AV_ENHANCEMENTS
    ExtendedUtils::drainSecurePool();
#endif
}

void AwesomePlayer::printStats() {
    char value[PROPERTY_VALUE_MAX];
    property_get("persist.debug.sf.statistics", value, "0");
    if (atoi(value) && mVideoSource != NULL) {
        ALOGI("===========================\n"
            "   videoDimensions(%d x %d)\n"
            "   Total Video Frames Decoded(%lld)\n"
            "   Total Video Frames Rendered(%lld)\n"
            "   Total Playback Duration(%lld ms)\n"
            "   numVideoFramesDropped(%lld)\n"
            "   Average Frames Per Second(%.4f)\n"
            "   Last Seek To Time(%lld ms)\n"
            "   Last Paused Time(%lld ms)\n"
            "   First Frame Latency (%lld ms)\n"
            "   Number of times AV Sync Lost(%u)\n"
            "   Max Video Ahead Time Delta(%u)\n"
            "   Max Video Behind Time Delta(%u)\n"
            "   Max Time Sync Loss(%u)\n"
            "   EOS(%d)\n"
            "   PLAYING(%d)\n"
            "===========================\n\n",
            mStats.mVideoWidth,
            mStats.mVideoHeight,
            mStats.mNumVideoFramesDecoded,
            mStats.mTotalFrames,
            mStats.mTotalTimeUs/1000,
            mStats.mNumVideoFramesDropped,
            mStats.mTotalTimeUs > 0 ? ((double)(mStats.mTotalFrames)*1E6)/((double)mStats.mTotalTimeUs) : 0,
            mStats.mLastSeekToTimeMs,
            mStats.mLastPausedTimeMs,
            mStats.mFirstFrameLatencyUs/1000,
            mStats.mNumTimesSyncLoss,
            -mStats.mMaxEarlyDelta/1000,
            mStats.mMaxLateDelta/1000,
            mStats.mMaxTimeSyncLoss/1000,
            (mFlags & VIDEO_AT_EOS) > 0,
            (mFlags & PLAYING) > 0);
    }
}

void AwesomePlayer::cancelPlayerEvents(bool keepNotifications) {
    mQueue.cancelEvent(mVideoEvent->eventID());
    mVideoEventPending = false;
    mQueue.cancelEvent(mVideoLagEvent->eventID());
    mVideoLagEventPending = false;

    if (mOffloadAudio) {
        mQueue.cancelEvent(mAudioTearDownEvent->eventID());
        mAudioTearDownEventPending = false;
    }

    if (!keepNotifications) {
        mQueue.cancelEvent(mStreamDoneEvent->eventID());
        mStreamDoneEventPending = false;
        mQueue.cancelEvent(mCheckAudioStatusEvent->eventID());
        mAudioStatusEventPending = false;

        mQueue.cancelEvent(mBufferingEvent->eventID());
        mBufferingEventPending = false;
        mAudioTearDown = false;
    }
}

void AwesomePlayer::setListener(const wp<MediaPlayerBase> &listener) {
    Mutex::Autolock autoLock(mLock);
    mListener = listener;
}

void AwesomePlayer::setUID(uid_t uid) {
    ALOGV("AwesomePlayer running on behalf of uid %d", uid);

    mUID = uid;
    mUIDValid = true;
}

status_t AwesomePlayer::setDataSource(
        const char *uri, const KeyedVector<String8, String8> *headers) {
    Mutex::Autolock autoLock(mLock);
    return setDataSource_l(uri, headers);
}

status_t AwesomePlayer::setDataSource_l(
        const char *uri, const KeyedVector<String8, String8> *headers) {
    reset_l();

    mUri = uri;
    if (uri) {
        printFileName(uri);
    }

#ifdef ENABLE_AV_ENHANCEMENTS
    ExtendedUtils::prefetchSecurePool(uri);
#endif

    if (headers) {
        mUriHeaders = *headers;

        ssize_t index = mUriHeaders.indexOfKey(String8("x-hide-urls-from-log"));
        if (index >= 0) {
            // Browser is in "incognito" mode, suppress logging URLs.

            // This isn't something that should be passed to the server.
            mUriHeaders.removeItemsAt(index);

            modifyFlags(INCOGNITO, SET);
        }
    }

    ALOGI("setDataSource_l(URL suppressed)");

    // The actual work will be done during preparation in the call to
    // ::finishSetDataSource_l to avoid blocking the calling thread in
    // setDataSource for any significant time.

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mFd = -1;
        mStats.mURI = mUri;
    }

    return OK;
}

status_t AwesomePlayer::setDataSource(
        int fd, int64_t offset, int64_t length) {
    Mutex::Autolock autoLock(mLock);

    ALOGD("Before reset_l");
    reset_l();
    if (fd) {
       printFileName(fd);
    }

#ifdef ENABLE_AV_ENHANCEMENTS
    if (fd) {
        ExtendedUtils::prefetchSecurePool(fd);
    }
#endif

    sp<DataSource> dataSource = new FileSource(fd, offset, length);

    status_t err = dataSource->initCheck();

    if (err != OK) {
        return err;
    }

    mFileSource = dataSource;

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mFd = fd;
        mStats.mURI = String8();
    }

    return setDataSource_l(dataSource);
}

status_t AwesomePlayer::setDataSource(const sp<IStreamSource> &source) {
    return INVALID_OPERATION;
}

status_t AwesomePlayer::setDataSource_l(
        const sp<DataSource> &dataSource) {
    sp<MediaExtractor> extractor = MediaExtractor::Create(dataSource);

    if (extractor == NULL) {
        return UNKNOWN_ERROR;
    }

    if (extractor->getDrmFlag()) {
        checkDrmStatus(dataSource);
    }

    return setDataSource_l(extractor);
}

void AwesomePlayer::checkDrmStatus(const sp<DataSource>& dataSource) {
    dataSource->getDrmInfo(mDecryptHandle, &mDrmManagerClient);
    if (mDecryptHandle != NULL) {
        CHECK(mDrmManagerClient);
        if (RightsStatus::RIGHTS_VALID != mDecryptHandle->status) {
            notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, ERROR_DRM_NO_LICENSE);
        }
    }
}

status_t AwesomePlayer::setDataSource_l(const sp<MediaExtractor> &extractor) {
    // Attempt to approximate overall stream bitrate by summing all
    // tracks' individual bitrates, if not all of them advertise bitrate,
    // we have to fail.

    int64_t totalBitRate = 0;

    mExtractor = extractor;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        int32_t bitrate;
        if (!meta->findInt32(kKeyBitRate, &bitrate)) {
            const char *mime;
            CHECK(meta->findCString(kKeyMIMEType, &mime));
            ALOGV("track of type '%s' does not publish bitrate", mime);

            totalBitRate = -1;
            break;
        }

        totalBitRate += bitrate;
    }

    mBitrate = totalBitRate;

    ALOGV("mBitrate = %lld bits/sec", mBitrate);

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mBitrate = mBitrate;
        mStats.mTracks.clear();
        mStats.mAudioTrackIndex = -1;
        mStats.mVideoTrackIndex = -1;
    }

    bool haveAudio = false;
    bool haveVideo = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *_mime;
        CHECK(meta->findCString(kKeyMIMEType, &_mime));

        String8 mime = String8(_mime);

        if (!haveVideo && !strncasecmp(mime.string(), "video/", 6)) {
            setVideoSource(extractor->getTrack(i));
            haveVideo = true;

            // Set the presentation/display size
            int32_t displayWidth, displayHeight;
            bool success = meta->findInt32(kKeyDisplayWidth, &displayWidth);
            if (success) {
                success = meta->findInt32(kKeyDisplayHeight, &displayHeight);
            }
            if (success) {
                mDisplayWidth = displayWidth;
                mDisplayHeight = displayHeight;
            }

            {
                Mutex::Autolock autoLock(mStatsLock);
                mStats.mVideoTrackIndex = mStats.mTracks.size();
                mStats.mTracks.push();
                TrackStat *stat =
                    &mStats.mTracks.editItemAt(mStats.mVideoTrackIndex);
                stat->mMIME = mime.string();
            }
        } else if (!haveAudio &&
#ifdef QCOM_HARDWARE
                !ExtendedUtils::ShellProp::isAudioDisabled(false) &&
#endif
                !strncasecmp(mime.string(), "audio/", 6)) {
            setAudioSource(extractor->getTrack(i));
            haveAudio = true;
            mActiveAudioTrackIndex = i;

            {
                Mutex::Autolock autoLock(mStatsLock);
                mStats.mAudioTrackIndex = mStats.mTracks.size();
                mStats.mTracks.push();
                TrackStat *stat =
                    &mStats.mTracks.editItemAt(mStats.mAudioTrackIndex);
                stat->mMIME = mime.string();
            }

            if (!strcasecmp(mime.string(), MEDIA_MIMETYPE_AUDIO_VORBIS)) {
                // Only do this for vorbis audio, none of the other audio
                // formats even support this ringtone specific hack and
                // retrieving the metadata on some extractors may turn out
                // to be very expensive.
                sp<MetaData> fileMeta = extractor->getMetaData();
                int32_t loop;
                if (fileMeta != NULL
                        && fileMeta->findInt32(kKeyAutoLoop, &loop) && loop != 0) {
                    modifyFlags(AUTO_LOOPING, SET);
                }
            }
        } else if (!strcasecmp(mime.string(), MEDIA_MIMETYPE_TEXT_3GPP)) {
            addTextSource_l(i, extractor->getTrack(i));
        }
    }

    if (!haveAudio && !haveVideo) {
        if (mWVMExtractor != NULL) {
            return mWVMExtractor->getError();
        } else {
            return UNKNOWN_ERROR;
        }
    }

    mExtractorFlags = extractor->flags();

    return OK;
}

void AwesomePlayer::reset() {
    Mutex::Autolock autoLock(mLock);
    reset_l();
}

void AwesomePlayer::reset_l() {
    mVideoRenderingStarted = false;
    mActiveAudioTrackIndex = -1;
    mDisplayWidth = 0;
    mDisplayHeight = 0;

    notifyListener_l(MEDIA_STOPPED);

    if (mDecryptHandle != NULL) {
            mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                    Playback::STOP, 0);
            mDecryptHandle = NULL;
            mDrmManagerClient = NULL;
    }

    if (mFlags & PLAYING) {
        uint32_t params = IMediaPlayerService::kBatteryDataTrackDecoder;
        if ((mAudioSource != NULL) && (mAudioSource != mAudioTrack)) {
            params |= IMediaPlayerService::kBatteryDataTrackAudio;
        }
        if (mVideoSource != NULL) {
            params |= IMediaPlayerService::kBatteryDataTrackVideo;
        }
        addBatteryData(params);
    }

    if (mFlags & PREPARING) {
        modifyFlags(PREPARE_CANCELLED, SET);
        if (mConnectingDataSource != NULL) {
            ALOGI("interrupting the connection process");
            mConnectingDataSource->disconnect();
        }

        if (mFlags & PREPARING_CONNECTED) {
            // We are basically done preparing, we're just buffering
            // enough data to start playback, we can safely interrupt that.
            finishAsyncPrepare_l();
        }
    }

    while (mFlags & PREPARING) {
        mPreparedCondition.wait(mLock);
    }

    cancelPlayerEvents();

    mWVMExtractor.clear();
    mCachedSource.clear();
    mAudioTrack.clear();
    mVideoTrack.clear();
    mExtractor.clear();

    // Shutdown audio first, so that the response to the reset request
    // appears to happen instantaneously as far as the user is concerned
    // If we did this later, audio would continue playing while we
    // shutdown the video-related resources and the player appear to
    // not be as responsive to a reset request.
    if ((mAudioPlayer == NULL || !(mFlags & AUDIOPLAYER_STARTED))
            && mAudioSource != NULL) {
        // If we had an audio player, it would have effectively
        // taken possession of the audio source and stopped it when
        // _it_ is stopped. Otherwise this is still our responsibility.
        mAudioSource->stop();
    }
    mAudioSource.clear();
    mOmxSource.clear();

    mTimeSource = NULL;

    delete mAudioPlayer;
    mAudioPlayer = NULL;

    if (mTextDriver != NULL) {
        delete mTextDriver;
        mTextDriver = NULL;
    }

    mVideoRenderer.clear();

    modifyFlags(PLAYING, CLEAR);
    printStats();
    if (mVideoSource != NULL) {
        shutdownVideoDecoder_l();
    }

    modifyFlags(0, ASSIGN);
    mExtractorFlags = 0;
    mTimeSourceDeltaUs = 0;
    mVideoTimeUs = 0;

    mSeeking = NO_SEEK;
    mSeekNotificationSent = true;
    mSeekTimeUs = 0;

    mUri.setTo("");
    mUriHeaders.clear();

    mFileSource.clear();

    mBitrate = -1;
    mLastVideoTimeUs = -1;

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mFd = -1;
        mStats.mURI = String8();
        mStats.mBitrate = -1;
        mStats.mAudioTrackIndex = -1;
        mStats.mVideoTrackIndex = -1;
        mStats.mNumVideoFramesDecoded = 0;
        mStats.mNumVideoFramesDropped = 0;
        mStats.mVideoWidth = -1;
        mStats.mVideoHeight = -1;
        mStats.mFlags = 0;
        mStats.mTracks.clear();
        mStats.mConsecutiveFramesDropped = 0;
        mStats.mCatchupTimeStart = 0;
        mStats.mNumTimesSyncLoss = 0;
        mStats.mMaxEarlyDelta = 0;
        mStats.mMaxLateDelta = 0;
        mStats.mMaxTimeSyncLoss = 0;
        mStats.mTotalFrames = 0;
        mStats.mLastFrameUs = 0;
        mStats.mTotalTimeUs = 0;
        mStats.mVeryFirstFrame = true;
        mStats.mFirstFrameLatencyUs = 0;
        mStats.mLastPausedTimeMs = 0;
        mStats.mLastSeekToTimeMs = 0;
        mStats.mResumeDelayStartUs = -1;
        mStats.mSeekDelayStartUs = -1;
    }

    mWatchForAudioSeekComplete = false;
    mWatchForAudioEOS = false;

    mMediaRenderingStartGeneration = 0;
    mStartGeneration = 0;
}

void AwesomePlayer::notifyListener_l(int msg, int ext1, int ext2) {
    if ((mListener != NULL) && !mAudioTearDown) {
        sp<MediaPlayerBase> listener = mListener.promote();

        if (listener != NULL) {
            listener->sendEvent(msg, ext1, ext2);
        }
    }
}

bool AwesomePlayer::getBitrate(int64_t *bitrate) {
    off64_t size;
    if (mDurationUs > 0 && mCachedSource != NULL
            && mCachedSource->getSize(&size) == OK) {
        *bitrate = size * 8000000ll / mDurationUs;  // in bits/sec
        return true;
    }

    if (mBitrate >= 0) {
        *bitrate = mBitrate;
        return true;
    }

    *bitrate = 0;

    return false;
}

// Returns true iff cached duration is available/applicable.
bool AwesomePlayer::getCachedDuration_l(int64_t *durationUs, bool *eos) {
    int64_t bitrate;

    if (mCachedSource != NULL && getBitrate(&bitrate) && (bitrate > 0)) {
        status_t finalStatus;
        size_t cachedDataRemaining = mCachedSource->approxDataRemaining(&finalStatus);
        *durationUs = cachedDataRemaining * 8000000ll / bitrate;
        *eos = (finalStatus != OK);
        return true;
    } else if (mWVMExtractor != NULL) {
        status_t finalStatus;
        *durationUs = mWVMExtractor->getCachedDurationUs(&finalStatus);
        *eos = (finalStatus != OK);
        return true;
    }

    return false;
}

void AwesomePlayer::ensureCacheIsFetching_l() {
    if (mCachedSource != NULL) {
        mCachedSource->resumeFetchingIfNecessary();
    }
}

void AwesomePlayer::onVideoLagUpdate() {
    Mutex::Autolock autoLock(mLock);
    if (!mVideoLagEventPending || mAudioPlayer == NULL) {
        return;
    }
    mVideoLagEventPending = false;

    int64_t audioTimeUs = mAudioPlayer->getMediaTimeUs();
    int64_t videoLateByUs = audioTimeUs - mVideoTimeUs;

    if (!(mFlags & VIDEO_AT_EOS) && videoLateByUs > 300000ll) {
        ALOGV("video late by %lld ms.", videoLateByUs / 1000ll);

        notifyListener_l(
                MEDIA_INFO,
                MEDIA_INFO_VIDEO_TRACK_LAGGING,
                videoLateByUs / 1000ll);
    }

    postVideoLagEvent_l();
}

void AwesomePlayer::onBufferingUpdate() {
    Mutex::Autolock autoLock(mLock);
    if (!mBufferingEventPending) {
        return;
    }
    mBufferingEventPending = false;

    if (mCachedSource != NULL) {
        status_t finalStatus;
        size_t cachedDataRemaining = mCachedSource->approxDataRemaining(&finalStatus);
        bool eos = (finalStatus != OK);

        if (eos) {
            if (finalStatus == ERROR_END_OF_STREAM) {
                notifyListener_l(MEDIA_BUFFERING_UPDATE, 100);
            }
            if (mFlags & PREPARING) {
                ALOGV("cache has reached EOS, prepare is done.");
                finishAsyncPrepare_l();
            }
        } else {
            int64_t bitrate;
            if (getBitrate(&bitrate)) {
                size_t cachedSize = mCachedSource->cachedSize();
                int64_t cachedDurationUs = cachedSize * 8000000ll / bitrate;

                int percentage = 100.0 * (double)cachedDurationUs / mDurationUs;
                if (percentage > 100) {
                    percentage = 100;
                }

                notifyListener_l(MEDIA_BUFFERING_UPDATE, percentage);
            } else {
                // We don't know the bitrate of the stream, use absolute size
                // limits to maintain the cache.

                if ((mFlags & PLAYING) && !eos
                        && (cachedDataRemaining < kLowWaterMarkBytes)) {
                    ALOGI("cache is running low (< %d) , pausing.",
                         kLowWaterMarkBytes);
                    modifyFlags(CACHE_UNDERRUN, SET);
                    pause_l();
                    ensureCacheIsFetching_l();
                    sendCacheStats();
                    notifyListener_l(MEDIA_INFO, MEDIA_INFO_BUFFERING_START);
                } else if (eos || cachedDataRemaining > kHighWaterMarkBytes) {
                    if (mFlags & CACHE_UNDERRUN) {
                        ALOGI("cache has filled up (> %d), resuming.",
                             kHighWaterMarkBytes);
                        modifyFlags(CACHE_UNDERRUN, CLEAR);
                        play_l();
                    } else if (mFlags & PREPARING) {
                        ALOGV("cache has filled up (> %d), prepare is done",
                             kHighWaterMarkBytes);
                        finishAsyncPrepare_l();
                    }
                }
            }
        }
    } else if (mWVMExtractor != NULL) {
        status_t finalStatus;

        int64_t cachedDurationUs
            = mWVMExtractor->getCachedDurationUs(&finalStatus);

        bool eos = (finalStatus != OK);

        if (eos) {
            if (finalStatus == ERROR_END_OF_STREAM) {
                notifyListener_l(MEDIA_BUFFERING_UPDATE, 100);
            }
            if (mFlags & PREPARING) {
                ALOGV("cache has reached EOS, prepare is done.");
                finishAsyncPrepare_l();
            }
        } else {
            int percentage = 100.0 * (double)cachedDurationUs / mDurationUs;
            if (percentage > 100) {
                percentage = 100;
            }

            notifyListener_l(MEDIA_BUFFERING_UPDATE, percentage);
        }
    }

    int64_t cachedDurationUs;
    bool eos;
    if (getCachedDuration_l(&cachedDurationUs, &eos)) {
        ALOGV("cachedDurationUs = %.2f secs, eos=%d",
             cachedDurationUs / 1E6, eos);

        if ((mFlags & PLAYING) && !eos
                && (cachedDurationUs < kLowWaterMarkUs)) {
            modifyFlags(CACHE_UNDERRUN, SET);
            ALOGI("cache is running low (%.2f secs) , pausing.",
                  cachedDurationUs / 1E6);
            pause_l();
            ensureCacheIsFetching_l();
            sendCacheStats();
            notifyListener_l(MEDIA_INFO, MEDIA_INFO_BUFFERING_START);
        } else if (eos || cachedDurationUs > kHighWaterMarkUs) {
            if (mFlags & CACHE_UNDERRUN) {
                modifyFlags(CACHE_UNDERRUN, CLEAR);
                ALOGI("cache has filled up (%.2f secs), resuming.",
                      cachedDurationUs / 1E6);
                play_l();
            } else if (mFlags & PREPARING) {
                ALOGV("cache has filled up (%.2f secs), prepare is done",
                     cachedDurationUs / 1E6);
                finishAsyncPrepare_l();
            }
        }
    }

    if (mFlags & (PLAYING | PREPARING | CACHE_UNDERRUN)) {
        postBufferingEvent_l();
    }
}

void AwesomePlayer::sendCacheStats() {
    sp<MediaPlayerBase> listener = mListener.promote();
    if (listener != NULL) {
        int32_t kbps = 0;
        status_t err = UNKNOWN_ERROR;
        if (mCachedSource != NULL) {
            err = mCachedSource->getEstimatedBandwidthKbps(&kbps);
        } else if (mWVMExtractor != NULL) {
            err = mWVMExtractor->getEstimatedBandwidthKbps(&kbps);
        }
        if (err == OK) {
            listener->sendEvent(
                MEDIA_INFO, MEDIA_INFO_NETWORK_BANDWIDTH, kbps);
        }
    }
}

void AwesomePlayer::onStreamDone() {
    // Posted whenever any stream finishes playing.
    ATRACE_CALL();

    Mutex::Autolock autoLock(mLock);
    if (!mStreamDoneEventPending) {
        return;
    }
    mStreamDoneEventPending = false;

    if (mStreamDoneStatus != ERROR_END_OF_STREAM) {
        ALOGV("MEDIA_ERROR %d", mStreamDoneStatus);

        notifyListener_l(
                MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, mStreamDoneStatus);

        pause_l(true /* at eos */);

        modifyFlags(AT_EOS, SET);
        return;
    }

    const bool allDone =
        (mVideoSource == NULL || (mFlags & VIDEO_AT_EOS))
            && (mAudioSource == NULL || (mFlags & AUDIO_AT_EOS));

    if (!allDone) {
        return;
    }

    if ((mFlags & LOOPING)
            || ((mFlags & AUTO_LOOPING)
                && (mAudioSink == NULL || mAudioSink->realtime()))) {
        // Don't AUTO_LOOP if we're being recorded, since that cannot be
        // turned off and recording would go on indefinitely.

        seekTo_l(0);

        if (mVideoSource != NULL) {
            postVideoEvent_l();
        }
    } else {
        ALOGV("MEDIA_PLAYBACK_COMPLETE");
        notifyListener_l(MEDIA_PLAYBACK_COMPLETE);

        pause_l(true /* at eos */);

        // If audio hasn't completed MEDIA_SEEK_COMPLETE yet,
        // notify MEDIA_SEEK_COMPLETE to observer immediately for state persistence.
        if (mWatchForAudioSeekComplete) {
            notifyListener_l(MEDIA_SEEK_COMPLETE);
            mWatchForAudioSeekComplete = false;
        }

        modifyFlags(AT_EOS, SET);
    }
}

status_t AwesomePlayer::play() {
    ATRACE_CALL();

    Mutex::Autolock autoLock(mLock);

    modifyFlags(CACHE_UNDERRUN, CLEAR);

    return play_l();
}

status_t AwesomePlayer::play_l() {
    modifyFlags(SEEK_PREVIEW, CLEAR);

    if (mFlags & PLAYING) {
        return OK;
    }

    mMediaRenderingStartGeneration = ++mStartGeneration;

    if (!(mFlags & PREPARED)) {
        status_t err = prepare_l();

        if (err != OK) {
            return err;
        }
    }

    modifyFlags(PLAYING, SET);
    modifyFlags(FIRST_FRAME, SET);

    if (mDecryptHandle != NULL) {
        int64_t position;
        getPosition(&position);
        mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                Playback::START, position / 1000);
    }

    if (mAudioSource != NULL) {
        if (mAudioPlayer == NULL) {
            createAudioPlayer_l();
        }

        CHECK(!(mFlags & AUDIO_RUNNING));

        if (mVideoSource == NULL) {

            // We don't want to post an error notification at this point,
            // the error returned from MediaPlayer::start() will suffice.
            bool sendErrorNotification = false;
#ifdef QCOM_DIRECTTRACK
            if(mIsTunnelAudio) {
                // For tunnel Audio error has to be posted to the client
                sendErrorNotification = true;
            }
#endif
            status_t err = startAudioPlayer_l(sendErrorNotification);

            if ((err != OK) && mOffloadAudio) {
                 err = fallbackToSWDecoder();
            }

            if (err != OK) {
                delete mAudioPlayer;
                mAudioPlayer = NULL;

                modifyFlags((PLAYING | FIRST_FRAME), CLEAR);

                if (mDecryptHandle != NULL) {
                    mDrmManagerClient->setPlaybackStatus(
                            mDecryptHandle, Playback::STOP, 0);
                }

                return err;
            }
        }
    }

    if (mTimeSource == NULL && mAudioPlayer == NULL) {
        mTimeSource = &mSystemTimeSource;
    }

    {
        Mutex::Autolock autoLock(mStatsLock);
        if (mStats.mVeryFirstFrame) {
            mStats.mFirstFrameLatencyStartUs = getTimeOfDayUs();
        } else {
            mStats.mResumeDelayStartUs = getTimeOfDayUs();
        }
    }

    if (mVideoSource != NULL) {
        // Kick off video playback
        postVideoEvent_l();

        if (mAudioSource != NULL && mVideoSource != NULL) {
            postVideoLagEvent_l();
        }
    }

    if (mFlags & AT_EOS) {
        // Legacy behaviour, if a stream finishes playing and then
        // is started again, we play from the start...
        seekTo_l(0);
    }

    uint32_t params = IMediaPlayerService::kBatteryDataCodecStarted
        | IMediaPlayerService::kBatteryDataTrackDecoder;
    if ((mAudioSource != NULL) && (mAudioSource != mAudioTrack)) {
        params |= IMediaPlayerService::kBatteryDataTrackAudio;
    }
    if (mVideoSource != NULL) {
        params |= IMediaPlayerService::kBatteryDataTrackVideo;
    }
    addBatteryData(params);

    if (isStreamingHTTP()) {
        postBufferingEvent_l();
    }

    return OK;
}

status_t AwesomePlayer::fallbackToSWDecoder() {
    int64_t curTimeUs;
    status_t err = OK;

    ALOGD("copl:play_l() cannot create offload output, fallback to sw decode");
    getPosition(&curTimeUs);

    delete mAudioPlayer;
    mAudioPlayer = NULL;
    // if the player was started it will take care of stopping the source when destroyed
    if (!(mFlags & AUDIOPLAYER_STARTED)) {
        mAudioSource->stop();
    }
    modifyFlags((AUDIO_RUNNING | AUDIOPLAYER_STARTED), CLEAR);
    mOffloadAudio = false;
    mAudioSource = mOmxSource;
    if (mAudioSource != NULL) {
        err = mAudioSource->start();

        if (err != OK) {
            mAudioSource.clear();
        } else {
            mSeekNotificationSent = true;
            if (mExtractorFlags & MediaExtractor::CAN_SEEK) {
                seekTo_l(curTimeUs);
            }
            createAudioPlayer_l();
            err = startAudioPlayer_l(false);
        }
    }

    return err;
}

void AwesomePlayer::createAudioPlayer_l()
{
    uint32_t flags = 0;
    int64_t cachedDurationUs;
    bool eos;
#ifdef QCOM_DIRECTTRACK
    sp<MetaData> format = mAudioTrack->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    int tunnelObjectsAlive = 0;
    CHECK(success);
#endif
    if (mOffloadAudio) {
        flags |= AudioPlayer::USE_OFFLOAD;
    } else if (mVideoSource == NULL
            && (mDurationUs > AUDIO_SINK_MIN_DEEP_BUFFER_DURATION_US ||
            (getCachedDuration_l(&cachedDurationUs, &eos) &&
            cachedDurationUs > AUDIO_SINK_MIN_DEEP_BUFFER_DURATION_US))) {
        flags |= AudioPlayer::ALLOW_DEEP_BUFFERING;
    }
    if (isStreamingHTTP() || isWidevineContent()) {
        flags |= AudioPlayer::IS_STREAMING;
    }
    if (mVideoSource != NULL) {
        flags |= AudioPlayer::HAS_VIDEO;
    }
#ifdef QCOM_DIRECTTRACK
#ifdef USE_TUNNEL_MODE
    // Create tunnel player if tunnel mode is enabled
    ALOGW("Trying to create tunnel player mIsTunnelAudio %d, \
            LPAPlayer::mObjectsAlive %d, \
            TunnelPlayer::mTunnelObjectsAlive = %d,\
            (mAudioPlayer == NULL) %d",
            mIsTunnelAudio, TunnelPlayer::mTunnelObjectsAlive,
            LPAPlayer::mObjectsAlive,(mAudioPlayer == NULL));

    if(mIsTunnelAudio && (mAudioPlayer == NULL) &&
            (LPAPlayer::mObjectsAlive == 0) &&
            (TunnelPlayer::mTunnelObjectsAlive < TunnelPlayer::getTunnelObjectsAliveMax())) {
        ALOGD("Tunnel player created for  mime %s duration %lld\n",\
            mime, mDurationUs);
        bool initCheck =  false;
        if(mVideoSource != NULL) {
            // The parameter true is to inform tunnel player that
            // clip is audio video
            ALOGD("Tunnel for video");
            mAudioPlayer = new TunnelPlayer(mAudioSink, initCheck,
                    this, true);
        }
        else {
            ALOGD("Tunnel for audio");
            mAudioPlayer = new TunnelPlayer(mAudioSink, initCheck,
                    this);
        }
        if(!initCheck) {
            ALOGE("deleting Tunnel Player - initCheck failed");
            delete mAudioPlayer;
            mAudioPlayer = NULL;
        }
    }
    tunnelObjectsAlive = (TunnelPlayer::mTunnelObjectsAlive);
#endif
    int32_t nchannels = 0;
    if(mAudioTrack != NULL) {
        sp<MetaData> format = mAudioTrack->getFormat();
        if(format != NULL) {
            format->findInt32( kKeyChannelCount, &nchannels );
            ALOGV("nchannels %d;LPA will be skipped if nchannels is > 2 or nchannels == 0",nchannels);
        }
    }
    char lpaDecode[PROPERTY_VALUE_MAX];
    uint32_t minDurationForLPA = LPA_MIN_DURATION_USEC_DEFAULT;
    char minUserDefDuration[PROPERTY_VALUE_MAX];
    property_get("lpa.decode",lpaDecode,"0");
    property_get("lpa.min_duration",minUserDefDuration,"LPA_MIN_DURATION_USEC_DEFAULT");
    minDurationForLPA = atoi(minUserDefDuration);
    if(minDurationForLPA < LPA_MIN_DURATION_USEC_ALLOWED) {
        if(mAudioPlayer == NULL) {
            ALOGE("LPAPlayer::Clip duration setting of less than 30sec not supported, defaulting to 60sec");
            minDurationForLPA = LPA_MIN_DURATION_USEC_DEFAULT;
        }
    }
    if((strcmp("true",lpaDecode) == 0) && (mAudioPlayer == NULL) &&
#ifdef USE_TUNNEL_MODE
       (tunnelObjectsAlive < TunnelPlayer::getTunnelObjectsAliveMax()) &&
#endif
       (nchannels && (nchannels <= 2)))
    {
        ALOGV("LPAPlayer::getObjectsAlive() %d",LPAPlayer::mObjectsAlive);
        if ( mDurationUs > minDurationForLPA
             && (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG) || !strcasecmp(mime,MEDIA_MIMETYPE_AUDIO_AAC))
             && LPAPlayer::mObjectsAlive == 0 && mVideoSource == NULL) {
            ALOGD("LPAPlayer created, LPA MODE detected mime %s duration %lld", mime, mDurationUs);
            bool initCheck =  false;
            mAudioPlayer = new LPAPlayer(mAudioSink, initCheck, this);
            if(!initCheck) {
                 ALOGE("deleting Tunnel Player - initCheck failed");
                 delete mAudioPlayer;
                 mAudioPlayer = NULL;
            }
        }
    }
    if(mAudioPlayer == NULL) {
        ALOGV("AudioPlayer created, Non-LPA mode mime %s duration %lld\n", mime, mDurationUs);
        mAudioPlayer = new AudioPlayer(mAudioSink, flags, this);
    }
#else
    mAudioPlayer = new AudioPlayer(mAudioSink, flags, this);
#endif
    mAudioPlayer->setSource(mAudioSource);

    mTimeSource = mAudioPlayer;

    // If there was a seek request before we ever started,
    // honor the request now.
    // Make sure to do this before starting the audio player
    // to avoid a race condition.
    seekAudioIfNecessary_l();
}

void AwesomePlayer::notifyIfMediaStarted_l() {
    if (mMediaRenderingStartGeneration == mStartGeneration) {
        mMediaRenderingStartGeneration = -1;
        notifyListener_l(MEDIA_STARTED);
    }
}

status_t AwesomePlayer::startAudioPlayer_l(bool sendErrorNotification) {
    CHECK(!(mFlags & AUDIO_RUNNING));
    status_t err = OK;

    if (mAudioSource == NULL || mAudioPlayer == NULL) {
        return OK;
    }

    if (mOffloadAudio) {
        mQueue.cancelEvent(mAudioTearDownEvent->eventID());
        mAudioTearDownEventPending = false;
    }

    if (!(mFlags & AUDIOPLAYER_STARTED)) {
        bool wasSeeking = mAudioPlayer->isSeeking();

        // We've already started the MediaSource in order to enable
        // the prefetcher to read its data.
        err = mAudioPlayer->start(
                true /* sourceAlreadyStarted */);

        if (err != OK) {
            ALOGE("AudioPlayer start error");
            if (sendErrorNotification) {
                notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
            }

            return err;
        }

        modifyFlags(AUDIOPLAYER_STARTED, SET);

        if (wasSeeking) {
            CHECK(!mAudioPlayer->isSeeking());

            // We will have finished the seek while starting the audio player.
            postAudioSeekComplete();
        } else {
            notifyIfMediaStarted_l();
        }
    } else {
        err = mAudioPlayer->resume();
    }

    if (err == OK) {
        modifyFlags(AUDIO_RUNNING, SET);

        mWatchForAudioEOS = true;
    }

    return err;
}

void AwesomePlayer::notifyVideoSize_l() {
    ATRACE_CALL();
    sp<MetaData> meta = mVideoSource->getFormat();

    int32_t cropLeft, cropTop, cropRight, cropBottom;
    if (!meta->findRect(
                kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom)) {
        int32_t width, height;
        CHECK(meta->findInt32(kKeyWidth, &width));
        CHECK(meta->findInt32(kKeyHeight, &height));

        cropLeft = cropTop = 0;
        cropRight = width - 1;
        cropBottom = height - 1;

        ALOGV("got dimensions only %d x %d", width, height);
    } else {
        ALOGV("got crop rect %d, %d, %d, %d",
             cropLeft, cropTop, cropRight, cropBottom);
    }

    int32_t displayWidth;
    if (meta->findInt32(kKeyDisplayWidth, &displayWidth)) {
        ALOGV("Display width changed (%d=>%d)", mDisplayWidth, displayWidth);
        mDisplayWidth = displayWidth;
    }
    int32_t displayHeight;
    if (meta->findInt32(kKeyDisplayHeight, &displayHeight)) {
        ALOGV("Display height changed (%d=>%d)", mDisplayHeight, displayHeight);
        mDisplayHeight = displayHeight;
    }

    int32_t usableWidth = cropRight - cropLeft + 1;
    int32_t usableHeight = cropBottom - cropTop + 1;
    if (mDisplayWidth != 0) {
        usableWidth = mDisplayWidth;
    }
    if (mDisplayHeight != 0) {
        usableHeight = mDisplayHeight;
    }

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mVideoWidth = usableWidth;
        mStats.mVideoHeight = usableHeight;
    }

    int32_t rotationDegrees;
    if (!mVideoTrack->getFormat()->findInt32(
                kKeyRotation, &rotationDegrees)) {
        rotationDegrees = 0;
    }

    if (rotationDegrees == 90 || rotationDegrees == 270) {
        notifyListener_l(
                MEDIA_SET_VIDEO_SIZE, usableHeight, usableWidth);
    } else {
        notifyListener_l(
                MEDIA_SET_VIDEO_SIZE, usableWidth, usableHeight);
    }
}

void AwesomePlayer::initRenderer_l() {
    ATRACE_CALL();

    if (mNativeWindow == NULL) {
        return;
    }

    sp<MetaData> meta = mVideoSource->getFormat();

    int32_t format;
    const char *component;
    int32_t decodedWidth, decodedHeight;
    CHECK(meta->findInt32(kKeyColorFormat, &format));
    CHECK(meta->findCString(kKeyDecoderComponent, &component));
    CHECK(meta->findInt32(kKeyWidth, &decodedWidth));
    CHECK(meta->findInt32(kKeyHeight, &decodedHeight));

    int32_t rotationDegrees;
    if (!mVideoTrack->getFormat()->findInt32(
                kKeyRotation, &rotationDegrees)) {
        rotationDegrees = 0;
    }

    mVideoRenderer.clear();

    // Must ensure that mVideoRenderer's destructor is actually executed
    // before creating a new one.
    IPCThreadState::self()->flushCommands();

    // Even if set scaling mode fails, we will continue anyway
    setVideoScalingMode_l(mVideoScalingMode);
    if (USE_SURFACE_ALLOC
            && !strncmp(component, "OMX.", 4)
            && strncmp(component, "OMX.ffmpeg.", 11)
            && strncmp(component, "OMX.google.", 11)) {
        // Hardware decoders avoid the CPU color conversion by decoding
        // directly to ANativeBuffers, so we must use a renderer that
        // just pushes those buffers to the ANativeWindow.
        mVideoRenderer =
            new AwesomeNativeWindowRenderer(mNativeWindow, rotationDegrees);
        if (mVSyncLocker == NULL && VSyncLocker::isSyncRenderEnabled()) {
            mVSyncLocker = new VSyncLocker();
            mVSyncLocker->start();
        }
    } else {
        // Other decoders are instantiated locally and as a consequence
        // allocate their buffers in local address space.  This renderer
        // then performs a color conversion and copy to get the data
        // into the ANativeBuffer.
        mVideoRenderer = new AwesomeLocalRenderer(mNativeWindow, meta);
    }
}

status_t AwesomePlayer::pause() {
    ATRACE_CALL();

    Mutex::Autolock autoLock(mLock);

    modifyFlags(CACHE_UNDERRUN, CLEAR);

    return pause_l();
}

status_t AwesomePlayer::pause_l(bool at_eos) {
    if (!(mFlags & PLAYING)) {
        if (mAudioTearDown && mAudioTearDownWasPlaying) {
            ALOGV("pause_l() during teardown and finishSetDataSource_l() mFlags %x" , mFlags);
            mAudioTearDownWasPlaying = false;
            notifyListener_l(MEDIA_PAUSED);
            mMediaRenderingStartGeneration = ++mStartGeneration;
        }
        return OK;
    }

    notifyListener_l(MEDIA_PAUSED);
    mMediaRenderingStartGeneration = ++mStartGeneration;

    cancelPlayerEvents(true /* keepNotifications */);

    if (mAudioPlayer != NULL && (mFlags & AUDIO_RUNNING)) {
        // If we played the audio stream to completion we
        // want to make sure that all samples remaining in the audio
        // track's queue are played out.
        mAudioPlayer->pause(at_eos /* playPendingSamples */);
        // send us a reminder to tear down the AudioPlayer if paused for too long.
        if (mOffloadAudio) {
            ALOGD("copl: pause, arm a tear down timer for %lld us", kOffloadPauseMaxUs);
            postAudioTearDownEvent(kOffloadPauseMaxUs);
        }
        modifyFlags(AUDIO_RUNNING, CLEAR);
    }

    if (mFlags & TEXTPLAYER_INITIALIZED) {
        mTextDriver->pause();
        modifyFlags(TEXT_RUNNING, CLEAR);
    }

    modifyFlags(PLAYING, CLEAR);

    if (mDecryptHandle != NULL) {
        mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                Playback::PAUSE, 0);
    }

    if(!(mFlags & VIDEO_AT_EOS)){
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mLastPausedTimeMs = mVideoTimeUs/1000;
        printStats();
    }

    uint32_t params = IMediaPlayerService::kBatteryDataTrackDecoder;
    if ((mAudioSource != NULL) && (mAudioSource != mAudioTrack)) {
        params |= IMediaPlayerService::kBatteryDataTrackAudio;
    }
    if (mVideoSource != NULL) {
        params |= IMediaPlayerService::kBatteryDataTrackVideo;
    }

    addBatteryData(params);

    return OK;
}

bool AwesomePlayer::isPlaying() const {
    return (mFlags & PLAYING) || (mFlags & CACHE_UNDERRUN);
}

status_t AwesomePlayer::setSurfaceTexture(const sp<IGraphicBufferProducer> &bufferProducer) {
    Mutex::Autolock autoLock(mLock);

    status_t err;
    if (bufferProducer != NULL) {
        err = setNativeWindow_l(new Surface(bufferProducer));
    } else {
        err = setNativeWindow_l(NULL);
    }

    return err;
}

void AwesomePlayer::shutdownVideoDecoder_l() {
    if (mVideoBuffer) {
        mVideoBuffer->release();
        mVideoBuffer = NULL;
    }

    mVideoSource->stop();

    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    wp<MediaSource> tmp = mVideoSource;
    mVideoSource.clear();
    while (tmp.promote() != NULL) {
        usleep(1000);
    }
    IPCThreadState::self()->flushCommands();
    ALOGV("video decoder shutdown completed");
}

status_t AwesomePlayer::setNativeWindow_l(const sp<ANativeWindow> &native) {
    mNativeWindow = native;

    mQueue.cancelEvent(mCheckAudioStatusEvent->eventID());
    mAudioStatusEventPending = false;

    if (mVideoSource == NULL) {
        return OK;
    }

    ALOGV("attempting to reconfigure to use new surface");

    bool wasPlaying = (mFlags & PLAYING) != 0;

    pause_l();
    mVideoRenderer.clear();

    shutdownVideoDecoder_l();

    status_t err = initVideoDecoder();

    if (err != OK) {
        ALOGE("failed to reinstantiate video decoder after surface change.");
        return err;
    }

    if (mLastVideoTimeUs >= 0) {
        mWatchForAudioSeekComplete = false;
        mSeeking = SEEK;
        mSeekTimeUs = mLastVideoTimeUs;
        modifyFlags((AT_EOS | AUDIO_AT_EOS | VIDEO_AT_EOS), CLEAR);
    }

    if (wasPlaying) {
        play_l();
    }

    return OK;
}

void AwesomePlayer::setAudioSink(
        const sp<MediaPlayerBase::AudioSink> &audioSink) {
    Mutex::Autolock autoLock(mLock);

    mAudioSink = audioSink;
}

status_t AwesomePlayer::setLooping(bool shouldLoop) {
    Mutex::Autolock autoLock(mLock);

    modifyFlags(LOOPING, CLEAR);

    if (shouldLoop) {
        modifyFlags(LOOPING, SET);
    }

    return OK;
}

status_t AwesomePlayer::getDuration(int64_t *durationUs) {
    Mutex::Autolock autoLock(mMiscStateLock);

    if (mDurationUs < 0) {
        return UNKNOWN_ERROR;
    }

    *durationUs = mDurationUs;

    return OK;
}

status_t AwesomePlayer::getPosition(int64_t *positionUs) {
    if (mSeeking != NO_SEEK) {
        *positionUs = mSeekTimeUs;
    } else if (mVideoSource != NULL
            && (mAudioPlayer == NULL || !(mFlags & VIDEO_AT_EOS))) {
        Mutex::Autolock autoLock(mMiscStateLock);
        *positionUs = mVideoTimeUs;
    } else if (mAudioPlayer != NULL) {
        *positionUs = mAudioPlayer->getMediaTimeUs();
    } else {
        *positionUs = mAudioTearDownPosition;
    }
    return OK;
}

status_t AwesomePlayer::seekTo(int64_t timeUs) {
    ATRACE_CALL();

    if (((timeUs == 0) && (mExtractorFlags & MediaExtractor::CAN_SEEK_TO_ZERO)) ||
        (mExtractorFlags & MediaExtractor::CAN_SEEK)) {
        Mutex::Autolock autoLock(mLock);
        return seekTo_l(timeUs);
    } else {
        ALOGV("Extractor cannot seek, post seek complete");
        Mutex::Autolock autoLock(mLock);
        notifyListener_l(MEDIA_SEEK_COMPLETE);
    }

    return OK;
}

status_t AwesomePlayer::seekTo_l(int64_t timeUs) {
    if (mFlags & CACHE_UNDERRUN) {
        modifyFlags(CACHE_UNDERRUN, CLEAR);
        play_l();
    }

    if ((mFlags & PLAYING) && mVideoSource != NULL && (mFlags & VIDEO_AT_EOS)) {
        // Video playback completed before, there's no pending
        // video event right now. In order for this new seek
        // to be honored, we need to post one.

        postVideoEvent_l();
    }

    mSeeking = SEEK;

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mSeekDelayStartUs = getTimeOfDayUs();
    }
    mSeekNotificationSent = false;
    mSeekTimeUs = timeUs;
    modifyFlags((AT_EOS | AUDIO_AT_EOS | VIDEO_AT_EOS), CLEAR);

    if (mFlags & PLAYING) {
        notifyListener_l(MEDIA_PAUSED);
        mMediaRenderingStartGeneration = ++mStartGeneration;
    }

    seekAudioIfNecessary_l();

    if (mFlags & TEXTPLAYER_INITIALIZED) {
        mTextDriver->seekToAsync(mSeekTimeUs);
    }

    if (!(mFlags & PLAYING)) {
        ALOGV("seeking while paused, sending SEEK_COMPLETE notification"
             " immediately.");

        notifyListener_l(MEDIA_SEEK_COMPLETE);
        mSeekNotificationSent = true;

        if ((mFlags & PREPARED) && mVideoSource != NULL) {
            modifyFlags(SEEK_PREVIEW, SET);
            postVideoEvent_l();
        }
    }

    mReadRetry = false;
    return OK;
}

void AwesomePlayer::seekAudioIfNecessary_l() {
    if (mSeeking != NO_SEEK && mVideoSource == NULL && mAudioPlayer != NULL) {
        mAudioPlayer->seekTo(mSeekTimeUs);

        mWatchForAudioSeekComplete = true;
        mWatchForAudioEOS = true;

        if (mDecryptHandle != NULL) {
            mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                    Playback::PAUSE, 0);
            mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                    Playback::START, mSeekTimeUs / 1000);
        }
    }
}

void AwesomePlayer::setAudioSource(sp<MediaSource> source) {
    CHECK(source != NULL);

    mAudioTrack = source;
}

void AwesomePlayer::addTextSource_l(size_t trackIndex, const sp<MediaSource>& source) {
    CHECK(source != NULL);

    if (mTextDriver == NULL) {
        mTextDriver = new TimedTextDriver(mListener);
    }

    mTextDriver->addInBandTextSource(trackIndex, source);
}

status_t AwesomePlayer::initAudioDecoder() {
    ATRACE_CALL();

    sp<MetaData> meta = mAudioTrack->getFormat();
    sp<MetaData> vMeta;
    if (mVideoTrack != NULL && mVideoSource != NULL) {
        vMeta = mVideoTrack->getFormat();
    }

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));
    // Check whether there is a hardware codec for this stream
    // This doesn't guarantee that the hardware has a free stream
    // but it avoids us attempting to open (and re-open) an offload
    // stream to hardware that doesn't have the necessary codec
    audio_stream_type_t streamType = AUDIO_STREAM_MUSIC;
    if (mAudioSink != NULL) {
        streamType = mAudioSink->getAudioStreamType();
    }

    mOffloadAudio = canOffloadStream(meta, (mVideoSource != NULL), vMeta,
                                     (isStreamingHTTP() || isWidevineContent()),
                                     streamType);

#ifdef QCOM_DIRECTTRACK
    int32_t nchannels = 0;
    int32_t isADTS = 0;
    meta->findInt32( kKeyChannelCount, &nchannels );
    meta->findInt32(kKeyIsADTS, &isADTS);
    if (isADTS == 1) {
        ALOGV("Widevine content\n");
    }

    ALOGV("nchannels %d;LPA will be skipped if nchannels is > 2 or nchannels == 0",
           nchannels);

#ifdef USE_TUNNEL_MODE
    char tunnelDecode[PROPERTY_VALUE_MAX];
    property_get("tunnel.decode",tunnelDecode,"0");
    // Enable tunnel mode for mp3 and aac and if the clip is not aac adif
    // and if no other tunnel mode instances aare running.
    ALOGD("Tunnel Mime Type: %s, object alive = %d, mTunnelAliveAP = %d",\
            mime, (TunnelPlayer::mTunnelObjectsAlive), mTunnelAliveAP);

    bool sys_prop_enabled = !strcmp("true",tunnelDecode) || atoi(tunnelDecode);
    ALOGD("maxPossible tunnels = %d", TunnelPlayer::getTunnelObjectsAliveMax());
    //widevine will fallback to software decoder
    if (sys_prop_enabled && (TunnelPlayer::mTunnelObjectsAlive < TunnelPlayer::getTunnelObjectsAliveMax()) &&
       (mTunnelAliveAP < TunnelPlayer::getTunnelObjectsAliveMax()) && (isADTS == 0) &&
        mAudioSink->realtime() &&
        inSupportedTunnelFormats(mime)) {

        if (mVideoSource != NULL) {
           char tunnelAVDecode[PROPERTY_VALUE_MAX];
           property_get("tunnel.audiovideo.decode",tunnelAVDecode,"0");
           sys_prop_enabled = !strncmp("true", tunnelAVDecode, 4) || atoi(tunnelAVDecode);
           if (sys_prop_enabled) {
               ALOGD("Enable Tunnel Mode for A-V playback");
               mIsTunnelAudio = true;
           }
        }
        else {
            ALOGI("Tunnel Mode Audio Enabled");
            mIsTunnelAudio = true;
        }
#ifdef NO_TUNNEL_MODE_FOR_MULTICHANNEL
        if (nchannels > 2 || nchannels <= 0) {
            ALOGD("Use tunnel mode only for mono and stereo channels");
            mIsTunnelAudio = false;
        }
#endif
    }
    else
       ALOGD("Normal Audio Playback");
#endif

    checkTunnelExceptions();

    if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW) ||
             (mIsTunnelAudio
#ifdef USE_TUNNEL_MODE
             && (mTunnelAliveAP < TunnelPlayer::getTunnelObjectsAliveMax())
#endif
    )) {
        ALOGD("Set Audio Track as Audio Source");
        if(mIsTunnelAudio) {
            mTunnelAliveAP++;
        }
        mAudioSource = mAudioTrack;
    } else {
        // For LPA Playback use the decoder without OMX layer
        char *matchComponentName = NULL;
        int64_t durationUs;
        uint32_t flags = 0;
        char lpaDecode[128];
        uint32_t minDurationForLPA = LPA_MIN_DURATION_USEC_DEFAULT;
        char minUserDefDuration[PROPERTY_VALUE_MAX];
        property_get("lpa.decode",lpaDecode,"0");
        property_get("lpa.min_duration",minUserDefDuration,"LPA_MIN_DURATION_USEC_DEFAULT");
        minDurationForLPA = atoi(minUserDefDuration);
        if(minDurationForLPA < LPA_MIN_DURATION_USEC_ALLOWED) {
            ALOGE("LPAPlayer::Clip duration setting of less than 30sec not supported, defaulting to 60sec");
            minDurationForLPA = LPA_MIN_DURATION_USEC_DEFAULT;
        }
        if (mAudioTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
            Mutex::Autolock autoLock(mMiscStateLock);
            if (mDurationUs < 0 || durationUs > mDurationUs) {
                mDurationUs = durationUs;
            }
        }
        if ( mDurationUs > minDurationForLPA
             && (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG) || !strcasecmp(mime,MEDIA_MIMETYPE_AUDIO_AAC))
             && LPAPlayer::mObjectsAlive == 0 && mVideoSource == NULL && (strcmp("true",lpaDecode) == 0)
             && (nchannels && (nchannels <= 2)) ) {
            char nonOMXDecoder[128];
            if(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
                ALOGD("matchComponentName is set to MP3Decoder %lld, mime %s",mDurationUs,mime);
                property_get("use.non-omx.mp3.decoder",nonOMXDecoder,"0");
                if((strcmp("true",nonOMXDecoder) == 0)) {
                    matchComponentName = (char *) "MP3Decoder";
                }
            } else if((!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC))) {
                ALOGD("matchComponentName is set to AACDecoder %lld, mime %s",mDurationUs,mime);
                property_get("use.non-omx.aac.decoder",nonOMXDecoder,"0");
                if((strcmp("true",nonOMXDecoder) == 0)) {
                    matchComponentName = (char *) "AACDecoder";
                } else {
                    matchComponentName = (char *) "OMX.google.aac.decoder";
                }
            }
            flags |= OMXCodec::kSoftwareCodecsOnly;
        }
        mAudioSource = OMXCodec::Create(
                mClient.interface(), mAudioTrack->getFormat(),
                false, // createEncoder
                mAudioTrack, matchComponentName, flags,NULL);
#else
    if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
        ALOGV("createAudioPlayer: bypass OMX (raw)");
        mAudioSource = mAudioTrack;
        // For PCM offload fallback
        if (mOffloadAudio) {
            mOmxSource = mAudioSource;
        }
    } else {
        // If offloading we still create a OMX decoder as a fall-back
        // but we don't start it
        mOmxSource = OMXCodec::Create(
                mClient.interface(), mAudioTrack->getFormat(),
                false, // createEncoder
                mAudioTrack);
#endif

        if (mOffloadAudio) {
            ALOGD("use compress offload playback path(copl)");
            mAudioSource = mAudioTrack;
#ifndef QCOM_DIRECTTRACK
        } else {
            mAudioSource = mOmxSource;
#endif
        }
    }

    int64_t durationUs = -1;
    mAudioTrack->getFormat()->findInt64(kKeyDuration, &durationUs);

    if (!mOffloadAudio && mAudioSource != NULL) {
        ALOGI("Could not offload audio decode, try pcm offload");
        sp<MetaData> format = mAudioSource->getFormat();
        if (durationUs >= 0) {
            format->setInt64(kKeyDuration, durationUs);
        }
        mOffloadAudio = canOffloadStream(format, (mVideoSource != NULL), vMeta,
                                    (isStreamingHTTP() || isWidevineContent()),
                                     streamType);
    }

    if (mAudioSource != NULL) {
        if (durationUs >= 0) {
            Mutex::Autolock autoLock(mMiscStateLock);
            if (mDurationUs < 0 || durationUs > mDurationUs) {
                mDurationUs = durationUs;
            }
        }

        status_t err = mAudioSource->start();

        if (err != OK) {
            mAudioSource.clear();
            mOmxSource.clear();
            return err;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_QCELP)) {
        // For legacy reasons we're simply going to ignore the absence
        // of an audio decoder for QCELP instead of aborting playback
        // altogether.
        return OK;
    }

    if (mAudioSource != NULL) {
        Mutex::Autolock autoLock(mStatsLock);
        TrackStat *stat = &mStats.mTracks.editItemAt(mStats.mAudioTrackIndex);
        const char *component;
        if (!mAudioSource->getFormat()
                ->findCString(kKeyDecoderComponent, &component)) {
            component = "none";
        }

        stat->mDecoderName = component;
    }

    return mAudioSource != NULL ? OK : UNKNOWN_ERROR;
}

void AwesomePlayer::setVideoSource(sp<MediaSource> source) {
    CHECK(source != NULL);

    mVideoTrack = source;
}

status_t AwesomePlayer::initVideoDecoder(uint32_t flags) {
    ATRACE_CALL();

    // Either the application or the DRM system can independently say
    // that there must be a hardware-protected path to an external video sink.
    // For now we always require a hardware-protected path to external video sink
    // if content is DRMed, but eventually this could be optional per DRM agent.
    // When the application wants protection, then
    //   (USE_SURFACE_ALLOC && (mSurface != 0) &&
    //   (mSurface->getFlags() & ISurfaceComposer::eProtectedByApp))
    // will be true, but that part is already handled by SurfaceFlinger.

#ifdef DEBUG_HDCP
    // For debugging, we allow a system property to control the protected usage.
    // In case of uninitialized or unexpected property, we default to "DRM only".
    bool setProtectionBit = false;
    char value[PROPERTY_VALUE_MAX];
    if (property_get("persist.sys.hdcp_checking", value, NULL)) {
        if (!strcmp(value, "never")) {
            // nop
        } else if (!strcmp(value, "always")) {
            setProtectionBit = true;
        } else if (!strcmp(value, "drm-only")) {
            if (mDecryptHandle != NULL) {
                setProtectionBit = true;
            }
        // property value is empty, or unexpected value
        } else {
            if (mDecryptHandle != NULL) {
                setProtectionBit = true;
            }
        }
    // can' read property value
    } else {
        if (mDecryptHandle != NULL) {
            setProtectionBit = true;
        }
    }
    // note that usage bit is already cleared, so no need to clear it in the "else" case
    if (setProtectionBit) {
        flags |= OMXCodec::kEnableGrallocUsageProtected;
    }
#else
    if (mDecryptHandle != NULL) {
        flags |= OMXCodec::kEnableGrallocUsageProtected;
    }
#endif

    {
        char value[PROPERTY_VALUE_MAX];
        property_get("sys.media.vdec.sw", value, "0");
        if (atoi(value)) {
            ALOGI("Software Codec is preferred for Video");
            flags |= OMXCodec::kPreferSoftwareCodecs;
        }

        mDropFramesDisable = false;
        property_get("sys.media.vdec.drop", value, "1");
        if (!atoi(value)) {
            ALOGI("Don't drop frame even if late");
            mDropFramesDisable = true;
        }
    }

    ALOGV("initVideoDecoder flags=0x%x", flags);
    mVideoSource = OMXCodec::Create(
            mClient.interface(), mVideoTrack->getFormat(),
            false, // createEncoder
            mVideoTrack,
            NULL, flags, USE_SURFACE_ALLOC ? mNativeWindow : NULL);

    if (mVideoSource != NULL) {
        int64_t durationUs;
        if (mVideoTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
            Mutex::Autolock autoLock(mMiscStateLock);
            if (mDurationUs < 0 || durationUs > mDurationUs) {
                mDurationUs = durationUs;
            }
        }

        status_t err = mVideoSource->start();

        if (err != OK) {
            ALOGE("failed to start video source");
            mVideoSource.clear();
            return err;
        }
    }

    if (mVideoSource != NULL) {
        const char *componentName;
        CHECK(mVideoSource->getFormat()
                ->findCString(kKeyDecoderComponent, &componentName));

        {
            Mutex::Autolock autoLock(mStatsLock);
            TrackStat *stat = &mStats.mTracks.editItemAt(mStats.mVideoTrackIndex);

            stat->mDecoderName = componentName;
        }

        static const char *kPrefix = "OMX.Nvidia.";
        static const char *kSuffix = ".decode";
        static const size_t kSuffixLength = strlen(kSuffix);

        size_t componentNameLength = strlen(componentName);

        if (!strncmp(componentName, kPrefix, strlen(kPrefix))
                && componentNameLength >= kSuffixLength
                && !strcmp(&componentName[
                    componentNameLength - kSuffixLength], kSuffix)) {
            modifyFlags(SLOW_DECODER_HACK, SET);
        }
    }

    return mVideoSource != NULL ? OK : UNKNOWN_ERROR;
}

void AwesomePlayer::finishSeekIfNecessary(int64_t videoTimeUs) {
    ATRACE_CALL();
    if (mSeeking != NO_SEEK)
    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mLastSeekToTimeMs = mSeekTimeUs/1000;
        printStats();
    }

    if (mSeeking == SEEK_VIDEO_ONLY) {
        mSeeking = NO_SEEK;
        return;
    }

    if (mSeeking == NO_SEEK || (mFlags & SEEK_PREVIEW)) {
        return;
    }

    // If we paused, then seeked, then resumed, it is possible that we have
    // signaled SEEK_COMPLETE at a copmletely different media time than where
    // we are now resuming.  Signal new position to media time provider.
    // Cannot signal another SEEK_COMPLETE, as existing clients may not expect
    // multiple SEEK_COMPLETE responses to a single seek() request.
    if (mSeekNotificationSent && abs(mSeekTimeUs - videoTimeUs) > 10000) {
        // notify if we are resuming more than 10ms away from desired seek time
        notifyListener_l(MEDIA_SKIPPED);
    }

    if (mAudioPlayer != NULL) {
        ALOGV("seeking audio to %lld us (%.2f secs).", videoTimeUs, videoTimeUs / 1E6);

        // If we don't have a video time, seek audio to the originally
        // requested seek time instead.

        mAudioPlayer->seekTo(videoTimeUs < 0 ? mSeekTimeUs : videoTimeUs);
        mWatchForAudioSeekComplete = true;
        mWatchForAudioEOS = true;
    } else if (!mSeekNotificationSent) {
        // If we're playing video only, report seek complete now,
        // otherwise audio player will notify us later.
        notifyListener_l(MEDIA_SEEK_COMPLETE);
        mSeekNotificationSent = true;
    }

    modifyFlags(FIRST_FRAME, SET);
    mSeeking = NO_SEEK;

    if (mDecryptHandle != NULL) {
        mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                Playback::PAUSE, 0);
        mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                Playback::START, videoTimeUs / 1000);
    }

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mLastSeekToTimeMs = mSeekTimeUs/1000;
        printStats();
    }
}

void AwesomePlayer::onVideoEvent() {
    ATRACE_CALL();
    Mutex::Autolock autoLock(mLock);
    if (!mVideoEventPending) {
        // The event has been cancelled in reset_l() but had already
        // been scheduled for execution at that time.
        return;
    }
    mVideoEventPending = false;

    {
        Mutex::Autolock autoLock(mStatsLock);
        if(!mStats.mVeryFirstFrame && mSeeking == NO_SEEK){
            mStats.mTotalTimeUs += getTimeOfDayUs() - mStats.mLastFrameUs;
        }
        mStats.mLastFrameUs = getTimeOfDayUs();
    }

    if ((mSeeking != NO_SEEK) && (mReadRetry == false)) {
        if (mVideoBuffer) {
            mVideoBuffer->release();
            mVideoBuffer = NULL;
        }

        if (mSeeking == SEEK && (isStreamingHTTP() || mOffloadAudio)
                && mAudioSource != NULL && !(mFlags & SEEK_PREVIEW)) {
            // We're going to seek the video source first, followed by
            // the audio source.
            // In order to avoid jumps in the DataSource offset caused by
            // the audio codec prefetching data from the old locations
            // while the video codec is already reading data from the new
            // locations, we'll "pause" the audio source, causing it to
            // stop reading input data until a subsequent seek.

            if (mAudioPlayer != NULL && (mFlags & AUDIO_RUNNING)) {
                mAudioPlayer->pause();

                modifyFlags(AUDIO_RUNNING, CLEAR);
            }
            mAudioSource->pause();
        }
    }

    if (!mVideoBuffer) {
        MediaSource::ReadOptions options;
        if (mSeeking != NO_SEEK) {
            ALOGV("seeking to %lld us (%.2f secs)", mSeekTimeUs, mSeekTimeUs / 1E6);

            MediaSource::ReadOptions::SeekMode seekmode = (mSeeking == SEEK_VIDEO_ONLY)
                                                          ? MediaSource::ReadOptions::SEEK_NEXT_SYNC
                                                          : MediaSource::ReadOptions::SEEK_CLOSEST_SYNC;
            // Seek to the next key-frame after resume for http streaming
            if (mCachedSource != NULL && mIsFirstFrameAfterResume) {
                seekmode = MediaSource::ReadOptions::SEEK_NEXT_SYNC;
                mIsFirstFrameAfterResume = false;
            }

            options.setSeekTo(
                    mSeekTimeUs,
                    seekmode);
            if (mVSyncLocker != NULL) {
                mVSyncLocker->resetProfile();
            }
        }
        for (;;) {
            status_t err = mVideoSource->read(&mVideoBuffer, &options);
            options.clearSeekTo();

            if(err == -EAGAIN) {
                mReadRetry = true;
                postVideoEvent_l(-1);
                return;
            }
            mReadRetry = false;
            if (err != OK) {
                CHECK(mVideoBuffer == NULL);

                if (err == INFO_FORMAT_CHANGED) {
                    ALOGV("VideoSource signalled format change.");

                    notifyVideoSize_l();

                    if (mVideoRenderer != NULL) {
                        mVideoRendererIsPreview = false;
                        initRenderer_l();
                    }
                    continue;
                }

                // So video playback is complete, but we may still have
                // a seek request pending that needs to be applied
                // to the audio track.
                if (mSeeking != NO_SEEK) {
                    ALOGV("video stream ended while seeking!");
                }
                finishSeekIfNecessary(-1);

                if (mAudioPlayer != NULL
                        && !(mFlags & (AUDIO_RUNNING | SEEK_PREVIEW))) {
                    startAudioPlayer_l();
                }

                modifyFlags(VIDEO_AT_EOS, SET);
                postStreamDoneEvent_l(err);
                return;
            }

            if (mVideoBuffer->range_length() == 0) {
                // Some decoders, notably the PV AVC software decoder
                // return spurious empty buffers that we just want to ignore.

                mVideoBuffer->release();
                mVideoBuffer = NULL;
                continue;
            }

#ifdef QCOM_HARDWARE
            if (mCustomAVSync) {
                int width = 0;
                int height = 0;
                sp<MetaData> meta = mVideoSource->getFormat();
                CHECK(meta->findInt32(kKeyWidth, &width));
                CHECK(meta->findInt32(kKeyHeight, &height));

                if (((height * width) >= (720 * 1280)) && (mStats.mConsecutiveFramesDropped >= 5) && !(mFlags & NO_AVSYNC))
                {
                    ALOGE("DISABLED AVSync as there are 5 consecutive frame drops");
                    modifyFlags(NO_AVSYNC,SET);
                }
            }
#endif

            break;
        }

        {
            Mutex::Autolock autoLock(mStatsLock);
            ++mStats.mNumVideoFramesDecoded;
        }
    }

    int64_t timeUs;
    CHECK(mVideoBuffer->meta_data()->findInt64(kKeyTime, &timeUs));

    mLastVideoTimeUs = timeUs;

    if (mSeeking == SEEK_VIDEO_ONLY) {
        if (mSeekTimeUs > timeUs) {
            ALOGI("XXX mSeekTimeUs = %lld us, timeUs = %lld us",
                 mSeekTimeUs, timeUs);
        }
    }

    {
        Mutex::Autolock autoLock(mMiscStateLock);
        mVideoTimeUs = timeUs;
    }

    SeekType wasSeeking = mSeeking;
    finishSeekIfNecessary(timeUs);

    if (mAudioPlayer != NULL && !(mFlags & (AUDIO_RUNNING | SEEK_PREVIEW))) {
        status_t err = startAudioPlayer_l(false /* sendErrorNotification */);
        if ((err != OK) && mOffloadAudio) {
            err = fallbackToSWDecoder();
        }

        if (err != OK) {
            ALOGE("Failed to fallback to SW decoder err = %d", err);
            notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);

            delete mAudioPlayer;
            mAudioPlayer = NULL;

            modifyFlags((PLAYING | FIRST_FRAME), CLEAR);

            if (mDecryptHandle != NULL) {
               mDrmManagerClient->setPlaybackStatus(
                       mDecryptHandle, Playback::STOP, 0);
            }
            return;
        }
    }

    if ((mFlags & TEXTPLAYER_INITIALIZED)
            && !(mFlags & (TEXT_RUNNING | SEEK_PREVIEW))) {
        mTextDriver->start();
        modifyFlags(TEXT_RUNNING, SET);
    }

    TimeSource *ts =
        ((mFlags & AUDIO_AT_EOS) || !(mFlags & AUDIOPLAYER_STARTED))
            ? &mSystemTimeSource : mTimeSource;
    int64_t systemTimeUs = mSystemTimeSource.getRealTimeUs();
    int64_t looperTimeUs = ALooper::GetNowUs();

    if (mFlags & FIRST_FRAME) {
        modifyFlags(FIRST_FRAME, CLEAR);
        mSinceLastDropped = 0;

        mClockEstimator->reset();
        mTimeSourceDeltaUs = estimateRealTimeUs(ts, systemTimeUs) - timeUs;

        {
            Mutex::Autolock autoLock(mStatsLock);
            if(mStats.mVeryFirstFrame){
                logFirstFrame();
                printStats();
                mStats.mLastFrameUs = getTimeOfDayUs();
            }
        }
    }

    int64_t realTimeUs, mediaTimeUs, nowUs = 0, latenessUs = 0;
    if (!(mFlags & AUDIO_AT_EOS) && mAudioPlayer != NULL
        && mAudioPlayer->getMediaTimeMapping(&realTimeUs, &mediaTimeUs)) {
        ATRACE_INT("TS delta change (ms)", (mTimeSourceDeltaUs - (realTimeUs - mediaTimeUs)) / 1E3);
        mTimeSourceDeltaUs = realTimeUs - mediaTimeUs;
    }

    if (wasSeeking == SEEK_VIDEO_ONLY) {
        nowUs = estimateRealTimeUs(ts, systemTimeUs) - mTimeSourceDeltaUs;

        latenessUs = nowUs - timeUs;

        ATRACE_INT("Video Lateness (ms)", latenessUs / 1E3);

        if (latenessUs > 0) {
            ALOGI("after SEEK_VIDEO_ONLY we're late by %.2f secs", latenessUs / 1E6);
        }
    }

    if (wasSeeking == NO_SEEK) {
        // Let's display the first frame after seeking right away.

        nowUs = estimateRealTimeUs(ts, systemTimeUs) - mTimeSourceDeltaUs;

        latenessUs = nowUs - timeUs;

        if (latenessUs >= 0) {
            ATRACE_INT("Video Lateness (ms)", latenessUs / 1E3);
        } else {
            ATRACE_INT("Video Earlyness (ms)", -latenessUs / 1E3);
        }

        if (latenessUs > 500000ll
                && mAudioPlayer != NULL
                && mAudioPlayer->getMediaTimeMapping(
                    &realTimeUs, &mediaTimeUs)) {
            if (mWVMExtractor == NULL) {
                ALOGI("we're much too late (%.2f secs), video skipping ahead",
                     latenessUs / 1E6);

                mVideoBuffer->release();
                mVideoBuffer = NULL;

                mSeeking = SEEK_VIDEO_ONLY;
                mSeekTimeUs = mediaTimeUs;

                postVideoEvent_l();
                return;
            } else {
                // The widevine extractor doesn't deal well with seeking
                // audio and video independently. We'll just have to wait
                // until the decoder catches up, which won't be long at all.
                ALOGI("we're very late (%.2f secs)", latenessUs / 1E6);
            }
        }

#ifdef QCOM_HARDWARE
        if ((latenessUs < mLateAVSyncMargin) && (mFlags & NO_AVSYNC))
        {
            ALOGE("ENABLED AVSync as the video frames are intime with audio");
            modifyFlags(NO_AVSYNC,CLEAR);
        }
#endif

        if (latenessUs > mLateAVSyncMargin) {
            // We're more than 40ms late.
            ALOGV("we're late by %lld us (%.2f secs)",
                 latenessUs, latenessUs / 1E6);

            if ((!(mFlags & SLOW_DECODER_HACK)
                    || (mSinceLastDropped > FRAME_DROP_FREQ))
#ifdef QCOM_HARDWARE
                    && !(mFlags & NO_AVSYNC)
#endif
                    && !mDropFramesDisable)
            {
                ALOGV("we're late by %lld us (%.2f secs) dropping "
                     "one after %d frames",
                     latenessUs, latenessUs / 1E6, mSinceLastDropped);

                mSinceLastDropped = 0;
                mVideoBuffer->release();
                mVideoBuffer = NULL;

                {
                    Mutex::Autolock autoLock(mStatsLock);
                    ++mStats.mNumVideoFramesDropped;
                    mStats.mConsecutiveFramesDropped++;
                    if(mVSyncLocker != NULL) {
                        mVSyncLocker->blockSync();
                    }
                    if (mStats.mConsecutiveFramesDropped == 1){
                        mStats.mCatchupTimeStart = mTimeSource->getRealTimeUs();
                    }
                    if(!(mFlags & AT_EOS)) logLate(timeUs,nowUs,latenessUs);
                }

                postVideoEvent_l(0);
                return;
            }
        }

        if (latenessUs < -30000) {
            logOnTime(timeUs,nowUs,latenessUs);
            {
                Mutex::Autolock autoLock(mStatsLock);
                mStats.mConsecutiveFramesDropped = 0;
            }
            // We're more than 30ms early, schedule at most 20 ms before time due
            postVideoEvent_l(latenessUs < -60000 ? 30000 : -latenessUs - 20000);
            return;
        }
    }

    if ((mNativeWindow != NULL)
            && (mVideoRendererIsPreview || mVideoRenderer == NULL)) {
        mVideoRendererIsPreview = false;

        initRenderer_l();
    }

    if (mVideoRenderer != NULL) {
        mSinceLastDropped++;

        if (mVSyncLocker != NULL) {
            mVSyncLocker->blockOnVSync();
        }

        mVideoBuffer->meta_data()->setInt64(kKeyTime, looperTimeUs - latenessUs);

        mVideoRenderer->render(mVideoBuffer);
        if (!mVideoRenderingStarted) {
            mVideoRenderingStarted = true;
            notifyListener_l(MEDIA_INFO, MEDIA_INFO_RENDERING_START);
        }

        if (mFlags & PLAYING) {
            notifyIfMediaStarted_l();
        }

        {
            Mutex::Autolock autoLock(mStatsLock);
            logOnTime(timeUs,nowUs,latenessUs);
            mStats.mTotalFrames++;
            mStats.mConsecutiveFramesDropped = 0;
            char value[PROPERTY_VALUE_MAX];
            property_get("persist.debug.sf.statistics", value, "0");
            if (atoi(value) && mVideoSource != NULL) {
                if (mStats.mResumeDelayStartUs > 0) {
                    printStats();
                    ALOGI("Resume Latency = %lld us", getTimeOfDayUs() - mStats.mResumeDelayStartUs);
                    mStats.mResumeDelayStartUs = -1;
                }
                if (mStats.mSeekDelayStartUs > 0) {
                    printStats();
                    ALOGI("Seek Latency = %lld us", getTimeOfDayUs() - mStats.mSeekDelayStartUs);
                    mStats.mSeekDelayStartUs = -1;
                }
            }
        }
    }

    mVideoBuffer->release();
    mVideoBuffer = NULL;

    if (wasSeeking != NO_SEEK && (mFlags & SEEK_PREVIEW)) {
        modifyFlags(SEEK_PREVIEW, CLEAR);
        return;
    }

    /* get next frame time */
    if (wasSeeking == NO_SEEK) {
        MediaSource::ReadOptions options;
        for (;;) {
            status_t err = mVideoSource->read(&mVideoBuffer, &options);
            if (err != OK) {
                // deal with any errors next time
                CHECK(mVideoBuffer == NULL);
                postVideoEvent_l(0);
                return;
            }

            if (mVideoBuffer->range_length() != 0) {
                break;
            }

            // Some decoders, notably the PV AVC software decoder
            // return spurious empty buffers that we just want to ignore.

            mVideoBuffer->release();
            mVideoBuffer = NULL;
        }

        {
            Mutex::Autolock autoLock(mStatsLock);
            ++mStats.mNumVideoFramesDecoded;
        }

        int64_t nextTimeUs;
        CHECK(mVideoBuffer->meta_data()->findInt64(kKeyTime, &nextTimeUs));
        systemTimeUs = mSystemTimeSource.getRealTimeUs();
        int64_t delayUs = nextTimeUs - estimateRealTimeUs(ts, systemTimeUs) + mTimeSourceDeltaUs;
        ATRACE_INT("Frame delta (ms)", (nextTimeUs - timeUs) / 1E3);
        // try to schedule 30ms before time due
        postVideoEvent_l(delayUs > 60000 ? 30000 : (delayUs < 30000 ? 0 : delayUs - 30000));
        return;
    }

    postVideoEvent_l();
}

int64_t AwesomePlayer::estimateRealTimeUs(TimeSource *ts, int64_t systemTimeUs) {
    if (ts == &mSystemTimeSource) {
        return systemTimeUs;
    } else {
        return (int64_t)mClockEstimator->estimate(systemTimeUs, ts->getRealTimeUs());
    }
}

void AwesomePlayer::postVideoEvent_l(int64_t delayUs) {
    ATRACE_CALL();

    if (mVideoEventPending) {
        return;
    }

    mVideoEventPending = true;
    mQueue.postEventWithDelay(mVideoEvent, delayUs < 0 ? 10000 : delayUs);
}

void AwesomePlayer::postStreamDoneEvent_l(status_t status) {
    if (mStreamDoneEventPending) {
        return;
    }
    mStreamDoneEventPending = true;

    mStreamDoneStatus = status;
    mQueue.postEvent(mStreamDoneEvent);
}

void AwesomePlayer::postBufferingEvent_l() {
    if (mBufferingEventPending) {
        return;
    }
    mBufferingEventPending = true;
    mQueue.postEventWithDelay(mBufferingEvent, 1000000ll);
}

void AwesomePlayer::postVideoLagEvent_l() {
    if (mVideoLagEventPending) {
        return;
    }
    mVideoLagEventPending = true;
    mQueue.postEventWithDelay(mVideoLagEvent, 1000000ll);
}

void AwesomePlayer::postCheckAudioStatusEvent(int64_t delayUs) {
    Mutex::Autolock autoLock(mAudioLock);
    if (mAudioStatusEventPending) {
        return;
    }
    mAudioStatusEventPending = true;

#ifdef EXYNOS4_ENHANCEMENTS
    /*
     * Do not honor delay when audio reached EOS
     * in order to change immediately time source from AudioPlayer to SystemTime
     */
    status_t finalStatus;
    if (mWatchForAudioEOS && mAudioPlayer->reachedEOS(&finalStatus)) {
        delayUs = 0;
    }
#endif

    // Do not honor delay when looping in order to limit audio gap
    if (mFlags & (LOOPING | AUTO_LOOPING)) {
        delayUs = 0;
    }
    mQueue.postEventWithDelay(mCheckAudioStatusEvent, delayUs);
}

void AwesomePlayer::postAudioTearDownEvent(int64_t delayUs) {
    Mutex::Autolock autoLock(mAudioLock);
    if (mAudioTearDownEventPending) {
        return;
    }
    mAudioTearDownEventPending = true;
    mQueue.postEventWithDelay(mAudioTearDownEvent, delayUs);
}

void AwesomePlayer::onCheckAudioStatus() {
    {
        Mutex::Autolock autoLock(mAudioLock);
        if (!mAudioStatusEventPending) {
            // Event was dispatched and while we were blocking on the mutex,
            // has already been cancelled.
            return;
        }

        mAudioStatusEventPending = false;
    }

    Mutex::Autolock autoLock(mLock);

    if (mWatchForAudioSeekComplete && !mAudioPlayer->isSeeking()) {
        mWatchForAudioSeekComplete = false;

        if (!mSeekNotificationSent) {
            notifyListener_l(MEDIA_SEEK_COMPLETE);
            mSeekNotificationSent = true;
        }

        if (mVideoSource == NULL) {
            // For video the mSeeking flag is always reset in finishSeekIfNecessary
            mSeeking = NO_SEEK;
        }

        notifyIfMediaStarted_l();
    }

    status_t finalStatus;
    if (mWatchForAudioEOS && mAudioPlayer->reachedEOS(&finalStatus)) {
        mWatchForAudioEOS = false;
        modifyFlags(AUDIO_AT_EOS, SET);
        modifyFlags(FIRST_FRAME, SET);
        postStreamDoneEvent_l(finalStatus);
    }
}

status_t AwesomePlayer::prepare() {
    ATRACE_CALL();
    Mutex::Autolock autoLock(mLock);
    return prepare_l();
}

status_t AwesomePlayer::prepare_l() {
    if (mFlags & PREPARED) {
        return OK;
    }

    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;
    }

    mIsAsyncPrepare = false;
    status_t err = prepareAsync_l();

    if (err != OK) {
        return err;
    }

    while (mFlags & PREPARING) {
        mPreparedCondition.wait(mLock);
    }

    return mPrepareResult;
}

status_t AwesomePlayer::prepareAsync() {
    ATRACE_CALL();
    Mutex::Autolock autoLock(mLock);

    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;  // async prepare already pending
    }

    mIsAsyncPrepare = true;
    return prepareAsync_l();
}

status_t AwesomePlayer::prepareAsync_l() {
    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;  // async prepare already pending
    }

    if (!mQueueStarted) {
        mQueue.start();
        mQueueStarted = true;
    }

    modifyFlags(PREPARING, SET);
    mAsyncPrepareEvent = new AwesomeEvent(
            this, &AwesomePlayer::onPrepareAsyncEvent);

    mQueue.postEvent(mAsyncPrepareEvent);

    return OK;
}

status_t AwesomePlayer::finishSetDataSource_l() {
    ATRACE_CALL();
    sp<DataSource> dataSource;

    bool isWidevineStreaming = false;
    if (!strncasecmp("widevine://", mUri.string(), 11)) {
        isWidevineStreaming = true;

        String8 newURI = String8("http://");
        newURI.append(mUri.string() + 11);

        mUri = newURI;
    }

    AString sniffedMIME;

    if (!strncasecmp("http://", mUri.string(), 7)
            || !strncasecmp("https://", mUri.string(), 8)
            || isWidevineStreaming) {
        mConnectingDataSource = HTTPBase::Create(
                (mFlags & INCOGNITO)
                    ? HTTPBase::kFlagIncognito
                    : 0);

        if (mUIDValid) {
            mConnectingDataSource->setUID(mUID);
        }

        String8 cacheConfig;
        bool disconnectAtHighwatermark;
        NuCachedSource2::RemoveCacheSpecificHeaders(
                &mUriHeaders, &cacheConfig, &disconnectAtHighwatermark);

        mLock.unlock();
        status_t err = mConnectingDataSource->connect(mUri, &mUriHeaders);
        mLock.lock();

        if (err != OK) {
            mConnectingDataSource.clear();

            ALOGI("mConnectingDataSource->connect() returned %d", err);
            return err;
        }

        if (!isWidevineStreaming) {
            // The widevine extractor does its own caching.

#if 0
            mCachedSource = new NuCachedSource2(
                    new ThrottledSource(
                        mConnectingDataSource, 50 * 1024 /* bytes/sec */));
#else
            mCachedSource = new NuCachedSource2(
                    mConnectingDataSource,
                    cacheConfig.isEmpty() ? NULL : cacheConfig.string(),
                    disconnectAtHighwatermark);
#endif

            dataSource = mCachedSource;
        } else {
            dataSource = mConnectingDataSource;
        }

        mConnectingDataSource.clear();

        String8 contentType = dataSource->getMIMEType();

        if (strncasecmp(contentType.string(), "audio/", 6)) {
            // We're not doing this for streams that appear to be audio-only
            // streams to ensure that even low bandwidth streams start
            // playing back fairly instantly.

            // We're going to prefill the cache before trying to instantiate
            // the extractor below, as the latter is an operation that otherwise
            // could block on the datasource for a significant amount of time.
            // During that time we'd be unable to abort the preparation phase
            // without this prefill.
            if (mCachedSource != NULL) {
                // We're going to prefill the cache before trying to instantiate
                // the extractor below, as the latter is an operation that otherwise
                // could block on the datasource for a significant amount of time.
                // During that time we'd be unable to abort the preparation phase
                // without this prefill.

                mLock.unlock();

                // Initially make sure we have at least 192 KB for the sniff
                // to complete without blocking.
                static const size_t kMinBytesForSniffing = 192 * 1024;

                off64_t metaDataSize = -1ll;
                for (;;) {
                    status_t finalStatus;
                    size_t cachedDataRemaining =
                        mCachedSource->approxDataRemaining(&finalStatus);

                    if (finalStatus != OK
                            || (metaDataSize >= 0
                                && cachedDataRemaining >= metaDataSize)
                            || (mFlags & PREPARE_CANCELLED)) {
                        break;
                    }

                    ALOGV("now cached %d bytes of data", cachedDataRemaining);

                    if (metaDataSize < 0
                            && cachedDataRemaining >= kMinBytesForSniffing) {
                        String8 tmp;
                        float confidence;
                        sp<AMessage> meta;
                        if (!dataSource->sniff(&tmp, &confidence, &meta)) {
                            mLock.lock();
                            return UNKNOWN_ERROR;
                        }

                        // We successfully identified the file's extractor to
                        // be, remember this mime type so we don't have to
                        // sniff it again when we call MediaExtractor::Create()
                        // below.
                        sniffedMIME = tmp.string();

                        if (meta == NULL
                                || !meta->findInt64(
                                    "meta-data-size", &metaDataSize)) {
                            metaDataSize = kHighWaterMarkBytes;
                        }

                        CHECK_GE(metaDataSize, 0ll);
                        ALOGV("metaDataSize = %lld bytes", metaDataSize);
#ifdef ENABLE_AV_ENHANCEMENTS
                        if (!strcasecmp(sniffedMIME.c_str(), MEDIA_MIMETYPE_CONTAINER_QCMPEG4)) {
                            if(mCachedSource->flags() && DataSource::kSupportNonBlockingRead) {
                                mCachedSource->enableNonBlockingRead(true);
                            }
                        }
#endif
                    }

                    usleep(200000);
                }

                mLock.lock();
            }

            if (mFlags & PREPARE_CANCELLED) {
                ALOGI("Prepare cancelled while waiting for initial cache fill.");
                return UNKNOWN_ERROR;
            }
        }
    } else {
        dataSource = DataSource::CreateFromURI(mUri.string(), &mUriHeaders);
    }

    if (dataSource == NULL) {
        return UNKNOWN_ERROR;
    }

    sp<MediaExtractor> extractor;

    if (isWidevineStreaming) {
        String8 mimeType;
        float confidence;
        sp<AMessage> dummy;
        bool success;

        // SniffWVM is potentially blocking since it may require network access.
        // Do not call it with mLock held.
        mLock.unlock();
        success = SniffWVM(dataSource, &mimeType, &confidence, &dummy);
        mLock.lock();

        if (!success
                || strcasecmp(
                    mimeType.string(), MEDIA_MIMETYPE_CONTAINER_WVM)) {
            return ERROR_UNSUPPORTED;
        }

        mWVMExtractor = new WVMExtractor(dataSource);
        mWVMExtractor->setAdaptiveStreamingMode(true);
        if (mUIDValid)
            mWVMExtractor->setUID(mUID);
        extractor = mWVMExtractor;
    } else {
        extractor = MediaExtractor::Create(
                dataSource, sniffedMIME.empty() ? NULL : sniffedMIME.c_str());

        if (extractor == NULL) {
            return UNKNOWN_ERROR;
        }
    }

    if (extractor->getDrmFlag()) {
        checkDrmStatus(dataSource);
    }

    status_t err = setDataSource_l(extractor);

    if (err != OK) {
        mWVMExtractor.clear();

        return err;
    }

    return OK;
}

void AwesomePlayer::abortPrepare(status_t err) {
    CHECK(err != OK);

    if (mIsAsyncPrepare) {
        notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
    }

    mPrepareResult = err;
    modifyFlags((PREPARING|PREPARE_CANCELLED|PREPARING_CONNECTED), CLEAR);
    mAsyncPrepareEvent = NULL;
    mPreparedCondition.broadcast();
    mAudioTearDown = false;
}

// static
bool AwesomePlayer::ContinuePreparation(void *cookie) {
    AwesomePlayer *me = static_cast<AwesomePlayer *>(cookie);

    return (me->mFlags & PREPARE_CANCELLED) == 0;
}

void AwesomePlayer::onPrepareAsyncEvent() {
    Mutex::Autolock autoLock(mLock);
    beginPrepareAsync_l();
}

void AwesomePlayer::beginPrepareAsync_l() {
    if (mFlags & PREPARE_CANCELLED) {
        ALOGI("prepare was cancelled before doing anything");
        abortPrepare(UNKNOWN_ERROR);
        return;
    }

    if (mUri.size() > 0) {
        status_t err = finishSetDataSource_l();

        if (err != OK) {
            abortPrepare(err);
            return;
        }
    }

    if (mVideoTrack != NULL && mVideoSource == NULL) {
        status_t err = initVideoDecoder();

        if (err != OK) {
            abortPrepare(err);
            return;
        }
    }

    if (mAudioTrack != NULL && mAudioSource == NULL) {
        status_t err = initAudioDecoder();

        if (err != OK) {
            abortPrepare(err);
            return;
        }
    }

    modifyFlags(PREPARING_CONNECTED, SET);

    if (isStreamingHTTP()) {
        postBufferingEvent_l();
    } else {
        finishAsyncPrepare_l();
    }
}

void AwesomePlayer::finishAsyncPrepare_l() {
    if (mIsAsyncPrepare) {
        if (mVideoSource == NULL) {
            notifyListener_l(MEDIA_SET_VIDEO_SIZE, 0, 0);
        } else {
            notifyVideoSize_l();
        }

        notifyListener_l(MEDIA_PREPARED);
    }

    mPrepareResult = OK;
    modifyFlags((PREPARING|PREPARE_CANCELLED|PREPARING_CONNECTED), CLEAR);
    modifyFlags(PREPARED, SET);
    mAsyncPrepareEvent = NULL;
    mPreparedCondition.broadcast();

    if (mAudioTearDown) {
        if (mPrepareResult == OK) {
            if (mExtractorFlags & MediaExtractor::CAN_SEEK) {
                seekTo_l(mAudioTearDownPosition);
            }

            if (mAudioTearDownWasPlaying) {
                modifyFlags(CACHE_UNDERRUN, CLEAR);
                play_l();
            }
        }
        mAudioTearDown = false;
    }
}

uint32_t AwesomePlayer::flags() const {
    return mExtractorFlags;
}

void AwesomePlayer::postAudioEOS(int64_t delayUs) {
    postCheckAudioStatusEvent(delayUs);
}

void AwesomePlayer::postAudioSeekComplete() {
    postCheckAudioStatusEvent(0);
}

void AwesomePlayer::postAudioTearDown() {
    postAudioTearDownEvent(0);
}

status_t AwesomePlayer::setParameter(int key, const Parcel &request) {
    switch (key) {
        case KEY_PARAMETER_CACHE_STAT_COLLECT_FREQ_MS:
        {
            return setCacheStatCollectFreq(request);
        }
        case KEY_PARAMETER_PLAYBACK_RATE_PERMILLE:
        {
            if (mAudioPlayer != NULL) {
                return mAudioPlayer->setPlaybackRatePermille(request.readInt32());
            } else {
                return NO_INIT;
            }
        }
        default:
        {
            return ERROR_UNSUPPORTED;
        }
    }
}

status_t AwesomePlayer::setCacheStatCollectFreq(const Parcel &request) {
    if (mCachedSource != NULL) {
        int32_t freqMs = request.readInt32();
        ALOGD("Request to keep cache stats in the past %d ms",
            freqMs);
        return mCachedSource->setCacheStatCollectFreq(freqMs);
    }
    return ERROR_UNSUPPORTED;
}

status_t AwesomePlayer::getParameter(int key, Parcel *reply) {
    switch (key) {
    case KEY_PARAMETER_AUDIO_CHANNEL_COUNT:
        {
            int32_t channelCount;
            if (mAudioTrack == 0 ||
                    !mAudioTrack->getFormat()->findInt32(kKeyChannelCount, &channelCount)) {
                channelCount = 0;
            }
            reply->writeInt32(channelCount);
        }
        return OK;
    default:
        {
            return ERROR_UNSUPPORTED;
        }
    }
}

status_t AwesomePlayer::getTrackInfo(Parcel *reply) const {
    Mutex::Autolock autoLock(mLock);
    size_t trackCount = mExtractor->countTracks();
    if (mTextDriver != NULL) {
        trackCount += mTextDriver->countExternalTracks();
    }

    reply->writeInt32(trackCount);
    for (size_t i = 0; i < mExtractor->countTracks(); ++i) {
        sp<MetaData> meta = mExtractor->getTrackMetaData(i);

        const char *_mime;
        CHECK(meta->findCString(kKeyMIMEType, &_mime));

        String8 mime = String8(_mime);

        reply->writeInt32(2); // 2 fields

        if (!strncasecmp(mime.string(), "video/", 6)) {
            reply->writeInt32(MEDIA_TRACK_TYPE_VIDEO);
        } else if (!strncasecmp(mime.string(), "audio/", 6)) {
            reply->writeInt32(MEDIA_TRACK_TYPE_AUDIO);
        } else if (!strcasecmp(mime.string(), MEDIA_MIMETYPE_TEXT_3GPP)) {
            reply->writeInt32(MEDIA_TRACK_TYPE_TIMEDTEXT);
        } else {
            reply->writeInt32(MEDIA_TRACK_TYPE_UNKNOWN);
        }

        const char *lang;
        if (!meta->findCString(kKeyMediaLanguage, &lang)) {
            lang = "und";
        }
        reply->writeString16(String16(lang));
    }

    if (mTextDriver != NULL) {
        mTextDriver->getExternalTrackInfo(reply);
    }
    return OK;
}

status_t AwesomePlayer::selectAudioTrack_l(
        const sp<MediaSource>& source, size_t trackIndex) {

    ALOGI("selectAudioTrack_l: trackIndex=%d, mFlags=0x%x", trackIndex, mFlags);

    {
        Mutex::Autolock autoLock(mStatsLock);
        if ((ssize_t)trackIndex == mActiveAudioTrackIndex) {
            ALOGI("Track %d is active. Does nothing.", trackIndex);
            return OK;
        }
        //mStats.mFlags = mFlags;
    }

    if (mSeeking != NO_SEEK) {
        ALOGE("Selecting a track while seeking is not supported");
        return ERROR_UNSUPPORTED;
    }

    if ((mFlags & PREPARED) == 0) {
        ALOGE("Data source has not finished preparation");
        return ERROR_UNSUPPORTED;
    }

    CHECK(source != NULL);
    bool wasPlaying = (mFlags & PLAYING) != 0;

    pause_l();

    int64_t curTimeUs;
    CHECK_EQ(getPosition(&curTimeUs), (status_t)OK);

    if ((mAudioPlayer == NULL || !(mFlags & AUDIOPLAYER_STARTED))
            && mAudioSource != NULL) {
        // If we had an audio player, it would have effectively
        // taken possession of the audio source and stopped it when
        // _it_ is stopped. Otherwise this is still our responsibility.
        mAudioSource->stop();
    }
    mAudioSource.clear();
    mOmxSource.clear();

    mTimeSource = NULL;

    delete mAudioPlayer;
    mAudioPlayer = NULL;

    modifyFlags(AUDIOPLAYER_STARTED, CLEAR);

    setAudioSource(source);

    modifyFlags(AUDIO_AT_EOS, CLEAR);
    modifyFlags(AT_EOS, CLEAR);

    status_t err;
    if ((err = initAudioDecoder()) != OK) {
        ALOGE("Failed to init audio decoder: 0x%x", err);
        return err;
    }

    mSeekNotificationSent = true;
    seekTo_l(curTimeUs);

    if (wasPlaying) {
        play_l();
    }

    mActiveAudioTrackIndex = trackIndex;

    return OK;
}

status_t AwesomePlayer::selectTrack(size_t trackIndex, bool select) {
    ATRACE_CALL();
    ALOGV("selectTrack: trackIndex = %d and select=%d", trackIndex, select);
    Mutex::Autolock autoLock(mLock);
    size_t trackCount = mExtractor->countTracks();
    if (mTextDriver != NULL) {
        trackCount += mTextDriver->countExternalTracks();
    }
    if (trackIndex >= trackCount) {
        ALOGE("Track index (%d) is out of range [0, %d)", trackIndex, trackCount);
        return ERROR_OUT_OF_RANGE;
    }

    bool isAudioTrack = false;
    if (trackIndex < mExtractor->countTracks()) {
        sp<MetaData> meta = mExtractor->getTrackMetaData(trackIndex);
        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));
        isAudioTrack = !strncasecmp(mime, "audio/", 6);

        if (!isAudioTrack && strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP) != 0) {
            ALOGE("Track %d is not either audio or timed text", trackIndex);
            return ERROR_UNSUPPORTED;
        }
    }

    if (isAudioTrack) {
        if (!select) {
            ALOGE("Deselect an audio track (%d) is not supported", trackIndex);
            return ERROR_UNSUPPORTED;
        }
        return selectAudioTrack_l(mExtractor->getTrack(trackIndex), trackIndex);
    }

    // Timed text track handling
    if (mTextDriver == NULL) {
        return INVALID_OPERATION;
    }

    status_t err = OK;
    if (select) {
        err = mTextDriver->selectTrack(trackIndex);
        if (err == OK) {
            modifyFlags(TEXTPLAYER_INITIALIZED, SET);
            if (mFlags & PLAYING && !(mFlags & TEXT_RUNNING)) {
                mTextDriver->start();
                modifyFlags(TEXT_RUNNING, SET);
            }
        }
    } else {
        err = mTextDriver->unselectTrack(trackIndex);
        if (err == OK) {
            modifyFlags(TEXTPLAYER_INITIALIZED, CLEAR);
            modifyFlags(TEXT_RUNNING, CLEAR);
        }
    }
    return err;
}

size_t AwesomePlayer::countTracks() const {
    return mExtractor->countTracks() + mTextDriver->countExternalTracks();
}

status_t AwesomePlayer::setVideoScalingMode(int32_t mode) {
    Mutex::Autolock lock(mLock);
    return setVideoScalingMode_l(mode);
}

status_t AwesomePlayer::setVideoScalingMode_l(int32_t mode) {
    mVideoScalingMode = mode;
    if (mNativeWindow != NULL) {
        status_t err = native_window_set_scaling_mode(
                mNativeWindow.get(), mVideoScalingMode);
        if (err != OK) {
            ALOGW("Failed to set scaling mode: %d", err);
        }
        return err;
    }
    return OK;
}

status_t AwesomePlayer::invoke(const Parcel &request, Parcel *reply) {
    ATRACE_CALL();
    if (NULL == reply) {
        return android::BAD_VALUE;
    }
    int32_t methodId;
    status_t ret = request.readInt32(&methodId);
    if (ret != android::OK) {
        return ret;
    }
    switch(methodId) {
        case INVOKE_ID_SET_VIDEO_SCALING_MODE:
        {
            int mode = request.readInt32();
            return setVideoScalingMode(mode);
        }

        case INVOKE_ID_GET_TRACK_INFO:
        {
            return getTrackInfo(reply);
        }
        case INVOKE_ID_ADD_EXTERNAL_SOURCE:
        {
            Mutex::Autolock autoLock(mLock);
            if (mTextDriver == NULL) {
                mTextDriver = new TimedTextDriver(mListener);
            }
            // String values written in Parcel are UTF-16 values.
            String8 uri(request.readString16());
            String8 mimeType(request.readString16());
            size_t nTracks = countTracks();
            return mTextDriver->addOutOfBandTextSource(nTracks, uri, mimeType);
        }
        case INVOKE_ID_ADD_EXTERNAL_SOURCE_FD:
        {
            Mutex::Autolock autoLock(mLock);
            if (mTextDriver == NULL) {
                mTextDriver = new TimedTextDriver(mListener);
            }
            int fd         = request.readFileDescriptor();
            off64_t offset = request.readInt64();
            off64_t length  = request.readInt64();
            String8 mimeType(request.readString16());
            size_t nTracks = countTracks();
            return mTextDriver->addOutOfBandTextSource(
                    nTracks, fd, offset, length, mimeType);
        }
        case INVOKE_ID_SELECT_TRACK:
        {
            int trackIndex = request.readInt32();
            return selectTrack(trackIndex, true /* select */);
        }
        case INVOKE_ID_UNSELECT_TRACK:
        {
            int trackIndex = request.readInt32();
            return selectTrack(trackIndex, false /* select */);
        }
        default:
        {
            return ERROR_UNSUPPORTED;
        }
    }
    // It will not reach here.
    return OK;
}

bool AwesomePlayer::isStreamingHTTP() const {
    return mCachedSource != NULL || mWVMExtractor != NULL;
}

bool AwesomePlayer::isWidevineContent() const {
    if (mWVMExtractor != NULL) {
        return true;
    }

    sp<MetaData> fileMeta = mExtractor->getMetaData();
    const char *containerMime;
    if (fileMeta != NULL &&
        fileMeta->findCString(kKeyMIMEType, &containerMime) &&
        !strcasecmp(containerMime, "video/wvm")) {
       return true;
    }

    return false;
}

status_t AwesomePlayer::dump(int fd, const Vector<String16> &args) const {
    Mutex::Autolock autoLock(mStatsLock);

    FILE *out = fdopen(dup(fd), "w");

    fprintf(out, " AwesomePlayer\n");
    if (mStats.mFd < 0) {
        fprintf(out, "  URI(suppressed)");
    } else {
        fprintf(out, "  fd(%d)", mStats.mFd);
    }

    fprintf(out, ", flags(0x%08x)", mStats.mFlags);

    if (mStats.mBitrate >= 0) {
        fprintf(out, ", bitrate(%lld bps)", mStats.mBitrate);
    }

    fprintf(out, "\n");

    for (size_t i = 0; i < mStats.mTracks.size(); ++i) {
        const TrackStat &stat = mStats.mTracks.itemAt(i);

        fprintf(out, "  Track %d\n", i + 1);
        fprintf(out, "   MIME(%s)", stat.mMIME.string());

        if (!stat.mDecoderName.isEmpty()) {
            fprintf(out, ", decoder(%s)", stat.mDecoderName.string());
        }

        fprintf(out, "\n");

        if ((ssize_t)i == mStats.mVideoTrackIndex) {
            fprintf(out,
                    "   videoDimensions(%d x %d)\n"
                    "   Total Video Frames Decoded(%lld)\n"
                    "   Total Video Frames Rendered(%lld)\n"
                    "   Total Playback Duration(%lld ms)\n"
                    "   numVideoFramesDropped(%lld)\n"
                    "   Average Frames Per Second(%.4f)\n"
                    "   First Frame Latency (%lld ms)\n"
                    "   Number of times AV Sync Lost(%u)\n"
                    "   Max Video Ahead Time Delta(%u)\n"
                    "   Max Video Behind Time Delta(%u)\n"
                    "   Max Time Sync Loss(%u)\n"
                    "   EOS(%d)\n"
                    "   PLAYING(%d)\n",
                    mStats.mVideoWidth,
                    mStats.mVideoHeight,
                    mStats.mNumVideoFramesDecoded,
                    mStats.mTotalFrames,
                    mStats.mTotalTimeUs/1000,
                    mStats.mNumVideoFramesDropped,
                    ((double)(mStats.mTotalFrames)*1E6)/((double)mStats.mTotalTimeUs),
                    mStats.mFirstFrameLatencyUs/1000,
                    mStats.mNumTimesSyncLoss,
                    -mStats.mMaxEarlyDelta/1000,
                    mStats.mMaxLateDelta/1000,
                    mStats.mMaxTimeSyncLoss/1000,
                    (mFlags & AT_EOS) > 0,
                    (mFlags & PLAYING) > 0);
        }
    }

    fclose(out);
    out = NULL;

    return OK;
}

void AwesomePlayer::modifyFlags(unsigned value, FlagMode mode) {
    switch (mode) {
        case SET:
            mFlags |= value;
            break;
        case CLEAR:
            if ((value & CACHE_UNDERRUN) && (mFlags & CACHE_UNDERRUN)) {
                notifyListener_l(MEDIA_INFO, MEDIA_INFO_BUFFERING_END);
            }
            mFlags &= ~value;
            break;
        case ASSIGN:
            mFlags = value;
            break;
        default:
            TRESPASS();
    }

    {
        Mutex::Autolock autoLock(mStatsLock);
        mStats.mFlags = mFlags;
    }
}

void AwesomePlayer::onAudioTearDownEvent() {

    Mutex::Autolock autoLock(mLock);
    if (!mAudioTearDownEventPending) {
        return;
    }
    mAudioTearDownEventPending = false;

    ALOGD("copl:onAudioTearDownEvent");

    // stream info is cleared by reset_l() so copy what we need
    mAudioTearDownWasPlaying = (mFlags & PLAYING);
    uint32_t loopingFlags = (mFlags & (LOOPING | AUTO_LOOPING));
    KeyedVector<String8, String8> uriHeaders(mUriHeaders);
    sp<DataSource> fileSource(mFileSource);

    mStatsLock.lock();
    String8 uri(mStats.mURI);
    mStatsLock.unlock();

    // get current position so we can start recreated stream from here
    getPosition(&mAudioTearDownPosition);

    // Reset and recreate
    reset_l();

    status_t err;

    if (fileSource != NULL) {
        mFileSource = fileSource;
        err = setDataSource_l(fileSource);
    } else {
        err = setDataSource_l(uri, &uriHeaders);
    }

    mFlags |= PREPARING;
    if ( err != OK ) {
        // This will force beingPrepareAsync_l() to notify
        // a MEDIA_ERROR to the client and abort the prepare
        mFlags |= PREPARE_CANCELLED;
    }

    mFlags |= loopingFlags;

    mAudioTearDown = true;
    mIsAsyncPrepare = true;

    // Call prepare for the host decoding
    beginPrepareAsync_l();
}

#ifdef QCOM_DIRECTTRACK
bool AwesomePlayer::inSupportedTunnelFormats(const char * mime) {
    const char * tunnelFormats [ ] = {
        MEDIA_MIMETYPE_AUDIO_MPEG,
        MEDIA_MIMETYPE_AUDIO_AAC,
#ifdef TUNNEL_MODE_SUPPORTS_AMRWB
        MEDIA_MIMETYPE_AUDIO_AMR_WB,
    #ifdef ENABLE_AV_ENHANCEMENTS
        MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS
    #endif
#endif
    };

    if (!mime) {
        return false;
    }

    size_t len = sizeof(tunnelFormats)/sizeof(const char *);
    for (size_t i = 0; i < len; i++) {
        const char * tf = tunnelFormats[i];
        if (!strncasecmp(mime, tf, strlen(tf))) {
            if (strlen(mime) == strlen(tf)) { //to prevent a substring match
                ALOGD("Tunnel playback supported for %s", tf);
                return true;
            }
        }
    }

    ALOGW("Tunnel playback unsupported for %s", mime);
    return false;
}

void AwesomePlayer::checkTunnelExceptions()
{
    if(!mIsTunnelAudio) {
        return;
    }
    /* exception 1: No streaming */
    if (isStreamingHTTP()) {
        ALOGV("Streaming, force disable tunnel mode playback");
        mIsTunnelAudio = false;
        return;
    }

    CHECK(mAudioTrack != NULL);

    /*
     * exception 2: No AAC-LD content,
     * hint given by setting kKeyTunnelException in the track meta
     */
    int32_t exception = 0;
    if (mAudioTrack->getFormat()->findInt32(kKeyTunnelException, &exception) &&
        exception) {
        ALOGV("kKeyTunnelException set, disable tunnel mode");
        mIsTunnelAudio = false;
        return;
    }

    /* exception 3: use tunnel player only for AUDIO_STREAM_MUSIC */
    if (mAudioSink->streamType() != AUDIO_STREAM_MUSIC ) {
        ALOGV("Use tunnel player only for AUDIO_STREAM_MUSIC");
        mIsTunnelAudio = false;
        return;
    }

    /* exception 4: check for  AAC main/AAC ELD profiles, it is not supported */
    sp<MetaData> metaData  = mAudioTrack->getFormat();
    const char * mime;
    int32_t objecttype = 0;

    if (metaData->findCString(kKeyMIMEType, &mime) &&
           !strcmp(mime, MEDIA_MIMETYPE_AUDIO_AAC) &&
           (metaData->findInt32(kKeyAACProfile, &objecttype) &&
           ((1 == objecttype) || (39 == objecttype)))) {
        ALOGD("FOUND unsupported AAC profiletype(%d) , disable tunnel mode\n",objecttype);
        mIsTunnelAudio = false;
        return;
    }

    /* below exceptions are only for av content */
    if (mVideoTrack == NULL) return;

    /* exception 3: No avi having video + mp3 */
    if (mExtractor == NULL) return;

    metaData = mExtractor->getMetaData();
    const char * container;

    /*only proceed for avi content.*/
    if (!metaData->findCString(kKeyMIMEType, &container) ||
        strcmp(container, MEDIA_MIMETYPE_CONTAINER_AVI)) {
        return;
    }

    metaData = mAudioTrack->getFormat();
    /*disable for av content having mp3*/
    if (metaData->findCString(kKeyMIMEType, &mime) &&
        !strcmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
        ALOGV("Clip has AVI extractor and mp3 content, disable tunnel mode");
        mIsTunnelAudio = false;
        return;
    }

    return;
}
#endif

// suspend() will release the decoders, the renderers and the buffers allocated for decoders
// Releasing decoders eliminates draining power in suspended state.
status_t AwesomePlayer::suspend() {
    ALOGV("suspend()");
    Mutex::Autolock autoLock(mLock);

    // Set PAUSE to DrmManagerClient which will be set START in play_l()
    if (mDecryptHandle != NULL) {
        mDrmManagerClient->setPlaybackStatus(mDecryptHandle,
                    Playback::PAUSE, 0);
    }

    cancelPlayerEvents();
    if (mQueueStarted) {
        mQueue.stop();
        mQueueStarted = false;
    }

    // Shutdown audio decoder first
    if ((mAudioPlayer == NULL || !(mFlags & AUDIOPLAYER_STARTED))
            && mAudioSource != NULL) {
        mAudioSource->stop();
    }
    mAudioSource.clear();
    mOmxSource.clear();
    delete mAudioPlayer;
    mAudioPlayer = NULL;
    modifyFlags(AUDIO_RUNNING | AUDIOPLAYER_STARTED, CLEAR);

    // Shutdown the video decoder
    mVideoRenderer.clear();
    printStats();
    if (mVideoSource != NULL) {
        shutdownVideoDecoder_l();
    }
    modifyFlags(PLAYING, CLEAR);
    mVideoRenderingStarted = false;

    // Disconnect the source
    if (mCachedSource != NULL) {
        status_t err = mCachedSource->disconnectWhileSuspend();
        if (err != OK) {
            return err;
        }
    }

    return OK;
}

status_t AwesomePlayer::resume() {
    ALOGV("resume()");
    Mutex::Autolock autoLock(mLock);

    // Reconnect the source
    status_t err = mCachedSource->connectWhileResume();

    if (mVideoTrack != NULL && mVideoSource == NULL) {
        status_t err = initVideoDecoder();
        if (err != OK) {
            return err;
        }
    }

    if (mAudioTrack != NULL && mAudioSource == NULL) {
        status_t err = initAudioDecoder();
        if (err != OK) {
            return err;
        }
    }

    mIsFirstFrameAfterResume = true;

    if (!mQueueStarted) {
        mQueue.start();
        mQueueStarted = true;
    }

    return OK;
}

inline void AwesomePlayer::logFirstFrame() {
    mStats.mFirstFrameLatencyUs = getTimeOfDayUs()-mStats.mFirstFrameLatencyStartUs;
    mStats.mVeryFirstFrame = false;
}

inline void AwesomePlayer::logCatchUp(int64_t ts, int64_t clock, int64_t delta)
{
    if (mStats.mConsecutiveFramesDropped > 0) {
        mStats.mNumTimesSyncLoss++;
        if (mStats.mMaxTimeSyncLoss < (clock - mStats.mCatchupTimeStart) && clock > 0 && ts > 0) {
            mStats.mMaxTimeSyncLoss = clock - mStats.mCatchupTimeStart;
        }
    }
}

inline void AwesomePlayer::logLate(int64_t ts, int64_t clock, int64_t delta)
{
    if (mStats.mMaxLateDelta < delta && clock > 0 && ts > 0) {
        mStats.mMaxLateDelta = delta;
    }
}

inline void AwesomePlayer::logOnTime(int64_t ts, int64_t clock, int64_t delta)
{
    bool needLogLate = false;
    logCatchUp(ts, clock, delta);
    if (delta <= 0) {
        if ((-delta) > (-mStats.mMaxEarlyDelta) && clock > 0 && ts > 0) {
            mStats.mMaxEarlyDelta = delta;
        }
    }
    else {
        needLogLate = true;
    }

    if(needLogLate) logLate(ts, clock, delta);
}

inline int64_t AwesomePlayer::getTimeOfDayUs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
}  // namespace android
