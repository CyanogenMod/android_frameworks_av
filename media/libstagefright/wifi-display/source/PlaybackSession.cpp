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
#include "TSPacketizer.h"
#include "include/avc_utils.h"

#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <media/IHDCP.h>
#include <media/stagefright/foundation/ABitReader.h>
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

namespace android {

static size_t kMaxRTPPacketSize = 1500;
static size_t kMaxNumTSPacketsPerRTPPacket = (kMaxRTPPacketSize - 12) / 188;

struct WifiDisplaySource::PlaybackSession::Track : public AHandler {
    enum {
        kWhatStopped,
    };

    Track(const sp<AMessage> &notify,
          const sp<ALooper> &pullLooper,
          const sp<ALooper> &codecLooper,
          const sp<MediaPuller> &mediaPuller,
          const sp<Converter> &converter);

    sp<AMessage> getFormat();
    bool isAudio() const;

    const sp<Converter> &converter() const;
    ssize_t packetizerTrackIndex() const;

    void setPacketizerTrackIndex(size_t index);

    status_t start();
    void stopAsync();

    bool isStopped() const { return !mStarted; }

    void queueAccessUnit(const sp<ABuffer> &accessUnit);
    sp<ABuffer> dequeueAccessUnit();

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~Track();

private:
    enum {
        kWhatMediaPullerStopped,
    };

    sp<AMessage> mNotify;
    sp<ALooper> mPullLooper;
    sp<ALooper> mCodecLooper;
    sp<MediaPuller> mMediaPuller;
    sp<Converter> mConverter;
    bool mStarted;
    ssize_t mPacketizerTrackIndex;
    bool mIsAudio;
    List<sp<ABuffer> > mQueuedAccessUnits;

    static bool IsAudioFormat(const sp<AMessage> &format);

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

WifiDisplaySource::PlaybackSession::Track::Track(
        const sp<AMessage> &notify,
        const sp<ALooper> &pullLooper,
        const sp<ALooper> &codecLooper,
        const sp<MediaPuller> &mediaPuller,
        const sp<Converter> &converter)
    : mNotify(notify),
      mPullLooper(pullLooper),
      mCodecLooper(codecLooper),
      mMediaPuller(mediaPuller),
      mConverter(converter),
      mStarted(false),
      mPacketizerTrackIndex(-1),
      mIsAudio(IsAudioFormat(mConverter->getOutputFormat())) {
}

WifiDisplaySource::PlaybackSession::Track::~Track() {
    CHECK(!mStarted);
}

// static
bool WifiDisplaySource::PlaybackSession::Track::IsAudioFormat(
        const sp<AMessage> &format) {
    AString mime;
    CHECK(format->findString("mime", &mime));

    return !strncasecmp(mime.c_str(), "audio/", 6);
}

sp<AMessage> WifiDisplaySource::PlaybackSession::Track::getFormat() {
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
    ALOGV("Track::start isAudio=%d", mIsAudio);

    CHECK(!mStarted);

    status_t err = OK;

    if (mMediaPuller != NULL) {
        err = mMediaPuller->start();
    }

    if (err == OK) {
        mStarted = true;
    }

    return err;
}

void WifiDisplaySource::PlaybackSession::Track::stopAsync() {
    ALOGV("Track::stopAsync isAudio=%d", mIsAudio);

    CHECK(mStarted);

    mConverter->shutdownAsync();

    sp<AMessage> msg = new AMessage(kWhatMediaPullerStopped, id());

    if (mMediaPuller != NULL) {
        mMediaPuller->stopAsync(msg);
    } else {
        msg->post();
    }
}

void WifiDisplaySource::PlaybackSession::Track::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatMediaPullerStopped:
        {
            mConverter.clear();

            mStarted = false;

            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatStopped);
            notify->post();
            break;
        }

        default:
            TRESPASS();
    }
}

void WifiDisplaySource::PlaybackSession::Track::queueAccessUnit(
        const sp<ABuffer> &accessUnit) {
    mQueuedAccessUnits.push_back(accessUnit);
}

sp<ABuffer> WifiDisplaySource::PlaybackSession::Track::dequeueAccessUnit() {
    if (mQueuedAccessUnits.empty()) {
        return NULL;
    }

    sp<ABuffer> accessUnit = *mQueuedAccessUnits.begin();
    CHECK(accessUnit != NULL);

    mQueuedAccessUnits.erase(mQueuedAccessUnits.begin());

    return accessUnit;
}

////////////////////////////////////////////////////////////////////////////////

WifiDisplaySource::PlaybackSession::PlaybackSession(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify,
        const in_addr &interfaceAddr,
        const sp<IHDCP> &hdcp)
    : mNetSession(netSession),
      mNotify(notify),
      mInterfaceAddr(interfaceAddr),
      mHDCP(hdcp),
      mWeAreDead(false),
      mLastLifesignUs(),
      mVideoTrackIndex(-1),
      mTSQueue(new ABuffer(12 + kMaxNumTSPacketsPerRTPPacket * 188)),
      mPrevTimeUs(-1ll),
      mTransportMode(TRANSPORT_UDP),
      mAllTracksHavePacketizerIndex(false),
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
    // XXX Give the dongle a second to bind its sockets.
    (new AMessage(kWhatFinishPlay, id()))->post(1000000ll);
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

    for (size_t i = 0; i < mTracks.size(); ++i) {
        CHECK_EQ((status_t)OK, mTracks.editValueAt(i)->start());
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

void WifiDisplaySource::PlaybackSession::destroyAsync() {
    ALOGI("destroyAsync");

    for (size_t i = 0; i < mTracks.size(); ++i) {
        mTracks.valueAt(i)->stopAsync();
    }
}

void WifiDisplaySource::PlaybackSession::onMessageReceived(
        const sp<AMessage> &msg) {
    if (mWeAreDead) {
        return;
    }

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

                    notifySessionDead();
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

        case kWhatConverterNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Converter::kWhatAccessUnit) {
                const sp<Track> &track = mTracks.valueFor(trackIndex);

                ssize_t packetizerTrackIndex = track->packetizerTrackIndex();

                if (packetizerTrackIndex < 0) {
                    packetizerTrackIndex =
                        mPacketizer->addTrack(track->getFormat());

                    CHECK_GE(packetizerTrackIndex, 0);

                    track->setPacketizerTrackIndex(packetizerTrackIndex);

                    if (allTracksHavePacketizerIndex()) {
                        status_t err = packetizeQueuedAccessUnits();

                        if (err != OK) {
                            notifySessionDead();
                            break;
                        }
                    }
                }

                sp<ABuffer> accessUnit;
                CHECK(msg->findBuffer("accessUnit", &accessUnit));

                if (!allTracksHavePacketizerIndex()) {
                    track->queueAccessUnit(accessUnit);
                    break;
                }

                status_t err = packetizeAccessUnit(trackIndex, accessUnit);

                if (err != OK) {
                    notifySessionDead();
                }
                break;
            } else if (what == Converter::kWhatEOS) {
                CHECK_EQ(what, Converter::kWhatEOS);

                ALOGI("output EOS on track %d", trackIndex);

                ssize_t index = mTracks.indexOfKey(trackIndex);
                CHECK_GE(index, 0);

                const sp<Converter> &converter =
                    mTracks.valueAt(index)->converter();
                looper()->unregisterHandler(converter->id());

                mTracks.removeItemsAt(index);

                if (mTracks.isEmpty()) {
                    ALOGI("Reached EOS");
                }
            } else {
                CHECK_EQ(what, Converter::kWhatError);

                status_t err;
                CHECK(msg->findInt32("err", &err));

                ALOGE("converter signaled error %d", err);

                notifySessionDead();
            }
            break;
        }

        case kWhatFinishPlay:
        {
            onFinishPlay();
            break;
        }

        case kWhatTrackNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            size_t trackIndex;
            CHECK(msg->findSize("trackIndex", &trackIndex));

            if (what == Track::kWhatStopped) {
                bool allTracksAreStopped = true;
                for (size_t i = 0; i < mTracks.size(); ++i) {
                    const sp<Track> &track = mTracks.valueAt(i);
                    if (!track->isStopped()) {
                        allTracksAreStopped = false;
                        break;
                    }
                }

                if (!allTracksAreStopped) {
                    break;
                }

                mTracks.clear();

                mPacketizer.clear();

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

                sp<AMessage> notify = mNotify->dup();
                notify->setInt32("what", kWhatSessionDestroyed);
                notify->post();
            }
            break;
        }

        default:
            TRESPASS();
    }
}

status_t WifiDisplaySource::PlaybackSession::setupPacketizer() {
    mPacketizer = new TSPacketizer;

    status_t err = addVideoSource();

    if (err != OK) {
        return err;
    }

    return addAudioSource();
}

status_t WifiDisplaySource::PlaybackSession::addSource(
        bool isVideo, const sp<MediaSource> &source, size_t *numInputBuffers) {
    sp<ALooper> pullLooper = new ALooper;
    pullLooper->setName("pull_looper");

    pullLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    sp<ALooper> codecLooper = new ALooper;
    codecLooper->setName("codec_looper");

    codecLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_AUDIO);

    size_t trackIndex;

    sp<AMessage> notify;

    trackIndex = mTracks.size();

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

    notify = new AMessage(Converter::kWhatMediaPullerNotify, converter->id());
    notify->setSize("trackIndex", trackIndex);

    sp<MediaPuller> puller = new MediaPuller(source, notify);
    pullLooper->registerHandler(puller);

    if (numInputBuffers != NULL) {
        *numInputBuffers = converter->getInputBufferCount();
    }

    notify = new AMessage(kWhatTrackNotify, id());
    notify->setSize("trackIndex", trackIndex);

    sp<Track> track = new Track(
            notify, pullLooper, codecLooper, puller, converter);

    looper()->registerHandler(track);

    mTracks.add(trackIndex, track);

    if (isVideo) {
        mVideoTrackIndex = trackIndex;
    }

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::addVideoSource() {
    sp<SurfaceMediaSource> source = new SurfaceMediaSource(width(), height());

    sp<MediaSource> videoSource =
            new RepeaterSource(source, 24.0 /* rateHz */);

    size_t numInputBuffers;
    status_t err = addSource(true /* isVideo */, videoSource, &numInputBuffers);

    if (err != OK) {
        return err;
    }

    err = source->setMaxAcquiredBufferCount(numInputBuffers);
    CHECK_EQ(err, (status_t)OK);

    mBufferQueue = source->getBufferQueue();

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
    return 1280;
}

int32_t WifiDisplaySource::PlaybackSession::height() const {
    return 720;
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

bool WifiDisplaySource::PlaybackSession::allTracksHavePacketizerIndex() {
    if (mAllTracksHavePacketizerIndex) {
        return true;
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        if (mTracks.valueAt(i)->packetizerTrackIndex() < 0) {
            return false;
        }
    }

    mAllTracksHavePacketizerIndex = true;

    return true;
}

static inline size_t MIN(size_t a, size_t b) {
    return (a < b) ? a : b;
}

status_t WifiDisplaySource::PlaybackSession::packetizeAccessUnit(
        size_t trackIndex, sp<ABuffer> accessUnit) {
    const sp<Track> &track = mTracks.valueFor(trackIndex);

    uint32_t flags = 0;

    bool isHDCPEncrypted = false;
    uint64_t inputCTR;
    uint8_t HDCP_private_data[16];
    if (mHDCP != NULL && !track->isAudio()) {
        isHDCPEncrypted = true;

#if 0
        ALOGI("in:");
        hexdump(accessUnit->data(), MIN(64, accessUnit->size()));
#endif

        if (IsIDR(accessUnit)) {
            // XXX remove this once the encoder takes care of this.
            accessUnit = mPacketizer->prependCSD(
                    track->packetizerTrackIndex(), accessUnit);
        }

        status_t err = mHDCP->encrypt(
                accessUnit->data(), accessUnit->size(),
                trackIndex  /* streamCTR */,
                &inputCTR,
                accessUnit->data());

        if (err != OK) {
            ALOGE("Failed to HDCP-encrypt media data (err %d)",
                  err);

            return err;
        } else {
#if 0
            ALOGI("out:");
            hexdump(accessUnit->data(), MIN(64, accessUnit->size()));
            ALOGI("inputCTR: 0x%016llx", inputCTR);
            ALOGI("streamCTR: 0x%08x", trackIndex);
#endif
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

#if 0
        ALOGI("HDCP_private_data:");
        hexdump(HDCP_private_data, sizeof(HDCP_private_data));

        ABitReader br(HDCP_private_data, sizeof(HDCP_private_data));
        CHECK_EQ(br.getBits(13), 0);
        CHECK_EQ(br.getBits(2), (trackIndex >> 30) & 3);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (trackIndex >> 15) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), trackIndex & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(11), 0);
        CHECK_EQ(br.getBits(4), (inputCTR >> 60) & 0xf);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (inputCTR >> 45) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (inputCTR >> 30) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), (inputCTR >> 15) & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
        CHECK_EQ(br.getBits(15), inputCTR & 0x7fff);
        CHECK_EQ(br.getBits(1), 1u);
#endif

        flags |= TSPacketizer::IS_ENCRYPTED;
    }

    int64_t timeUs = ALooper::GetNowUs();
    if (mPrevTimeUs < 0ll || mPrevTimeUs + 100000ll <= timeUs) {
        flags |= TSPacketizer::EMIT_PCR;
        flags |= TSPacketizer::EMIT_PAT_AND_PMT;

        mPrevTimeUs = timeUs;
    }

    sp<ABuffer> packets;
    mPacketizer->packetize(
            track->packetizerTrackIndex(), accessUnit, &packets, flags,
            !isHDCPEncrypted ? NULL : HDCP_private_data,
            !isHDCPEncrypted ? 0 : sizeof(HDCP_private_data));

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

    return OK;
}

status_t WifiDisplaySource::PlaybackSession::packetizeQueuedAccessUnits() {
    for (;;) {
        bool gotMoreData = false;
        for (size_t i = 0; i < mTracks.size(); ++i) {
            size_t trackIndex = mTracks.keyAt(i);
            const sp<Track> &track = mTracks.valueAt(i);

            sp<ABuffer> accessUnit = track->dequeueAccessUnit();
            if (accessUnit != NULL) {
                status_t err = packetizeAccessUnit(trackIndex, accessUnit);

                if (err != OK) {
                    return err;
                }

                gotMoreData = true;
            }
        }

        if (!gotMoreData) {
            break;
        }
    }

    return OK;
}

void WifiDisplaySource::PlaybackSession::notifySessionDead() {
    // Inform WifiDisplaySource of our premature death (wish).
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatSessionDead);
    notify->post();

    mWeAreDead = true;
}

}  // namespace android

