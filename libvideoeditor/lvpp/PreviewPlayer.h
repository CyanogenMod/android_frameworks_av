/*
 * Copyright (C) 2011 NXP Software
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

#ifndef PREVIEW_PLAYER_H_

#define PREVIEW_PLAYER_H_

#include "NuHTTPDataSource.h"
#include "TimedEventQueue.h"
#include "VideoEditorAudioPlayer.h"

#include <media/MediaPlayerInterface.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/TimeSource.h>
#include <utils/threads.h>
#include <AwesomePlayer.h>
#include "VideoEditorPreviewController.h"

namespace android {

struct AudioPlayer;
struct DataSource;
struct MediaBuffer;
struct MediaExtractor;
struct MediaSource;

struct PreviewPlayerRenderer : public RefBase {
    PreviewPlayerRenderer() {}

    virtual void render(MediaBuffer *buffer) = 0;
    virtual void render() = 0;
    virtual void getBuffer(uint8_t **data, size_t *stride) = 0;

private:
    PreviewPlayerRenderer(const PreviewPlayerRenderer &);
    PreviewPlayerRenderer &operator=(const PreviewPlayerRenderer &);
};

struct PreviewPlayer : public AwesomePlayer {
    PreviewPlayer();
    ~PreviewPlayer();

    //Override baseclass methods
    void reset();

    status_t play();

    void setISurface(const sp<ISurface> &isurface);

    status_t seekTo(int64_t timeUs);

    status_t getVideoDimensions(int32_t *width, int32_t *height) const;

    status_t suspend();
    status_t resume();

    status_t prepare();
    status_t setDataSource(
        const char *uri, const KeyedVector<String8, String8> *headers);

    //Added methods
    status_t loadEffectsSettings(M4VSS3GPP_EffectSettings* pEffectSettings,
                                 int nEffects);
    status_t loadAudioMixSettings(M4xVSS_AudioMixingSettings* pAudioMixSettings);
    status_t setAudioMixPCMFileHandle(M4OSA_Context pAudioMixPCMFileHandle);
    status_t setAudioMixStoryBoardParam(M4OSA_UInt32 audioMixStoryBoardTS,
                            M4OSA_UInt32 currentMediaBeginCutTime,
                            M4OSA_UInt32 currentMediaVolumeVol);

    status_t setPlaybackBeginTime(uint32_t msec);
    status_t setPlaybackEndTime(uint32_t msec);
    status_t setStoryboardStartTime(uint32_t msec);
    status_t setProgressCallbackInterval(uint32_t cbInterval);
    status_t setMediaRenderingMode(M4xVSS_MediaRendering mode,
                            M4VIDEOEDITING_VideoFrameSize outputVideoSize);

    status_t resetJniCallbackTimeStamp();
    status_t setImageClipProperties(uint32_t width, uint32_t height);
    status_t readFirstVideoFrame();


private:
    friend struct PreviewPlayerEvent;

    enum {
        PLAYING             = 1,
        LOOPING             = 2,
        FIRST_FRAME         = 4,
        PREPARING           = 8,
        PREPARED            = 16,
        AT_EOS              = 32,
        PREPARE_CANCELLED   = 64,
        CACHE_UNDERRUN      = 128,
        AUDIO_AT_EOS        = 256,
        VIDEO_AT_EOS        = 512,
        AUTO_LOOPING        = 1024,
    };

    sp<ISurface> mISurface;

    void cancelPlayerEvents(bool keepBufferingGoing = false);
    status_t setDataSource_l(const sp<MediaExtractor> &extractor);
    status_t setDataSource_l(
        const char *uri, const KeyedVector<String8, String8> *headers);
    void reset_l();
    void partial_reset_l();
    status_t play_l();
    status_t initRenderer_l();
    status_t initAudioDecoder();
    status_t initVideoDecoder(uint32_t flags = 0);
    void onVideoEvent();
    status_t finishSetDataSource_l();
    static bool ContinuePreparation(void *cookie);
    void onPrepareAsyncEvent();
    void finishAsyncPrepare_l();

    sp<PreviewPlayerRenderer> mVideoRenderer;

    int32_t mVideoWidth, mVideoHeight;

    MediaBuffer *mLastVideoBuffer;

    struct SuspensionState {
        String8 mUri;
        KeyedVector<String8, String8> mUriHeaders;
        sp<DataSource> mFileSource;

        uint32_t mFlags;
        int64_t mPositionUs;

        void *mLastVideoFrame;
        size_t mLastVideoFrameSize;
        int32_t mColorFormat;
        int32_t mVideoWidth, mVideoHeight;
        int32_t mDecodedWidth, mDecodedHeight;

        SuspensionState()
            : mLastVideoFrame(NULL) {
        }

        ~SuspensionState() {
            if (mLastVideoFrame) {
                free(mLastVideoFrame);
                mLastVideoFrame = NULL;
            }
        }
    } *mSuspensionState;

    //Data structures used for audio and video effects
    M4VSS3GPP_EffectSettings* mEffectsSettings;
    M4xVSS_AudioMixingSettings* mPreviewPlayerAudioMixSettings;
    M4OSA_Context mAudioMixPCMFileHandle;
    M4OSA_UInt32 mAudioMixStoryBoardTS;
    M4OSA_UInt32 mCurrentMediaBeginCutTime;
    M4OSA_UInt32 mCurrentMediaVolumeValue;
    M4OSA_UInt32 mCurrFramingEffectIndex;

    uint32_t mNumberEffects;
    uint32_t mPlayBeginTimeMsec;
    uint32_t mPlayEndTimeMsec;
    uint64_t mDecodedVideoTs; // timestamp of current decoded video frame buffer
    uint64_t mDecVideoTsStoryBoard; // timestamp of frame relative to storyboard
    uint32_t mCurrentVideoEffect;
    uint32_t mProgressCbInterval;
    uint32_t mNumberDecVideoFrames; // Counter of number of video frames decoded
    sp<TimedEventQueue::Event> mProgressCbEvent;
    bool mProgressCbEventPending;
    sp<TimedEventQueue::Event> mOverlayUpdateEvent;
    bool mOverlayUpdateEventPending;
    bool mOverlayUpdateEventPosted;

    MediaBuffer *mResizedVideoBuffer;
    bool mVideoResizedOrCropped;
    M4xVSS_MediaRendering mRenderingMode;
    uint32_t mOutputVideoWidth;
    uint32_t mOutputVideoHeight;

    int32_t mReportedWidth;  //docoder reported width
    int32_t mReportedHeight; //docoder reported height

    uint32_t mStoryboardStartTimeMsec;

    bool mIsVideoSourceJpg;
    bool mIsFiftiesEffectStarted;
    int64_t mImageFrameTimeUs;
    bool mStartNextPlayer;

    M4VIFI_UInt8*  mFrameRGBBuffer;
    M4VIFI_UInt8*  mFrameYUVBuffer;

    void setVideoPostProcessingNode(
                    M4VSS3GPP_VideoEffectType type, M4OSA_Bool enable);
    M4OSA_ERR doVideoPostProcessing();
    M4OSA_ERR doMediaRendering();
    void postProgressCallbackEvent_l();
    void onProgressCbEvent();

    void postOverlayUpdateEvent_l();
    void onUpdateOverlayEvent();

    status_t setDataSource_l_jpg();

    status_t prepare_l();
    status_t prepareAsync_l();

    PreviewPlayer(const PreviewPlayer &);
    PreviewPlayer &operator=(const PreviewPlayer &);
};

}  // namespace android

#endif  // PREVIEW_PLAYER_H_

