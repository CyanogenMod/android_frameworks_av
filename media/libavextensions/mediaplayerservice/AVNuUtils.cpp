/*
 * Copyright (c) 2013 - 2015, The Linux Foundation. All rights reserved.
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

sp<MetaData> AVNuUtils::createPCMMetaFromSource(const sp<MetaData> &sMeta) {
    return sMeta;
}

bool AVNuUtils::pcmOffloadException(const sp<MetaData> &) {
    return true;
}

bool AVNuUtils::isRAWFormat(const sp<MetaData> &) {
    return false;
}

bool AVNuUtils::isRAWFormat(const sp<AMessage> &) {
    return false;
}

bool AVNuUtils::isVorbisFormat(const sp<MetaData> &) {
    return false;
}

int AVNuUtils::updateAudioBitWidth(audio_format_t /*audioFormat*/,
        const sp<AMessage> &){
    return 16;
}

audio_format_t AVNuUtils::getKeyPCMFormat(const sp<MetaData> &) {
    return AUDIO_FORMAT_INVALID;
}

void AVNuUtils::setKeyPCMFormat(const sp<MetaData> &, audio_format_t /*audioFormat*/) {

}

audio_format_t AVNuUtils::getPCMFormat(const sp<AMessage> &) {
    return AUDIO_FORMAT_PCM_16_BIT;
}

void AVNuUtils::setPCMFormat(const sp<AMessage> &, audio_format_t /*audioFormat*/) {

}

void AVNuUtils::setSourcePCMFormat(const sp<MetaData> &) {

}

void AVNuUtils::setDecodedPCMFormat(const sp<AMessage> &) {

}

status_t AVNuUtils::convertToSinkFormatIfNeeded(const sp<ABuffer> &, sp<ABuffer> &,
        audio_format_t /*sinkFormat*/, bool /*isOffload*/) {
    return INVALID_OPERATION;
}

void AVNuUtils::printFileName(int) {}

void AVNuUtils::checkFormatChange(bool * /*formatChange*/,
        const sp<ABuffer> & /*accessUnit*/) {
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVNuUtils::AVNuUtils() {}

AVNuUtils::~AVNuUtils() {}

//static
AVNuUtils *AVNuUtils::sInst =
        ExtensionsLoader<AVNuUtils>::createInstance("createExtendedNuUtils");

} //namespace android

