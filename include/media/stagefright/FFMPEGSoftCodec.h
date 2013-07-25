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

struct MediaCodecList;
struct OMXCodec;

struct FFMPEGSoftCodec {

    enum {
        kPortIndexInput  = 0,
        kPortIndexOutput = 1
    };
    static status_t convertMetaDataToMessage(
            const sp<MetaData> &meta, sp<AMessage> *format);

    static status_t setSupportedRole(
            const sp<IOMX> &omx, IOMX::node_id node,
            bool isEncoder, const char *mime);

    static status_t setAudioFormat(
            const sp<MetaData> &meta, const char* mime,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder);

    static status_t setAudioFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder);

    static status_t setVideoFormat(
            const sp<MetaData> &meta, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder, OMX_VIDEO_CODINGTYPE *compressionFormat);

    static status_t setVideoFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder, OMX_VIDEO_CODINGTYPE *compressionFormat);

    static status_t handleSupportedAudioFormats(
            int format, AString* mime);

    static status_t handleSupportedVideoFormats(
            int format, AString* mime);

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

};

}
#endif
