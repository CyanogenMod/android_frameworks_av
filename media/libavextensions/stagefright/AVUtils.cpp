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

#define LOG_TAG "AVUtils"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaCodec.h>

#ifdef QCOM_HARDWARE
#include "QCMediaDefs.h"
#include "QCMetaData.h"
#ifdef FLAC_OFFLOAD_ENABLED
#include "audio_defs.h"
#endif
#endif

#include "common/ExtensionsLoader.hpp"
#include "stagefright/AVExtensions.h"

namespace android {

enum MetaKeyType{
    INT32, INT64, STRING, DATA, CSD
};

struct MetaKeyEntry{
    int MetaKey;
    const char* MsgKey;
    MetaKeyType KeyType;
};

static const MetaKeyEntry MetaKeyTable[] {
#ifdef QCOM_HARDWARE
   {kKeyAacCodecSpecificData , "aac-codec-specific-data", CSD},
   {kKeyDivXVersion          , "divx-version"           , INT32},  // int32_t
   {kKeyDivXDrm              , "divx-drm"               , DATA},  // void *
   {kKeyWMAEncodeOpt         , "wma-encode-opt"         , INT32},  // int32_t
   {kKeyWMABlockAlign        , "wma-block-align"        , INT32},  // int32_t
   {kKeyWMAAdvEncOpt1        , "wma-adv-enc-opt1"       , INT32},  // int16_t
   {kKeyWMAAdvEncOpt2        , "wma-adv-enc-opt2"       , INT32},  // int32_t
   {kKeyWMAFormatTag         , "wma-format-tag"         , INT32},  // int32_t
   {kKeyWMABitspersample     , "wma-bits-per-sample"    , INT32},  // int32_t
   {kKeyWMAVirPktSize        , "wma-vir-pkt-size"       , INT32},  // int32_t
   {kKeyWMAChannelMask       , "wma-channel-mask"       , INT32},  // int32_t
   {kKeyFileFormat           , "file-format"            , STRING},  // cstring

   {kkeyAacFormatAdif        , "aac-format-adif"        , INT32},  // bool (int32_t)
   {kkeyAacFormatLtp         , "aac-format-ltp"         , INT32},

   //DTS subtype
   {kKeyDTSSubtype           , "dts-subtype"            , INT32},  //int32_t

   //Extractor sets this
   {kKeyUseArbitraryMode     , "use-arbitrary-mode"     , INT32},  //bool (int32_t)
   {kKeySmoothStreaming      , "smooth-streaming"       , INT32},  //bool (int32_t)
   {kKeyHFR                  , "hfr"                    , INT32},  // int32_t
#ifdef FLAC_OFFLOAD_ENABLED
   {kKeyMinBlkSize           , "min-block-size"         , INT32},
   {kKeyMaxBlkSize           , "max-block-size"         , INT32},
   {kKeyMinFrmSize           , "min-frame-size"         , INT32},
   {kKeyMaxFrmSize           , "max-frame-size"         , INT32},
#endif
#endif


   {kKeyBitRate              , "bitrate"                , INT32},
   {kKeySampleRate           , "sample-rate"            , INT32},
   {kKeyChannelCount         , "channel-count"          , INT32},
   {kKeyRawCodecSpecificData , "raw-codec-specific-data", CSD},

   {kKeyBitsPerSample        , "bits-per-sample"        , INT32},
   {kKeyCodecId              , "codec-id"               , INT32},
   {kKeySampleFormat         , "sample-format"          , INT32},
   {kKeyBlockAlign           , "block-align"            , INT32},
   {kKeyCodedSampleBits      , "coded-sample-bits"      , INT32},
   {kKeyAACAOT               , "aac-profile"            , INT32},
   {kKeyRVVersion            , "rv-version"             , INT32},
   {kKeyWMAVersion           , "wma-version"            , INT32},  // int32_t
   {kKeyWMVVersion           , "wmv-version"            , INT32},
};

status_t AVUtils::convertMetaDataToMessage(
        const sp<MetaData> &meta, sp<AMessage> *format) {
    const char * str_val;
    int32_t int32_val;
    int64_t int64_val;
    uint32_t data_type;
    const void * data;
    size_t size;
    static const size_t numMetaKeys =
                     sizeof(MetaKeyTable) / sizeof(MetaKeyTable[0]);
    size_t i;
    for (i = 0; i < numMetaKeys; ++i) {
        if (MetaKeyTable[i].KeyType == INT32 &&
            meta->findInt32(MetaKeyTable[i].MetaKey, &int32_val)) {
            ALOGV("found metakey %s of type int32", MetaKeyTable[i].MsgKey);
            format->get()->setInt32(MetaKeyTable[i].MsgKey, int32_val);
        } else if (MetaKeyTable[i].KeyType == INT64 &&
                 meta->findInt64(MetaKeyTable[i].MetaKey, &int64_val)) {
            ALOGV("found metakey %s of type int64", MetaKeyTable[i].MsgKey);
            format->get()->setInt64(MetaKeyTable[i].MsgKey, int64_val);
        } else if (MetaKeyTable[i].KeyType == STRING &&
                 meta->findCString(MetaKeyTable[i].MetaKey, &str_val)) {
            ALOGV("found metakey %s of type string", MetaKeyTable[i].MsgKey);
            format->get()->setString(MetaKeyTable[i].MsgKey, str_val);
        } else if ( (MetaKeyTable[i].KeyType == DATA ||
                   MetaKeyTable[i].KeyType == CSD) &&
                   meta->findData(MetaKeyTable[i].MetaKey, &data_type, &data, &size)) {
            ALOGV("found metakey %s of type data", MetaKeyTable[i].MsgKey);
            if (MetaKeyTable[i].KeyType == CSD) {
                const char *mime;
                CHECK(meta->findCString(kKeyMIMEType, &mime));
                if (strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
                    sp<ABuffer> buffer = new ABuffer(size);
                    memcpy(buffer->data(), data, size);
                    buffer->meta()->setInt32("csd", true);
                    buffer->meta()->setInt64("timeUs", 0);
                    format->get()->setBuffer("csd-0", buffer);
                } else {
                    const uint8_t *ptr = (const uint8_t *)data;
                    CHECK(size >= 8);
                    int seqLength = 0, picLength = 0;
                    for (size_t i = 4; i < (size - 4); i++)
                    {
                        if ((*(ptr + i) == 0) && (*(ptr + i + 1) == 0) &&
                           (*(ptr + i + 2) == 0) && (*(ptr + i + 3) == 1))
                            seqLength = i;
                    }
                    sp<ABuffer> buffer = new ABuffer(seqLength);
                    memcpy(buffer->data(), data, seqLength);
                    buffer->meta()->setInt32("csd", true);
                    buffer->meta()->setInt64("timeUs", 0);
                    format->get()->setBuffer("csd-0", buffer);
                    picLength=size-seqLength;
                    sp<ABuffer> buffer1 = new ABuffer(picLength);
                    memcpy(buffer1->data(), (const uint8_t *)data + seqLength, picLength);
                    buffer1->meta()->setInt32("csd", true);
                    buffer1->meta()->setInt64("timeUs", 0);
                    format->get()->setBuffer("csd-1", buffer1);
                }
            } else {
                sp<ABuffer> buffer = new ABuffer(size);
                memcpy(buffer->data(), data, size);
                format->get()->setBuffer(MetaKeyTable[i].MsgKey, buffer);
            }
        }
    }
    return OK;
}

struct mime_conv_t {
    const char* mime;
    audio_format_t format;
};

static const struct mime_conv_t mimeLookup[] = {
    { MEDIA_MIMETYPE_AUDIO_MPEG,        AUDIO_FORMAT_MP3 },
    { MEDIA_MIMETYPE_AUDIO_RAW,         AUDIO_FORMAT_PCM_16_BIT },
    { MEDIA_MIMETYPE_AUDIO_AMR_NB,      AUDIO_FORMAT_AMR_NB },
    { MEDIA_MIMETYPE_AUDIO_AMR_WB,      AUDIO_FORMAT_AMR_WB },
    { MEDIA_MIMETYPE_AUDIO_AAC,         AUDIO_FORMAT_AAC },
    { MEDIA_MIMETYPE_AUDIO_VORBIS,      AUDIO_FORMAT_VORBIS },
    { MEDIA_MIMETYPE_AUDIO_OPUS,        AUDIO_FORMAT_OPUS},
#ifdef QCOM_HARDWARE
    { MEDIA_MIMETYPE_AUDIO_AC3,         AUDIO_FORMAT_AC3 },
    { MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS, AUDIO_FORMAT_AMR_WB_PLUS },
    { MEDIA_MIMETYPE_AUDIO_DTS,         AUDIO_FORMAT_DTS },
    { MEDIA_MIMETYPE_AUDIO_EAC3,        AUDIO_FORMAT_E_AC3 },
    { MEDIA_MIMETYPE_AUDIO_EVRC,        AUDIO_FORMAT_EVRC },
    { MEDIA_MIMETYPE_AUDIO_QCELP,       AUDIO_FORMAT_QCELP },
    { MEDIA_MIMETYPE_AUDIO_WMA,         AUDIO_FORMAT_WMA },
    { MEDIA_MIMETYPE_AUDIO_FLAC,        AUDIO_FORMAT_FLAC },
    { MEDIA_MIMETYPE_CONTAINER_QTIFLAC, AUDIO_FORMAT_FLAC },
#ifdef DOLBY_UDC
    { MEDIA_MIMETYPE_AUDIO_EAC3_JOC,    AUDIO_FORMAT_E_AC3_JOC },
#endif
#endif
    { 0, AUDIO_FORMAT_INVALID }
};

status_t AVUtils::mapMimeToAudioFormat(
        audio_format_t& format, const char* mime) {
    const struct mime_conv_t* p = &mimeLookup[0];
    while (p->mime != NULL) {
        if (0 == strcasecmp(mime, p->mime)) {
            format = p->format;
            return OK;
        }
        ++p;
    }

    return BAD_VALUE;
}

status_t AVUtils::sendMetaDataToHal(
        const sp<MetaData>& meta, AudioParameter *param){
#ifdef FLAC_OFFLOAD_ENABLED
    int32_t minBlkSize, maxBlkSize, minFrmSize, maxFrmSize; //FLAC params
    if (meta->findInt32(kKeyMinBlkSize, &minBlkSize)) {
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_FLAC_MIN_BLK_SIZE), minBlkSize);
    }
    if (meta->findInt32(kKeyMaxBlkSize, &maxBlkSize)) {
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_FLAC_MAX_BLK_SIZE), maxBlkSize);
    }
    if (meta->findInt32(kKeyMinFrmSize, &minFrmSize)) {
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_FLAC_MIN_FRAME_SIZE), minFrmSize);
    }
    if (meta->findInt32(kKeyMaxFrmSize, &maxFrmSize)) {
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_FLAC_MAX_FRAME_SIZE), maxFrmSize);
    }
#else
    (void)meta;
    (void)param;
#endif
    return OK;
}

bool AVUtils::is24bitPCMOffloadEnabled() {
    char propPCMOfload[PROPERTY_VALUE_MAX] = {0};
    property_get("audio.offload.pcm.24bit.enable", propPCMOfload, "0");
    if (!strncmp(propPCMOfload, "true", 4) || atoi(propPCMOfload))
        return true;
    else
        return false;
}

bool AVUtils::is16bitPCMOffloadEnabled() {
    char propPCMOfload[PROPERTY_VALUE_MAX] = {0};
    property_get("audio.offload.pcm.16bit.enable", propPCMOfload, "0");
    if (!strncmp(propPCMOfload, "true", 4) || atoi(propPCMOfload))
        return true;
    else
        return false;
}


int AVUtils::getAudioSampleBits(const sp<MetaData> &meta) {
    int32_t bits = 16;
    audio_format_t audioFormat = AUDIO_FORMAT_INVALID;
    if (meta->findInt32('pfmt', (int32_t *)&audioFormat)) {
        bits = audio_bytes_per_sample(audioFormat) * 8;
    } else if (meta->findInt32(kKeyBitsPerSample, &bits)) {
        return bits;
    }
    return bits;
}

int AVUtils::getAudioSampleBits(const sp<AMessage> &format) {
    int32_t bits = 16;
    audio_format_t audioFormat = AUDIO_FORMAT_INVALID;
    if (format->findInt32("pcm-format", (int32_t *)&audioFormat)) {
        bits = audio_bytes_per_sample(audioFormat) * 8;
    } else if (format->findInt32("bits-per-sample", &bits)) {
        return bits;
    }
    return bits;
}

void AVUtils::setPcmSampleBits(const sp<AMessage> &format, int32_t bitWidth) {
    format->setInt32("bits-per-sample", bitWidth);
}

void AVUtils::setPcmSampleBits(const sp<MetaData> &meta, int32_t bitWidth) {
    meta->setInt32(kKeyBitsPerSample, bitWidth);
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<MetaData> &meta){
    int32_t bits = getAudioSampleBits(meta);

    ALOGV("updateAudioFormat %x %d", audioFormat, bits);
    meta->dumpToLog();

    // Override audio format for PCM offload
    if (audio_is_linear_pcm(audioFormat)) {
        if (bits > 16 && is24bitPCMOffloadEnabled()) {
            audioFormat = AUDIO_FORMAT_PCM_24_BIT_OFFLOAD;
            meta->setInt32(kKeyBitsPerSample, 24);
        } else if (bits == 16 && is16bitPCMOffloadEnabled()) {
            audioFormat = AUDIO_FORMAT_PCM_16_BIT_OFFLOAD;
        }
    }

    return audioFormat;
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<AMessage> &format){
    int32_t bits = getAudioSampleBits(format);

    ALOGV("updateAudioFormat %x %d %s", audioFormat, bits, format->debugString().c_str());

    // Override audio format for PCM offload
    if (audio_is_linear_pcm(audioFormat)) {
        if (bits > 16 && is24bitPCMOffloadEnabled()) {
            audioFormat = AUDIO_FORMAT_PCM_24_BIT_OFFLOAD;
            format->setInt32("bits-per-sample", 24);
        } else if (bits == 16 && is16bitPCMOffloadEnabled()) {
            audioFormat = AUDIO_FORMAT_PCM_16_BIT_OFFLOAD;
        }
    }

    return audioFormat;
}

static bool dumbSniffer(
        const sp<DataSource> &, String8 *,
        float *, sp<AMessage> *) {
    return false;
}

DataSource::SnifferFunc AVUtils::getExtendedSniffer() {
    return dumbSniffer;
}

sp<MediaCodec> AVUtils::createCustomComponentByName(
        const sp<ALooper> &, const char* , bool, const sp<AMessage> &) {
    return NULL;
}

bool AVUtils::canOffloadAPE(const sp<MetaData> &) {
   return true;
}

int32_t AVUtils::getAudioMaxInputBufferSize(audio_format_t, const sp<AMessage> &) {
    return 0;
}

bool AVUtils::mapAACProfileToAudioFormat(const sp<MetaData> &, audio_format_t &,
                 uint64_t  /*eAacProfile*/) {
    return false ;
}

bool AVUtils::mapAACProfileToAudioFormat(const sp<AMessage> &,  audio_format_t &,
                 uint64_t  /*eAacProfile*/) {
    return false ;
}

bool AVUtils::isEnhancedExtension(const char *) {
    return false;
}

bool AVUtils::HEVCMuxer::reassembleHEVCCSD(const AString &/*mime*/, sp<ABuffer> /*csd0*/, sp<MetaData> &/*meta*/) {
    return false;
}

void AVUtils::HEVCMuxer::writeHEVCFtypBox(MPEG4Writer * /*writer*/) {
    return;
}

status_t AVUtils::HEVCMuxer::makeHEVCCodecSpecificData(const uint8_t * /*data*/,
        size_t /*size*/, void ** /*codecSpecificData*/,
        size_t * /*codecSpecificDataSize*/) {
    return UNKNOWN_ERROR;
}

const char *AVUtils::HEVCMuxer::getFourCCForMime(const char * /*mime*/) {
    return NULL;
}

void AVUtils::HEVCMuxer::writeHvccBox(MPEG4Writer * /*writer*/,
        void * /*codecSpecificData*/, size_t /*codecSpecificDataSize*/,
        bool /*useNalLengthFour*/) {
    return;
}

bool AVUtils::HEVCMuxer::isVideoHEVC(const char * /*mime*/) {
    return false;
}

void AVUtils::HEVCMuxer::getHEVCCodecSpecificDataFromInputFormatIfPossible(
        sp<MetaData> /*meta*/, void ** /*codecSpecificData*/,
        size_t * /*codecSpecificDataSize*/, bool * /*gotAllCodecSpecificData*/) {
    return;
}

bool AVUtils::isAudioMuxFormatSupported(const char *) {
    return true;
}

void AVUtils::cacheCaptureBuffers(sp<ICamera>, video_encoder) {
    return;
}

const char *AVUtils::getCustomCodecsLocation() {
    return "/etc/media_codecs.xml";
}

void AVUtils::setIntraPeriod(
        int, int, const sp<IOMX>,
        IOMX::node_id) {
    return;
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVUtils::AVUtils() {}

AVUtils::~AVUtils() {}

//static
AVUtils *AVUtils::sInst =
        ExtensionsLoader<AVUtils>::createInstance("createExtendedUtils");

} //namespace android

