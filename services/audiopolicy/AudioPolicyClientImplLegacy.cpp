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

static audio_io_handle_t open_output(audio_module_handle_t module,
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
        return AUDIO_IO_HANDLE_NONE;
    }

    if (pSamplingRate == NULL || pFormat == NULL || pChannelMask == NULL ||
            pDevices == NULL || pLatencyMs == NULL) {
        return AUDIO_IO_HANDLE_NONE;
    }
    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
    config.sample_rate = *pSamplingRate;
    config.format = *pFormat;
    config.channel_mask = *pChannelMask;
    if (offloadInfo != NULL) {
        config.offload_info = *offloadInfo;
    }
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    status_t status = af->openOutput(module, &output, &config, pDevices,
                                     String8(""), pLatencyMs, flags);
    if (status == NO_ERROR) {
        *pSamplingRate = config.sample_rate;
        *pFormat = config.format;
        *pChannelMask = config.channel_mask;
        if (offloadInfo != NULL) {
            *((audio_offload_info_t *)offloadInfo) = config.offload_info;
        }
    }
    return output;
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
    return open_output((audio_module_handle_t)0, pDevices, pSamplingRate, pFormat, pChannelMask,
                          pLatencyMs, flags, NULL);
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
#ifdef HAVE_PRE_KITKAT_AUDIO_POLICY_BLOB
    return open_output(module, pDevices, pSamplingRate, pFormat, pChannelMask,
                          pLatencyMs, flags, NULL);
#else
    return open_output(module, pDevices, pSamplingRate, pFormat, pChannelMask,
                          pLatencyMs, flags, offloadInfo);
#endif
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

static audio_io_handle_t open_input(audio_module_handle_t module,
                                    audio_devices_t *pDevices,
                                    uint32_t *pSamplingRate,
                                    audio_format_t *pFormat,
                                    audio_channel_mask_t *pChannelMask)
{
    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
    if (af == 0) {
        ALOGW("%s: could not get AudioFlinger", __func__);
        return AUDIO_IO_HANDLE_NONE;
    }

    if (pSamplingRate == NULL || pFormat == NULL || pChannelMask == NULL || pDevices == NULL) {
        return AUDIO_IO_HANDLE_NONE;
    }

    if (((*pDevices & AUDIO_DEVICE_IN_REMOTE_SUBMIX) == AUDIO_DEVICE_IN_REMOTE_SUBMIX)
            && !captureAudioOutputAllowed()) {
        ALOGE("open_input() permission denied: capture not allowed");
        return AUDIO_IO_HANDLE_NONE;
    }

    audio_config_t config = AUDIO_CONFIG_INITIALIZER;;
    config.sample_rate = *pSamplingRate;
    config.format = *pFormat;
    config.channel_mask = *pChannelMask;
    audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
    status_t status = af->openInput(module, &input, &config, pDevices,
                                    String8(""), AUDIO_SOURCE_MIC, AUDIO_INPUT_FLAG_FAST /*FIXME*/);
    if (status == NO_ERROR) {
        *pSamplingRate = config.sample_rate;
        *pFormat = config.format;
        *pChannelMask = config.channel_mask;
    }
    return input;
}


// deprecated: replaced by aps_open_input_on_module(), and acoustics parameter is ignored
audio_io_handle_t aps_open_input(void *service __unused,
                                        audio_devices_t *pDevices,
                                        uint32_t *pSamplingRate,
                                        audio_format_t *pFormat,
                                        audio_channel_mask_t *pChannelMask,
                                        audio_in_acoustics_t acoustics __unused)
{
    return  open_input((audio_module_handle_t)0, pDevices, pSamplingRate, pFormat, pChannelMask);
}

audio_io_handle_t aps_open_input_on_module(void *service __unused,
                                                  audio_module_handle_t module,
                                                  audio_devices_t *pDevices,
                                                  uint32_t *pSamplingRate,
                                                  audio_format_t *pFormat,
                                                  audio_channel_mask_t *pChannelMask)
{
    return  open_input(module, pDevices, pSamplingRate, pFormat, pChannelMask);
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
