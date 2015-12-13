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

#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXCodec.h>
#include <cutils/properties.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/MediaProfiles.h>
#include <media/stagefright/Utils.h>

#include <audio_utils/format.h>

#include <nuplayer/NuPlayer.h>
#include <nuplayer/NuPlayerDecoderBase.h>
#include <nuplayer/NuPlayerDecoderPassThrough.h>
#include <nuplayer/NuPlayerSource.h>
#include <nuplayer/NuPlayerRenderer.h>

#include "common/ExtensionsLoader.hpp"
#include "mediaplayerservice/AVNuExtensions.h"

namespace android {

static bool is24bitPCMOffloadEnabled() {
    char propPCMOfload[PROPERTY_VALUE_MAX] = {0};
    property_get("audio.offload.pcm.24bit.enable", propPCMOfload, "0");
    if (!strncmp(propPCMOfload, "true", 4) || atoi(propPCMOfload))
        return true;
    else
        return false;
}

static bool is16bitPCMOffloadEnabled() {
    char propPCMOfload[PROPERTY_VALUE_MAX] = {0};
    property_get("audio.offload.pcm.16bit.enable", propPCMOfload, "0");
    if (!strncmp(propPCMOfload, "true", 4) || atoi(propPCMOfload))
        return true;
    else
        return false;
}

sp<MetaData> AVNuUtils::createPCMMetaFromSource(const sp<MetaData> &sMeta) {
    sp<MetaData> tPCMMeta = new MetaData;
    //hard code as RAW
    tPCMMeta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

    int32_t bits = 16;
    sMeta->findInt32(kKeyBitsPerSample, &bits);
    tPCMMeta->setInt32(kKeyBitsPerSample, bits > 24 ? 24 : bits);

    if (sMeta == NULL) {
        ALOGW("no meta returning dummy meta");
        return tPCMMeta;
    }

    int32_t srate = -1;
    if (!sMeta->findInt32(kKeySampleRate, &srate)) {
        ALOGV("No sample rate");
    }
    tPCMMeta->setInt32(kKeySampleRate, srate);

    int32_t cmask = 0;
    if (!sMeta->findInt32(kKeyChannelMask, &cmask) || (cmask == 0)) {
        ALOGI("No channel mask, try channel count");
    }
    int32_t channelCount = 0;
    if (!sMeta->findInt32(kKeyChannelCount, &channelCount)) {
        ALOGI("No channel count either");
    } else {
        //if channel mask is not set till now, use channel count
        //to retrieve channel count
        if (!cmask) {
            cmask = audio_channel_out_mask_from_count(channelCount);
        }
    }
    tPCMMeta->setInt32(kKeyChannelCount, channelCount);
    tPCMMeta->setInt32(kKeyChannelMask, cmask);

    int64_t duration = INT_MAX;
    if (!sMeta->findInt64(kKeyDuration, &duration)) {
        ALOGW("No duration in meta setting max duration");
    }
    tPCMMeta->setInt64(kKeyDuration, duration);

    int32_t bitRate = -1;
    if (!sMeta->findInt32(kKeyBitRate, &bitRate)) {
        ALOGW("No bitrate info");
    } else {
        tPCMMeta->setInt32(kKeyBitRate, bitRate);
    }

    return tPCMMeta;
}

bool AVNuUtils::pcmOffloadException(const sp<MetaData> &meta) {
    bool decision = false;
    const char *mime = {0};

    if (meta == NULL) {
        return true;
    }
    meta->findCString(kKeyMIMEType, &mime);

    if (!mime) {
        ALOGV("%s: no audio mime present, ignoring pcm offload", __func__);
        return true;
    }
//#if defined (PCM_OFFLOAD_ENABLED) || defined (PCM_OFFLOAD_ENABLED_24)
    const char * const ExceptionTable[] = {
        MEDIA_MIMETYPE_AUDIO_AMR_NB,
        MEDIA_MIMETYPE_AUDIO_AMR_WB,
        MEDIA_MIMETYPE_AUDIO_QCELP,
        MEDIA_MIMETYPE_AUDIO_G711_ALAW,
        MEDIA_MIMETYPE_AUDIO_G711_MLAW,
        MEDIA_MIMETYPE_AUDIO_EVRC
    };
    int countException = (sizeof(ExceptionTable) / sizeof(ExceptionTable[0]));

    for(int i = 0; i < countException; i++) {
        if (!strcasecmp(mime, ExceptionTable[i])) {
            decision = true;
            break;
        }
    }
    ALOGI("decision %d mime %s", decision, mime);
    return decision;
#if 0
    //if PCM offload flag is disabled, do not offload any sessions
    //using pcm offload
    decision = true;
    ALOGI("decision %d mime %s", decision, mime);
    return decision;
#endif
}

bool AVNuUtils::isRAWFormat(const sp<MetaData> &meta) {
    const char *mime = {0};
    if (meta == NULL) {
        return false;
    }
    CHECK(meta->findCString(kKeyMIMEType, &mime));
    if (!strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW, 9))
        return true;
    else
        return false;
}

bool AVNuUtils::isRAWFormat(const sp<AMessage> &format) {
    AString mime;
    if (format == NULL) {
        return false;
    }
    CHECK(format->findString("mime", &mime));
    if (!strncasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_RAW, 9))
        return true;
    else
        return false;

}

bool AVNuUtils::isVorbisFormat(const sp<MetaData> &) {
    return false;
}

int AVNuUtils::updateAudioBitWidth(audio_format_t audioFormat,
        const sp<AMessage> &format){
    int bits = 16;
    if (audio_is_linear_pcm(audioFormat) || audio_is_offload_pcm(audioFormat)) {
        bits = audio_bytes_per_sample(audioFormat) * 8;
        format->setInt32("bits-per-sample", bits);
    }
    return bits;
}

audio_format_t AVNuUtils::getKeyPCMFormat(const sp<MetaData> &meta) {
    audio_format_t pcmFormat = AUDIO_FORMAT_INVALID;
    meta->findInt32('pfmt', (int32_t *)&pcmFormat);
    return pcmFormat;
}

void AVNuUtils::setKeyPCMFormat(const sp<MetaData> &meta, audio_format_t audioFormat) {
    if (meta != NULL && audio_is_linear_pcm(audioFormat))
        meta->setInt32('pfmt', audioFormat);
}

audio_format_t AVNuUtils::getPCMFormat(const sp<AMessage> &format) {
    audio_format_t pcmFormat = AUDIO_FORMAT_INVALID;
    format->findInt32("pcm-format", (int32_t *)&pcmFormat);
    return pcmFormat;
}

void AVNuUtils::setPCMFormat(const sp<AMessage> &format, audio_format_t audioFormat) {
    if (audio_is_linear_pcm(audioFormat) || audio_is_offload_pcm(audioFormat))
        format->setInt32("pcm-format", audioFormat);
}

void AVNuUtils::setSourcePCMFormat(const sp<MetaData> &audioMeta) {
    if (!isRAWFormat(audioMeta))
        return;

    audio_format_t pcmFormat = getKeyPCMFormat(audioMeta);
    ALOGI("setSourcePCMFormat fmt=%x", pcmFormat);
    audioMeta->dumpToLog();
    if (pcmFormat == AUDIO_FORMAT_INVALID) {
        int32_t bits = 16;
        if (audioMeta->findInt32(kKeyBitsPerSample, &bits)) {
            if (bits == 8)
                pcmFormat = AUDIO_FORMAT_PCM_8_BIT;
            if (bits == 24)
                pcmFormat = AUDIO_FORMAT_PCM_32_BIT;
            if (bits == 32)
                pcmFormat = AUDIO_FORMAT_PCM_FLOAT;
            setKeyPCMFormat(audioMeta, pcmFormat);
        }
    }
}

void AVNuUtils::setDecodedPCMFormat(const sp<AMessage> &) {

}

status_t AVNuUtils::convertToSinkFormatIfNeeded(
        const sp<ABuffer> &buffer, sp<ABuffer> &newBuffer,
        audio_format_t sinkFormat, bool isOffload) {

    audio_format_t srcFormat = AUDIO_FORMAT_INVALID;
    if (!buffer->meta()->findInt32("pcm-format", (int32_t *)&srcFormat)) {
        newBuffer = buffer;
        return OK;
    }

    size_t bps = audio_bytes_per_sample(srcFormat);

    if (bps <= 0) {
        ALOGE("Invalid pcmformat %x given for conversion", srcFormat);
        return INVALID_OPERATION;
    }

    size_t frames = buffer->size() / bps;

    if (frames == 0) {
        ALOGE("zero sized buffer, nothing to convert");
        return BAD_VALUE;
    }

    ALOGV("convert %zu bytes (frames %d) of format %x",
          buffer->size(), frames, srcFormat);

    audio_format_t dstFormat;
    if (isOffload) {
        switch (sinkFormat) {
            case AUDIO_FORMAT_PCM_16_BIT_OFFLOAD:
                dstFormat = AUDIO_FORMAT_PCM_16_BIT;
                break;
            case AUDIO_FORMAT_PCM_24_BIT_OFFLOAD:
                if (srcFormat != AUDIO_FORMAT_PCM_24_BIT_PACKED &&
                    srcFormat != AUDIO_FORMAT_PCM_8_24_BIT) {
                        ALOGE("Invalid src format for 24 bit conversion");
                        return INVALID_OPERATION;
                }
                dstFormat = AUDIO_FORMAT_PCM_24_BIT_OFFLOAD;
                break;
            case AUDIO_FORMAT_DEFAULT:
                ALOGI("OffloadInfo not yet initialized, retry");
                return NO_INIT;
            default:
                ALOGE("Invalid offload format %x given for conversion",
                      sinkFormat);
                return INVALID_OPERATION;
        }
    } else {
        if (sinkFormat == AUDIO_FORMAT_INVALID) {
            ALOGD("PCM Info not yet initialized, drop buffer");
            return INVALID_OPERATION;
        }

        dstFormat = sinkFormat;
    }
    if (srcFormat == dstFormat) {
        ALOGV("same format");
        newBuffer = buffer;
        return OK;
    }

    size_t dstFrameSize = audio_bytes_per_sample(dstFormat);
    size_t dstBytes = frames * dstFrameSize;

    newBuffer = new ABuffer(dstBytes);

    memcpy_by_audio_format(newBuffer->data(), dstFormat,
                           buffer->data(), srcFormat, frames);

    ALOGV("convert to format %x newBuffer->size() %zu",
          dstFormat, newBuffer->size());

    // copy over some meta info
    int64_t timeUs = 0;
    buffer->meta()->findInt64("timeUs", &timeUs);
    newBuffer->meta()->setInt64("timeUs", timeUs);

    int32_t eos = false;
    buffer->meta()->findInt32("eos", &eos);
    newBuffer->meta()->setInt32("eos", eos);

    newBuffer->meta()->setInt32("pcm-format", (int32_t)dstFormat);
    return OK;
}

void AVNuUtils::printFileName(int) {}

void AVNuUtils::checkFormatChange(bool * /*formatChange*/,
        const sp<ABuffer> & /*accessUnit*/) {
}

#ifdef TARGET_8974
void AVNuUtils::addFlagsInMeta(const sp<ABuffer> & /*buffer*/,
        int32_t /*flags*/, bool /*isAudio*/) {
}
#endif

uint32_t AVNuUtils::getFlags() {
    return 0;
}

bool AVNuUtils::canUseSetBuffers(const sp<MetaData> &/*Meta*/) {
    return false;
}

bool AVNuUtils::dropCorruptFrame() { return false; }

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVNuUtils::AVNuUtils() {}

AVNuUtils::~AVNuUtils() {}

//static
AVNuUtils *AVNuUtils::sInst =
        ExtensionsLoader<AVNuUtils>::createInstance("createExtendedNuUtils");

} //namespace android

