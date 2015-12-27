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
#ifndef FFMPEG_SOFT_CODEC_H_
#define FFMPEG_SOFT_CODEC_H_

#include <media/IOMX.h>
#include <media/MediaCodecInfo.h>

#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>

#include <media/stagefright/MetaData.h>

#include <OMX_Audio.h>
#include <OMX_Video.h>

namespace android {

struct FFMPEGSoftCodec {

    enum {
        kPortIndexInput  = 0,
        kPortIndexOutput = 1
    };

    static void convertMessageToMetaDataFF(
            const sp<AMessage> &msg, sp<MetaData> &meta);

    static void convertMetaDataToMessageFF(
        const sp<MetaData> &meta, sp<AMessage> *format);

    static const char* overrideComponentName(
            uint32_t quirks, const sp<MetaData> &meta,
            const char *mime, bool isEncoder);

    static void overrideComponentName(
            uint32_t quirks, const sp<AMessage> &msg,
            AString* componentName, AString* mime,
            int32_t isEncoder);

    static status_t setSupportedRole(
            const sp<IOMX> &omx, IOMX::node_id node,
            bool isEncoder, const char *mime);

    static status_t setAudioFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID);

    static status_t setVideoFormat(
            status_t status,
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder, OMX_VIDEO_CODINGTYPE *compressionFormat,
            const char* componentName);

    static status_t getAudioPortFormat(
            OMX_U32 portIndex, int coding,
            sp<AMessage> &notify, sp<IOMX> OMXhandle, IOMX::node_id nodeID);

    static status_t getVideoPortFormat(
            OMX_U32 portIndex, int coding,
            sp<AMessage> &notify, sp<IOMX> OMXhandle, IOMX::node_id nodeID);

private:
    static const char* getMsgKey(int key);

    static status_t setWMVFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setRVFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setFFmpegVideoFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setRawAudioFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setWMAFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setVORBISFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setRAFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setFLACFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setMP2Format(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setAC3Format(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setAPEFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setDTSFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

    static status_t setFFmpegAudioFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID);

#ifdef QCOM_HARDWARE
    static status_t setQCDIVXFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID, int port_index);
#endif

};

}
#endif
