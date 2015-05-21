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

#include <media/stagefright/ExtendedCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/Utils.h>

#include <OMX_Component.h>
#include <OMX_AudioExt.h>
#include <OMX_IndexExt.h>

namespace android {

void FFMPEGSoftCodec::convertMessageToMetaData(
    const sp<AMessage> &msg, sp<MetaData> &meta) {

    int32_t blockAlign;
    if (msg->findInt32("block-align", &blockAlign)) {
        meta->setInt32(kKeyBlockAlign, blockAlign);
    }

    int32_t rvVersion;
    if (msg->findInt32("rv-version", &rvVersion)) {
        meta->setInt32(kKeyRVVersion, rvVersion);
    }

    int32_t wmvVersion;
    if (msg->findInt32("wmv-version", &wmvVersion)) {
        meta->setInt32(kKeyWMVVersion, wmvVersion);
    }

    int32_t bitrate;
    if (msg->findInt32("bitrate", &bitrate)) {
        meta->setInt32(kKeyBitRate, bitrate);
    }

    int32_t codedSampleBits;
    if (msg->findInt32("coded-sample-bits", &codedSampleBits)) {
        meta->setInt32(kKeyCodedSampleBits, codedSampleBits);
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

const char* FFMPEGSoftCodec::overrideComponentName(
        uint32_t /*quirks*/, const sp<MetaData> &meta, const char *mime, bool isEncoder) {
    const char* componentName = NULL;

    int32_t wmvVersion = 0;
    if (!strncasecmp(mime, MEDIA_MIMETYPE_VIDEO_WMV, strlen(MEDIA_MIMETYPE_VIDEO_WMV)) &&
            meta->findInt32(kKeyWMVVersion, &wmvVersion)) {
        ALOGD("Found WMV version key %d", wmvVersion);
        if (wmvVersion == 1) {
            ALOGD("Use FFMPEG for unsupported WMV track");
            componentName = "OMX.ffmpeg.wmv.decoder";
        }
    }

    int32_t encodeOptions = 0;
    if (!isEncoder && !strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMA, strlen(MEDIA_MIMETYPE_AUDIO_WMA)) &&
            !meta->findInt32(kKeyWMAEncodeOpt, &encodeOptions)) {
        ALOGD("Use FFMPEG for unsupported WMA track");
        componentName = "OMX.ffmpeg.wma.decoder";
    }

    // Google's decoder doesn't support MAIN profile
    int32_t aacProfile = 0;
    if (!isEncoder && !strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC, strlen(MEDIA_MIMETYPE_AUDIO_AAC)) &&
            meta->findInt32(kKeyAACAOT, &aacProfile)) {
        if (aacProfile == OMX_AUDIO_AACObjectMain) {
            ALOGD("Use FFMPEG for AAC MAIN profile");
            componentName = "OMX.ffmpeg.aac.decoder";
        }
    }
    
    return componentName;
}

void FFMPEGSoftCodec::overrideComponentName(
        uint32_t /*quirks*/, const sp<AMessage> &msg, AString* componentName, AString* mime, int32_t isEncoder) {

    int32_t wmvVersion = 0;
    if (!strncasecmp(mime->c_str(), MEDIA_MIMETYPE_VIDEO_WMV, strlen(MEDIA_MIMETYPE_VIDEO_WMV)) &&
            msg->findInt32(ExtendedCodec::getMsgKey(kKeyWMVVersion), &wmvVersion)) {
        ALOGD("Found WMV version key %d", wmvVersion);
        if (wmvVersion == 1) {
            ALOGD("Use FFMPEG for unsupported WMV track");
            componentName->setTo("OMX.ffmpeg.wmv.decoder");
        }
    }

    int32_t encodeOptions = 0;
    if (!isEncoder && !strncasecmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_WMA, strlen(MEDIA_MIMETYPE_AUDIO_WMA)) &&
            !msg->findInt32(ExtendedCodec::getMsgKey(kKeyWMAEncodeOpt), &encodeOptions)) {
        ALOGD("Use FFMPEG for unsupported WMA track");
        componentName->setTo("OMX.ffmpeg.wma.decoder");
    }

    // Google's decoder doesn't support MAIN profile
    int32_t aacProfile = 0;
    if (!isEncoder && !strncasecmp(mime->c_str(), MEDIA_MIMETYPE_AUDIO_AAC, strlen(MEDIA_MIMETYPE_AUDIO_AAC)) &&
            msg->findInt32(ExtendedCodec::getMsgKey(kKeyAACAOT), &aacProfile)) {
        if (aacProfile == OMX_AUDIO_AACObjectMain) {
            ALOGD("Use FFMPEG for AAC MAIN profile");
            componentName->setTo("OMX.ffmpeg.aac.decoder");
        }
    }
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
    } else if (format == OMX_AUDIO_CodingAC3 || format == OMX_AUDIO_CodingAndroidAC3) {
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
    }

    ALOGV("setAudioFormat: %s", msg->debugString(0).c_str());

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
          "video_decoder.mpeg2", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX4,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX311,
          "video_decoder.divx", NULL },
        { MEDIA_MIMETYPE_VIDEO_WMV,
          "video_decoder.wmv",  NULL },
        { MEDIA_MIMETYPE_VIDEO_VC1,
          "video_decoder.vc1", NULL },
        { MEDIA_MIMETYPE_VIDEO_RV,
          "video_decoder.rv", NULL },
        { MEDIA_MIMETYPE_VIDEO_FLV1,
          "video_decoder.flv1", NULL },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
          "video_decoder.hevc", NULL },
        { MEDIA_MIMETYPE_AUDIO_FFMPEG,
          "audio_decoder.trial", NULL },
        { MEDIA_MIMETYPE_VIDEO_FFMPEG,
          "video_decoder.trial", NULL },
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
    int32_t version = -1;
    OMX_VIDEO_PARAM_WMVTYPE paramWMV;

    if (!msg->findInt32(ExtendedCodec::getMsgKey(kKeyWMVVersion), &version)) {
        ALOGE("WMV version not detected");
    }

    InitOMXParams(&paramWMV);
    paramWMV.nPortIndex = kPortIndexInput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    if (err != OK) {
        return err;
    }

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
    int32_t version = kTypeRVVer_G2;
    OMX_VIDEO_PARAM_RVTYPE paramRV;

    if (!msg->findInt32(ExtendedCodec::getMsgKey(kKeyRVVersion), &version)) {
        ALOGE("RV version not detected");
    }

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

    if (msg->findInt32(ExtendedCodec::getMsgKey(kKeyWidth), &width)) {
        ALOGE("No video width specified");
    }
    if (msg->findInt32(ExtendedCodec::getMsgKey(kKeyHeight), &height)) {
        ALOGE("No video height specified");
    }
    if (!msg->findInt32(ExtendedCodec::getMsgKey(kKeyCodecId), &codec_id)) {
        ALOGE("No codec id sent for FFMPEG catch-all codec!");
    }

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
status_t FFMPEGSoftCodec::setRawAudioFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));
    if (!msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitsPerSample), &bitsPerSample)) {
        ALOGD("No PCM format specified, let decoder decide");
    }

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    OMX_AUDIO_PARAM_PCMMODETYPE pcmParams;
    InitOMXParams(&pcmParams);
    pcmParams.nPortIndex = kPortIndexOutput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    if (err != OK) {
        return err;
    }

    pcmParams.nChannels = numChannels;
    pcmParams.eNumData = OMX_NumericalDataSigned;
    pcmParams.bInterleaved = OMX_TRUE;
    if (bitsPerSample > 0) {
        pcmParams.nBitPerSample = bitsPerSample;
    }
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    if (getOMXChannelMapping(numChannels, pcmParams.eChannelMapping) != OK) {
        return OMX_ErrorNone;
    }

    err = OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    if (err != OK) {
        return err;
    }

    msg->setInt32("bits-per-sample", pcmParams.nBitPerSample);

    return OK;
}

status_t FFMPEGSoftCodec::setWMAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t version = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t formattag = 0;
    int32_t bitsPerSample = 0;

    OMX_AUDIO_PARAM_WMATYPE paramWMA;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitRate), &bitRate));
    if (!msg->findInt32(ExtendedCodec::getMsgKey(kKeyBlockAlign), &blockAlign)) {
        // we should be last on the codec list, but another sniffer may
        // have handled it and there is no hardware codec.
        if (!msg->findInt32(ExtendedCodec::getMsgKey(kKeyWMABlockAlign), &blockAlign)) {
            return ERROR_UNSUPPORTED;
        }
    }

    // mm-parser may want a different bit depth
    if (msg->findInt32(ExtendedCodec::getMsgKey(kKeyWMABitspersample), &bitsPerSample)) {
        msg->setInt32("bits-per-sample", bitsPerSample);
    }

    ALOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyWMAVersion), &version));

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&paramWMA);
    paramWMA.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
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

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
}

status_t FFMPEGSoftCodec::setVORBISFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_VORBISTYPE param;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioVorbis, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioVorbis, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setRAFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    OMX_AUDIO_PARAM_RATYPE paramRA;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));
    msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitRate), &bitRate);
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyBlockAlign), &blockAlign));

    ALOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&paramRA);
    paramRA.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
    if (err != OK)
        return err;

    paramRA.eFormat = OMX_AUDIO_RAFormatUnused; // FIXME, cook only???
    paramRA.nChannels = numChannels;
    paramRA.nSamplingRate = sampleRate;
    // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
    // the cook audio codec need blockAlign!
    paramRA.nNumRegions = blockAlign;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
}

status_t FFMPEGSoftCodec::setFLACFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_FLACTYPE param;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitsPerSample), &bitsPerSample));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioFlac, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioFlac, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setMP2Format(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_MP2TYPE param;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioMp2, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioMp2, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setAC3Format(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_ANDROID_AC3TYPE param;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, (OMX_INDEXTYPE)OMX_IndexParamAudioAndroidAc3, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setAPEFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_APETYPE param;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitsPerSample), &bitsPerSample));

    ALOGV("Channels:%d, SampleRate:%d, bitsPerSample:%d",
            numChannels, sampleRate, bitsPerSample);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioApe, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;
    param.nBitsPerSample = bitsPerSample;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioApe, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setDTSFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_DTSTYPE param;

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate));

    ALOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioDts, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioDts, &param, sizeof(param));
}

status_t FFMPEGSoftCodec::setFFmpegAudioFormat(
        const sp<AMessage> &msg, sp<IOMX> OMXhandle, IOMX::node_id nodeID)
{
    int32_t codec_id = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t bitsPerSample = 16;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t sampleFormat = 0;
    int32_t codedSampleBits = 0;
    OMX_AUDIO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegAudioFormat");

    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyCodecId), &codec_id));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeyChannelCount), &numChannels));
    CHECK(msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleFormat), &sampleFormat));
    msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitRate), &bitRate);
    msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitsPerSample), &bitsPerSample);
    msg->findInt32(ExtendedCodec::getMsgKey(kKeySampleRate), &sampleRate);
    msg->findInt32(ExtendedCodec::getMsgKey(kKeyBlockAlign), &blockAlign);
    msg->findInt32(ExtendedCodec::getMsgKey(kKeyBitsPerSample), &bitsPerSample);
    msg->findInt32(ExtendedCodec::getMsgKey(kKeyCodedSampleBits), &codedSampleBits);

    status_t err = setRawAudioFormat(msg, OMXhandle, nodeID);
    if (err != OK)
        return err;

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    err = OMXhandle->getParameter(
            nodeID, OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId       = codec_id;
    param.nChannels      = numChannels;
    param.nBitRate       = bitRate;
    param.nBitsPerSample = codedSampleBits;
    param.nSampleRate    = sampleRate;
    param.nBlockAlign    = blockAlign;
    param.eSampleFormat  = sampleFormat;

    return OMXhandle->setParameter(
            nodeID, OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
}

}
