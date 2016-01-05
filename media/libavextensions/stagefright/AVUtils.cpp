/*
 * Copyright (c) 2013 - 2016, The Linux Foundation. All rights reserved.
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

#define LOG_TAG "AVUtils"
#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <cutils/properties.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaCodec.h>
#include <media/MediaProfiles.h>

#if defined(QCOM_HARDWARE) || defined(FLAC_OFFLOAD_ENABLED)
#include "QCMediaDefs.h"
#include "QCMetaData.h"
#ifdef FLAC_OFFLOAD_ENABLED
#include "audio_defs.h"
#endif
#endif

#include <binder/IPCThreadState.h>
#include <camera/CameraParameters.h>

#include "common/ExtensionsLoader.hpp"
#include "stagefright/AVExtensions.h"

namespace android {

status_t AVUtils::convertMetaDataToMessage(
        const sp<MetaData> &, sp<AMessage> *) {
    return OK;
}

status_t AVUtils::convertMessageToMetaData(
        const sp<AMessage> &, sp<MetaData> &) {
    return OK;
}

status_t AVUtils::mapMimeToAudioFormat(
        audio_format_t&, const char* ) {
    return OK;
}

status_t AVUtils::sendMetaDataToHal(
        const sp<MetaData>&, AudioParameter *){
    return OK;
}

bool AVUtils::hasAudioSampleBits(const sp<MetaData> &) {
    return false;
}

bool AVUtils::hasAudioSampleBits(const sp<AMessage> &) {
    return false;
}

int AVUtils::getAudioSampleBits(const sp<MetaData> &) {
    return 16;
}

int AVUtils::getAudioSampleBits(const sp<AMessage> &format) {
    AudioEncoding encoding = kAudioEncodingPcm16bit;
    format->findInt32("pcm-encoding", (int32_t*)&encoding);
    return audioEncodingToBits(encoding);
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<MetaData> &){
    return audioFormat;
}

audio_format_t AVUtils::updateAudioFormat(audio_format_t audioFormat,
        const sp<AMessage> &){
    return audioFormat;
}

static bool dumbSniffer(
        const sp<DataSource> &, String8 *,
        float *, sp<AMessage> *) {
    return false;
}

DataSource::SnifferFunc AVUtils::getExtendedSniffer() {
    return dumbSniffer;
}

sp<MediaCodec> AVUtils::createCustomComponentByName(
           const sp<ALooper> &, const char* , bool, const sp<AMessage> &) {
               return NULL;
}

int32_t AVUtils::getAudioMaxInputBufferSize(audio_format_t, const sp<AMessage> &) {
    return 0;
}

bool AVUtils::mapAACProfileToAudioFormat(const sp<MetaData> &, audio_format_t &,
                 uint64_t  /*eAacProfile*/) {
    return false ;
}

bool AVUtils::mapAACProfileToAudioFormat(const sp<AMessage> &,  audio_format_t &,
                 uint64_t  /*eAacProfile*/) {
    return false ;
}

bool AVUtils::canOffloadAPE(const sp<MetaData> &) {
   return true;
}

bool AVUtils::isEnhancedExtension(const char *) {
    return false;
}


bool AVUtils::isAudioMuxFormatSupported(const char * mime) {
    if (mime == NULL) {
        ALOGE("NULL audio mime type");
        return false;
    }

    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, mime)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, mime)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime)) {
        return true;
    }
    return false;
}

void AVUtils::cacheCaptureBuffers(sp<hardware::ICamera> camera, video_encoder encoder) {
    if (camera != NULL) {
        char mDeviceName[PROPERTY_VALUE_MAX];
        property_get("ro.board.platform", mDeviceName, "0");
        if (!strncmp(mDeviceName, "msm8909", 7)) {
            int64_t token = IPCThreadState::self()->clearCallingIdentity();
            String8 s = camera->getParameters();
            CameraParameters params(s);
            const char *enable;
            if (encoder == VIDEO_ENCODER_H263 ||
                encoder == VIDEO_ENCODER_MPEG_4_SP) {
                enable = "1";
            } else {
                enable = "0";
            }
            params.set("cache-video-buffers", enable);
            if (camera->setParameters(params.flatten()) != OK) {
                ALOGE("Failed to enabled cached camera buffers");
            }
            IPCThreadState::self()->restoreCallingIdentity(token);
        }
    }
}

const char *AVUtils::getCustomCodecsLocation() {
    return "/etc/media_codecs.xml";
}

void AVUtils::setIntraPeriod(
        int, int, const sp<IOMX>,
        IOMX::node_id) {
    return;
}

const char *AVUtils::getCustomCodecsPerformanceLocation() {
    return "/etc/media_codecs_performance.xml";
}

bool AVUtils::IsHevcIDR(const sp<ABuffer> &) {
   return false;
}

#ifdef QCOM_HARDWARE
void AVUtils::HFR::setHFRIfEnabled(
        const CameraParameters& params,
        sp<MetaData> &meta) {
    const char *hfrParam = params.get("video-hfr");
    int32_t hfr = -1;
    if (hfrParam != NULL) {
        hfr = atoi(hfrParam);
        if (hfr > 0) {
            ALOGI("Enabling HFR @ %d fps", hfr);
            meta->setInt32(kKeyHFR, hfr);
            return;
        } else {
            ALOGI("Invalid HFR rate specified : %d", hfr);
        }
    }

    const char *hsrParam = params.get("video-hsr");
    int32_t hsr = -1;
    if (hsrParam != NULL ) {
        hsr = atoi(hsrParam);
        if (hsr > 0) {
            ALOGI("Enabling HSR @ %d fps", hsr);
            meta->setInt32(kKeyHSR, hsr);
        } else {
            ALOGI("Invalid HSR rate specified : %d", hfr);
        }
    }
}

status_t AVUtils::HFR::initializeHFR(
        const sp<MetaData> &meta, sp<AMessage> &format,
        int64_t & /*maxFileDurationUs*/, video_encoder videoEncoder) {
    status_t retVal = OK;

    int32_t hsr = 0;
    if (meta->findInt32(kKeyHSR, &hsr) && hsr > 0) {
        ALOGI("HSR cue found. Override encode fps to %d", hsr);
        format->setInt32("frame-rate", hsr);
        return retVal;
    }

    int32_t hfr = 0;
    if (!meta->findInt32(kKeyHFR, &hfr) || (hfr <= 0)) {
        ALOGW("Invalid HFR rate specified");
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
                "[%d x %d @%d fps]. Will stay disabled",
                width, height, hfr, maxW, maxH, MaxFrameRate);
        return ERROR_UNSUPPORTED;
    }

    int32_t frameRate = 0, bitRate = 0;
    CHECK(meta->findInt32(kKeyFrameRate, &frameRate));
    CHECK(format->findInt32("bitrate", &bitRate));

    if (frameRate) {
        // scale the bitrate proportional to the hfr ratio
        // to maintain quality, but cap it to max-supported.
        bitRate = (hfr * bitRate) / frameRate;
        bitRate = bitRate > maxBitRate ? maxBitRate : bitRate;
        format->setInt32("bitrate", bitRate);

        int32_t hfrRatio = hfr / frameRate;
        format->setInt32("frame-rate", hfr);
        format->setInt32("hfr-ratio", hfrRatio);
    } else {
        ALOGE("HFR: Invalid framerate");
        return BAD_VALUE;
    }

    return retVal;
}

void AVUtils::HFR::setHFRRatio(
        const sp<MetaData> &meta, const int32_t hfrRatio) {
    if (hfrRatio > 0) {
        meta->setInt32(kKeyHFR, hfrRatio);
    }
}

int32_t AVUtils::HFR::getHFRRatio(
        const sp<MetaData> &meta) {
    int32_t hfrRatio = 0;
    meta->findInt32(kKeyHFR, &hfrRatio);
    return hfrRatio ? hfrRatio : 1;
}

int32_t AVUtils::HFR::getHFRCapabilities(
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
#else
void AVUtils::HFR::setHFRIfEnabled(
        const CameraParameters& /*params*/,
        sp<MetaData> & /*meta*/) {}

status_t AVUtils::HFR::initializeHFR(
        const sp<MetaData> & /*meta*/, sp<AMessage> & /*format*/,
        int64_t & /*maxFileDurationUs*/, video_encoder /*videoEncoder*/) {
    return OK;
}

void AVUtils::HFR::setHFRRatio(
        const sp<MetaData> & /*meta*/, const int32_t /*hfrRatio*/) {}

int32_t AVUtils::HFR::getHFRRatio(
        const sp<MetaData> & /*meta */) {
    return 1;
}

int32_t AVUtils::HFR::getHFRCapabilities(
        video_encoder /*codec*/,
        int& /*maxHFRWidth*/, int& /*maxHFRHeight*/, int& /*maxHFRFps*/,
        int& /*maxBitRate*/) {
    return -1;
}
#endif

void AVUtils::extractCustomCameraKeys(
        const CameraParameters& params, sp<MetaData> &meta) {
    mHFR.setHFRIfEnabled(params, meta);
}

// ----- NO TRESSPASSING BEYOND THIS LINE ------
AVUtils::AVUtils() {}

AVUtils::~AVUtils() {}

//static
AVUtils *AVUtils::sInst =
        ExtensionsLoader<AVUtils>::createInstance("createExtendedUtils");

} //namespace android

