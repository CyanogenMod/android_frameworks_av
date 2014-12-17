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

#ifndef NU_PLAYER_H_

#define NU_PLAYER_H_

#include <media/MediaPlayerInterface.h>
#include <media/stagefright/ExtendedStats.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/NativeWindowWrapper.h>

#define PLAYER_STATS(func, ...) \
    do { \
        if(mPlayerExtendedStats != NULL) { \
            mPlayerExtendedStats->func(__VA_ARGS__);} \
    } \
    while(0)

namespace android {

struct ABuffer;
struct AMessage;
struct MetaData;
struct NuPlayerDriver;

struct NuPlayer : public AHandler {
    NuPlayer();

    void setUID(uid_t uid);

    void setDriver(const wp<NuPlayerDriver> &driver);

    void setDataSourceAsync(const sp<IStreamSource> &source);

    void setDataSourceAsync(
            const sp<IMediaHTTPService> &httpService,
            const char *url,
            const KeyedVector<String8, String8> *headers);

    void setDataSourceAsync(int fd, int64_t offset, int64_t length);

    void prepareAsync();

    void setVideoSurfaceTextureAsync(
            const sp<IGraphicBufferProducer> &bufferProducer);

    void setAudioSink(const sp<MediaPlayerBase::AudioSink> &sink);
    void start();

    void pause();
    void resume();

    // Will notify the driver through "notifyResetComplete" once finished.
    void resetAsync();

    // Will notify the driver through "notifySeekComplete" once finished
    // and needNotify is true.
    void seekToAsync(int64_t seekTimeUs, bool needNotify = false);

    status_t setVideoScalingMode(int32_t mode);
    status_t getTrackInfo(Parcel* reply) const;
    status_t getSelectedTrack(int32_t type, Parcel* reply) const;
    status_t selectTrack(size_t trackIndex, bool select);
    status_t getCurrentPosition(int64_t *mediaUs);
    void getStats(int64_t *mNumFramesTotal, int64_t *mNumFramesDropped);

    sp<MetaData> getFileMeta();

    int64_t getServerTimeoutUs();

    static const size_t kAggregateBufferSizeBytes;

protected:
    virtual ~NuPlayer();

    virtual void onMessageReceived(const sp<AMessage> &msg);

public:
    struct NuPlayerStreamListener;
    struct Source;

private:
    struct Decoder;
    struct DecoderPassThrough;
    struct CCDecoder;
    struct GenericSource;
    struct HTTPLiveSource;
    struct Renderer;
    struct RTSPSource;
    struct StreamingSource;
    struct Action;
    struct SeekAction;
    struct SetSurfaceAction;
    struct ShutdownDecoderAction;
    struct PostMessageAction;
    struct SimpleAction;

    enum {
        kWhatSetDataSource              = '=DaS',
        kWhatPrepare                    = 'prep',
        kWhatSetVideoNativeWindow       = '=NaW',
        kWhatSetAudioSink               = '=AuS',
        kWhatMoreDataQueued             = 'more',
        kWhatStart                      = 'strt',
        kWhatScanSources                = 'scan',
        kWhatVideoNotify                = 'vidN',
        kWhatAudioNotify                = 'audN',
        kWhatClosedCaptionNotify        = 'capN',
        kWhatRendererNotify             = 'renN',
        kWhatReset                      = 'rset',
        kWhatSeek                       = 'seek',
        kWhatPause                      = 'paus',
        kWhatResume                     = 'rsme',
        kWhatPollDuration               = 'polD',
        kWhatSourceNotify               = 'srcN',
        kWhatGetTrackInfo               = 'gTrI',
        kWhatGetSelectedTrack           = 'gSel',
        kWhatSelectTrack                = 'selT',
    };
    sp<PlayerExtendedStats> mPlayerExtendedStats;

    wp<NuPlayerDriver> mDriver;
    bool mUIDValid;
    uid_t mUID;
    sp<Source> mSource;
    uint32_t mSourceFlags;
    sp<NativeWindowWrapper> mNativeWindow;
    sp<MediaPlayerBase::AudioSink> mAudioSink;
    sp<Decoder> mVideoDecoder;
    bool mVideoIsAVC;
    bool mOffloadAudio;
    bool mIsStreaming;
    sp<Decoder> mAudioDecoder;
    sp<CCDecoder> mCCDecoder;
    sp<Renderer> mRenderer;
    sp<ALooper> mRendererLooper;
    int32_t mAudioDecoderGeneration;
    int32_t mVideoDecoderGeneration;
    int32_t mRendererGeneration;

    List<sp<Action> > mDeferredActions;

    bool mAudioEOS;
    bool mVideoEOS;

    bool mScanSourcesPending;
    int32_t mScanSourcesGeneration;

    int32_t mPollDurationGeneration;
    int32_t mTimedTextGeneration;

    enum FlushStatus {
        NONE,
        FLUSHING_DECODER,
        FLUSHING_DECODER_SHUTDOWN,
        SHUTTING_DOWN_DECODER,
        FLUSHED,
        SHUT_DOWN,
    };

    // Once the current flush is complete this indicates whether the
    // notion of time has changed.
    bool mTimeDiscontinuityPending;

    // Status of flush responses from the decoder and renderer.
    bool mFlushComplete[2][2];

    // Used by feedDecoderInputData to aggregate small buffers into
    // one large buffer.
    sp<ABuffer> mPendingAudioAccessUnit;
    status_t    mPendingAudioErr;
    sp<ABuffer> mAggregateBuffer;

    FlushStatus mFlushingAudio;
    FlushStatus mFlushingVideo;

    int64_t mSkipRenderingAudioUntilMediaTimeUs;
    int64_t mSkipRenderingVideoUntilMediaTimeUs;

    int64_t mNumFramesTotal, mNumFramesDropped;

    int32_t mVideoScalingMode;

    bool mStarted;
    bool mBuffering;
    bool mPlaying;

    inline const sp<Decoder> &getDecoder(bool audio) {
        return audio ? mAudioDecoder : mVideoDecoder;
    }

    inline void clearFlushComplete() {
        mFlushComplete[0][0] = false;
        mFlushComplete[0][1] = false;
        mFlushComplete[1][0] = false;
        mFlushComplete[1][1] = false;
    }

    void openAudioSink(const sp<AMessage> &format, bool offloadOnly);
    void closeAudioSink();

    status_t instantiateDecoder(bool audio, sp<Decoder> *decoder);

    void updateVideoSize(
            const sp<AMessage> &inputFormat,
            const sp<AMessage> &outputFormat = NULL);

    status_t feedDecoderInputData(bool audio, const sp<AMessage> &msg);
    void renderBuffer(bool audio, const sp<AMessage> &msg);

    void notifyListener(int msg, int ext1, int ext2, const Parcel *in = NULL);

    void handleFlushComplete(bool audio, bool isDecoder);
    void finishFlushIfPossible();

    bool audioDecoderStillNeeded();

    void flushDecoder(
            bool audio, bool needShutdown, const sp<AMessage> &newFormat = NULL);
    void updateDecoderFormatWithoutFlush(bool audio, const sp<AMessage> &format);

    void postScanSources();

    void schedulePollDuration();
    void cancelPollDuration();

    void processDeferredActions();

    void performSeek(int64_t seekTimeUs, bool needNotify);
    void performDecoderFlush();
    void performDecoderShutdown(bool audio, bool video);
    void performReset();
    void performScanSources();
    void performSetSurface(const sp<NativeWindowWrapper> &wrapper);

    void onSourceNotify(const sp<AMessage> &msg);
    void onClosedCaptionNotify(const sp<AMessage> &msg);

    void queueDecoderShutdown(
            bool audio, bool video, const sp<AMessage> &reply);

    void sendSubtitleData(const sp<ABuffer> &buffer, int32_t baseIndex);
    void sendTimedTextData(const sp<ABuffer> &buffer);

    void writeTrackInfo(Parcel* reply, const sp<AMessage> format) const;

    DISALLOW_EVIL_CONSTRUCTORS(NuPlayer);
};

}  // namespace android

#endif  // NU_PLAYER_H_
