/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioPolicyService"
//#define LOG_NDEBUG 0

#include "Configuration.h"
#undef __STRICT_ANSI__
#define __STDINT_LIMITS
#define __STDC_LIMIT_MACROS
#include <stdint.h>

#include <sys/time.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include <binder/IPCThreadState.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include "AudioPolicyService.h"
#include "ServiceUtilities.h"
#include <hardware_legacy/power.h>
#include <media/AudioEffect.h>
#include <media/EffectsFactoryApi.h>
//#include <media/IAudioFlinger.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <hardware/audio_policy.h>
#include <audio_effects/audio_effects_conf.h>
#include <media/AudioParameter.h>


namespace android {

/* implementation of the interface to the policy manager */
extern "C" {

audio_module_handle_t aps_load_hw_module(void *service __unused,
                                             const char *name)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return 0;
    }

    return af->loadHwModule(name);
}

// deprecated: replaced by aps_open_output_on_module()
audio_io_handle_t aps_open_output(void *service __unused,
                                         audio_devices_t *pDevices,
                                         uint32_t *pSamplingRate,
                                         audio_format_t *pFormat,
                                         audio_channel_mask_t *pChannelMask,
                                         uint32_t *pLatencyMs,
                                         audio_output_flags_t flags)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return 0;
    }

    return af->openOutput((audio_module_handle_t)0, pDevices, pSamplingRate, pFormat, pChannelMask,
                          pLatencyMs, flags);
}

audio_io_handle_t aps_open_output_on_module(void *service __unused,
                                                   audio_module_handle_t module,
                                                   audio_devices_t *pDevices,
                                                   uint32_t *pSamplingRate,
                                                   audio_format_t *pFormat,
                                                   audio_channel_mask_t *pChannelMask,
                                                   uint32_t *pLatencyMs,
                                                   audio_output_flags_t flags,
                                                   const audio_offload_info_t *offloadInfo)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return 0;
    }
    return af->openOutput(module, pDevices, pSamplingRate, pFormat, pChannelMask,
                          pLatencyMs, flags, offloadInfo);
}

audio_io_handle_t aps_open_dup_output(void *service __unused,
                                                 audio_io_handle_t output1,
                                                 audio_io_handle_t output2)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return 0;
    }
    return af->openDuplicateOutput(output1, output2);
}

int aps_close_output(void *service __unused, audio_io_handle_t output)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        return PERMISSION_DENIED;
    }

    return af->closeOutput(output);
}

int aps_suspend_output(void *service __unused, audio_io_handle_t output)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return PERMISSION_DENIED;
    }

    return af->suspendOutput(output);
}

int aps_restore_output(void *service __unused, audio_io_handle_t output)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return PERMISSION_DENIED;
    }

    return af->restoreOutput(output);
}

// deprecated: replaced by aps_open_input_on_module(), and acoustics parameter is ignored
audio_io_handle_t aps_open_input(void *service __unused,
                                        audio_devices_t *pDevices,
                                        uint32_t *pSamplingRate,
                                        audio_format_t *pFormat,
                                        audio_channel_mask_t *pChannelMask,
                                        audio_in_acoustics_t acoustics __unused)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return 0;
    }

    return af->openInput((audio_module_handle_t)0, pDevices, pSamplingRate, pFormat, pChannelMask);
}

audio_io_handle_t aps_open_input_on_module(void *service __unused,
                                                  audio_module_handle_t module,
                                                  audio_devices_t *pDevices,
                                                  uint32_t *pSamplingRate,
                                                  audio_format_t *pFormat,
                                                  audio_channel_mask_t *pChannelMask)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return 0;
    }

    return af->openInput(module, pDevices, pSamplingRate, pFormat, pChannelMask);
}

int aps_close_input(void *service __unused, audio_io_handle_t input)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        return PERMISSION_DENIED;
    }

    return af->closeInput(input);
}

int aps_invalidate_stream(void *service __unused, audio_stream_type_t stream)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        return PERMISSION_DENIED;
    }

    return af->invalidateStream(stream);
}

int aps_move_effects(void *service __unused, int session,
                                audio_io_handle_t src_output,
                                audio_io_handle_t dst_output)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        return PERMISSION_DENIED;
    }

    return af->moveEffects(session, src_output, dst_output);
}

char * aps_get_parameters(void *service __unused, audio_io_handle_t io_handle,
                                     const char *keys)
{
    String8 result = AudioSystem::getParameters(io_handle, String8(keys));
    return strdup(result.string());
}

void aps_set_parameters(void *service, audio_io_handle_t io_handle,
                                   const char *kv_pairs, int delay_ms)
{
    AudioPolicyService *audioPolicyService = (AudioPolicyService *)service;

    audioPolicyService->setParameters(io_handle, kv_pairs, delay_ms);
}

int aps_set_stream_volume(void *service, audio_stream_type_t stream,
                                     float volume, audio_io_handle_t output,
                                     int delay_ms)
{
    AudioPolicyService *audioPolicyService = (AudioPolicyService *)service;

    return audioPolicyService->setStreamVolume(stream, volume, output,
                                               delay_ms);
}

int aps_start_tone(void *service, audio_policy_tone_t tone,
                              audio_stream_type_t stream)
{
    AudioPolicyService *audioPolicyService = (AudioPolicyService *)service;

    return audioPolicyService->startTone(tone, stream);
}

int aps_stop_tone(void *service)
{
    AudioPolicyService *audioPolicyService = (AudioPolicyService *)service;

    return audioPolicyService->stopTone();
}

int aps_set_voice_volume(void *service, float volume, int delay_ms)
{
    AudioPolicyService *audioPolicyService = (AudioPolicyService *)service;

    return audioPolicyService->setVoiceVolume(volume, delay_ms);
}

}; // extern "C"

}; // namespace android
