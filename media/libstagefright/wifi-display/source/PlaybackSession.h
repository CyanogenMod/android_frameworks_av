/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PLAYBACK_SESSION_H_

#define PLAYBACK_SESSION_H_

#include "WifiDisplaySource.h"

namespace android {

struct ABuffer;
struct BufferQueue;
struct IHDCP;
struct ISurfaceTexture;
struct MediaPuller;
struct MediaSource;
struct Serializer;
struct TSPacketizer;

#define LOG_TRANSPORT_STREAM    0

// Encapsulates the state of an RTP/RTCP session in the context of wifi
// display.
struct WifiDisplaySource::PlaybackSession : public AHandler {
    PlaybackSession(
            const sp<ANetworkSession> &netSession,
            const sp<AMessage> &notify,
            const struct in_addr &interfaceAddr,
            bool legacyMode,
            const sp<IHDCP> &hdcp);

    enum TransportMode {
        TRANSPORT_UDP,
        TRANSPORT_TCP_INTERLEAVED,
        TRANSPORT_TCP,
    };
    status_t init(
            const char *clientIP, int32_t clientRtp, int32_t clientRtcp,
            TransportMode transportMode);

    status_t destroy();

    int32_t getRTPPort() const;

    int64_t getLastLifesignUs() const;
    void updateLiveness();

    status_t play();
    status_t finishPlay();
    status_t pause();

    sp<ISurfaceTexture> getSurfaceTexture();
    int32_t width() const;
    int32_t height() const;

    void requestIDRFrame();

    enum {
        kWhatSessionDead,
        kWhatBinaryData,
        kWhatSessionEstablished,
    };

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~PlaybackSession();

private:
    struct Track;

    enum {
        kWhatSendSR,
        kWhatRTPNotify,
        kWhatRTCPNotify,
        kWhatSerializerNotify,
        kWhatConverterNotify,
        kWhatUpdateSurface,
        kWhatFinishPlay,
    };

    static const int64_t kSendSRIntervalUs = 10000000ll;
    static const uint32_t kSourceID = 0xdeadbeef;
    static const size_t kMaxHistoryLength = 128;

    sp<ANetworkSession> mNetSession;
    sp<AMessage> mNotify;
    in_addr mInterfaceAddr;
    bool mLegacyMode;
    sp<IHDCP> mHDCP;

    int64_t mLastLifesignUs;

    sp<ALooper> mSerializerLooper;
    sp<Serializer> mSerializer;
    sp<TSPacketizer> mPacketizer;
    sp<BufferQueue> mBufferQueue;

    KeyedVector<size_t, sp<Track> > mTracks;
    ssize_t mVideoTrackIndex;

    sp<ABuffer> mTSQueue;
    int64_t mPrevTimeUs;

    TransportMode mTransportMode;

    AString mClientIP;

    // in TCP mode
    int32_t mRTPChannel;
    int32_t mRTCPChannel;

    // in UDP mode
    int32_t mRTPPort;
    int32_t mRTPSessionID;
    int32_t mRTCPSessionID;

    int32_t mClientRTPPort;
    int32_t mClientRTCPPort;
    bool mRTPConnected;
    bool mRTCPConnected;

    uint32_t mRTPSeqNo;

    uint64_t mLastNTPTime;
    uint32_t mLastRTPTime;
    uint32_t mNumRTPSent;
    uint32_t mNumRTPOctetsSent;
    uint32_t mNumSRsSent;

    bool mSendSRPending;

    int64_t mFirstPacketTimeUs;

    List<sp<ABuffer> > mHistory;
    size_t mHistoryLength;

    uint64_t mTotalBytesSent;

#if LOG_TRANSPORT_STREAM
    FILE *mLogFile;
#endif

    void onSendSR();
    void addSR(const sp<ABuffer> &buffer);
    void addSDES(const sp<ABuffer> &buffer);
    static uint64_t GetNowNTP();

    status_t setupPacketizer();

    status_t addFakeSources();

    status_t addSource(
            bool isVideo,
            const sp<MediaSource> &source,
            size_t *numInputBuffers);

    status_t addVideoSource();
    status_t addAudioSource();

    ssize_t appendTSData(
            const void *data, size_t size, bool timeDiscontinuity, bool flush);

    void scheduleSendSR();

    status_t parseRTCP(const sp<ABuffer> &buffer);
    status_t parseTSFB(const uint8_t *data, size_t size);

    status_t sendPacket(int32_t sessionID, const void *data, size_t size);
    status_t onFinishPlay();
    status_t onFinishPlay2();

    DISALLOW_EVIL_CONSTRUCTORS(PlaybackSession);
};

}  // namespace android

#endif  // PLAYBACK_SESSION_H_

