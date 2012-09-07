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
#include "RepeaterSource.h"
#include "Serializer.h"
#include "TSPacketizer.h"

#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
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

#define FAKE_VIDEO      0

namespace android {

static size_t kMaxRTPPacketSize = 1500;
static size_t kMaxNumTSPacketsPerRTPPacket = (kMaxRTPPacketSize - 12) / 188;

struct WifiDisplaySource::PlaybackSession::Track : public RefBase {
    Track(const sp<Converter> &converter);
    Track(const sp<AMessage> &format);

    sp<AMessage> getFormat();

    const sp<Converter> &converter() const;
    ssize_t packetizerTrackIndex() const;

    void setPacketizerTrackIndex(size_t index);

protected:
    virtual ~Track();

private:
    sp<Converter> mConverter;
    sp<AMessage> mFormat;
    ssize_t mPacketizerTrackIndex;

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

WifiDisplaySource::PlaybackSession::Track::Track(const sp<Converter> &converter)
    : mConverter(converter),
      mPacketizerTrackIndex(-1) {
}

WifiDisplaySource::PlaybackSession::Track::Track(const sp<AMessage> &format)
    : mFormat(format),
      mPacketizerTrackIndex(-1) {
}

WifiDisplaySource::PlaybackSession::Track::~Track() {
}

sp<AMessage> WifiDisplaySource::PlaybackSession::Track::getFormat() {
    if (mFormat != NULL) {
        return mFormat;
    }

    return mConverter->getOutputFormat();
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

////////////////////////////////////////////////////////////////////////////////

WifiDisplaySource::PlaybackSession::PlaybackSession(
        const sp<ANetworkSession> &netSession,
        const sp<AMessage> &notify,
        bool legacyMode)
    : mNetSession(netSession),
      mNotify(notify),
      mLegacyMode(legacyMode),
      mLastLifesignUs(),
      mTSQueue(new ABuffer(12 + kMaxNumTSPacketsPerRTPPacket * 188)),
      mPrevTimeUs(-1ll),
      mUseInterleavedTCP(false),
      mRTPChannel(0),
      mRTCPChannel(0),
      mRTPPort(0),
      mRTPSessionID(0),
      mRTCPSessionID(0),
      mRTPSeqNo(0),
      mLastNTPTime(0),
      mLastRTPTime(0),
      mNumRTPSent(0),
      mNumRTPOctetsSent(0),
      mNumSRsSent(0),
      mSendSRPending(false),
      mFirstPacketTimeUs(-1ll),
      mHistoryLength(0)
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
        bool useInterleavedTCP) {
    status_t err = setupPacketizer();

    if (err != OK) {
        return err;
    }

    if (useInterleavedTCP) {
        mUseInterleavedTCP = true;
        mRTPChannel = clientRtp;
        mRTCPChannel = clientRtcp;
        mRTPPort = 0;
        mRTPSessionID = 0;
        mRTCPSessionID = 0;

        updateLiveness();
        return OK;
    }

    mUseInterleavedTCP = false;
    mRTPChannel = 0;
    mRTCPChannel = 0;

    int serverRtp;

    sp<AMessage> rtpNotify = new AMessage(kWhatRTPNotify, id());
    sp<AMessage> rtcpNotify = new AMessage(kWhatRTCPNotify, id());
    for (serverRtp = 15550;; serverRtp += 2) {
        int32_t rtpSession;
        err = mNetSession->createUDPSession(
                    serverRtp, clientIP, clientRtp,
                    rtpNotify, &rtpSession);

        if (err != OK) {
            ALOGI("failed to create RTP socket on port %d", serverRtp);
            continue;
        }

        if (clientRtcp < 0) {
            // No RTCP.

            mRTPPort = serverRtp;
            mRTPSessionID = rtpSession;
            mRTCPSessionID = 0;

            ALOGI("rtpSessionId = %d", rtpSession);
            break;
        }

        int32_t rtcpSession;
        err = mNetSession->createUDPSession(
                serverRtp + 1, clientIP, clientRtcp,
                rtcpNotify, &rtcpSession);

        if (err == OK) {
            mRTPPort = serverRtp;
            mRTPSessionID = rtpSession;
            mRTCPSessionID = rtcpSession;

            ALOGI("rtpSessionID = %d, rtcpSessionID = %d", rtpSession, rtcpSession);
            break;
        }

        ALOGI("failed to create RTCP socket on port %d", serverRtp + 1);
        mNetSession->destroySession(rtpSession);
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

    mTracks.clear();

    if (mCodecLooper != NULL) {
        mCodecLooper->stop();
        mCodecLooper.clear();
    }

    mPacketizer.clear();

    if (mSerializer != NULL) {
        mSerializer->stop();

        looper()->unregisterHandler(mSerializer->id());
        mSerializer.clear();
    }

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

    if (mRTCPSessionID != 0) {
        mNetSession->destroySession(mRTCPSessionID);
    }

    if (mRTPSessionID != 0) {
        mNetSession->destroySession(mRTPSessionID);
    }
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

    if (mRTCPSessionID != 0) {
        scheduleSendSR();
    }

    return mSerializer->start();
}

status_t WifiDisplaySource::PlaybackSession::pause() {
    updateLiveness();

    return OK;
}

void WifiDisplaySource::PlaybackSession::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatRTPNotify:
        case kWhatRTCPNotify:
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

                    if (msg->what() == kWhatRTPNotify
                            && !errorOccuredDuringSend) {
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
                    if (msg->what() == kWhatRTCPNotify) {
                        err = parseRTCP(data);
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

                    int64_t timeUs;
                    CHECK(accessUnit->meta()->findInt64("timeUs", &timeUs));

                    if (mPrevTimeUs < 0ll || mPrevTimeUs + 100000ll >= timeUs) {
                        flags |= TSPacketizer::EMIT_PCR;
                        mPrevTimeUs = timeUs;
                    }

                    sp<ABuffer> packets;
                    mPacketizer->packetize(
                            packetizerTrackIndex, accessUnit, &packets, flags);

                    for (size_t offset = 0;
                            offset < packets->size(); offset += 188) {
                        bool lastTSPacket = (offset + 188 >= packets->size());

                        appendTSData(
                                packets->data() + offset,
                                188,
                                true /* timeDiscontinuity */,
                                lastTSPacket /* flush */);
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

        default:
            TRESPASS();
    }
}

status_t WifiDisplaySource::PlaybackSession::setupPacketizer() {
    sp<AMessage> msg = new AMessage(kWhatSerializerNotify, id());

    mSerializerLooper = new ALooper;
    mSerializerLooper->start();

    mSerializer = new Serializer(
#if FAKE_VIDEO
            true /* throttled */
#else
            false /* throttled */
#endif
            , msg);
    mSerializerLooper->registerHandler(mSerializer);

    mPacketizer = new TSPacketizer;

#if FAKE_VIDEO
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
#else
    mCodecLooper = new ALooper;
    mCodecLooper->start();

    sp<SurfaceMediaSource> source = new SurfaceMediaSource(width(), height());

#if 0
    ssize_t index = mSerializer->addSource(source);
#else
    ssize_t index = mSerializer->addSource(
            new RepeaterSource(source, 30.0 /* rateHz */));
#endif

    CHECK_GE(index, 0);

    sp<AMessage> format;
    status_t err = convertMetaDataToMessage(source->getFormat(), &format);
    CHECK_EQ(err, (status_t)OK);

    format->setInt32("store-metadata-in-buffers", true);

    format->setInt32(
            "color-format", OMX_COLOR_FormatAndroidOpaque);

    sp<AMessage> notify = new AMessage(kWhatConverterNotify, id());
    notify->setSize("trackIndex", index);

    sp<Converter> converter =
        new Converter(notify, mCodecLooper, format);
    CHECK_EQ(converter->initCheck(), (status_t)OK);

    size_t numInputBuffers = converter->getInputBufferCount();
    ALOGI("numInputBuffers to the encoder is %d", numInputBuffers);

    looper()->registerHandler(converter);

    mTracks.add(index, new Track(converter));

    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("SurfaceFlinger"));
    sp<ISurfaceComposer> service = interface_cast<ISurfaceComposer>(binder);
    CHECK(service != NULL);

    // Add one reference to account for the serializer.
    // Add another one for unknown reasons.
    err = source->setMaxAcquiredBufferCount(numInputBuffers + 2);
    CHECK_EQ(err, (status_t)OK);

    mBufferQueue = source->getBufferQueue();

    if (mLegacyMode) {
        service->connectDisplay(mBufferQueue);
    }

#if 0
    sp<AudioSource> audioSource = new AudioSource(
            AUDIO_SOURCE_MIC,
            48000 /* sampleRate */,
            2 /* channelCount */);  // XXX AUDIO_CHANNEL_IN_STEREO?

    if (audioSource->initCheck() == OK) {
        audioSource->setUseLooperTime(true);

        index = mSerializer->addSource(audioSource);
        CHECK_GE(index, 0);

        sp<AMessage> audioFormat;
        err = convertMetaDataToMessage(audioSource->getFormat(), &audioFormat);
        CHECK_EQ(err, (status_t)OK);

        sp<AMessage> audioNotify = new AMessage(kWhatConverterNotify, id());
        audioNotify->setSize("trackIndex", index);

        converter = new Converter(audioNotify, mCodecLooper, audioFormat);
        looper()->registerHandler(converter);

        mTracks.add(index, new Track(converter));
    } else {
        ALOGW("Unable to instantiate audio source");
    }
#endif
#endif

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

    if (mUseInterleavedTCP) {
        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatBinaryData);
        notify->setInt32("channel", mRTCPChannel);
        notify->setBuffer("data", buffer);
        notify->post();
    } else {
        mNetSession->sendRequest(
                mRTCPSessionID, buffer->data(), buffer->size());
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

        if (mUseInterleavedTCP) {
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatBinaryData);

            sp<ABuffer> data = new ABuffer(mTSQueue->size());
            memcpy(data->data(), rtp, mTSQueue->size());

            notify->setInt32("channel", mRTPChannel);
            notify->setBuffer("data", data);
            notify->post();
        } else {
            mNetSession->sendRequest(
                    mRTPSessionID, rtp, mTSQueue->size());
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

            case 205:  // TSFB (transport layer specific feedback)
                parseTSFB(data, headerLength);
                break;

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
        bool found = false;
        while (it != mHistory.end()) {
            const sp<ABuffer> &buffer = *it;

            uint16_t bufferSeqNo = buffer->int32Data() & 0xffff;

            if (bufferSeqNo == seqNo) {
                mNetSession->sendRequest(
                        mRTPSessionID, buffer->data(), buffer->size());

                found = true;
                break;
            }

            ++it;
        }

        if (found) {
            ALOGI("retransmitting seqNo %d", seqNo);
        } else {
            ALOGI("seqNo %d no longer available", seqNo);
        }
    }

    return OK;
}

}  // namespace android

