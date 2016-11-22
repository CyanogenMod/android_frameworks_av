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
                                                  const char *device_address,
                                                  const char *device_name)
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
    return mAudioPolicyManager->setDeviceConnectionState(device, state,
                                                         device_address, device_name);
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

    // acquire lock before calling setMode() so that setMode() + setPhoneState() are an atomic
    // operation from policy manager standpoint (no other operation (e.g track start or stop)
    // can be interleaved).
    Mutex::Autolock _l(mLock);

    // TODO: check if it is more appropriate to do it in platform specific policy manager
    AudioSystem::setMode(state);

    mAudioPolicyManager->setPhoneState(state);
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
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return AUDIO_IO_HANDLE_NONE;
    }
    if (mAudioPolicyManager == NULL) {
        return AUDIO_IO_HANDLE_NONE;
    }
    ALOGV("getOutput()");
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->getOutput(stream, samplingRate,
                                    format, channelMask, flags, offloadInfo);
}

status_t AudioPolicyService::getOutputForAttr(const audio_attributes_t *attr,
                                              audio_io_handle_t *output,
                                              audio_session_t session,
                                              audio_stream_type_t *stream,
                                              uid_t uid,
                                              uint32_t samplingRate,
                                              audio_format_t format,
                                              audio_channel_mask_t channelMask,
                                              audio_output_flags_t flags,
                                              audio_port_handle_t selectedDeviceId,
                                              const audio_offload_info_t *offloadInfo)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    ALOGV("getOutputForAttr()");
    status_t status = NO_ERROR;
    sp<AudioPolicyEffects> audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);

        const uid_t callingUid = IPCThreadState::self()->getCallingUid();
        if (!isTrustedCallingUid(callingUid) || uid == (uid_t)-1) {
            ALOGW_IF(uid != (uid_t)-1 && uid != callingUid,
                    "%s uid %d tried to pass itself off as %d", __FUNCTION__, callingUid, uid);
            uid = callingUid;
        }
        status = mAudioPolicyManager->getOutputForAttr(attr, output, session, stream, uid, samplingRate,
                                 format, channelMask, flags, selectedDeviceId, offloadInfo);

        audioPolicyEffects = mAudioPolicyEffects;
    }

    if (status == NO_ERROR && audioPolicyEffects != 0) {
        audioPolicyEffects->updateOutputAudioSessionInfo(*output,
                *stream, session, flags, channelMask, uid);
    }

    return status;
}

status_t AudioPolicyService::startOutput(audio_io_handle_t output,
                                         audio_stream_type_t stream,
                                         audio_session_t session)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    ALOGV("startOutput()");
    return mOutputCommandThread->startOutputCommand(output, stream, session);
}

status_t AudioPolicyService::doStartOutput(audio_io_handle_t output,
                                           audio_stream_type_t stream,
                                           audio_session_t session)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    ALOGV("doStartOutput()");
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (audioPolicyEffects != 0) {
        // create audio processors according to stream
        status_t status = audioPolicyEffects->addOutputSessionEffects(output, stream, session);
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to add effects on session %d", session);
        }
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->startOutput(output, stream, session);
}

status_t AudioPolicyService::stopOutput(audio_io_handle_t output,
                                        audio_stream_type_t stream,
                                        audio_session_t session)
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    if (mAudioPolicyManager == NULL) {
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
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        audioPolicyEffects = mAudioPolicyEffects;
    }
    if (audioPolicyEffects != 0) {
        // release audio processors from the stream
        status_t status = audioPolicyEffects->releaseOutputSessionEffects(output, stream, session);
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to release effects on session %d", session);
        }
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->stopOutput(output, stream, session);
}

void AudioPolicyService::releaseOutput(audio_io_handle_t output,
                                       audio_stream_type_t stream,
                                       audio_session_t session)
{
    if (mAudioPolicyManager == NULL) {
        return;
    }
    ALOGV("releaseOutput()");
    mOutputCommandThread->releaseOutputCommand(output, stream, session);
}

void AudioPolicyService::doReleaseOutput(audio_io_handle_t output,
                                         audio_stream_type_t stream,
                                         audio_session_t session)
{
    ALOGV("doReleaseOutput from tid %d", gettid());
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        audioPolicyEffects = mAudioPolicyEffects;
        mAudioPolicyManager->releaseOutput(output, stream, session);
    }
    if (audioPolicyEffects != 0) {
        audioPolicyEffects->releaseOutputAudioSessionInfo(output,
                stream, session);
    }
}

status_t AudioPolicyService::getInputForAttr(const audio_attributes_t *attr,
                                             audio_io_handle_t *input,
                                             audio_session_t session,
                                             pid_t pid,
                                             uid_t uid,
                                             uint32_t samplingRate,
                                             audio_format_t format,
                                             audio_channel_mask_t channelMask,
                                             audio_input_flags_t flags,
                                             audio_port_handle_t selectedDeviceId)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    // already checked by client, but double-check in case the client wrapper is bypassed
    if (attr->source >= AUDIO_SOURCE_CNT && attr->source != AUDIO_SOURCE_HOTWORD &&
        attr->source != AUDIO_SOURCE_FM_TUNER) {
        return BAD_VALUE;
    }

    if ((attr->source == AUDIO_SOURCE_HOTWORD) && !captureHotwordAllowed()) {
        return BAD_VALUE;
    }
    sp<AudioPolicyEffects>audioPolicyEffects;
    status_t status;
    AudioPolicyInterface::input_type_t inputType;

    bool updatePid = (pid == -1);
    const uid_t callingUid = IPCThreadState::self()->getCallingUid();
    if (!isTrustedCallingUid(callingUid)) {
        ALOGW_IF(uid != (uid_t)-1 && uid != callingUid,
                "%s uid %d tried to pass itself off as %d", __FUNCTION__, callingUid, uid);
        uid = callingUid;
        updatePid = true;
    }

    if (updatePid) {
        const pid_t callingPid = IPCThreadState::self()->getCallingPid();
        ALOGW_IF(pid != (pid_t)-1 && pid != callingPid,
                 "%s uid %d pid %d tried to pass itself off as pid %d",
                 __func__, callingUid, callingPid, pid);
        pid = callingPid;
    }

    {
        Mutex::Autolock _l(mLock);
        // the audio_in_acoustics_t parameter is ignored by get_input()
        status = mAudioPolicyManager->getInputForAttr(attr, input, session, uid,
                                                     samplingRate, format, channelMask,
                                                     flags, selectedDeviceId,
                                                     &inputType);
        audioPolicyEffects = mAudioPolicyEffects;

        if (status == NO_ERROR) {
            // enforce permission (if any) required for each type of input
            switch (inputType) {
            case AudioPolicyInterface::API_INPUT_LEGACY:
                break;
            case AudioPolicyInterface::API_INPUT_TELEPHONY_RX:
                // FIXME: use the same permission as for remote submix for now.
            case AudioPolicyInterface::API_INPUT_MIX_CAPTURE:
                if (!isTrustedCallingUid(callingUid) && !captureAudioOutputAllowed(pid, uid)) {
                    ALOGE("getInputForAttr() permission denied: capture not allowed");
                    status = PERMISSION_DENIED;
                }
                break;
            case AudioPolicyInterface::API_INPUT_MIX_EXT_POLICY_REROUTE:
                if (!modifyAudioRoutingAllowed()) {
                    ALOGE("getInputForAttr() permission denied: modify audio routing not allowed");
                    status = PERMISSION_DENIED;
                }
                break;
            case AudioPolicyInterface::API_INPUT_INVALID:
            default:
                LOG_ALWAYS_FATAL("getInputForAttr() encountered an invalid input type %d",
                        (int)inputType);
            }
        }

        if (status != NO_ERROR) {
            if (status == PERMISSION_DENIED) {
                mAudioPolicyManager->releaseInput(*input, session);
            }
            return status;
        }
    }

    if (audioPolicyEffects != 0) {
        // create audio pre processors according to input source
        status_t status = audioPolicyEffects->addInputEffects(*input, attr->source, session);
        if (status != NO_ERROR && status != ALREADY_EXISTS) {
            ALOGW("Failed to add effects on input %d", *input);
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyService::startInput(audio_io_handle_t input,
                                        audio_session_t session)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);

    return mAudioPolicyManager->startInput(input, session);
}

status_t AudioPolicyService::stopInput(audio_io_handle_t input,
                                       audio_session_t session)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);

    return mAudioPolicyManager->stopInput(input, session);
}

void AudioPolicyService::releaseInput(audio_io_handle_t input,
                                      audio_session_t session)
{
    if (mAudioPolicyManager == NULL) {
        return;
    }
    sp<AudioPolicyEffects>audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        mAudioPolicyManager->releaseInput(input, session);
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
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
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
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
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
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->getStreamVolumeIndex(stream,
                                                    index,
                                                    device);
}

uint32_t AudioPolicyService::getStrategyForStream(audio_stream_type_t stream)
{
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return 0;
    }
    if (mAudioPolicyManager == NULL) {
        return 0;
    }
    return mAudioPolicyManager->getStrategyForStream(stream);
}

//audio policy: use audio_device_t appropriately

audio_devices_t AudioPolicyService::getDevicesForStream(audio_stream_type_t stream)
{
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return AUDIO_DEVICE_NONE;
    }
    if (mAudioPolicyManager == NULL) {
        return AUDIO_DEVICE_NONE;
    }
    Mutex::Autolock _l(mLock);
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
                                audio_session_t session,
                                int id)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mEffectsLock);
    return mAudioPolicyManager->registerEffect(desc, io, strategy, session, id);
}

status_t AudioPolicyService::unregisterEffect(int id)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mEffectsLock);
    return mAudioPolicyManager->unregisterEffect(id);
}

status_t AudioPolicyService::setEffectEnabled(int id, bool enabled)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mEffectsLock);
    return mAudioPolicyManager->setEffectEnabled(id, enabled);
}

bool AudioPolicyService::isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return false;
    }
    if (mAudioPolicyManager == NULL) {
        return false;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->isStreamActive(stream, inPastMs);
}

bool AudioPolicyService::isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT) {
        return false;
    }
    if (mAudioPolicyManager == NULL) {
        return false;
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

status_t AudioPolicyService::queryDefaultPreProcessing(audio_session_t audioSession,
                                                       effect_descriptor_t *descriptors,
                                                       uint32_t *count)
{
    if (mAudioPolicyManager == NULL) {
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
    if (mAudioPolicyManager == NULL) {
        ALOGV("mAudioPolicyManager == NULL");
        return false;
    }
    Mutex::Autolock _l(mLock);
    Mutex::Autolock _le(mEffectsLock); // isOffloadSupported queries for
                                      // non-offloadable effects
    return mAudioPolicyManager->isOffloadSupported(info);
}

status_t AudioPolicyService::listAudioPorts(audio_port_role_t role,
                                            audio_port_type_t type,
                                            unsigned int *num_ports,
                                            struct audio_port *ports,
                                            unsigned int *generation)
{
    Mutex::Autolock _l(mLock);
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->listAudioPorts(role, type, num_ports, ports, generation);
}

status_t AudioPolicyService::getAudioPort(struct audio_port *port)
{
    Mutex::Autolock _l(mLock);
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

status_t AudioPolicyService::acquireSoundTriggerSession(audio_session_t *session,
                                       audio_io_handle_t *ioHandle,
                                       audio_devices_t *device)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->acquireSoundTriggerSession(session, ioHandle, device);
}

status_t AudioPolicyService::releaseSoundTriggerSession(audio_session_t session)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->releaseSoundTriggerSession(session);
}

status_t AudioPolicyService::registerPolicyMixes(Vector<AudioMix> mixes, bool registration)
{
    Mutex::Autolock _l(mLock);
    if(!modifyAudioRoutingAllowed()) {
        return PERMISSION_DENIED;
    }
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    if (registration) {
        return mAudioPolicyManager->registerPolicyMixes(mixes);
    } else {
        return mAudioPolicyManager->unregisterPolicyMixes(mixes);
    }
}

status_t AudioPolicyService::startAudioSource(const struct audio_port_config *source,
                                  const audio_attributes_t *attributes,
                                  audio_io_handle_t *handle)
{
    Mutex::Autolock _l(mLock);
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->startAudioSource(source, attributes, handle,
                                                 IPCThreadState::self()->getCallingUid());
}

status_t AudioPolicyService::stopAudioSource(audio_io_handle_t handle)
{
    Mutex::Autolock _l(mLock);
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }

    return mAudioPolicyManager->stopAudioSource(handle);
}

status_t AudioPolicyService::setMasterMono(bool mono)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->setMasterMono(mono);
}

status_t AudioPolicyService::getMasterMono(bool *mono)
{
    if (mAudioPolicyManager == NULL) {
        return NO_INIT;
    }
    Mutex::Autolock _l(mLock);
    return mAudioPolicyManager->getMasterMono(mono);
}

status_t AudioPolicyService::listAudioSessions(audio_stream_type_t streams,
                                               Vector< sp<AudioSessionInfo>> &sessions)
{
    sp<AudioPolicyEffects> audioPolicyEffects;
    {
        Mutex::Autolock _l(mLock);
        if (mAudioPolicyManager == NULL) {
            return NO_INIT;
        }
        audioPolicyEffects = mAudioPolicyEffects;
    }

    if (audioPolicyEffects != 0) {
        return audioPolicyEffects->listAudioSessions(streams, sessions);
    }

    // no errors here if effects are not available
    return NO_ERROR;
}


}; // namespace android
