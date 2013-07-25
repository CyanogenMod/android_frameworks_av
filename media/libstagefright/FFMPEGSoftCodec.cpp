/*
 * Copyright (C) 2014 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "FFMPEGSoftCodec"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ABitReader.h>

#include <media/stagefright/FFMPEGSoftCodec.h>

#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>

#include <OMX_Component.h>

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
   {kKeyWMAVersion           , "wma-version"            , INT32},  // int32_t
   {kKeyWMVVersion           , "wmv-version"            , INT32},
   {kKeyWMAEncodeOpt         , "wma-encode-opt"         , INT32},  // int32_t
   {kKeyWMABlockAlign        , "wma-block-align"        , INT32},  // int32_t
   {kKeyRVVersion            , "rv-version"             , INT32},
   {kKeyWidth                , "width"                  , INT32},
   {kKeyHeight               , "height"                 , INT32},
   {kKeyCodecId              , "codec-id"               , INT32},
   {kKeyBitspersample        , "sample-bits"            , INT32},
   {kKeyBlockAlign           , "block-align"            , INT32},
   {kKeySampleFormat         , "sample-format"          , INT32},
   {kKeySampleRate           , "sample-rate"            , INT32},
   {kKeyChannelCount         , "channel-count"          , INT32},
};

const char* FFMPEGSoftCodec::getMsgKey(int key) {
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

status_t FFMPEGSoftCodec::convertMetaDataToMessage(
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

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

status_t FFMPEGSoftCodec::handleSupportedAudioFormats(int format, AString* mime) {
    ALOGV("handleSupportedAudioFormats called for format:%x",format);
    status_t retVal = OK;
    if (format == OMX_AUDIO_CodingVORBIS ) {
        *mime = MEDIA_MIMETYPE_AUDIO_VORBIS;
    } else if (format == OMX_AUDIO_CodingRA ) {
        *mime = MEDIA_MIMETYPE_AUDIO_RA;
    } else if (format == OMX_AUDIO_CodingFLAC) {
        *mime = MEDIA_MIMETYPE_AUDIO_FLAC;
    } else if (format == OMX_AUDIO_CodingMP2) {
        *mime = MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II;
    } else if (format == OMX_AUDIO_CodingWMA ) {
        *mime = MEDIA_MIMETYPE_AUDIO_WMA;
    } else if (format == OMX_AUDIO_CodingAC3 ) {
        *mime = MEDIA_MIMETYPE_AUDIO_AC3;
    } else if (format == OMX_AUDIO_CodingAPE) {
        *mime = MEDIA_MIMETYPE_AUDIO_APE;
    } else if (format == OMX_AUDIO_CodingDTS) {
        *mime = MEDIA_MIMETYPE_AUDIO_DTS;
    } else if (format == OMX_AUDIO_CodingAutoDetect) {
        *mime = MEDIA_MIMETYPE_AUDIO_FFMPEG;
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}

status_t FFMPEGSoftCodec::handleSupportedVideoFormats(int format, AString* mime) {
    ALOGV("handleSupportedVideoFormats called");
    status_t retVal = OK;
    if (format == OMX_VIDEO_CodingWMV) {
        *mime = MEDIA_MIMETYPE_VIDEO_WMV;
    } else if (format == OMX_VIDEO_CodingRV) {
        *mime = MEDIA_MIMETYPE_VIDEO_RV;
    } else if (format == OMX_VIDEO_CodingVC1) {
        *mime = MEDIA_MIMETYPE_VIDEO_VC1;
    } else if (format == OMX_VIDEO_CodingFLV1) {
        *mime = MEDIA_MIMETYPE_VIDEO_FLV1;
    } else if (format == OMX_VIDEO_CodingDIVX) {
        *mime = MEDIA_MIMETYPE_VIDEO_DIVX;
    } else if (format == OMX_VIDEO_CodingHEVC) {
        *mime = MEDIA_MIMETYPE_VIDEO_HEVC;
    } else if (format == OMX_VIDEO_CodingAutoDetect) {
        *mime = MEDIA_MIMETYPE_VIDEO_FFMPEG;
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}

status_t FFMPEGSoftCodec::setVideoFormat(
        const sp<MetaData> &meta, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder,
        OMX_VIDEO_CODINGTYPE *compressionFormat) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    return setVideoFormat(msg, mime, OMXhandle, nodeID, isEncoder, compressionFormat);
}

status_t FFMPEGSoftCodec::setVideoFormat(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder,
        OMX_VIDEO_CODINGTYPE *compressionFormat) {
    status_t err = OK;

    if (isEncoder) {
        ALOGE("Encoding not supported");
        err = BAD_VALUE;
    
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)) {
        err = setWMVFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setWMVFormat() failed (err = %d)", err);
        } else {
            *compressionFormat = OMX_VIDEO_CodingWMV;
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_RV, mime)) {
        err = setRVFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setRVFormat() failed (err = %d)", err);
        } else {
            *compressionFormat = OMX_VIDEO_CodingRV;
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VC1, mime)) {
        *compressionFormat = OMX_VIDEO_CodingVC1;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_FLV1, mime)) {
        *compressionFormat = OMX_VIDEO_CodingFLV1;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)) {
        *compressionFormat = OMX_VIDEO_CodingDIVX;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) {
        *compressionFormat = OMX_VIDEO_CodingHEVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_FFMPEG, mime)) {
        ALOGV("Setting the OMX_VIDEO_PARAM_FFMPEGTYPE params");
        err = setFFmpegVideoFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setFFmpegVideoFormat() failed (err = %d)", err);
        } else {
            *compressionFormat = OMX_VIDEO_CodingAutoDetect;
        }
    } else {
        err = BAD_VALUE;
    }

    return err;
}

status_t FFMPEGSoftCodec::setAudioFormat(
        const sp<MetaData> &meta, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    sp<AMessage> msg = new AMessage();
    msg->clear();
    convertMetaDataToMessage(meta, &msg);
    return setAudioFormat(msg, mime, OMXhandle, nodeID, isEncoder);
}

status_t FFMPEGSoftCodec::setAudioFormat(
        const sp<AMessage> &msg, const char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setAudioFormat called");
    status_t err = OK;

    ALOGV("setAudioFormat: %s", msg->debugString(0).c_str());

    if (isEncoder) {
        ALOGE("Encoding not supported");
        err = BAD_VALUE;

    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mime))  {
        err = setWMAFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setWMAFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mime))  {
        err = setVORBISFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setVORBISFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RA, mime))  {
        err = setRAFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setRAFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FLAC, mime))  {
        err = setFLACFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setFLACFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II, mime))  {
        err = setMP2Format(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setMP2Format() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mime)) {
        err = setAC3Format(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setAC3Format() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_APE, mime))  {
        err = setAPEFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setAPEFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_DTS, mime))  {
        err = setDTSFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setDTSFormat() failed (err = %d)", err);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FFMPEG, mime))  {
        err = setFFmpegAudioFormat(msg, OMXhandle, nodeID);
        if (err != OK) {
            ALOGE("setFFmpegAudioFormat() failed (err = %d)", err);
        }
    } else {
        err = BAD_VALUE;
    }

    return err;
}

status_t FFMPEGSoftCodec::setSupportedRole(
        const sp<IOMX> &omx, IOMX::node_id node,
        bool isEncoder, const char *mime) {

    ALOGV("setSupportedRole Called %s", mime);

    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kFFMPEGMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_AAC,
          "audio_decoder.aac", NULL },
        { MEDIA_MIMETYPE_AUDIO_MPEG,
          "audio_decoder.mp3", NULL },
        { MEDIA_MIMETYPE_AUDIO_VORBIS,
          "audio_decoder.vorbis", NULL },
        { MEDIA_MIMETYPE_AUDIO_WMA,
          "audio_decoder.wma", NULL },
        { MEDIA_MIMETYPE_AUDIO_RA,
          "audio_decoder.ra" , NULL },
        { MEDIA_MIMETYPE_AUDIO_FLAC,
          "audio_decoder.flac", NULL },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II,
          "audio_decoder.mp2", NULL },
        { MEDIA_MIMETYPE_AUDIO_AC3,
          "audio_decoder.ac3", NULL },
        { MEDIA_MIMETYPE_AUDIO_APE,
          "audio_decoder.ape", NULL },
        { MEDIA_MIMETYPE_AUDIO_DTS,
          "audio_decoder.dts", NULL },
        { MEDIA_MIMETYPE_VIDEO_MPEG2,
          "video_decoder.mpeg2v", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX4,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX311,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_WMV,
          "video_decoder.vc1",  NULL },
        { MEDIA_MIMETYPE_VIDEO_RV,
          "video_decoder.rv", NULL },
        { MEDIA_MIMETYPE_VIDEO_FLV1,
          "video_decoder.flv1", NULL },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
          "video_decoder.hevc", NULL },
        };
    static const size_t kNumMimeToRole =
                     sizeof(kFFMPEGMimeToRole) / sizeof(kFFMPEGMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kFFMPEGMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
        return ERROR_UNSUPPORTED;
    }

    const char *role =
        isEncoder ? kFFMPEGMimeToRole[i].encoderRole
                  : kFFMPEGMimeToRole[i].decoderRole;

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

//video
status_t FFMPEGSoftCodec::setWMVFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = 0;
    OMX_VIDEO_PARAM_WMVTYPE paramWMV;

    CHECK(msg->findInt32(getMsgKey(kKeyWMVVersion), &version));

    InitOMXParams(&paramWMV);
    paramWMV.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    if (err != OK)
        return err;

    if (version == kTypeWMVVer_7) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat7;
    } else if (version == kTypeWMVVer_8) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat8;
    } else if (version == kTypeWMVVer_9) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat9;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    return err;
}

status_t FFMPEGSoftCodec::setRVFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = 0;
    OMX_VIDEO_PARAM_RVTYPE paramRV;

    CHECK(msg->findInt32(getMsgKey(kKeyRVVersion), &version));

    InitOMXParams(&paramRV);
    paramRV.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamVideoRv, &paramRV, sizeof(paramRV));
    if (err != OK)
        return err;

    if (version == kTypeRVVer_G2) {
        paramRV.eFormat = OMX_VIDEO_RVFormatG2;
    } else if (version == kTypeRVVer_8) {
        paramRV.eFormat = OMX_VIDEO_RVFormat8;
    } else if (version == kTypeRVVer_9) {
        paramRV.eFormat = OMX_VIDEO_RVFormat9;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamVideoRv, &paramRV, sizeof(paramRV));
    return err;
}

status_t FFMPEGSoftCodec::setFFmpegVideoFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t codec_id = 0;
    int32_t width = 0;
    int32_t height = 0;
    OMX_VIDEO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegVideoFormat");

    CHECK(msg->findInt32(getMsgKey(kKeyCodecId), &codec_id));
    CHECK(msg->findInt32(getMsgKey(kKeyWidth), &width));
    CHECK(msg->findInt32(getMsgKey(kKeyHeight), &height));

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamVideoFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId = codec_id;
    param.nWidth   = width;
    param.nHeight  = height;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamVideoFFmpeg, &param, sizeof(param));
    return err;
}

//audio
status_t FFMPEGSoftCodec::setWMAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t formattag = 0;
    OMX_AUDIO_PARAM_WMATYPE paramWMA;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBitRate), &bitRate));
    if (!msg->findInt32(getMsgKey(kKeyBlockAlign), &blockAlign)) {
        // we should be last on the codec list, but another sniffer may
        // have handled it and there is no hardware codec.
        if (!msg->findInt32(getMsgKey(kKeyWMABlockAlign), &blockAlign)) {
            return ERROR_UNSUPPORTED;
        }
    }

    ALOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    CHECK(msg->findInt32(getMsgKey(kKeyWMAVersion), &version));

    InitOMXParams(&paramWMA);
    paramWMA.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
    if (err != OK)
        return err;

    paramWMA.nChannels = numChannels;
    paramWMA.nSamplingRate = sampleRate;
    paramWMA.nBitRate = bitRate;
    paramWMA.nBlockAlign = blockAlign;

    // http://msdn.microsoft.com/en-us/library/ff819498(v=vs.85).aspx
    if (version == kTypeWMA) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat7;
    } else if (version == kTypeWMAPro) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat8;
    } else if (version == kTypeWMALossLess) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat9;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
    return err;
}

status_t FFMPEGSoftCodec::setVORBISFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_VORBISTYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioVorbis, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioVorbis, &param, sizeof(param));
    return err;
}

status_t FFMPEGSoftCodec::setRAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    OMX_AUDIO_PARAM_RATYPE paramRA;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBitRate), &bitRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBlockAlign), &blockAlign));

    ALOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    InitOMXParams(&paramRA);
    paramRA.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
    if (err != OK)
        return err;

    paramRA.eFormat = OMX_AUDIO_RAFormatUnused; // FIXME, cook only???
    paramRA.nChannels = numChannels;
    paramRA.nSamplingRate = sampleRate;
    // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
    // the cook audio codec need blockAlign!
    paramRA.nNumRegions = blockAlign;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
    return err;
}

status_t FFMPEGSoftCodec::setFLACFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_FLACTYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    //CHECK(meta->findInt32(kKeyBitspersample, &bitsPerSample));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioFlac, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;
    //param.nBitsPerSample = bitsPerSample;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioFlac, &param, sizeof(param));
    return err;
}

status_t FFMPEGSoftCodec::setMP2Format(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_MP2TYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioMp2, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioMp2, &param, sizeof(param));
    return err;
}

status_t FFMPEGSoftCodec::setAC3Format(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_AC3TYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioAc3, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioAc3, &param, sizeof(param));
    return err;
}

status_t FFMPEGSoftCodec::setAPEFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_APETYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBitspersample), &bitsPerSample));

    ALOGV("Channels:%d, SampleRate:%d, bitsPerSample:%d",
            numChannels, sampleRate, bitsPerSample);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioApe, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;
    param.nBitsPerSample = bitsPerSample;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioApe, &param, sizeof(param));
    return err;
}

status_t FFMPEGSoftCodec::setDTSFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_DTSTYPE param;

    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioDts, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioDts, &param, sizeof(param));
    return err;
}

status_t FFMPEGSoftCodec::setFFmpegAudioFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t codec_id = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t bitsPerSample = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t sampleFormat = 0;
    OMX_AUDIO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegAudioFormat");

    CHECK(msg->findInt32(getMsgKey(kKeyCodecId), &codec_id));
    CHECK(msg->findInt32(getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(getMsgKey(kKeyBitRate), &bitRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBitspersample), &bitsPerSample));
    CHECK(msg->findInt32(getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(getMsgKey(kKeyBlockAlign), &blockAlign));
    CHECK(msg->findInt32(getMsgKey(kKeySampleFormat), &sampleFormat));

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId       = codec_id;
    param.nChannels      = numChannels;
    param.nBitRate       = bitRate;
    param.nBitsPerSample = bitsPerSample;
    param.nSampleRate    = sampleRate;
    param.nBlockAlign    = blockAlign;
    param.eSampleFormat  = sampleFormat;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
    return err;
}

}
