/*Copyright (c) 2013, The Linux Foundation. All rights reserved.
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

#ifndef QC_UTIL_CLASS_H_
#define QC_UTIL_CLASS_H_

#include <QCMetaData.h>
#include <cutils/properties.h>
#include <QCMediaDefs.h>

#include <media/Metadata.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/MediaCodecList.h>

#include <utils/Errors.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>
#include <utils/StrongPointer.h>

#include <media/MediaRecorderBase.h>
#include <camera/CameraParameters.h>

#include <OMX_Video.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>

namespace android {

struct QCUtilityClass
{
    // helper function to enable Stagefright Recoder to recalculate fileduration
    // when hfr property is set
    static status_t helper_StageFrightRecoder_hfr(sp<MetaData> &meta, sp<MetaData> &enc_meta,
                                                  int64_t &maxFileDurationUs, int32_t frameRate,
                                                  video_encoder videoEncoder);

    // helper function to enable camera source to set kKeyHFR when video-hfr is enabled
    static void  helper_CameraSource_hfr(const CameraParameters& params, sp<MetaData> &meta);

    // helper function to enable MPEG4Writer to compute timestamp when hfr is enable
    static void  helper_MPEG4Writer_hfr(sp<MetaData> &meta, int64_t &timestampUs);

    // helper function to enable OMXCodec to recalculate frameRate, bitrate when hfr is enable
    static void  helper_OMXCodec_hfr(const sp<MetaData> &meta, int32_t &frameRate,
                                     int32_t &bitRate, int32_t &newFrameRate);

    // helper function to enable OMXCodec to set HFR and FrameRate on output format when
    // present on input format
    static void  helper_OMXCodec_hfr(const sp<MetaData> &inputFormat, sp<MetaData> &outputFormat);

    // helper function to disable audio when decode audio disable prop is set
    static bool  helper_Awesomeplayer_checkIfAudioDisable();

    // helper function to disable audio when encode audio disable prop is set
    static bool  helper_StagefrightRecoder_checkIfAudioDisable();

    //helper function to set encoding profiles
    static void  helper_StagefrightRecoder_setUserprofile(video_encoder &videoEncoder,
                                                                    int32_t &videoEncoderProfile);
    //helper function to setBframe related info for MPEG4type
    static void helper_OMXCodec_setBFrames(OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type, bool &numBFrames);

    //helper function to setBframe related info for H264 type
    static void helper_OMXCodec_setBFrames(OMX_VIDEO_PARAM_AVCTYPE &h264type, bool &numBFrames,
                                           int32_t iFramesInterval, int32_t frameRate);

    //helper function to add media codecs with specific quirks
    static void helper_addMediaCodec(Vector<MediaCodecList::CodecInfo> &mCodecInfos,
                                     KeyedVector<AString, size_t> &mTypes,
                                     bool encoder, const char *name,
                                     const char *type, uint32_t quirks);

    //helper function to calculate the value of quirks from strings
    static uint32_t helper_getCodecSpecificQuirks(KeyedVector<AString, size_t> &mCodecQuirks,
                                                  Vector<AString> quirks);
    static sp<MediaExtractor> helper_MediaExtractor_CreateIfNeeded(sp<MediaExtractor> defaultExt,
                                                                    const sp<DataSource> &source,
                                                                                const char *mime);
};

}
#endif  //QC_UTIL_CLASS
