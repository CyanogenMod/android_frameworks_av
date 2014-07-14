/*Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
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
#include <media/MediaProfiles.h>

#include "include/ExtendedUtils.h"

static const int64_t kDefaultAVSyncLateMargin =  40000;
static const int64_t kMaxAVSyncLateMargin     = 250000;

static const unsigned kDefaultRtpPortRangeStart = 15550;
static const unsigned kDefaultRtpPortRangeEnd = 65535;

#ifdef ENABLE_AV_ENHANCEMENTS

#include <QCMetaData.h>
#include <QCMediaDefs.h>

#include "include/ExtendedExtractor.h"
#include "include/avc_utils.h"
#include <fcntl.h>
#ifdef USE_ION
#include <linux/msm_ion.h>
#endif
#define MEM_DEVICE "/dev/ion"
#define MEM_HEAP_ID ION_CP_MM_HEAP_ID

#include <media/stagefright/foundation/ALooper.h>

namespace android {

void ExtendedUtils::HFR::setHFRIfEnabled(
        const CameraParameters& params,
        sp<MetaData> &meta) {
    const char *hfr_str = params.get("video-hfr");
    int32_t hfr = -1;
    if ( hfr_str != NULL ) {
        hfr = atoi(hfr_str);
    }
    if (hfr < 0) {
        ALOGW("Invalid hfr value(%d) set from app. Disabling HFR.", hfr);
        hfr = 0;
    }

#if 0
    const char *hsr_str = params.get("video-hsr");

    if(hsr_str && !strncmp(hsr_str,"on",2)) {
         ALOGI("HSR [%d] ON",hfr);
         meta->setInt32(kKeyHSR, hfr);
    } else
#endif
         meta->setInt32(kKeyHFR, hfr);
}

status_t ExtendedUtils::HFR::initializeHFR(
        sp<MetaData> &meta, sp<MetaData> &enc_meta,
        int64_t &maxFileDurationUs, video_encoder videoEncoder) {
    status_t retVal = OK;

#if 0
    //Check HSR first, if HSR is enable set HSR to kKeyFrameRate
    int32_t hsr =0;
    if (meta->findInt32(kKeyHSR, &hsr)) {
        ALOGI("HSR found %d, set this to encoder frame rate",hsr);
        enc_meta->setInt32(kKeyFrameRate, hsr);
        return retVal;
    }
#endif

    int32_t hfr = 0;
    if (!meta->findInt32(kKeyHFR, &hfr)) {
        ALOGW("hfr not found, default to 0");
    }

    enc_meta->setInt32(kKeyHFR, hfr);

    if (hfr == 0) {
        return retVal;
    }

    int32_t width = 0, height = 0;
    CHECK(meta->findInt32(kKeyWidth, &width));
    CHECK(meta->findInt32(kKeyHeight, &height));

    int maxW, maxH, MaxFrameRate, maxBitRate = 0;
    if (getHFRCapabilities(videoEncoder,
            maxW, maxH, MaxFrameRate, maxBitRate) < 0) {
        ALOGE("Failed to query HFR target capabilities");
        return ERROR_UNSUPPORTED;
    }

    if ((width * height * hfr) > (maxW * maxH * MaxFrameRate)) {
        ALOGE("HFR request [%d x %d @%d fps] exceeds "
                "[%d x %d @%d fps]",
                width, height, hfr, maxW, maxH, MaxFrameRate);
        return ERROR_UNSUPPORTED;
    }

    int32_t frameRate = 0, bitRate = 0;
    CHECK(meta->findInt32(kKeyFrameRate, &frameRate));
    CHECK(enc_meta->findInt32(kKeyBitRate, &bitRate));

    if (frameRate) {
        // scale the bitrate proportional to the hfr ratio
        // to maintain quality, but cap it to max-supported.
        bitRate = (hfr * bitRate) / frameRate;
        bitRate = bitRate > maxBitRate ? maxBitRate : bitRate;
        enc_meta->setInt32(kKeyBitRate, bitRate);

        int32_t hfrRatio = hfr / frameRate;
        enc_meta->setInt32(kKeyFrameRate, hfr);
        enc_meta->setInt32(kKeyHFR, hfrRatio);
    } else {
        ALOGE("HFR: Invalid framerate");
        return BAD_VALUE;
    }

    return retVal;
}

void ExtendedUtils::HFR::copyHFRParams(
        const sp<MetaData> &inputFormat,
        sp<MetaData> &outputFormat) {
    int32_t frameRate = 0, hfr = 0;
    inputFormat->findInt32(kKeyHFR, &hfr);
    inputFormat->findInt32(kKeyFrameRate, &frameRate);
    outputFormat->setInt32(kKeyHFR, hfr);
    outputFormat->setInt32(kKeyFrameRate, frameRate);
}

int32_t ExtendedUtils::HFR::getHFRRatio(
        const sp<MetaData> &meta) {
    int32_t hfr = 0;
    meta->findInt32(kKeyHFR, &hfr);
    return hfr ? hfr : 1;
}

int32_t ExtendedUtils::HFR::getHFRCapabilities(
        video_encoder codec,
        int& maxHFRWidth, int& maxHFRHeight, int& maxHFRFps,
        int& maxBitRate) {
    maxHFRWidth = maxHFRHeight = maxHFRFps = maxBitRate = 0;
    MediaProfiles *profiles = MediaProfiles::getInstance();
    if (profiles) {
        maxHFRWidth = profiles->getVideoEncoderParamByName("enc.vid.hfr.width.max", codec);
        maxHFRHeight = profiles->getVideoEncoderParamByName("enc.vid.hfr.height.max", codec);
        maxHFRFps = profiles->getVideoEncoderParamByName("enc.vid.hfr.mode.max", codec);
        maxBitRate = profiles->getVideoEncoderParamByName("enc.vid.bps.max", codec);
    }
    return (maxHFRWidth > 0) && (maxHFRHeight > 0) &&
            (maxHFRFps > 0) && (maxBitRate > 0) ? 1 : -1;
}

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

bool ExtendedUtils::ShellProp::isCustomAVSyncEnabled() {
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get("mm.enable.customavsync", prop, "0");
    if (!strncmp(prop, "true", 4) || atoi(prop)) {
        return true;
    }
    return false;
}

bool ExtendedUtils::ShellProp::isMpeg4DPSupportedByHardware() {
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get("mm.mpeg4dp.hw.support", prop, "0");
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
            }else if(!strncasecmp(mime.string(), "video/", 6)) {
                videoTrackFound = true;
            }
        }

        if(amrwbAudio) {
            bCheckExtendedExtractor = true;
        }else if (numOfTrack  == 0) {
            bCheckExtendedExtractor = true;
        } else if(numOfTrack == 1) {
            if((videoTrackFound) ||
                (!videoTrackFound && !audioTrackFound)){
                bCheckExtendedExtractor = true;
            }
        } else if (numOfTrack >= 2){
            if(videoTrackFound && audioTrackFound) {
                if(amrwbAudio) {
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


void ExtendedUtils::helper_addMediaCodec(Vector<MediaCodecList::CodecInfo> &mCodecInfos,
                                          KeyedVector<AString, size_t> &mTypes,
                                          bool encoder, const char *name,
                                          const char *type, uint32_t quirks) {
    mCodecInfos.push();
    MediaCodecList::CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    info->mName = name;
    info->mIsEncoder = encoder;
    info->mTypes=0;
    ssize_t index = mTypes.indexOfKey(type);
    uint32_t bit;
    if(index < 0) {
         bit = mTypes.size();
         if (bit == 64) {
             ALOGW("Too many distinct type names in configuration.");
             return;
         }
         mTypes.add(name, bit);
    } else {
        bit = mTypes.valueAt(index);
    }
    info->mTypes |= 1ul << bit;
    info->mQuirks = quirks;
}

uint32_t ExtendedUtils::helper_getCodecSpecificQuirks(KeyedVector<AString, size_t> &mCodecQuirks,
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

bool ExtendedUtils::isAVCProfileSupported(int32_t  profile){
   if(profile == OMX_VIDEO_AVCProfileMain || profile == OMX_VIDEO_AVCProfileHigh || profile == OMX_VIDEO_AVCProfileBaseline){
      return true;
   } else {
      return false;
   }
}

void ExtendedUtils::updateNativeWindowBufferGeometry(ANativeWindow* anw,
        OMX_U32 width, OMX_U32 height, OMX_COLOR_FORMATTYPE colorFormat) {
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

void ExtendedUtils::helper_Mpeg4ExtractorCheckAC3EAC3(MediaBuffer *buffer,
                                                        sp<MetaData> &format,
                                                        size_t size) {
    bool mMakeBigEndian = false;
    const char *mime;

    if (format->findCString(kKeyMIMEType, &mime)
            && (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AC3) ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_EAC3))) {
        mMakeBigEndian = true;
    }
    if (mMakeBigEndian && *((uint8_t *)buffer->data())==0x0b &&
            *((uint8_t *)buffer->data()+1)==0x77 ) {
        size_t count = 0;
        for(count=0;count<size;count+=2) { // size is always even bytes in ac3/ec3 read
            uint8_t tmp = *((uint8_t *)buffer->data() + count);
            *((uint8_t *)buffer->data() + count) = *((uint8_t *)buffer->data()+count+1);
            *((uint8_t *)buffer->data() + count+1) = tmp;
        }
    }
}

int32_t ExtendedUtils::getEncoderTypeFlags() {
    int32_t flags = 0;

    char mDeviceName[PROPERTY_VALUE_MAX];
    property_get("ro.board.platform",mDeviceName,"0");
    if (!strncmp(mDeviceName, "msm8610", 7) ||
        !strncmp(mDeviceName, "msm8226", 7)) {
        flags |= OMXCodec::kHardwareCodecsOnly;
    }
    return flags;

}

void ExtendedUtils::prefetchSecurePool(const char *uri)
{
    if (!strncasecmp("widevine://", uri, 11)) {
        ALOGV("Widevine streaming content\n");
        createSecurePool();
    }
}

void ExtendedUtils::prefetchSecurePool(int fd)
{
    char symName[40] = {0};
    char fileName[256] = {0};
    char* kSuffix;
    size_t kSuffixLength = 0;
    size_t fileNameLength;

    snprintf(symName, sizeof(symName), "/proc/%d/fd/%d", getpid(), fd);

    if (readlink( symName, fileName, (sizeof(fileName) - 1)) != -1 ) {
        kSuffix = (char *)".wvm";
        kSuffixLength = strlen(kSuffix);
        fileNameLength = strlen(fileName);

        if (!strcmp(&fileName[fileNameLength - kSuffixLength], kSuffix)) {
            ALOGV("Widevine local content\n");
            createSecurePool();
        }
    }
}

void ExtendedUtils::prefetchSecurePool()
{
    createSecurePool();
}

void ExtendedUtils::createSecurePool()
{
#ifdef ION_IOC_PREFETCH
    struct ion_prefetch_data prefetch_data;
    struct ion_custom_data d;
    int ion_dev_flag = O_RDONLY;
    int rc = 0;
    int fd = open (MEM_DEVICE, ion_dev_flag);

    if (fd < 0) {
        ALOGE("opening ion device failed with fd = %d", fd);
    } else {
        prefetch_data.heap_id = ION_HEAP(MEM_HEAP_ID);
        prefetch_data.len = 0x0;
        d.cmd = ION_IOC_PREFETCH;
        d.arg = (unsigned long int)&prefetch_data;
        rc = ioctl(fd, ION_IOC_CUSTOM, &d);
        if (rc != 0) {
            ALOGE("creating secure pool failed, rc is %d, errno is %d", rc, errno);
        }
        close(fd);
    }
#endif
}

void ExtendedUtils::drainSecurePool()
{
#ifdef ION_IOC_DRAIN
    struct ion_prefetch_data prefetch_data;
    struct ion_custom_data d;
    int ion_dev_flag = O_RDONLY;
    int rc = 0;
    int fd = open (MEM_DEVICE, ion_dev_flag);

    if (fd < 0) {
        ALOGE("opening ion device failed with fd = %d", fd);
    } else {
        prefetch_data.heap_id = ION_HEAP(MEM_HEAP_ID);
        prefetch_data.len = 0x0;
        d.cmd = ION_IOC_DRAIN;
        d.arg = (unsigned long int)&prefetch_data;
        rc = ioctl(fd, ION_IOC_CUSTOM, &d);
        if (rc != 0) {
            ALOGE("draining secure pool failed rc is %d, errno is %d", rc, errno);
        }
        close(fd);
    }
#endif
}

VSyncLocker::VSyncLocker()
    : mExitVsyncEvent(true),
      mLooper(NULL),
      mSyncState(PROFILE_FPS),
      mStartTime(-1),
      mProfileCount(0) {
}

VSyncLocker::~VSyncLocker() {
    if(!mExitVsyncEvent) {
        mExitVsyncEvent = true;
        void *dummy;
        pthread_join(mThread, &dummy);
    }
}

bool VSyncLocker::isSyncRenderEnabled() {
    char value[PROPERTY_VALUE_MAX];
    bool ret = true;
    property_get("mm.enable.vsync.render", value, "0");
    if (atoi(value) == 0) {
        ret = false;
    }
    return ret;
}

void VSyncLocker::updateSyncState() {
    if (mSyncState == PROFILE_FPS) {
        mProfileCount++;
        if (mProfileCount == 1) {
            mStartTime = ALooper::GetNowUs();
        } else if (mProfileCount == kMaxProfileCount) {
            int fps = (kMaxProfileCount * 1000000) /
                      (ALooper::GetNowUs() - mStartTime);
            if (fps > 35) {
                ALOGI("Synchronized rendering blocked at %d fps", fps);
                mSyncState = BLOCK_SYNC;
                mExitVsyncEvent = true;
            } else {
                ALOGI("Synchronized rendering enabled at %d fps", fps);
                mSyncState = ENABLE_SYNC;
            }
        }
    }
}

void VSyncLocker::waitOnVSync() {
    Mutex::Autolock autoLock(mVsyncLock);
    mVSyncCondition.wait(mVsyncLock);
}

void VSyncLocker::resetProfile() {
    if (mSyncState == PROFILE_FPS) {
        mProfileCount = 0;
    }
}

void VSyncLocker::blockSync() {
    if (mSyncState == ENABLE_SYNC) {
        ALOGI("Synchronized rendering blocked");
        mSyncState = BLOCK_SYNC;
        mExitVsyncEvent = true;
    }
}

void VSyncLocker::blockOnVSync() {
        if (mSyncState == PROFILE_FPS) {
            updateSyncState();
        } else if(mSyncState == ENABLE_SYNC) {
            waitOnVSync();
        }
}

void VSyncLocker::start() {
    mExitVsyncEvent = false;
    mLooper = new Looper(false);
    mLooper->addFd(mDisplayEventReceiver.getFd(), 0,
                   ALOOPER_EVENT_INPUT, receiver, (void *)this);
    mDisplayEventReceiver.setVsyncRate(1);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&mThread, &attr, ThreadWrapper, (void *)this);
    pthread_attr_destroy(&attr);
}

void VSyncLocker::VSyncEvent() {
    do {
        int ret = 0;
        if (mLooper != NULL) {
            ret = mLooper->pollOnce(-1);
        }
    } while (!mExitVsyncEvent);
    mDisplayEventReceiver.setVsyncRate(0);
    mLooper->removeFd(mDisplayEventReceiver.getFd());
}

void VSyncLocker::signalVSync() {
   DisplayEventReceiver::Event buffer[1];
   if(mDisplayEventReceiver.getEvents(buffer, 1)) {
       if (buffer[0].header.type != DisplayEventReceiver::DISPLAY_EVENT_VSYNC) {
           return;
        }
   }
   mVsyncLock.lock();
   mVSyncCondition.signal();
   mVsyncLock.unlock();
   ALOGV("Signalling VSync");
}

void *VSyncLocker::ThreadWrapper(void *context) {
    VSyncLocker *renderer = (VSyncLocker *)context;
    renderer->VSyncEvent();
    return NULL;
}

int VSyncLocker::receiver(int fd, int events, void *context) {
    VSyncLocker *locker = (VSyncLocker *)context;
    locker->signalVSync();
    return 1;
}

void ExtendedUtils::parseRtpPortRangeFromSystemProperty(unsigned *start, unsigned *end) {
    char value[PROPERTY_VALUE_MAX];
    if (!property_get("persist.sys.media.rtp-ports", value, NULL)) {
        ALOGV("Cannot get property of persist.sys.media.rtp-ports");
        *start = kDefaultRtpPortRangeStart;
        *end = kDefaultRtpPortRangeEnd;
        return;
    }

    if (sscanf(value, "%u/%u", start, end) != 2) {
        ALOGE("Failed to parse rtp port range from '%s'.", value);
        *start = kDefaultRtpPortRangeStart;
        *end = kDefaultRtpPortRangeEnd;
        return;
    }

    if (*start > *end || *start <= 1024 || *end >= 65535) {
        ALOGE("Illegal rtp port start/end specified, reverting to defaults.");
        *start = kDefaultRtpPortRangeStart;
        *end = kDefaultRtpPortRangeEnd;
        return;
    }

    ALOGV("rtp port_start = %u, port_end = %u", *start, *end);
}

}
#else //ENABLE_AV_ENHANCEMENTS

namespace android {

void ExtendedUtils::HFR::setHFRIfEnabled(
        const CameraParameters& params, sp<MetaData> &meta) {
}

status_t ExtendedUtils::HFR::initializeHFR(
        sp<MetaData> &meta, sp<MetaData> &enc_meta,
        int64_t &maxFileDurationUs, video_encoder videoEncoder) {
    return OK;
}

void ExtendedUtils::HFR::copyHFRParams(
        const sp<MetaData> &inputFormat,
        sp<MetaData> &outputFormat) {
}

int32_t ExtendedUtils::HFR::getHFRRatio(
        const sp<MetaData> &meta) {
    return 1;
}

int32_t ExtendedUtils::HFR::getHFRCapabilities(
        video_encoder codec,
        int& maxHFRWidth, int& maxHFRHeight, int& maxHFRFps,
        int& maxBitRate) {
    maxHFRWidth = maxHFRHeight = maxHFRFps = maxBitRate = 0;
    return -1;
}

bool ExtendedUtils::ShellProp::isAudioDisabled(bool isEncoder) {
    return false;
}

void ExtendedUtils::ShellProp::setEncoderProfile(
        video_encoder &videoEncoder, int32_t &videoEncoderProfile) {
}

int64_t ExtendedUtils::ShellProp::getMaxAVSyncLateMargin() {
     return kDefaultAVSyncLateMargin;
}

bool ExtendedUtils::ShellProp::isSmoothStreamingEnabled() {
    return false;
}

bool ExtendedUtils::ShellProp::isCustomAVSyncEnabled() {
    return false;
}

bool ExtendedUtils::ShellProp::isMpeg4DPSupportedByHardware() {
    return false;
}

void ExtendedUtils::setBFrames(
        OMX_VIDEO_PARAM_MPEG4TYPE &mpeg4type, int32_t &numBFrames,
        const char* componentName) {
}

void ExtendedUtils::setBFrames(
        OMX_VIDEO_PARAM_AVCTYPE &h264type, int32_t &numBFrames,
        int32_t iFramesInterval, int32_t frameRate,
        const char* componentName) {
}

bool ExtendedUtils::UseQCHWAACEncoder(audio_encoder Encoder,int32_t Channel,
    int32_t BitRate,int32_t SampleRate) {
    return false;
}

sp<MediaExtractor> ExtendedUtils::MediaExtractor_CreateIfNeeded(sp<MediaExtractor> defaultExt,
                                                            const sp<DataSource> &source,
                                                            const char *mime) {
    return defaultExt;
}

void ExtendedUtils::helper_addMediaCodec(Vector<MediaCodecList::CodecInfo> &mCodecInfos,
                                          KeyedVector<AString, size_t> &mTypes,
                                          bool encoder, const char *name,
                                          const char *type, uint32_t quirks) {
}

uint32_t ExtendedUtils::helper_getCodecSpecificQuirks(KeyedVector<AString, size_t> &mCodecQuirks,
                                                       Vector<AString> quirks) {
    return 0;
}

bool ExtendedUtils::isAVCProfileSupported(int32_t  profile){
     return false;
}

void ExtendedUtils::updateNativeWindowBufferGeometry(ANativeWindow* anw,
        OMX_U32 width, OMX_U32 height, OMX_COLOR_FORMATTYPE colorFormat) {
}

bool ExtendedUtils::checkIsThumbNailMode(const uint32_t flags, char* componentName) {
    return false;
}

void ExtendedUtils::helper_Mpeg4ExtractorCheckAC3EAC3(MediaBuffer *buffer,
                                                        sp<MetaData> &format,
                                                        size_t size) {
}

int32_t ExtendedUtils::getEncoderTypeFlags() {
    return 0;
}

void ExtendedUtils::prefetchSecurePool(int fd) {}

void ExtendedUtils::prefetchSecurePool(const char *uri) {}

void ExtendedUtils::prefetchSecurePool() {}

void ExtendedUtils::createSecurePool() {}

void ExtendedUtils::drainSecurePool() {}

VSyncLocker::VSyncLocker() {}

VSyncLocker::~VSyncLocker() {}

bool VSyncLocker::isSyncRenderEnabled() {
    return false;
}

void *VSyncLocker::ThreadWrapper(void *context) {
    return NULL;
}

int VSyncLocker::receiver(int fd, int events, void *context) {
    return 0;
}

void VSyncLocker::updateSyncState() {}

void VSyncLocker::waitOnVSync() {}

void VSyncLocker::resetProfile() {}

void VSyncLocker::blockSync() {}

void VSyncLocker::blockOnVSync() {}

void VSyncLocker::start() {}

void VSyncLocker::VSyncEvent() {}

void VSyncLocker::signalVSync() {}

void ExtendedUtils::parseRtpPortRangeFromSystemProperty(unsigned *start, unsigned *end) {
    *start = kDefaultRtpPortRangeStart;
    *end = kDefaultRtpPortRangeEnd;
}

}
#endif //ENABLE_AV_ENHANCEMENTS
