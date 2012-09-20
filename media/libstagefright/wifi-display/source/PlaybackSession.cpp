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

//#define LOG_NDEBUG 0
#define LOG_TAG "PlaybackSession"
#include <utils/Log.h>

#include "PlaybackSession.h"

#include "Converter.h"
#include "MediaPuller.h"
#include "RepeaterSource.h"
#include "Serializer.h"
#include "TSPacketizer.h"

#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <media/IHDCP.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MPEG2TSWriter.h>
#include <media/stagefright/SurfaceMediaSource.h>
#include <media/stagefright/Utils.h>

#include <OMX_IVCommon.h>

//#define FAKE_VIDEO            1
#define USE_SERIALIZER          0

namespace android {

static size_t kMaxRTPPacketSize = 1500;
static size_t kMaxNumTSPacketsPerRTPPacket = (kMaxRTPPacketSize - 12) / 188;

struct WifiDisplaySource::PlaybackSession::Track : public RefBase {
    Track(const sp<ALooper> &pullLooper,
          const sp<ALooper> &codecLooper,
          const sp<MediaPuller> &mediaPuller,
          const sp<Converter> &converter);

    Track(const sp<AMessage> &format);

    sp<AMessage> getFormat();
    bool isAudio() const;

    const sp<Converter> &converter() const;
    ssize_t packetizerTrackIndex() const;

    void setPacketizerTrackIndex(size_t index);

    status_t start();
    status_t stop();

protected:
    virtual ~Track();

private:
    sp<ALooper> mPullLooper;
    sp<ALooper> mCodecLooper;
    sp<MediaPuller> mMediaPuller;
    sp<Converter> mConverter;
    sp<AMessage> mFormat;
    bool mStarted;
    ssize_t mPacketizerTrackIndex;
    bool mIsAudio;

    static bool IsAudioFormat(const sp<AMessage> &format);

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

WifiDisplaySource::PlaybackSession::Track::Track(
        const sp<ALooper> &pullLooper,
        const sp<ALooper> &codecLooper,
        const sp<MediaPuller> &mediaPuller,
        const sp<Converter> &converter)
    : mPullLooper(pullLooper),
      mCodecLooper(codecLooper),
      mMediaPuller(mediaPuller),
      mConverter(converter),
      mStarted(false),
      mPacketizerTrackIndex(-1),
      mIsAudio(IsAudioFormat(mConverter->getOutputFormat())) {
}

WifiDisplaySource::PlaybackSession::Track::Track(const sp<AMessage> &format)
    : mFormat(format),
      mPacketizerTrackIndex(-1),
      mIsAudio(IsAudioFormat(mFormat)) {
}

WifiDisplaySource::PlaybackSession::Track::~Track() {
    stop();
}

// static
bool WifiDisplaySource::PlaybackSession::Track::IsAudioFormat(
        const sp<AMessage> &format) {
    AString mime;
    CHECK(format->findString("mime", &mime));

    return !strncasecmp(mime.c_str(), "audio/", 6);
}

sp<AMessage> WifiDisplaySource::PlaybackSession::Track::getFormat() {
    if (mFormat != NULL) {
        return mFormat;
    }

    return mConverter->getOutputFormat();
}

bool WifiDisplaySource::PlaybackSession::Track::isAudio() const {
    return mIsAudio;
}

const sp<Converter> &WifiDisplaySource::PlaybackSession::Track::converter() const {
    return mConverter;
}

ssize_t WifiDisplaySource::PlaybackSession::Track::packetizerTrackIndex() const {
    return mPacketizerTrackIndex;
}

void WifiDisplaySource::PlaybackSession::Track::setPacketizerTrackIndex(size_t index) {
    CHECK_LT(mPacketizerTrackIndex, 0);
    mPacketizerTrackIndex = index;
}

status_t WifiDisplaySource::PlaybackSession::Track::start() {
    if (mStarted) {
        return INVALID_OPERATION;
    }

    status_t err = OK;

    if (mMediaPuller != NULL) {
        err = mMediaPuller->start();
    }

    if (err == OK) {
        mStarted = true;
    }

    return err;
}

status_t WifiDisplaySource::PlaybackSession::Track::stop() {
    if (!mStarted) {
        return INVALID_OPERATION;
    }

    status_t err = OK;

    if (mMediaPuller != NULL) {
        err = mMediaPuller->stop();
    }

    mConverter.clear();

    mStarted = false;

    return err;
}

////////////////////////////////////////////////////////////////////////////////

WifiDisplaySource::PlaybackSession::PlaybackSession(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify,
        const in_addr &interfaceAddr,
        bool legacyMode,
        const sp<IHDCP> &hdcp)
    : mNetSession(netSession),
      mNotify(notify),
      mInterfaceAddr(interfaceAddr),
      mLegacyMode(legacyMode),
      mHDCP(hdcp),
      mLastLifesignUs(),
      mVideoTrackIndex(-1),
      mTSQueue(new ABuffer(12 + kMaxNumTSPacketsPerRTPPacket * 188)),
      mPrevTimeUs(-1ll),
      mTransportMode(TRANSPORT_UDP),
      mRTPChannel(0),
      mRTCPChannel(0),
      mRTPPort(0),
      mRTPSessionID(0),
      mRTCPSessionID(0),
#if ENABLE_RETRANSMISSION
      mRTPRetransmissionSessionID(0),
      mRTCPRetransmissionSessionID(0),
#endif
      mClientRTPPort(0),
      mClientRTCPPort(0),
      mRTPConnected(false),
      mRTCPConnected(false),
      mRTPSeqNo(0),
#if ENABLE_RETRANSMISSION
      mRTPRetransmissionSeqNo(0),
#endif
      mLastNTPTime(0),
      mLastRTPTime(0),
      mNumRTPSent(0),
      mNumRTPOctetsSent(0),
      mNumSRsSent(0),
      mSendSRPending(false),
      mFirstPacketTimeUs(-1ll),
      mHistoryLength(0),
      mTotalBytesSent(0ll)
#if LOG_TRANSPORT_STREAM
      ,mLogFile(NULL)
#endif
{
    mTSQueue->setRange(0, 12);

#if LOG_TRANSPORT_STREAM
    mLogFile = fopen("/system/etc/log.ts", "wb");
#endif
}

status_t WifiDisplaySource::PlaybackSession::init(
        const char *clientIP, int32_t clientRtp, int32_t clientRtcp,
        TransportMode transportMode) {
    mClientIP = clientIP;

    status_t err = setupPacketizer();

    if (err != OK) {
        return err;
    }

    mTransportMode = transportMode;

    if (transportMode == TRANSPORT_TCP_INTERLEAVED) {
        mRTPChannel = clientRtp;
        mRTCPChannel = clientRtcp;
        mRTPPort = 0;
        mRTPSessionID = 0;
        mRTCPSessionID = 0;

        updateLiveness();
        return OK;
    }

    mRTPChannel = 0;
    mRTCPChannel = 0;

    if (mTransportMode == TRANSPORT_TCP) {
        // XXX This is wrong, we need to allocate sockets here, we only
        // need to do this because the dongles are not establishing their
        // end until after PLAY instead of before SETUP.
        mRTPPort = 20000;
        mRTPSessionID = 0;
        mRTCPSessionID = 0;
        mClientRTPPort = clientRtp;
        mClientRTCPPort = clientRtcp;

        updateLiveness();
        return OK;
    }

    int serverRtp;

    sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, id());
    sp<AMessage> rtcpNotify = new AMessage(kWhatRTCPNotify, id());

#if ENABLE_RETRANSMISSION
    sp<AMessage> rtpRetransmissionNotify =
        new AMessage(kWhatRTPRetransmissionNotify, id());

    sp<AMessage> rtcpRetransmissionNotify =
        new AMessage(kWhatRTCPRetransmissionNotify, id());
#endif

    for (serverRtp = 15550;; serverRtp += 2) {
        int32_t rtpSession;
        if (mTransportMode == TRANSPORT_UDP) {
            err = mNetSession->createUDPSession(
                        serverRtp, clientIP, clientRtp,
                        rtpNotify, &rtpSession);
        } else {
            err = mNetSession->createTCPDatagramSession(
                        serverRtp, clientIP, clientRtp,
                        rtpNotify, &rtpSession);
        }

        if (err != OK) {
            ALOGI("failed to create RTP socket on port %d", serverRtp);
            continue;
        }

        int32_t rtcpSession = 0;

        if (clientRtcp >= 0) {
            if (mTransportMode == TRANSPORT_UDP) {
                err = mNetSession->createUDPSession(
                        serverRtp + 1, clientIP, clientRtcp,
                        rtcpNotify, &rtcpSession);
            } else {
                err = mNetSession->createTCPDatagramSession(
                        serverRtp + 1, clientIP, clientRtcp,
                        rtcpNotify, &rtcpSession);
            }

            if (err != OK) {
                ALOGI("failed to create RTCP socket on port %d", serverRtp + 1);

                mNetSession->destroySession(rtpSession);
                continue;
            }
        }

#if ENABLE_RETRANSMISSION
        if (mTransportMode == TRANSPORT_UDP) {
            int32_t rtpRetransmissionSession;

            err = mNetSession->createUDPSession(
                        serverRtp + kRetransmissionPortOffset,
                        clientIP,
                        clientRtp + kRetransmissionPortOffset,
                        rtpRetransmissionNotify,
                        &rtpRetransmissionSession);

            if (err != OK) {
                mNetSession->destroySession(rtcpSession);
                mNetSession->destroySession(rtpSession);
                continue;
            }

            CHECK_GE(clientRtcp, 0);

            int32_t rtcpRetransmissionSession;
            err = mNetSession->createUDPSession(
                        serverRtp + 1 + kRetransmissionPortOffset,
                        clientIP,
                        clientRtp + 1 + kRetransmissionPortOffset,
                        rtcpRetransmissionNotify,
                        &rtcpRetransmissionSession);

            if (err != OK) {
                mNetSession->destroySession(rtpRetransmissionSession);
                mNetSession->destroySession(rtcpSession);
                mNetSession->destroySession(rtpSession);
                continue;
            }

            mRTPRetransmissionSessionID = rtpRetransmissionSession;
            mRTCPRetransmissionSessionID = rtcpRetransmissionSession;

            ALOGI("rtpRetransmissionSessionID = %d, "
                  "rtcpRetransmissionSessionID = %d",
                  rtpRetransmissionSession, rtcpRetransmissionSession);
        }
#endif

        mRTPPort = serverRtp;
        mRTPSessionID = rtpSession;
        mRTCPSessionID = rtcpSession;

        ALOGI("rtpSessionID = %d, rtcpSessionID = %d", rtpSession, rtcpSession);
        break;
    }

    if (mRTPPort == 0) {
        return UNKNOWN_ERROR;
    }

    updateLiveness();

    return OK;
}

WifiDisplaySource::PlaybackSession::~PlaybackSession() {
#if LOG_TRANSPORT_STREAM
    if (mLogFile != NULL) {
        fclose(mLogFile);
        mLogFile = NULL;
    }
#endif
}

int32_t WifiDisplaySource::PlaybackSession::getRTPPort() const {
    return mRTPPort;
}

int64_t WifiDisplaySource::PlaybackSession::getLastLifesignUs() const {
    return mLastLifesignUs;
}

void WifiDisplaySource::PlaybackSession::updateLiveness() {
    mLastLifesignUs = ALooper::GetNowUs();
}

status_t WifiDisplaySource::PlaybackSession::play() {
    updateLiveness();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::finishPlay() {
    // XXX Give the dongle 3 secs to bind its sockets.
    (new AMessage(kWhatFinishPlay, id()))->post(3000000ll);
    return OK;
}

status_t WifiDisplaySource::PlaybackSession::onFinishPlay() {
    if (mTransportMode != TRANSPORT_TCP) {
        return onFinishPlay2();
    }

    sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, id());

    status_t err = mNetSession->createTCPDatagramSession(
                mRTPPort, mClientIP.c_str(), mClientRTPPort,
                rtpNotify, &mRTPSessionID);

    if (err != OK) {
        return err;
    }

    if (mClientRTCPPort >= 0) {
        sp<AMessage> rtcpNotify = new AMessage(kWhatRTCPNotify, id());

        err = mNetSession->createTCPDatagramSession(
                mRTPPort + 1, mClientIP.c_str(), mClientRTCPPort,
                rtcpNotify, &mRTCPSessionID);
    }

    return err;
}

status_t WifiDisplaySource::PlaybackSession::onFinishPlay2() {
    if (mRTCPSessionID != 0) {
        scheduleSendSR();
    }

    if (mSerializer != NULL) {
        return mSerializer->start();
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        status_t err = mTracks.editValueAt(i)->start();

        if (err != OK) {
            for (size_t j = 0; j < i; ++j) {
                mTracks.editValueAt(j)->stop();
            }

            return err;
        }
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionEstablished);
    notify->post();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::pause() {
    updateLiveness();

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::destroy() {
    mTracks.clear();

    mPacketizer.clear();

    if (mSerializer != NULL) {
        mSerializer->stop();

        looper()->unregisterHandler(mSerializer->id());
        mSerializer.clear();
    }

    mTracks.clear();

    if (mSerializerLooper != NULL) {
        mSerializerLooper->stop();
        mSerializerLooper.clear();
    }

    if (mLegacyMode) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16("SurfaceFlinger"));
        sp<ISurfaceComposer> service = interface_cast<ISurfaceComposer>(binder);
        CHECK(service != NULL);

        service->connectDisplay(NULL);
    }

#if ENABLE_RETRANSMISSION
    if (mRTCPRetransmissionSessionID != 0) {
        mNetSession->destroySession(mRTCPRetransmissionSessionID);
    }

    if (mRTPRetransmissionSessionID != 0) {
        mNetSession->destroySession(mRTPRetransmissionSessionID);
    }
#endif

    if (mRTCPSessionID != 0) {
        mNetSession->destroySession(mRTCPSessionID);
    }

    if (mRTPSessionID != 0) {
        mNetSession->destroySession(mRTPSessionID);
    }

    return OK;
}

void WifiDisplaySource::PlaybackSession::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRTPNotify:
        case kWhatRTCPNotify:
#if ENABLE_RETRANSMISSION
        case kWhatRTPRetransmissionNotify:
        case kWhatRTCPRetransmissionNotify:
#endif
        {
            int32_t reason;
            CHECK(msg->findInt32("reason", &reason));

            switch (reason) {
                case ANetworkSession::kWhatError:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    int32_t err;
                    CHECK(msg->findInt32("err", &err));

                    int32_t errorOccuredDuringSend;
                    CHECK(msg->findInt32("send", &errorOccuredDuringSend));

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    if ((msg->what() == kWhatRTPNotify
#if ENABLE_RETRANSMISSION
                            || msg->what() == kWhatRTPRetransmissionNotify
#endif
                        ) && !errorOccuredDuringSend) {
                        // This is ok, we don't expect to receive anything on
                        // the RTP socket.
                        break;
                    }

                    ALOGE("An error occurred during %s in session %d "
                          "(%d, '%s' (%s)).",
                          errorOccuredDuringSend ? "send" : "receive",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    mNetSession->destroySession(sessionID);

                    if (sessionID == mRTPSessionID) {
                        mRTPSessionID = 0;
                    } else if (sessionID == mRTCPSessionID) {
                        mRTCPSessionID = 0;
                    }
#if ENABLE_RETRANSMISSION
                    else if (sessionID == mRTPRetransmissionSessionID) {
                        mRTPRetransmissionSessionID = 0;
                    } else if (sessionID == mRTCPRetransmissionSessionID) {
                        mRTCPRetransmissionSessionID = 0;
                    }
#endif

                    // Inform WifiDisplaySource of our premature death (wish).
                    sp<AMessage> notify = mNotify->dup();
                    notify->setInt32("what", kWhatSessionDead);
                    notify->post();
                    break;
                }

                case ANetworkSession::kWhatDatagram:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    sp<ABuffer> data;
                    CHECK(msg->findBuffer("data", &data));

                    status_t err;
                    if (msg->what() == kWhatRTCPNotify
#if ENABLE_RETRANSMISSION
                            || msg->what() == kWhatRTCPRetransmissionNotify
#endif
                       )
                    {
                        err = parseRTCP(data);
                    }
                    break;
                }

                case ANetworkSession::kWhatConnected:
                {
                    CHECK_EQ(mTransportMode, TRANSPORT_TCP);

                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    if (sessionID == mRTPSessionID) {
                        CHECK(!mRTPConnected);
                        mRTPConnected = true;
                        ALOGI("RTP Session now connected.");
                    } else if (sessionID == mRTCPSessionID) {
                        CHECK(!mRTCPConnected);
                        mRTCPConnected = true;
                        ALOGI("RTCP Session now connected.");
                    } else {
                        TRESPASS();
                    }

                    if (mRTPConnected
                            && (mClientRTCPPort < 0 || mRTCPConnected)) {
                        onFinishPlay2();
                    }
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatSendSR:
        {
            mSendSRPending = false;

            if (mRTCPSessionID == 0) {
                break;
            }

            onSendSR();

            scheduleSendSR();
            break;
        }

        case kWhatSerializerNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == Serializer::kWhatEOS) {
                ALOGI("input eos");

                for (size_t i = 0; i < mTracks.size(); ++i) {
#if FAKE_VIDEO
                    sp<AMessage> msg = new AMessage(kWhatConverterNotify, id());
                    msg->setInt32("what", Converter::kWhatEOS);
                    msg->setSize("trackIndex", i);
                    msg->post();
#else
                    mTracks.valueAt(i)->converter()->signalEOS();
#endif
                }
            } else {
                CHECK_EQ(what, Serializer::kWhatAccessUnit);

                size_t trackIndex;
                CHECK(msg->findSize("trackIndex", &trackIndex));

                sp<ABuffer> accessUnit;
                CHECK(msg->findBuffer("accessUnit", &accessUnit));

#if FAKE_VIDEO
                int64_t timeUs;
                CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

                void *mbuf;
                CHECK(accessUnit->meta()->findPointer("mediaBuffer", &mbuf));

                ((MediaBuffer *)mbuf)->release();
                mbuf = NULL;

                sp<AMessage> msg = new AMessage(kWhatConverterNotify, id());
                msg->setInt32("what", Converter::kWhatAccessUnit);
                msg->setSize("trackIndex", trackIndex);
                msg->setBuffer("accessUnit", accessUnit);
                msg->post();
#else
                mTracks.valueFor(trackIndex)->converter()
                    ->feedAccessUnit(accessUnit);
#endif
            }
            break;
        }

        case kWhatConverterNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Converter::kWhatAccessUnit) {
                const sp<Track> &track = mTracks.valueFor(trackIndex);

                uint32_t flags = 0;

                ssize_t packetizerTrackIndex = track->packetizerTrackIndex();
                if (packetizerTrackIndex < 0) {
                    flags = TSPacketizer::EMIT_PAT_AND_PMT;

                    packetizerTrackIndex =
                        mPacketizer->addTrack(track->getFormat());

                    if (packetizerTrackIndex >= 0) {
                        track->setPacketizerTrackIndex(packetizerTrackIndex);
                    }
                }

                if (packetizerTrackIndex >= 0) {
                    sp<ABuffer> accessUnit;
                    CHECK(msg->findBuffer("accessUnit", &accessUnit));

                    bool isHDCPEncrypted = false;
                    uint64_t inputCTR;
                    uint8_t HDCP_private_data[16];
                    if (mHDCP != NULL && !track->isAudio()) {
                        isHDCPEncrypted = true;

                        status_t err = mHDCP->encrypt(
                                accessUnit->data(), accessUnit->size(),
                                trackIndex  /* streamCTR */,
                                &inputCTR,
                                accessUnit->data());

                        if (err != OK) {
                            ALOGI("Failed to HDCP-encrypt media data (err %d)",
                                  err);

                            // Inform WifiDisplaySource of our premature death
                            // (wish).
                            sp<AMessage> notify = mNotify->dup();
                            notify->setInt32("what", kWhatSessionDead);
                            notify->post();
                            break;
                        }

                        HDCP_private_data[0] = 0x00;

                        HDCP_private_data[1] =
                            (((trackIndex >> 30) & 3) << 1) | 1;

                        HDCP_private_data[2] = (trackIndex >> 22) & 0xff;

                        HDCP_private_data[3] =
                            (((trackIndex >> 15) & 0x7f) << 1) | 1;

                        HDCP_private_data[4] = (trackIndex >> 7) & 0xff;

                        HDCP_private_data[5] =
                            ((trackIndex & 0x7f) << 1) | 1;

                        HDCP_private_data[6] = 0x00;

                        HDCP_private_data[7] =
                            (((inputCTR >> 60) & 0x0f) << 1) | 1;

                        HDCP_private_data[8] = (inputCTR >> 52) & 0xff;

                        HDCP_private_data[9] =
                            (((inputCTR >> 45) & 0x7f) << 1) | 1;

                        HDCP_private_data[10] = (inputCTR >> 37) & 0xff;

                        HDCP_private_data[11] =
                            (((inputCTR >> 30) & 0x7f) << 1) | 1;

                        HDCP_private_data[12] = (inputCTR >> 22) & 0xff;

                        HDCP_private_data[13] =
                            (((inputCTR >> 15) & 0x7f) << 1) | 1;

                        HDCP_private_data[14] = (inputCTR >> 7) & 0xff;

                        HDCP_private_data[15] =
                            ((inputCTR & 0x7f) << 1) | 1;

                        flags |= TSPacketizer::IS_ENCRYPTED;
                    }

                    int64_t timeUs;
                    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

                    if (mPrevTimeUs < 0ll || mPrevTimeUs + 100000ll >= timeUs) {
                        flags |= TSPacketizer::EMIT_PCR;
                        mPrevTimeUs = timeUs;
                    }

                    sp<ABuffer> packets;
                    mPacketizer->packetize(
                            packetizerTrackIndex, accessUnit, &packets, flags,
                            isHDCPEncrypted ? NULL : HDCP_private_data,
                            isHDCPEncrypted ? 0 : sizeof(HDCP_private_data));

                    for (size_t offset = 0;
                            offset < packets->size(); offset += 188) {
                        bool lastTSPacket = (offset + 188 >= packets->size());

                        // We're only going to flush video, audio packets are
                        // much more frequent and would waste all that space
                        // available in a full sized UDP packet.
                        bool flush =
                            lastTSPacket
                                && ((ssize_t)trackIndex == mVideoTrackIndex);

                        appendTSData(
                                packets->data() + offset,
                                188,
                                true /* timeDiscontinuity */,
                                flush);
                    }

#if LOG_TRANSPORT_STREAM
                    if (mLogFile != NULL) {
                        fwrite(packets->data(), 1, packets->size(), mLogFile);
                    }
#endif
                }
            } else if (what == Converter::kWhatEOS) {
                CHECK_EQ(what, Converter::kWhatEOS);

                ALOGI("output EOS on track %d", trackIndex);

                ssize_t index = mTracks.indexOfKey(trackIndex);
                CHECK_GE(index, 0);

#if !FAKE_VIDEO
                const sp<Converter> &converter =
                    mTracks.valueAt(index)->converter();
                looper()->unregisterHandler(converter->id());
#endif

                mTracks.removeItemsAt(index);

                if (mTracks.isEmpty()) {
                    ALOGI("Reached EOS");
                }
            } else {
                CHECK_EQ(what, Converter::kWhatError);

                status_t err;
                CHECK(msg->findInt32("err", &err));

                ALOGE("converter signaled error %d", err);
            }
            break;
        }

        case kWhatFinishPlay:
        {
            onFinishPlay();
            break;
        }

        default:
            TRESPASS();
    }
}

status_t WifiDisplaySource::PlaybackSession::setupPacketizer() {
    sp<AMessage> msg = new AMessage(kWhatSerializerNotify, id());

    mPacketizer = new TSPacketizer;

#if FAKE_VIDEO
    return addFakeSources();
#else
    status_t err = addVideoSource();

    if (err != OK) {
        return err;
    }

    return addAudioSource();
#endif
}

status_t WifiDisplaySource::PlaybackSession::addFakeSources() {
#if FAKE_VIDEO
    mSerializerLooper = new ALooper;
    mSerializerLooper->setName("serializer_looper");
    mSerializerLooper->start();

    sp<AMessage> msg = new AMessage(kWhatSerializerNotify, id());
    mSerializer = new Serializer(
            true /* throttled */, msg);

    mSerializerLooper->registerHandler(mSerializer);

    DataSource::RegisterDefaultSniffers();

    sp<DataSource> dataSource =
        DataSource::CreateFromURI(
                "/system/etc/inception_1500.mp4");

    CHECK(dataSource != NULL);

    sp<MediaExtractor> extractor = MediaExtractor::Create(dataSource);
    CHECK(extractor != NULL);

    bool haveAudio = false;
    bool haveVideo = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        bool useTrack = false;
        if (!strncasecmp(mime, "audio/", 6) && !haveAudio) {
            useTrack = true;
            haveAudio = true;
        } else if (!strncasecmp(mime, "video/", 6) && !haveVideo) {
            useTrack = true;
            haveVideo = true;
        }

        if (!useTrack) {
            continue;
        }

        sp<MediaSource> source = extractor->getTrack(i);

        ssize_t index = mSerializer->addSource(source);
        CHECK_GE(index, 0);

        sp<AMessage> format;
        status_t err = convertMetaDataToMessage(source->getFormat(), &format);
        CHECK_EQ(err, (status_t)OK);

        mTracks.add(index, new Track(format));
    }
    CHECK(haveAudio || haveVideo);
#endif

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addSource(
        bool isVideo, const sp<MediaSource> &source, size_t *numInputBuffers) {
#if USE_SERIALIZER
    if (mSerializer == NULL) {
        mSerializerLooper = new ALooper;
        mSerializerLooper->setName("serializer_looper");
        mSerializerLooper->start();

        sp<AMessage> msg = new AMessage(kWhatSerializerNotify, id());
        mSerializer = new Serializer(
                false /* throttled */, msg);

        mSerializerLooper->registerHandler(mSerializer);
    }
#else
    sp<ALooper> pullLooper = new ALooper;
    pullLooper->setName("pull_looper");

    pullLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_DEFAULT);
#endif

    sp<ALooper> codecLooper = new ALooper;
    codecLooper->setName("codec_looper");

    codecLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_DEFAULT);

    size_t trackIndex;

    sp<AMessage> notify;

#if USE_SERIALIZER
    trackIndex = mSerializer->addSource(source);
#else
    trackIndex = mTracks.size();

    notify = new AMessage(kWhatSerializerNotify, id());
    notify->setSize("trackIndex", trackIndex);
    sp<MediaPuller> puller = new MediaPuller(source, notify);
    pullLooper->registerHandler(puller);
#endif

    sp<AMessage> format;
    status_t err = convertMetaDataToMessage(source->getFormat(), &format);
    CHECK_EQ(err, (status_t)OK);

    if (isVideo) {
        format->setInt32("store-metadata-in-buffers", true);

        format->setInt32(
                "color-format", OMX_COLOR_FormatAndroidOpaque);
    }

    notify = new AMessage(kWhatConverterNotify, id());
    notify->setSize("trackIndex", trackIndex);

    sp<Converter> converter =
        new Converter(notify, codecLooper, format);
    CHECK_EQ(converter->initCheck(), (status_t)OK);

    looper()->registerHandler(converter);

    if (numInputBuffers != NULL) {
        *numInputBuffers = converter->getInputBufferCount();
    }

#if USE_SERIALIZER
    mTracks.add(trackIndex, new Track(NULL, codecLooper, NULL, converter));
#else
    mTracks.add(trackIndex, new Track(pullLooper, codecLooper, puller, converter));
#endif

    if (isVideo) {
        mVideoTrackIndex = trackIndex;
    }

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addVideoSource() {
    sp<SurfaceMediaSource> source = new SurfaceMediaSource(width(), height());

    sp<MediaSource> videoSource =
            new RepeaterSource(source, 30.0 /* rateHz */);

    size_t numInputBuffers;
    status_t err = addSource(true /* isVideo */, videoSource, &numInputBuffers);

    if (err != OK) {
        return err;
    }

    // Add one reference to account for the serializer.
    err = source->setMaxAcquiredBufferCount(numInputBuffers);
    CHECK_EQ(err, (status_t)OK);

    mBufferQueue = source->getBufferQueue();

    if (mLegacyMode) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16("SurfaceFlinger"));
        sp<ISurfaceComposer> service = interface_cast<ISurfaceComposer>(binder);
        CHECK(service != NULL);

        service->connectDisplay(mBufferQueue);
    }

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addAudioSource() {
    sp<AudioSource> audioSource = new AudioSource(
            AUDIO_SOURCE_REMOTE_SUBMIX,
            48000 /* sampleRate */,
            2 /* channelCount */);

    if (audioSource->initCheck() == OK) {
        audioSource->setUseLooperTime(true);

        return addSource(
                false /* isVideo */, audioSource, NULL /* numInputBuffers */);
    }

    ALOGW("Unable to instantiate audio source");

    return OK;
}

sp<ISurfaceTexture> WifiDisplaySource::PlaybackSession::getSurfaceTexture() {
    return mBufferQueue;
}

int32_t WifiDisplaySource::PlaybackSession::width() const {
    return mLegacyMode ? 720 : 1280;
}

int32_t WifiDisplaySource::PlaybackSession::height() const {
    return mLegacyMode ? 1280 : 720;
}

void WifiDisplaySource::PlaybackSession::scheduleSendSR() {
    if (mSendSRPending) {
        return;
    }

    mSendSRPending = true;
    (new AMessage(kWhatSendSR, id()))->post(kSendSRIntervalUs);
}

void WifiDisplaySource::PlaybackSession::addSR(const sp<ABuffer> &buffer) {
    uint8_t *data = buffer->data() + buffer->size();

    // TODO: Use macros/utility functions to clean up all the bitshifts below.

    data[0] = 0x80 | 0;
    data[1] = 200;  // SR
    data[2] = 0;
    data[3] = 6;
    data[4] = kSourceID >> 24;
    data[5] = (kSourceID >> 16) & 0xff;
    data[6] = (kSourceID >> 8) & 0xff;
    data[7] = kSourceID & 0xff;

    data[8] = mLastNTPTime >> (64 - 8);
    data[9] = (mLastNTPTime >> (64 - 16)) & 0xff;
    data[10] = (mLastNTPTime >> (64 - 24)) & 0xff;
    data[11] = (mLastNTPTime >> 32) & 0xff;
    data[12] = (mLastNTPTime >> 24) & 0xff;
    data[13] = (mLastNTPTime >> 16) & 0xff;
    data[14] = (mLastNTPTime >> 8) & 0xff;
    data[15] = mLastNTPTime & 0xff;

    data[16] = (mLastRTPTime >> 24) & 0xff;
    data[17] = (mLastRTPTime >> 16) & 0xff;
    data[18] = (mLastRTPTime >> 8) & 0xff;
    data[19] = mLastRTPTime & 0xff;

    data[20] = mNumRTPSent >> 24;
    data[21] = (mNumRTPSent >> 16) & 0xff;
    data[22] = (mNumRTPSent >> 8) & 0xff;
    data[23] = mNumRTPSent & 0xff;

    data[24] = mNumRTPOctetsSent >> 24;
    data[25] = (mNumRTPOctetsSent >> 16) & 0xff;
    data[26] = (mNumRTPOctetsSent >> 8) & 0xff;
    data[27] = mNumRTPOctetsSent & 0xff;

    buffer->setRange(buffer->offset(), buffer->size() + 28);
}

void WifiDisplaySource::PlaybackSession::addSDES(const sp<ABuffer> &buffer) {
    uint8_t *data = buffer->data() + buffer->size();
    data[0] = 0x80 | 1;
    data[1] = 202;  // SDES
    data[4] = kSourceID >> 24;
    data[5] = (kSourceID >> 16) & 0xff;
    data[6] = (kSourceID >> 8) & 0xff;
    data[7] = kSourceID & 0xff;

    size_t offset = 8;

    data[offset++] = 1;  // CNAME

    static const char *kCNAME = "someone@somewhere";
    data[offset++] = strlen(kCNAME);

    memcpy(&data[offset], kCNAME, strlen(kCNAME));
    offset += strlen(kCNAME);

    data[offset++] = 7;  // NOTE

    static const char *kNOTE = "Hell's frozen over.";
    data[offset++] = strlen(kNOTE);

    memcpy(&data[offset], kNOTE, strlen(kNOTE));
    offset += strlen(kNOTE);

    data[offset++] = 0;

    if ((offset % 4) > 0) {
        size_t count = 4 - (offset % 4);
        switch (count) {
            case 3:
                data[offset++] = 0;
            case 2:
                data[offset++] = 0;
            case 1:
                data[offset++] = 0;
        }
    }

    size_t numWords = (offset / 4) - 1;
    data[2] = numWords >> 8;
    data[3] = numWords & 0xff;

    buffer->setRange(buffer->offset(), buffer->size() + offset);
}

// static
uint64_t WifiDisplaySource::PlaybackSession::GetNowNTP() {
    uint64_t nowUs = ALooper::GetNowUs();

    nowUs += ((70ll * 365 + 17) * 24) * 60 * 60 * 1000000ll;

    uint64_t hi = nowUs / 1000000ll;
    uint64_t lo = ((1ll << 32) * (nowUs % 1000000ll)) / 1000000ll;

    return (hi << 32) | lo;
}

void WifiDisplaySource::PlaybackSession::onSendSR() {
    sp<ABuffer> buffer = new ABuffer(1500);
    buffer->setRange(0, 0);

    addSR(buffer);
    addSDES(buffer);

    if (mTransportMode == TRANSPORT_TCP_INTERLEAVED) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatBinaryData);
        notify->setInt32("channel", mRTCPChannel);
        notify->setBuffer("data", buffer);
        notify->post();
    } else {
        sendPacket(mRTCPSessionID, buffer->data(), buffer->size());
    }

    ++mNumSRsSent;
}

ssize_t WifiDisplaySource::PlaybackSession::appendTSData(
        const void *data, size_t size, bool timeDiscontinuity, bool flush) {
    CHECK_EQ(size, 188);

    CHECK_LE(mTSQueue->size() + size, mTSQueue->capacity());

    memcpy(mTSQueue->data() + mTSQueue->size(), data, size);
    mTSQueue->setRange(0, mTSQueue->size() + size);

    if (flush || mTSQueue->size() == mTSQueue->capacity()) {
        // flush

        int64_t nowUs = ALooper::GetNowUs();
        if (mFirstPacketTimeUs < 0ll) {
            mFirstPacketTimeUs = nowUs;
        }

        // 90kHz time scale
        uint32_t rtpTime = ((nowUs - mFirstPacketTimeUs) * 9ll) / 100ll;

        uint8_t *rtp = mTSQueue->data();
        rtp[0] = 0x80;
        rtp[1] = 33 | (timeDiscontinuity ? (1 << 7) : 0);  // M-bit
        rtp[2] = (mRTPSeqNo >> 8) & 0xff;
        rtp[3] = mRTPSeqNo & 0xff;
        rtp[4] = rtpTime >> 24;
        rtp[5] = (rtpTime >> 16) & 0xff;
        rtp[6] = (rtpTime >> 8) & 0xff;
        rtp[7] = rtpTime & 0xff;
        rtp[8] = kSourceID >> 24;
        rtp[9] = (kSourceID >> 16) & 0xff;
        rtp[10] = (kSourceID >> 8) & 0xff;
        rtp[11] = kSourceID & 0xff;

        ++mRTPSeqNo;
        ++mNumRTPSent;
        mNumRTPOctetsSent += mTSQueue->size() - 12;

        mLastRTPTime = rtpTime;
        mLastNTPTime = GetNowNTP();

        if (mTransportMode == TRANSPORT_TCP_INTERLEAVED) {
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatBinaryData);

            sp<ABuffer> data = new ABuffer(mTSQueue->size());
            memcpy(data->data(), rtp, mTSQueue->size());

            notify->setInt32("channel", mRTPChannel);
            notify->setBuffer("data", data);
            notify->post();
        } else {
            sendPacket(mRTPSessionID, rtp, mTSQueue->size());

            mTotalBytesSent += mTSQueue->size();
            int64_t delayUs = ALooper::GetNowUs() - mFirstPacketTimeUs;

            if (delayUs > 0ll) {
                ALOGV("approx. net bandwidth used: %.2f Mbit/sec",
                        mTotalBytesSent * 8.0 / delayUs);
            }
        }

        mTSQueue->setInt32Data(mRTPSeqNo - 1);
        mHistory.push_back(mTSQueue);
        ++mHistoryLength;

        if (mHistoryLength > kMaxHistoryLength) {
            mTSQueue = *mHistory.begin();
            mHistory.erase(mHistory.begin());

            --mHistoryLength;
        } else {
            mTSQueue = new ABuffer(12 + kMaxNumTSPacketsPerRTPPacket * 188);
        }

        mTSQueue->setRange(0, 12);
    }

    return size;
}

status_t WifiDisplaySource::PlaybackSession::parseRTCP(
        const sp<ABuffer> &buffer) {
    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    while (size > 0) {
        if (size < 8) {
            // Too short to be a valid RTCP header
            return ERROR_MALFORMED;
        }

        if ((data[0] >> 6) != 2) {
            // Unsupported version.
            return ERROR_UNSUPPORTED;
        }

        if (data[0] & 0x20) {
            // Padding present.

            size_t paddingLength = data[size - 1];

            if (paddingLength + 12 > size) {
                // If we removed this much padding we'd end up with something
                // that's too short to be a valid RTP header.
                return ERROR_MALFORMED;
            }

            size -= paddingLength;
        }

        size_t headerLength = 4 * (data[2] << 8 | data[3]) + 4;

        if (size < headerLength) {
            // Only received a partial packet?
            return ERROR_MALFORMED;
        }

        switch (data[1]) {
            case 200:
            case 201:  // RR
            case 202:  // SDES
            case 203:
            case 204:  // APP
                break;

#if ENABLE_RETRANSMISSION
            case 205:  // TSFB (transport layer specific feedback)
                parseTSFB(data, headerLength);
                break;
#endif

            case 206:  // PSFB (payload specific feedback)
                hexdump(data, headerLength);
                break;

            default:
            {
                ALOGW("Unknown RTCP packet type %u of size %d",
                     (unsigned)data[1], headerLength);
                break;
            }
        }

        data += headerLength;
        size -= headerLength;
    }

    return OK;
}

#if ENABLE_RETRANSMISSION
status_t WifiDisplaySource::PlaybackSession::parseTSFB(
        const uint8_t *data, size_t size) {
    if ((data[0] & 0x1f) != 1) {
        return ERROR_UNSUPPORTED;  // We only support NACK for now.
    }

    uint32_t srcId = U32_AT(&data[8]);
    if (srcId != kSourceID) {
        return ERROR_MALFORMED;
    }

    for (size_t i = 12; i < size; i += 4) {
        uint16_t seqNo = U16_AT(&data[i]);
        uint16_t blp = U16_AT(&data[i + 2]);

        List<sp<ABuffer> >::iterator it = mHistory.begin();
        bool foundSeqNo = false;
        while (it != mHistory.end()) {
            const sp<ABuffer> &buffer = *it;

            uint16_t bufferSeqNo = buffer->int32Data() & 0xffff;

            bool retransmit = false;
            if (bufferSeqNo == seqNo) {
                retransmit = true;
            } else if (blp != 0) {
                for (size_t i = 0; i < 16; ++i) {
                    if ((blp & (1 << i))
                        && (bufferSeqNo == ((seqNo + i + 1) & 0xffff))) {
                        blp &= ~(1 << i);
                        retransmit = true;
                    }
                }
            }

            if (retransmit) {
                ALOGI("retransmitting seqNo %d", bufferSeqNo);

                sp<ABuffer> retransRTP = new ABuffer(2 + buffer->size());
                uint8_t *rtp = retransRTP->data();
                memcpy(rtp, buffer->data(), 12);
                rtp[2] = (mRTPRetransmissionSeqNo >> 8) & 0xff;
                rtp[3] = mRTPRetransmissionSeqNo & 0xff;
                rtp[12] = (bufferSeqNo >> 8) & 0xff;
                rtp[13] = bufferSeqNo & 0xff;
                memcpy(&rtp[14], buffer->data() + 12, buffer->size() - 12);

                ++mRTPRetransmissionSeqNo;

                sendPacket(
                        mRTPRetransmissionSessionID,
                        retransRTP->data(), retransRTP->size());

                if (bufferSeqNo == seqNo) {
                    foundSeqNo = true;
                }

                if (foundSeqNo && blp == 0) {
                    break;
                }
            }

            ++it;
        }

        if (!foundSeqNo || blp != 0) {
            ALOGI("Some sequence numbers were no longer available for "
                  "retransmission");
        }
    }

    return OK;
}
#endif

void WifiDisplaySource::PlaybackSession::requestIDRFrame() {
    for (size_t i = 0; i < mTracks.size(); ++i) {
        const sp<Track> &track = mTracks.valueAt(i);

        track->converter()->requestIDRFrame();
    }
}

status_t WifiDisplaySource::PlaybackSession::sendPacket(
        int32_t sessionID, const void *data, size_t size) {
    return mNetSession->sendRequest(sessionID, data, size);
}

}  // namespace android

