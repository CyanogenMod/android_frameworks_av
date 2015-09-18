/*
 * Copyright (c) 2013 - 2016, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _AV_MEDIA_SERVICE_EXTENSIONS_H_
#define _AV_MEDIA_SERVICE_EXTENSIONS_H_

#include <common/AVExtensionsCommon.h>
#include <MediaPlayerFactory.h>

#include <utils/List.h>
#include <utils/RefBase.h>
#include <utils/String16.h>

#include <media/Metadata.h>

namespace android {

struct StagefrightRecorder;
struct ARTSPConnection;
struct ARTPConnection;
struct AString;
struct MyHandler;
struct ABuffer;

/*
 * Factory to create objects of base-classes in libmediaplayerservice
 */
struct AVMediaServiceFactory {
    virtual StagefrightRecorder *createStagefrightRecorder(const String16 &);

    // RTSP extensions
    virtual sp<ARTSPConnection> createARTSPConnection(bool uidValid, uid_t uid);
    virtual sp<ARTPConnection> createARTPConnection();

    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVMediaServiceFactory);
};

/*
 * Common delegate to the classes in libmediaplayerservice
 */
struct AVMediaServiceUtils {
    virtual void getDashPlayerFactory(MediaPlayerFactory::IFactory *&, player_type ) {}

    // RTSP IPV6 utils
    virtual bool pokeAHole(sp<MyHandler> handler, int rtpSocket, int rtcpSocket,
            const AString &transport, const AString &sessionHost);
    virtual void makePortPair(int *rtpSocket, int *rtcpSocket, unsigned *rtpPort,
            bool isIPV6);
    virtual const char* parseURL(AString *host);
    // RTSP customization utils
    virtual bool parseTrackURL(AString url, AString val);
    virtual void appendRange(AString *request);
    virtual void setServerTimeoutUs(int64_t timeout);
    virtual void appendMeta(media::Metadata *meta);
    virtual bool checkNPTMapping(uint32_t *rtpInfoTime, int64_t *playTimeUs,
            bool *nptValid, uint32_t rtpTime);
    virtual void addH263AdvancedPacket(const sp<ABuffer> &buffer,
            List<sp<ABuffer>> *packets, uint32_t rtpTime);
    virtual bool parseNTPRange(const char *s, float *npt1, float *npt2);

    // ----- NO TRESSPASSING BEYOND THIS LINE ------
    DECLARE_LOADABLE_SINGLETON(AVMediaServiceUtils);
};

}

#endif // _AV_EXTENSIONS__H_
