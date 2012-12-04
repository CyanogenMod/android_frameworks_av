/*
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
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
#define LOG_TAG "QCOMXCodec"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>

#include <media/stagefright/MetaData.h>
#include <media/stagefright/QCOMXCodec.h>
#include <media/stagefright/OMXCodec.h>
#include <QCMetaData.h>
#include <QCMediaDefs.h>
#include <OMX_QCOMExtns.h>

#include <OMX_Component.h>
#include <QOMX_AudioExtensions.h>


namespace android {

uint32_t QCOMXCodec::getQCComponentQuirks(const MediaCodecList *list, size_t index) {
    uint32_t quirks = 0;

    if (list->codecHasQuirk(
                index, "requires-wma-pro-component")) {
        quirks |= kRequiresWMAProComponent;
    }
    return quirks;
}

void QCOMXCodec::setASFQuirks(uint32_t quirks, const sp<MetaData> &meta, const char* componentName) {
    if(quirks & kRequiresWMAProComponent)
    {
       int32_t version;
       CHECK(meta->findInt32(kKeyWMAVersion, &version));
       if(version==kTypeWMA) {
          componentName = "OMX.qcom.audio.decoder.wma";
       } else if(version==kTypeWMAPro) {
          componentName= "OMX.qcom.audio.decoder.wma10Pro";
       } else if(version==kTypeWMALossLess) {
          componentName= "OMX.qcom.audio.decoder.wmaLossLess";
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


status_t QCOMXCodec::configureDIVXCodec(const sp<MetaData> &meta, char* mime, sp<IOMX> OMXhandle, IOMX::node_id nodeID, int port_index) {
    status_t err = OK;
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime) ||
        !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime) ||
        !strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
        ALOGV("Setting the QOMX_VIDEO_PARAM_DIVXTYPE params ");
        QOMX_VIDEO_PARAM_DIVXTYPE paramDivX;
        InitOMXParams(&paramDivX);
        paramDivX.nPortIndex = port_index;
        int32_t DivxVersion = 0;
        CHECK(meta->findInt32(kKeyDivXVersion,&DivxVersion));
        ALOGV("Divx Version Type %d\n",DivxVersion);

        if(DivxVersion == kTypeDivXVer_4) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat4;
        } else if(DivxVersion == kTypeDivXVer_5) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat5;
        } else if(DivxVersion == kTypeDivXVer_6) {
            paramDivX.eFormat = QOMX_VIDEO_DIVXFormat6;
        } else if(DivxVersion == kTypeDivXVer_3_11 ) {
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

void QCOMXCodec::checkAndAddRawFormat(OMXCodec *handle, const sp<MetaData> &meta){
    uint32_t type;
    const void *data;
    size_t size;

    if (meta->findData(kKeyRawCodecSpecificData, &type, &data, &size)) {
        ALOGV("OMXCodec::configureCodec found kKeyRawCodecSpecificData of size %d\n", size);
        handle->addCodecSpecificData(data, size);
    }

}

status_t QCOMXCodec::setQCFormat(const sp<MetaData> &meta, char* mime, sp<IOMX> OMXhandle,
                                     IOMX::node_id nodeID, OMXCodec *handle, bool isEncoder ) {
    ALOGV("setQCFormat -- called ");
    status_t err = OK;
    if ((!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mime)) ||
        (!strcasecmp(MEDIA_MIMETYPE_AUDIO_EAC3, mime))){
        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        setAC3Format(numChannels, sampleRate, OMXhandle, nodeID);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_EVRC, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        setEVRCFormat(numChannels, sampleRate, OMXhandle, nodeID, handle,isEncoder );
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_QCELP, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        setQCELPFormat(numChannels, sampleRate, OMXhandle, nodeID, handle,isEncoder);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mime))  {
        err = setWMAFormat(meta, OMXhandle, nodeID, isEncoder);
    }
    return err;
}


void QCOMXCodec::setEVRCFormat(int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
                                    IOMX::node_id nodeID, OMXCodec *handle,  bool isEncoder ) {
    ALOGV("setEVRCFormat -- called ");
    if (isEncoder) {
        CHECK(numChannels == 1);
        //////////////// input port ////////////////////
        handle->setRawAudioFormat(OMXCodec::kPortIndexInput, sampleRate, numChannels);
        //////////////// output port ////////////////////
        // format
        OMX_AUDIO_PARAM_PORTFORMATTYPE format;
        format.nPortIndex = OMXCodec::kPortIndexOutput;
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
        def.nPortIndex = OMXCodec::kPortIndexOutput;
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
        profile.nPortIndex = OMXCodec::kPortIndexOutput;
        CHECK_EQ(OMXhandle->getParameter(nodeID, OMX_IndexParamAudioEvrc,
                &profile, sizeof(profile)), (status_t)OK);
        profile.nChannels = numChannels;
        CHECK_EQ(OMXhandle->setParameter(nodeID, OMX_IndexParamAudioEvrc,
                &profile, sizeof(profile)), (status_t)OK);
    }
    else{
      ALOGI("EVRC decoder \n");
    }
}


void QCOMXCodec::setQCELPFormat(int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
                                     IOMX::node_id nodeID, OMXCodec *handle, bool isEncoder ) {
    if (isEncoder) {
        CHECK(numChannels == 1);
        //////////////// input port ////////////////////
        handle->setRawAudioFormat(OMXCodec::kPortIndexInput, sampleRate, numChannels);
        //////////////// output port ////////////////////
        // format
        OMX_AUDIO_PARAM_PORTFORMATTYPE format;
        format.nPortIndex = OMXCodec::kPortIndexOutput;
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
        def.nPortIndex = OMXCodec::kPortIndexOutput;
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
        profile.nPortIndex = OMXCodec::kPortIndexOutput;
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

status_t QCOMXCodec::setWMAFormat(const sp<MetaData> &meta, sp<IOMX> OMXhandle,
                                       IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setWMAFormat Called");
    if (isEncoder) {
        ALOGE("WMA encoding not supported");
        return OK;
    } else {
        int32_t version;
        OMX_AUDIO_PARAM_WMATYPE paramWMA;
        QOMX_AUDIO_PARAM_WMA10PROTYPE paramWMA10;
        CHECK(meta->findInt32(kKeyWMAVersion, &version));
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
        if(version==kTypeWMAPro || version==kTypeWMALossLess) {
            CHECK(meta->findInt32(kKeyWMABitspersample, &bitspersample));
            CHECK(meta->findInt32(kKeyWMAFormatTag, &formattag));
            CHECK(meta->findInt32(kKeyWMAAdvEncOpt1,&advencopt1));
            CHECK(meta->findInt32(kKeyWMAAdvEncOpt2,&advencopt2));
            CHECK(meta->findInt32(kKeyWMAVirPktSize,&VirtualPktSize));
        }
        if(version==kTypeWMA) {
            InitOMXParams(&paramWMA);
            paramWMA.nPortIndex = OMXCodec::kPortIndexInput;
        } else if(version==kTypeWMAPro || version==kTypeWMALossLess) {
            InitOMXParams(&paramWMA10);
            paramWMA10.nPortIndex = OMXCodec::kPortIndexInput;
        }
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        CHECK(meta->findInt32(kKeyBitRate, &bitRate));
        CHECK(meta->findInt32(kKeyWMAEncodeOpt, &encodeOptions));
        CHECK(meta->findInt32(kKeyWMABlockAlign, &blockAlign));
        ALOGV("Channels: %d, SampleRate: %d, BitRate; %d"
                   "EncodeOptions: %d, blockAlign: %d", numChannels,
                   sampleRate, bitRate, encodeOptions, blockAlign);
        if(sampleRate>48000 || numChannels>2)
        {
            ALOGE("Unsupported samplerate/channels");
            return ERROR_UNSUPPORTED;
        }
        if(version==kTypeWMAPro || version==kTypeWMALossLess)
        {
            ALOGV("Bitspersample: %d, wmaformattag: %d,"
                       "advencopt1: %d, advencopt2: %d VirtualPktSize %d", bitspersample,
                       formattag, advencopt1, advencopt2, VirtualPktSize);
        }
        status_t err = OK;
        OMX_INDEXTYPE index;
        if(version==kTypeWMA) {
            err = OMXhandle->getParameter(
                   nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
        } else if(version==kTypeWMAPro || version==kTypeWMALossLess) {
            OMXhandle->getExtensionIndex(nodeID,"OMX.Qualcomm.index.audio.wma10Pro",&index);
            err = OMXhandle->getParameter(
                   nodeID, index, &paramWMA10, sizeof(paramWMA10));
        }
        CHECK_EQ(err, (status_t)OK);
        if(version==kTypeWMA) {
            paramWMA.nChannels = numChannels;
            paramWMA.nSamplingRate = sampleRate;
            paramWMA.nEncodeOptions = encodeOptions;
            paramWMA.nBitRate = bitRate;
            paramWMA.nBlockAlign = blockAlign;
        } else if(version==kTypeWMAPro || version==kTypeWMALossLess) {
            paramWMA10.nChannels = numChannels;
            paramWMA10.nSamplingRate = sampleRate;
            paramWMA10.nEncodeOptions = encodeOptions;
            paramWMA10.nBitRate = bitRate;
            paramWMA10.nBlockAlign = blockAlign;
        }
        if(version==kTypeWMAPro || version==kTypeWMALossLess) {
            paramWMA10.advancedEncodeOpt = advencopt1;
            paramWMA10.advancedEncodeOpt2 = advencopt2;
            paramWMA10.formatTag = formattag;
            paramWMA10.validBitsPerSample = bitspersample;
            paramWMA10.nVirtualPktSize = VirtualPktSize;
        }
        if(version==kTypeWMA) {
            err = OMXhandle->setParameter(
                  nodeID, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
        } else if(version==kTypeWMAPro || version==kTypeWMALossLess) {
           err = OMXhandle->setParameter(
                 nodeID, index, &paramWMA10, sizeof(paramWMA10));
        }
        return err;
    }
    return OK;
}


void QCOMXCodec::setAC3Format(int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
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
    profileAC3.nPortIndex = OMXCodec::kPortIndexInput;
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
    profilePcm.nPortIndex = OMXCodec::kPortIndexOutput;
    err = OMXhandle->getParameter(nodeID, OMX_IndexParamAudioPcm, &profilePcm, sizeof(profilePcm));
    CHECK_EQ(err, (status_t)OK);

    profilePcm.nSamplingRate  =  sampleRate;
    err = OMXhandle->setParameter(nodeID, OMX_IndexParamAudioPcm, &profilePcm, sizeof(profilePcm));
    CHECK_EQ(err, (status_t)OK);
    OMXhandle->getExtensionIndex(nodeID, OMX_QCOM_INDEX_PARAM_AC3PP, &indexTypeAC3PP);

    InitOMXParams(&profileAC3PP);
    profileAC3PP.nPortIndex = OMXCodec::kPortIndexInput;
    err = OMXhandle->getParameter(nodeID, indexTypeAC3PP, &profileAC3PP, sizeof(profileAC3PP));
    CHECK_EQ(err, (status_t)OK);

    int i;
    int channel_routing[6];

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


status_t QCOMXCodec::setQCVideoInputFormat(const char *mime, OMX_VIDEO_CODINGTYPE *compressionFormat) {
    status_t retVal = OK;
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)){
        *compressionFormat= (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime)){
        *compressionFormat= (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)){
        *compressionFormat= (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)){
        *compressionFormat = OMX_VIDEO_CodingWMV;
    } else if (!strcasecmp(MEDIA_MIMETYPE_CONTAINER_MPEG2, mime)){
        *compressionFormat = OMX_VIDEO_CodingMPEG2;
    } else {
        retVal = BAD_VALUE;
    }

    return retVal;
}

status_t QCOMXCodec::setQCVideoOutputFormat(const char *mime, OMX_VIDEO_CODINGTYPE *compressionFormat) {
    status_t retVal = OK;
    if(!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)) {
        *compressionFormat = (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if(!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX311, mime)) {
        *compressionFormat = (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if(!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX4, mime)) {
        *compressionFormat = (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingDivx;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)){
        *compressionFormat = OMX_VIDEO_CodingWMV;
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}


void QCOMXCodec::checkQCRole( const sp<IOMX> &omx, IOMX::node_id node,
                                  bool isEncoder, const char *mime){
    ALOGV("checkQCRole Called");
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
        { MEDIA_MIMETYPE_AUDIO_AC3,
          "audio_decoder.ac3", NULL },
        { MEDIA_MIMETYPE_VIDEO_DIVX311,
          "video_decoder.divx", NULL },
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
        return;
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
        }
    }

}

status_t QCOMXCodec::checkQCFormats(int format, AString* meta){
    ALOGV("checkQCFormats called");
    status_t retVal = OK;
    if (format == OMX_AUDIO_CodingQCELP13 ) {
        *meta = MEDIA_MIMETYPE_AUDIO_QCELP;
    } else if(format == OMX_AUDIO_CodingEVRC ) {
        *meta = MEDIA_MIMETYPE_AUDIO_EVRC;
    } else {
        retVal = BAD_VALUE;
    }
    return retVal;
}

}
