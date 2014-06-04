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

#define LOG_TAG "AudioPolicyIntefaceImpl"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include "AudioPolicyService.h"
#include "ServiceUtilities.h"

namespace android {


// ----------------------------------------------------------------------------

status_t AudioPolicyService::setDeviceConnectionState(audio_devices_t device,
                                                  audio_policy_dev_state_t state,
                                                  const char *device_address)
{
    if (mAudioPolicyManager == NULL) {
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
    return mAudioPolicyManager->setDeviceConnectionState(device,
                                                      state, device_address);
}

audio_policy_dev_state_t AudioPolicyService::getDeviceConnectionState(
                                                              audio_devices_t device,
                                                              const char *device_address)
{
    if (mAudioPolicyManager == NULL) {
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }
    return mAudioPolicyManager->getDeviceConnectionState(device,
                                                      device_address);
}

status_t AudioPolicyService::setPhoneState(audio_mode_t state)
{
    if (mAudioPolicyManager == NULL) {
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
    mAudioPolicyManager->setPhoneState(state);
    return NO_ERROR;
}

status_t AudioPolicyService::setForceUse(audio_policy_force_use_t usage,
                                         audio_policy_forced_cfg_t config)
{
    if (mAudioPolicyManager == NULL) {
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
    mAudioPolicyManager->setForceUse(usage, config);
    return NO_ERROR;
}

audio_policy_forced_cfg_t AudioPolicyService::getForceUse(audio_policy_force_use_t usage)
{
    if (mAudioPolicyManager == NULL) {
        return AUDIO_POLICY_FORCE_NONE;
    }
    if (usage < 0 || usage >= AUDIO_POLICY_FORCE_USE_CNT) {
        return AUDIO_POLICY_FORCE_NONE;
    }
    return mAudioPolicyManager->getForceUse(usage);
}

audio_io_handle_t AudioPolicyService::getOutput(audio_stream_type_t stream,
                                    uint32_t samplingRate,
                                    audio_format_t format,
                                    audio_channel_mask_t channelMask,
                                    audio_output_flags_t flags,
                                    const audio_offload_info_t *offloadInfo)
{
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    ALOGV("getOutput()");
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->getOutput(stream, samplingRate,
                                    format, channelMask, flags, offloadInfo);
}

status_t AudioPolicyService::startOutput(audio_io_handle_t output,
                                         audio_stream_type_t stream,
                                         int session)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    ALOGV("startOutput()");
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->startOutput(output, stream, session);
}

status_t AudioPolicyService::stopOutput(audio_io_handle_t output,
                                        audio_stream_type_t stream,
                                        int session)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    ALOGV("stopOutput()");
    mOutputCommandThread->stopOutputCommand(output, stream, session);
    return NO_ERROR;
}

status_t  AudioPolicyService::doStopOutput(audio_io_handle_t output,
                                      audio_stream_type_t stream,
                                      int session)
{
    ALOGV("doStopOutput from tid %d", gettid());
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->stopOutput(output, stream, session);
}

void AudioPolicyService::releaseOutput(audio_io_handle_t output)
{
    if (mAudioPolicyManager == NULL) {
        return;
    }
    ALOGV("releaseOutput()");
    mOutputCommandThread->releaseOutputCommand(output);
}

void AudioPolicyService::doReleaseOutput(audio_io_handle_t output)
{
    ALOGV("doReleaseOutput from tid %d", gettid());
    Mutex::Autolock _l(mLock);
    mAudioPolicyManager->releaseOutput(output);
}

audio_io_handle_t AudioPolicyService::getInput(audio_source_t inputSource,
                                    uint32_t samplingRate,
                                    audio_format_t format,
                                    audio_channel_mask_t channelMask,
                                    int audioSession)
{
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    // already checked by client, but double-check in case the client wrapper is bypassed
    if (inputSource >= AUDIO_SOURCE_CNT && inputSource != AUDIO_SOURCE_HOTWORD) {
        return 0;
    }

    if ((inputSource == AUDIO_SOURCE_HOTWORD) && !captureHotwordAllowed()) {
        return 0;
    }

    Mutex::Autolock _l(mLock);
    // the audio_in_acoustics_t parameter is ignored by get_input()
    audio_io_handle_t input = mAudioPolicyManager->getInput(inputSource, samplingRate,
                                                   format, channelMask, (audio_in_acoustics_t) 0);

    if (input == 0) {
        return input;
    }
    // create audio pre processors according to input source
    audio_source_t aliasSource = (inputSource == AUDIO_SOURCE_HOTWORD) ?
                                    AUDIO_SOURCE_VOICE_RECOGNITION : inputSource;

    ssize_t index = mInputSources.indexOfKey(aliasSource);
    if (index < 0) {
        return input;
    }
    ssize_t idx = mInputs.indexOfKey(input);
    InputDesc *inputDesc;
    if (idx < 0) {
        inputDesc = new InputDesc(audioSession);
        mInputs.add(input, inputDesc);
    } else {
        inputDesc = mInputs.valueAt(idx);
    }

    Vector <EffectDesc *> effects = mInputSources.valueAt(index)->mEffects;
    for (size_t i = 0; i < effects.size(); i++) {
        EffectDesc *effect = effects[i];
        sp<AudioEffect> fx = new AudioEffect(NULL, &effect->mUuid, -1, 0, 0, audioSession, input);
        status_t status = fx->initCheck();
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to create Fx %s on input %d", effect->mName, input);
            // fx goes out of scope and strong ref on AudioEffect is released
            continue;
        }
        for (size_t j = 0; j < effect->mParams.size(); j++) {
            fx->setParameter(effect->mParams[j]);
        }
        inputDesc->mEffects.add(fx);
    }
    setPreProcessorEnabled(inputDesc, true);
    return input;
}

status_t AudioPolicyService::startInput(audio_io_handle_t input)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);

    return mAudioPolicyManager->startInput(input);
}

status_t AudioPolicyService::stopInput(audio_io_handle_t input)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);

    return mAudioPolicyManager->stopInput(input);
}

void AudioPolicyService::releaseInput(audio_io_handle_t input)
{
    if (mAudioPolicyManager == NULL) {
        return;
    }
    Mutex::Autolock _l(mLock);
    mAudioPolicyManager->releaseInput(input);

    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        return;
    }
    InputDesc *inputDesc = mInputs.valueAt(index);
    setPreProcessorEnabled(inputDesc, false);
    delete inputDesc;
    mInputs.removeItemsAt(index);
}

status_t AudioPolicyService::initStreamVolume(audio_stream_type_t stream,
                                            int indexMin,
                                            int indexMax)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    mAudioPolicyManager->initStreamVolume(stream, indexMin, indexMax);
    return NO_ERROR;
}

status_t AudioPolicyService::setStreamVolumeIndex(audio_stream_type_t stream,
                                                  int index,
                                                  audio_devices_t device)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->setStreamVolumeIndex(stream,
                                                    index,
                                                    device);
}

status_t AudioPolicyService::getStreamVolumeIndex(audio_stream_type_t stream,
                                                  int *index,
                                                  audio_devices_t device)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->getStreamVolumeIndex(stream,
                                                    index,
                                                    device);
}

uint32_t AudioPolicyService::getStrategyForStream(audio_stream_type_t stream)
{
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    return mAudioPolicyManager->getStrategyForStream(stream);
}

//audio policy: use audio_device_t appropriately

audio_devices_t AudioPolicyService::getDevicesForStream(audio_stream_type_t stream)
{
    if (mAudioPolicyManager == NULL) {
        return (audio_devices_t)0;
    }
    return mAudioPolicyManager->getDevicesForStream(stream);
}

audio_io_handle_t AudioPolicyService::getOutputForEffect(const effect_descriptor_t *desc)
{
    // FIXME change return type to status_t, and return NO_INIT here
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->getOutputForEffect(desc);
}

status_t AudioPolicyService::registerEffect(const effect_descriptor_t *desc,
                                audio_io_handle_t io,
                                uint32_t strategy,
                                int session,
                                int id)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    return mAudioPolicyManager->registerEffect(desc, io, strategy, session, id);
}

status_t AudioPolicyService::unregisterEffect(int id)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    return mAudioPolicyManager->unregisterEffect(id);
}

status_t AudioPolicyService::setEffectEnabled(int id, bool enabled)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    return mAudioPolicyManager->setEffectEnabled(id, enabled);
}

bool AudioPolicyService::isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
{
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->isStreamActive(stream, inPastMs);
}

bool AudioPolicyService::isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs) const
{
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->isStreamActiveRemotely(stream, inPastMs);
}

bool AudioPolicyService::isSourceActive(audio_source_t source) const
{
    if (mAudioPolicyManager == NULL) {
        return false;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->isSourceActive(source);
}

status_t AudioPolicyService::queryDefaultPreProcessing(int audioSession,
                                                       effect_descriptor_t *descriptors,
                                                       uint32_t *count)
{

    if (mAudioPolicyManager == NULL) {
        *count = 0;
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);
    status_t status = NO_ERROR;

    size_t index;
    for (index = 0; index < mInputs.size(); index++) {
        if (mInputs.valueAt(index)->mSessionId == audioSession) {
            break;
        }
    }
    if (index == mInputs.size()) {
        *count = 0;
        return BAD_VALUE;
    }
    Vector< sp<AudioEffect> > effects = mInputs.valueAt(index)->mEffects;

    for (size_t i = 0; i < effects.size(); i++) {
        effect_descriptor_t desc = effects[i]->descriptor();
        if (i < *count) {
            descriptors[i] = desc;
        }
    }
    if (effects.size() > *count) {
        status = NO_MEMORY;
    }
    *count = effects.size();
    return status;
}

bool AudioPolicyService::isOffloadSupported(const audio_offload_info_t& info)
{
    if (mAudioPolicyManager == NULL) {
        ALOGV("mAudioPolicyManager == NULL");
        return false;
    }

    return mAudioPolicyManager->isOffloadSupported(info);
}

status_t AudioPolicyService::listAudioPorts(audio_port_role_t role,
                                            audio_port_type_t type,
                                            unsigned int *num_ports,
                                            struct audio_port *ports,
                                            unsigned int *generation)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->listAudioPorts(role, type, num_ports, ports, generation);
}

status_t AudioPolicyService::getAudioPort(struct audio_port *port)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->getAudioPort(port);
}

status_t AudioPolicyService::createAudioPatch(const struct audio_patch *patch,
        audio_patch_handle_t *handle)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    return mAudioPolicyManager->createAudioPatch(patch, handle,
                                                  IPCThreadState::self()->getCallingUid());
}

status_t AudioPolicyService::releaseAudioPatch(audio_patch_handle_t handle)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->releaseAudioPatch(handle,
                                                     IPCThreadState::self()->getCallingUid());
}

status_t AudioPolicyService::listAudioPatches(unsigned int *num_patches,
        struct audio_patch *patches,
        unsigned int *generation)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->listAudioPatches(num_patches, patches, generation);
}

status_t AudioPolicyService::setAudioPortConfig(const struct audio_port_config *config)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->setAudioPortConfig(config);
}

}; // namespace android
