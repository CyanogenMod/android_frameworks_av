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

#ifndef EXTENDED_CODEC_H_
#define EXTENDED_CODEC_H_

#include <android/native_window.h>
#include <media/IOMX.h>
#include <media/MediaCodecInfo.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/threads.h>

#include <OMX_Audio.h>

namespace android {

struct MediaCodecList;
struct OMXCodec;

enum{
    kRequiresWMAProComponent = 0x40000000,
};


struct ExtendedCodec {

    enum {
        kPortIndexInput  = 0,
        kPortIndexOutput = 1
    };
    static status_t convertMetaDataToMessage(
            const sp<MetaData> &meta, sp<AMessage> *format);

    static uint32_t getComponentQuirks (
            const sp<MediaCodecInfo> &info);

    static status_t setAudioFormat(
            const sp<MetaData> &meta, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder);

    static status_t setAudioFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID,
            bool isEncoder);

    static status_t setVideoFormat(
            const sp<MetaData> &meta, const char *mime,
            OMX_VIDEO_CODINGTYPE *compressionFormat);

    static status_t setVideoFormat(
            const sp<AMessage> &msg, const char *mime,
            OMX_VIDEO_CODINGTYPE *compressionFormat);

    static status_t getSupportedAudioFormatInfo(
            const AString* mime,
            sp<IOMX> OMXhandle,
            IOMX::node_id nodeID,
            int portIndex,
            int* channelCount,
            int* sampleRate);

    static status_t handleSupportedAudioFormats(
            int format, AString* mime);

    static status_t handleSupportedVideoFormats(
            int format, AString* mime);

    static bool checkIfCompressionHEVC(int format);

    static status_t setupHEVCEncoderParameters(
            const sp<MetaData> &meta, const sp<IOMX> &omx,
            IOMX::node_id node, const char* componentName,
            int portIndex, const sp<OMXCodec> &target);

    static const char* overrideComponentName(
            uint32_t quirks, const sp<MetaData> &meta,
            const char *mime, bool isEncoder);

    static void overrideComponentName(
            uint32_t quirks, const sp<AMessage> &msg,
            AString* componentName, AString* mime,
            int32_t isEncoder);

    static void overrideMimeType(
        const sp<AMessage> &msg, AString* mime);

    static void getRawCodecSpecificData(
            const sp<MetaData> &meta,
            const void* &data,
            size_t& size);

    static sp<ABuffer> getRawCodecSpecificData(
            const sp<AMessage> &msg);

    static void getAacCodecSpecificData(
            const sp<MetaData> &meta,
            const void* &data,
            size_t& size);

    static sp<ABuffer> getAacCodecSpecificData(
            const sp<AMessage> &msg);

    static status_t setSupportedRole(
            const sp<IOMX> &omx, IOMX::node_id node,
            bool isEncoder, const char *mime);

    static void configureFramePackingFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, const char* componentName);

    static void configureFramePackingFormat(
            const sp<MetaData> &meta, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, const char* componentName);

    static void configureVideoDecoder(
            const sp<MetaData> &meta, const char* mime,
            sp<IOMX> OMXhandle, const uint32_t flags,
            IOMX::node_id nodeID, const char* componentName);

    static void configureVideoDecoder(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle, const uint32_t flags,
            IOMX::node_id nodeID, const char* componentName);

    static void enableSmoothStreaming(
            const sp<IOMX> &omx, IOMX::node_id nodeID, bool* isEnabled,
            const char* componentName);

    static bool useHWAACDecoder(const char *mime);

    static bool isSourcePauseRequired(const char *componentName);

    static const char* getMsgKey(int key );

private:

    static status_t setWMAFormat(
            const sp<MetaData> &meta, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, bool isEncoder );

    static status_t setWMAFormat(
            const sp<AMessage> &msg, sp<IOMX> OMXhandle,
            IOMX::node_id nodeID, bool isEncoder );

    static void setEVRCFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder );

    static void setQCELPFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID,
            bool isEncoder );

#ifdef QCOM_ADDITIONAL_CODECS
    static void setAC3Format(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID);
#endif

    static status_t setDIVXFormat(
            const sp<AMessage> &msg, const char* mime,
            sp<IOMX> OMXhandle,IOMX::node_id nodeID, int port_index);

    static status_t setAMRWBPLUSFormat(
            int32_t numChannels, int32_t sampleRate,
            sp<IOMX> OMXhandle, IOMX::node_id nodeID);

};

}
#endif /*EXTENDED_CODEC_H_ */

