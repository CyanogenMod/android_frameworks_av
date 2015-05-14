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

#include <utils/Log.h>
#include "AudioPolicyService.h"
#include "ServiceUtilities.h"

#include <system/audio.h>
#include <system/audio_policy.h>
#include <hardware/audio_policy.h>
#include <media/AudioPolicyHelper.h>

namespace android {


// ----------------------------------------------------------------------------

status_t AudioPolicyService::setDeviceConnectionState(audio_devices_t device,
                                                  audio_policy_dev_state_t state,
                                                  const char *device_address)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (!audio_is_output_device(device) && !audio_is_input_device(device)) {
        return BAD_VALUE;
    }
    if (state != AUDIO_POLICY_DEVICE_STATE_AVAILABLE &&
            state != AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
        return BAD_VALUE;
    }

    ALOGV("setDeviceConnectionState()");
    Mutex::Autolock _l(mLock);
    return mpAudioPolicy->set_device_connection_state(mpAudioPolicy, device,
                                                      state, device_address);
}

audio_policy_dev_state_t AudioPolicyService::getDeviceConnectionState(
                                                              audio_devices_t device,
                                                              const char *device_address)
{
    if (mpAudioPolicy == NULL) {
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }
    return mpAudioPolicy->get_device_connection_state(mpAudioPolicy, device,
                                                      device_address);
}

status_t AudioPolicyService::setPhoneState(audio_mode_t state)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(state) >= AUDIO_MODE_CNT) {
        return BAD_VALUE;
    }

    ALOGV("setPhoneState()");

    // TODO: check if it is more appropriate to do it in platform specific policy manager
    AudioSystem::setMode(state);

    Mutex::Autolock _l(mLock);
    mpAudioPolicy->set_phone_state(mpAudioPolicy, state);
    mPhoneState = state;
    return NO_ERROR;
}

audio_mode_t AudioPolicyService::getPhoneState()
{
    Mutex::Autolock _l(mLock);
    return mPhoneState;
}

status_t AudioPolicyService::setForceUse(audio_policy_force_use_t usage,
                                         audio_policy_forced_cfg_t config)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (usage < 0 || usage >= AUDIO_POLICY_FORCE_USE_CNT) {
        return BAD_VALUE;
    }
    if (config < 0 || config >= AUDIO_POLICY_FORCE_CFG_CNT) {
        return BAD_VALUE;
    }
    ALOGV("setForceUse()");
    Mutex::Autolock _l(mLock);
    mpAudioPolicy->set_force_use(mpAudioPolicy, usage, config);
    return NO_ERROR;
}

audio_policy_forced_cfg_t AudioPolicyService::getForceUse(audio_policy_force_use_t usage)
{
    if (mpAudioPolicy == NULL) {
        return AUDIO_POLICY_FORCE_NONE;
    }
    if (usage < 0 || usage >= AUDIO_POLICY_FORCE_USE_CNT) {
        return AUDIO_POLICY_FORCE_NONE;
    }
    return mpAudioPolicy->get_force_use(mpAudioPolicy, usage);
}

audio_io_handle_t AudioPolicyService::getOutput(audio_stream_type_t stream,
                                    uint32_t samplingRate,
                                    audio_format_t format,
                                    audio_channel_mask_t channelMask,
                                    audio_output_flags_t flags,
                                    const audio_offload_info_t *offloadInfo)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return AUDIO_IO_HANDLE_NONE;
    }
    ALOGV("getOutput()");
    Mutex::Autolock _l(mLock);
    return mpAudioPolicy->get_output(mpAudioPolicy, stream, samplingRate,
                                    format, channelMask, flags, offloadInfo);
}

status_t AudioPolicyService::startOutput(audio_io_handle_t output,
                                         audio_stream_type_t stream,
                                         audio_session_t session)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    ALOGV("startOutput()");
    // create audio processors according to stream
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (audioPolicyEffects != 0) {
        status_t status = audioPolicyEffects->addOutputSessionEffects(output, stream, session);
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to add effects on session %d", session);
        }
    }

    Mutex::Autolock _l(mLock);
    setPowerHint(true);
    return mpAudioPolicy->start_output(mpAudioPolicy, output, stream, session);
}

status_t AudioPolicyService::stopOutput(audio_io_handle_t output,
                                        audio_stream_type_t stream,
                                        audio_session_t session)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    ALOGV("stopOutput()");
    mOutputCommandThread->stopOutputCommand(output, stream, session);
    return NO_ERROR;
}

status_t  AudioPolicyService::doStopOutput(audio_io_handle_t output,
                                      audio_stream_type_t stream,
                                      audio_session_t session)
{
    ALOGV("doStopOutput from tid %d", gettid());
    // release audio processors from the stream
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (audioPolicyEffects != 0) {
        status_t status = audioPolicyEffects->releaseOutputSessionEffects(output, stream, session);
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to release effects on session %d", session);
        }
    }
    Mutex::Autolock _l(mLock);
    status_t ret = mpAudioPolicy->stop_output(mpAudioPolicy, output, stream, session);
    setPowerHint(false);
    return ret;
}

void AudioPolicyService::releaseOutput(audio_io_handle_t output,
                                       audio_stream_type_t stream,
                                       audio_session_t session)
{
    if (mpAudioPolicy == NULL) {
        return;
    }
    ALOGV("releaseOutput()");
    mOutputCommandThread->releaseOutputCommand(output, stream, session);
}

void AudioPolicyService::doReleaseOutput(audio_io_handle_t output,
                                         audio_stream_type_t stream __unused,
                                         audio_session_t session __unused)
{
    ALOGV("doReleaseOutput from tid %d", gettid());
    Mutex::Autolock _l(mLock);
    mpAudioPolicy->release_output(mpAudioPolicy, output);
}

status_t AudioPolicyService::getInputForAttr(const audio_attributes_t *attr,
                                             audio_io_handle_t *input,
                                             audio_session_t session,
                                             uint32_t samplingRate,
                                             audio_format_t format,
                                             audio_channel_mask_t channelMask,
                                             audio_input_flags_t flags __unused)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }

    audio_source_t inputSource = attr->source;

    // already checked by client, but double-check in case the client wrapper is bypassed
    if (inputSource >= AUDIO_SOURCE_CNT && inputSource != AUDIO_SOURCE_HOTWORD &&
        inputSource != AUDIO_SOURCE_FM_TUNER) {
        return BAD_VALUE;
    }

    if (inputSource == AUDIO_SOURCE_DEFAULT) {
        inputSource = AUDIO_SOURCE_MIC;
    }

    if (((inputSource == AUDIO_SOURCE_HOTWORD) && !captureHotwordAllowed()) ||
        ((inputSource == AUDIO_SOURCE_FM_TUNER) && !captureFmTunerAllowed())) {
        return BAD_VALUE;
    }

#ifdef HAVE_PRE_KITKAT_AUDIO_POLICY_BLOB
    if (inputSource == AUDIO_SOURCE_HOTWORD)
      inputSource = AUDIO_SOURCE_VOICE_RECOGNITION;
#endif

    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        // the audio_in_acoustics_t parameter is ignored by get_input()
        *input = mpAudioPolicy->get_input(mpAudioPolicy, inputSource, samplingRate,
                                             format, channelMask, (audio_in_acoustics_t) 0);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (*input == AUDIO_IO_HANDLE_NONE) {
        return INVALID_OPERATION;
    }

    if (audioPolicyEffects != 0) {
        // create audio pre processors according to input source
        status_t status = audioPolicyEffects->addInputEffects(*input, inputSource, session);
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to add effects on input %d", input);
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyService::startInput(audio_io_handle_t input,
                                        audio_session_t session __unused)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);

    setPowerHint(true);
    return mpAudioPolicy->start_input(mpAudioPolicy, input);
}

status_t AudioPolicyService::stopInput(audio_io_handle_t input,
                                       audio_session_t session __unused)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);

    status_t ret = mpAudioPolicy->stop_input(mpAudioPolicy, input);
    setPowerHint(false);
    return ret;
}

void AudioPolicyService::releaseInput(audio_io_handle_t input,
                                      audio_session_t session __unused)
{
    if (mpAudioPolicy == NULL) {
        return;
    }

    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        mpAudioPolicy->release_input(mpAudioPolicy, input);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (audioPolicyEffects != 0) {
        // release audio processors from the input
        status_t status = audioPolicyEffects->releaseInputEffects(input);
        if(status != NO_ERROR) {
            ALOGW("Failed to release effects on input %d", input);
        }
    }
}

status_t AudioPolicyService::initStreamVolume(audio_stream_type_t stream,
                                            int indexMin,
                                            int indexMax)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    mpAudioPolicy->init_stream_volume(mpAudioPolicy, stream, indexMin, indexMax);
    return NO_ERROR;
}

status_t AudioPolicyService::setStreamVolumeIndex(audio_stream_type_t stream,
                                                  int index,
                                                  audio_devices_t device)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    if (mpAudioPolicy->set_stream_volume_index_for_device) {
        return mpAudioPolicy->set_stream_volume_index_for_device(mpAudioPolicy,
                                                                stream,
                                                                index,
                                                                device);
    } else {
        return mpAudioPolicy->set_stream_volume_index(mpAudioPolicy, stream, index);
    }
}

status_t AudioPolicyService::getStreamVolumeIndex(audio_stream_type_t stream,
                                                  int *index,
                                                  audio_devices_t device)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    if (mpAudioPolicy->get_stream_volume_index_for_device) {
        return mpAudioPolicy->get_stream_volume_index_for_device(mpAudioPolicy,
                                                                stream,
                                                                index,
                                                                device);
    } else {
        return mpAudioPolicy->get_stream_volume_index(mpAudioPolicy, stream, index);
    }
}

uint32_t AudioPolicyService::getStrategyForStream(audio_stream_type_t stream)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return 0;
    }
    return mpAudioPolicy->get_strategy_for_stream(mpAudioPolicy, stream);
}

//audio policy: use audio_device_t appropriately

audio_devices_t AudioPolicyService::getDevicesForStream(audio_stream_type_t stream)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return AUDIO_DEVICE_NONE;
    }
    return mpAudioPolicy->get_devices_for_stream(mpAudioPolicy, stream);
}

audio_io_handle_t AudioPolicyService::getOutputForEffect(const effect_descriptor_t *desc)
{
    // FIXME change return type to status_t, and return NO_INIT here
    if (mpAudioPolicy == NULL) {
        return 0;
    }
    Mutex::Autolock _l(mLock);
    return mpAudioPolicy->get_output_for_effect(mpAudioPolicy, desc);
}

status_t AudioPolicyService::registerEffect(const effect_descriptor_t *desc,
                                audio_io_handle_t io,
                                uint32_t strategy,
                                int session,
                                int id)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    return mpAudioPolicy->register_effect(mpAudioPolicy, desc, io, strategy, session, id);
}

status_t AudioPolicyService::unregisterEffect(int id)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    return mpAudioPolicy->unregister_effect(mpAudioPolicy, id);
}

status_t AudioPolicyService::setEffectEnabled(int id, bool enabled)
{
    if (mpAudioPolicy == NULL) {
        return NO_INIT;
    }
    return mpAudioPolicy->set_effect_enabled(mpAudioPolicy, id, enabled);
}

bool AudioPolicyService::isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return false;
    }
    Mutex::Autolock _l(mLock);
    return mpAudioPolicy->is_stream_active(mpAudioPolicy, stream, inPastMs);
}

bool AudioPolicyService::isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mpAudioPolicy == NULL) {
        return false;
    }
    Mutex::Autolock _l(mLock);
    return mpAudioPolicy->is_stream_active_remotely(mpAudioPolicy, stream, inPastMs);
}

bool AudioPolicyService::isSourceActive(audio_source_t source) const
{
    if (mpAudioPolicy == NULL) {
        return false;
    }
    if (mpAudioPolicy->is_source_active == 0) {
        return false;
    }
    Mutex::Autolock _l(mLock);
    return mpAudioPolicy->is_source_active(mpAudioPolicy, source);
}

status_t AudioPolicyService::queryDefaultPreProcessing(int audioSession,
                                                       effect_descriptor_t *descriptors,
                                                       uint32_t *count)
{
    if (mpAudioPolicy == NULL) {
        *count = 0;
        return NO_INIT;
    }
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (audioPolicyEffects == 0) {
        *count = 0;
        return NO_INIT;
    }
    return audioPolicyEffects->queryDefaultInputEffects(audioSession, descriptors, count);
}

bool AudioPolicyService::isOffloadSupported(const audio_offload_info_t& info)
{
#if HAVE_PRE_KITKAT_AUDIO_POLICY_BLOB
    return false;
#endif
    if (mpAudioPolicy == NULL) {
        ALOGV("mpAudioPolicy == NULL");
        return false;
    }

    if (mpAudioPolicy->is_offload_supported == NULL) {
        ALOGV("HAL does not implement is_offload_supported");
        return false;
    }

    return mpAudioPolicy->is_offload_supported(mpAudioPolicy, &info);
}

status_t AudioPolicyService::listAudioPorts(audio_port_role_t role __unused,
                                            audio_port_type_t type __unused,
                                            unsigned int *num_ports,
                                            struct audio_port *ports __unused,
                                            unsigned int *generation __unused)
{
    *num_ports = 0;
    return INVALID_OPERATION;
}

status_t AudioPolicyService::getAudioPort(struct audio_port *port __unused)
{
    return INVALID_OPERATION;
}

status_t AudioPolicyService::createAudioPatch(const struct audio_patch *patch __unused,
        audio_patch_handle_t *handle __unused)
{
    return INVALID_OPERATION;
}

status_t AudioPolicyService::releaseAudioPatch(audio_patch_handle_t handle __unused)
{
    return INVALID_OPERATION;
}

status_t AudioPolicyService::listAudioPatches(unsigned int *num_patches,
        struct audio_patch *patches __unused,
        unsigned int *generation __unused)
{
    *num_patches = 0;
    return INVALID_OPERATION;
}

status_t AudioPolicyService::setAudioPortConfig(const struct audio_port_config *config __unused)
{
    return INVALID_OPERATION;
}

status_t AudioPolicyService::getOutputForAttr(const audio_attributes_t *attr,
                                              audio_io_handle_t *output,
                                              audio_session_t session __unused,
                                              audio_stream_type_t *stream,
                                              uint32_t samplingRate,
                                              audio_format_t format,
                                              audio_channel_mask_t channelMask,
                                              audio_output_flags_t flags,
                                              const audio_offload_info_t *offloadInfo)
{
    if (attr != NULL) {
        *stream = audio_attributes_to_stream_type(attr);
    } else {
        if (*stream == AUDIO_STREAM_DEFAULT) {
            return BAD_VALUE;
        }
    }
    *output = getOutput(*stream, samplingRate, format, channelMask,
                                          flags, offloadInfo);
    if (*output == AUDIO_IO_HANDLE_NONE) {
        return INVALID_OPERATION;
    }
    return NO_ERROR;
}

status_t AudioPolicyService::acquireSoundTriggerSession(audio_session_t *session __unused,
                                       audio_io_handle_t *ioHandle __unused,
                                       audio_devices_t *device __unused)
{
    return INVALID_OPERATION;
}

status_t AudioPolicyService::releaseSoundTriggerSession(audio_session_t session __unused)
{
    return INVALID_OPERATION;
}

status_t AudioPolicyService::registerPolicyMixes(Vector<AudioMix> mixes __unused,
                                                 bool registration __unused)
{
    return INVALID_OPERATION;
}

}; // namespace android
