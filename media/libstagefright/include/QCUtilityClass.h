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

#include <QCMetaData.h>
#include <cutils/properties.h>
#include <QCMediaDefs.h>

#include <media/Metadata.h>
#include <media/stagefright/MediaSource.h>

#include <utils/Errors.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>
#include <utils/StrongPointer.h>

#include <media/MediaRecorderBase.h>
#include <camera/CameraParameters.h>

#ifndef QC_UTIL_CLASS
#define QC_UTIL_CLASS


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

};

}
#endif  //QC_UTIL_CLASS
