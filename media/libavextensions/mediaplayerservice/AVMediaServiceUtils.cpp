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

#define LOG_TAG "AVMediaServiceUtils"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include "ARTPConnection.h"
#include "ASessionDescription.h"
#include "MyHandler.h"

#include "common/ExtensionsLoader.hpp"
#include "mediaplayerservice/AVMediaServiceExtensions.h"

namespace android {

bool AVMediaServiceUtils::pokeAHole(sp<MyHandler> handler, int rtpSocket, int rtcpSocket,
        const AString &transport, const AString &/*sessionHost*/) {
    if (handler == NULL) {
        ALOGW("MyHandler is NULL");
        return false;
    }
    return handler->pokeAHole(rtpSocket, rtcpSocket, transport);
}

void AVMediaServiceUtils::makePortPair(int *rtpSocket, int *rtcpSocket, unsigned *rtpPort,
        bool /*isIPV6*/) {
    return ARTPConnection::MakePortPair(rtpSocket, rtcpSocket, rtpPort);
}

const char* AVMediaServiceUtils::parseURL(AString *host) {
    return strchr(host->c_str(), ':');
}

bool AVMediaServiceUtils::parseTrackURL(AString /*url*/, AString /*val*/) {
    return false;
}

void AVMediaServiceUtils::appendRange(AString * /*request*/) {
    return;
}

void AVMediaServiceUtils::setServerTimeoutUs(int64_t /*timeout*/) {
    return;
}

void AVMediaServiceUtils::addH263AdvancedPacket(const sp<ABuffer> &/*buffer*/,
        List<sp<ABuffer>> * /*packets*/, uint32_t /*rtpTime*/) {
    return;
}

void AVMediaServiceUtils::appendMeta(media::Metadata * /*meta*/) {
    return;
}

bool AVMediaServiceUtils::checkNPTMapping(uint32_t * /*rtpInfoTime*/, int64_t * /*playTimeUs*/,
        bool * /*nptValid*/, uint32_t /*rtpTime*/) {
    return false;
}

bool AVMediaServiceUtils::parseNTPRange(const char *s, float *npt1, float *npt2) {
    return ASessionDescription::parseNTPRange(s, npt1, npt2);
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVMediaServiceUtils::AVMediaServiceUtils() {
}

AVMediaServiceUtils::~AVMediaServiceUtils() {
}

//static
AVMediaServiceUtils *AVMediaServiceUtils::sInst =
        ExtensionsLoader<AVMediaServiceUtils>::createInstance("createExtendedMediaServiceUtils");

} //namespace android

