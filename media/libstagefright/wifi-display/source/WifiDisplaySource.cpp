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

#define LOG_NDEBUG 0
#define LOG_TAG "WifiDisplaySource"
#include <utils/Log.h>

#include "WifiDisplaySource.h"
#include "PlaybackSession.h"
#include "ParsedMessage.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace android {

WifiDisplaySource::WifiDisplaySource(const sp<ANetworkSession> &netSession)
    : mNetSession(netSession),
      mSessionID(0),
      mReaperPending(false),
      mNextCSeq(1) {
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

            struct in_addr addr;

            if (err == OK) {
                if (inet_aton(iface.c_str(), &addr) != 0) {
                    sp<AMessage> notify = new AMessage(kWhatRTSPNotify, id());

                    err = mNetSession->createRTSPServer(
                            addr, port, notify, &mSessionID);

                    ALOGI("createRTSPServer returned err %d", err);
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

                    mClientIPs.removeItem(sessionID);
                    break;
                }

                case ANetworkSession::kWhatClientConnected:
                {
                    int32_t sessionID;
                    CHECK(msg->findInt32("sessionID", &sessionID));

                    ClientInfo info;
                    CHECK(msg->findString("client-ip", &info.mRemoteIP));
                    CHECK(msg->findString("server-ip", &info.mLocalIP));
                    CHECK(msg->findInt32("server-port", &info.mLocalPort));

                    ALOGI("We now have a client (%d) connected.", sessionID);

                    mClientIPs.add(sessionID, info);

                    status_t err = sendM1(sessionID);
                    CHECK_EQ(err, (status_t)OK);
                    break;
                }

                case ANetworkSession::kWhatData:
                {
                    onReceiveClientData(msg);
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

            for (size_t i = mPlaybackSessions.size(); i-- > 0;) {
                const sp<PlaybackSession> &playbackSession =
                    mPlaybackSessions.valueAt(i);

                looper()->unregisterHandler(playbackSession->id());
                mPlaybackSessions.removeItemsAt(i);
            }

            status_t err = OK;

            sp<AMessage> response = new AMessage;
            response->setInt32("err", err);
            response->postReply(replyID);
            break;
        }

        case kWhatReapDeadClients:
        {
            mReaperPending = false;

            for (size_t i = mPlaybackSessions.size(); i-- > 0;) {
                const sp<PlaybackSession> &playbackSession =
                    mPlaybackSessions.valueAt(i);

                if (playbackSession->getLastLifesignUs()
                        + kPlaybackSessionTimeoutUs < ALooper::GetNowUs()) {
                    ALOGI("playback session %d timed out, reaping.",
                            mPlaybackSessions.keyAt(i));

                    looper()->unregisterHandler(playbackSession->id());
                    mPlaybackSessions.removeItemsAt(i);
                }
            }

            if (!mPlaybackSessions.isEmpty()) {
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

            ssize_t index = mPlaybackSessions.indexOfKey(playbackSessionID);
            if (index >= 0) {
                const sp<PlaybackSession> &playbackSession =
                    mPlaybackSessions.valueAt(index);

                if (what == PlaybackSession::kWhatSessionDead) {
                    ALOGI("playback sessions %d wants to quit.",
                          playbackSessionID);

                    looper()->unregisterHandler(playbackSession->id());
                    mPlaybackSessions.removeItemsAt(index);
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
            }
            break;
        }

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

    const ClientInfo &info = mClientIPs.valueFor(sessionID);

    AString body = StringPrintf(
        "wfd_video_formats: "
        "30 00 02 02 00000040 00000000 00000000 00 0000 0000 00 none none\r\n"
        "wfd_audio_codecs: AAC 00000001 00\r\n"  // 2 ch AAC 48kHz
        "wfd_presentation_URL: rtsp://%s:%d/wfd1.0/streamid=0 none\r\n"
        "wfd_client_rtp_ports: RTP/AVP/UDP;unicast 19000 0 mode=play\r\n",
        info.mLocalIP.c_str(), info.mLocalPort);

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

status_t WifiDisplaySource::sendM5(int32_t sessionID) {
    AString body = "wfd_trigger_method: SETUP\r\n";

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

    return sendM5(sessionID);
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

void WifiDisplaySource::scheduleReaper() {
    if (mReaperPending) {
        return;
    }

    mReaperPending = true;
    (new AMessage(kWhatReapDeadClients, id()))->post(kReaperIntervalUs);
}

void WifiDisplaySource::onReceiveClientData(const sp<AMessage> &msg) {
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
        return;
    }

    if (method.startsWith("RTSP/")) {
        // This is a response.

        ResponseID id;
        id.mSessionID = sessionID;
        id.mCSeq = cseq;

        ssize_t index = mResponseHandlers.indexOfKey(id);

        if (index < 0) {
            ALOGW("Received unsolicited server response, cseq %d", cseq);
            return;
        }

        HandleRTSPResponseFunc func = mResponseHandlers.valueAt(index);
        mResponseHandlers.removeItemsAt(index);

        status_t err = (this->*func)(sessionID, data);

        if (err != OK) {
            ALOGW("Response handler for session %d, cseq %d returned "
                  "err %d (%s)",
                  sessionID, cseq, err, strerror(-err));
        }
    } else {
        AString version;
        data->getRequestField(2, &version);
        if (!(version == AString("RTSP/1.0"))) {
            sendErrorResponse(sessionID, "505 RTSP Version not supported", cseq);
            return;
        }

        if (method == "DESCRIBE") {
            onDescribeRequest(sessionID, cseq, data);
        } else if (method == "OPTIONS") {
            onOptionsRequest(sessionID, cseq, data);
        } else if (method == "SETUP") {
            onSetupRequest(sessionID, cseq, data);
        } else if (method == "PLAY") {
            onPlayRequest(sessionID, cseq, data);
        } else if (method == "PAUSE") {
            onPauseRequest(sessionID, cseq, data);
        } else if (method == "TEARDOWN") {
            onTeardownRequest(sessionID, cseq, data);
        } else if (method == "GET_PARAMETER") {
            onGetParameterRequest(sessionID, cseq, data);
        } else if (method == "SET_PARAMETER") {
            onSetParameterRequest(sessionID, cseq, data);
        } else {
            sendErrorResponse(sessionID, "405 Method Not Allowed", cseq);
        }
    }
}

void WifiDisplaySource::onDescribeRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int64_t nowUs = ALooper::GetNowUs();

    AString sdp;
    sdp.append("v=0\r\n");

    sdp.append(StringPrintf(
                "o=- %lld %lld IN IP4 0.0.0.0\r\n", nowUs, nowUs));

    sdp.append(
            "o=- 0 0 IN IP4 127.0.0.0\r\n"
            "s=Sample\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "b=AS:502\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "a=range:npt=now-\r\n"
            "m=video 0 RTP/AVP 33\r\n"
            "a=rtpmap:33 MP2T/90000\r\n"
            "a=control:\r\n");

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq);

    response.append("Content-Type: application/sdp\r\n");

    // response.append("Content-Base: rtsp://0.0.0.0:7236\r\n");
    response.append(StringPrintf("Content-Length: %d\r\n", sdp.size()));
    response.append("\r\n");
    response.append(sdp);

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySource::onOptionsRequest(
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
            "Public: org.wfa.wfd1.0, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, "
            "GET_PARAMETER, SET_PARAMETER\r\n");

    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);

    err = sendM3(sessionID);
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySource::onSetupRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    AString transport;
    if (!data->findString("transport", &transport)) {
        sendErrorResponse(sessionID, "400 Bad Request", cseq);
        return;
    }

    bool useInterleavedTCP = false;

    int clientRtp, clientRtcp;
    if (transport.startsWith("RTP/AVP/TCP;")) {
        AString interleaved;
        if (!ParsedMessage::GetAttribute(
                    transport.c_str(), "interleaved", &interleaved)
                || sscanf(interleaved.c_str(), "%d-%d",
                          &clientRtp, &clientRtcp) != 2) {
            sendErrorResponse(sessionID, "400 Bad Request", cseq);
            return;
        }

        useInterleavedTCP = true;
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
        } else {
            badRequest = true;
        }

        if (badRequest) {
            sendErrorResponse(sessionID, "400 Bad Request", cseq);
            return;
        }
#if 1
    // The LG dongle doesn't specify client_port=xxx apparently.
    } else if (transport == "RTP/AVP/UDP;unicast") {
        clientRtp = 19000;
        clientRtcp = clientRtp + 1;
#endif
    } else {
        sendErrorResponse(sessionID, "461 Unsupported Transport", cseq);
        return;
    }

    int32_t playbackSessionID = makeUniquePlaybackSessionID();

    sp<AMessage> notify = new AMessage(kWhatPlaybackSessionNotify, id());
    notify->setInt32("playbackSessionID", playbackSessionID);
    notify->setInt32("sessionID", sessionID);

    sp<PlaybackSession> playbackSession =
        new PlaybackSession(mNetSession, notify);

    looper()->registerHandler(playbackSession);

    AString uri;
    data->getRequestField(1, &uri);

    if (strncasecmp("rtsp://", uri.c_str(), 7)) {
        sendErrorResponse(sessionID, "400 Bad Request", cseq);
        return;
    }

    if (!(uri.startsWith("rtsp://") && uri.endsWith("/wfd1.0/streamid=0"))) {
        sendErrorResponse(sessionID, "404 Not found", cseq);
        return;
    }

    const ClientInfo &info = mClientIPs.valueFor(sessionID);

    status_t err = playbackSession->init(
            info.mRemoteIP.c_str(),
            clientRtp,
            clientRtcp,
            useInterleavedTCP);

    if (err != OK) {
        looper()->unregisterHandler(playbackSession->id());
        playbackSession.clear();
    }

    switch (err) {
        case OK:
            break;
        case -ENOENT:
            sendErrorResponse(sessionID, "404 Not Found", cseq);
            return;
        default:
            sendErrorResponse(sessionID, "403 Forbidden", cseq);
            return;
    }

    mPlaybackSessions.add(playbackSessionID, playbackSession);

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);

    if (useInterleavedTCP) {
        response.append(
                StringPrintf(
                    "Transport: RTP/AVP/TCP;interleaved=%d-%d;",
                    clientRtp, clientRtcp));
    } else {
        int32_t serverRtp = playbackSession->getRTPPort();

        if (clientRtcp >= 0) {
            response.append(
                    StringPrintf(
                        "Transport: RTP/AVP;unicast;client_port=%d-%d;"
                        "server_port=%d-%d\r\n",
                        clientRtp, clientRtcp, serverRtp, serverRtp + 1));
        } else {
            response.append(
                    StringPrintf(
                        "Transport: RTP/AVP;unicast;client_port=%d;"
                        "server_port=%d\r\n",
                        clientRtp, serverRtp));
        }
    }

    response.append("\r\n");

    err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);

#if 0
    // XXX the dongle does not currently send keep-alives.
    scheduleReaper();
#endif
}

void WifiDisplaySource::onPlayRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return;
    }

    status_t err = playbackSession->play();
    CHECK_EQ(err, (status_t)OK);

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("Range: npt=now-\r\n");
    response.append("\r\n");

    err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySource::onPauseRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return;
    }

    status_t err = playbackSession->pause();
    CHECK_EQ(err, (status_t)OK);

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySource::onTeardownRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return;
    }

    looper()->unregisterHandler(playbackSession->id());
    mPlaybackSessions.removeItem(playbackSessionID);

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySource::onGetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return;
    }

    playbackSession->updateLiveness();

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
}

void WifiDisplaySource::onSetParameterRequest(
        int32_t sessionID,
        int32_t cseq,
        const sp<ParsedMessage> &data) {
    int32_t playbackSessionID;
#if 0
    // XXX the dongle does not include a "Session:" header in this request.
    sp<PlaybackSession> playbackSession =
        findPlaybackSession(data, &playbackSessionID);

    if (playbackSession == NULL) {
        sendErrorResponse(sessionID, "454 Session Not Found", cseq);
        return;
    }
#else
    CHECK_EQ(mPlaybackSessions.size(), 1u);
    playbackSessionID = mPlaybackSessions.keyAt(0);
    sp<PlaybackSession> playbackSession = mPlaybackSessions.valueAt(0);
#endif

    playbackSession->updateLiveness();

    AString response = "RTSP/1.0 200 OK\r\n";
    AppendCommonResponse(&response, cseq, playbackSessionID);
    response.append("\r\n");

    status_t err = mNetSession->sendRequest(sessionID, response.c_str());
    CHECK_EQ(err, (status_t)OK);
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
    for (;;) {
        int32_t playbackSessionID = rand();

        for (size_t i = 0; i < mPlaybackSessions.size(); ++i) {
            if (mPlaybackSessions.keyAt(i) == playbackSessionID) {
                continue;
            }
        }

        return playbackSessionID;
    }
}

sp<WifiDisplaySource::PlaybackSession> WifiDisplaySource::findPlaybackSession(
        const sp<ParsedMessage> &data, int32_t *playbackSessionID) const {
    if (!data->findInt32("session", playbackSessionID)) {
        *playbackSessionID = 0;
        return NULL;
    }

    ssize_t index = mPlaybackSessions.indexOfKey(*playbackSessionID);
    if (index < 0) {
        return NULL;
    }

    return mPlaybackSessions.valueAt(index);
}

}  // namespace android

