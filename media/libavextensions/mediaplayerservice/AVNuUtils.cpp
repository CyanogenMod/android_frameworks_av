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

#define LOG_TAG "AVNuUtils"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

#include <nuplayer/NuPlayer.h>
#include <nuplayer/NuPlayerDecoderBase.h>
#include <nuplayer/NuPlayerDecoderPassThrough.h>
#include <nuplayer/NuPlayerSource.h>
#include <nuplayer/NuPlayerRenderer.h>

#include "common/ExtensionsLoader.hpp"
#include "mediaplayerservice/AVNuExtensions.h"

namespace android {

bool AVNuUtils::isVorbisFormat(const sp<MetaData> &) {
    return false;
}

void AVNuUtils::printFileName(int) {}

bool AVNuUtils::dropCorruptFrame() { return false; }

void AVNuUtils::addFlagsInMeta(const sp<ABuffer> & /*buffer*/,
        int32_t /*flags*/, bool /*isAudio*/) {
}

bool AVNuUtils::pcmOffloadException(const sp<AMessage> &) {
    return true;
}

audio_format_t AVNuUtils::getPCMFormat(const sp<AMessage> &) {
    return AUDIO_FORMAT_PCM_16_BIT;
}

void AVNuUtils::setCodecOutputFormat(const sp<AMessage> &) {

}

void AVNuUtils::overWriteAudioOutputFormat(
       sp <AMessage> & /*dst*/, const sp <AMessage> & /*src*/) {
}

void AVNuUtils::checkFormatChange(bool * /*formatChange*/,
        const sp<ABuffer> & /*accessUnit*/) {
}

bool AVNuUtils::isByteStreamModeEnabled(const sp<MetaData> &) {
    return false;
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVNuUtils::AVNuUtils() {}

AVNuUtils::~AVNuUtils() {}

//static
AVNuUtils *AVNuUtils::sInst =
        ExtensionsLoader<AVNuUtils>::createInstance("createExtendedNuUtils");

} //namespace android

