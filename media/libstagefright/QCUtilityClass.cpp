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
#include "include/ExtendedExtractor.h"
#include <media/stagefright/MetaData.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXCodec.h>

namespace android {

//-- START :: HFR Related Changes -----

status_t QCUtilityClass::helper_StageFrightRecoder_hfr(sp<MetaData> &meta, sp<MetaData> &enc_meta,
                                                       int64_t &maxFileDurationUs, int32_t frameRate,
                                                       video_encoder videoEncoder) {
    status_t retVal = OK;
    int32_t hfr = 0;

    if (!meta->findInt32(kKeyHFR, &hfr)) {
        ALOGW("hfr not found, default to 0");
    }

    if (hfr && frameRate) {
        maxFileDurationUs = maxFileDurationUs * (hfr/frameRate);
    }

    enc_meta->setInt32(kKeyHFR, hfr);
    int32_t width = 0, height = 0;

    CHECK(meta->findInt32(kKeyWidth, &width));
    CHECK(meta->findInt32(kKeyHeight, &height));

    char mDeviceName[100];
    property_get("ro.board.platform",mDeviceName,"0");
    if (!strncmp(mDeviceName, "msm7627a", 8)) {
        if (hfr && (width * height > 432*240)) {
            ALOGE("HFR mode is supported only upto WQVGA resolution");
            return INVALID_OPERATION;
        }
    } else {
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

    if (hfr_str != NULL) {
        hfr = atoi(hfr_str);
    }
    if (hfr < 0) {
        ALOGW("Invalid hfr value(%d) set from app. Disabling HFR.", hfr);
        hfr = 0;
    }
    meta->setInt32(kKeyHFR, hfr);
}

void QCUtilityClass::helper_MPEG4Writer_hfr(sp<MetaData> &meta,
                                            int64_t &timestampUs) {
    int32_t frameRate = 0, hfr = 0, multiple = 0;

    if (!(meta->findInt32(kKeyFrameRate, &frameRate))) {
        return;
    }

    if (!(meta->findInt32(kKeyHFR, &hfr))) {
        return;
    }

    multiple = hfr ? (hfr/frameRate) : 1;
    timestampUs = multiple * timestampUs;
}

void QCUtilityClass::helper_OMXCodec_hfr(const sp<MetaData> &meta,
                                         int32_t &frameRate,
                                         int32_t &bitRate,
                                         int32_t &newFrameRate) {
    int32_t hfr = 0, hfrRatio = 0;
    if (!(meta->findInt32(kKeyHFR, &hfr))) {
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

//-- END :: HFR related changes -----


//-- START :: AUDIO disable and change in profile base on property -----

bool QCUtilityClass::helper_Awesomeplayer_checkIfAudioDisable() {
    bool retVal = false;
    char disableAudio[PROPERTY_VALUE_MAX];
    property_get("persist.debug.sf.noaudio", disableAudio, "0");
    if (atoi(disableAudio) == 1) {
        retVal = true;
    }
    return retVal;
}

bool QCUtilityClass::helper_StagefrightRecoder_checkIfAudioDisable() {
    bool retVal = false;
    char disableAudio[PROPERTY_VALUE_MAX];
    property_get("camcorder.debug.disableaudio", disableAudio, "0");
    if (atoi(disableAudio) == 1) {
        retVal = true;
    }
    return retVal;
}

void QCUtilityClass::helper_StagefrightRecoder_setUserprofile(video_encoder &videoEncoder,
                                                             int32_t &videoEncoderProfile) {
    char value[PROPERTY_VALUE_MAX];
    bool customProfile = false;
    if (!property_get("encoder.video.profile", value, NULL) > 0) {
        return;
    }

    switch (videoEncoder) {
    case VIDEO_ENCODER_H264:
        if (strncmp("base", value, 4) == 0) {
            videoEncoderProfile = OMX_VIDEO_AVCProfileBaseline;
            ALOGI("H264 Baseline Profile");
        } else if (strncmp("main", value, 4) == 0) {
            videoEncoderProfile = OMX_VIDEO_AVCProfileMain;
            ALOGI("H264 Main Profile");
        } else if (strncmp("high", value, 4) == 0) {
            videoEncoderProfile = OMX_VIDEO_AVCProfileHigh;
            ALOGI("H264 High Profile");
        } else {
            ALOGW("Unsupported H264 Profile");
        }
        break;
    case VIDEO_ENCODER_MPEG_4_SP:
        if (strncmp("simple", value, 5) == 0 ) {
            videoEncoderProfile = OMX_VIDEO_MPEG4ProfileSimple;
            ALOGI("MPEG4 Simple profile");
        } else if (strncmp("asp", value, 3) == 0 ) {
            videoEncoderProfile = OMX_VIDEO_MPEG4ProfileAdvancedSimple;
            ALOGI("MPEG4 Advanced Simple Profile");
        } else {
            ALOGW("Unsupported MPEG4 Profile");
        }
        break;
    default:
        ALOGW("No custom profile support for other codecs");
        break;
    }
}

void QCUtilityClass::helper_OMXCodec_setBFrames(OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type,
                                                bool &numBFrames) {
    if (mpeg4type.eProfile > OMX_VIDEO_MPEG4ProfileSimple) {
        mpeg4type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
        mpeg4type.nBFrames = 1;
        mpeg4type.nPFrames /= (mpeg4type.nBFrames + 1);
        numBFrames = mpeg4type.nBFrames;
    }
    return;
}

void QCUtilityClass::helper_OMXCodec_setBFrames(OMX_VIDEO_PARAM_AVCTYPE &h264type,
                                                bool &numBFrames,
                                                int32_t iFramesInterval,
                                                int32_t frameRate) {
    OMX_U32 val = 0;
    if (iFramesInterval < 0) {
        val =  0xFFFFFFFF;
    } else if (iFramesInterval == 0) {
        val = 0;
    } else {
        val  = frameRate * iFramesInterval - 1;
        CHECK(val > 1);
    }

    h264type.nPFrames = val;

    if (h264type.nPFrames == 0) {
        h264type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }

    if (h264type.eProfile > OMX_VIDEO_AVCProfileBaseline) {
        h264type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
        h264type.nBFrames = 1;
        h264type.nPFrames /= (h264type.nBFrames + 1);
        numBFrames = h264type.nBFrames;
    }
    return;
}
//--  END  :: AUDIO disable and change in profile base on property -----
void QCUtilityClass::helper_addMediaCodec(Vector<MediaCodecList::CodecInfo> &mCodecInfos,
                                          KeyedVector<AString, size_t> &mTypes,
                                          bool encoder, const char *name,
                                          const char *type, uint32_t quirks) {
    mCodecInfos.push();
    MediaCodecList::CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    info->mName = name;
    info->mIsEncoder = encoder;
    ssize_t index = mTypes.indexOfKey(type);
    uint32_t bit = mTypes.valueAt(index);
    info->mTypes |= 1ul << bit;
    info->mQuirks = quirks;
}

uint32_t QCUtilityClass::helper_getCodecSpecificQuirks(KeyedVector<AString, size_t> &mCodecQuirks,
                                                       Vector<AString> quirks) {
    size_t i = 0, numQuirks = quirks.size();
    uint32_t bit = 0, value = 0;
    for (i = 0; i < numQuirks; i++)
    {
        ssize_t index = mCodecQuirks.indexOfKey(quirks.itemAt(i));
        bit = mCodecQuirks.valueAt(index);
        value |= 1ul << bit;
    }
    return value;
}

//- returns NULL if we dont really need a new extractor (or cannot),
//  valid extractor is returned otherwise
//- caller needs to check for NULL
// defaultExt - the existing extractor
// source - file source
// mime - container mime
// Note that defaultExt will be deleted in this function if the new parser is taken
sp<MediaExtractor> QCUtilityClass::helper_MediaExtractor_CreateIfNeeded(sp<MediaExtractor> defaultExt,
                                                     const sp<DataSource> &source,
                                                     const char *mime) {
    bool bCheckExtendedExtractor = false;
    bool videoOnly = true;
    bool amrwbAudio = false;
    if (defaultExt != NULL) {
        for (size_t i = 0; i < defaultExt->countTracks(); ++i) {
            sp<MetaData> meta = defaultExt->getTrackMetaData(i);
            const char *_mime;
            CHECK(meta->findCString(kKeyMIMEType, &_mime));

            String8 mime = String8(_mime);

            if (!strncasecmp(mime.string(), "audio/", 6)) {
                videoOnly = false;

                amrwbAudio = !strncasecmp(mime.string(),
                                          MEDIA_MIMETYPE_AUDIO_AMR_WB,
                                          strlen(MEDIA_MIMETYPE_AUDIO_AMR_WB));
                if (amrwbAudio) break;
            }
        }
        bCheckExtendedExtractor = videoOnly || amrwbAudio;
    } else {
        bCheckExtendedExtractor = true;
    }

    if (!bCheckExtendedExtractor) {
        ALOGD("extended extractor not needed, return default");
        return defaultExt;
    }

    sp<MediaExtractor> retextParser;

    //Create Extended Extractor only if default extractor are not selected
    ALOGD("Try creating ExtendedExtractor");
    retextParser = ExtendedExtractor::CreateExtractor(source, mime);

    if (retextParser == NULL) {
        ALOGD("Couldn't create the extended extractor, return default one");
        return defaultExt;
    }

    if (defaultExt == NULL) {
        ALOGD("default one is NULL, return extended extractor");
        return retextParser;
    }

    //bCheckExtendedExtractor is true which means default extractor was found
    //but we want to give preference to extended extractor based on certain
    //conditions.

    //needed to prevent a leak in case both extractors are valid
    //but we still dont want to use the extended one. we need
    //to delete the new one
    bool bUseDefaultExtractor = true;

    for (size_t i = 0; (i < retextParser->countTracks()); ++i) {
        sp<MetaData> meta = retextParser->getTrackMetaData(i);
        const char *mime;
        bool success = meta->findCString(kKeyMIMEType, &mime);
        if ((success == true) && !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS)) {
            ALOGD("Discarding default extractor and using the extended one");
            bUseDefaultExtractor = false;
            break;
        }
    }

    if (bUseDefaultExtractor) {
        ALOGD("using default extractor inspite of having a new extractor");
        retextParser.clear();
        return defaultExt;
    } else {
        defaultExt.clear();
        return retextParser;
    }
}

}
