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

    // Will notify the driver through "notifyResetComplete" once finished.
    void resetAsync();

    // Will notify the driver through "notifySeekComplete" once finished
    // and needNotify is true.
    void seekToAsync(int64_t seekTimeUs, bool needNotify = false);

    status_t setVideoScalingMode(int32_t mode);
    status_t getTrackInfo(Parcel* reply) const;
    status_t getSelectedTrack(int32_t type, Parcel* reply) const;
    status_t selectTrack(size_t trackIndex, bool select, int64_t timeUs);
    status_t getCurrentPosition(int64_t *mediaUs);
    void getStats(int64_t *mNumFramesTotal, int64_t *mNumFramesDropped);

    sp<MetaData> getFileMeta();
    int64_t getServerTimeoutUs();

protected:
    virtual ~NuPlayer();

    virtual void onMessageReceived(const sp<AMessage> &msg);

public:
    struct NuPlayerStreamListener;
    struct Source;

private:
    struct Decoder;
    struct DecoderBase;
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
    struct ResumeDecoderAction;
    struct FlushDecoderAction;
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
    sp<DecoderBase> mVideoDecoder;
    bool mOffloadAudio;

    bool mOffloadDecodedPCM;
    bool mSwitchingFromPcmOffload;
    bool mIsStreaming;
    sp<DecoderBase> mAudioDecoder;

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

    enum FlushCommand {
        FLUSH_CMD_NONE,
        FLUSH_CMD_FLUSH,
        FLUSH_CMD_SHUTDOWN,
    };

    // Status of flush responses from the decoder and renderer.
    bool mFlushComplete[2][2];

    FlushStatus mFlushingAudio;
    FlushStatus mFlushingVideo;

    // Status of flush responses from the decoder and renderer.
    bool mResumePending;

    int32_t mVideoScalingMode;

    bool mStarted;

    // Actual pause state, either as requested by client or due to buffering.
    bool mPaused;

    // Pause state as requested by client. Note that if mPausedByClient is
    // true, mPaused is always true; if mPausedByClient is false, mPaused could
    // still become true, when we pause internally due to buffering.
    bool mPausedByClient;

    bool mOffloadAudioTornDown;

    inline const sp<DecoderBase> &getDecoder(bool audio) {
        return audio ? mAudioDecoder : mVideoDecoder;
    }

    inline void clearFlushComplete() {
        mFlushComplete[0][0] = false;
        mFlushComplete[0][1] = false;
        mFlushComplete[1][0] = false;
        mFlushComplete[1][1] = false;
    }

    void tryOpenAudioSinkForOffload(const sp<AMessage> &format, bool hasVideo);
    void closeAudioSink();

    status_t instantiateDecoder(bool audio, sp<DecoderBase> *decoder);

    status_t onInstantiateSecureDecoders();

    void updateVideoSize(
            const sp<AMessage> &inputFormat,
            const sp<AMessage> &outputFormat = NULL);

    void notifyListener(int msg, int ext1, int ext2, const Parcel *in = NULL);

    void handleFlushComplete(bool audio, bool isDecoder);
    void finishFlushIfPossible();

    void onStart();
    void onResume();
    void onPause();

    bool audioDecoderStillNeeded();

    void flushDecoder(bool audio, bool needShutdown);

    void finishResume();

    void postScanSources();

    void schedulePollDuration();
    void cancelPollDuration();

    void processDeferredActions();

    void performSeek(int64_t seekTimeUs, bool needNotify);
    void performDecoderFlush(FlushCommand audio, FlushCommand video);
    void performReset();
    void performScanSources();
    void performSetSurface(const sp<NativeWindowWrapper> &wrapper);
    void performResumeDecoders(bool needNotify);

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
