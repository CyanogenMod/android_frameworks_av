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
#define LOG_TAG "WifiDisplaySource"
#include <utils/Log.h>

#include "WifiDisplaySource.h"
#include "PlaybackSession.h"
#include "Parameters.h"
#include "ParsedMessage.h"

#include <binder/IServiceManager.h>
#include <gui/ISurfaceTexture.h>
#include <media/IHDCP.h>
#include <media/IMediaPlayerService.h>
#include <media/IRemoteDisplayClient.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>

#include <arpa/inet.h>
#include <cutils/properties.h>

namespace android {

WifiDisplaySource::WifiDisplaySource(
        const sp<ANetworkSession> &netSession,
        const sp<IRemoteDisplayClient> &client)
    : mNetSession(netSession),
      mClient(client),
      mSessionID(0),
      mStopReplyID(0),
      mClientSessionID(0),
      mReaperPending(false),
      mNextCSeq(1)
#if REQUIRE_HDCP
      ,mIsHDCP2_0(false)
      ,mHDCPPort(0)
      ,mHDCPInitializationComplete(false)
      ,mSetupTriggerDeferred(false)
#endif
{
}

WifiDisplaySource::~WifiDisplaySource() {
}

status_t WifiDisplaySource::start(const char *iface) {
    sp<AMessage> msg = new AMessage(kWhatStart, id());
    msg->setString("iface", iface);

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err != OK) {
        return err;
    }

    if (!response->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

status_t WifiDisplaySource::stop() {
    sp<AMessage> msg = new AMessage(kWhatStop, id());

    sp<AMessage> response;
    status_t err = msg->postAndAwaitResponse(&response);

    if (err != OK) {
        return err;
    }

    if (!response->findInt32("err", &err)) {
        err = OK;
    }

    return err;
}

void WifiDisplaySource::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatStart:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            AString iface;
            CHECK(msg->findString("iface", &iface));

            status_t err = OK;

            ssize_t colonPos = iface.find(":");

            unsigned long port;

            if (colonPos >= 0) {
                const char *s = iface.c_str() + colonPos + 1;

                char *end;
                port = strtoul(s, &end, 10);

                if (end == s || *end != '\0' || port > 65535) {
                    err = -EINVAL;
                } else {
                    iface.erase(colonPos, iface.size() - colonPos);
                }
            } else {
                port = kWifiDisplayDefaultPort;
            }

            if (err == OK) {
                if (inet_aton(iface.c_str(), &mInterfaceAddr) != 0) {
                    sp<AMessage> notify = new AMessage(kWhatRTSPNotify, id());

                    err = mNetSession->createRTSPServer(
                            mInterfaceAddr, port, notify, &mSessionID);
                } else {
                    err = -EINVAL;
                }
            }

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);
            response->postReply(replyID);
            break;
        }

        case kWhatRTSPNotify:
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

                    AString detail;
                    CHECK(msg->findString("detail", &detail));

                    ALOGE("An error occurred in session %d (%d, '%s/%s').",
                          sessionID,
                          err,
                          detail.c_str(),
                          strerror(-err));

                    mNetSession->destroySession(sessionID);

                    if (sessionID == mClientSessionID) {
                        mClientSessionID = -1;

                        disconnectClient(UNKNOWN_ERROR);
                    }
                    break;
                }

                case ANetworkSession::kWhatClientConnected:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    if (mClientSessionID > 0) {
                        ALOGW("A client tried to connect, but we already "
                              "have one.");

                        mNetSession->destroySession(sessionID);
                        break;
                    }

                    CHECK(msg->findString("client-ip", &mClientInfo.mRemoteIP));
                    CHECK(msg->findString("server-ip", &mClientInfo.mLocalIP));

                    if (mClientInfo.mRemoteIP == mClientInfo.mLocalIP) {
                        // Disallow connections from the local interface
                        // for security reasons.
                        mNetSession->destroySession(sessionID);
                        break;
                    }

                    CHECK(msg->findInt32(
                                "server-port", &mClientInfo.mLocalPort));
                    mClientInfo.mPlaybackSessionID = -1;

                    mClientSessionID = sessionID;

                    ALOGI("We now have a client (%d) connected.", sessionID);

                    status_t err = sendM1(sessionID);
                    CHECK_EQ(err, (status_t)OK);
                    break;
                }

                case ANetworkSession::kWhatData:
                {
                    status_t err = onReceiveClientData(msg);

                    if (err != OK) {
                        disconnectClient(err);
                    }
                    break;
                }

                default:
                    TRESPASS();
            }
            break;
        }

        case kWhatStop:
        {
            uint32_t replyID;
            CHECK(msg->senderAwaitsResponse(&replyID));

            if (mSessionID != 0 && mClientSessionID != 0) {
                status_t err = sendM5(
                        mClientSessionID, true /* requestShutdown */);

                if (err == OK) {
                    mStopReplyID = replyID;
                    break;
                }
            }

            finishStop(replyID);
            break;
        }

        case kWhatReapDeadClients:
        {
            mReaperPending = false;

            if (mClientSessionID == 0
                    || mClientInfo.mPlaybackSession == NULL) {
                break;
            }

            if (mClientInfo.mPlaybackSession->getLastLifesignUs()
                    + kPlaybackSessionTimeoutUs < ALooper::GetNowUs()) {
                ALOGI("playback session timed out, reaping.");

                disconnectClient(-ETIMEDOUT);
            } else {
                scheduleReaper();
            }
            break;
        }

        case kWhatPlaybackSessionNotify:
        {
            int32_t playbackSessionID;
            CHECK(msg->findInt32("playbackSessionID", &playbackSessionID));

            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == PlaybackSession::kWhatSessionDead) {
                ALOGI("playback session wants to quit.");

                disconnectClient(UNKNOWN_ERROR);
            } else if (what == PlaybackSession::kWhatSessionEstablished) {
                if (mClient != NULL) {
                    mClient->onDisplayConnected(
                            mClientInfo.mPlaybackSession->getSurfaceTexture(),
                            mClientInfo.mPlaybackSession->width(),
                            mClientInfo.mPlaybackSession->height(),
                            0 /* flags */);
                }
            } else {
                CHECK_EQ(what, PlaybackSession::kWhatBinaryData);

                int32_t channel;
                CHECK(msg->findInt32("channel", &channel));

                sp<ABuffer> data;
                CHECK(msg->findBuffer("data", &data));

                CHECK_LE(channel, 0xffu);
                CHECK_LE(data->size(), 0xffffu);

                int32_t sessionID;
                CHECK(msg->findInt32("sessionID", &sessionID));

                char header[4];
                header[0] = '$';
                header[1] = channel;
                header[2] = data->size() >> 8;
                header[3] = data->size() & 0xff;

                mNetSession->sendRequest(
                        sessionID, header, sizeof(header));

                mNetSession->sendRequest(
                        sessionID, data->data(), data->size());
            }
            break;
        }

        case kWhatKeepAlive:
        {
            int32_t sessionID;
            CHECK(msg->findInt32("sessionID", &sessionID));

            if (mClientSessionID != sessionID) {
                // Obsolete event, client is already gone.
                break;
            }

            sendM16(sessionID);
            break;
        }

#if REQUIRE_HDCP
        case kWhatHDCPNotify:
        {
            int32_t msgCode, ext1, ext2;
            CHECK(msg->findInt32("msg", &msgCode));
            CHECK(msg->findInt32("ext1", &ext1));
            CHECK(msg->findInt32("ext2", &ext2));

            ALOGV("Saw HDCP notification code %d, ext1 %d, ext2 %d",
                    msgCode, ext1, ext2);

            switch (msgCode) {
                case HDCPModule::HDCP_INITIALIZATION_COMPLETE:
                {
                    mHDCPInitializationComplete = true;

                    if (mSetupTriggerDeferred) {
                        mSetupTriggerDeferred = false;

                        sendM5(mClientSessionID, false /* requestShutdown */);
                    }
                    break;
                }

                default:
                {
                    ALOGE("HDCP failure, shutting down.");

                    disconnectClient(-EACCES);
                    break;
                }
            }
            break;
        }
#endif

        default:
            TRESPASS();
    }
}

void WifiDisplaySource::registerResponseHandler(
        int32_t sessionID, int32_t cseq, HandleRTSPResponseFunc func) {
    ResponseID id;
    id.mSessionID = sessionID;
    id.mCSeq = cseq;
    mResponseHandlers.add(id, func);
}

status_t WifiDisplaySource::sendM1(int32_t sessionID) {
    AString request = "OPTIONS * RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append(
            "Require: org.wfa.wfd1.0\r\n"
            "\r\n");

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySource::onReceiveM1Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySource::sendM3(int32_t sessionID) {
    AString body =
#if REQUIRE_HDCP
        "wfd_content_protection\r\n"
#endif
        "wfd_video_formats\r\n"
        "wfd_audio_codecs\r\n"
        "wfd_client_rtp_ports\r\n";

    AString request = "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append("Content-Type: text/parameters\r\n");
    request.append(StringPrintf("Content-Length: %d\r\n", body.size()));
    request.append("\r\n");
    request.append(body);

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySource::onReceiveM3Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySource::sendM4(int32_t sessionID) {
    // wfd_video_formats:
    // 1 byte "native"
    // 1 byte "preferred-display-mode-supported" 0 or 1
    // one or more avc codec structures
    //   1 byte profile
    //   1 byte level
    //   4 byte CEA mask
    //   4 byte VESA mask
    //   4 byte HH mask
    //   1 byte latency
    //   2 byte min-slice-slice
    //   2 byte slice-enc-params
    //   1 byte framerate-control-support
    //   max-hres (none or 2 byte)
    //   max-vres (none or 2 byte)

    CHECK_EQ(sessionID, mClientSessionID);

    AString transportString = "UDP";

    char val[PROPERTY_VALUE_MAX];
    if (property_get("media.wfd.enable-tcp", val, NULL)
            && (!strcasecmp("true", val) || !strcmp("1", val))) {
        ALOGI("Using TCP transport.");
        transportString = "TCP";
    }

    // For 720p60:
    //   use "30 00 02 02 00000040 00000000 00000000 00 0000 0000 00 none none\r\n"
    // For 720p30:
    //   use "28 00 02 02 00000020 00000000 00000000 00 0000 0000 00 none none\r\n"
    AString body = StringPrintf(
        "wfd_video_formats: "
        "28 00 02 02 00000020 00000000 00000000 00 0000 0000 00 none none\r\n"
        "wfd_audio_codecs: AAC 00000001 00\r\n"  // 2 ch AAC 48kHz
        "wfd_presentation_URL: rtsp://%s:%d/wfd1.0/streamid=0 none\r\n"
        "wfd_client_rtp_ports: RTP/AVP/%s;unicast 19000 0 mode=play\r\n",
        mClientInfo.mLocalIP.c_str(), mClientInfo.mLocalPort,
        transportString.c_str());

    AString request = "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append("Content-Type: text/parameters\r\n");
    request.append(StringPrintf("Content-Length: %d\r\n", body.size()));
    request.append("\r\n");
    request.append(body);

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySource::onReceiveM4Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySource::sendM5(int32_t sessionID, bool requestShutdown) {
    AString body = "wfd_trigger_method: ";
    if (requestShutdown) {
        body.append("TEARDOWN");
    } else {
        body.append("SETUP");
    }

    body.append("\r\n");

    AString request = "SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    request.append("Content-Type: text/parameters\r\n");
    request.append(StringPrintf("Content-Length: %d\r\n", body.size()));
    request.append("\r\n");
    request.append(body);

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySource::onReceiveM5Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySource::sendM16(int32_t sessionID) {
    AString request = "GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\n";
    AppendCommonResponse(&request, mNextCSeq);

    CHECK_EQ(sessionID, mClientSessionID);
    request.append(
            StringPrintf("Session: %d\r\n", mClientInfo.mPlaybackSessionID));
    request.append("\r\n");  // Empty body

    status_t err =
        mNetSession->sendRequest(sessionID, request.c_str(), request.size());

    if (err != OK) {
        return err;
    }

    registerResponseHandler(
            sessionID, mNextCSeq, &WifiDisplaySource::onReceiveM16Response);

    ++mNextCSeq;

    return OK;
}

status_t WifiDisplaySource::onReceiveM1Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    return OK;
}

status_t WifiDisplaySource::onReceiveM3Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    sp<Parameters> params =
        Parameters::Parse(msg->getContent(), strlen(msg->getContent()));

    if (params == NULL) {
        return ERROR_MALFORMED;
    }

#if REQUIRE_HDCP
    AString value;
    if (!params->findParameter("wfd_content_protection", &value)) {
        ALOGE("Sink doesn't appear to support content protection.");
        return -EACCES;
    }

    if (value == "none") {
        ALOGE("Sink does not support content protection.");
        return -EACCES;
    }

    bool isHDCP2_0 = false;
    if (value.startsWith("HDCP2.0 ")) {
        isHDCP2_0 = true;
    } else if (!value.startsWith("HDCP2.1 ")) {
        return ERROR_MALFORMED;
    }

    int32_t hdcpPort;
    if (!ParsedMessage::GetInt32Attribute(value.c_str() + 8, "port", &hdcpPort)
            || hdcpPort < 1 || hdcpPort > 65535) {
        return ERROR_MALFORMED;
    }

    mIsHDCP2_0 = isHDCP2_0;
    mHDCPPort = hdcpPort;

    status_t err = makeHDCP();
    if (err != OK) {
        ALOGE("Unable to instantiate HDCP component.");
        return err;
    }
#endif

    return sendM4(sessionID);
}

status_t WifiDisplaySource::onReceiveM4Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

#if REQUIRE_HDCP
    if (!mHDCPInitializationComplete) {
        ALOGI("Deferring SETUP trigger until HDCP initialization completes.");

        mSetupTriggerDeferred = true;
        return OK;
    }
#endif

    return sendM5(sessionID, false /* requestShutdown */);
}

status_t WifiDisplaySource::onReceiveM5Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    int32_t statusCode;
    if (!msg->getStatusCode(&statusCode)) {
        return ERROR_MALFORMED;
    }

    if (statusCode != 200) {
        return ERROR_UNSUPPORTED;
    }

    return OK;
}

status_t WifiDisplaySource::onReceiveM16Response(
        int32_t sessionID, const sp<ParsedMessage> &msg) {
    // If only the response was required to include a "Session:" header...

    CHECK_EQ(sessionID, mClientSessionID);

    if (mClientInfo.mPlaybackSession != NULL) {
        mClientInfo.mPlaybackSession->updateLiveness();

        scheduleKeepAlive(sessionID);
    }

    return OK;
}

void WifiDisplaySource::scheduleReaper() {
    if (mReaperPending) {
        return;
    }

    mReaperPending = true;
    (new AMessage(kWhatReapDeadClients, id()))->post(kReaperIntervalUs);
}

void WifiDisplaySource::scheduleKeepAlive(int32_t sessionID) {
    // We need to send updates at least 5 secs before the timeout is set to
    // expire, make sure the timeout is greater than 5 secs to begin with.
    CHECK_GT(kPlaybackSessionTimeoutUs, 5000000ll);

    sp<AMessage> msg = new AMessage(kWhatKeepAlive, id());
    msg->setInt32("sessionID", sessionID);
    msg->post(kPlaybackSessionTimeoutUs - 5000000ll);
}

status_t WifiDisplaySource::onReceiveClientData(const sp<AMessage> &msg) {
    int32_t sessionID;
    CHECK(msg->findInt32("sessionID", &sessionID));

    sp<RefBase> obj;
    CHECK(msg->findObject("data", &obj));

    sp<ParsedMessage> data =
        static_cast<ParsedMessage *>(obj.get());

    ALOGV("session %d received '%s'",
          sessionID, data->debugString().c_str());

    AString method;
    AString uri;
    data->getRequestField(0, &method);

    int32_t cseq;
    if (!data->findInt32("cseq", &cseq)) {
        sendErrorResponse(sessionID, "400 Bad Request", -1 /* cseq */);
        return ERROR_MALFORMED;
    }

    if (method.startsWith("RTSP/")) {
        // This is a response.

        ResponseID id;
        id.mSessionID = sessionID;
        id.mCSeq = cseq;

        ssize_t index = mResponseHandlers.indexOfKey(id);

        if (index < 0) {
            ALOGW("Received unsolicited server response, cseq %d", cseq);
            return ERROR_MALFORMED;
        }

        HandleRTSPResponseFunc func = mResponseHandlers.valueAt(index);
        mResponseHandlers.removeItemsAt(index);

        status_t err = (this->*func)(sessionID, data);

        if (err != OK) {
            ALOGW("Response handler for session %d, cseq %d returned "
                  "err %d (%s)",
                  sessionID, cseq, err, strerror(-err));

            return err;
        }

        return OK;
    }

    AString version;
    data->getRequestField(2, &version);
    if (!(version == AString("RTSP/1.0"))) {
        sendErrorResponse(sessionID, "505 RTSP Version not supported", cseq);
        return ERROR_UNSUPPORTED;
    }

    status_t err;
    if (method == "OPTIONS") {
        err = onOptionsRequest(sessionID, cseq, data);
    } else if (method == "SETUP") {
        err = onSetupRequest(sessionID, cseq, data);
    } else if (method == "PLAY") {
        err = onPlayRequest(sessionID, cseq, data);
    } else if (method == "PAUSE") {
        err = onPauseRequest(sessionID, cseq, data);
    } else if (method == "TEARDOWN") {
        err = onTeardownRequest(sessionID, cseq, data);
    } else if (method == "GET_PARAMETER") {
        err = onGetParameterRequest(sessionID, cseq, data);
    } else if (method == "SET_PARAMETER") {
        err = onSetParameterRequest(sessionID, cseq, data);
    } else {
        sendErrorResponse(sessionID, "405 Method Not Allowed", cseq);

        err = ERROR_UNSUPPORTED;
    }

    return err;
}

status_t WifiDisplaySource::onOptionsRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession != NULL) {
        playbackSession->updateLiveness();
    }

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);

    response.append(
            "Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, "
            "GET_PARAMETER, SET_PARAMETER\r\n");

    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());

    if (err == OK) {
        err = sendM3(sessionID);
    }

    return err;
}

status_t WifiDisplaySource::onSetupRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    CHECK_EQ(sessionID, mClientSessionID);
    if (mClientInfo.mPlaybackSessionID != -1) {
        // We only support a single playback session per client.
        // This is due to the reversed keep-alive design in the wfd specs...
        sendErrorResponse(sessionID, "400 Bad Request", cseq);
        return ERROR_MALFORMED;
    }

    AString transport;
    if (!data->findString("transport", &transport)) {
        sendErrorResponse(sessionID, "400 Bad Request", cseq);
        return ERROR_MALFORMED;
    }

    PlaybackSession::TransportMode transportMode =
        PlaybackSession::TRANSPORT_UDP;

    int clientRtp, clientRtcp;
    if (transport.startsWith("RTP/AVP/TCP;")) {
        AString interleaved;
        if (ParsedMessage::GetAttribute(
                    transport.c_str(), "interleaved", &interleaved)
                && sscanf(interleaved.c_str(), "%d-%d",
                          &clientRtp, &clientRtcp) == 2) {
            transportMode = PlaybackSession::TRANSPORT_TCP_INTERLEAVED;
        } else {
            bool badRequest = false;

            AString clientPort;
            if (!ParsedMessage::GetAttribute(
                        transport.c_str(), "client_port", &clientPort)) {
                badRequest = true;
            } else if (sscanf(clientPort.c_str(), "%d-%d",
                              &clientRtp, &clientRtcp) == 2) {
            } else if (sscanf(clientPort.c_str(), "%d", &clientRtp) == 1) {
                // No RTCP.
                clientRtcp = -1;
            } else {
                badRequest = true;
            }

            if (badRequest) {
                sendErrorResponse(sessionID, "400 Bad Request", cseq);
                return ERROR_MALFORMED;
            }

            transportMode = PlaybackSession::TRANSPORT_TCP;
        }
    } else if (transport.startsWith("RTP/AVP;unicast;")
            || transport.startsWith("RTP/AVP/UDP;unicast;")) {
        bool badRequest = false;

        AString clientPort;
        if (!ParsedMessage::GetAttribute(
                    transport.c_str(), "client_port", &clientPort)) {
            badRequest = true;
        } else if (sscanf(clientPort.c_str(), "%d-%d",
                          &clientRtp, &clientRtcp) == 2) {
        } else if (sscanf(clientPort.c_str(), "%d", &clientRtp) == 1) {
            // No RTCP.
            clientRtcp = -1;
            clientRtcp = clientRtp + 1;  // XXX
        } else {
            badRequest = true;
        }

        if (badRequest) {
            sendErrorResponse(sessionID, "400 Bad Request", cseq);
            return ERROR_MALFORMED;
        }
#if 1
    // The older LG dongles doesn't specify client_port=xxx apparently.
    } else if (transport == "RTP/AVP/UDP;unicast") {
        clientRtp = 19000;
        clientRtcp = clientRtp + 1;
#endif
    } else {
        sendErrorResponse(sessionID, "461 Unsupported Transport", cseq);
        return ERROR_UNSUPPORTED;
    }

    int32_t playbackSessionID = makeUniquePlaybackSessionID();

    sp<AMessage> notify = new AMessage(kWhatPlaybackSessionNotify, id());
    notify->setInt32("playbackSessionID", playbackSessionID);
    notify->setInt32("sessionID", sessionID);

    sp<PlaybackSession> playbackSession =
        new PlaybackSession(
                mNetSession, notify, mInterfaceAddr,
#if REQUIRE_HDCP
                mHDCP
#else
                NULL
#endif
                );

    looper()->registerHandler(playbackSession);

    AString uri;
    data->getRequestField(1, &uri);

    if (strncasecmp("rtsp://", uri.c_str(), 7)) {
        sendErrorResponse(sessionID, "400 Bad Request", cseq);
        return ERROR_MALFORMED;
    }

    if (!(uri.startsWith("rtsp://") && uri.endsWith("/wfd1.0/streamid=0"))) {
        sendErrorResponse(sessionID, "404 Not found", cseq);
        return ERROR_MALFORMED;
    }

    status_t err = playbackSession->init(
            mClientInfo.mRemoteIP.c_str(),
            clientRtp,
            clientRtcp,
            transportMode);

    if (err != OK) {
        looper()->unregisterHandler(playbackSession->id());
        playbackSession.clear();
    }

    switch (err) {
        case OK:
            break;
        case -ENOENT:
            sendErrorResponse(sessionID, "404 Not Found", cseq);
            return err;
        default:
            sendErrorResponse(sessionID, "403 Forbidden", cseq);
            return err;
    }

    mClientInfo.mPlaybackSessionID = playbackSessionID;
    mClientInfo.mPlaybackSession = playbackSession;

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);

    if (transportMode == PlaybackSession::TRANSPORT_TCP_INTERLEAVED) {
        response.append(
                StringPrintf(
                    "Transport: RTP/AVP/TCP;interleaved=%d-%d;",
                    clientRtp, clientRtcp));
    } else {
        int32_t serverRtp = playbackSession->getRTPPort();

        AString transportString = "UDP";
        if (transportMode == PlaybackSession::TRANSPORT_TCP) {
            transportString = "TCP";
        }

        if (clientRtcp >= 0) {
            response.append(
                    StringPrintf(
                        "Transport: RTP/AVP/%s;unicast;client_port=%d-%d;"
                        "server_port=%d-%d\r\n",
                        transportString.c_str(),
                        clientRtp, clientRtcp, serverRtp, serverRtp + 1));
        } else {
            response.append(
                    StringPrintf(
                        "Transport: RTP/AVP/%s;unicast;client_port=%d;"
                        "server_port=%d\r\n",
                        transportString.c_str(),
                        clientRtp, serverRtp));
        }
    }

    response.append("\r\n");

    err = mNetSession->sendRequest(sessionID, response.c_str());

    if (err != OK) {
        return err;
    }

    scheduleReaper();
    scheduleKeepAlive(sessionID);

    return OK;
}

status_t WifiDisplaySource::onPlayRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return ERROR_MALFORMED;
    }

    status_t err = playbackSession->play();
    CHECK_EQ(err, (status_t)OK);

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("Range: npt=now-\r\n");
    response.append("\r\n");

    err = mNetSession->sendRequest(sessionID, response.c_str());

    if (err != OK) {
        return err;
    }

    playbackSession->finishPlay();

    return OK;
}

status_t WifiDisplaySource::onPauseRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return ERROR_MALFORMED;
    }

    status_t err = playbackSession->pause();
    CHECK_EQ(err, (status_t)OK);

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    err = mNetSession->sendRequest(sessionID, response.c_str());

    return err;
}

status_t WifiDisplaySource::onTeardownRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return ERROR_MALFORMED;
    }

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("Connection: close\r\n");
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());

    if (err != OK) {
        return err;
    }

    if (mStopReplyID != 0) {
        finishStop(mStopReplyID);
        mStopReplyID = 0;
    } else {
        disconnectClient(UNKNOWN_ERROR);
    }

    return OK;
}

void WifiDisplaySource::finishStop(uint32_t replyID) {
    disconnectClient(OK);

#if REQUIRE_HDCP
    if (mHDCP != NULL) {
        mHDCP->shutdownAsync();
        mHDCP.clear();
    }
#endif

    status_t err = OK;

    sp<AMessage> response = new AMessage;
    response->setInt32("err", err);
    response->postReply(replyID);
}

status_t WifiDisplaySource::onGetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return ERROR_MALFORMED;
    }

    playbackSession->updateLiveness();

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    return err;
}

status_t WifiDisplaySource::onSetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return ERROR_MALFORMED;
    }

    if (strstr(data->getContent(), "wfd_idr_request\r\n")) {
        playbackSession->requestIDRFrame();
    }

    playbackSession->updateLiveness();

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    return err;
}

// static
void WifiDisplaySource::AppendCommonResponse(
        AString *response, int32_t cseq, int32_t playbackSessionID) {
    time_t now = time(NULL);
    struct tm *now2 = gmtime(&now);
    char buf[128];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", now2);

    response->append("Date: ");
    response->append(buf);
    response->append("\r\n");

    response->append("Server: Mine/1.0\r\n");

    if (cseq >= 0) {
        response->append(StringPrintf("CSeq: %d\r\n", cseq));
    }

    if (playbackSessionID >= 0ll) {
        response->append(
                StringPrintf(
                    "Session: %d;timeout=%lld\r\n",
                    playbackSessionID, kPlaybackSessionTimeoutSecs));
    }
}

void WifiDisplaySource::sendErrorResponse(
        int32_t sessionID,
        const char *errorDetail,
        int32_t cseq) {
    AString response;
    response.append("RTSP/1.0 ");
    response.append(errorDetail);
    response.append("\r\n");

    AppendCommonResponse(&response, cseq);

    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

int32_t WifiDisplaySource::makeUniquePlaybackSessionID() const {
    return rand();
}

sp<WifiDisplaySource::PlaybackSession> WifiDisplaySource::findPlaybackSession(
        const sp<ParsedMessage> &data, int32_t *playbackSessionID) const {
    if (!data->findInt32("session", playbackSessionID)) {
        // XXX the older dongles do not always include a "Session:" header.
        *playbackSessionID = mClientInfo.mPlaybackSessionID;
        return mClientInfo.mPlaybackSession;
    }

    if (*playbackSessionID != mClientInfo.mPlaybackSessionID) {
        return NULL;
    }

    return mClientInfo.mPlaybackSession;
}

void WifiDisplaySource::disconnectClient(status_t err) {
    if (mClientSessionID != 0) {
        if (mClientInfo.mPlaybackSession != NULL) {
            looper()->unregisterHandler(mClientInfo.mPlaybackSession->id());
            mClientInfo.mPlaybackSession.clear();
        }

        mNetSession->destroySession(mClientSessionID);
        mClientSessionID = 0;
    }

    if (mClient != NULL) {
        if (err != OK) {
            mClient->onDisplayError(IRemoteDisplayClient::kDisplayErrorUnknown);
        } else {
            mClient->onDisplayDisconnected();
        }
    }
}

#if REQUIRE_HDCP
struct WifiDisplaySource::HDCPObserver : public BnHDCPObserver {
    HDCPObserver(const sp<AMessage> &notify);

    virtual void notify(
            int msg, int ext1, int ext2, const Parcel *obj);

private:
    sp<AMessage> mNotify;

    DISALLOW_EVIL_CONSTRUCTORS(HDCPObserver);
};

WifiDisplaySource::HDCPObserver::HDCPObserver(
        const sp<AMessage> &notify)
    : mNotify(notify) {
}

void WifiDisplaySource::HDCPObserver::notify(
        int msg, int ext1, int ext2, const Parcel *obj) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("msg", msg);
    notify->setInt32("ext1", ext1);
    notify->setInt32("ext2", ext2);
    notify->post();
}

status_t WifiDisplaySource::makeHDCP() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    CHECK(service != NULL);

    mHDCP = service->makeHDCP();

    if (mHDCP == NULL) {
        return ERROR_UNSUPPORTED;
    }

    sp<AMessage> notify = new AMessage(kWhatHDCPNotify, id());
    mHDCPObserver = new HDCPObserver(notify);

    status_t err = mHDCP->setObserver(mHDCPObserver);

    if (err != OK) {
        ALOGE("Failed to set HDCP observer.");

        mHDCPObserver.clear();
        mHDCP.clear();

        return err;
    }

    ALOGI("initiating HDCP negotiation w/ host %s:%d",
            mClientInfo.mRemoteIP.c_str(), mHDCPPort);

    err = mHDCP->initAsync(mClientInfo.mRemoteIP.c_str(), mHDCPPort);

    if (err != OK) {
        return err;
    }

    return OK;
}
#endif

}  // namespace android

