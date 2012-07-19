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
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/NativeWindowWrapper.h>
#include "NuPlayerStats.h"
#include <media/stagefright/foundation/ABuffer.h>

namespace android {

struct ACodec;
struct MetaData;
struct NuPlayerDriver;

struct NuPlayer : public AHandler {
    NuPlayer();

    void setUID(uid_t uid);

    void setDriver(const wp<NuPlayerDriver> &driver);

    void setDataSource(const sp<IStreamSource> &source);

    void setDataSource(
            const char *url, const KeyedVector<String8, String8> *headers);

    void setDataSource(int fd, int64_t offset, int64_t length);

    void setVideoSurfaceTexture(const sp<ISurfaceTexture> &surfaceTexture);
    void setAudioSink(const sp<MediaPlayerBase::AudioSink> &sink);
    void start();

    void pause();
    void resume();

    // Will notify the driver through "notifyResetComplete" once finished.
    void resetAsync();

    // Will notify the driver through "notifySeekComplete" once finished.
    void seekToAsync(int64_t seekTimeUs);

#ifdef QCOM_HARDWARE
    status_t prepareAsync();
    status_t getParameter(int key, Parcel *reply);
    status_t setParameter(int key, const Parcel &request);

public:
    struct DASHHTTPLiveSource;
    struct WFDSource;
#endif

protected:
    virtual ~NuPlayer();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    struct Decoder;
    struct GenericSource;
    struct HTTPLiveSource;
    struct NuPlayerStreamListener;
    struct Renderer;
    struct RTSPSource;
    struct Source;
    struct StreamingSource;
    struct RTSPSource;
    struct MPQHALWrapper;

    enum {
        kWhatSetDataSource              = '=DaS',
        kWhatSetVideoNativeWindow       = '=NaW',
        kWhatSetAudioSink               = '=AuS',
        kWhatMoreDataQueued             = 'more',
        kWhatStart                      = 'strt',
        kWhatScanSources                = 'scan',
        kWhatVideoNotify                = 'vidN',
        kWhatAudioNotify                = 'audN',
        kWhatTextNotify                 = 'texN',
        kWhatRendererNotify             = 'renN',
        kWhatReset                      = 'rset',
        kWhatSeek                       = 'seek',
        kWhatPause                      = 'paus',
        kWhatResume                     = 'rsme',
        kWhatPrepareAsync               = 'pras',
        kWhatIsPrepareDone              = 'prdn',
        kWhatSourceNotify               = 'snfy',
    };

    enum {
        kWhatBufferingStart             = 'bfst',
        kWhatBufferingEnd               = 'bfen',
    };

    wp<NuPlayerDriver> mDriver;
    bool mUIDValid;
    uid_t mUID;
    sp<Source> mSource;
    sp<NativeWindowWrapper> mNativeWindow;
    sp<MediaPlayerBase::AudioSink> mAudioSink;
    sp<Decoder> mVideoDecoder;
    bool mVideoIsAVC;
    sp<Decoder> mAudioDecoder;
    sp<Decoder> mTextDecoder;
    sp<Renderer> mRenderer;

    bool mAudioEOS;
    bool mVideoEOS;

    bool mScanSourcesPending;
    int32_t mScanSourcesGeneration;
    bool mBufferingNotification;

    enum TrackName {
        kVideo = 0,
        kAudio,
        kText,
        kTrackAll,
    };

    enum FlushStatus {
        NONE,
        AWAITING_DISCONTINUITY,
        FLUSHING_DECODER,
        FLUSHING_DECODER_SHUTDOWN,
        SHUTTING_DOWN_DECODER,
        FLUSHED,
        SHUT_DOWN,
    };

    enum FrameFlags {
         TIMED_TEXT_FLAG_FRAME = 0x00,
         TIMED_TEXT_FLAG_CODEC_CONFIG_FRAME,
         TIMED_TEXT_FLAG_EOS,
         TIMED_TEXT_FLAG_END = TIMED_TEXT_FLAG_EOS,
    };

    // Once the current flush is complete this indicates whether the
    // notion of time has changed.
    bool mTimeDiscontinuityPending;

    FlushStatus mFlushingAudio;
    FlushStatus mFlushingVideo;
    bool mResetInProgress;
    bool mResetPostponed;

    int64_t mSkipRenderingAudioUntilMediaTimeUs;
    int64_t mSkipRenderingVideoUntilMediaTimeUs;

    int64_t mVideoLateByUs;
    int64_t mNumFramesTotal, mNumFramesDropped;

#ifdef QCOM_HARDWARE
    bool mPauseIndication;
#endif

    Mutex mLock;

    char *mTrackName;
    sp<AMessage> mTextNotify;
    sp<AMessage> mSourceNotify;

    enum NuSourceType {
        kHttpLiveSource = 0,
        kHttpDashSource,
        kRtspSource,
        kStreamingSource,
        kWfdSource,
        kGenericSource,
        kDefaultSource
    };
#ifdef QCOM_HARDWARE
    NuSourceType mSourceType;
#endif

    status_t instantiateDecoder(int track, sp<Decoder> *decoder);

    status_t feedDecoderInputData(int track, const sp<AMessage> &msg);
    void renderBuffer(bool audio, const sp<AMessage> &msg);

    void notifyListener(int msg, int ext1, int ext2, const Parcel *obj=NULL);

    void finishFlushIfPossible();

    void flushDecoder(bool audio, bool needShutdown);

    static bool IsFlushingState(FlushStatus state, bool *needShutdown = NULL);

    void finishReset();
    void postScanSources();

#ifdef QCOM_HARDWARE
    sp<Source> LoadCreateSource(const char * uri, const KeyedVector<String8,
                                 String8> *headers, bool uidValid, uid_t uid, NuSourceType srcTyp);

    void postIsPrepareDone();

    // for qualcomm statistics profiling
    sp<NuPlayerStats> mStats;
#endif

    void sendTextPacket(sp<ABuffer> accessUnit, status_t err);
    void getTrackName(int track, char* name);
    void prepareSource();

    DISALLOW_EVIL_CONSTRUCTORS(NuPlayer);
};

}  // namespace android

#endif  // NU_PLAYER_H_
