/*
 * Copyright (c) 2013 - 2014, The Linux Foundation. All rights reserved.
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

//#define LOG_NDEBUG 0
#define LOG_TAG "ExtendedCodec"
#include <utils/Log.h>
#include <cutils/properties.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>

#include <media/stagefright/MetaData.h>
#include <media/stagefright/ExtendedCodec.h>
#include <media/stagefright/OMXCodec.h>

#define ARG_TOUCH(x) (void)x

#ifdef ENABLE_AV_ENHANCEMENTS

#include <QCMetaData.h>
#include <QCMediaDefs.h>
#include <OMX_QCOMExtns.h>
#include <OMX_Component.h>
#include <OMX_VideoExt.h>
#include <OMX_IndexExt.h>
#include <QOMX_AudioExtensions.h>
#include <QOMX_AudioIndexExtensions.h>
#include "include/ExtendedUtils.h"

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
   {kKeyBitRate              , "bitrate"                , INT32},
   {kKeyAacCodecSpecificData , "aac-codec-specific-data", CSD},
   {kKeyRawCodecSpecificData , "raw-codec-specific-data", CSD},
   {kKeyDivXVersion          , "divx-version"           , INT32},  // int32_t
   {kKeyDivXDrm              , "divx-drm"               , DATA},  // void *
   {kKeyWMAEncodeOpt         , "wma-encode-opt"         , INT32},  // int32_t
   {kKeyWMABlockAlign        , "wma-block-align"        , INT32},  // int32_t
   {kKeyWMAVersion           , "wma-version"            , INT32},  // int32_t
   {kKeyWMAAdvEncOpt1        , "wma-adv-enc-opt1"       , INT32},  // int16_t
   {kKeyWMAAdvEncOpt2        , "wma-adv-enc-opt2"       , INT32},  // int32_t
   {kKeyWMAFormatTag         , "wma-format-tag"         , INT32},  // int32_t
   {kKeyWMABitspersample     , "wma-bits-per-sample"    , INT32},  // int32_t
   {kKeyWMAVirPktSize        , "wma-vir-pkt-size"       , INT32},  // int32_t
   {kKeyWMAChannelMask       , "wma-channel-mask"       , INT32},  // int32_t
   {kKeyWMVVersion           , "wmv-version"            , INT32},
   {kKeyFileFormat           , "file-format"            , STRING},  // cstring
   {kKeyBlockAlign           , "block-align"            , INT32},
   {kKeyRVVersion            , "rv-version"             , INT32},

   {kkeyAacFormatAdif        , "aac-format-adif"        , INT32},  // bool (int32_t)
   {kkeyAacFormatLtp         , "aac-format-ltp"         , INT32},

   //DTS subtype
   {kKeyDTSSubtype           , "dts-subtype"            , INT32},  //int32_t

   //Extractor sets this
   {kKeyUseArbitraryMode     , "use-arbitrary-mode"     , INT32},  //bool (int32_t)
   {kKeySmoothStreaming      , "smooth-streaming"       , INT32},  //bool (int32_t)
   {kKeyHFR                  , "hfr"                    , INT32},  // int32_t

   {kKeySampleRate           , "sample-rate"            , INT32},
   {kKeyChannelCount         , "channel-count"          , INT32},
};

const char* ExtendedCodec::getMsgKey(int key) {
    static const size_t numMetaKeys =
                     sizeof(MetaKeyTable) / sizeof(MetaKeyTable[0]);
    size_t i;
    for (i = 0; i < numMetaKeys; ++i) {
        if (key == MetaKeyTable[i].MetaKey) {
            return MetaKeyTable[i].MsgKey;
        }
    }
    return "unknown";
}

status_t ExtendedCodec::convertMetaDataToMessage(
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

uint32_t ExtendedCodec::getComponentQuirks(
        const sp<MediaCodecInfo> &info) {
    uint32_t quirks = 0;

    if (info->hasQuirk("requires-wma-pro-component")) {
        quirks |= kRequiresWMAProComponent;
    }
    return quirks;
}

const char* ExtendedCodec::overrideComponentName(
        uint32_t quirks, const sp<MetaData> &meta, const char *mime, bool isEncoder) {
    const char* componentName = NULL;
    char value[PROPERTY_VALUE_MAX] = {0};
    int sw_codectype = 0;
    int enableSwHevc = 0;

    if (quirks & kRequiresWMAProComponent)
    {
       int32_t version = 0;
       if ((meta->findInt32(kKeyWMAVersion, &version))) {
          if (version==kTypeWMA) {
             componentName = "OMX.qcom.audio.decoder.wma";
          } else if (version==kTypeWMAPro) {
             componentName = "OMX.qcom.audio.decoder.wma10Pro";
          } else if (version==kTypeWMALossLess) {
             componentName = "OMX.qcom.audio.decoder.wmaLossLess";
          }
       }
    }

    if (!isEncoder && !strncasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC, strlen(MEDIA_MIMETYPE_VIDEO_HEVC))) {
        sw_codectype = property_get("media.swhevccodectype", value, NULL);
        enableSwHevc = atoi(value);
        if (sw_codectype && enableSwHevc) {
           componentName = "OMX.qcom.video.decoder.hevcswvdec";
        }
    }
    return componentName;
}

void ExtendedCodec::overrideComponentName(
        uint32_t quirks, const sp<AMessage> &msg, AString* componentName, AString* mime, int32_t isEncoder) {
    char value[PROPERTY_VALUE_MAX] = {0};
    int sw_codectype = 0;
    int enableSwHevc = 0;

    if (quirks & kRequiresWMAProComponent)
    {
       int32_t version = 0;
       if ((msg->findInt32(getMsgKey(kKeyWMAVersion), &version))) {
          if (version==kTypeWMA) {
             componentName->setTo("OMX.qcom.audio.decoder.wma");
          } else if (version==kTypeWMAPro) {
             componentName->setTo("OMX.qcom.audio.decoder.wma10Pro");
          } else if (version==kTypeWMALossLess) {
             componentName->setTo("OMX.qcom.audio.decoder.wmaLossLess");
          }
       }
    }

    if (!isEncoder && !strncasecmp(mime->c_str(), MEDIA_MIMETYPE_VIDEO_HEVC, strlen(MEDIA_MIMETYPE_VIDEO_HEVC))) {
        sw_codectype = property_get("media.swhevccodectype", value, NULL);
        enableSwHevc = atoi(value);
        if (sw_codectype && enableSwHevc) {
           componentName->setTo("OMX.qcom.video.decoder.hevcswvdec");
        }
    }
}

void ExtendedCodec::overrideMimeType(
        const sp<AMessage> &msg, AString* mime) {

    if (!strncmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_WMA,
         strlen(MEDIA_MIMETYPE_AUDIO_WMA))) {
        int32_t WMAVersion = 0;
        if ((msg->findInt32(getMsgKey(kKeyWMAVersion), &WMAVersion))) {
            if (WMAVersion==kTypeWMA) {
                //no need to update mime type
            } else if (WMAVersion==kTypeWMAPro) {
                mime->setTo("audio/x-ms-wma-pro");
            } else if (WMAVersion==kTypeWMALossLess) {
                mime->setTo("audio/x-ms-wma-lossless");
            } else {
                ALOGE("could not set valid wma mime type");
            }
        }
    }
}

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

status_t ExtendedCodec::setDIVXFormat(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, int port_index) {
    status_t err = OK;

    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime) ||
        !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime) ||
        !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
        ALOGV("Setting the QOMX_VIDEO_PARAM_DIVXTYPE params ");
        QOMX_VIDEO_PARAM_DIVXTYPE paramDivX;
        InitOMXParams(&paramDivX);
        paramDivX.nPortIndex = port_index;
        int32_t DivxVersion = 0;
        CHECK(msg->findInt32(getMsgKey(kKeyDivXVersion),&DivxVersion));
        ALOGV("Divx Version Type %d\n",DivxVersion);

        if (DivxVersion == kTypeDivXVer_4) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat4;
        } else if (DivxVersion == kTypeDivXVer_5) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat5;
        } else if (DivxVersion == kTypeDivXVer_6) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat6;
        } else if (DivxVersion == kTypeDivXVer_3_11 ) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat311;
        } else {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormatUnused;
        }
        paramDivX.eProfile = (QOMX_VIDEO_DIVXPROFILETYPE)0;    //Not used for now.

        err =  OMXhandle->setParameter(nodeID,
                         (OMX_INDEXTYPE)OMX_QcomIndexParamVideoDivx,
                         &paramDivX, sizeof(paramDivX));
    }

    return err;
}

void ExtendedCodec::getRawCodecSpecificData(
        const sp<MetaData> &meta, const void* &data, size_t &size) {
    uint32_t type = 0;
    size = 0;
    if (meta->findData(kKeyRawCodecSpecificData, &type, &data, &size)) {
        ALOGV("OMXCodec::configureCodec found kKeyRawCodecSpecificData of size %d\n", size);
    }
}

sp<ABuffer> ExtendedCodec::getRawCodecSpecificData(
        const sp<AMessage> &msg) {
    sp<ABuffer> buffer;
    if (msg->findBuffer(getMsgKey(kKeyRawCodecSpecificData), &buffer)) {
        ALOGV("ACodec found kKeyRawCodecSpecificData of size %d\n", buffer->size());
        return buffer;
    }
    return NULL;
}

void ExtendedCodec::getAacCodecSpecificData(
        const sp<MetaData> &meta, const void* &data, size_t &size) {
    uint32_t type = 0;
    size = 0;
    if (meta->findData(kKeyAacCodecSpecificData, &type, &data, &size)) {
        ALOGV("OMXCodec::configureCodec found kKeyAacCodecSpecificData of size %d\n", size);
    }
}

sp<ABuffer> ExtendedCodec::getAacCodecSpecificData(
        const sp<AMessage> &msg) {
    sp<ABuffer> buffer;
    if (msg->findBuffer(getMsgKey(kKeyAacCodecSpecificData), &buffer)) {
        ALOGV("ACodec found kKeyAacCodecSpecificData of size %d\n", buffer->size());
        return buffer;
    }
    return NULL;
}

status_t ExtendedCodec::setAudioFormat(
        const sp<MetaData> &meta, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    return setAudioFormat(msg, mime, OMXhandle, nodeID, isEncoder);
}

status_t ExtendedCodec::setAudioFormat(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setAudioFormat called");
    status_t err = OK;

    if ((!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mime)) ||
        (!strcasecmp(MEDIA_MIMETYPE_AUDIO_EAC3, mime))) {
        int32_t numChannels, sampleRate;
        CHECK(msg->findInt32("channel-count", &numChannels));
        CHECK(msg->findInt32("sample-rate", &sampleRate));
        /* Commenting following call as AC3 soft decoder does not
         need it and it causes issue with playback*/
        //setAC3Format(numChannels, sampleRate, OMXhandle, nodeID);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_EVRC, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(msg->findInt32("channel-count", &numChannels));
        CHECK(msg->findInt32("sample-rate", &sampleRate));
        setEVRCFormat(numChannels, sampleRate, OMXhandle, nodeID, isEncoder );
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_QCELP, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(msg->findInt32("channel-count", &numChannels));
        CHECK(msg->findInt32("sample-rate", &sampleRate));
        setQCELPFormat(numChannels, sampleRate, OMXhandle, nodeID, isEncoder);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mime))  {
        err = setWMAFormat(msg, OMXhandle, nodeID, isEncoder);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(msg->findInt32("channel-count", &numChannels));
        CHECK(msg->findInt32("sample-rate", &sampleRate));
        err = setAMRWBPLUSFormat(numChannels, sampleRate, OMXhandle, nodeID);
    } else {
        err = BAD_VALUE;
    }
    return err;
}

status_t ExtendedCodec::setVideoFormat(
        const sp<MetaData> &meta, const char* mime,
        OMX_VIDEO_CODINGTYPE *compressionFormat) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    return setVideoFormat(msg, mime, compressionFormat);
}

status_t ExtendedCodec::setVideoFormat(
        const sp<AMessage> &msg, const char* mime,
        OMX_VIDEO_CODINGTYPE *compressionFormat) {
    status_t retVal = OK;
    ALOGV("setVideoFormat: %s", msg->debugString(0).c_str());
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)) {
        *compressionFormat= (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime)) {
        *compressionFormat= (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
        *compressionFormat= (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)) {
        int32_t wmvVersion = 0;
        if (msg->findInt32(getMsgKey(kKeyWMVVersion), &wmvVersion)) {
            if (wmvVersion == 1) {
                ALOGE("Unsupported WMV version %d", wmvVersion);
                return ERROR_UNSUPPORTED;
            }
        }
        *compressionFormat = OMX_VIDEO_CodingWMV;
    } else if (!strcasecmp(MEDIA_MIMETYPE_CONTAINER_MPEG2, mime)) {
        *compressionFormat = OMX_VIDEO_CodingMPEG2;
    } else {
        retVal = BAD_VALUE;
    }

    return retVal;
}

status_t ExtendedCodec::setSupportedRole(
        const sp<IOMX> &omx, IOMX::node_id node,
        bool isEncoder, const char *mime) {
    ALOGV("setSupportedRole Called %s", mime);
    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kQCMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_EVRC,
          "audio_decoder.evrchw", "audio_encoder.evrc" },
        { MEDIA_MIMETYPE_AUDIO_QCELP,
          "audio_decoder,qcelp13Hw", "audio_encoder.qcelp13" },
        { MEDIA_MIMETYPE_VIDEO_DIVX,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX4,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX311,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_WMV,
          "video_decoder.vc1",  NULL },
        { MEDIA_MIMETYPE_AUDIO_AC3,
          "audio_decoder.ac3", NULL },
        { MEDIA_MIMETYPE_AUDIO_WMA,
          "audio_decoder.wma", NULL },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
          "video_decoder.hevc", "video_encoder.hevc" },
        };

    static const size_t kNumMimeToRole =
                     sizeof(kQCMimeToRole) / sizeof(kQCMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kQCMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
        return ERROR_UNSUPPORTED;
    }

    const char *role =
        isEncoder ? kQCMimeToRole[i].encoderRole
                  : kQCMimeToRole[i].decoderRole;

    if (role != NULL) {
        OMX_PARAM_COMPONENTROLETYPE roleParams;
        InitOMXParams(&roleParams);

        strncpy((char *)roleParams.cRole,
                role, OMX_MAX_STRINGNAME_SIZE - 1);

        roleParams.cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';

        status_t err = omx->setParameter(
                node, OMX_IndexParamStandardComponentRole,
                &roleParams, sizeof(roleParams));

        if (err != OK) {
            ALOGW("Failed to set standard component role '%s'.", role);
            return err;
        }
    }
    return OK;
}

status_t ExtendedCodec::getSupportedAudioFormatInfo(
        const AString* mime,
        sp<IOMX> OMXhandle,
        IOMX::node_id nodeID,
        int portIndex,
        int* channelCount,
        int* sampleRate) {
    status_t retVal = OK;
    if (!strncmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_QCELP, strlen(MEDIA_MIMETYPE_AUDIO_QCELP))) {
        OMX_AUDIO_PARAM_QCELP13TYPE params;
        InitOMXParams(&params);
        params.nPortIndex = portIndex;
        CHECK_EQ(OMXhandle->getParameter(
                   nodeID, OMX_IndexParamAudioQcelp13, &params, sizeof(params)), (status_t)OK);
        *channelCount = params.nChannels;
        /* QCELP supports only 8k sample rate*/
        *sampleRate = 8000;
    } else if (!strncmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_EVRC, strlen(MEDIA_MIMETYPE_AUDIO_EVRC))) {
        OMX_AUDIO_PARAM_EVRCTYPE params;
        InitOMXParams(&params);
        params.nPortIndex = portIndex;
        CHECK_EQ(OMXhandle->getParameter(
                   nodeID, OMX_IndexParamAudioEvrc, &params, sizeof(params)), (status_t)OK);
        *channelCount = params.nChannels;
        /* EVRC supports only 8k sample rate*/
        *sampleRate = 8000;
    } else if (!strncmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS, strlen(MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS))) {
        OMX_INDEXTYPE index;
        QOMX_AUDIO_PARAM_AMRWBPLUSTYPE params;

        InitOMXParams(&params);
        params.nPortIndex = portIndex;
        OMXhandle->getExtensionIndex(nodeID, OMX_QCOM_INDEX_PARAM_AMRWBPLUS, &index);
        CHECK_EQ(OMXhandle->getParameter(nodeID, index, &params, sizeof(params)),(status_t)OK);
        *channelCount = params.nChannels;
        *sampleRate = params.nSampleRate;
    } else if (!strncmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_WMA, strlen(MEDIA_MIMETYPE_AUDIO_WMA))) {
        status_t err = OK;
        OMX_INDEXTYPE index;
        OMX_AUDIO_PARAM_WMATYPE paramWMA;
        QOMX_AUDIO_PARAM_WMA10PROTYPE paramWMA10;

        InitOMXParams(&paramWMA);
        paramWMA.nPortIndex = portIndex;
        err = OMXhandle->getParameter(
                   nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
        if(err == OK) {
            ALOGV("WMA format");
            *channelCount = paramWMA.nChannels;
            *sampleRate = paramWMA.nSamplingRate;
        } else {
            InitOMXParams(&paramWMA10);
            paramWMA10.nPortIndex = portIndex;
            OMXhandle->getExtensionIndex(nodeID,"OMX.Qualcomm.index.audio.wma10Pro",&index);
            CHECK_EQ(OMXhandle->getParameter(nodeID, index, &paramWMA10, sizeof(paramWMA10)),(status_t)OK);
            ALOGV("WMA10 format");
            *channelCount = paramWMA10.nChannels;
            *sampleRate = paramWMA10.nSamplingRate;
        }
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}

status_t ExtendedCodec::handleSupportedAudioFormats(int format, AString* mime) {
    ALOGV("handleSupportedAudioFormats called for format:%x",format);
    status_t retVal = OK;
    if (format == OMX_AUDIO_CodingQCELP13 ) {
        *mime = MEDIA_MIMETYPE_AUDIO_QCELP;
    } else if (format == OMX_AUDIO_CodingEVRC ) {
        *mime = MEDIA_MIMETYPE_AUDIO_EVRC;
    } else if (format == OMX_AUDIO_CodingWMA ) {
        *mime = MEDIA_MIMETYPE_AUDIO_WMA;
    } else if (format == QOMX_IndexParamAudioAmrWbPlus ) {
        *mime = MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS;
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}

status_t ExtendedCodec::setupHEVCEncoderParameters(
            const sp<MetaData> &meta, const sp<IOMX> &omx,
            IOMX::node_id node, const char* componentName,
            int portIndex, const sp<OMXCodec> &target) {
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    if(!success) {
        ALOGE("Error: failed to find bitRate / frameRate / iFramesInterval");
        return UNKNOWN_ERROR;
    }

    OMX_VIDEO_PARAM_HEVCTYPE h265type;
    InitOMXParams(&h265type);
    h265type.nPortIndex = portIndex;

    status_t err = omx->getParameter(
            node, (OMX_INDEXTYPE)OMX_IndexParamVideoHevc, &h265type, sizeof(h265type));
    if (err != OK) {
        ALOGE("Error: getParameter IndexParamVideoHevc failed");
        return UNKNOWN_ERROR;
    }

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = h265type.eProfile;
    defaultProfileLevel.mLevel = h265type.eLevel;
    err = target->getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) {
        ALOGE("Error: failed to get Profile / Level");
        return err;
    }

    h265type.eProfile = static_cast<OMX_VIDEO_HEVCPROFILETYPE>(profileLevel.mProfile);
    h265type.eLevel = static_cast<OMX_VIDEO_HEVCLEVELTYPE>(profileLevel.mLevel);

    if (h265type.eProfile == OMX_VIDEO_HEVCProfileMain ||
        h265type.eProfile == OMX_VIDEO_HEVCProfileMain10){
        ALOGI("Profile type is %d", h265type.eProfile);
    } else {
        ALOGW("Use main profile instead of %d for HEVC recording",
            h265type.eProfile);
        h265type.eProfile = OMX_VIDEO_HEVCProfileMain;
    }

    return err;
}

status_t ExtendedCodec::handleSupportedVideoFormats(int format, AString* mime) {
    ALOGV("handleSupportedVideoFormats called");
    status_t retVal = OK;
    if (format == QOMX_VIDEO_CodingHevc) {
        *mime = MEDIA_MIMETYPE_VIDEO_HEVC;
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}

bool ExtendedCodec::checkIfCompressionHEVC(int format) {
    bool retVal = false;
    if (format == QOMX_VIDEO_CodingHevc) {
        retVal = true;
    }
    return retVal;
}

void ExtendedCodec::configureFramePackingFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID,
        const char* componentName) {
    //ignore non QC components
    if (strncmp(componentName, "OMX.qcom.", 9)) {
        return;
    }

    int32_t mode = 0;
    OMX_QCOM_PARAM_PORTDEFINITIONTYPE portFmt;
    portFmt.nPortIndex = kPortIndexInput;

    if (msg->findInt32(getMsgKey(kKeyUseArbitraryMode), &mode) && mode) {
        ALOGI("Decoder will be in arbitrary mode");
        portFmt.nFramePackingFormat = OMX_QCOM_FramePacking_Arbitrary;
    } else {
        ALOGI("Decoder will be in frame by frame mode");
        portFmt.nFramePackingFormat = OMX_QCOM_FramePacking_OnlyOneCompleteFrame;
    }
    status_t err = OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_QcomIndexPortDefn,
            (void *)&portFmt, sizeof(portFmt));
    if (err != OK) {
        ALOGW("Failed to set frame packing format on component");
    }
}

void ExtendedCodec::configureFramePackingFormat(
        const sp<MetaData> &meta, sp<IOMX> OMXhandle, IOMX::node_id nodeID,
        const char* componentName) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    configureFramePackingFormat(msg, OMXhandle, nodeID, componentName);
}

void ExtendedCodec::configureVideoDecoder(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        const uint32_t flags, IOMX::node_id nodeID, const char* componentName ) {
    if ((strncmp(componentName, "OMX.qcom.", 9)) && (strncmp(componentName, "OMX.ittiam.", 11))) {
        //do nothing for non QC component
        return;
    }

    configureFramePackingFormat(msg, OMXhandle, nodeID, componentName);

    setDIVXFormat(msg, mime, OMXhandle, nodeID, kPortIndexOutput);
    AString fileFormat;
    const char *fileFormatCStr = NULL;
    bool success = msg->findString(getMsgKey(kKeyFileFormat), &fileFormat);
    if (success) {
        fileFormatCStr = fileFormat.c_str();
    }

    // Enable timestamp reordering for AVI file type, mpeg4 and vc1 codec types
    const char* roleVC1 = "OMX.qcom.video.decoder.vc1";
    const char* roleMPEG4 = "OMX.qcom.video.decoder.mpeg4";
    if (!strncmp(componentName, roleVC1, strlen(roleVC1)) ||
            !strncmp(componentName, roleMPEG4, strlen(roleMPEG4)) ||
            (fileFormatCStr!= NULL && !strncmp(fileFormatCStr, "video/avi", 9))) {
        ALOGI("Enabling timestamp reordering");
        QOMX_INDEXTIMESTAMPREORDER reorder;
        InitOMXParams(&reorder);
        reorder.nPortIndex = kPortIndexOutput;
        reorder.bEnable = OMX_TRUE;
        status_t err = OMXhandle->setParameter(nodeID,
                       (OMX_INDEXTYPE)OMX_QcomIndexParamEnableTimeStampReorder,
                       (void *)&reorder, sizeof(reorder));

        if (err != OK) {
            ALOGW("Failed to enable timestamp reordering");
        }
    }

    // Enable Sync-frame decode mode for thumbnails
    if (flags & OMXCodec::kClientNeedsFramebuffer) {
        ALOGV("Enabling thumbnail mode.");
        QOMX_ENABLETYPE enableType;
        OMX_INDEXTYPE indexType;

        status_t err = OMXhandle->getExtensionIndex(
                nodeID, OMX_QCOM_INDEX_PARAM_VIDEO_SYNCFRAMEDECODINGMODE,
                &indexType);
        if (err != OK) {
            ALOGW("Failed to get extension for SYNCFRAMEDECODINGMODE");
            return;
        }

        enableType.bEnable = OMX_TRUE;
        err = OMXhandle->setParameter(nodeID,indexType,
                   (void *)&enableType, sizeof(enableType));
        if (err != OK) {
            ALOGW("Failed to get extension for SYNCFRAMEDECODINGMODE");
            return;
        }
        ALOGI("Thumbnail mode enabled.");
    }

    // MediaCodec clients can request decoder extradata by setting
    // "enable-extradata-<type>" in MediaFormat.
    // Following <type>s are supported:
    //    "user" => user-extradata
    int extraDataRequested = 0;
    if (msg->findInt32("enable-extradata-user", &extraDataRequested) &&
            extraDataRequested == 1) {
        ALOGI("[%s] User-extradata requested", componentName);
        QOMX_ENABLETYPE enableType;
        enableType.bEnable = OMX_TRUE;

        status_t err = OMXhandle->setParameter(
                nodeID, (OMX_INDEXTYPE)OMX_QcomIndexEnableExtnUserData,
                (OMX_PTR)&enableType, sizeof(enableType));
        if (err != OK) {
            ALOGW("[%s] Failed to enable user-extradata", componentName);
        }
    }
}

void ExtendedCodec::configureVideoDecoder(
        const sp<MetaData> &meta, const char* mime, sp<IOMX> OMXhandle,
        const uint32_t flags, IOMX::node_id nodeID, const char* componentName ) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    configureVideoDecoder(msg, mime, OMXhandle, flags, nodeID, componentName);
}

void ExtendedCodec::enableSmoothStreaming(
        const sp<IOMX> &omx, IOMX::node_id nodeID, bool* isEnabled,
        const char* componentName) {
    *isEnabled = false;

    if (!ExtendedUtils::ShellProp::isSmoothStreamingEnabled()) {
        return;
    }

    //ignore non QC components
    if (strncmp(componentName, "OMX.qcom.", 9)) {
        return;
    }
    if (strstr(componentName, ".secure")) {
        char prop[PROPERTY_VALUE_MAX] = {0};
        property_get("mm.disable.sec_smoothstreaming", prop, "0");
        if (!strncmp(prop, "true", 4) || atoi(prop)) {
            ALOGI("Smoothstreaming not enabled for secure Sessions");
            return;
        }
    }
    status_t err = omx->setParameter(
            nodeID,
            (OMX_INDEXTYPE)OMX_QcomIndexParamEnableSmoothStreaming,
            &err, sizeof(status_t));
    if (err != OK) {
        ALOGE("Failed to enable Smoothstreaming!");
        return;
    }
    *isEnabled = true;
    ALOGI("Smoothstreaming Enabled");
    return;
}

//private methods
void ExtendedCodec::setEVRCFormat(
        int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setEVRCFormat called");

    ARG_TOUCH(sampleRate);
    if (isEncoder) {
        CHECK(numChannels == 1);
        //////////////// input port ////////////////////
        //handle->setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
        //////////////// output port ////////////////////
        // format
        OMX_AUDIO_PARAM_PORTFORMATTYPE format;
        format.nPortIndex = kPortIndexOutput;
        format.nIndex = 0;
        status_t err = OMX_ErrorNone;
        while (OMX_ErrorNone == err) {
            CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamAudioPortFormat,
                    &format, sizeof(format)), (status_t)OK);
            if (format.eEncoding == OMX_AUDIO_CodingEVRC) {
                break;
            }
            format.nIndex++;
        }
        CHECK_EQ((status_t)OK, err);
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamAudioPortFormat,
                &format, sizeof(format)), (status_t)OK);

        // port definition
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;
        def.format.audio.cMIMEType = NULL;
        CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);
        def.format.audio.bFlagErrorConcealment = OMX_TRUE;
        def.format.audio.eEncoding = OMX_AUDIO_CodingEVRC;
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);

        // profile
        OMX_AUDIO_PARAM_EVRCTYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexOutput;
        CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamAudioEvrc,
                &profile, sizeof(profile)), (status_t)OK);
        profile.nChannels = numChannels;
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamAudioEvrc,
                &profile, sizeof(profile)), (status_t)OK);
    } else {
        ALOGI("EVRC decoder \n");
    }
}

void ExtendedCodec::setQCELPFormat(
        int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {

    ARG_TOUCH(sampleRate);
    if (isEncoder) {
        CHECK(numChannels == 1);
        //////////////// input port ////////////////////
        //handle->setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
        //////////////// output port ////////////////////
        // format
        OMX_AUDIO_PARAM_PORTFORMATTYPE format;
        format.nPortIndex = kPortIndexOutput;
        format.nIndex = 0;
        status_t err = OMX_ErrorNone;
        while (OMX_ErrorNone == err) {
            CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamAudioPortFormat,
                    &format, sizeof(format)), (status_t)OK);
            if (format.eEncoding == OMX_AUDIO_CodingQCELP13) {
                break;
            }
            format.nIndex++;
        }
        CHECK_EQ((status_t)OK, err);
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamAudioPortFormat,
                &format, sizeof(format)), (status_t)OK);

        // port definition
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;
        def.format.audio.cMIMEType = NULL;
        CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);
        def.format.audio.bFlagErrorConcealment = OMX_TRUE;
        def.format.audio.eEncoding = OMX_AUDIO_CodingQCELP13;
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);

        // profile
        OMX_AUDIO_PARAM_QCELP13TYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexOutput;
        CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamAudioQcelp13,
                &profile, sizeof(profile)), (status_t)OK);
        profile.nChannels = numChannels;
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamAudioQcelp13,
                &profile, sizeof(profile)), (status_t)OK);
    }
    else {
        ALOGI("QCELP decoder \n");
    }
}

status_t ExtendedCodec::setWMAFormat(
        const sp<MetaData> &meta, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    return setWMAFormat(msg, OMXhandle, nodeID, isEncoder);
}

status_t ExtendedCodec::setWMAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setWMAFormat Called");

    if (isEncoder) {
        ALOGE("WMA encoding not supported");
        return OK;
    } else {
        int32_t version;
        OMX_AUDIO_PARAM_WMATYPE paramWMA;
        QOMX_AUDIO_PARAM_WMA10PROTYPE paramWMA10;
        CHECK(msg->findInt32(getMsgKey(kKeyWMAVersion), &version));
        int32_t numChannels;
        int32_t bitRate;
        int32_t sampleRate;
        int32_t encodeOptions;
        int32_t blockAlign;
        int32_t bitspersample;
        int32_t formattag;
        int32_t advencopt1;
        int32_t advencopt2;
        int32_t VirtualPktSize;
        if (version==kTypeWMAPro || version==kTypeWMALossLess) {
            CHECK(msg->findInt32(getMsgKey(kKeyWMABitspersample), &bitspersample));
            CHECK(msg->findInt32(getMsgKey(kKeyWMAFormatTag), &formattag));
            CHECK(msg->findInt32(getMsgKey(kKeyWMAAdvEncOpt1), &advencopt1));
            CHECK(msg->findInt32(getMsgKey(kKeyWMAAdvEncOpt2), &advencopt2));
            CHECK(msg->findInt32(getMsgKey(kKeyWMAVirPktSize), &VirtualPktSize));
        }
        if (version==kTypeWMA) {
            InitOMXParams(&paramWMA);
            paramWMA.nPortIndex = kPortIndexInput;
        } else if (version==kTypeWMAPro || version==kTypeWMALossLess) {
            InitOMXParams(&paramWMA10);
            paramWMA10.nPortIndex = kPortIndexInput;
        }
        CHECK(msg->findInt32("channel-count", &numChannels));
        CHECK(msg->findInt32("sample-rate", &sampleRate));
        CHECK(msg->findInt32(getMsgKey(kKeyBitRate), &bitRate));
        if (!msg->findInt32(getMsgKey(kKeyWMAEncodeOpt), &encodeOptions)) {
            ALOGE("Unsupported encode options");
            return ERROR_UNSUPPORTED;
        }
        CHECK(msg->findInt32(getMsgKey(kKeyWMABlockAlign), &blockAlign));
        ALOGV("Channels: %d, SampleRate: %d, BitRate; %d"
                   "EncodeOptions: %d, blockAlign: %d", numChannels,
                   sampleRate, bitRate, encodeOptions, blockAlign);
        if (sampleRate>48000 || numChannels>2)
        {
            ALOGE("Unsupported samplerate/channels");
            return ERROR_UNSUPPORTED;
        }
        if (version==kTypeWMAPro || version==kTypeWMALossLess)
        {
            ALOGV("Bitspersample: %d, wmaformattag: %d,"
                       "advencopt1: %d, advencopt2: %d VirtualPktSize %d", bitspersample,
                       formattag, advencopt1, advencopt2, VirtualPktSize);
        }
        status_t err = OK;
        OMX_INDEXTYPE index;
        if (version==kTypeWMA) {
            err = OMXhandle->getParameter(
                   nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
        } else if (version==kTypeWMAPro || version==kTypeWMALossLess) {
            OMXhandle->getExtensionIndex(nodeID,"OMX.Qualcomm.index.audio.wma10Pro",&index);
            err = OMXhandle->getParameter(
                   nodeID, index, &paramWMA10, sizeof(paramWMA10));
        }
        CHECK_EQ(err, (status_t)OK);
        if (version==kTypeWMA) {
            paramWMA.nChannels = numChannels;
            paramWMA.nSamplingRate = sampleRate;
            paramWMA.nEncodeOptions = encodeOptions;
            paramWMA.nBitRate = bitRate;
            paramWMA.nBlockAlign = blockAlign;
        } else if (version==kTypeWMAPro || version==kTypeWMALossLess) {
            paramWMA10.nChannels = numChannels;
            paramWMA10.nSamplingRate = sampleRate;
            paramWMA10.nEncodeOptions = encodeOptions;
            paramWMA10.nBitRate = bitRate;
            paramWMA10.nBlockAlign = blockAlign;
        }
        if (version==kTypeWMAPro || version==kTypeWMALossLess) {
            paramWMA10.advancedEncodeOpt = advencopt1;
            paramWMA10.advancedEncodeOpt2 = advencopt2;
            paramWMA10.formatTag = formattag;
            paramWMA10.validBitsPerSample = bitspersample;
            paramWMA10.nVirtualPktSize = VirtualPktSize;
        }
        if (version==kTypeWMA) {
            err = OMXhandle->setParameter(
                  nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
        } else if (version==kTypeWMAPro || version==kTypeWMALossLess) {
           err = OMXhandle->setParameter(
                 nodeID, index, &paramWMA10, sizeof(paramWMA10));
        }
        return err;
    }
    return OK;
}

void ExtendedCodec::setAC3Format(
        int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID) {
    QOMX_AUDIO_PARAM_AC3TYPE profileAC3;
    QOMX_AUDIO_PARAM_AC3PP profileAC3PP;
    OMX_INDEXTYPE indexTypeAC3;
    OMX_INDEXTYPE indexTypeAC3PP;
    OMX_PARAM_PORTDEFINITIONTYPE portParam;

    //configure input port
    ALOGV("setAC3Format samplerate %d, numChannels %d", sampleRate, numChannels);
    InitOMXParams(&portParam);
    portParam.nPortIndex = 0;
    status_t err = OMXhandle->getParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);
    err = OMXhandle->setParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);

    //configure output port
    portParam.nPortIndex = 1;
    err = OMXhandle->getParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);
    err = OMXhandle->setParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);

    err = OMXhandle->getExtensionIndex(nodeID, OMX_QCOM_INDEX_PARAM_AC3TYPE, &indexTypeAC3);

    InitOMXParams(&profileAC3);
    profileAC3.nPortIndex = kPortIndexInput;
    err = OMXhandle->getParameter(nodeID, indexTypeAC3, &profileAC3, sizeof(profileAC3));
    CHECK_EQ(err,(status_t)OK);

    profileAC3.nSamplingRate  =  sampleRate;
    profileAC3.nChannels      =  2;
    profileAC3.eChannelConfig =  OMX_AUDIO_AC3_CHANNEL_CONFIG_2_0;

    ALOGV("numChannels = %d, profileAC3.nChannels = %d", numChannels, profileAC3.nChannels);

    err = OMXhandle->setParameter(nodeID, indexTypeAC3, &profileAC3, sizeof(profileAC3));
    CHECK_EQ(err,(status_t)OK);

    //for output port
    OMX_AUDIO_PARAM_PCMMODETYPE profilePcm;
    InitOMXParams(&profilePcm);
    profilePcm.nPortIndex = kPortIndexOutput;
    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioPcm, &profilePcm, sizeof(profilePcm));
    CHECK_EQ(err, (status_t)OK);

    profilePcm.nSamplingRate  =  sampleRate;
    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioPcm, &profilePcm, sizeof(profilePcm));
    CHECK_EQ(err, (status_t)OK);
    OMXhandle->getExtensionIndex(nodeID, OMX_QCOM_INDEX_PARAM_AC3PP, &indexTypeAC3PP);

    InitOMXParams(&profileAC3PP);
    profileAC3PP.nPortIndex = kPortIndexInput;
    err = OMXhandle->getParameter(
            nodeID, indexTypeAC3PP, &profileAC3PP, sizeof(profileAC3PP));
    CHECK_EQ(err, (status_t)OK);

    int i;
    int channel_routing[6] = {0};

    for (i=0; i<6; i++) {
        channel_routing[i] = -1;
    }
    for (i=0; i<6; i++) {
        profileAC3PP.eChannelRouting[i] =  (OMX_AUDIO_AC3_CHANNEL_ROUTING)channel_routing[i];
    }

    profileAC3PP.eChannelRouting[0] =  OMX_AUDIO_AC3_CHANNEL_LEFT;
    profileAC3PP.eChannelRouting[1] =  OMX_AUDIO_AC3_CHANNEL_RIGHT;
    err = OMXhandle->setParameter(nodeID, indexTypeAC3PP, &profileAC3PP, sizeof(profileAC3PP));
    CHECK_EQ(err, (status_t)OK);
}

status_t ExtendedCodec::setAMRWBPLUSFormat(
        int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID) {

    QOMX_AUDIO_PARAM_AMRWBPLUSTYPE profileAMRWBPlus;
    OMX_INDEXTYPE indexTypeAMRWBPlus;
    OMX_PARAM_PORTDEFINITIONTYPE portParam;

    ALOGV("AMRWB+ setformat sampleRate:%d numChannels:%d",sampleRate,numChannels);

    //configure input port
    InitOMXParams(&portParam);
    portParam.nPortIndex = kPortIndexInput;
    status_t err = OMXhandle->getParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);
    err = OMXhandle->setParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);

    //configure output port
    portParam.nPortIndex = kPortIndexOutput;
    err = OMXhandle->getParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);
    err = OMXhandle->setParameter(
       nodeID, OMX_IndexParamPortDefinition, &portParam, sizeof(portParam));
    CHECK_EQ(err, (status_t)OK);

    err = OMXhandle->getExtensionIndex(nodeID, OMX_QCOM_INDEX_PARAM_AMRWBPLUS, &indexTypeAMRWBPlus);

    //for input port
    InitOMXParams(&profileAMRWBPlus);
    profileAMRWBPlus.nPortIndex = kPortIndexInput;
    err = OMXhandle->getParameter(nodeID, indexTypeAMRWBPlus, &profileAMRWBPlus, sizeof(profileAMRWBPlus));
    CHECK_EQ(err,(status_t)OK);

    profileAMRWBPlus.nSampleRate = sampleRate;
    profileAMRWBPlus.nChannels = numChannels;
    err = OMXhandle->setParameter(nodeID, indexTypeAMRWBPlus, &profileAMRWBPlus, sizeof(profileAMRWBPlus));
    CHECK_EQ(err,(status_t)OK);

    //for output port
    OMX_AUDIO_PARAM_PCMMODETYPE profilePcm;
    InitOMXParams(&profilePcm);
    profilePcm.nPortIndex = kPortIndexOutput;
    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioPcm, &profilePcm, sizeof(profilePcm));
    CHECK_EQ(err, (status_t)OK);

    profilePcm.nSamplingRate = sampleRate;
    profilePcm.nChannels = numChannels;
    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioPcm, &profilePcm, sizeof(profilePcm));
    CHECK_EQ(err, (status_t)OK);

    return err;
}

bool ExtendedCodec::useHWAACDecoder(const char *mime) {
    char value[PROPERTY_VALUE_MAX] = {0};
    int aaccodectype = 0;
    aaccodectype = property_get("media.aaccodectype", value, NULL);
    if (aaccodectype && !strncmp("0", value, 1) &&
        !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
        ALOGI("Using Hardware AAC Decoder");
        return true;
    }
    return false;
}

bool ExtendedCodec::isSourcePauseRequired(const char *componentName) {
    /* pause is required for hardware component to release adsp resources */
    if (!strncmp(componentName, "OMX.qcom.", 9)) {
        return true;
    }
    return false;
}

} //namespace android

#else //ENABLE_AV_ENHANCEMENTS

namespace android {
    status_t ExtendedCodec::convertMetaDataToMessage(
            const sp<MetaData> &meta, sp<AMessage> *format) {
        ARG_TOUCH(meta);
        ARG_TOUCH(format);
        return OK;
    }

    uint32_t ExtendedCodec::getComponentQuirks (
            const sp<MediaCodecInfo> &info) {
        ARG_TOUCH(info);
        return 0;
    }

    status_t ExtendedCodec::setDIVXFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID, int port_index) {
        ARG_TOUCH(msg);
        ARG_TOUCH(mime);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(port_index);
        return OK;
    }

    status_t ExtendedCodec::setAudioFormat(
            const sp<MetaData> &meta, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder) {
        ARG_TOUCH(meta);
        ARG_TOUCH(mime);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(isEncoder);
        return ERROR_UNSUPPORTED;
    }

    status_t ExtendedCodec::setAudioFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder) {
        ARG_TOUCH(msg);
        ARG_TOUCH(mime);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(isEncoder);
        return ERROR_UNSUPPORTED;
    }

    status_t ExtendedCodec::setVideoFormat(
            const char *mime,
            OMX_VIDEO_CODINGTYPE *compressionFormat) {
        ARG_TOUCH(mime);
        ARG_TOUCH(compressionFormat);
        return ERROR_UNSUPPORTED;
    }

    status_t ExtendedCodec::getSupportedAudioFormatInfo(
            const AString* mime,
            sp<IOMX> OMXhandle,
            IOMX::node_id nodeID,
            int portIndex,
            int* channelCount,
            int* sampleRate) {
        ARG_TOUCH(mime);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(portIndex);
        ARG_TOUCH(channelCount);
        return OK;
    }

    status_t ExtendedCodec::handleSupportedAudioFormats(
            int format, AString* meta) {
        ARG_TOUCH(format);
        ARG_TOUCH(meta);
        return UNKNOWN_ERROR;
    }

    bool ExtendedCodec::checkIfCompressionHEVC(int format) {
        ARG_TOUCH(format);
        return false;
    }

    status_t ExtendedCodec::handleSupportedVideoFormats(
            int format, AString* meta) {
        ARG_TOUCH(meta);
        ARG_TOUCH(format);
        return UNKNOWN_ERROR;
    }

    status_t ExtendedCodec::setupHEVCEncoderParameters(
            const sp<MetaData> &meta, const sp<IOMX> &omx,
            IOMX::node_id node, const char* componentName,
            int portIndex, const sp<OMXCodec> &target) {
        ARG_TOUCH(meta);
        ARG_TOUCH(omx);
        ARG_TOUCH(node);
        ARG_TOUCH(componentName);
        ARG_TOUCH(portIndex);
        ARG_TOUCH(target);
        return UNKNOWN_ERROR;
    }

    const char* ExtendedCodec::overrideComponentName (
            uint32_t quirks, const sp<MetaData> &meta, const char *mime, bool isEncoder) {
        ARG_TOUCH(quirks);
        ARG_TOUCH(meta);
        ARG_TOUCH(mime);
        ARG_TOUCH(isEncoder);
        return NULL;
    }

    void ExtendedCodec::overrideComponentName(
            uint32_t quirks, const sp<AMessage> &msg,
            AString* componentName, AString* mime,
            int32_t isEncoder) {
        ARG_TOUCH(quirks);
        ARG_TOUCH(msg);
        ARG_TOUCH(componentName);
        ARG_TOUCH(mime);
        ARG_TOUCH(isEncoder);
    }

    void ExtendedCodec::overrideMimeType(
        const sp<AMessage> &msg, AString* mime) {
        ARG_TOUCH(msg);
        ARG_TOUCH(mime);
    }

    void ExtendedCodec::getRawCodecSpecificData(
        const sp<MetaData> &meta, const void* &data, size_t &size) {
        ARG_TOUCH(meta);
        ARG_TOUCH(data);
        size = 0;
    }

    sp<ABuffer> ExtendedCodec::getRawCodecSpecificData(
            const sp<AMessage> &msg) {
        ARG_TOUCH(msg);
        return NULL;
    }

    void ExtendedCodec::getAacCodecSpecificData(
            const sp<MetaData> &meta,
            const void* &data,
            size_t& size) {
        ARG_TOUCH(meta);
        ARG_TOUCH(data);
        size = 0;
        return;
    }

    sp<ABuffer> ExtendedCodec::getAacCodecSpecificData(
            const sp<AMessage> &msg) {
        ARG_TOUCH(msg);
        return NULL;
    }


    status_t ExtendedCodec::setSupportedRole(
            const sp<IOMX> &omx, IOMX::node_id node,
            bool isEncoder, const char *mime) {
        ARG_TOUCH(omx);
        ARG_TOUCH(node);
        ARG_TOUCH(isEncoder);
        ARG_TOUCH(mime);
        return BAD_VALUE;
    }

    status_t ExtendedCodec::setWMAFormat(
            const sp<MetaData> &meta, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, bool isEncoder) {
        ARG_TOUCH(meta);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(isEncoder);
        return OK;
    }

    status_t ExtendedCodec::setWMAFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, bool isEncoder) {
        ARG_TOUCH(msg);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(isEncoder);
        return OK;
    }

    void ExtendedCodec::setEVRCFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder) {
        ARG_TOUCH(numChannels);
        ARG_TOUCH(sampleRate);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(isEncoder);
    }

    void ExtendedCodec::setQCELPFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder) {
        ARG_TOUCH(numChannels);
        ARG_TOUCH(sampleRate);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(isEncoder);
    }

    void ExtendedCodec::setAC3Format(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID) {
        ARG_TOUCH(numChannels);
        ARG_TOUCH(sampleRate);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
    }

    status_t ExtendedCodec::setAMRWBPLUSFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID) {
        ARG_TOUCH(numChannels);
        ARG_TOUCH(sampleRate);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        return OK;
    }

    void ExtendedCodec::configureFramePackingFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, const char* componentName) {
        ARG_TOUCH(msg);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(componentName);
    }

    void ExtendedCodec::configureFramePackingFormat(
            const sp<MetaData> &meta, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, const char* componentName) {
        ARG_TOUCH(meta);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(componentName);
    }

    void ExtendedCodec::configureVideoDecoder(
        const sp<MetaData> &meta, const char* mime, sp<IOMX> OMXhandle,
        const uint32_t flags, IOMX::node_id nodeID, const char* componentName) {
        ARG_TOUCH(meta);
        ARG_TOUCH(mime);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(flags);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(componentName);
    }

    void ExtendedCodec::configureVideoDecoder(
        const sp<AMessage> &msg, const char* mime,  sp<IOMX> OMXhandle,
        const uint32_t flags, IOMX::node_id nodeID, const char* componentName) {
        ARG_TOUCH(msg);
        ARG_TOUCH(mime);
        ARG_TOUCH(OMXhandle);
        ARG_TOUCH(flags);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(componentName);
    }

    bool ExtendedCodec::useHWAACDecoder(const char *mime) {
        ARG_TOUCH(mime);
        return false;
    }

    void ExtendedCodec::enableSmoothStreaming(
            const sp<IOMX> &omx, IOMX::node_id nodeID, bool* isEnabled,
            const char* componentName) {
        ARG_TOUCH(omx);
        ARG_TOUCH(nodeID);
        ARG_TOUCH(componentName);
        *isEnabled = false;
        return;
    }

    bool ExtendedCodec::isSourcePauseRequired(const char *componentName) {
        return false;
    }
} //namespace android

#endif //ENABLE_AV_ENHANCEMENTS
