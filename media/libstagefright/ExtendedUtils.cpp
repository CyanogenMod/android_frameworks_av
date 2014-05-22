/*Copyright (c) 2013 - 2014, The Linux Foundation. All rights reserved.
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
#define LOG_TAG "ExtendedUtils"
#include <utils/Log.h>

#include <utils/Errors.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>

#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/OMXCodec.h>
#include <cutils/properties.h>
#include <media/stagefright/MediaExtractor.h>

#include "include/ExtendedUtils.h"

static const int64_t kDefaultAVSyncLateMargin =  40000;
static const int64_t kMaxAVSyncLateMargin     = 250000;

#define ARG_TOUCH(x) (void)x

#ifdef ENABLE_AV_ENHANCEMENTS

#include <QCMetaData.h>
#include <QCMediaDefs.h>

#include "include/ExtendedExtractor.h"
#include "include/avc_utils.h"

namespace android {

bool ExtendedUtils::ShellProp::isAudioDisabled(bool isEncoder) {
    bool retVal = false;
    char disableAudio[PROPERTY_VALUE_MAX];
    property_get("persist.debug.sf.noaudio", disableAudio, "0");
    if (isEncoder && (atoi(disableAudio) & 0x02)) {
        retVal = true;
    } else if (atoi(disableAudio) & 0x01) {
        retVal = true;
    }
    return retVal;
}

void ExtendedUtils::ShellProp::setEncoderProfile(
        video_encoder &videoEncoder, int32_t &videoEncoderProfile) {
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

int64_t ExtendedUtils::ShellProp::getMaxAVSyncLateMargin() {
    char lateMarginMs[PROPERTY_VALUE_MAX] = {0};
    property_get("media.sf.set.late.margin", lateMarginMs, "0");
    int64_t newLateMarginUs = atoi(lateMarginMs)*1000;
    int64_t maxLateMarginUs = newLateMarginUs;

    if (newLateMarginUs > kDefaultAVSyncLateMargin
            || newLateMarginUs < kDefaultAVSyncLateMargin) {
        maxLateMarginUs = kDefaultAVSyncLateMargin;
    }

    ALOGI("AV Sync late margin : Intended=%lldms Using=%lldms",
            maxLateMarginUs/1000, newLateMarginUs/1000);
    return maxLateMarginUs;
}

bool ExtendedUtils::ShellProp::isSmoothStreamingEnabled() {
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get("mm.enable.smoothstreaming", prop, "0");
    if (!strncmp(prop, "true", 4) || atoi(prop)) {
        return true;
    }
    return false;
}

void ExtendedUtils::setBFrames(
        OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type, int32_t &numBFrames,
        const char* componentName) {
    //ignore non QC components
    if (strncmp(componentName, "OMX.qcom.", 9)) {
        return;
    }
    if (mpeg4type.eProfile > OMX_VIDEO_MPEG4ProfileSimple) {
        mpeg4type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
        mpeg4type.nBFrames = 1;
        mpeg4type.nPFrames /= (mpeg4type.nBFrames + 1);
        numBFrames = mpeg4type.nBFrames;
    }
    return;
}

void ExtendedUtils::setBFrames(
        OMX_VIDEO_PARAM_AVCTYPE &h264type, int32_t &numBFrames,
        int32_t iFramesInterval, int32_t frameRate, const char* componentName) {
    //ignore non QC components
    if (strncmp(componentName, "OMX.qcom.", 9)) {
        return;
    }
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
        //enable CABAC as default entropy mode for Hihg/Main profiles
        h264type.bEntropyCodingCABAC = OMX_TRUE;
        h264type.nCabacInitIdc = 0;
        numBFrames = h264type.nBFrames;
    }
    return;
}

/*
QCOM HW AAC encoder allowed bitrates
------------------------------------------------------------------------------------------------------------------
Bitrate limit |AAC-LC(Mono)           | AAC-LC(Stereo)        |AAC+(Mono)            | AAC+(Stereo)            | eAAC+                      |
Minimum     |Min(24000,0.5 * f_s)   |Min(24000,f_s)           | 24000                      |24000                        |  24000                       |
Maximum    |Min(192000,6 * f_s)    |Min(192000,12 * f_s)  | Min(192000,6 * f_s)  | Min(192000,12 * f_s)  |  Min(192000,12 * f_s) |
------------------------------------------------------------------------------------------------------------------
*/
bool ExtendedUtils::UseQCHWAACEncoder(audio_encoder Encoder,int32_t Channel,int32_t BitRate,int32_t SampleRate)
{
    bool ret = false;
    int minBiteRate = -1;
    int maxBiteRate = -1;
    char propValue[PROPERTY_VALUE_MAX] = {0};

    ARG_TOUCH(BitRate);
    property_get("qcom.hw.aac.encoder",propValue,NULL);
    if (!strncmp(propValue,"true",sizeof("true"))) {
        //check for QCOM's HW AAC encoder only when qcom.aac.encoder =  true;
        ALOGV("qcom.aac.encoder enabled, check AAC encoder(%d) allowed bitrates",Encoder);
        switch (Encoder) {
        case AUDIO_ENCODER_AAC:// for AAC-LC format
            if (Channel == 1) {//mono
                minBiteRate = MIN_BITERATE_AAC<(SampleRate/2)?MIN_BITERATE_AAC:(SampleRate/2);
                maxBiteRate = MAX_BITERATE_AAC<(SampleRate*6)?MAX_BITERATE_AAC:(SampleRate*6);
            } else if (Channel == 2) {//stereo
                minBiteRate = MIN_BITERATE_AAC<SampleRate?MIN_BITERATE_AAC:SampleRate;
                maxBiteRate = MAX_BITERATE_AAC<(SampleRate*12)?MAX_BITERATE_AAC:(SampleRate*12);
            }
            break;
        case AUDIO_ENCODER_HE_AAC:// for AAC+ format
            if (Channel == 1) {//mono
                minBiteRate = MIN_BITERATE_AAC;
                maxBiteRate = MAX_BITERATE_AAC<(SampleRate*6)?MAX_BITERATE_AAC:(SampleRate*6);
            } else if (Channel == 2) {//stereo
                minBiteRate = MIN_BITERATE_AAC;
                maxBiteRate = MAX_BITERATE_AAC<(SampleRate*12)?MAX_BITERATE_AAC:(SampleRate*12);
            }
            break;
        default:
            ALOGV("encoder:%d not supported by QCOM HW AAC encoder",Encoder);

        }

        //return true only when 1. minBiteRate and maxBiteRate are updated(not -1) 2. minBiteRate <= SampleRate <= maxBiteRate
        if (BitRate >= minBiteRate && BitRate <= maxBiteRate) {
            ret = true;
        }
    }

    return ret;
}


//- returns NULL if we dont really need a new extractor (or cannot),
//  valid extractor is returned otherwise
//- caller needs to check for NULL
//  ----------------------------------------
//  defaultExt - the existing extractor
//  source - file source
//  mime - container mime
//  ----------------------------------------
//  Note: defaultExt will be deleted in this function if the new parser is selected

sp<MediaExtractor> ExtendedUtils::MediaExtractor_CreateIfNeeded(sp<MediaExtractor> defaultExt,
                                                            const sp<DataSource> &source,
                                                            const char *mime) {
    bool bCheckExtendedExtractor = false;
    bool videoTrackFound         = false;
    bool audioTrackFound         = false;
    bool amrwbAudio              = false;
    int  numOfTrack              = 0;

    if (defaultExt != NULL) {
        for (size_t trackItt = 0; trackItt < defaultExt->countTracks(); ++trackItt) {
            ++numOfTrack;
            sp<MetaData> meta = defaultExt->getTrackMetaData(trackItt);
            const char *_mime;
            CHECK(meta->findCString(kKeyMIMEType, &_mime));

            String8 mime = String8(_mime);

            if (!strncasecmp(mime.string(), "audio/", 6)) {
                audioTrackFound = true;

                amrwbAudio = !strncasecmp(mime.string(),
                                          MEDIA_MIMETYPE_AUDIO_AMR_WB,
                                          strlen(MEDIA_MIMETYPE_AUDIO_AMR_WB));
                if (amrwbAudio) {
                    break;
                }
            } else if (!strncasecmp(mime.string(), "video/", 6)) {
                videoTrackFound = true;
            }
        }

        if (amrwbAudio) {
            bCheckExtendedExtractor = true;
        } else if (numOfTrack  == 0) {
            bCheckExtendedExtractor = true;
        } else if (numOfTrack == 1) {
            if ((videoTrackFound) ||
                (!videoTrackFound && !audioTrackFound)) {
                bCheckExtendedExtractor = true;
            }
        } else if (numOfTrack >= 2) {
            if (videoTrackFound && audioTrackFound) {
                if (amrwbAudio) {
                    bCheckExtendedExtractor = true;
                }
            } else {
                bCheckExtendedExtractor = true;
            }
        }
    } else {
        bCheckExtendedExtractor = true;
    }

    if (!bCheckExtendedExtractor) {
        ALOGD("extended extractor not needed, return default");
        return defaultExt;
    }

    //Create Extended Extractor only if default extractor is not selected
    ALOGD("Try creating ExtendedExtractor");
    sp<MediaExtractor>  retExtExtractor = ExtendedExtractor::Create(source, mime);

    if (retExtExtractor == NULL) {
        ALOGD("Couldn't create the extended extractor, return default one");
        return defaultExt;
    }

    if (defaultExt == NULL) {
        ALOGD("default extractor is NULL, return extended extractor");
        return retExtExtractor;
    }

    //bCheckExtendedExtractor is true which means default extractor was found
    //but we want to give preference to extended extractor based on certain
    //conditions.

    //needed to prevent a leak in case both extractors are valid
    //but we still dont want to use the extended one. we need
    //to delete the new one
    bool bUseDefaultExtractor = true;

    for (size_t trackItt = 0; (trackItt < retExtExtractor->countTracks()); ++trackItt) {
        sp<MetaData> meta = retExtExtractor->getTrackMetaData(trackItt);
        const char *mime;
        bool success = meta->findCString(kKeyMIMEType, &mime);
        if ((success == true) &&
            (!strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS,
                                strlen(MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS)) ||
             !strncasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC,
                                strlen(MEDIA_MIMETYPE_VIDEO_HEVC)) )) {

            ALOGD("Discarding default extractor and using the extended one");
            bUseDefaultExtractor = false;
            break;
        }
    }

    if (bUseDefaultExtractor) {
        ALOGD("using default extractor inspite of having a new extractor");
        retExtExtractor.clear();
        return defaultExt;
    } else {
        defaultExt.clear();
        return retExtExtractor;
    }

}

bool ExtendedUtils::isAVCProfileSupported(int32_t  profile) {
   if (profile == OMX_VIDEO_AVCProfileMain ||
           profile == OMX_VIDEO_AVCProfileHigh ||
           profile == OMX_VIDEO_AVCProfileBaseline) {
      return true;
   } else {
      return false;
   }
}

void ExtendedUtils::updateNativeWindowBufferGeometry(ANativeWindow* anw,
        OMX_U32 width, OMX_U32 height, OMX_COLOR_FORMATTYPE colorFormat) {
    ARG_TOUCH(anw);
    ARG_TOUCH(width);
    ARG_TOUCH(height);
    ARG_TOUCH(colorFormat);

#if UPDATE_BUFFER_GEOMETRY_AVAILABLE
    if (anw != NULL) {
        ALOGI("Calling native window update buffer geometry [%lu x %lu]",
                width, height);
        status_t err = anw->perform(
                anw, NATIVE_WINDOW_UPDATE_BUFFERS_GEOMETRY,
                width, height, colorFormat);
        if (err != OK) {
            ALOGE("UPDATE_BUFFER_GEOMETRY failed %d", err);
        }
    }
#endif
}

bool ExtendedUtils::checkIsThumbNailMode(const uint32_t flags, char* componentName) {
    bool isInThumbnailMode = false;
    if ((flags & OMXCodec::kClientNeedsFramebuffer) && !strncmp(componentName, "OMX.qcom.", 9)) {
        isInThumbnailMode = true;
    }
    return isInThumbnailMode;
}

}
#else //ENABLE_AV_ENHANCEMENTS

namespace android {

bool ExtendedUtils::ShellProp::isAudioDisabled(bool isEncoder) {
    return false;
}

void ExtendedUtils::ShellProp::setEncoderProfile(
        video_encoder &videoEncoder, int32_t &videoEncoderProfile) {
    ARG_TOUCH(videoEncoder);
    ARG_TOUCH(videoEncoderProfile);
}

int64_t ExtendedUtils::ShellProp::getMaxAVSyncLateMargin() {
     return kDefaultAVSyncLateMargin;
}

bool ExtendedUtils::ShellProp::isSmoothStreamingEnabled() {
    return false;
}

void ExtendedUtils::setBFrames(
        OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type, int32_t &numBFrames,
        const char* componentName) {
    ARG_TOUCH(mpeg4type);
    ARG_TOUCH(numBFrames);
    ARG_TOUCH(componentName);
}

void ExtendedUtils::setBFrames(
        OMX_VIDEO_PARAM_AVCTYPE &h264type, int32_t &numBFrames,
        int32_t iFramesInterval, int32_t frameRate,
        const char* componentName) {
    ARG_TOUCH(h264type);
    ARG_TOUCH(numBFrames);
    ARG_TOUCH(iFramesInterval);
    ARG_TOUCH(frameRate);
    ARG_TOUCH(componentName);
}

bool ExtendedUtils::UseQCHWAACEncoder(audio_encoder Encoder,int32_t Channel,
        int32_t BitRate,int32_t SampleRate) {
    ARG_TOUCH(Encoder);
    ARG_TOUCH(Channel);
    ARG_TOUCH(BitRate);
    ARG_TOUCH(SampleRate);
    return false;
}

sp<MediaExtractor> ExtendedUtils::MediaExtractor_CreateIfNeeded(
        sp<MediaExtractor> defaultExt,
        const sp<DataSource> &source,
        const char *mime) {
    ARG_TOUCH(defaultExt);
    ARG_TOUCH(source);
    ARG_TOUCH(mime);
    return defaultExt;
}

bool ExtendedUtils::isAVCProfileSupported(int32_t  profile) {
    ARG_TOUCH(profile);
    return false;
}

void ExtendedUtils::updateNativeWindowBufferGeometry(ANativeWindow* anw,
        OMX_U32 width, OMX_U32 height, OMX_COLOR_FORMATTYPE colorFormat) {
    ARG_TOUCH(anw);
    ARG_TOUCH(width);
    ARG_TOUCH(height);
    ARG_TOUCH(colorFormat);
}

bool ExtendedUtils::checkIsThumbNailMode(const uint32_t flags, char* componentName) {
    ARG_TOUCH(flags);
    ARG_TOUCH(componentName);
    return false;
}

}
#endif //ENABLE_AV_ENHANCEMENTS
