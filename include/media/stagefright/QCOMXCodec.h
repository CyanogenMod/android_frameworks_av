/*
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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

#ifndef QC_OMX_CODEC_H_

#define QC_OMX_CODEC_H_

#include <android/native_window.h>
#include <media/IOMX.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/threads.h>

#include <OMX_Audio.h>

namespace android {

struct MediaCodecList;
struct OMXCodec;

enum{
    kRequiresWMAProComponent = 0x40000000,
};


struct QCOMXCodec {

    static uint32_t getQCComponentQuirks(const MediaCodecList *list, size_t index);

    static status_t configureDIVXCodec(const sp<MetaData> &meta, char* mime,
                          sp<IOMX> OMXhandle,IOMX::node_id nodeID, int port_index);

    static status_t setQCFormat(const sp<MetaData> &meta, char* mime,
                                sp<IOMX> OMXhandle,IOMX::node_id nodeID,
                                       OMXCodec *handle, bool isEncoder);

    static status_t setWMAFormat(const sp<MetaData> &meta, sp<IOMX> OMXhandle,
                                        IOMX::node_id nodeID, bool isEncoder );

    static status_t setQCVideoInputFormat(const char *mime,
                                          OMX_VIDEO_CODINGTYPE *compressionFormat);

    static status_t setQCVideoOutputFormat(const char *mime,
                                           OMX_VIDEO_CODINGTYPE *compressionFormat);

    static status_t checkQCFormats(int format, AString* meta);

    static void     setASFQuirks(uint32_t quirks, const sp<MetaData> &meta,
                                                 const char* componentName);

    static void     checkAndAddRawFormat(OMXCodec *handle, const sp<MetaData> &meta);

    static void     setEVRCFormat(int32_t numChannels, int32_t sampleRate,
                                  sp<IOMX> OMXhandle, IOMX::node_id nodeID,
                                        OMXCodec *handle,  bool isEncoder );

    static void     setQCELPFormat(int32_t numChannels, int32_t sampleRate,
                                   sp<IOMX> OMXhandle, IOMX::node_id nodeID,
                                         OMXCodec *handle,  bool isEncoder );

    static void     setAC3Format(int32_t numChannels, int32_t sampleRate,
                                 sp<IOMX> OMXhandle, IOMX::node_id nodeID);

    static void     checkQCRole(const sp<IOMX> &omx, IOMX::node_id node,
                                        bool isEncoder,const char *mime);

    static void     setQCSpecificVideoFormat(const sp<MetaData> &meta, sp<IOMX> OMXhandle,
                                               IOMX::node_id nodeID, char* componentName );

    static void     checkIfInterlaced(const uint8_t *ptr, const sp<MetaData> &meta);

    static bool     useHWAACDecoder(const char *mime);

};

}
#endif /*QC_OMX_CODEC_H_ */

