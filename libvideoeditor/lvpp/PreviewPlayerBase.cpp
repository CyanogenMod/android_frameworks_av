/*
 * Copyright (C) 2011 The Android Open Source Project
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
#define LOG_TAG "PreviewPlayerBase"
#include <utils/Log.h>

#include <dlfcn.h>

#include "PreviewPlayerBase.h"
#include "AudioPlayerBase.h"
#include "include/SoftwareRenderer.h"

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>

#include <surfaceflinger/Surface.h>
#include <gui/ISurfaceTexture.h>
#include <gui/SurfaceTextureClient.h>
#include <surfaceflinger/ISurfaceComposer.h>

#include <cutils/properties.h>

#define USE_SURFACE_ALLOC 1

namespace android {

struct AwesomeEvent : public TimedEventQueue::Event {
    AwesomeEvent(
            PreviewPlayerBase *player,
            void (PreviewPlayerBase::*method)())
        : mPlayer(player),
          mMethod(method) {
    }

protected:
    virtual ~AwesomeEvent() {}

    virtual void fire(TimedEventQueue *queue, int64_t /* now_us */) {
        (mPlayer->*mMethod)();
    }

private:
    PreviewPlayerBase *mPlayer;
    void (PreviewPlayerBase::*mMethod)();

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
        status_t err = mNativeWindow->queueBuffer(
                mNativeWindow.get(), buffer->graphicBuffer().get());
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
PreviewPlayerBase::PreviewPlayerBase()
    : mQueueStarted(false),
      mTimeSource(NULL),
      mVideoRendererIsPreview(false),
      mAudioPlayer(NULL),
      mDisplayWidth(0),
      mDisplayHeight(0),
      mFlags(0),
      mExtractorFlags(0),
      mVideoBuffer(NULL),
      mLastVideoTimeUs(-1) {
    CHECK_EQ(mClient.connect(), (status_t)OK);

    DataSource::RegisterDefaultSniffers();

    mVideoEvent = new AwesomeEvent(this, &PreviewPlayerBase::onVideoEvent);
    mVideoEventPending = false;
    mStreamDoneEvent = new AwesomeEvent(this, &PreviewPlayerBase::onStreamDone);
    mStreamDoneEventPending = false;
    mVideoLagEvent = new AwesomeEvent(this, &PreviewPlayerBase::onVideoLagUpdate);
    mVideoEventPending = false;

    mCheckAudioStatusEvent = new AwesomeEvent(
            this, &PreviewPlayerBase::onCheckAudioStatus);

    mAudioStatusEventPending = false;

    reset();
}

PreviewPlayerBase::~PreviewPlayerBase() {
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();

    mClient.disconnect();
}

void PreviewPlayerBase::cancelPlayerEvents() {
    mQueue.cancelEvent(mVideoEvent->eventID());
    mVideoEventPending = false;
    mQueue.cancelEvent(mStreamDoneEvent->eventID());
    mStreamDoneEventPending = false;
    mQueue.cancelEvent(mCheckAudioStatusEvent->eventID());
    mAudioStatusEventPending = false;
    mQueue.cancelEvent(mVideoLagEvent->eventID());
    mVideoLagEventPending = false;
}

void PreviewPlayerBase::setListener(const wp<MediaPlayerBase> &listener) {
    Mutex::Autolock autoLock(mLock);
    mListener = listener;
}

status_t PreviewPlayerBase::setDataSource(const char *path) {
    Mutex::Autolock autoLock(mLock);
    return setDataSource_l(path);
}

status_t PreviewPlayerBase::setDataSource_l(const char *path) {
    reset_l();

    mUri = path;

    if (!(mFlags & INCOGNITO)) {
        ALOGI("setDataSource_l('%s')", mUri.string());
    } else {
        ALOGI("setDataSource_l(URL suppressed)");
    }

    // The actual work will be done during preparation in the call to
    // ::finishSetDataSource_l to avoid blocking the calling thread in
    // setDataSource for any significant time.

    return OK;
}

status_t PreviewPlayerBase::setDataSource(const sp<IStreamSource> &source) {
    return INVALID_OPERATION;
}

status_t PreviewPlayerBase::setDataSource_l(const sp<MediaExtractor> &extractor) {
    if (extractor == NULL) {
        return UNKNOWN_ERROR;
    }

    if (extractor->getDrmFlag()) {
        ALOGE("Data source is drm protected");
        return INVALID_OPERATION;
    }

    // Attempt to approximate overall stream bitrate by summing all
    // tracks' individual bitrates, if not all of them advertise bitrate,
    // we have to fail.

    int64_t totalBitRate = 0;

    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        int32_t bitrate;
        if (!meta->findInt32(kKeyBitRate, &bitrate)) {
            totalBitRate = -1;
            break;
        }

        totalBitRate += bitrate;
    }

    mBitrate = totalBitRate;

    ALOGV("mBitrate = %lld bits/sec", mBitrate);

    bool haveAudio = false;
    bool haveVideo = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!haveVideo && !strncasecmp(mime, "video/", 6)) {
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

        } else if (!haveAudio && !strncasecmp(mime, "audio/", 6)) {
            setAudioSource(extractor->getTrack(i));
            haveAudio = true;

            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)) {
                // Only do this for vorbis audio, none of the other audio
                // formats even support this ringtone specific hack and
                // retrieving the metadata on some extractors may turn out
                // to be very expensive.
                sp<MetaData> fileMeta = extractor->getMetaData();
                int32_t loop;
                if (fileMeta != NULL
                        && fileMeta->findInt32(kKeyAutoLoop, &loop) && loop != 0) {
                    mFlags |= AUTO_LOOPING;
                }
            }
        }

        if (haveAudio && haveVideo) {
            break;
        }
    }

    if (!haveAudio && !haveVideo) {
        return UNKNOWN_ERROR;
    }

    mExtractorFlags = extractor->flags();

    return OK;
}

void PreviewPlayerBase::reset() {
    Mutex::Autolock autoLock(mLock);
    reset_l();
}

void PreviewPlayerBase::reset_l() {
    mDisplayWidth = 0;
    mDisplayHeight = 0;

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
        mFlags |= PREPARE_CANCELLED;

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

    mAudioTrack.clear();
    mVideoTrack.clear();

    // Shutdown audio first, so that the respone to the reset request
    // appears to happen instantaneously as far as the user is concerned
    // If we did this later, audio would continue playing while we
    // shutdown the video-related resources and the player appear to
    // not be as responsive to a reset request.
    if (mAudioPlayer == NULL && mAudioSource != NULL) {
        // If we had an audio player, it would have effectively
        // taken possession of the audio source and stopped it when
        // _it_ is stopped. Otherwise this is still our responsibility.
        mAudioSource->stop();
    }
    mAudioSource.clear();

    mTimeSource = NULL;

    delete mAudioPlayer;
    mAudioPlayer = NULL;

    mVideoRenderer.clear();

    if (mVideoSource != NULL) {
        shutdownVideoDecoder_l();
    }

    mDurationUs = -1;
    mFlags = 0;
    mExtractorFlags = 0;
    mTimeSourceDeltaUs = 0;
    mVideoTimeUs = 0;

    mSeeking = NO_SEEK;
    mSeekNotificationSent = false;
    mSeekTimeUs = 0;

    mUri.setTo("");

    mBitrate = -1;
    mLastVideoTimeUs = -1;
}

void PreviewPlayerBase::notifyListener_l(int msg, int ext1, int ext2) {
    if (mListener != NULL) {
        sp<MediaPlayerBase> listener = mListener.promote();

        if (listener != NULL) {
            listener->sendEvent(msg, ext1, ext2);
        }
    }
}

void PreviewPlayerBase::onVideoLagUpdate() {
    Mutex::Autolock autoLock(mLock);
    if (!mVideoLagEventPending) {
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

void PreviewPlayerBase::onStreamDone() {
    // Posted whenever any stream finishes playing.

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

        mFlags |= AT_EOS;
        return;
    }

    const bool allDone =
        (mVideoSource == NULL || (mFlags & VIDEO_AT_EOS))
            && (mAudioSource == NULL || (mFlags & AUDIO_AT_EOS));

    if (!allDone) {
        return;
    }

    if (mFlags & (LOOPING | AUTO_LOOPING)) {
        seekTo_l(0);

        if (mVideoSource != NULL) {
            postVideoEvent_l();
        }
    } else {
        ALOGV("MEDIA_PLAYBACK_COMPLETE");
        notifyListener_l(MEDIA_PLAYBACK_COMPLETE);

        pause_l(true /* at eos */);

        mFlags |= AT_EOS;
    }
}

status_t PreviewPlayerBase::play() {
    Mutex::Autolock autoLock(mLock);

    mFlags &= ~CACHE_UNDERRUN;

    return play_l();
}

status_t PreviewPlayerBase::play_l() {
    mFlags &= ~SEEK_PREVIEW;

    if (mFlags & PLAYING) {
        return OK;
    }

    if (!(mFlags & PREPARED)) {
        status_t err = prepare_l();

        if (err != OK) {
            return err;
        }
    }

    mFlags |= PLAYING;
    mFlags |= FIRST_FRAME;

    if (mAudioSource != NULL) {
        if (mAudioPlayer == NULL) {
            if (mAudioSink != NULL) {
                mAudioPlayer = new AudioPlayerBase(mAudioSink, this);
                mAudioPlayer->setSource(mAudioSource);

                mTimeSource = mAudioPlayer;

                // If there was a seek request before we ever started,
                // honor the request now.
                // Make sure to do this before starting the audio player
                // to avoid a race condition.
                seekAudioIfNecessary_l();
            }
        }

        CHECK(!(mFlags & AUDIO_RUNNING));

        if (mVideoSource == NULL) {
            status_t err = startAudioPlayer_l();

            if (err != OK) {
                delete mAudioPlayer;
                mAudioPlayer = NULL;

                mFlags &= ~(PLAYING | FIRST_FRAME);

                return err;
            }
        }
    }

    if (mTimeSource == NULL && mAudioPlayer == NULL) {
        mTimeSource = &mSystemTimeSource;
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

    return OK;
}

status_t PreviewPlayerBase::startAudioPlayer_l() {
    CHECK(!(mFlags & AUDIO_RUNNING));

    if (mAudioSource == NULL || mAudioPlayer == NULL) {
        return OK;
    }

    if (!(mFlags & AUDIOPLAYER_STARTED)) {
        mFlags |= AUDIOPLAYER_STARTED;

        // We've already started the MediaSource in order to enable
        // the prefetcher to read its data.
        status_t err = mAudioPlayer->start(
                true /* sourceAlreadyStarted */);

        if (err != OK) {
            notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
            return err;
        }
    } else {
        mAudioPlayer->resume();
    }

    mFlags |= AUDIO_RUNNING;

    mWatchForAudioEOS = true;

    return OK;
}

void PreviewPlayerBase::notifyVideoSize_l() {
    sp<MetaData> meta = mVideoSource->getFormat();

    int32_t vWidth, vHeight;
    int32_t cropLeft, cropTop, cropRight, cropBottom;

    CHECK(meta->findInt32(kKeyWidth, &vWidth));
    CHECK(meta->findInt32(kKeyHeight, &vHeight));

    mGivenWidth = vWidth;
    mGivenHeight = vHeight;

    if (!meta->findRect(
                kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom)) {

        cropLeft = cropTop = 0;
        cropRight = vWidth - 1;
        cropBottom = vHeight - 1;

        ALOGD("got dimensions only %d x %d", vWidth, vHeight);
    } else {
        ALOGD("got crop rect %d, %d, %d, %d",
             cropLeft, cropTop, cropRight, cropBottom);
    }

    mCropRect.left = cropLeft;
    mCropRect.right = cropRight;
    mCropRect.top = cropTop;
    mCropRect.bottom = cropBottom;

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

void PreviewPlayerBase::initRenderer_l() {
    if (mNativeWindow == NULL) {
        return;
    }

    sp<MetaData> meta = mVideoSource->getFormat();

    int32_t format;
    const char *component;
    CHECK(meta->findInt32(kKeyColorFormat, &format));
    CHECK(meta->findCString(kKeyDecoderComponent, &component));

    int32_t rotationDegrees;
    if (!mVideoTrack->getFormat()->findInt32(
                kKeyRotation, &rotationDegrees)) {
        rotationDegrees = 0;
    }

    mVideoRenderer.clear();

    // Must ensure that mVideoRenderer's destructor is actually executed
    // before creating a new one.
    IPCThreadState::self()->flushCommands();

    if (USE_SURFACE_ALLOC && strncmp(component, "OMX.", 4) == 0) {
        // Hardware decoders avoid the CPU color conversion by decoding
        // directly to ANativeBuffers, so we must use a renderer that
        // just pushes those buffers to the ANativeWindow.
        mVideoRenderer =
            new AwesomeNativeWindowRenderer(mNativeWindow, rotationDegrees);
    } else {
        // Other decoders are instantiated locally and as a consequence
        // allocate their buffers in local address space.  This renderer
        // then performs a color conversion and copy to get the data
        // into the ANativeBuffer.
        mVideoRenderer = new AwesomeLocalRenderer(mNativeWindow, meta);
    }
}

status_t PreviewPlayerBase::pause() {
    Mutex::Autolock autoLock(mLock);

    mFlags &= ~CACHE_UNDERRUN;

    return pause_l();
}

status_t PreviewPlayerBase::pause_l(bool at_eos) {
    if (!(mFlags & PLAYING)) {
        return OK;
    }

    cancelPlayerEvents();

    if (mAudioPlayer != NULL && (mFlags & AUDIO_RUNNING)) {
        if (at_eos) {
            // If we played the audio stream to completion we
            // want to make sure that all samples remaining in the audio
            // track's queue are played out.
            mAudioPlayer->pause(true /* playPendingSamples */);
        } else {
            mAudioPlayer->pause();
        }

        mFlags &= ~AUDIO_RUNNING;
    }

    mFlags &= ~PLAYING;

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

bool PreviewPlayerBase::isPlaying() const {
    return (mFlags & PLAYING) || (mFlags & CACHE_UNDERRUN);
}

void PreviewPlayerBase::setSurface(const sp<Surface> &surface) {
    Mutex::Autolock autoLock(mLock);

    mSurface = surface;
    setNativeWindow_l(surface);
}

void PreviewPlayerBase::setSurfaceTexture(const sp<ISurfaceTexture> &surfaceTexture) {
    Mutex::Autolock autoLock(mLock);

    mSurface.clear();
    if (surfaceTexture != NULL) {
        setNativeWindow_l(new SurfaceTextureClient(surfaceTexture));
    }
}

void PreviewPlayerBase::shutdownVideoDecoder_l() {
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
}

void PreviewPlayerBase::setNativeWindow_l(const sp<ANativeWindow> &native) {
    mNativeWindow = native;

    if (mVideoSource == NULL) {
        return;
    }

    ALOGI("attempting to reconfigure to use new surface");

    bool wasPlaying = (mFlags & PLAYING) != 0;

    pause_l();
    mVideoRenderer.clear();

    shutdownVideoDecoder_l();

    CHECK_EQ(initVideoDecoder(), (status_t)OK);

    if (mLastVideoTimeUs >= 0) {
        mSeeking = SEEK;
        mSeekNotificationSent = true;
        mSeekTimeUs = mLastVideoTimeUs;
        mFlags &= ~(AT_EOS | AUDIO_AT_EOS | VIDEO_AT_EOS);
    }

    if (wasPlaying) {
        play_l();
    }
}

void PreviewPlayerBase::setAudioSink(
        const sp<MediaPlayerBase::AudioSink> &audioSink) {
    Mutex::Autolock autoLock(mLock);

    mAudioSink = audioSink;
}

status_t PreviewPlayerBase::setLooping(bool shouldLoop) {
    Mutex::Autolock autoLock(mLock);

    mFlags = mFlags & ~LOOPING;

    if (shouldLoop) {
        mFlags |= LOOPING;
    }

    return OK;
}

status_t PreviewPlayerBase::getDuration(int64_t *durationUs) {
    Mutex::Autolock autoLock(mMiscStateLock);

    if (mDurationUs < 0) {
        return UNKNOWN_ERROR;
    }

    *durationUs = mDurationUs;

    return OK;
}

status_t PreviewPlayerBase::getPosition(int64_t *positionUs) {
    if (mSeeking != NO_SEEK) {
        *positionUs = mSeekTimeUs;
    } else if (mVideoSource != NULL
            && (mAudioPlayer == NULL || !(mFlags & VIDEO_AT_EOS))) {
        Mutex::Autolock autoLock(mMiscStateLock);
        *positionUs = mVideoTimeUs;
    } else if (mAudioPlayer != NULL) {
        *positionUs = mAudioPlayer->getMediaTimeUs();
    } else {
        *positionUs = 0;
    }

    return OK;
}

status_t PreviewPlayerBase::seekTo(int64_t timeUs) {
    if (mExtractorFlags & MediaExtractor::CAN_SEEK) {
        Mutex::Autolock autoLock(mLock);
        return seekTo_l(timeUs);
    }

    return OK;
}

status_t PreviewPlayerBase::seekTo_l(int64_t timeUs) {
    if (mFlags & CACHE_UNDERRUN) {
        mFlags &= ~CACHE_UNDERRUN;
        play_l();
    }

    if ((mFlags & PLAYING) && mVideoSource != NULL && (mFlags & VIDEO_AT_EOS)) {
        // Video playback completed before, there's no pending
        // video event right now. In order for this new seek
        // to be honored, we need to post one.

        postVideoEvent_l();
    }

    mSeeking = SEEK;
    mSeekNotificationSent = false;
    mSeekTimeUs = timeUs;
    mFlags &= ~(AT_EOS | AUDIO_AT_EOS | VIDEO_AT_EOS);

    seekAudioIfNecessary_l();

    if (!(mFlags & PLAYING)) {
        ALOGV("seeking while paused, sending SEEK_COMPLETE notification"
             " immediately.");

        notifyListener_l(MEDIA_SEEK_COMPLETE);
        mSeekNotificationSent = true;

        if ((mFlags & PREPARED) && mVideoSource != NULL) {
            mFlags |= SEEK_PREVIEW;
            postVideoEvent_l();
        }
    }

    return OK;
}

void PreviewPlayerBase::seekAudioIfNecessary_l() {
    if (mSeeking != NO_SEEK && mVideoSource == NULL && mAudioPlayer != NULL) {
        mAudioPlayer->seekTo(mSeekTimeUs);

        mWatchForAudioSeekComplete = true;
        mWatchForAudioEOS = true;
    }
}

void PreviewPlayerBase::setAudioSource(sp<MediaSource> source) {
    CHECK(source != NULL);

    mAudioTrack = source;
}

status_t PreviewPlayerBase::initAudioDecoder() {
    sp<MetaData> meta = mAudioTrack->getFormat();

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
        mAudioSource = mAudioTrack;
    } else {
        mAudioSource = OMXCodec::Create(
                mClient.interface(), mAudioTrack->getFormat(),
                false, // createEncoder
                mAudioTrack);
    }

    if (mAudioSource != NULL) {
        int64_t durationUs;
        if (mAudioTrack->getFormat()->findInt64(kKeyDuration, &durationUs)) {
            Mutex::Autolock autoLock(mMiscStateLock);
            if (mDurationUs < 0 || durationUs > mDurationUs) {
                mDurationUs = durationUs;
            }
        }

        status_t err = mAudioSource->start();

        if (err != OK) {
            mAudioSource.clear();
            return err;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_QCELP)) {
        // For legacy reasons we're simply going to ignore the absence
        // of an audio decoder for QCELP instead of aborting playback
        // altogether.
        return OK;
    }

    return mAudioSource != NULL ? OK : UNKNOWN_ERROR;
}

void PreviewPlayerBase::setVideoSource(sp<MediaSource> source) {
    CHECK(source != NULL);

    mVideoTrack = source;
}

status_t PreviewPlayerBase::initVideoDecoder(uint32_t flags) {
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
            mVideoSource.clear();
            return err;
        }
    }

    return mVideoSource != NULL ? OK : UNKNOWN_ERROR;
}

void PreviewPlayerBase::finishSeekIfNecessary(int64_t videoTimeUs) {
    if (mSeeking == SEEK_VIDEO_ONLY) {
        mSeeking = NO_SEEK;
        return;
    }

    if (mSeeking == NO_SEEK || (mFlags & SEEK_PREVIEW)) {
        return;
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

    mFlags |= FIRST_FRAME;
    mSeeking = NO_SEEK;
}

void PreviewPlayerBase::onVideoEvent() {
    Mutex::Autolock autoLock(mLock);
    if (!mVideoEventPending) {
        // The event has been cancelled in reset_l() but had already
        // been scheduled for execution at that time.
        return;
    }
    mVideoEventPending = false;

    if (mSeeking != NO_SEEK) {
        if (mVideoBuffer) {
            mVideoBuffer->release();
            mVideoBuffer = NULL;
        }
    }

    if (!mVideoBuffer) {
        MediaSource::ReadOptions options;
        if (mSeeking != NO_SEEK) {
            ALOGV("seeking to %lld us (%.2f secs)", mSeekTimeUs, mSeekTimeUs / 1E6);

            options.setSeekTo(
                    mSeekTimeUs,
                    mSeeking == SEEK_VIDEO_ONLY
                        ? MediaSource::ReadOptions::SEEK_NEXT_SYNC
                        : MediaSource::ReadOptions::SEEK_CLOSEST_SYNC);
        }
        for (;;) {
            status_t err = mVideoSource->read(&mVideoBuffer, &options);
            options.clearSeekTo();

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

                mFlags |= VIDEO_AT_EOS;
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

            break;
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
        status_t err = startAudioPlayer_l();
        if (err != OK) {
            ALOGE("Startung the audio player failed w/ err %d", err);
            return;
        }
    }

    TimeSource *ts = (mFlags & AUDIO_AT_EOS) ? &mSystemTimeSource : mTimeSource;

    if (mFlags & FIRST_FRAME) {
        mFlags &= ~FIRST_FRAME;
        mTimeSourceDeltaUs = ts->getRealTimeUs() - timeUs;
    }

    int64_t realTimeUs, mediaTimeUs;
    if (!(mFlags & AUDIO_AT_EOS) && mAudioPlayer != NULL
        && mAudioPlayer->getMediaTimeMapping(&realTimeUs, &mediaTimeUs)) {
        mTimeSourceDeltaUs = realTimeUs - mediaTimeUs;
    }

    if (wasSeeking == SEEK_VIDEO_ONLY) {
        int64_t nowUs = ts->getRealTimeUs() - mTimeSourceDeltaUs;

        int64_t latenessUs = nowUs - timeUs;

        if (latenessUs > 0) {
            ALOGI("after SEEK_VIDEO_ONLY we're late by %.2f secs", latenessUs / 1E6);
        }
    }

    if (wasSeeking == NO_SEEK) {
        // Let's display the first frame after seeking right away.

        int64_t nowUs = ts->getRealTimeUs() - mTimeSourceDeltaUs;

        int64_t latenessUs = nowUs - timeUs;

        if (latenessUs > 500000ll
                && mAudioPlayer != NULL
                && mAudioPlayer->getMediaTimeMapping(
                    &realTimeUs, &mediaTimeUs)) {
            ALOGI("we're much too late (%.2f secs), video skipping ahead",
                 latenessUs / 1E6);

            mVideoBuffer->release();
            mVideoBuffer = NULL;

            mSeeking = SEEK_VIDEO_ONLY;
            mSeekTimeUs = mediaTimeUs;

            postVideoEvent_l();
            return;
        }

        if (latenessUs > 40000) {
            // We're more than 40ms late.
            ALOGV("we're late by %lld us (%.2f secs), dropping frame",
                 latenessUs, latenessUs / 1E6);
            mVideoBuffer->release();
            mVideoBuffer = NULL;

            postVideoEvent_l();
            return;
        }

        if (latenessUs < -10000) {
            // We're more than 10ms early.

            postVideoEvent_l(10000);
            return;
        }
    }

    if (mVideoRendererIsPreview || mVideoRenderer == NULL) {
        mVideoRendererIsPreview = false;

        initRenderer_l();
    }

    if (mVideoRenderer != NULL) {
        mVideoRenderer->render(mVideoBuffer);
    }

    mVideoBuffer->release();
    mVideoBuffer = NULL;

    if (wasSeeking != NO_SEEK && (mFlags & SEEK_PREVIEW)) {
        mFlags &= ~SEEK_PREVIEW;
        return;
    }

    postVideoEvent_l();
}

void PreviewPlayerBase::postVideoEvent_l(int64_t delayUs) {
    if (mVideoEventPending) {
        return;
    }

    mVideoEventPending = true;
    mQueue.postEventWithDelay(mVideoEvent, delayUs < 0 ? 10000 : delayUs);
}

void PreviewPlayerBase::postStreamDoneEvent_l(status_t status) {
    if (mStreamDoneEventPending) {
        return;
    }
    mStreamDoneEventPending = true;

    mStreamDoneStatus = status;
    mQueue.postEvent(mStreamDoneEvent);
}

void PreviewPlayerBase::postVideoLagEvent_l() {
    if (mVideoLagEventPending) {
        return;
    }
    mVideoLagEventPending = true;
    mQueue.postEventWithDelay(mVideoLagEvent, 1000000ll);
}

void PreviewPlayerBase::postCheckAudioStatusEvent_l(int64_t delayUs) {
    if (mAudioStatusEventPending) {
        return;
    }
    mAudioStatusEventPending = true;
    mQueue.postEventWithDelay(mCheckAudioStatusEvent, delayUs);
}

void PreviewPlayerBase::onCheckAudioStatus() {
    Mutex::Autolock autoLock(mLock);
    if (!mAudioStatusEventPending) {
        // Event was dispatched and while we were blocking on the mutex,
        // has already been cancelled.
        return;
    }

    mAudioStatusEventPending = false;

    if (mWatchForAudioSeekComplete && !mAudioPlayer->isSeeking()) {
        mWatchForAudioSeekComplete = false;

        if (!mSeekNotificationSent) {
            notifyListener_l(MEDIA_SEEK_COMPLETE);
            mSeekNotificationSent = true;
        }

        mSeeking = NO_SEEK;
    }

    status_t finalStatus;
    if (mWatchForAudioEOS && mAudioPlayer->reachedEOS(&finalStatus)) {
        mWatchForAudioEOS = false;
        mFlags |= AUDIO_AT_EOS;
        mFlags |= FIRST_FRAME;
        postStreamDoneEvent_l(finalStatus);
    }
}

status_t PreviewPlayerBase::prepare() {
    Mutex::Autolock autoLock(mLock);
    return prepare_l();
}

status_t PreviewPlayerBase::prepare_l() {
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

status_t PreviewPlayerBase::prepareAsync() {
    Mutex::Autolock autoLock(mLock);

    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;  // async prepare already pending
    }

    mIsAsyncPrepare = true;
    return prepareAsync_l();
}

status_t PreviewPlayerBase::prepareAsync_l() {
    if (mFlags & PREPARING) {
        return UNKNOWN_ERROR;  // async prepare already pending
    }

    if (!mQueueStarted) {
        mQueue.start();
        mQueueStarted = true;
    }

    mFlags |= PREPARING;
    mAsyncPrepareEvent = new AwesomeEvent(
            this, &PreviewPlayerBase::onPrepareAsyncEvent);

    mQueue.postEvent(mAsyncPrepareEvent);

    return OK;
}

status_t PreviewPlayerBase::finishSetDataSource_l() {
    sp<DataSource> dataSource =
            DataSource::CreateFromURI(mUri.string(), NULL);

    if (dataSource == NULL) {
        return UNKNOWN_ERROR;
    }

    sp<MediaExtractor> extractor = MediaExtractor::Create(dataSource);

    return setDataSource_l(extractor);
}

void PreviewPlayerBase::abortPrepare(status_t err) {
    CHECK(err != OK);

    if (mIsAsyncPrepare) {
        notifyListener_l(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err);
    }

    mPrepareResult = err;
    mFlags &= ~(PREPARING|PREPARE_CANCELLED|PREPARING_CONNECTED);
    mAsyncPrepareEvent = NULL;
    mPreparedCondition.broadcast();
}

// static
bool PreviewPlayerBase::ContinuePreparation(void *cookie) {
    PreviewPlayerBase *me = static_cast<PreviewPlayerBase *>(cookie);

    return (me->mFlags & PREPARE_CANCELLED) == 0;
}

void PreviewPlayerBase::onPrepareAsyncEvent() {
    Mutex::Autolock autoLock(mLock);

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

    mFlags |= PREPARING_CONNECTED;

    finishAsyncPrepare_l();
}

void PreviewPlayerBase::finishAsyncPrepare_l() {
    if (mIsAsyncPrepare) {
        if (mVideoSource == NULL) {
            notifyListener_l(MEDIA_SET_VIDEO_SIZE, 0, 0);
        } else {
            notifyVideoSize_l();
        }

        notifyListener_l(MEDIA_PREPARED);
    }

    mPrepareResult = OK;
    mFlags &= ~(PREPARING|PREPARE_CANCELLED|PREPARING_CONNECTED);
    mFlags |= PREPARED;
    mAsyncPrepareEvent = NULL;
    mPreparedCondition.broadcast();
}

uint32_t PreviewPlayerBase::flags() const {
    return mExtractorFlags;
}

void PreviewPlayerBase::postAudioEOS(int64_t delayUs) {
    Mutex::Autolock autoLock(mLock);
    postCheckAudioStatusEvent_l(delayUs);
}

void PreviewPlayerBase::postAudioSeekComplete() {
    Mutex::Autolock autoLock(mLock);
    postCheckAudioStatusEvent_l(0 /* delayUs */);
}

}  // namespace android
