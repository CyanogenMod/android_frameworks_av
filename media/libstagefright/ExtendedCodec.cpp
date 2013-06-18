/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
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

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>

#include <media/stagefright/MetaData.h>
#include <media/stagefright/ExtendedCodec.h>
#include <media/stagefright/OMXCodec.h>

#ifdef ENABLE_QC_AV_ENHANCEMENTS

#include <QCMetaData.h>
#include <QCMediaDefs.h>
#include <OMX_QCOMExtns.h>
#include <OMX_Component.h>
#include <QOMX_AudioExtensions.h>

namespace android {

uint32_t ExtendedCodec::getComponentQuirks(
        const MediaCodecList *list, size_t index) {
    uint32_t quirks = 0;

    if (list->codecHasQuirk(
                index, "requires-wma-pro-component")) {
        quirks |= kRequiresWMAProComponent;
    }
    return quirks;
}

const char* ExtendedCodec::overrideComponentName(
        uint32_t quirks, const sp<MetaData> &meta) {
    const char* componentName = NULL;
    if(quirks & kRequiresWMAProComponent)
    {
       int32_t version = 0;
       CHECK(meta->findInt32(kKeyWMAVersion, &version));
       if(version==kTypeWMA) {
          componentName = "OMX.qcom.audio.decoder.wma";
       } else if(version==kTypeWMAPro) {
          componentName = "OMX.qcom.audio.decoder.wma10Pro";
       } else if(version==kTypeWMALossLess) {
          componentName = "OMX.qcom.audio.decoder.wmaLossLess";
       }
    }
    return componentName;
}

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

status_t ExtendedCodec::configureDIVXCodec(
        const sp<MetaData> &meta, char* mime, sp<IOMX> OMXhandle,
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

void ExtendedCodec::getRawCodecSpecificData(
        const sp<MetaData> &meta, const void* &data, size_t &size) {
    uint32_t type = 0;
    size = 0;
    if (meta->findData(kKeyRawCodecSpecificData, &type, &data, &size)) {
        ALOGV("OMXCodec::configureCodec found kKeyRawCodecSpecificData of size %d\n", size);
    }
}

status_t ExtendedCodec::setAudioFormat(
        const sp<MetaData> &meta, char* mime, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setAudioFormat called");
    status_t err = OK;

    if ((!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mime)) ||
        (!strcasecmp(MEDIA_MIMETYPE_AUDIO_EAC3, mime))){
        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        /* Commenting follwoing call as AC3 soft decoder does not
         need it and it causes issue with playback*/
        //setAC3Format(numChannels, sampleRate, OMXhandle, nodeID);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_EVRC, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        setEVRCFormat(numChannels, sampleRate, OMXhandle, nodeID, isEncoder );
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_QCELP, mime)) {
        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
        setQCELPFormat(numChannels, sampleRate, OMXhandle, nodeID, isEncoder);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mime))  {
        err = setWMAFormat(meta, OMXhandle, nodeID, isEncoder);
    }
    return err;
}

status_t ExtendedCodec::setVideoInputFormat(
        const char *mime, OMX_VIDEO_CODINGTYPE *compressionFormat) {
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

status_t ExtendedCodec::setVideoOutputFormat(
        const char *mime, OMX_VIDEO_CODINGTYPE *compressionFormat) {
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

void ExtendedCodec::setSupportedRole(
        const sp<IOMX> &omx, IOMX::node_id node,
        bool isEncoder, const char *mime){
    ALOGV("setSupportedRole Called");
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

status_t ExtendedCodec::handleSupportedAudioFormats(int format, AString* meta) {
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

void ExtendedCodec::configureVideoCodec(
        const sp<MetaData> &meta, sp<IOMX> OMXhandle,
        const uint32_t flags, IOMX::node_id nodeID, char* componentName ) {
    if (strncmp(componentName, "OMX.qcom.", 9)) {
        //do nothing for non QC component
        return;
    }

    int32_t arbitraryMode = 1;
    bool success = meta->findInt32(kKeyUseArbitraryMode, &arbitraryMode);
    bool useArbitraryMode = true;
    if (success) {
        useArbitraryMode = arbitraryMode ? true : false;
    }

    if (useArbitraryMode) {
        ALOGI("Decoder should be in arbitrary mode");
    } else{
        ALOGI("Enable frame by frame mode");
        OMX_QCOM_PARAM_PORTDEFINITIONTYPE portFmt;
        portFmt.nPortIndex = kPortIndexInput;
        portFmt.nFramePackingFormat = OMX_QCOM_FramePacking_OnlyOneCompleteFrame;
        status_t err = OMXhandle->setParameter(
        nodeID, (OMX_INDEXTYPE)OMX_QcomIndexPortDefn, (void *)&portFmt, sizeof(portFmt));
        if(err != OK) {
            ALOGW("Failed to set frame packing format on component");
        }
    }

    // Enable timestamp reordering only for AVI/mpeg4 and vc1 clips
    const char *fileFormat;
    success = meta->findCString(kKeyFileFormat, &fileFormat);
    if (!strcmp(componentName, "OMX.qcom.video.decoder.vc1") ||
        (success && !strncmp(fileFormat, "video/avi", 9))) {
        ALOGI("Enabling timestamp reordering");
        QOMX_INDEXTIMESTAMPREORDER reorder;
        InitOMXParams(&reorder);
        reorder.nPortIndex = kPortIndexOutput;
        reorder.bEnable = OMX_TRUE;
        status_t err = OMXhandle->setParameter(nodeID,
                       (OMX_INDEXTYPE)OMX_QcomIndexParamEnableTimeStampReorder,
                       (void *)&reorder, sizeof(reorder));

        if(err != OK) {
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
        if(err != OK) {
            ALOGW("Failed to get extension for SYNCFRAMEDECODINGMODE");
            return;
        }

        enableType.bEnable = OMX_TRUE;
        err = OMXhandle->setParameter(nodeID,indexType,
                   (void *)&enableType, sizeof(enableType));
        if(err != OK) {
            ALOGW("Failed to get extension for SYNCFRAMEDECODINGMODE");
            return;
        }
        ALOGI("Thumbnail mode enabled.");
    }
}

//private methods
void ExtendedCodec::setEVRCFormat(
        int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {
    ALOGV("setEVRCFormat called");

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
    }
    else{
      ALOGI("EVRC decoder \n");
    }
}

void ExtendedCodec::setQCELPFormat(
        int32_t numChannels, int32_t sampleRate, sp<IOMX> OMXhandle,
        IOMX::node_id nodeID, bool isEncoder ) {

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
            paramWMA.nPortIndex = kPortIndexInput;
        } else if(version==kTypeWMAPro || version==kTypeWMALossLess) {
            InitOMXParams(&paramWMA10);
            paramWMA10.nPortIndex = kPortIndexInput;
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

} //namespace android

#else //ENABLE_QC_AV_ENHANCEMENTS

namespace android {

    uint32_t ExtendedCodec::getComponentQuirks (
            const MediaCodecList *list, size_t index) {
        return 0;
    }

    status_t ExtendedCodec::configureDIVXCodec(
            const sp<MetaData> &meta, char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID, int port_index) {
        return OK;
    }

    status_t ExtendedCodec::setAudioFormat(
            const sp<MetaData> &meta, char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder) {
        return OK;
    }

    status_t ExtendedCodec::setVideoInputFormat(
            const char *mime,
            OMX_VIDEO_CODINGTYPE *compressionFormat) {
        return OK;
    }

    status_t ExtendedCodec::setVideoOutputFormat(
            const char *mime,
            OMX_VIDEO_CODINGTYPE *compressionFormat) {
        return OK;
    }

    status_t ExtendedCodec::handleSupportedAudioFormats(
            int format, AString* meta) {
        return UNKNOWN_ERROR;
    }

    const char* ExtendedCodec::overrideComponentName (
            uint32_t quirks, const sp<MetaData> &meta) {
        return NULL;
    }

    void ExtendedCodec::getRawCodecSpecificData(
        const sp<MetaData> &meta, const void* &data, size_t &size) {
            size = 0;
    }

    void ExtendedCodec::setSupportedRole(
            const sp<IOMX> &omx, IOMX::node_id node,
            bool isEncoder,const char *mime) {
    }

    status_t ExtendedCodec::setWMAFormat(
            const sp<MetaData> &meta, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, bool isEncoder ) {
        return OK;
    }

    void ExtendedCodec::setEVRCFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder ) {
    }

    void ExtendedCodec::setQCELPFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder ) {
    }

    void ExtendedCodec::setAC3Format(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID) {
    }

    void ExtendedCodec::configureVideoCodec(
        const sp<MetaData> &meta, sp<IOMX> OMXhandle,
        const uint32_t flags, IOMX::node_id nodeID, char* componentName ) {
    }

} //namespace android

#endif //ENABLE_QC_AV_ENHANCEMENTS
