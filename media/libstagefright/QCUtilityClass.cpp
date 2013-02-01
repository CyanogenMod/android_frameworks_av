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



//#define LOG_NDEBUG 0
#define LOG_TAG "QCUtilClass"
#include <utils/Log.h>

#include <include/QCUtilityClass.h>
#include <media/stagefright/MetaData.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>


namespace android {

status_t QCUtilityClass::helper_StageFrightRecoder_hfr(sp<MetaData> &meta, sp<MetaData> &enc_meta,
                                                       int64_t &maxFileDurationUs, int32_t frameRate,
                                                       video_encoder videoEncoder) {
    status_t retVal = OK;

    int32_t hfr = 0;
    if (!meta->findInt32(kKeyHFR, &hfr)) {
        ALOGW("hfr not found, default to 0");
    }

    if(hfr && frameRate) {
        maxFileDurationUs = maxFileDurationUs * (hfr/frameRate);
    }

    enc_meta->setInt32(kKeyHFR, hfr);
    int32_t width = 0, height = 0;

    CHECK(meta->findInt32(kKeyWidth, &width));
    CHECK(meta->findInt32(kKeyHeight, &height));

    char mDeviceName[100];
    property_get("ro.board.platform",mDeviceName,"0");
    if(!strncmp(mDeviceName, "msm7627a", 8)) {
      if(hfr && (width * height > 432*240)) {
        ALOGE("HFR mode is supported only upto WQVGA resolution");
        return INVALID_OPERATION;
      }
    }
    else {
      if(hfr && ((videoEncoder != VIDEO_ENCODER_H264) || (width * height > 800*480))) {
        ALOGE("HFR mode is supported only upto WVGA and H264 codec.");
        return INVALID_OPERATION;
      }
    }

    return retVal;
}

void QCUtilityClass::helper_CameraSource_hfr(const CameraParameters& params,
                                             sp<MetaData> &meta) {
    const char *hfr_str = params.get("video-hfr");
    int32_t hfr = -1;
    if ( hfr_str != NULL ) {
      hfr = atoi(hfr_str);
    }
    if(hfr < 0) {
      ALOGW("Invalid hfr value(%d) set from app. Disabling HFR.", hfr);
      hfr = 0;
    }

    meta->setInt32(kKeyHFR, hfr);
}

void QCUtilityClass::helper_MPEG4Writer_hfr(sp<MetaData> &meta, int64_t &timestampUs) {
          int32_t frameRate = 0, hfr = 0, multiple = 0;

          if(!(meta->findInt32(kKeyFrameRate, &frameRate))) {
              return;
          }

          if(!(meta->findInt32(kKeyHFR, &hfr))) {
              return;
          }

          multiple = hfr?(hfr/frameRate):1;
          timestampUs = multiple * timestampUs;
}

void QCUtilityClass::helper_OMXCodec_hfr(const sp<MetaData> &meta, int32_t &frameRate,
                                         int32_t &bitRate, int32_t &newFrameRate) {
    int32_t hfr = 0, hfrRatio = 0;
    if(!(meta->findInt32(kKeyHFR, &hfr))) {
        return;
    }

    hfrRatio = hfr ? hfr/frameRate : 1;
    frameRate = hfr?hfr:frameRate;
    bitRate = hfr ? (hfrRatio*bitRate) : bitRate;

    newFrameRate = frameRate / hfrRatio;
}

void QCUtilityClass::helper_OMXCodec_hfr(const sp<MetaData> &inputFormat,
                                         sp<MetaData> &outputFormat) {

    int32_t frameRate = 0, hfr = 0;
    inputFormat->findInt32(kKeyHFR, &hfr);
    inputFormat->findInt32(kKeyFrameRate, &frameRate);
    outputFormat->setInt32(kKeyHFR, hfr);
    outputFormat->setInt32(kKeyFrameRate, frameRate);
}

}
