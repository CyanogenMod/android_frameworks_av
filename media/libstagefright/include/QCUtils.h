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
#ifndef QC_UTILS_H_
#define QC_UTILS_H_

#include <utils/StrongPointer.h>
#include <media/Metadata.h>
#include <media/stagefright/MediaSource.h>
#include <media/MediaRecorderBase.h>
#include <media/stagefright/MediaExtractor.h>
#include <camera/CameraParameters.h>
#include <OMX_Video.h>


namespace android {

/*
 * This class is a placeholder for utility methods for
 * QC specific changes
 */
struct QCUtils {

    /*
     * This class is a placeholder for set of methods used
     * to enable HFR (High Frame Rate) Recording
     *
     * HFR is a slow-motion recording feature where framerate
     * is increased at capture, but file is composed to play
     * back at normal rate, giving a net result of slow-motion.
     * If HFR factor = N
     *   framerate (at capture and encoder) = N * actual value
     *   timeStamps (at composition) = N * actual value
     *   bitrate = N * actual value
     *      (as the encoder still gets actual timestamps)
     */
    struct HFR {
        // set kKeyHFR when 'video-hfr' paramater is enabled
        static void setHFRIfEnabled(
                const CameraParameters& params, sp<MetaData> &meta);

        // recalculate fileduration when hfr is enabled
        static status_t reCalculateFileDuration(
                sp<MetaData> &meta, sp<MetaData> &enc_meta,
                int64_t &maxFileDurationUs, int32_t frameRate,
                video_encoder videoEncoder);

        // compute timestamp when hfr is enabled
        static void reCalculateTimeStamp(
                sp<MetaData> &meta, int64_t &timestampUs);

        // recalculate frameRate and bitrate when hfr is enabled
        static void reCalculateHFRParams(
                const sp<MetaData> &meta, int32_t &frameRate,
                int32_t &bitrate);

        // Copy HFR params (bitrate,framerate) from output to
        // to input format, if HFR is enabled
        static void copyHFRParams(
                const sp<MetaData> &inputFormat,
                sp<MetaData> &outputFormat);
    };

    /*
     * This class is a placeholder for methods to override
     * default heaviour based on shell properties set
     */
    struct ShellProp {
        // check if shell property to disable audio is set
        static bool isAudioDisabled();

        //helper function to set encoding profiles
        static void setEncoderprofile(video_encoder &videoEncoder,
                int32_t &videoEncoderProfile);
    };

    //set B frames for MPEG4
    static void setBFrames(OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type, bool &numBFrames);

    //set B frames for H264
    static void setBFrames(OMX_VIDEO_PARAM_AVCTYPE &h264type, bool &numBFrames,
            int32_t iFramesInterval, int32_t frameRate);

    static sp<MediaExtractor> MediaExtractor_CreateIfNeeded(sp<MediaExtractor> defaultExt,
              const sp<DataSource> &source, const char *mime);

    static bool isAVCProfileSupported(int32_t profile);

};

}
#endif  //QC_UTILS_H_
