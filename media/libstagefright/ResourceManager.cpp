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
#define LOG_NDDEBUG 0

#define LOG_TAG "ResourceManager"
#include <utils/Log.h>
#include <utils/Trace.h>

#include <utils/Errors.h>
#ifdef RESOURCE_MANAGER
#include <media/AudioParameter.h>
#include <media/AudioSystem.h>
#include <binder/IPCThreadState.h>
#endif

#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <cutils/properties.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/LPAPlayer.h>

#ifdef USE_TUNNEL_MODE
#include <media/stagefright/TunnelPlayer.h>
#endif
#include "include/ResourceManager.h"

#define LPA_MIN_DURATION_USEC_ALLOWED 30000000
#define LPA_MIN_DURATION_USEC_DEFAULT 60000000

namespace android {

#ifdef USE_LPA_MODE
// Check for the conditions to honor lpa playback
bool  ResourceManager::isLPAPlayback(const sp<MediaSource> &audioTrack,
        const sp<MediaSource> &videoSource, const AudioPlayer * audioPlayer,
        const sp<MediaPlayerBase::AudioSink> &audioSink,
        int64_t &durationUs, String8 &useCase,
        bool &useCaseFlag) {

    ALOGV("isLPAPlayback");
    int32_t nchannels = 0;
    int tunnelObjectsAlive = 0;
    const char *mime = NULL;
#ifdef USE_TUNNEL_MODE
    tunnelObjectsAlive = (TunnelPlayer::mTunnelObjectsAlive);
#endif

    if(audioTrack != NULL) {
        sp<MetaData> format = audioTrack->getFormat();
        if(format != NULL) {
            format->findInt32( kKeyChannelCount, &nchannels );
            ALOGV("nchannels %d;LPA will be skipped if nchannels\
                    is > 2 or nchannels == 0",nchannels);
        }

        bool success = format->findCString(kKeyMIMEType, &mime);
        CHECK(success);
    }
    char lpaDecode[PROPERTY_VALUE_MAX];
    uint32_t minDurationForLPA = LPA_MIN_DURATION_USEC_DEFAULT;
    char minUserDefDuration[PROPERTY_VALUE_MAX];
    property_get("lpa.decode",lpaDecode,"0");
    property_get("lpa.min_duration",minUserDefDuration,
           "LPA_MIN_DURATION_USEC_DEFAULT");
    minDurationForLPA = atoi(minUserDefDuration);
    if(minDurationForLPA < LPA_MIN_DURATION_USEC_ALLOWED) {
        if(audioPlayer == NULL) {
            ALOGV("LPAPlayer::Clip duration setting of less than\
                    30sec not supported, defaulting to 60sec");
            minDurationForLPA = LPA_MIN_DURATION_USEC_DEFAULT;
        }
    }
    int64_t metaDurationUs;
    if (audioTrack->getFormat()->findInt64(kKeyDuration, &metaDurationUs)) {
        if (durationUs < 0 || metaDurationUs > durationUs) {
            durationUs = metaDurationUs;
        }
    }

    ALOGV("LPAPlayer::getObjectsAlive() %d",LPAPlayer::mObjectsAlive);

    if(!strcmp("true",lpaDecode) == 0) {
       ALOGV("property lpa false");
       return false;
    }
    if (audioPlayer != NULL) {
       ALOGV("audio player  - lpa false");
       return false;
    }
#ifdef USE_TUNNEL_MODE
    if((tunnelObjectsAlive >= TunnelPlayer::getTunnelObjectsAliveMax())) {
        ALOGV("tunnel objects-lpa false");
        return false;
    }
#endif
    if(nchannels <= 0  && (nchannels > 2)) {
        ALOGV("channels zero lpa false");
        return false;
    }
    if(durationUs < minDurationForLPA) {
        ALOGV("duration lpa false");
        return false;
    }

    if(strncasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG, strlen(MEDIA_MIMETYPE_AUDIO_MPEG) ) &&
            strncasecmp(mime,MEDIA_MIMETYPE_AUDIO_AAC, strlen(MEDIA_MIMETYPE_AUDIO_AAC))) {
        ALOGV("mime lpa false");
        return false;
    }

    if(LPAPlayer::mObjectsAlive != 0) {
       ALOGV("objects alive - lpa false");
       return false;
    }

    if(videoSource != NULL) {
        ALOGV("video source - lpa false");
        return false;
    }

    if(!ResourceManager::isStreamMusic(audioSink)) {
        ALOGV("No LPA /Tunnel for stream Music");
        return false;
    }

    status_t err = OK;
    if(useCase != "USECASE_LPA_PLAYBACK") {
        useCase = "USECASE_LPA_PLAYBACK";
        err = ResourceManager::AudioConcurrencyInfo::setNonCodecParameter(useCase, useCaseFlag);
        if(err != OK) {
           useCase = "";
           return false;
        }
    }
    return true;
}

// Check if the stream is Music. This purpose is to differentiate between
// music stream and other streams like ringtones/alarm/notifications.
bool ResourceManager::isStreamMusic(
       const sp<MediaPlayerBase::AudioSink> &audioSink) {

    if (audioSink->streamType() == AUDIO_STREAM_MUSIC ) {
        ALOGV("AUDIO_STREAM_MUSIC");
        return true;
    }
    return false;
}

#else

bool  ResourceManager::isStreamMusic(
        const sp<MediaPlayerBase::AudioSink> &audioSink) {
    return true;
}

bool ResourceManager::isLPAPlayback(const sp<MediaSource> &audioTrack,
        const sp<MediaSource> &videoSource, const AudioPlayer * audioPlayer,
        const sp<MediaPlayerBase::AudioSink> &audioSink,
        int64_t &durationUs, String8 &useCase,
        bool &useCaseFlag) {
    return false;
}

#endif

#ifdef RESOURCE_MANAGER
Mutex ResourceManager::AudioConcurrencyInfo::mLock;

// Function to reset the concurrency parameter(set usecase to false) for
// usecase. This is called from destructor/reset of the clients like
// AwesomePlayer, ACodec, OMXCodec, StagefrightRecorder.
status_t ResourceManager::AudioConcurrencyInfo::resetParameter(
    String8 &useCase, bool &useCaseFlag, uint32_t codecFlags) {

    Mutex::Autolock autoLock(mLock);
    ALOGD("ResetParameter - useCase =%s, useCaseFlag = %d, codecFlags = %d",\
            useCase.string(), useCaseFlag, codecFlags);
    //If Codec flag set for LPA mode, do not reset parameter.
    //Awesome Player would call reset. Ignore the call from Codec.
    if(codecFlags & OMXCodec::kInLPAMode) {
        return OK;
    }

    //If Codec flag set for ULL mode, do not reset parameter.
    //HAL need not be informed for ULL case
    if (codecFlags & OMXCodec::kULL) {
        return OK;
    }

    //No valid use case. Do not reset
    if(useCase.isEmpty()) {
        return OK;
    }

    //use case already reset
    if(useCaseFlag == false) {
        return OK;
    }

    status_t err = ResourceManager::AudioConcurrencyInfo::setParameter(useCase, false);
    if(err != OK) {
        ALOGE("setParameter failed for wave record err =%d", err);
    }
    ResourceManager::AudioConcurrencyInfo::modifyUseCaseMetaData(useCase,
        useCaseFlag, String8(""), false);

    return NO_ERROR;
}

// Function to set concurrency parameter (usecase is set to true) for
// PCM Playback, Tunnel Playback,  LPA Playback and PCM Recording.
// For these usecases AwesomePlayer takes care of all the desicions.

status_t ResourceManager::AudioConcurrencyInfo::setNonCodecParameter(
        String8 &useCase, bool &useCaseFlag, uint32_t codecFlags, const char * mime) {

   Mutex::Autolock autoLock(mLock);
   ALOGD("setNonCodecParameter - useCase = %s, useCaseFlag = %d,\
           codecFlags = %d, mime = %s",\
           useCase.string(), useCaseFlag, codecFlags, mime);

   if(codecFlags &  OMXCodec::kULL) {
       ALOGD("ULL session no setparameter");
       return OK;
   }

   if (mime &&  !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
       ALOGD("PCM playback session");
       useCase =  ("USECASE_PCM_PLAYBACK");
   }

   if(codecFlags & OMXCodec::kInTunnelMode && useCase != "USECASE_TUNNEL_DSP_PLAYBACK") {
       ALOGD("Tunnel Playback Session");
       useCase = "USECASE_TUNNEL_DSP_PLAYBACK";
   }

   if(useCase.isEmpty()) {
       ALOGD("No Valid usecase");
       return BAD_VALUE;
   }

   if(useCaseFlag == true) {
       ALOGD("useCase = %s already set to true", useCase.string());
       return OK;
   }

   status_t err = ResourceManager::AudioConcurrencyInfo::setParameter(useCase, true);
   if(err != OK) {
         ALOGE("setParameter failed for useCase = %s , err =%d",useCase.string(), err);
         useCase = "";
         return err;
   } else {
        ResourceManager::AudioConcurrencyInfo::modifyUseCaseMetaData(useCase, useCaseFlag,
            useCase, true);
   }
   return OK;
}

// Function to set concurrency parameter (usecase is set to true) for
// all recording and playback use cases which has a decoder/encoder.
// The function is called from ACodec and OMXCodec which is the common
// code for all encoders/decoder.

status_t ResourceManager::AudioConcurrencyInfo::findUseCaseAndSetParameter(
             const char *mime, const char *componentName, bool isDecoder,
             String8 &useCase, bool &useCaseFlag, uint32_t codecFlags) {

   Mutex::Autolock autoLock(mLock);
   ALOGD("findUseCaseAndSetParameter - mime=%s,componentName=%s,isDecoder=%d",\
       mime, componentName, isDecoder);
   ALOGD("findUseCaseAndSetParameter-useCase =%s,useCaseFlag = %d, codecFlags = %d",\
       useCase.string(), useCaseFlag, codecFlags );
   if(codecFlags & OMXCodec::kInLPAMode) {
       ALOGV("In LPA mode, AwesomePlayer will set for LPA");
       return OK;
   }

   if (codecFlags & OMXCodec::kULL) {
       ALOGV("In ULL mode we dont need to inform HAL");
       return OK;
   }

   ResourceManager::AudioConcurrencyInfo::findUseCase(
                    mime, componentName, isDecoder, useCase);

   status_t err = ResourceManager::AudioConcurrencyInfo::setParameter(
                        useCase, true);
   if(err != OK) {
       ALOGE("setParameter failed for usecase = %s, err :%d", useCase.string(), err);
       useCase = "";
   }
   else {
       useCaseFlag = true;
   }
   return err;
}

// Function to update the concurrency parameter(usecase set to true /false)
// in pause/resume scenario. This API has Amessage interface and is called
// from ACodec.
status_t ResourceManager::AudioConcurrencyInfo::updateConcurrencyParam(
        const sp<AMessage> &msg, String8 &useCase, bool &useCaseFlag ) {

    Mutex::Autolock autoLock(mLock);
    int32_t streamPaused = 0;
    CHECK(msg->findInt32("streamPaused", &streamPaused));
    ALOGD("updateConcurrencyParam-useCase=%s, UseCaseFlag=%d, streamPaused=%d",
            useCase.string(), useCaseFlag, streamPaused);
    if(useCaseFlag == streamPaused) {
        status_t err = ResourceManager::AudioConcurrencyInfo::setParameter(
                useCase, !streamPaused);
        if(err) {
            ALOGE("ACodec setParameter failed for err =%d", err);
            return err;
        } else {
            useCaseFlag = !streamPaused;
        }
    }
    return OK;
}

// Function to update the concurrency parameter(usecase set to true /false)
// in pause/resume scenario. This API has is called from OMXCodec for decoders
// and AwesomePlayer for pcm and tunnel and lpa playback
status_t ResourceManager::AudioConcurrencyInfo::updateConcurrencyParam(
        String8 &useCase, bool &useCaseFlag, bool pauseflag, uint32_t codecFlag) {

    Mutex::Autolock autoLock(mLock);
    ALOGD("updateConcurrencyParam useCase=%s,useCaseFlag=%d,pauseflag=%d,codecFlag =%d",\
        useCase.string(),useCaseFlag,pauseflag,codecFlag);
    if(codecFlag & OMXCodec::kInLPAMode) {
        return OK;
    }

    if(codecFlag & OMXCodec::kULL) {
        return OK;
    }
    if(useCase.isEmpty()) {
        return OK;
    }

    status_t err = ResourceManager::AudioConcurrencyInfo::setParameter(
            useCase, !pauseflag);
    if(err != OK) {
        ALOGE("setParameter failed for err =%d", err);
    }  else {
        useCaseFlag = !pauseflag;
    }
    return err;
}

// Fucntion that calls the Audio System API and sets a usecase to Audio HAL
status_t ResourceManager::AudioConcurrencyInfo::setParameter(String8 useCase, bool value) {

    status_t err = NO_ERROR;
    if(useCase == "USECASE_ULL" || useCase.isEmpty()) {
        return NO_ERROR;
    }
    AudioParameter param = AudioParameter();
    if(value == true) {
       param.add(useCase, String8("true"));
    } else {
       param.add(useCase, String8("false"));
    }
    int64_t token = IPCThreadState::self()->clearCallingIdentity();
    err = AudioSystem::setParameters(0, param.toString());
    IPCThreadState::self()->restoreCallingIdentity(token);
    if(!err) {
       ALOGD("setParameter success for usecase = %s",useCase.string());
    } else if(err == INVALID_OPERATION) {
        ALOGE("setParameter failed usecase = %s err = %d", useCase.string(),err );
        ALOGE("Use case cannot be supported because of DSP limitation");
    } else {
        ALOGE("setParameter failed with usecase = %s err = %d",  useCase.string(), err);
    }
    return err;
}

// Function to identify the incoming use case based on mime type / component
// name and encoder /decoder flag.
void ResourceManager::AudioConcurrencyInfo::findUseCase(const char *mime,
         const char *componentName, bool isDecoder, String8 &useCase) {

    //Mutex::Autolock autoLock(mLock);
    bool isHardwareCodec = false;

    ALOGD("mime = %s, componentName = %s, isDecoder = %d", mime, componentName, isDecoder);
    //voice /voip /fm will be set from the hal  and not from stagefright
    // For LPA/Tunnel/pcm playback/pcm record player recorder will set the
    // concurrency. LPA - uses non omx software decoder. No Omxcodec for
    // tunnel playback / wav playback and recording. UseCase string is
    // supposed to be already valid in that case. So we do not need to go
    // through this function.

    //TODO : if LPA uses omx decoder we will see both trying to set ref count.
    //        this should be avoided.
    if(!useCase.isEmpty()) {
        ALOGD("useCase already set = %s", useCase.string());
        return;
    }

    if(!strncmp("OMX.qcom", componentName, 8))
        isHardwareCodec = true;

    //Case where decoding is happening
    if(isDecoder == true) {
        if(componentName != NULL) {
            if(!strncmp("OMX.qcom.audio.decoder.Qcelp13", componentName, 30))
                isHardwareCodec = false;
            if(!strncmp("OMX.qcom.audio.decoder.evrc", componentName, 27))
                isHardwareCodec = false;

            if(!strncmp(mime, "video/x-ms-wmv", 14)) {
                useCase = "USECASE_NON_TUNNEL_VIDEO_DSP_PLAYBACK";
                ALOGD("useCase = %s", useCase.string());
                return;
            }

            if(!strncmp(mime, "video/", 6)) {
                if(isHardwareCodec) {
                    ALOGD("USECASE_VIDEO_PLAYBACK");
                    useCase = "USECASE_VIDEO_PLAYBACK";
                } else {
                    //We donot need to inform HAL for software video decoder
                    useCase = "";
                    ALOGD("software decoder useCase =%s", useCase.string());
                    return;
                }
            }

            if(!strncmp(mime, "audio/x-ms-wma", 14)) {
                useCase = "USECASE_NON_TUNNEL_DSP_PLAYBACK";
                ALOGD("useCase = %s", useCase.string());
                return;
            }

            if(!strncmp(mime, "audio/", 6)) {
                useCase = "USECASE_PCM_PLAYBACK";
            }
        }
    }
    else {
        if(componentName != NULL) {
            if(!strncmp(mime, "video/", 6)) {
                if(isHardwareCodec) {
                    useCase = "USECASE_VIDEO_RECORD";
                } else {
                    //We donot need to inform HAL for software video decoder
                    useCase = "";
                    ALOGD("software video useCase = %s", useCase.string());
                    return;
                }
            }

            if(!strncmp(mime, "audio/", 6)) {
                useCase = "USECASE_PCM_RECORDING";
            }

        }
    }

    ALOGD("useCase = %s", useCase.string());
    return;
}

// Function to set/reset the flags  and usecase.
void ResourceManager::AudioConcurrencyInfo::modifyUseCaseMetaData(String8 &useCaseDst, bool &flagDst,
            String8 useCase, bool useCaseFlag) {
    if(flagDst != useCaseFlag) {
        useCaseDst =  useCase;
        flagDst = useCaseFlag;
    }
    return;
}

// Function is used to set ULL flag, This is used to skip the informing the HAL
// about the ULL usecases that are active on the device.
void ResourceManager::AudioConcurrencyInfo::setULLStream(
       const sp<MediaPlayerBase::AudioSink> &audioSink,
       uint32_t &codecFlags) {

    if(!ResourceManager::isStreamMusic(audioSink)) {
        ALOGV("Stream not music - set ULL");
        codecFlags |=  OMXCodec::kULL;
    }
}

#else

status_t ResourceManager::AudioConcurrencyInfo::resetParameter(
    String8 &useCase, bool &useCaseFlag, uint32_t codecFlags) {
    return NO_ERROR;
}

status_t ResourceManager::AudioConcurrencyInfo::setNonCodecParameter(
        String8 &useCase, bool &flag, uint32_t codecFlags, const char * mime) {
    return NO_ERROR;
}

status_t ResourceManager::AudioConcurrencyInfo::findUseCaseAndSetParameter(
             const char *mime, const char *componentName, bool isDecoder,
             String8 &useCase, bool &useCaseFlag, uint32_t codecFlags) {
    return NO_ERROR;
}

status_t ResourceManager::AudioConcurrencyInfo::updateConcurrencyParam(
        const sp<AMessage> &msg, String8 &useCase, bool &useCaseFlag ) {
    return NO_ERROR;
}

status_t ResourceManager::AudioConcurrencyInfo::updateConcurrencyParam(
                String8 &useCase, bool &useCaseFlag,
                bool pause, uint32_t codecFlag) {
    return NO_ERROR;
}
status_t ResourceManager::AudioConcurrencyInfo::setParameter(String8 useCase, bool value) {

    return NO_ERROR;
}

void ResourceManager::AudioConcurrencyInfo::findUseCase(const char *mime,
         const char *componentName, bool isDecoder, String8 &useCase) {
    return;
}

void ResourceManager::AudioConcurrencyInfo::modifyUseCaseMetaData(
         String8 &useCaseDst,  bool &flagDst, String8 useCase, bool codecFlag) {
    return;
}

void ResourceManager::AudioConcurrencyInfo::setULLStream(
        const sp<MediaPlayerBase::AudioSink> &audioSink,
        uint32_t &flags) {
    return;
}
#endif
}
