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

#define LOG_TAG "AVUtils"
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <cutils/properties.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaCodec.h>

#include "omx/OMXUtils.h"

#if defined(QCOM_HARDWARE) || defined(EXTENDED_OFFLOAD_ENABLED)
#include "OMX_QCOMExtns.h"
#include "QCMediaDefs.h"
#include "QCMetaData.h"
#ifdef EXTENDED_OFFLOAD_ENABLED
#include <mm-audio/audio_defs.h>
#endif
#endif

#include <binder/IPCThreadState.h>
#include <camera/CameraParameters.h>

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
#endif
#ifdef FLAC_OFFLOAD_ENABLED
   {kKeyMinBlkSize           , "min-block-size"         , INT32},
   {kKeyMaxBlkSize           , "max-block-size"         , INT32},
   {kKeyMinFrmSize           , "min-frame-size"         , INT32},
   {kKeyMaxFrmSize           , "max-frame-size"         , INT32},
#endif

   {kKeyBitRate              , "bitrate"                , INT32},
   {kKeySampleRate           , "sample-rate"            , INT32},
   {kKeyChannelCount         , "channel-count"          , INT32},
   {kKeyRawCodecSpecificData , "raw-codec-specific-data", CSD},

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
    { MEDIA_MIMETYPE_AUDIO_ALAC,        AUDIO_FORMAT_ALAC },
    { MEDIA_MIMETYPE_CONTAINER_QTIFLAC, AUDIO_FORMAT_FLAC },
#ifdef DOLBY_UDC
    { MEDIA_MIMETYPE_AUDIO_EAC3_JOC,    AUDIO_FORMAT_E_AC3_JOC },
#endif
#endif
    { 0, AUDIO_FORMAT_INVALID }
};

status_t AVUtils::convertMessageToMetaData(
        const sp<AMessage> &, sp<MetaData> &) {
    return OK;
}

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
#ifdef EXTENDED_OFFLOAD_ENABLED
    const char *mime;
    if (!meta->findCString(kKeyMIMEType, &mime)) {
        return NO_INIT;
    }
    ALOGV("extended mime: %s", mime);

#ifdef FLAC_OFFLOAD_ENABLED
    if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)) {
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
        ALOGV("FLAC metadata: minBlkSize %d, maxBlkSize %d, minFrmSize %d, maxFrmSize %d",
                minBlkSize, maxBlkSize, minFrmSize, maxFrmSize);
        return OK;
    }
#endif

    const void *data;
    size_t size;
    uint32_t type = 0;
    if (!meta->findData(kKeyRawCodecSpecificData, &type, &data, &size)) {
        return NO_INIT;
    }
    ALOGV("Found kKeyRawCodecSpecificData of size %zu", size);
    const uint8_t *ptr = (uint8_t *) data;

#ifdef ALAC_OFFLOAD_ENABLED
    if (!strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_ALAC,
                strlen(MEDIA_MIMETYPE_AUDIO_ALAC))) {
        uint32_t frameLength = 0, maxFrameBytes = 0;
        uint32_t channelLayoutTag = 0;
        uint16_t maxRun = 0;
        uint8_t compatibleVersion = 0, pb = 0, mb = 0, kb = 0;

        int32_t numChannels = 0, samplingRate = 0, avgBitRate = 0;
        int32_t bitDepth = getAudioSampleBits(meta);

        meta->findInt32(kKeyChannelCount, &numChannels);
        meta->findInt32(kKeySampleRate, &samplingRate);
        meta->findInt32(kKeyBitRate, &avgBitRate);

        /*
         * 32bit  atom size
         * 32bit  tag                  ("alac")
         * 32bit  tag version          (0)
         * 32bit  samples per frame    (used when not set explicitly in the frames)
         *  8bit  compatible version   (0)
         *  8bit  sample size
         *  8bit  history mult         (40)
         *  8bit  initial history      (10)
         *  8bit  rice param limit     (14)
         *  8bit  channels
         * 16bit  maxRun               (255)
         * 32bit  max coded frame size (0 means unknown)
         * 32bit  average bitrate      (0 means unknown)
         * 32bit  samplerate
         */
        ABitReader br(ptr, size);
        br.skipBits(96);
        frameLength = br.getBits(32);
        compatibleVersion = br.getBits(8);
        br.skipBits(8);
        pb = br.getBits(8);
        mb = br.getBits(8);
        kb = br.getBits(8);
        br.skipBits(8);
        maxRun = br.getBits(16);
        maxFrameBytes = br.getBits(32);

        ALOGV("ALAC CSD values: frameLength %d bitDepth %d numChannels %d"
                " maxFrameBytes %d avgBitRate %d samplingRate %d",
                frameLength, bitDepth, numChannels, maxFrameBytes, avgBitRate, samplingRate);

        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_FRAME_LENGTH), frameLength);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_COMPATIBLE_VERSION), compatibleVersion);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_BIT_DEPTH), bitDepth);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_PB), pb);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_MB), mb);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_KB), kb);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_NUM_CHANNELS), numChannels);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_MAX_RUN), maxRun);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_MAX_FRAME_BYTES), maxFrameBytes);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_AVG_BIT_RATE), avgBitRate);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_SAMPLING_RATE), samplingRate);
        param->addInt(String8(AUDIO_OFFLOAD_CODEC_ALAC_CHANNEL_LAYOUT_TAG), channelLayoutTag);
        return OK;
    }
#endif

#else
    (void)meta;
    (void)param;
#endif
    return OK;
}

bool AVUtils::hasAudioSampleBits(const sp<MetaData> &meta) {
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    return meta->findInt32(kKeyPcmEncoding, (int32_t*)&encoding);
}

bool AVUtils::hasAudioSampleBits(const sp<AMessage> &format) {
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    return format->findInt32("pcm-encoding", (int32_t*)&encoding);
}

int AVUtils::getAudioSampleBits(const sp<MetaData> &meta) {
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    meta->findInt32(kKeyPcmEncoding, (int32_t*)&encoding);
    return audioEncodingToBits(encoding);
}

int AVUtils::getAudioSampleBits(const sp<AMessage> &format) {
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    format->findInt32("pcm-encoding", (int32_t*)&encoding);
    return audioEncodingToBits(encoding);
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<MetaData> &){
    return audioFormat;
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<AMessage> &){
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

bool AVUtils::canOffloadAPE(const sp<MetaData> &) {
   return true;
}

bool AVUtils::isEnhancedExtension(const char *) {
    return false;
}


bool AVUtils::isAudioMuxFormatSupported(const char * mime) {
    if (mime == NULL) {
        ALOGE("NULL audio mime type");
        return false;
    }

    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, mime)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, mime)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime)) {
        return true;
    }
    return false;
}

void AVUtils::cacheCaptureBuffers(sp<hardware::ICamera> camera, video_encoder encoder) {
    if (camera != NULL) {
        char mDeviceName[PROPERTY_VALUE_MAX];
        property_get("ro.board.platform", mDeviceName, "0");
        if (!strncmp(mDeviceName, "msm8909", 7)) {
            int64_t token = IPCThreadState::self()->clearCallingIdentity();
            String8 s = camera->getParameters();
            CameraParameters params(s);
            const char *enable;
            if (encoder == VIDEO_ENCODER_H263 ||
                encoder == VIDEO_ENCODER_MPEG_4_SP) {
                enable = "1";
            } else {
                enable = "0";
            }
            params.set("cache-video-buffers", enable);
            if (camera->setParameters(params.flatten()) != OK) {
                ALOGE("Failed to enabled cached camera buffers");
            }
            IPCThreadState::self()->restoreCallingIdentity(token);
        }
    }
}

const char *AVUtils::getCustomCodecsLocation() {
    return "/etc/media_codecs.xml";
}

#ifdef QCOM_HARDWARE
void AVUtils::setIntraPeriod(
        int nPFrames, int nBFrames, const sp<IOMX> omxHandle,
        IOMX::node_id nodeID) {

    QOMX_VIDEO_INTRAPERIODTYPE intraperiod;
    InitOMXParams(&intraperiod);

    intraperiod.nPortIndex = kPortIndexOutput;
    intraperiod.nIDRPeriod = 1;
    intraperiod.nPFrames = nPFrames - 1;
    intraperiod.nBFrames = nBFrames;
    omxHandle->setConfig(
        nodeID, (OMX_INDEXTYPE)QOMX_IndexConfigVideoIntraperiod, &intraperiod, sizeof(intraperiod));
}
#else
void AVUtils::setIntraPeriod(
        int, int, const sp<IOMX>,
        IOMX::node_id) {
    return;
}
#endif

const char *AVUtils::getCustomCodecsPerformanceLocation() {
    return "/etc/media_codecs_performance.xml";
}

bool AVUtils::IsHevcIDR(const sp<ABuffer> &) {
   return false;
}

bool AVUtils::useQCHWEncoder(const sp<AMessage> &format, Vector<AString> *matchingCodecs) {
#ifdef QCOM_HARDWARE
    AString mime;
    if (!format->findString("mime", &mime)) {
        return false;
    }

    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime.c_str())) {
        if (!property_get_bool("qcom.hw.aac.encoder", false)) {
            return false;
        }
        matchingCodecs->push(AString("OMX.qcom.audio.encoder.aac"));
        return true;
    }
#else
    (void)format;
    (void)matchingCodecs;
#endif

    return false;
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVUtils::AVUtils() {}

AVUtils::~AVUtils() {}

//static
AVUtils *AVUtils::sInst =
        ExtensionsLoader<AVUtils>::createInstance("createExtendedUtils");

} //namespace android

