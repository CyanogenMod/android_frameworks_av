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

#define LOG_NDEBUG 0
#define LOG_TAG "TrackUtils"

#include <utils/Log.h>
#include <binder/IServiceManager.h>
#include <media/IMediaPlayerService.h>
#include <media/IOMX.h>
#include <binder/IPCThreadState.h>
#include <media/AudioParameter.h>
#include <media/AudioSystem.h>

#include <TrackUtils.h>
namespace android {

Mutex TrackUtils::mLock;

#ifdef RESOURCE_MANAGER
void TrackUtils::setFastFlag(audio_stream_type_t &streamType, audio_output_flags_t &flags)
{

   //Set fast flag for ringtones/Alarm/Notification/system sound

    ALOGD("setFastFlag - flags b4 = %d , streamType = %d", flags, streamType);
    switch (streamType) {

    case AUDIO_STREAM_RING:
    case AUDIO_STREAM_ALARM:
    case AUDIO_STREAM_NOTIFICATION:
    case AUDIO_STREAM_ENFORCED_AUDIBLE:
    case AUDIO_STREAM_SYSTEM:
        /* Direct flag and fast does not go together
         * Clear deep buffer flag and set fast flag for
         * ringtones/Alarm/Notification/system sound
         * enforced audible that falls into deep buffer
         * path
         */
        ALOGD("ULL for ringtones/Alarm/Notification/system\
            sound/enforced audible");
        if(!(flags & AUDIO_OUTPUT_FLAG_LPA || flags & AUDIO_OUTPUT_FLAG_TUNNEL)) {
            flags = (audio_output_flags_t) (flags | AUDIO_OUTPUT_FLAG_FAST);
            flags = (audio_output_flags_t) (flags & ~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
        }
    break;
    default:
    // Ignore changing the flags to fast path
       ALOGD("Stream not ringtones/Alarm/Notification/system\
            sound/enforced audible");
    break;

    }
    ALOGD("setFastFlags after = %d", flags);
}

void  TrackUtils::isClientLivesLocally(bool &livesLocally)
{

    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    sp<IOMX> omx = service->getOMX();
    livesLocally = omx->livesLocally(NULL /* node */, getpid());
    ALOGD("livesLocally  = %d", livesLocally);
}

bool TrackUtils::SetConcurrencyParameterForRemotePlaybackSession(
        audio_stream_type_t &streamType,
        audio_format_t &format,
        audio_output_flags_t &flags,  bool active)
{
    bool livesLocally = 0;
    if(format != AUDIO_FORMAT_PCM_16_BIT) {
        ALOGE("Tunnel Playback please ignore");
        return OK;
    }
    isClientLivesLocally(livesLocally);
    if(livesLocally) {
        ALOGV("Lives in the media player context");
        ALOGV("Concurrency taken care by stagefright");
    }
    else {
        switch (streamType) {
            case AUDIO_STREAM_MUSIC:
            case AUDIO_STREAM_DEFAULT:
            case AUDIO_STREAM_VOICE_CALL:
            case AUDIO_STREAM_INCALL_MUSIC:
            //Can in call music come from non framework context:
            //case AUDIO_STREAM_INCALL_MUSIC:
            //We are ignoring FAST/VOIP/TUNNEL and LPA streams here.
                if(!(flags & AUDIO_OUTPUT_FLAG_VOIP_RX ||
                     flags & AUDIO_OUTPUT_FLAG_LPA ||
                     flags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                     flags & AUDIO_OUTPUT_FLAG_FAST)) {
                    ALOGD("USECASE_PCM_PLAYBACK");
                    String8 useCase ("USECASE_PCM_PLAYBACK");
                    return setParameterForConcurrency(useCase, active);
                }

            break;

            default:
                ALOGW("AudioTrack created for streamType =%d, flags =%d", streamType, flags);
                ALOGW("We donot need to inform HAL");
            break;
        }
    }
    return OK;
}

bool TrackUtils::SetConcurrencyParameterForRemoteRecordSession(
        audio_source_t inputSource, audio_format_t format, uint32_t sampleRate, uint32_t channels, bool active) {
   ALOGD("inputSource = %d, format =%d, sampleRate = %d, channels =%d,\
           active =%d", inputSource, format, sampleRate, channels, active);
    bool livesLocally = 0;
    String8 useCase;
    if(format != AUDIO_FORMAT_PCM_16_BIT) {
        ALOGV("Tunnel Record please ignore");
        return OK;
    }

    isClientLivesLocally(livesLocally);
    if(livesLocally) {
        ALOGD("Lives in the media player context");
        ALOGD("Concurrency taken care by stagefright");
    } else {

         // TODO: Use voip if skype gives voice communication always.
         // also check if voice recognition needs to be added.

        /*if(inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION) {
            useCase =  "USECASE_PCM_VOIP_CALL";
        } else */
        //Need to check mode here.
        if (inputSource == AUDIO_SOURCE_MIC ||
                  inputSource == AUDIO_SOURCE_DEFAULT ||
                  (inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION &&
                      (sampleRate != 8000 && sampleRate != 16000))) {
             ALOGD("USECASE_PCM_RECORDING");
             useCase = "USECASE_PCM_RECORDING";
             return setParameterForConcurrency(useCase, active);
        }

    }

    return OK;
}
status_t TrackUtils::setParameterForConcurrency(String8 useCase, bool value)
{
    status_t err = NO_ERROR;
    Mutex::Autolock autolock(mLock);
    if( useCase.isEmpty()) {
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

#else

void TrackUtils::setFastFlag(audio_stream_type_t &streamType, audio_output_flags_t &flags)
{
    return;
}

bool TrackUtils::SetConcurrencyParameterForRemotePlaybackSession(
        audio_stream_type_t &streamType, audio_format_t &format,
        audio_output_flags_t &flags, bool active)
{
    return OK;
}

bool TrackUtils::SetConcurrencyParameterForRemoteRecordSession(
        audio_source_t inputSource, audio_format_t format,  uint32_t sampleRate, uint32_t channels, bool active) {
    return OK;
}
status_t TrackUtils::setParameterForConcurrency(String8 useCase, bool value)
{
    return OK;
}

#endif
}
