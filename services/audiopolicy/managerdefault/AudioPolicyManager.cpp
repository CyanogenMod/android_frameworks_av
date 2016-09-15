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

#define LOG_TAG "APM_AudioPolicyManager"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#define AUDIO_POLICY_XML_CONFIG_FILE "/system/etc/audio_policy_configuration.xml"

#include <inttypes.h>
#include <math.h>

#include <AudioPolicyManagerInterface.h>
#include <AudioPolicyEngineInstance.h>
#include <cutils/properties.h>
#include <utils/Log.h>
#include <hardware/audio.h>
#include <hardware/audio_effect.h>
#include <media/AudioParameter.h>
#include <media/AudioPolicyHelper.h>
#include <soundtrigger/SoundTrigger.h>
#include "AudioPolicyManager.h"
#ifndef USE_XML_AUDIO_POLICY_CONF
#include <ConfigParsingUtils.h>
#include <StreamDescriptor.h>
#endif
#include <Serializer.h>
#include "TypeConverter.h"
#include <policy.h>

namespace android {

//FIXME: workaround for truncated touch sounds
// to be removed when the problem is handled by system UI
#define TOUCH_SOUND_FIXED_DELAY_MS 100
// ----------------------------------------------------------------------------
// AudioPolicyInterface implementation
// ----------------------------------------------------------------------------

status_t AudioPolicyManager::setDeviceConnectionState(audio_devices_t device,
                                                      audio_policy_dev_state_t state,
                                                      const char *device_address,
                                                      const char *device_name)
{
    return setDeviceConnectionStateInt(device, state, device_address, device_name);
}

status_t AudioPolicyManager::setDeviceConnectionStateInt(audio_devices_t device,
                                                         audio_policy_dev_state_t state,
                                                         const char *device_address,
                                                         const char *device_name)
{
    ALOGV("setDeviceConnectionStateInt() device: 0x%X, state %d, address %s name %s",
-            device, state, device_address, device_name);

    // connect/disconnect only 1 device at a time
    if (!audio_is_output_device(device) && !audio_is_input_device(device)) return BAD_VALUE;

    sp<DeviceDescriptor> devDesc =
            mHwModules.getDeviceDescriptor(device, device_address, device_name);

    // handle output devices
    if (audio_is_output_device(device)) {
        SortedVector <audio_io_handle_t> outputs;

        ssize_t index = mAvailableOutputDevices.indexOf(devDesc);

        // save a copy of the opened output descriptors before any output is opened or closed
        // by checkOutputsForDevice(). This will be needed by checkOutputForAllStrategies()
        mPreviousOutputs = mOutputs;
        switch (state)
        {
        // handle output device connection
        case AUDIO_POLICY_DEVICE_STATE_AVAILABLE: {
            if (index >= 0) {
                ALOGW("setDeviceConnectionState() device already connected: %x", device);
                return INVALID_OPERATION;
            }
            ALOGV("setDeviceConnectionState() connecting device %x", device);

            // register new device as available
            index = mAvailableOutputDevices.add(devDesc);
            if (index >= 0) {
                sp<HwModule> module = mHwModules.getModuleForDevice(device);
                if (module == 0) {
                    ALOGD("setDeviceConnectionState() could not find HW module for device %08x",
                          device);
                    mAvailableOutputDevices.remove(devDesc);
                    return INVALID_OPERATION;
                }
                mAvailableOutputDevices[index]->attach(module);
            } else {
                return NO_MEMORY;
            }

            if (checkOutputsForDevice(devDesc, state, outputs, devDesc->mAddress) != NO_ERROR) {
                mAvailableOutputDevices.remove(devDesc);
                return INVALID_OPERATION;
            }
            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);

            // outputs should never be empty here
            ALOG_ASSERT(outputs.size() != 0, "setDeviceConnectionState():"
                    "checkOutputsForDevice() returned no outputs but status OK");
            ALOGV("setDeviceConnectionState() checkOutputsForDevice() returned %zu outputs",
                  outputs.size());

            // Send connect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_CONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

            } break;
        // handle output device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
                ALOGW("setDeviceConnectionState() device not connected: %x", device);
                return INVALID_OPERATION;
            }

            ALOGV("setDeviceConnectionState() disconnecting output device %x", device);

            // Send Disconnect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_DISCONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

            // remove device from available output devices
            mAvailableOutputDevices.remove(devDesc);

            checkOutputsForDevice(devDesc, state, outputs, devDesc->mAddress);

            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
            } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        // checkA2dpSuspend must run before checkOutputForAllStrategies so that A2DP
        // output is suspended before any tracks are moved to it
        checkA2dpSuspend();
        checkOutputForAllStrategies();
        // outputs must be closed after checkOutputForAllStrategies() is executed
        if (!outputs.isEmpty()) {
            for (size_t i = 0; i < outputs.size(); i++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(outputs[i]);
                // close unused outputs after device disconnection or direct outputs that have been
                // opened by checkOutputsForDevice() to query dynamic parameters
                if ((state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) ||
                        (((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) &&
                         (desc->mDirectOpenCount == 0))) {
                    closeOutput(outputs[i]);
                }
            }
            // check again after closing A2DP output to reset mA2dpSuspended if needed
            checkA2dpSuspend();
        }

        updateDevicesAndOutputs();
        if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL && hasPrimaryOutput()) {
            audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
            updateCallRouting(newDevice);
        }
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) || (desc != mPrimaryOutput)) {
                audio_devices_t newDevice = getNewOutputDevice(desc, true /*fromCache*/);
                // do not force device change on duplicated output because if device is 0, it will
                // also force a device 0 for the two outputs it is duplicated to which may override
                // a valid device selection on those outputs.
                bool force = !desc->isDuplicated()
                        && (!device_distinguishes_on_address(device)
                                // always force when disconnecting (a non-duplicated device)
                                || (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE));
                setOutputDevice(desc, newDevice, force, 0);
            }
        }

        if (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
            cleanUpForDevice(devDesc);
        }

        mpClientInterface->onAudioPortListUpdate();
        return NO_ERROR;
    }  // end if is output device

    // handle input devices
    if (audio_is_input_device(device)) {
        SortedVector <audio_io_handle_t> inputs;

        ssize_t index = mAvailableInputDevices.indexOf(devDesc);
        switch (state)
        {
        // handle input device connection
        case AUDIO_POLICY_DEVICE_STATE_AVAILABLE: {
            if (index >= 0) {
                ALOGW("setDeviceConnectionState() device already connected: %d", device);
                return INVALID_OPERATION;
            }
            sp<HwModule> module = mHwModules.getModuleForDevice(device);
            if (module == NULL) {
                ALOGW("setDeviceConnectionState(): could not find HW module for device %08x",
                      device);
                return INVALID_OPERATION;
            }
            if (checkInputsForDevice(devDesc, state, inputs, devDesc->mAddress) != NO_ERROR) {
                return INVALID_OPERATION;
            }

            index = mAvailableInputDevices.add(devDesc);
            if (index >= 0) {
                mAvailableInputDevices[index]->attach(module);
            } else {
                return NO_MEMORY;
            }

            // Set connect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_CONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
        } break;

        // handle input device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
                ALOGW("setDeviceConnectionState() device not connected: %d", device);
                return INVALID_OPERATION;
            }

            ALOGV("setDeviceConnectionState() disconnecting input device %x", device);

            // Set Disconnect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_DISCONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

            checkInputsForDevice(devDesc, state, inputs, devDesc->mAddress);
            mAvailableInputDevices.remove(devDesc);

            // Propagate device availability to Engine
            mEngine->setDeviceConnectionState(devDesc, state);
        } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        closeAllInputs();
        // As the input device list can impact the output device selection, update
        // getDeviceForStrategy() cache
        updateDevicesAndOutputs();

        if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL && hasPrimaryOutput()) {
            audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
            updateCallRouting(newDevice);
        }

        if (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE) {
            cleanUpForDevice(devDesc);
        }

        mpClientInterface->onAudioPortListUpdate();
        return NO_ERROR;
    } // end if is input device

    ALOGW("setDeviceConnectionState() invalid device: %x", device);
    return BAD_VALUE;
}

audio_policy_dev_state_t AudioPolicyManager::getDeviceConnectionState(audio_devices_t device,
                                                                      const char *device_address)
{
    sp<DeviceDescriptor> devDesc =
            mHwModules.getDeviceDescriptor(device, device_address, "",
                                           (strlen(device_address) != 0)/*matchAddress*/);

    if (devDesc == 0) {
        ALOGW("getDeviceConnectionState() undeclared device, type %08x, address: %s",
              device, device_address);
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }

    DeviceVector *deviceVector;

    if (audio_is_output_device(device)) {
        deviceVector = &mAvailableOutputDevices;
    } else if (audio_is_input_device(device)) {
        deviceVector = &mAvailableInputDevices;
    } else {
        ALOGW("getDeviceConnectionState() invalid device type %08x", device);
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }

    return (deviceVector->getDevice(device, String8(device_address)) != 0) ?
            AUDIO_POLICY_DEVICE_STATE_AVAILABLE : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
}

uint32_t AudioPolicyManager::updateCallRouting(audio_devices_t rxDevice, uint32_t delayMs)
{
    bool createTxPatch = false;
    status_t status;
    audio_patch_handle_t afPatchHandle;
    DeviceVector deviceList;
    uint32_t muteWaitMs = 0;

    if(!hasPrimaryOutput()) {
        return muteWaitMs;
    }
    audio_devices_t txDevice = getDeviceAndMixForInputSource(AUDIO_SOURCE_VOICE_COMMUNICATION);
    ALOGV("updateCallRouting device rxDevice %08x txDevice %08x", rxDevice, txDevice);

    // release existing RX patch if any
    if (mCallRxPatch != 0) {
        mpClientInterface->releaseAudioPatch(mCallRxPatch->mAfPatchHandle, 0);
        mCallRxPatch.clear();
    }
    // release TX patch if any
    if (mCallTxPatch != 0) {
        mpClientInterface->releaseAudioPatch(mCallTxPatch->mAfPatchHandle, 0);
        mCallTxPatch.clear();
    }

    // If the RX device is on the primary HW module, then use legacy routing method for voice calls
    // via setOutputDevice() on primary output.
    // Otherwise, create two audio patches for TX and RX path.
    if (availablePrimaryOutputDevices() & rxDevice) {
        muteWaitMs = setOutputDevice(mPrimaryOutput, rxDevice, true, delayMs);
        // If the TX device is also on the primary HW module, setOutputDevice() will take care
        // of it due to legacy implementation. If not, create a patch.
        if ((availablePrimaryInputDevices() & txDevice & ~AUDIO_DEVICE_BIT_IN)
                == AUDIO_DEVICE_NONE) {
            createTxPatch = true;
        }
    } else { // create RX path audio patch
        struct audio_patch patch;

        patch.num_sources = 1;
        patch.num_sinks = 1;
        deviceList = mAvailableOutputDevices.getDevicesFromType(rxDevice);
        ALOG_ASSERT(!deviceList.isEmpty(),
                    "updateCallRouting() selected device not in output device list");
        sp<DeviceDescriptor> rxSinkDeviceDesc = deviceList.itemAt(0);
        deviceList = mAvailableInputDevices.getDevicesFromType(AUDIO_DEVICE_IN_TELEPHONY_RX);
        ALOG_ASSERT(!deviceList.isEmpty(),
                    "updateCallRouting() no telephony RX device");
        sp<DeviceDescriptor> rxSourceDeviceDesc = deviceList.itemAt(0);

        rxSourceDeviceDesc->toAudioPortConfig(&patch.sources[0]);
        rxSinkDeviceDesc->toAudioPortConfig(&patch.sinks[0]);

        // request to reuse existing output stream if one is already opened to reach the RX device
        SortedVector<audio_io_handle_t> outputs =
                                getOutputsForDevice(rxDevice, mOutputs);
        audio_io_handle_t output = selectOutput(outputs,
                                                AUDIO_OUTPUT_FLAG_NONE,
                                                AUDIO_FORMAT_INVALID);
        if (output != AUDIO_IO_HANDLE_NONE) {
            sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "updateCallRouting() RX device output is duplicated");
            outputDesc->toAudioPortConfig(&patch.sources[1]);
            patch.sources[1].ext.mix.usecase.stream = AUDIO_STREAM_PATCH;
            patch.num_sources = 2;
        }

        afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
        status = mpClientInterface->createAudioPatch(&patch, &afPatchHandle, delayMs);
        ALOGW_IF(status != NO_ERROR, "updateCallRouting() error %d creating RX audio patch",
                                               status);
        if (status == NO_ERROR) {
            mCallRxPatch = new AudioPatch(&patch, mUidCached);
            mCallRxPatch->mAfPatchHandle = afPatchHandle;
            mCallRxPatch->mUid = mUidCached;
        }
        createTxPatch = true;
    }
    if (createTxPatch) { // create TX path audio patch
        struct audio_patch patch;

        patch.num_sources = 1;
        patch.num_sinks = 1;
        deviceList = mAvailableInputDevices.getDevicesFromType(txDevice);
        ALOG_ASSERT(!deviceList.isEmpty(),
                    "updateCallRouting() selected device not in input device list");
        sp<DeviceDescriptor> txSourceDeviceDesc = deviceList.itemAt(0);
        txSourceDeviceDesc->toAudioPortConfig(&patch.sources[0]);
        deviceList = mAvailableOutputDevices.getDevicesFromType(AUDIO_DEVICE_OUT_TELEPHONY_TX);
        ALOG_ASSERT(!deviceList.isEmpty(),
                    "updateCallRouting() no telephony TX device");
        sp<DeviceDescriptor> txSinkDeviceDesc = deviceList.itemAt(0);
        txSinkDeviceDesc->toAudioPortConfig(&patch.sinks[0]);

        SortedVector<audio_io_handle_t> outputs =
                                getOutputsForDevice(AUDIO_DEVICE_OUT_TELEPHONY_TX, mOutputs);
        audio_io_handle_t output = selectOutput(outputs,
                                                AUDIO_OUTPUT_FLAG_NONE,
                                                AUDIO_FORMAT_INVALID);
        // request to reuse existing output stream if one is already opened to reach the TX
        // path output device
        if (output != AUDIO_IO_HANDLE_NONE) {
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "updateCallRouting() RX device output is duplicated");
            outputDesc->toAudioPortConfig(&patch.sources[1]);
            patch.sources[1].ext.mix.usecase.stream = AUDIO_STREAM_PATCH;
            patch.num_sources = 2;
        }

        // terminate active capture if on the same HW module as the call TX source device
        // FIXME: would be better to refine to only inputs whose profile connects to the
        // call TX device but this information is not in the audio patch and logic here must be
        // symmetric to the one in startInput()
        audio_io_handle_t activeInput = mInputs.getActiveInput();
        if (activeInput != 0) {
            sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
            if (activeDesc->getModuleHandle() == txSourceDeviceDesc->getModuleHandle()) {
                //FIXME: consider all active sessions
                AudioSessionCollection activeSessions = activeDesc->getActiveAudioSessions();
                audio_session_t activeSession = activeSessions.keyAt(0);
                stopInput(activeInput, activeSession);
                releaseInput(activeInput, activeSession);
            }
        }

        afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
        status = mpClientInterface->createAudioPatch(&patch, &afPatchHandle, delayMs);
        ALOGW_IF(status != NO_ERROR, "setPhoneState() error %d creating TX audio patch",
                                               status);
        if (status == NO_ERROR) {
            mCallTxPatch = new AudioPatch(&patch, mUidCached);
            mCallTxPatch->mAfPatchHandle = afPatchHandle;
            mCallTxPatch->mUid = mUidCached;
        }
    }

    return muteWaitMs;
}

void AudioPolicyManager::setPhoneState(audio_mode_t state)
{
    ALOGV("setPhoneState() state %d", state);
    // store previous phone state for management of sonification strategy below
    int oldState = mEngine->getPhoneState();

    if (mEngine->setPhoneState(state) != NO_ERROR) {
        ALOGW("setPhoneState() invalid or same state %d", state);
        return;
    }
    /// Opens: can these line be executed after the switch of volume curves???
    // if leaving call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (isStateInCall(oldState)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AUDIO_STREAM_FOR_POLICY_CNT; stream++) {
            handleIncallSonification((audio_stream_type_t)stream, false, true);
        }

        // force reevaluating accessibility routing when call stops
        mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }

    /**
     * Switching to or from incall state or switching between telephony and VoIP lead to force
     * routing command.
     */
    bool force = ((is_state_in_call(oldState) != is_state_in_call(state))
                  || (is_state_in_call(state) && (state != oldState)));

    // check for device and output changes triggered by new phone state
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();

    int delayMs = 0;
    if (isStateInCall(state)) {
        nsecs_t sysTime = systemTime();
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            // mute media and sonification strategies and delay device switch by the largest
            // latency of any output where either strategy is active.
            // This avoid sending the ring tone or music tail into the earpiece or headset.
            if ((isStrategyActive(desc, STRATEGY_MEDIA,
                                  SONIFICATION_HEADSET_MUSIC_DELAY,
                                  sysTime) ||
                 isStrategyActive(desc, STRATEGY_SONIFICATION,
                                  SONIFICATION_HEADSET_MUSIC_DELAY,
                                  sysTime)) &&
                    (delayMs < (int)desc->latency()*2)) {
                delayMs = desc->latency()*2;
            }
            setStrategyMute(STRATEGY_MEDIA, true, desc);
            setStrategyMute(STRATEGY_MEDIA, false, desc, MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_MEDIA, true /*fromCache*/));
            setStrategyMute(STRATEGY_SONIFICATION, true, desc);
            setStrategyMute(STRATEGY_SONIFICATION, false, desc, MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_SONIFICATION, true /*fromCache*/));
        }
    }

    if (hasPrimaryOutput()) {
        // Note that despite the fact that getNewOutputDevice() is called on the primary output,
        // the device returned is not necessarily reachable via this output
        audio_devices_t rxDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
        // force routing command to audio hardware when ending call
        // even if no device change is needed
        if (isStateInCall(oldState) && rxDevice == AUDIO_DEVICE_NONE) {
            rxDevice = mPrimaryOutput->device();
        }

        if (state == AUDIO_MODE_IN_CALL) {
            updateCallRouting(rxDevice, delayMs);
        } else if (oldState == AUDIO_MODE_IN_CALL) {
            if (mCallRxPatch != 0) {
                mpClientInterface->releaseAudioPatch(mCallRxPatch->mAfPatchHandle, 0);
                mCallRxPatch.clear();
            }
            if (mCallTxPatch != 0) {
                mpClientInterface->releaseAudioPatch(mCallTxPatch->mAfPatchHandle, 0);
                mCallTxPatch.clear();
            }
            setOutputDevice(mPrimaryOutput, rxDevice, force, 0);
        } else {
            setOutputDevice(mPrimaryOutput, rxDevice, force, 0);
        }
    }
    // if entering in call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (isStateInCall(state)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AUDIO_STREAM_FOR_POLICY_CNT; stream++) {
            handleIncallSonification((audio_stream_type_t)stream, true, true);
        }

        // force reevaluating accessibility routing when call starts
        mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }

    // Flag that ringtone volume must be limited to music volume until we exit MODE_RINGTONE
    if (state == AUDIO_MODE_RINGTONE &&
        isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY)) {
        mLimitRingtoneVolume = true;
    } else {
        mLimitRingtoneVolume = false;
    }
}

audio_mode_t AudioPolicyManager::getPhoneState() {
    return mEngine->getPhoneState();
}

void AudioPolicyManager::setForceUse(audio_policy_force_use_t usage,
                                         audio_policy_forced_cfg_t config)
{
    ALOGV("setForceUse() usage %d, config %d, mPhoneState %d", usage, config, mEngine->getPhoneState());

    if (mEngine->setForceUse(usage, config) != NO_ERROR) {
        ALOGW("setForceUse() could not set force cfg %d for usage %d", config, usage);
        return;
    }
    bool forceVolumeReeval = (usage == AUDIO_POLICY_FORCE_FOR_COMMUNICATION) ||
            (usage == AUDIO_POLICY_FORCE_FOR_DOCK) ||
            (usage == AUDIO_POLICY_FORCE_FOR_SYSTEM);

    // check for device and output changes triggered by new force usage
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();

    //FIXME: workaround for truncated touch sounds
    // to be removed when the problem is handled by system UI
    uint32_t delayMs = 0;
    uint32_t waitMs = 0;
    if (usage == AUDIO_POLICY_FORCE_FOR_COMMUNICATION) {
        delayMs = TOUCH_SOUND_FIXED_DELAY_MS;
    }
    if (mEngine->getPhoneState() == AUDIO_MODE_IN_CALL && hasPrimaryOutput()) {
        audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, true /*fromCache*/);
        waitMs = updateCallRouting(newDevice, delayMs);
    }
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
        audio_devices_t newDevice = getNewOutputDevice(outputDesc, true /*fromCache*/);
        if ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) || (outputDesc != mPrimaryOutput)) {
            waitMs = setOutputDevice(outputDesc, newDevice, (newDevice != AUDIO_DEVICE_NONE),
                                     delayMs);
        }
        if (forceVolumeReeval && (newDevice != AUDIO_DEVICE_NONE)) {
            applyStreamVolumes(outputDesc, newDevice, waitMs, true);
        }
    }

    audio_io_handle_t activeInput = mInputs.getActiveInput();
    if (activeInput != 0) {
        sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
        audio_devices_t newDevice = getNewInputDevice(activeInput);
        // Force new input selection if the new device can not be reached via current input
        if (activeDesc->mProfile->getSupportedDevices().types() & (newDevice & ~AUDIO_DEVICE_BIT_IN)) {
            setInputDevice(activeInput, newDevice);
        } else {
            closeInput(activeInput);
        }
    }
}

void AudioPolicyManager::setSystemProperty(const char* property, const char* value)
{
    ALOGV("setSystemProperty() property %s, value %s", property, value);
}

// Find a direct output profile compatible with the parameters passed, even if the input flags do
// not explicitly request a direct output
sp<IOProfile> AudioPolicyManager::getProfileForDirectOutput(
                                                               audio_devices_t device,
                                                               uint32_t samplingRate,
                                                               audio_format_t format,
                                                               audio_channel_mask_t channelMask,
                                                               audio_output_flags_t flags)
{
    // only retain flags that will drive the direct output profile selection
    // if explicitly requested
    static const uint32_t kRelevantFlags =
            (AUDIO_OUTPUT_FLAG_HW_AV_SYNC | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
    flags =
        (audio_output_flags_t)((flags & kRelevantFlags) | AUDIO_OUTPUT_FLAG_DIRECT);

    sp<IOProfile> profile;

    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (mHwModules[i]->mHandle == 0) {
            continue;
        }
        for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++) {
            sp<IOProfile> curProfile = mHwModules[i]->mOutputProfiles[j];
            if (!curProfile->isCompatibleProfile(device, String8(""),
                    samplingRate, NULL /*updatedSamplingRate*/,
                    format, NULL /*updatedFormat*/,
                    channelMask, NULL /*updatedChannelMask*/,
                    flags)) {
                continue;
            }
            // reject profiles not corresponding to a device currently available
            if ((mAvailableOutputDevices.types() & curProfile->getSupportedDevicesType()) == 0) {
                continue;
            }
            // if several profiles are compatible, give priority to one with offload capability
            if (profile != 0 && ((curProfile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) == 0)) {
                continue;
            }
            profile = curProfile;
            if ((profile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
                break;
            }
        }
    }
    return profile;
}

audio_io_handle_t AudioPolicyManager::getOutput(audio_stream_type_t stream,
                                                uint32_t samplingRate,
                                                audio_format_t format,
                                                audio_channel_mask_t channelMask,
                                                audio_output_flags_t flags,
                                                const audio_offload_info_t *offloadInfo)
{
    routing_strategy strategy = getStrategy(stream);
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);
    ALOGV("getOutput() device %d, stream %d, samplingRate %d, format %x, channelMask %x, flags %x",
          device, stream, samplingRate, format, channelMask, flags);

    return getOutputForDevice(device, AUDIO_SESSION_ALLOCATE,
                              stream, samplingRate,format, channelMask,
                              flags, offloadInfo);
}

status_t AudioPolicyManager::getOutputForAttr(const audio_attributes_t *attr,
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
    audio_attributes_t attributes;
    if (attr != NULL) {
        if (!isValidAttributes(attr)) {
            ALOGE("getOutputForAttr() invalid attributes: usage=%d content=%d flags=0x%x tags=[%s]",
                  attr->usage, attr->content_type, attr->flags,
                  attr->tags);
            return BAD_VALUE;
        }
        attributes = *attr;
    } else {
        if (*stream < AUDIO_STREAM_MIN || *stream >= AUDIO_STREAM_PUBLIC_CNT) {
            ALOGE("getOutputForAttr():  invalid stream type");
            return BAD_VALUE;
        }
        stream_type_to_audio_attributes(*stream, &attributes);
    }
    sp<SwAudioOutputDescriptor> desc;
    if (mPolicyMixes.getOutputForAttr(attributes, uid, desc) == NO_ERROR) {
        ALOG_ASSERT(desc != 0, "Invalid desc returned by getOutputForAttr");
        if (!audio_has_proportional_frames(format)) {
            return BAD_VALUE;
        }
        *stream = streamTypefromAttributesInt(&attributes);
        *output = desc->mIoHandle;
        ALOGV("getOutputForAttr() returns output %d", *output);
        return NO_ERROR;
    }
    if (attributes.usage == AUDIO_USAGE_VIRTUAL_SOURCE) {
        ALOGW("getOutputForAttr() no policy mix found for usage AUDIO_USAGE_VIRTUAL_SOURCE");
        return BAD_VALUE;
    }

    ALOGV("getOutputForAttr() usage=%d, content=%d, tag=%s flags=%08x"
            " session %d selectedDeviceId %d",
            attributes.usage, attributes.content_type, attributes.tags, attributes.flags,
            session, selectedDeviceId);

    *stream = streamTypefromAttributesInt(&attributes);

    // Explicit routing?
    sp<DeviceDescriptor> deviceDesc;
    for (size_t i = 0; i < mAvailableOutputDevices.size(); i++) {
        if (mAvailableOutputDevices[i]->getId() == selectedDeviceId) {
            deviceDesc = mAvailableOutputDevices[i];
            break;
        }
    }
    mOutputRoutes.addRoute(session, *stream, SessionRoute::SOURCE_TYPE_NA, deviceDesc, uid);

    routing_strategy strategy = (routing_strategy) getStrategyForAttr(&attributes);
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);

    if ((attributes.flags & AUDIO_FLAG_HW_AV_SYNC) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
    }

    ALOGV("getOutputForAttr() device 0x%x, samplingRate %d, format %x, channelMask %x, flags %x",
          device, samplingRate, format, channelMask, flags);

    *output = getOutputForDevice(device, session, *stream,
                                 samplingRate, format, channelMask,
                                 flags, offloadInfo);
    if (*output == AUDIO_IO_HANDLE_NONE) {
        mOutputRoutes.removeRoute(session);
        return INVALID_OPERATION;
    }

    return NO_ERROR;
}

audio_io_handle_t AudioPolicyManager::getOutputForDevice(
        audio_devices_t device,
        audio_session_t session __unused,
        audio_stream_type_t stream,
        uint32_t samplingRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        audio_output_flags_t flags,
        const audio_offload_info_t *offloadInfo)
{
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    status_t status;

#ifdef AUDIO_POLICY_TEST
    if (mCurOutput != 0) {
        ALOGV("getOutput() test output mCurOutput %d, samplingRate %d, format %d, channelMask %x, mDirectOutput %d",
                mCurOutput, mTestSamplingRate, mTestFormat, mTestChannels, mDirectOutput);

        if (mTestOutputs[mCurOutput] == 0) {
            ALOGV("getOutput() opening test output");
            sp<AudioOutputDescriptor> outputDesc = new SwAudioOutputDescriptor(NULL,
                                                                               mpClientInterface);
            outputDesc->mDevice = mTestDevice;
            outputDesc->mLatency = mTestLatencyMs;
            outputDesc->mFlags =
                    (audio_output_flags_t)(mDirectOutput ? AUDIO_OUTPUT_FLAG_DIRECT : 0);
            outputDesc->mRefCount[stream] = 0;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = mTestSamplingRate;
            config.channel_mask = mTestChannels;
            config.format = mTestFormat;
            if (offloadInfo != NULL) {
                config.offload_info = *offloadInfo;
            }
            status = mpClientInterface->openOutput(0,
                                                  &mTestOutputs[mCurOutput],
                                                  &config,
                                                  &outputDesc->mDevice,
                                                  String8(""),
                                                  &outputDesc->mLatency,
                                                  outputDesc->mFlags);
            if (status == NO_ERROR) {
                outputDesc->mSamplingRate = config.sample_rate;
                outputDesc->mFormat = config.format;
                outputDesc->mChannelMask = config.channel_mask;
                AudioParameter outputCmd = AudioParameter();
                outputCmd.addInt(String8("set_id"),mCurOutput);
                mpClientInterface->setParameters(mTestOutputs[mCurOutput],outputCmd.toString());
                addOutput(mTestOutputs[mCurOutput], outputDesc);
            }
        }
        return mTestOutputs[mCurOutput];
    }
#endif //AUDIO_POLICY_TEST

    // open a direct output if required by specified parameters
    //force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    if ((flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    // only allow deep buffering for music stream type
    if (stream != AUDIO_STREAM_MUSIC) {
        flags = (audio_output_flags_t)(flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
    } else if (/* stream == AUDIO_STREAM_MUSIC && */
            flags == AUDIO_OUTPUT_FLAG_NONE &&
            property_get_bool("audio.deep_buffer.media", false /* default_value */)) {
        // use DEEP_BUFFER as default output for music stream type
        flags = (audio_output_flags_t)AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    }
    if (stream == AUDIO_STREAM_TTS) {
        flags = AUDIO_OUTPUT_FLAG_TTS;
    }

    sp<IOProfile> profile;

    // skip direct output selection if the request can obviously be attached to a mixed output
    // and not explicitly requested
    if (((flags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) &&
            audio_is_linear_pcm(format) && samplingRate <= SAMPLE_RATE_HZ_MAX &&
            audio_channel_count_from_out_mask(channelMask) <= 2) {
        goto non_direct_output;
    }

    // Do not allow offloading if one non offloadable effect is enabled or MasterMono is enabled.
    // This prevents creating an offloaded track and tearing it down immediately after start
    // when audioflinger detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.

    if (((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) == 0) ||
            !(mEffects.isNonOffloadableEffectEnabled() || mMasterMono)) {
        profile = getProfileForDirectOutput(device,
                                           samplingRate,
                                           format,
                                           channelMask,
                                           (audio_output_flags_t)flags);
    }

    if (profile != 0) {
        sp<SwAudioOutputDescriptor> outputDesc = NULL;

        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (profile == desc->mProfile)) {
                outputDesc = desc;
                // reuse direct output if currently open and configured with same parameters
                if ((samplingRate == outputDesc->mSamplingRate) &&
                        audio_formats_match(format, outputDesc->mFormat) &&
                        (channelMask == outputDesc->mChannelMask)) {
                    outputDesc->mDirectOpenCount++;
                    ALOGV("getOutput() reusing direct output %d", mOutputs.keyAt(i));
                    return mOutputs.keyAt(i);
                }
            }
        }
        // close direct output if currently open and configured with different parameters
        if (outputDesc != NULL) {
            closeOutput(outputDesc->mIoHandle);
        }

        // if the selected profile is offloaded and no offload info was specified,
        // create a default one
        audio_offload_info_t defaultOffloadInfo = AUDIO_INFO_INITIALIZER;
        if ((profile->getFlags() & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) && !offloadInfo) {
            flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
            defaultOffloadInfo.sample_rate = samplingRate;
            defaultOffloadInfo.channel_mask = channelMask;
            defaultOffloadInfo.format = format;
            defaultOffloadInfo.stream_type = stream;
            defaultOffloadInfo.bit_rate = 0;
            defaultOffloadInfo.duration_us = -1;
            defaultOffloadInfo.has_video = true; // conservative
            defaultOffloadInfo.is_streaming = true; // likely
            offloadInfo = &defaultOffloadInfo;
        }

        outputDesc = new SwAudioOutputDescriptor(profile, mpClientInterface);
        outputDesc->mDevice = device;
        outputDesc->mLatency = 0;
        outputDesc->mFlags = (audio_output_flags_t)(outputDesc->mFlags | flags);
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        config.sample_rate = samplingRate;
        config.channel_mask = channelMask;
        config.format = format;
        if (offloadInfo != NULL) {
            config.offload_info = *offloadInfo;
        }
        status = mpClientInterface->openOutput(profile->getModuleHandle(),
                                               &output,
                                               &config,
                                               &outputDesc->mDevice,
                                               String8(""),
                                               &outputDesc->mLatency,
                                               outputDesc->mFlags);

        // only accept an output with the requested parameters
        if (status != NO_ERROR ||
            (samplingRate != 0 && samplingRate != config.sample_rate) ||
            (format != AUDIO_FORMAT_DEFAULT && !audio_formats_match(format, config.format)) ||
            (channelMask != 0 && channelMask != config.channel_mask)) {
            ALOGV("getOutput() failed opening direct output: output %d samplingRate %d %d,"
                    "format %d %d, channelMask %04x %04x", output, samplingRate,
                    outputDesc->mSamplingRate, format, outputDesc->mFormat, channelMask,
                    outputDesc->mChannelMask);
            if (output != AUDIO_IO_HANDLE_NONE) {
                mpClientInterface->closeOutput(output);
            }
            // fall back to mixer output if possible when the direct output could not be open
            if (audio_is_linear_pcm(format) && samplingRate <= SAMPLE_RATE_HZ_MAX) {
                goto non_direct_output;
            }
            return AUDIO_IO_HANDLE_NONE;
        }
        outputDesc->mSamplingRate = config.sample_rate;
        outputDesc->mChannelMask = config.channel_mask;
        outputDesc->mFormat = config.format;
        outputDesc->mRefCount[stream] = 0;
        outputDesc->mStopTime[stream] = 0;
        outputDesc->mDirectOpenCount = 1;

        audio_io_handle_t srcOutput = getOutputForEffect();
        addOutput(output, outputDesc);
        audio_io_handle_t dstOutput = getOutputForEffect();
        if (dstOutput == output) {
            mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, srcOutput, dstOutput);
        }
        mPreviousOutputs = mOutputs;
        ALOGV("getOutput() returns new direct output %d", output);
        mpClientInterface->onAudioPortListUpdate();
        return output;
    }

non_direct_output:

    // A request for HW A/V sync cannot fallback to a mixed output because time
    // stamps are embedded in audio data
    if ((flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) {
        return AUDIO_IO_HANDLE_NONE;
    }

    // ignoring channel mask due to downmix capability in mixer

    // open a non direct output

    // for non direct outputs, only PCM is supported
    if (audio_is_linear_pcm(format)) {
        // get which output is suitable for the specified stream. The actual
        // routing change will happen when startOutput() will be called
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(device, mOutputs);

        // at this stage we should ignore the DIRECT flag as no direct output could be found earlier
        flags = (audio_output_flags_t)(flags & ~AUDIO_OUTPUT_FLAG_DIRECT);
        output = selectOutput(outputs, flags, format);
    }
    ALOGW_IF((output == 0), "getOutput() could not find output for stream %d, samplingRate %d,"
            "format %d, channels %x, flags %x", stream, samplingRate, format, channelMask, flags);

    ALOGV("  getOutputForDevice() returns output %d", output);

    return output;
}

audio_io_handle_t AudioPolicyManager::selectOutput(const SortedVector<audio_io_handle_t>& outputs,
                                                       audio_output_flags_t flags,
                                                       audio_format_t format)
{
    // select one output among several that provide a path to a particular device or set of
    // devices (the list was previously build by getOutputsForDevice()).
    // The priority is as follows:
    // 1: the output with the highest number of requested policy flags
    // 2: the output with the bit depth the closest to the requested one
    // 3: the primary output
    // 4: the first output in the list

    if (outputs.size() == 0) {
        return 0;
    }
    if (outputs.size() == 1) {
        return outputs[0];
    }

    int maxCommonFlags = 0;
    audio_io_handle_t outputForFlags = 0;
    audio_io_handle_t outputForPrimary = 0;
    audio_io_handle_t outputForFormat = 0;
    audio_format_t bestFormat = AUDIO_FORMAT_INVALID;
    audio_format_t bestFormatForFlags = AUDIO_FORMAT_INVALID;

    for (size_t i = 0; i < outputs.size(); i++) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(outputs[i]);
        if (!outputDesc->isDuplicated()) {
            // if a valid format is specified, skip output if not compatible
            if (format != AUDIO_FORMAT_INVALID) {
                if (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
                    if (!audio_formats_match(format, outputDesc->mFormat)) {
                        continue;
                    }
                } else if (!audio_is_linear_pcm(format)) {
                    continue;
                }
                if (AudioPort::isBetterFormatMatch(
                        outputDesc->mFormat, bestFormat, format)) {
                    outputForFormat = outputs[i];
                    bestFormat = outputDesc->mFormat;
                }
            }

            int commonFlags = popcount(outputDesc->mProfile->getFlags() & flags);
            if (commonFlags >= maxCommonFlags) {
                if (commonFlags == maxCommonFlags) {
                    if (AudioPort::isBetterFormatMatch(
                            outputDesc->mFormat, bestFormatForFlags, format)) {
                        outputForFlags = outputs[i];
                        bestFormatForFlags = outputDesc->mFormat;
                    }
                } else {
                    outputForFlags = outputs[i];
                    maxCommonFlags = commonFlags;
                    bestFormatForFlags = outputDesc->mFormat;
                }
                ALOGV("selectOutput() commonFlags for output %d, %04x", outputs[i], commonFlags);
            }
            if (outputDesc->mProfile->getFlags() & AUDIO_OUTPUT_FLAG_PRIMARY) {
                outputForPrimary = outputs[i];
            }
        }
    }

    if (outputForFlags != 0) {
        return outputForFlags;
    }
    if (outputForFormat != 0) {
        return outputForFormat;
    }
    if (outputForPrimary != 0) {
        return outputForPrimary;
    }

    return outputs[0];
}

status_t AudioPolicyManager::startOutput(audio_io_handle_t output,
                                             audio_stream_type_t stream,
                                             audio_session_t session)
{
    ALOGV("startOutput() output %d, stream %d, session %d",
          output, stream, session);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("startOutput() unknown output %d", output);
        return BAD_VALUE;
    }

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);

    // Routing?
    mOutputRoutes.incRouteActivity(session);

    audio_devices_t newDevice;
    AudioMix *policyMix = NULL;
    const char *address = NULL;
    if (outputDesc->mPolicyMix != NULL) {
        policyMix = outputDesc->mPolicyMix;
        address = policyMix->mDeviceAddress.string();
        if ((policyMix->mRouteFlags & MIX_ROUTE_FLAG_RENDER) == MIX_ROUTE_FLAG_RENDER) {
            newDevice = policyMix->mDeviceType;
        } else {
            newDevice = AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
        }
    } else if (mOutputRoutes.hasRouteChanged(session)) {
        newDevice = getNewOutputDevice(outputDesc, false /*fromCache*/);
        checkStrategyRoute(getStrategy(stream), output);
    } else {
        newDevice = AUDIO_DEVICE_NONE;
    }

    uint32_t delayMs = 0;

    status_t status = startSource(outputDesc, stream, newDevice, address, &delayMs);

    if (status != NO_ERROR) {
        mOutputRoutes.decRouteActivity(session);
        return status;
    }
    // Automatically enable the remote submix input when output is started on a re routing mix
    // of type MIX_TYPE_RECORDERS
    if (audio_is_remote_submix_device(newDevice) && policyMix != NULL &&
            policyMix->mMixType == MIX_TYPE_RECORDERS) {
            setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                    AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                    address,
                    "remote-submix");
    }

    if (delayMs != 0) {
        usleep(delayMs * 1000);
    }

    return status;
}

status_t AudioPolicyManager::startSource(sp<AudioOutputDescriptor> outputDesc,
                                             audio_stream_type_t stream,
                                             audio_devices_t device,
                                             const char *address,
                                             uint32_t *delayMs)
{
    // cannot start playback of STREAM_TTS if any other output is being used
    uint32_t beaconMuteLatency = 0;

    *delayMs = 0;
    if (stream == AUDIO_STREAM_TTS) {
        ALOGV("\t found BEACON stream");
        if (!mTtsOutputAvailable && mOutputs.isAnyOutputActive(AUDIO_STREAM_TTS /*streamToIgnore*/)) {
            return INVALID_OPERATION;
        } else {
            beaconMuteLatency = handleEventForBeacon(STARTING_BEACON);
        }
    } else {
        // some playback other than beacon starts
        beaconMuteLatency = handleEventForBeacon(STARTING_OUTPUT);
    }

    // force device change if the output is inactive and no audio patch is already present.
    // check active before incrementing usage count
    bool force = !outputDesc->isActive() &&
            (outputDesc->getPatchHandle() == AUDIO_PATCH_HANDLE_NONE);

    // increment usage count for this stream on the requested output:
    // NOTE that the usage count is the same for duplicated output and hardware output which is
    // necessary for a correct control of hardware output routing by startOutput() and stopOutput()
    outputDesc->changeRefCount(stream, 1);

    if (outputDesc->mRefCount[stream] == 1 || device != AUDIO_DEVICE_NONE) {
        // starting an output being rerouted?
        if (device == AUDIO_DEVICE_NONE) {
            device = getNewOutputDevice(outputDesc, false /*fromCache*/);
        }
        routing_strategy strategy = getStrategy(stream);
        bool shouldWait = (strategy == STRATEGY_SONIFICATION) ||
                            (strategy == STRATEGY_SONIFICATION_RESPECTFUL) ||
                            (beaconMuteLatency > 0);
        uint32_t waitMs = beaconMuteLatency;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc != outputDesc) {
                // force a device change if any other output is:
                // - managed by the same hw module
                // - has a current device selection that differs from selected device.
                // - supports currently selected device
                // - has an active audio patch
                // In this case, the audio HAL must receive the new device selection so that it can
                // change the device currently selected by the other active output.
                if (outputDesc->sharesHwModuleWith(desc) &&
                        desc->device() != device &&
                        desc->supportedDevices() & device &&
                        desc->getPatchHandle() != AUDIO_PATCH_HANDLE_NONE) {
                    force = true;
                }
                // wait for audio on other active outputs to be presented when starting
                // a notification so that audio focus effect can propagate, or that a mute/unmute
                // event occurred for beacon
                uint32_t latency = desc->latency();
                if (shouldWait && desc->isActive(latency * 2) && (waitMs < latency)) {
                    waitMs = latency;
                }
            }
        }
        uint32_t muteWaitMs = setOutputDevice(outputDesc, device, force, 0, NULL, address);

        // handle special case for sonification while in call
        if (isInCall()) {
            handleIncallSonification(stream, true, false);
        }

        // apply volume rules for current stream and device if necessary
        checkAndSetVolume(stream,
                          mVolumeCurves->getVolumeIndex(stream, device),
                          outputDesc,
                          device);

        // update the outputs if starting an output with a stream that can affect notification
        // routing
        handleNotificationRoutingForStream(stream);

        // force reevaluating accessibility routing when ringtone or alarm starts
        if (strategy == STRATEGY_SONIFICATION) {
            mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
        }

        if (waitMs > muteWaitMs) {
            *delayMs = waitMs - muteWaitMs;
        }
    }

    return NO_ERROR;
}


status_t AudioPolicyManager::stopOutput(audio_io_handle_t output,
                                            audio_stream_type_t stream,
                                            audio_session_t session)
{
    ALOGV("stopOutput() output %d, stream %d, session %d", output, stream, session);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("stopOutput() unknown output %d", output);
        return BAD_VALUE;
    }

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);

    if (outputDesc->mRefCount[stream] == 1) {
        // Automatically disable the remote submix input when output is stopped on a
        // re routing mix of type MIX_TYPE_RECORDERS
        if (audio_is_remote_submix_device(outputDesc->mDevice) &&
                outputDesc->mPolicyMix != NULL &&
                outputDesc->mPolicyMix->mMixType == MIX_TYPE_RECORDERS) {
            setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                    AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                    outputDesc->mPolicyMix->mDeviceAddress,
                    "remote-submix");
        }
    }

    // Routing?
    bool forceDeviceUpdate = false;
    if (outputDesc->mRefCount[stream] > 0) {
        int activityCount = mOutputRoutes.decRouteActivity(session);
        forceDeviceUpdate = (mOutputRoutes.hasRoute(session) && (activityCount == 0));

        if (forceDeviceUpdate) {
            checkStrategyRoute(getStrategy(stream), AUDIO_IO_HANDLE_NONE);
        }
    }

    return stopSource(outputDesc, stream, forceDeviceUpdate);
}

status_t AudioPolicyManager::stopSource(sp<AudioOutputDescriptor> outputDesc,
                                            audio_stream_type_t stream,
                                            bool forceDeviceUpdate)
{
    // always handle stream stop, check which stream type is stopping
    handleEventForBeacon(stream == AUDIO_STREAM_TTS ? STOPPING_BEACON : STOPPING_OUTPUT);

    // handle special case for sonification while in call
    if (isInCall()) {
        handleIncallSonification(stream, false, false);
    }

    if (outputDesc->mRefCount[stream] > 0) {
        // decrement usage count of this stream on the output
        outputDesc->changeRefCount(stream, -1);

        // store time at which the stream was stopped - see isStreamActive()
        if (outputDesc->mRefCount[stream] == 0 || forceDeviceUpdate) {
            outputDesc->mStopTime[stream] = systemTime();
            audio_devices_t newDevice = getNewOutputDevice(outputDesc, false /*fromCache*/);
            // delay the device switch by twice the latency because stopOutput() is executed when
            // the track stop() command is received and at that time the audio track buffer can
            // still contain data that needs to be drained. The latency only covers the audio HAL
            // and kernel buffers. Also the latency does not always include additional delay in the
            // audio path (audio DSP, CODEC ...)
            setOutputDevice(outputDesc, newDevice, false, outputDesc->latency()*2);

            // force restoring the device selection on other active outputs if it differs from the
            // one being selected for this output
            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
                if (desc != outputDesc &&
                        desc->isActive() &&
                        outputDesc->sharesHwModuleWith(desc) &&
                        (newDevice != desc->device())) {
                    audio_devices_t newDevice2 = getNewOutputDevice(desc, false /*fromCache*/);
                    bool force = desc->device() != newDevice2;
                    setOutputDevice(desc,
                                    newDevice2,
                                    force,
                                    outputDesc->latency()*2);
                }
            }
            // update the outputs if stopping one with a stream that can affect notification routing
            handleNotificationRoutingForStream(stream);
        }
        return NO_ERROR;
    } else {
        ALOGW("stopOutput() refcount is already 0");
        return INVALID_OPERATION;
    }
}

void AudioPolicyManager::releaseOutput(audio_io_handle_t output,
                                       audio_stream_type_t stream __unused,
                                       audio_session_t session __unused)
{
    ALOGV("releaseOutput() %d", output);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("releaseOutput() releasing unknown output %d", output);
        return;
    }

#ifdef AUDIO_POLICY_TEST
    int testIndex = testOutputIndex(output);
    if (testIndex != 0) {
        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);
        if (outputDesc->isActive()) {
            mpClientInterface->closeOutput(output);
            removeOutput(output);
            mTestOutputs[testIndex] = 0;
        }
        return;
    }
#endif //AUDIO_POLICY_TEST

    // Routing
    mOutputRoutes.removeRoute(session);

    sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(index);
    if (desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
        if (desc->mDirectOpenCount <= 0) {
            ALOGW("releaseOutput() invalid open count %d for output %d",
                                                              desc->mDirectOpenCount, output);
            return;
        }
        if (--desc->mDirectOpenCount == 0) {
            closeOutput(output);
            // If effects where present on the output, audioflinger moved them to the primary
            // output by default: move them back to the appropriate output.
            audio_io_handle_t dstOutput = getOutputForEffect();
            if (hasPrimaryOutput() && dstOutput != mPrimaryOutput->mIoHandle) {
                mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX,
                                               mPrimaryOutput->mIoHandle, dstOutput);
            }
            mpClientInterface->onAudioPortListUpdate();
        }
    }
}


status_t AudioPolicyManager::getInputForAttr(const audio_attributes_t *attr,
                                             audio_io_handle_t *input,
                                             audio_session_t session,
                                             uid_t uid,
                                             uint32_t samplingRate,
                                             audio_format_t format,
                                             audio_channel_mask_t channelMask,
                                             audio_input_flags_t flags,
                                             audio_port_handle_t selectedDeviceId,
                                             input_type_t *inputType)
{
    ALOGV("getInputForAttr() source %d, samplingRate %d, format %d, channelMask %x,"
            "session %d, flags %#x",
          attr->source, samplingRate, format, channelMask, session, flags);

    *input = AUDIO_IO_HANDLE_NONE;
    *inputType = API_INPUT_INVALID;
    audio_devices_t device;
    // handle legacy remote submix case where the address was not always specified
    String8 address = String8("");
    audio_source_t inputSource = attr->source;
    audio_source_t halInputSource;
    AudioMix *policyMix = NULL;

    if (inputSource == AUDIO_SOURCE_DEFAULT) {
        inputSource = AUDIO_SOURCE_MIC;
    }
    halInputSource = inputSource;

    // Explicit routing?
    sp<DeviceDescriptor> deviceDesc;
    for (size_t i = 0; i < mAvailableInputDevices.size(); i++) {
        if (mAvailableInputDevices[i]->getId() == selectedDeviceId) {
            deviceDesc = mAvailableInputDevices[i];
            break;
        }
    }
    mInputRoutes.addRoute(session, SessionRoute::STREAM_TYPE_NA, inputSource, deviceDesc, uid);

    if (inputSource == AUDIO_SOURCE_REMOTE_SUBMIX &&
            strncmp(attr->tags, "addr=", strlen("addr=")) == 0) {
        status_t ret = mPolicyMixes.getInputMixForAttr(*attr, &policyMix);
        if (ret != NO_ERROR) {
            return ret;
        }
        *inputType = API_INPUT_MIX_EXT_POLICY_REROUTE;
        device = AUDIO_DEVICE_IN_REMOTE_SUBMIX;
        address = String8(attr->tags + strlen("addr="));
    } else {
        device = getDeviceAndMixForInputSource(inputSource, &policyMix);
        if (device == AUDIO_DEVICE_NONE) {
            ALOGW("getInputForAttr() could not find device for source %d", inputSource);
            return BAD_VALUE;
        }
        if (policyMix != NULL) {
            address = policyMix->mDeviceAddress;
            if (policyMix->mMixType == MIX_TYPE_RECORDERS) {
                // there is an external policy, but this input is attached to a mix of recorders,
                // meaning it receives audio injected into the framework, so the recorder doesn't
                // know about it and is therefore considered "legacy"
                *inputType = API_INPUT_LEGACY;
            } else {
                // recording a mix of players defined by an external policy, we're rerouting for
                // an external policy
                *inputType = API_INPUT_MIX_EXT_POLICY_REROUTE;
            }
        } else if (audio_is_remote_submix_device(device)) {
            address = String8("0");
            *inputType = API_INPUT_MIX_CAPTURE;
        } else if (device == AUDIO_DEVICE_IN_TELEPHONY_RX) {
            *inputType = API_INPUT_TELEPHONY_RX;
        } else {
            *inputType = API_INPUT_LEGACY;
        }

    }

    *input = getInputForDevice(device, address, session, uid, inputSource,
                               samplingRate, format, channelMask, flags,
                               policyMix);
    if (*input == AUDIO_IO_HANDLE_NONE) {
        mInputRoutes.removeRoute(session);
        return INVALID_OPERATION;
    }
    ALOGV("getInputForAttr() returns input type = %d", *inputType);
    return NO_ERROR;
}


audio_io_handle_t AudioPolicyManager::getInputForDevice(audio_devices_t device,
                                                        String8 address,
                                                        audio_session_t session,
                                                        uid_t uid,
                                                        audio_source_t inputSource,
                                                        uint32_t samplingRate,
                                                        audio_format_t format,
                                                        audio_channel_mask_t channelMask,
                                                        audio_input_flags_t flags,
                                                        AudioMix *policyMix)
{
    audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
    audio_source_t halInputSource = inputSource;
    bool isSoundTrigger = false;

    if (inputSource == AUDIO_SOURCE_HOTWORD) {
        ssize_t index = mSoundTriggerSessions.indexOfKey(session);
        if (index >= 0) {
            input = mSoundTriggerSessions.valueFor(session);
            isSoundTrigger = true;
            flags = (audio_input_flags_t)(flags | AUDIO_INPUT_FLAG_HW_HOTWORD);
            ALOGV("SoundTrigger capture on session %d input %d", session, input);
        } else {
            halInputSource = AUDIO_SOURCE_VOICE_RECOGNITION;
        }
    }

    // find a compatible input profile (not necessarily identical in parameters)
    sp<IOProfile> profile;
    // samplingRate and flags may be updated by getInputProfile
    uint32_t profileSamplingRate = (samplingRate == 0) ? SAMPLE_RATE_HZ_DEFAULT : samplingRate;
    audio_format_t profileFormat = format;
    audio_channel_mask_t profileChannelMask = channelMask;
    audio_input_flags_t profileFlags = flags;
    for (;;) {
        profile = getInputProfile(device, address,
                                  profileSamplingRate, profileFormat, profileChannelMask,
                                  profileFlags);
        if (profile != 0) {
            break; // success
        } else if (profileFlags & AUDIO_INPUT_FLAG_RAW) {
            profileFlags = (audio_input_flags_t) (profileFlags & ~AUDIO_INPUT_FLAG_RAW); // retry
        } else if (profileFlags != AUDIO_INPUT_FLAG_NONE) {
            profileFlags = AUDIO_INPUT_FLAG_NONE; // retry
        } else { // fail
            ALOGW("getInputForDevice() could not find profile for device 0x%X,"
                  "samplingRate %u, format %#x, channelMask 0x%X, flags %#x",
                    device, samplingRate, format, channelMask, flags);
            return input;
        }
    }
    // Pick input sampling rate if not specified by client
    if (samplingRate == 0) {
        samplingRate = profileSamplingRate;
    }

    if (profile->getModuleHandle() == 0) {
        ALOGE("getInputForAttr(): HW module %s not opened", profile->getModuleName());
        return input;
    }

    sp<AudioSession> audioSession = new AudioSession(session,
                                                              inputSource,
                                                              format,
                                                              samplingRate,
                                                              channelMask,
                                                              flags,
                                                              uid,
                                                              isSoundTrigger,
                                                              policyMix, mpClientInterface);

// TODO enable input reuse
#if 0
    // reuse an open input if possible
    for (size_t i = 0; i < mInputs.size(); i++) {
        sp<AudioInputDescriptor> desc = mInputs.valueAt(i);
        // reuse input if it shares the same profile and same sound trigger attribute
        if (profile == desc->mProfile &&
            isSoundTrigger == desc->isSoundTrigger()) {

            sp<AudioSession> as = desc->getAudioSession(session);
            if (as != 0) {
                // do not allow unmatching properties on same session
                if (as->matches(audioSession)) {
                    as->changeOpenCount(1);
                } else {
                    ALOGW("getInputForDevice() record with different attributes"
                          " exists for session %d", session);
                    return input;
                }
            } else {
                desc->addAudioSession(session, audioSession);
            }
            ALOGV("getInputForDevice() reusing input %d", mInputs.keyAt(i));
            return mInputs.keyAt(i);
        }
    }
#endif

    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
    config.sample_rate = profileSamplingRate;
    config.channel_mask = profileChannelMask;
    config.format = profileFormat;

    status_t status = mpClientInterface->openInput(profile->getModuleHandle(),
                                                   &input,
                                                   &config,
                                                   &device,
                                                   address,
                                                   halInputSource,
                                                   profileFlags);

    // only accept input with the exact requested set of parameters
    if (status != NO_ERROR || input == AUDIO_IO_HANDLE_NONE ||
        (profileSamplingRate != config.sample_rate) ||
        !audio_formats_match(profileFormat, config.format) ||
        (profileChannelMask != config.channel_mask)) {
        ALOGW("getInputForAttr() failed opening input: samplingRate %d"
              ", format %d, channelMask %x",
                samplingRate, format, channelMask);
        if (input != AUDIO_IO_HANDLE_NONE) {
            mpClientInterface->closeInput(input);
        }
        return AUDIO_IO_HANDLE_NONE;
    }

    sp<AudioInputDescriptor> inputDesc = new AudioInputDescriptor(profile);
    inputDesc->mSamplingRate = profileSamplingRate;
    inputDesc->mFormat = profileFormat;
    inputDesc->mChannelMask = profileChannelMask;
    inputDesc->mDevice = device;
    inputDesc->mPolicyMix = policyMix;
    inputDesc->addAudioSession(session, audioSession);

    addInput(input, inputDesc);
    mpClientInterface->onAudioPortListUpdate();

    return input;
}

status_t AudioPolicyManager::startInput(audio_io_handle_t input,
                                        audio_session_t session)
{
    ALOGV("startInput() input %d", input);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        ALOGW("startInput() unknown input %d", input);
        return BAD_VALUE;
    }
    sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(index);

    sp<AudioSession> audioSession = inputDesc->getAudioSession(session);
    if (audioSession == 0) {
        ALOGW("startInput() unknown session %d on input %d", session, input);
        return BAD_VALUE;
    }

    // virtual input devices are compatible with other input devices
    if (!is_virtual_input_device(inputDesc->mDevice)) {

        // for a non-virtual input device, check if there is another (non-virtual) active input
        audio_io_handle_t activeInput = mInputs.getActiveInput();
        if (activeInput != 0 && activeInput != input) {

            // If the already active input uses AUDIO_SOURCE_HOTWORD then it is closed,
            // otherwise the active input continues and the new input cannot be started.
            sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
            if ((activeDesc->inputSource() == AUDIO_SOURCE_HOTWORD) &&
                    !activeDesc->hasPreemptedSession(session)) {
                ALOGW("startInput(%d) preempting low-priority input %d", input, activeInput);
                //FIXME: consider all active sessions
                AudioSessionCollection activeSessions = activeDesc->getActiveAudioSessions();
                audio_session_t activeSession = activeSessions.keyAt(0);
                SortedVector<audio_session_t> sessions =
                                           activeDesc->getPreemptedSessions();
                sessions.add(activeSession);
                inputDesc->setPreemptedSessions(sessions);
                stopInput(activeInput, activeSession);
                releaseInput(activeInput, activeSession);
            } else {
                ALOGE("startInput(%d) failed: other input %d already started", input, activeInput);
                return INVALID_OPERATION;
            }
        }

        // Do not allow capture if an active voice call is using a software patch and
        // the call TX source device is on the same HW module.
        // FIXME: would be better to refine to only inputs whose profile connects to the
        // call TX device but this information is not in the audio patch
        if (mCallTxPatch != 0 &&
            inputDesc->getModuleHandle() == mCallTxPatch->mPatch.sources[0].ext.device.hw_module) {
            return INVALID_OPERATION;
        }
    }

    // Routing?
    mInputRoutes.incRouteActivity(session);

    if (!inputDesc->isActive() || mInputRoutes.hasRouteChanged(session)) {
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((inputDesc->mPolicyMix != NULL)
                && ((inputDesc->mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0)) {
            mpClientInterface->onDynamicPolicyMixStateUpdate(inputDesc->mPolicyMix->mDeviceAddress,
                    MIX_STATE_MIXING);
        }

        if (mInputs.activeInputsCount() == 0) {
            SoundTrigger::setCaptureState(true);
        }
        setInputDevice(input, getNewInputDevice(input), true /* force */);

        // automatically enable the remote submix output when input is started if not
        // used by a policy mix of type MIX_TYPE_RECORDERS
        // For remote submix (a virtual device), we open only one input per capture request.
        if (audio_is_remote_submix_device(inputDesc->mDevice)) {
            String8 address = String8("");
            if (inputDesc->mPolicyMix == NULL) {
                address = String8("0");
            } else if (inputDesc->mPolicyMix->mMixType == MIX_TYPE_PLAYERS) {
                address = inputDesc->mPolicyMix->mDeviceAddress;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        address, "remote-submix");
            }
        }
    }

    ALOGV("AudioPolicyManager::startInput() input source = %d", audioSession->inputSource());

    audioSession->changeActiveCount(1);
    return NO_ERROR;
}

status_t AudioPolicyManager::stopInput(audio_io_handle_t input,
                                       audio_session_t session)
{
    ALOGV("stopInput() input %d", input);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        ALOGW("stopInput() unknown input %d", input);
        return BAD_VALUE;
    }
    sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(index);

    sp<AudioSession> audioSession = inputDesc->getAudioSession(session);
    if (index < 0) {
        ALOGW("stopInput() unknown session %d on input %d", session, input);
        return BAD_VALUE;
    }

    if (audioSession->activeCount() == 0) {
        ALOGW("stopInput() input %d already stopped", input);
        return INVALID_OPERATION;
    }

    audioSession->changeActiveCount(-1);

    // Routing?
    mInputRoutes.decRouteActivity(session);

    if (!inputDesc->isActive()) {
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((inputDesc->mPolicyMix != NULL)
                && ((inputDesc->mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0)) {
            mpClientInterface->onDynamicPolicyMixStateUpdate(inputDesc->mPolicyMix->mDeviceAddress,
                    MIX_STATE_IDLE);
        }

        // automatically disable the remote submix output when input is stopped if not
        // used by a policy mix of type MIX_TYPE_RECORDERS
        if (audio_is_remote_submix_device(inputDesc->mDevice)) {
            String8 address = String8("");
            if (inputDesc->mPolicyMix == NULL) {
                address = String8("0");
            } else if (inputDesc->mPolicyMix->mMixType == MIX_TYPE_PLAYERS) {
                address = inputDesc->mPolicyMix->mDeviceAddress;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                                         AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                         address, "remote-submix");
            }
        }

        resetInputDevice(input);

        if (mInputs.activeInputsCount() == 0) {
            SoundTrigger::setCaptureState(false);
        }
        inputDesc->clearPreemptedSessions();
    }
    return NO_ERROR;
}

void AudioPolicyManager::releaseInput(audio_io_handle_t input,
                                      audio_session_t session)
{

    ALOGV("releaseInput() %d", input);
    ssize_t index = mInputs.indexOfKey(input);
    if (index < 0) {
        ALOGW("releaseInput() releasing unknown input %d", input);
        return;
    }

    // Routing
    mInputRoutes.removeRoute(session);

    sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(index);
    ALOG_ASSERT(inputDesc != 0);

    sp<AudioSession> audioSession = inputDesc->getAudioSession(session);
    if (index < 0) {
        ALOGW("releaseInput() unknown session %d on input %d", session, input);
        return;
    }

    if (audioSession->openCount() == 0) {
        ALOGW("releaseInput() invalid open count %d on session %d",
              audioSession->openCount(), session);
        return;
    }

    if (audioSession->changeOpenCount(-1) == 0) {
        inputDesc->removeAudioSession(session);
    }

    if (inputDesc->getOpenRefCount() > 0) {
        ALOGV("releaseInput() exit > 0");
        return;
    }

    closeInput(input);
    mpClientInterface->onAudioPortListUpdate();
    ALOGV("releaseInput() exit");
}

void AudioPolicyManager::closeAllInputs() {
    bool patchRemoved = false;

    for(size_t input_index = 0; input_index < mInputs.size(); input_index++) {
        sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(input_index);
        ssize_t patch_index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
        if (patch_index >= 0) {
            sp<AudioPatch> patchDesc = mAudioPatches.valueAt(patch_index);
            (void) /*status_t status*/ mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
            mAudioPatches.removeItemsAt(patch_index);
            patchRemoved = true;
        }
        mpClientInterface->closeInput(mInputs.keyAt(input_index));
    }
    mInputs.clear();
    SoundTrigger::setCaptureState(false);
    nextAudioPortGeneration();

    if (patchRemoved) {
        mpClientInterface->onAudioPatchListUpdate();
    }
}

void AudioPolicyManager::initStreamVolume(audio_stream_type_t stream,
                                            int indexMin,
                                            int indexMax)
{
    ALOGV("initStreamVolume() stream %d, min %d, max %d", stream , indexMin, indexMax);
    mVolumeCurves->initStreamVolume(stream, indexMin, indexMax);

    // initialize other private stream volumes which follow this one
    for (int curStream = 0; curStream < AUDIO_STREAM_FOR_POLICY_CNT; curStream++) {
        if (!streamsMatchForvolume(stream, (audio_stream_type_t)curStream)) {
            continue;
        }
        mVolumeCurves->initStreamVolume((audio_stream_type_t)curStream, indexMin, indexMax);
    }
}

status_t AudioPolicyManager::setStreamVolumeIndex(audio_stream_type_t stream,
                                                  int index,
                                                  audio_devices_t device)
{

    if ((index < mVolumeCurves->getVolumeIndexMin(stream)) ||
            (index > mVolumeCurves->getVolumeIndexMax(stream))) {
        return BAD_VALUE;
    }
    if (!audio_is_output_device(device)) {
        return BAD_VALUE;
    }

    // Force max volume if stream cannot be muted
    if (!mVolumeCurves->canBeMuted(stream)) index = mVolumeCurves->getVolumeIndexMax(stream);

    ALOGV("setStreamVolumeIndex() stream %d, device %08x, index %d",
          stream, device, index);

    // update other private stream volumes which follow this one
    for (int curStream = 0; curStream < AUDIO_STREAM_FOR_POLICY_CNT; curStream++) {
        if (!streamsMatchForvolume(stream, (audio_stream_type_t)curStream)) {
            continue;
        }
        mVolumeCurves->addCurrentVolumeIndex((audio_stream_type_t)curStream, device, index);
    }

    // update volume on all outputs and streams matching the following:
    // - The requested stream (or a stream matching for volume control) is active on the output
    // - The device (or devices) selected by the strategy corresponding to this stream includes
    // the requested device
    // - For non default requested device, currently selected device on the output is either the
    // requested device or one of the devices selected by the strategy
    // - For default requested device (AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME), apply volume only if
    // no specific device volume value exists for currently selected device.
    status_t status = NO_ERROR;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        audio_devices_t curDevice = Volume::getDeviceForVolume(desc->device());
        for (int curStream = 0; curStream < AUDIO_STREAM_FOR_POLICY_CNT; curStream++) {
            if (!streamsMatchForvolume(stream, (audio_stream_type_t)curStream)) {
                continue;
            }
            if (!(desc->isStreamActive((audio_stream_type_t)curStream) ||
                    (isInCall() && (curStream == AUDIO_STREAM_VOICE_CALL)))) {
                continue;
            }
            routing_strategy curStrategy = getStrategy((audio_stream_type_t)curStream);
            audio_devices_t curStreamDevice = getDeviceForStrategy(curStrategy, true /*fromCache*/);
            if ((curStreamDevice & device) == 0) {
                continue;
            }
            bool applyDefault = false;
            if (device != AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME) {
                curStreamDevice |= device;
            } else if (!mVolumeCurves->hasVolumeIndexForDevice(
                    stream, Volume::getDeviceForVolume(curStreamDevice))) {
                applyDefault = true;
            }

            if (applyDefault || ((curDevice & curStreamDevice) != 0)) {
                //FIXME: workaround for truncated touch sounds
                // delayed volume change for system stream to be removed when the problem is
                // handled by system UI
                status_t volStatus =
                        checkAndSetVolume((audio_stream_type_t)curStream, index, desc, curDevice,
                            (stream == AUDIO_STREAM_SYSTEM) ? TOUCH_SOUND_FIXED_DELAY_MS : 0);
                if (volStatus != NO_ERROR) {
                    status = volStatus;
                }
            }
        }
    }
    return status;
}

status_t AudioPolicyManager::getStreamVolumeIndex(audio_stream_type_t stream,
                                                      int *index,
                                                      audio_devices_t device)
{
    if (index == NULL) {
        return BAD_VALUE;
    }
    if (!audio_is_output_device(device)) {
        return BAD_VALUE;
    }
    // if device is AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME, return volume for device corresponding to
    // the strategy the stream belongs to.
    if (device == AUDIO_DEVICE_OUT_DEFAULT_FOR_VOLUME) {
        device = getDeviceForStrategy(getStrategy(stream), true /*fromCache*/);
    }
    device = Volume::getDeviceForVolume(device);

    *index =  mVolumeCurves->getVolumeIndex(stream, device);
    ALOGV("getStreamVolumeIndex() stream %d device %08x index %d", stream, device, *index);
    return NO_ERROR;
}

audio_io_handle_t AudioPolicyManager::selectOutputForEffects(
                                            const SortedVector<audio_io_handle_t>& outputs)
{
    // select one output among several suitable for global effects.
    // The priority is as follows:
    // 1: An offloaded output. If the effect ends up not being offloadable,
    //    AudioFlinger will invalidate the track and the offloaded output
    //    will be closed causing the effect to be moved to a PCM output.
    // 2: A deep buffer output
    // 3: the first output in the list

    if (outputs.size() == 0) {
        return 0;
    }

    audio_io_handle_t outputOffloaded = 0;
    audio_io_handle_t outputDeepBuffer = 0;

    for (size_t i = 0; i < outputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(outputs[i]);
        ALOGV("selectOutputForEffects outputs[%zu] flags %x", i, desc->mFlags);
        if ((desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
            outputOffloaded = outputs[i];
        }
        if ((desc->mFlags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) != 0) {
            outputDeepBuffer = outputs[i];
        }
    }

    ALOGV("selectOutputForEffects outputOffloaded %d outputDeepBuffer %d",
          outputOffloaded, outputDeepBuffer);
    if (outputOffloaded != 0) {
        return outputOffloaded;
    }
    if (outputDeepBuffer != 0) {
        return outputDeepBuffer;
    }

    return outputs[0];
}

audio_io_handle_t AudioPolicyManager::getOutputForEffect(const effect_descriptor_t *desc)
{
    // apply simple rule where global effects are attached to the same output as MUSIC streams

    routing_strategy strategy = getStrategy(AUDIO_STREAM_MUSIC);
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);
    SortedVector<audio_io_handle_t> dstOutputs = getOutputsForDevice(device, mOutputs);

    audio_io_handle_t output = selectOutputForEffects(dstOutputs);
    ALOGV("getOutputForEffect() got output %d for fx %s flags %x",
          output, (desc == NULL) ? "unspecified" : desc->name,  (desc == NULL) ? 0 : desc->flags);

    return output;
}

status_t AudioPolicyManager::registerEffect(const effect_descriptor_t *desc,
                                audio_io_handle_t io,
                                uint32_t strategy,
                                int session,
                                int id)
{
    ssize_t index = mOutputs.indexOfKey(io);
    if (index < 0) {
        index = mInputs.indexOfKey(io);
        if (index < 0) {
            ALOGW("registerEffect() unknown io %d", io);
            return INVALID_OPERATION;
        }
    }
    return mEffects.registerEffect(desc, io, strategy, session, id);
}

bool AudioPolicyManager::isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
{
    bool active = false;
    for (int curStream = 0; curStream < AUDIO_STREAM_FOR_POLICY_CNT && !active; curStream++) {
        if (!streamsMatchForvolume(stream, (audio_stream_type_t)curStream)) {
            continue;
        }
        active = mOutputs.isStreamActive((audio_stream_type_t)curStream, inPastMs);
    }
    return active;
}

bool AudioPolicyManager::isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs) const
{
    return mOutputs.isStreamActiveRemotely(stream, inPastMs);
}

bool AudioPolicyManager::isSourceActive(audio_source_t source) const
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = mInputs.valueAt(i);
        if (inputDescriptor->isSourceActive(source)) {
            return true;
        }
    }
    return false;
}

// Register a list of custom mixes with their attributes and format.
// When a mix is registered, corresponding input and output profiles are
// added to the remote submix hw module. The profile contains only the
// parameters (sampling rate, format...) specified by the mix.
// The corresponding input remote submix device is also connected.
//
// When a remote submix device is connected, the address is checked to select the
// appropriate profile and the corresponding input or output stream is opened.
//
// When capture starts, getInputForAttr() will:
//  - 1 look for a mix matching the address passed in attribtutes tags if any
//  - 2 if none found, getDeviceForInputSource() will:
//     - 2.1 look for a mix matching the attributes source
//     - 2.2 if none found, default to device selection by policy rules
// At this time, the corresponding output remote submix device is also connected
// and active playback use cases can be transferred to this mix if needed when reconnecting
// after AudioTracks are invalidated
//
// When playback starts, getOutputForAttr() will:
//  - 1 look for a mix matching the address passed in attribtutes tags if any
//  - 2 if none found, look for a mix matching the attributes usage
//  - 3 if none found, default to device and output selection by policy rules.

status_t AudioPolicyManager::registerPolicyMixes(Vector<AudioMix> mixes)
{
    ALOGV("registerPolicyMixes() %zu mix(es)", mixes.size());
    status_t res = NO_ERROR;

    sp<HwModule> rSubmixModule;
    // examine each mix's route type
    for (size_t i = 0; i < mixes.size(); i++) {
        // we only support MIX_ROUTE_FLAG_LOOP_BACK or MIX_ROUTE_FLAG_RENDER, not the combination
        if ((mixes[i].mRouteFlags & MIX_ROUTE_FLAG_ALL) == MIX_ROUTE_FLAG_ALL) {
            res = INVALID_OPERATION;
            break;
        }
        if ((mixes[i].mRouteFlags & MIX_ROUTE_FLAG_LOOP_BACK) == MIX_ROUTE_FLAG_LOOP_BACK) {
            // Loop back through "remote submix"
            if (rSubmixModule == 0) {
                for (size_t j = 0; i < mHwModules.size(); j++) {
                    if (strcmp(AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX, mHwModules[j]->mName) == 0
                            && mHwModules[j]->mHandle != 0) {
                        rSubmixModule = mHwModules[j];
                        break;
                    }
                }
            }

            ALOGV("registerPolicyMixes() mix %zu of %zu is LOOP_BACK", i, mixes.size());

            if (rSubmixModule == 0) {
                ALOGE(" Unable to find audio module for submix, aborting mix %zu registration", i);
                res = INVALID_OPERATION;
                break;
            }

            String8 address = mixes[i].mDeviceAddress;

            if (mPolicyMixes.registerMix(address, mixes[i], 0 /*output desc*/) != NO_ERROR) {
                ALOGE(" Error registering mix %zu for address %s", i, address.string());
                res = INVALID_OPERATION;
                break;
            }
            audio_config_t outputConfig = mixes[i].mFormat;
            audio_config_t inputConfig = mixes[i].mFormat;
            // NOTE: audio flinger mixer does not support mono output: configure remote submix HAL in
            // stereo and let audio flinger do the channel conversion if needed.
            outputConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
            inputConfig.channel_mask = AUDIO_CHANNEL_IN_STEREO;
            rSubmixModule->addOutputProfile(address, &outputConfig,
                    AUDIO_DEVICE_OUT_REMOTE_SUBMIX, address);
            rSubmixModule->addInputProfile(address, &inputConfig,
                    AUDIO_DEVICE_IN_REMOTE_SUBMIX, address);

            if (mixes[i].mMixType == MIX_TYPE_PLAYERS) {
                setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        address.string(), "remote-submix");
            } else {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        address.string(), "remote-submix");
            }
        } else if ((mixes[i].mRouteFlags & MIX_ROUTE_FLAG_RENDER) == MIX_ROUTE_FLAG_RENDER) {
            String8 address = mixes[i].mDeviceAddress;
            audio_devices_t device = mixes[i].mDeviceType;
            ALOGV(" registerPolicyMixes() mix %zu of %zu is RENDER, dev=0x%X addr=%s",
                    i, mixes.size(), device, address.string());

            bool foundOutput = false;
            for (size_t j = 0 ; j < mOutputs.size() ; j++) {
                sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(j);
                sp<AudioPatch> patch = mAudioPatches.valueFor(desc->getPatchHandle());
                if ((patch != 0) && (patch->mPatch.num_sinks != 0)
                        && (patch->mPatch.sinks[0].type == AUDIO_PORT_TYPE_DEVICE)
                        && (patch->mPatch.sinks[0].ext.device.type == device)
                        && (strncmp(patch->mPatch.sinks[0].ext.device.address, address.string(),
                                AUDIO_DEVICE_MAX_ADDRESS_LEN) == 0)) {
                    if (mPolicyMixes.registerMix(address, mixes[i], desc) != NO_ERROR) {
                        res = INVALID_OPERATION;
                    } else {
                        foundOutput = true;
                    }
                    break;
                }
            }

            if (res != NO_ERROR) {
                ALOGE(" Error registering mix %zu for device 0x%X addr %s",
                        i, device, address.string());
                res = INVALID_OPERATION;
                break;
            } else if (!foundOutput) {
                ALOGE(" Output not found for mix %zu for device 0x%X addr %s",
                        i, device, address.string());
                res = INVALID_OPERATION;
                break;
            }
        }
    }
    if (res != NO_ERROR) {
        unregisterPolicyMixes(mixes);
    }
    return res;
}

status_t AudioPolicyManager::unregisterPolicyMixes(Vector<AudioMix> mixes)
{
    ALOGV("unregisterPolicyMixes() num mixes %zu", mixes.size());
    status_t res = NO_ERROR;
    sp<HwModule> rSubmixModule;
    // examine each mix's route type
    for (size_t i = 0; i < mixes.size(); i++) {
        if ((mixes[i].mRouteFlags & MIX_ROUTE_FLAG_LOOP_BACK) == MIX_ROUTE_FLAG_LOOP_BACK) {

            if (rSubmixModule == 0) {
                for (size_t j = 0; i < mHwModules.size(); j++) {
                    if (strcmp(AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX, mHwModules[j]->mName) == 0
                            && mHwModules[j]->mHandle != 0) {
                        rSubmixModule = mHwModules[j];
                        break;
                    }
                }
            }
            if (rSubmixModule == 0) {
                res = INVALID_OPERATION;
                continue;
            }

            String8 address = mixes[i].mDeviceAddress;

            if (mPolicyMixes.unregisterMix(address) != NO_ERROR) {
                res = INVALID_OPERATION;
                continue;
            }

            if (getDeviceConnectionState(AUDIO_DEVICE_IN_REMOTE_SUBMIX, address.string()) ==
                    AUDIO_POLICY_DEVICE_STATE_AVAILABLE)  {
                setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                        address.string(), "remote-submix");
            }
            if (getDeviceConnectionState(AUDIO_DEVICE_OUT_REMOTE_SUBMIX, address.string()) ==
                    AUDIO_POLICY_DEVICE_STATE_AVAILABLE)  {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                        address.string(), "remote-submix");
            }
            rSubmixModule->removeOutputProfile(address);
            rSubmixModule->removeInputProfile(address);

        } if ((mixes[i].mRouteFlags & MIX_ROUTE_FLAG_RENDER) == MIX_ROUTE_FLAG_RENDER) {
            if (mPolicyMixes.unregisterMix(mixes[i].mDeviceAddress) != NO_ERROR) {
                res = INVALID_OPERATION;
                continue;
            }
        }
    }
    return res;
}


status_t AudioPolicyManager::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "\nAudioPolicyManager Dump: %p\n", this);
    result.append(buffer);

    snprintf(buffer, SIZE, " Primary Output: %d\n",
             hasPrimaryOutput() ? mPrimaryOutput->mIoHandle : AUDIO_IO_HANDLE_NONE);
    result.append(buffer);
    snprintf(buffer, SIZE, " Phone state: %d\n", mEngine->getPhoneState());
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for communications %d\n",
             mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION));
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for media %d\n", mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA));
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for record %d\n", mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_RECORD));
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for dock %d\n", mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_DOCK));
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for system %d\n", mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM));
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for hdmi system audio %d\n",
            mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_HDMI_SYSTEM_AUDIO));
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for encoded surround output %d\n",
            mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND));
    result.append(buffer);
    snprintf(buffer, SIZE, " TTS output %s\n", mTtsOutputAvailable ? "available" : "not available");
    result.append(buffer);
    snprintf(buffer, SIZE, " Master mono: %s\n", mMasterMono ? "on" : "off");
    result.append(buffer);

    write(fd, result.string(), result.size());

    mAvailableOutputDevices.dump(fd, String8("Available output"));
    mAvailableInputDevices.dump(fd, String8("Available input"));
    mHwModules.dump(fd);
    mOutputs.dump(fd);
    mInputs.dump(fd);
    mVolumeCurves->dump(fd);
    mEffects.dump(fd);
    mAudioPatches.dump(fd);

    return NO_ERROR;
}

// This function checks for the parameters which can be offloaded.
// This can be enhanced depending on the capability of the DSP and policy
// of the system.
bool AudioPolicyManager::isOffloadSupported(const audio_offload_info_t& offloadInfo)
{
    ALOGV("isOffloadSupported: SR=%u, CM=0x%x, Format=0x%x, StreamType=%d,"
     " BitRate=%u, duration=%" PRId64 " us, has_video=%d",
     offloadInfo.sample_rate, offloadInfo.channel_mask,
     offloadInfo.format,
     offloadInfo.stream_type, offloadInfo.bit_rate, offloadInfo.duration_us,
     offloadInfo.has_video);

    if (mMasterMono) {
        return false; // no offloading if mono is set.
    }

    // Check if offload has been disabled
    char propValue[PROPERTY_VALUE_MAX];
    if (property_get("audio.offload.disable", propValue, "0")) {
        if (atoi(propValue) != 0) {
            ALOGV("offload disabled by audio.offload.disable=%s", propValue );
            return false;
        }
    }

    // Check if stream type is music, then only allow offload as of now.
    if (offloadInfo.stream_type != AUDIO_STREAM_MUSIC)
    {
        ALOGV("isOffloadSupported: stream_type != MUSIC, returning false");
        return false;
    }

    //TODO: enable audio offloading with video when ready
    const bool allowOffloadWithVideo =
            property_get_bool("audio.offload.video", false /* default_value */);
    if (offloadInfo.has_video && !allowOffloadWithVideo) {
        ALOGV("isOffloadSupported: has_video == true, returning false");
        return false;
    }

    //If duration is less than minimum value defined in property, return false
    if (property_get("audio.offload.min.duration.secs", propValue, NULL)) {
        if (offloadInfo.duration_us < (atoi(propValue) * 1000000 )) {
            ALOGV("Offload denied by duration < audio.offload.min.duration.secs(=%s)", propValue);
            return false;
        }
    } else if (offloadInfo.duration_us < OFFLOAD_DEFAULT_MIN_DURATION_SECS * 1000000) {
        ALOGV("Offload denied by duration < default min(=%u)", OFFLOAD_DEFAULT_MIN_DURATION_SECS);
        return false;
    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    if (mEffects.isNonOffloadableEffectEnabled()) {
        return false;
    }

    // See if there is a profile to support this.
    // AUDIO_DEVICE_NONE
    sp<IOProfile> profile = getProfileForDirectOutput(AUDIO_DEVICE_NONE /*ignore device */,
                                            offloadInfo.sample_rate,
                                            offloadInfo.format,
                                            offloadInfo.channel_mask,
                                            AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
    ALOGV("isOffloadSupported() profile %sfound", profile != 0 ? "" : "NOT ");
    return (profile != 0);
}

status_t AudioPolicyManager::listAudioPorts(audio_port_role_t role,
                                            audio_port_type_t type,
                                            unsigned int *num_ports,
                                            struct audio_port *ports,
                                            unsigned int *generation)
{
    if (num_ports == NULL || (*num_ports != 0 && ports == NULL) ||
            generation == NULL) {
        return BAD_VALUE;
    }
    ALOGV("listAudioPorts() role %d type %d num_ports %d ports %p", role, type, *num_ports, ports);
    if (ports == NULL) {
        *num_ports = 0;
    }

    size_t portsWritten = 0;
    size_t portsMax = *num_ports;
    *num_ports = 0;
    if (type == AUDIO_PORT_TYPE_NONE || type == AUDIO_PORT_TYPE_DEVICE) {
        // do not report devices with type AUDIO_DEVICE_IN_STUB or AUDIO_DEVICE_OUT_STUB
        // as they are used by stub HALs by convention
        if (role == AUDIO_PORT_ROLE_SINK || role == AUDIO_PORT_ROLE_NONE) {
            for (size_t i = 0; i < mAvailableOutputDevices.size(); i++) {
                if (mAvailableOutputDevices[i]->type() == AUDIO_DEVICE_OUT_STUB) {
                    continue;
                }
                if (portsWritten < portsMax) {
                    mAvailableOutputDevices[i]->toAudioPort(&ports[portsWritten++]);
                }
                (*num_ports)++;
            }
        }
        if (role == AUDIO_PORT_ROLE_SOURCE || role == AUDIO_PORT_ROLE_NONE) {
            for (size_t i = 0; i < mAvailableInputDevices.size(); i++) {
                if (mAvailableInputDevices[i]->type() == AUDIO_DEVICE_IN_STUB) {
                    continue;
                }
                if (portsWritten < portsMax) {
                    mAvailableInputDevices[i]->toAudioPort(&ports[portsWritten++]);
                }
                (*num_ports)++;
            }
        }
    }
    if (type == AUDIO_PORT_TYPE_NONE || type == AUDIO_PORT_TYPE_MIX) {
        if (role == AUDIO_PORT_ROLE_SINK || role == AUDIO_PORT_ROLE_NONE) {
            for (size_t i = 0; i < mInputs.size() && portsWritten < portsMax; i++) {
                mInputs[i]->toAudioPort(&ports[portsWritten++]);
            }
            *num_ports += mInputs.size();
        }
        if (role == AUDIO_PORT_ROLE_SOURCE || role == AUDIO_PORT_ROLE_NONE) {
            size_t numOutputs = 0;
            for (size_t i = 0; i < mOutputs.size(); i++) {
                if (!mOutputs[i]->isDuplicated()) {
                    numOutputs++;
                    if (portsWritten < portsMax) {
                        mOutputs[i]->toAudioPort(&ports[portsWritten++]);
                    }
                }
            }
            *num_ports += numOutputs;
        }
    }
    *generation = curAudioPortGeneration();
    ALOGV("listAudioPorts() got %zu ports needed %d", portsWritten, *num_ports);
    return NO_ERROR;
}

status_t AudioPolicyManager::getAudioPort(struct audio_port *port __unused)
{
    return NO_ERROR;
}

status_t AudioPolicyManager::createAudioPatch(const struct audio_patch *patch,
                                               audio_patch_handle_t *handle,
                                               uid_t uid)
{
    ALOGV("createAudioPatch()");

    if (handle == NULL || patch == NULL) {
        return BAD_VALUE;
    }
    ALOGV("createAudioPatch() num sources %d num sinks %d", patch->num_sources, patch->num_sinks);

    if (patch->num_sources == 0 || patch->num_sources > AUDIO_PATCH_PORTS_MAX ||
            patch->num_sinks == 0 || patch->num_sinks > AUDIO_PATCH_PORTS_MAX) {
        return BAD_VALUE;
    }
    // only one source per audio patch supported for now
    if (patch->num_sources > 1) {
        return INVALID_OPERATION;
    }

    if (patch->sources[0].role != AUDIO_PORT_ROLE_SOURCE) {
        return INVALID_OPERATION;
    }
    for (size_t i = 0; i < patch->num_sinks; i++) {
        if (patch->sinks[i].role != AUDIO_PORT_ROLE_SINK) {
            return INVALID_OPERATION;
        }
    }

    sp<AudioPatch> patchDesc;
    ssize_t index = mAudioPatches.indexOfKey(*handle);

    ALOGV("createAudioPatch source id %d role %d type %d", patch->sources[0].id,
                                                           patch->sources[0].role,
                                                           patch->sources[0].type);
#if LOG_NDEBUG == 0
    for (size_t i = 0; i < patch->num_sinks; i++) {
        ALOGV("createAudioPatch sink %zu: id %d role %d type %d", i, patch->sinks[i].id,
                                                             patch->sinks[i].role,
                                                             patch->sinks[i].type);
    }
#endif

    if (index >= 0) {
        patchDesc = mAudioPatches.valueAt(index);
        ALOGV("createAudioPatch() mUidCached %d patchDesc->mUid %d uid %d",
                                                                  mUidCached, patchDesc->mUid, uid);
        if (patchDesc->mUid != mUidCached && uid != patchDesc->mUid) {
            return INVALID_OPERATION;
        }
    } else {
        *handle = AUDIO_PATCH_HANDLE_NONE;
    }

    if (patch->sources[0].type == AUDIO_PORT_TYPE_MIX) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputFromId(patch->sources[0].id);
        if (outputDesc == NULL) {
            ALOGV("createAudioPatch() output not found for id %d", patch->sources[0].id);
            return BAD_VALUE;
        }
        ALOG_ASSERT(!outputDesc->isDuplicated(),"duplicated output %d in source in ports",
                                                outputDesc->mIoHandle);
        if (patchDesc != 0) {
            if (patchDesc->mPatch.sources[0].id != patch->sources[0].id) {
                ALOGV("createAudioPatch() source id differs for patch current id %d new id %d",
                                          patchDesc->mPatch.sources[0].id, patch->sources[0].id);
                return BAD_VALUE;
            }
        }
        DeviceVector devices;
        for (size_t i = 0; i < patch->num_sinks; i++) {
            // Only support mix to devices connection
            // TODO add support for mix to mix connection
            if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                ALOGV("createAudioPatch() source mix but sink is not a device");
                return INVALID_OPERATION;
            }
            sp<DeviceDescriptor> devDesc =
                    mAvailableOutputDevices.getDeviceFromId(patch->sinks[i].id);
            if (devDesc == 0) {
                ALOGV("createAudioPatch() out device not found for id %d", patch->sinks[i].id);
                return BAD_VALUE;
            }

            if (!outputDesc->mProfile->isCompatibleProfile(devDesc->type(),
                                                           devDesc->mAddress,
                                                           patch->sources[0].sample_rate,
                                                           NULL,  // updatedSamplingRate
                                                           patch->sources[0].format,
                                                           NULL,  // updatedFormat
                                                           patch->sources[0].channel_mask,
                                                           NULL,  // updatedChannelMask
                                                           AUDIO_OUTPUT_FLAG_NONE /*FIXME*/)) {
                ALOGV("createAudioPatch() profile not supported for device %08x",
                        devDesc->type());
                return INVALID_OPERATION;
            }
            devices.add(devDesc);
        }
        if (devices.size() == 0) {
            return INVALID_OPERATION;
        }

        // TODO: reconfigure output format and channels here
        ALOGV("createAudioPatch() setting device %08x on output %d",
              devices.types(), outputDesc->mIoHandle);
        setOutputDevice(outputDesc, devices.types(), true, 0, handle);
        index = mAudioPatches.indexOfKey(*handle);
        if (index >= 0) {
            if (patchDesc != 0 && patchDesc != mAudioPatches.valueAt(index)) {
                ALOGW("createAudioPatch() setOutputDevice() did not reuse the patch provided");
            }
            patchDesc = mAudioPatches.valueAt(index);
            patchDesc->mUid = uid;
            ALOGV("createAudioPatch() success");
        } else {
            ALOGW("createAudioPatch() setOutputDevice() failed to create a patch");
            return INVALID_OPERATION;
        }
    } else if (patch->sources[0].type == AUDIO_PORT_TYPE_DEVICE) {
        if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
            // input device to input mix connection
            // only one sink supported when connecting an input device to a mix
            if (patch->num_sinks > 1) {
                return INVALID_OPERATION;
            }
            sp<AudioInputDescriptor> inputDesc = mInputs.getInputFromId(patch->sinks[0].id);
            if (inputDesc == NULL) {
                return BAD_VALUE;
            }
            if (patchDesc != 0) {
                if (patchDesc->mPatch.sinks[0].id != patch->sinks[0].id) {
                    return BAD_VALUE;
                }
            }
            sp<DeviceDescriptor> devDesc =
                    mAvailableInputDevices.getDeviceFromId(patch->sources[0].id);
            if (devDesc == 0) {
                return BAD_VALUE;
            }

            if (!inputDesc->mProfile->isCompatibleProfile(devDesc->type(),
                                                          devDesc->mAddress,
                                                          patch->sinks[0].sample_rate,
                                                          NULL, /*updatedSampleRate*/
                                                          patch->sinks[0].format,
                                                          NULL, /*updatedFormat*/
                                                          patch->sinks[0].channel_mask,
                                                          NULL, /*updatedChannelMask*/
                                                          // FIXME for the parameter type,
                                                          // and the NONE
                                                          (audio_output_flags_t)
                                                            AUDIO_INPUT_FLAG_NONE)) {
                return INVALID_OPERATION;
            }
            // TODO: reconfigure output format and channels here
            ALOGV("createAudioPatch() setting device %08x on output %d",
                                                  devDesc->type(), inputDesc->mIoHandle);
            setInputDevice(inputDesc->mIoHandle, devDesc->type(), true, handle);
            index = mAudioPatches.indexOfKey(*handle);
            if (index >= 0) {
                if (patchDesc != 0 && patchDesc != mAudioPatches.valueAt(index)) {
                    ALOGW("createAudioPatch() setInputDevice() did not reuse the patch provided");
                }
                patchDesc = mAudioPatches.valueAt(index);
                patchDesc->mUid = uid;
                ALOGV("createAudioPatch() success");
            } else {
                ALOGW("createAudioPatch() setInputDevice() failed to create a patch");
                return INVALID_OPERATION;
            }
        } else if (patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
            // device to device connection
            if (patchDesc != 0) {
                if (patchDesc->mPatch.sources[0].id != patch->sources[0].id) {
                    return BAD_VALUE;
                }
            }
            sp<DeviceDescriptor> srcDeviceDesc =
                    mAvailableInputDevices.getDeviceFromId(patch->sources[0].id);
            if (srcDeviceDesc == 0) {
                return BAD_VALUE;
            }

            //update source and sink with our own data as the data passed in the patch may
            // be incomplete.
            struct audio_patch newPatch = *patch;
            srcDeviceDesc->toAudioPortConfig(&newPatch.sources[0], &patch->sources[0]);

            for (size_t i = 0; i < patch->num_sinks; i++) {
                if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                    ALOGV("createAudioPatch() source device but one sink is not a device");
                    return INVALID_OPERATION;
                }

                sp<DeviceDescriptor> sinkDeviceDesc =
                        mAvailableOutputDevices.getDeviceFromId(patch->sinks[i].id);
                if (sinkDeviceDesc == 0) {
                    return BAD_VALUE;
                }
                sinkDeviceDesc->toAudioPortConfig(&newPatch.sinks[i], &patch->sinks[i]);

                // create a software bridge in PatchPanel if:
                // - source and sink devices are on differnt HW modules OR
                // - audio HAL version is < 3.0
                if ((srcDeviceDesc->getModuleHandle() != sinkDeviceDesc->getModuleHandle()) ||
                        (srcDeviceDesc->mModule->getHalVersion() < AUDIO_DEVICE_API_VERSION_3_0)) {
                    // support only one sink device for now to simplify output selection logic
                    if (patch->num_sinks > 1) {
                        return INVALID_OPERATION;
                    }
                    SortedVector<audio_io_handle_t> outputs =
                                            getOutputsForDevice(sinkDeviceDesc->type(), mOutputs);
                    // if the sink device is reachable via an opened output stream, request to go via
                    // this output stream by adding a second source to the patch description
                    audio_io_handle_t output = selectOutput(outputs,
                                                            AUDIO_OUTPUT_FLAG_NONE,
                                                            AUDIO_FORMAT_INVALID);
                    if (output != AUDIO_IO_HANDLE_NONE) {
                        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
                        if (outputDesc->isDuplicated()) {
                            return INVALID_OPERATION;
                        }
                        outputDesc->toAudioPortConfig(&newPatch.sources[1], &patch->sources[0]);
                        newPatch.sources[1].ext.mix.usecase.stream = AUDIO_STREAM_PATCH;
                        newPatch.num_sources = 2;
                    }
                }
            }
            // TODO: check from routing capabilities in config file and other conflicting patches

            audio_patch_handle_t afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
            if (index >= 0) {
                afPatchHandle = patchDesc->mAfPatchHandle;
            }

            status_t status = mpClientInterface->createAudioPatch(&newPatch,
                                                                  &afPatchHandle,
                                                                  0);
            ALOGV("createAudioPatch() patch panel returned %d patchHandle %d",
                                                                  status, afPatchHandle);
            if (status == NO_ERROR) {
                if (index < 0) {
                    patchDesc = new AudioPatch(&newPatch, uid);
                    addAudioPatch(patchDesc->mHandle, patchDesc);
                } else {
                    patchDesc->mPatch = newPatch;
                }
                patchDesc->mAfPatchHandle = afPatchHandle;
                *handle = patchDesc->mHandle;
                nextAudioPortGeneration();
                mpClientInterface->onAudioPatchListUpdate();
            } else {
                ALOGW("createAudioPatch() patch panel could not connect device patch, error %d",
                status);
                return INVALID_OPERATION;
            }
        } else {
            return BAD_VALUE;
        }
    } else {
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::releaseAudioPatch(audio_patch_handle_t handle,
                                                  uid_t uid)
{
    ALOGV("releaseAudioPatch() patch %d", handle);

    ssize_t index = mAudioPatches.indexOfKey(handle);

    if (index < 0) {
        return BAD_VALUE;
    }
    sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    ALOGV("releaseAudioPatch() mUidCached %d patchDesc->mUid %d uid %d",
          mUidCached, patchDesc->mUid, uid);
    if (patchDesc->mUid != mUidCached && uid != patchDesc->mUid) {
        return INVALID_OPERATION;
    }

    struct audio_patch *patch = &patchDesc->mPatch;
    patchDesc->mUid = mUidCached;
    if (patch->sources[0].type == AUDIO_PORT_TYPE_MIX) {
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputFromId(patch->sources[0].id);
        if (outputDesc == NULL) {
            ALOGV("releaseAudioPatch() output not found for id %d", patch->sources[0].id);
            return BAD_VALUE;
        }

        setOutputDevice(outputDesc,
                        getNewOutputDevice(outputDesc, true /*fromCache*/),
                       true,
                       0,
                       NULL);
    } else if (patch->sources[0].type == AUDIO_PORT_TYPE_DEVICE) {
        if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
            sp<AudioInputDescriptor> inputDesc = mInputs.getInputFromId(patch->sinks[0].id);
            if (inputDesc == NULL) {
                ALOGV("releaseAudioPatch() input not found for id %d", patch->sinks[0].id);
                return BAD_VALUE;
            }
            setInputDevice(inputDesc->mIoHandle,
                           getNewInputDevice(inputDesc->mIoHandle),
                           true,
                           NULL);
        } else if (patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
            status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
            ALOGV("releaseAudioPatch() patch panel returned %d patchHandle %d",
                                                              status, patchDesc->mAfPatchHandle);
            removeAudioPatch(patchDesc->mHandle);
            nextAudioPortGeneration();
            mpClientInterface->onAudioPatchListUpdate();
        } else {
            return BAD_VALUE;
        }
    } else {
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::listAudioPatches(unsigned int *num_patches,
                                              struct audio_patch *patches,
                                              unsigned int *generation)
{
    if (generation == NULL) {
        return BAD_VALUE;
    }
    *generation = curAudioPortGeneration();
    return mAudioPatches.listAudioPatches(num_patches, patches);
}

status_t AudioPolicyManager::setAudioPortConfig(const struct audio_port_config *config)
{
    ALOGV("setAudioPortConfig()");

    if (config == NULL) {
        return BAD_VALUE;
    }
    ALOGV("setAudioPortConfig() on port handle %d", config->id);
    // Only support gain configuration for now
    if (config->config_mask != AUDIO_PORT_CONFIG_GAIN) {
        return INVALID_OPERATION;
    }

    sp<AudioPortConfig> audioPortConfig;
    if (config->type == AUDIO_PORT_TYPE_MIX) {
        if (config->role == AUDIO_PORT_ROLE_SOURCE) {
            sp<SwAudioOutputDescriptor> outputDesc = mOutputs.getOutputFromId(config->id);
            if (outputDesc == NULL) {
                return BAD_VALUE;
            }
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "setAudioPortConfig() called on duplicated output %d",
                        outputDesc->mIoHandle);
            audioPortConfig = outputDesc;
        } else if (config->role == AUDIO_PORT_ROLE_SINK) {
            sp<AudioInputDescriptor> inputDesc = mInputs.getInputFromId(config->id);
            if (inputDesc == NULL) {
                return BAD_VALUE;
            }
            audioPortConfig = inputDesc;
        } else {
            return BAD_VALUE;
        }
    } else if (config->type == AUDIO_PORT_TYPE_DEVICE) {
        sp<DeviceDescriptor> deviceDesc;
        if (config->role == AUDIO_PORT_ROLE_SOURCE) {
            deviceDesc = mAvailableInputDevices.getDeviceFromId(config->id);
        } else if (config->role == AUDIO_PORT_ROLE_SINK) {
            deviceDesc = mAvailableOutputDevices.getDeviceFromId(config->id);
        } else {
            return BAD_VALUE;
        }
        if (deviceDesc == NULL) {
            return BAD_VALUE;
        }
        audioPortConfig = deviceDesc;
    } else {
        return BAD_VALUE;
    }

    struct audio_port_config backupConfig;
    status_t status = audioPortConfig->applyAudioPortConfig(config, &backupConfig);
    if (status == NO_ERROR) {
        struct audio_port_config newConfig;
        audioPortConfig->toAudioPortConfig(&newConfig, config);
        status = mpClientInterface->setAudioPortConfig(&newConfig, 0);
    }
    if (status != NO_ERROR) {
        audioPortConfig->applyAudioPortConfig(&backupConfig);
    }

    return status;
}

void AudioPolicyManager::releaseResourcesForUid(uid_t uid)
{
    clearAudioSources(uid);
    clearAudioPatches(uid);
    clearSessionRoutes(uid);
}

void AudioPolicyManager::clearAudioPatches(uid_t uid)
{
    for (ssize_t i = (ssize_t)mAudioPatches.size() - 1; i >= 0; i--)  {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(i);
        if (patchDesc->mUid == uid) {
            releaseAudioPatch(mAudioPatches.keyAt(i), uid);
        }
    }
}

void AudioPolicyManager::checkStrategyRoute(routing_strategy strategy,
                                            audio_io_handle_t ouptutToSkip)
{
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);
    SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(device, mOutputs);
    for (size_t j = 0; j < mOutputs.size(); j++) {
        if (mOutputs.keyAt(j) == ouptutToSkip) {
            continue;
        }
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueAt(j);
        if (!isStrategyActive(outputDesc, (routing_strategy)strategy)) {
            continue;
        }
        // If the default device for this strategy is on another output mix,
        // invalidate all tracks in this strategy to force re connection.
        // Otherwise select new device on the output mix.
        if (outputs.indexOf(mOutputs.keyAt(j)) < 0) {
            for (int stream = 0; stream < AUDIO_STREAM_FOR_POLICY_CNT; stream++) {
                if (getStrategy((audio_stream_type_t)stream) == strategy) {
                    mpClientInterface->invalidateStream((audio_stream_type_t)stream);
                }
            }
        } else {
            audio_devices_t newDevice = getNewOutputDevice(outputDesc, false /*fromCache*/);
            setOutputDevice(outputDesc, newDevice, false);
        }
    }
}

void AudioPolicyManager::clearSessionRoutes(uid_t uid)
{
    // remove output routes associated with this uid
    SortedVector<routing_strategy> affectedStrategies;
    for (ssize_t i = (ssize_t)mOutputRoutes.size() - 1; i >= 0; i--)  {
        sp<SessionRoute> route = mOutputRoutes.valueAt(i);
        if (route->mUid == uid) {
            mOutputRoutes.removeItemsAt(i);
            if (route->mDeviceDescriptor != 0) {
                affectedStrategies.add(getStrategy(route->mStreamType));
            }
        }
    }
    // reroute outputs if necessary
    for (size_t i = 0; i < affectedStrategies.size(); i++) {
        checkStrategyRoute(affectedStrategies[i], AUDIO_IO_HANDLE_NONE);
    }

    // remove input routes associated with this uid
    SortedVector<audio_source_t> affectedSources;
    for (ssize_t i = (ssize_t)mInputRoutes.size() - 1; i >= 0; i--)  {
        sp<SessionRoute> route = mInputRoutes.valueAt(i);
        if (route->mUid == uid) {
            mInputRoutes.removeItemsAt(i);
            if (route->mDeviceDescriptor != 0) {
                affectedSources.add(route->mSource);
            }
        }
    }
    // reroute inputs if necessary
    SortedVector<audio_io_handle_t> inputsToClose;
    for (size_t i = 0; i < mInputs.size(); i++) {
        sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(i);
        if (affectedSources.indexOf(inputDesc->inputSource()) >= 0) {
            inputsToClose.add(inputDesc->mIoHandle);
        }
    }
    for (size_t i = 0; i < inputsToClose.size(); i++) {
        closeInput(inputsToClose[i]);
    }
}

void AudioPolicyManager::clearAudioSources(uid_t uid)
{
    for (ssize_t i = (ssize_t)mAudioSources.size() - 1; i >= 0; i--)  {
        sp<AudioSourceDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc->mUid == uid) {
            stopAudioSource(mAudioSources.keyAt(i));
        }
    }
}

status_t AudioPolicyManager::acquireSoundTriggerSession(audio_session_t *session,
                                       audio_io_handle_t *ioHandle,
                                       audio_devices_t *device)
{
    *session = (audio_session_t)mpClientInterface->newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION);
    *ioHandle = (audio_io_handle_t)mpClientInterface->newAudioUniqueId(AUDIO_UNIQUE_ID_USE_INPUT);
    *device = getDeviceAndMixForInputSource(AUDIO_SOURCE_HOTWORD);

    return mSoundTriggerSessions.acquireSession(*session, *ioHandle);
}

status_t AudioPolicyManager::startAudioSource(const struct audio_port_config *source,
                                  const audio_attributes_t *attributes,
                                  audio_io_handle_t *handle,
                                  uid_t uid)
{
    ALOGV("%s source %p attributes %p handle %p", __FUNCTION__, source, attributes, handle);
    if (source == NULL || attributes == NULL || handle == NULL) {
        return BAD_VALUE;
    }

    *handle = AUDIO_IO_HANDLE_NONE;

    if (source->role != AUDIO_PORT_ROLE_SOURCE ||
            source->type != AUDIO_PORT_TYPE_DEVICE) {
        ALOGV("%s INVALID_OPERATION source->role %d source->type %d", __FUNCTION__, source->role, source->type);
        return INVALID_OPERATION;
    }

    sp<DeviceDescriptor> srcDeviceDesc =
            mAvailableInputDevices.getDevice(source->ext.device.type,
                                              String8(source->ext.device.address));
    if (srcDeviceDesc == 0) {
        ALOGV("%s source->ext.device.type %08x not found", __FUNCTION__, source->ext.device.type);
        return BAD_VALUE;
    }
    sp<AudioSourceDescriptor> sourceDesc =
            new AudioSourceDescriptor(srcDeviceDesc, attributes, uid);

    struct audio_patch dummyPatch;
    sp<AudioPatch> patchDesc = new AudioPatch(&dummyPatch, uid);
    sourceDesc->mPatchDesc = patchDesc;

    status_t status = connectAudioSource(sourceDesc);
    if (status == NO_ERROR) {
        mAudioSources.add(sourceDesc->getHandle(), sourceDesc);
        *handle = sourceDesc->getHandle();
    }
    return status;
}

status_t AudioPolicyManager::connectAudioSource(const sp<AudioSourceDescriptor>& sourceDesc)
{
    ALOGV("%s handle %d", __FUNCTION__, sourceDesc->getHandle());

    // make sure we only have one patch per source.
    disconnectAudioSource(sourceDesc);

    routing_strategy strategy = (routing_strategy) getStrategyForAttr(&sourceDesc->mAttributes);
    audio_stream_type_t stream = streamTypefromAttributesInt(&sourceDesc->mAttributes);
    sp<DeviceDescriptor> srcDeviceDesc = sourceDesc->mDevice;

    audio_devices_t sinkDevice = getDeviceForStrategy(strategy, true);
    sp<DeviceDescriptor> sinkDeviceDesc =
            mAvailableOutputDevices.getDevice(sinkDevice, String8(""));

    audio_patch_handle_t afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
    struct audio_patch *patch = &sourceDesc->mPatchDesc->mPatch;

    if (srcDeviceDesc->getAudioPort()->mModule->getHandle() ==
            sinkDeviceDesc->getAudioPort()->mModule->getHandle() &&
            srcDeviceDesc->getAudioPort()->mModule->getHalVersion() >= AUDIO_DEVICE_API_VERSION_3_0 &&
            srcDeviceDesc->getAudioPort()->mGains.size() > 0) {
        ALOGV("%s AUDIO_DEVICE_API_VERSION_3_0", __FUNCTION__);
        //   create patch between src device and output device
        //   create Hwoutput and add to mHwOutputs
    } else {
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(sinkDevice, mOutputs);
        audio_io_handle_t output =
                selectOutput(outputs, AUDIO_OUTPUT_FLAG_NONE, AUDIO_FORMAT_INVALID);
        if (output == AUDIO_IO_HANDLE_NONE) {
            ALOGV("%s no output for device %08x", __FUNCTION__, sinkDevice);
            return INVALID_OPERATION;
        }
        sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
        if (outputDesc->isDuplicated()) {
            ALOGV("%s output for device %08x is duplicated", __FUNCTION__, sinkDevice);
            return INVALID_OPERATION;
        }
        // create a special patch with no sink and two sources:
        // - the second source indicates to PatchPanel through which output mix this patch should
        // be connected as well as the stream type for volume control
        // - the sink is defined by whatever output device is currently selected for the output
        // though which this patch is routed.
        patch->num_sinks = 0;
        patch->num_sources = 2;
        srcDeviceDesc->toAudioPortConfig(&patch->sources[0], NULL);
        outputDesc->toAudioPortConfig(&patch->sources[1], NULL);
        patch->sources[1].ext.mix.usecase.stream = stream;
        status_t status = mpClientInterface->createAudioPatch(patch,
                                                              &afPatchHandle,
                                                              0);
        ALOGV("%s patch panel returned %d patchHandle %d", __FUNCTION__,
                                                              status, afPatchHandle);
        if (status != NO_ERROR) {
            ALOGW("%s patch panel could not connect device patch, error %d",
                  __FUNCTION__, status);
            return INVALID_OPERATION;
        }
        uint32_t delayMs = 0;
        status = startSource(outputDesc, stream, sinkDevice, NULL, &delayMs);

        if (status != NO_ERROR) {
            mpClientInterface->releaseAudioPatch(sourceDesc->mPatchDesc->mAfPatchHandle, 0);
            return status;
        }
        sourceDesc->mSwOutput = outputDesc;
        if (delayMs != 0) {
            usleep(delayMs * 1000);
        }
    }

    sourceDesc->mPatchDesc->mAfPatchHandle = afPatchHandle;
    addAudioPatch(sourceDesc->mPatchDesc->mHandle, sourceDesc->mPatchDesc);

    return NO_ERROR;
}

status_t AudioPolicyManager::stopAudioSource(audio_io_handle_t handle __unused)
{
    sp<AudioSourceDescriptor> sourceDesc = mAudioSources.valueFor(handle);
    ALOGV("%s handle %d", __FUNCTION__, handle);
    if (sourceDesc == 0) {
        ALOGW("%s unknown source for handle %d", __FUNCTION__, handle);
        return BAD_VALUE;
    }
    status_t status = disconnectAudioSource(sourceDesc);

    mAudioSources.removeItem(handle);
    return status;
}

status_t AudioPolicyManager::setMasterMono(bool mono)
{
    if (mMasterMono == mono) {
        return NO_ERROR;
    }
    mMasterMono = mono;
    // if enabling mono we close all offloaded devices, which will invalidate the
    // corresponding AudioTrack. The AudioTrack client/MediaPlayer is responsible
    // for recreating the new AudioTrack as non-offloaded PCM.
    //
    // If disabling mono, we leave all tracks as is: we don't know which clients
    // and tracks are able to be recreated as offloaded. The next "song" should
    // play back offloaded.
    if (mMasterMono) {
        Vector<audio_io_handle_t> offloaded;
        for (size_t i = 0; i < mOutputs.size(); ++i) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                offloaded.push(desc->mIoHandle);
            }
        }
        for (size_t i = 0; i < offloaded.size(); ++i) {
            closeOutput(offloaded[i]);
        }
    }
    // update master mono for all remaining outputs
    for (size_t i = 0; i < mOutputs.size(); ++i) {
        updateMono(mOutputs.keyAt(i));
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::getMasterMono(bool *mono)
{
    *mono = mMasterMono;
    return NO_ERROR;
}

status_t AudioPolicyManager::disconnectAudioSource(const sp<AudioSourceDescriptor>& sourceDesc)
{
    ALOGV("%s handle %d", __FUNCTION__, sourceDesc->getHandle());

    sp<AudioPatch> patchDesc = mAudioPatches.valueFor(sourceDesc->mPatchDesc->mHandle);
    if (patchDesc == 0) {
        ALOGW("%s source has no patch with handle %d", __FUNCTION__,
              sourceDesc->mPatchDesc->mHandle);
        return BAD_VALUE;
    }
    removeAudioPatch(sourceDesc->mPatchDesc->mHandle);

    audio_stream_type_t stream = streamTypefromAttributesInt(&sourceDesc->mAttributes);
    sp<SwAudioOutputDescriptor> swOutputDesc = sourceDesc->mSwOutput.promote();
    if (swOutputDesc != 0) {
        stopSource(swOutputDesc, stream, false);
        mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
    } else {
        sp<HwAudioOutputDescriptor> hwOutputDesc = sourceDesc->mHwOutput.promote();
        if (hwOutputDesc != 0) {
          //   release patch between src device and output device
          //   close Hwoutput and remove from mHwOutputs
        } else {
            ALOGW("%s source has neither SW nor HW output", __FUNCTION__);
        }
    }
    return NO_ERROR;
}

sp<AudioSourceDescriptor> AudioPolicyManager::getSourceForStrategyOnOutput(
        audio_io_handle_t output, routing_strategy strategy)
{
    sp<AudioSourceDescriptor> source;
    for (size_t i = 0; i < mAudioSources.size(); i++)  {
        sp<AudioSourceDescriptor> sourceDesc = mAudioSources.valueAt(i);
        routing_strategy sourceStrategy =
                (routing_strategy) getStrategyForAttr(&sourceDesc->mAttributes);
        sp<SwAudioOutputDescriptor> outputDesc = sourceDesc->mSwOutput.promote();
        if (sourceStrategy == strategy && outputDesc != 0 && outputDesc->mIoHandle == output) {
            source = sourceDesc;
            break;
        }
    }
    return source;
}

// ----------------------------------------------------------------------------
// AudioPolicyManager
// ----------------------------------------------------------------------------
uint32_t AudioPolicyManager::nextAudioPortGeneration()
{
    return android_atomic_inc(&mAudioPortGeneration);
}

AudioPolicyManager::AudioPolicyManager(AudioPolicyClientInterface *clientInterface)
    :
#ifdef AUDIO_POLICY_TEST
    Thread(false),
#endif //AUDIO_POLICY_TEST
    mLimitRingtoneVolume(false), mLastVoiceVolume(-1.0f),
    mA2dpSuspended(false),
    mAudioPortGeneration(1),
    mBeaconMuteRefCount(0),
    mBeaconPlayingRefCount(0),
    mBeaconMuted(false),
    mTtsOutputAvailable(false),
    mMasterMono(false)
{
    mUidCached = getuid();
    mpClientInterface = clientInterface;

    // TODO: remove when legacy conf file is removed. true on devices that use DRC on the
    // DEVICE_CATEGORY_SPEAKER path to boost soft sounds, used to adjust volume curves accordingly.
    // Note: remove also speaker_drc_enabled from global configuration of XML config file.
    bool speakerDrcEnabled = false;

#ifdef USE_XML_AUDIO_POLICY_CONF
    mVolumeCurves = new VolumeCurvesCollection();
    AudioPolicyConfig config(mHwModules, mAvailableOutputDevices, mAvailableInputDevices,
                             mDefaultOutputDevice, speakerDrcEnabled,
                             static_cast<VolumeCurvesCollection *>(mVolumeCurves));
    PolicySerializer serializer;
    if (serializer.deserialize(AUDIO_POLICY_XML_CONFIG_FILE, config) != NO_ERROR) {
#else
    mVolumeCurves = new StreamDescriptorCollection();
    AudioPolicyConfig config(mHwModules, mAvailableOutputDevices, mAvailableInputDevices,
                             mDefaultOutputDevice, speakerDrcEnabled);
    if ((ConfigParsingUtils::loadConfig(AUDIO_POLICY_VENDOR_CONFIG_FILE, config) != NO_ERROR) &&
            (ConfigParsingUtils::loadConfig(AUDIO_POLICY_CONFIG_FILE, config) != NO_ERROR)) {
#endif
        ALOGE("could not load audio policy configuration file, setting defaults");
        config.setDefault();
    }
    // must be done after reading the policy (since conditionned by Speaker Drc Enabling)
    mVolumeCurves->initializeVolumeCurves(speakerDrcEnabled);

    // Once policy config has been parsed, retrieve an instance of the engine and initialize it.
    audio_policy::EngineInstance *engineInstance = audio_policy::EngineInstance::getInstance();
    if (!engineInstance) {
        ALOGE("%s:  Could not get an instance of policy engine", __FUNCTION__);
        return;
    }
    // Retrieve the Policy Manager Interface
    mEngine = engineInstance->queryInterface<AudioPolicyManagerInterface>();
    if (mEngine == NULL) {
        ALOGE("%s: Failed to get Policy Engine Interface", __FUNCTION__);
        return;
    }
    mEngine->setObserver(this);
    status_t status = mEngine->initCheck();
    (void) status;
    ALOG_ASSERT(status == NO_ERROR, "Policy engine not initialized(err=%d)", status);

    // mAvailableOutputDevices and mAvailableInputDevices now contain all attached devices
    // open all output streams needed to access attached devices
    audio_devices_t outputDeviceTypes = mAvailableOutputDevices.types();
    audio_devices_t inputDeviceTypes = mAvailableInputDevices.types() & ~AUDIO_DEVICE_BIT_IN;
    for (size_t i = 0; i < mHwModules.size(); i++) {
        mHwModules[i]->mHandle = mpClientInterface->loadHwModule(mHwModules[i]->getName());
        if (mHwModules[i]->mHandle == 0) {
            ALOGW("could not open HW module %s", mHwModules[i]->getName());
            continue;
        }
        // open all output streams needed to access attached devices
        // except for direct output streams that are only opened when they are actually
        // required by an app.
        // This also validates mAvailableOutputDevices list
        for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
        {
            const sp<IOProfile> outProfile = mHwModules[i]->mOutputProfiles[j];

            if (!outProfile->hasSupportedDevices()) {
                ALOGW("Output profile contains no device on module %s", mHwModules[i]->getName());
                continue;
            }
            if ((outProfile->getFlags() & AUDIO_OUTPUT_FLAG_TTS) != 0) {
                mTtsOutputAvailable = true;
            }

            if ((outProfile->getFlags() & AUDIO_OUTPUT_FLAG_DIRECT) != 0) {
                continue;
            }
            audio_devices_t profileType = outProfile->getSupportedDevicesType();
            if ((profileType & mDefaultOutputDevice->type()) != AUDIO_DEVICE_NONE) {
                profileType = mDefaultOutputDevice->type();
            } else {
                // chose first device present in profile's SupportedDevices also part of
                // outputDeviceTypes
                profileType = outProfile->getSupportedDeviceForType(outputDeviceTypes);
            }
            if ((profileType & outputDeviceTypes) == 0) {
                continue;
            }
            sp<SwAudioOutputDescriptor> outputDesc = new SwAudioOutputDescriptor(outProfile,
                                                                                 mpClientInterface);
            const DeviceVector &supportedDevices = outProfile->getSupportedDevices();
            const DeviceVector &devicesForType = supportedDevices.getDevicesFromType(profileType);
            String8 address = devicesForType.size() > 0 ? devicesForType.itemAt(0)->mAddress
                    : String8("");

            outputDesc->mDevice = profileType;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = outputDesc->mSamplingRate;
            config.channel_mask = outputDesc->mChannelMask;
            config.format = outputDesc->mFormat;
            audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
            status_t status = mpClientInterface->openOutput(outProfile->getModuleHandle(),
                                                            &output,
                                                            &config,
                                                            &outputDesc->mDevice,
                                                            address,
                                                            &outputDesc->mLatency,
                                                            outputDesc->mFlags);

            if (status != NO_ERROR) {
                ALOGW("Cannot open output stream for device %08x on hw module %s",
                      outputDesc->mDevice,
                      mHwModules[i]->getName());
            } else {
                outputDesc->mSamplingRate = config.sample_rate;
                outputDesc->mChannelMask = config.channel_mask;
                outputDesc->mFormat = config.format;

                for (size_t k = 0; k  < supportedDevices.size(); k++) {
                    ssize_t index = mAvailableOutputDevices.indexOf(supportedDevices[k]);
                    // give a valid ID to an attached device once confirmed it is reachable
                    if (index >= 0 && !mAvailableOutputDevices[index]->isAttached()) {
                        mAvailableOutputDevices[index]->attach(mHwModules[i]);
                    }
                }
                if (mPrimaryOutput == 0 &&
                        outProfile->getFlags() & AUDIO_OUTPUT_FLAG_PRIMARY) {
                    mPrimaryOutput = outputDesc;
                }
                addOutput(output, outputDesc);
                setOutputDevice(outputDesc,
                                outputDesc->mDevice,
                                true,
                                0,
                                NULL,
                                address.string());
            }
        }
        // open input streams needed to access attached devices to validate
        // mAvailableInputDevices list
        for (size_t j = 0; j < mHwModules[i]->mInputProfiles.size(); j++)
        {
            const sp<IOProfile> inProfile = mHwModules[i]->mInputProfiles[j];

            if (!inProfile->hasSupportedDevices()) {
                ALOGW("Input profile contains no device on module %s", mHwModules[i]->getName());
                continue;
            }
            // chose first device present in profile's SupportedDevices also part of
            // inputDeviceTypes
            audio_devices_t profileType = inProfile->getSupportedDeviceForType(inputDeviceTypes);

            if ((profileType & inputDeviceTypes) == 0) {
                continue;
            }
            sp<AudioInputDescriptor> inputDesc =
                    new AudioInputDescriptor(inProfile);

            inputDesc->mDevice = profileType;

            // find the address
            DeviceVector inputDevices = mAvailableInputDevices.getDevicesFromType(profileType);
            //   the inputs vector must be of size 1, but we don't want to crash here
            String8 address = inputDevices.size() > 0 ? inputDevices.itemAt(0)->mAddress
                    : String8("");
            ALOGV("  for input device 0x%x using address %s", profileType, address.string());
            ALOGE_IF(inputDevices.size() == 0, "Input device list is empty!");

            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = inputDesc->mSamplingRate;
            config.channel_mask = inputDesc->mChannelMask;
            config.format = inputDesc->mFormat;
            audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
            status_t status = mpClientInterface->openInput(inProfile->getModuleHandle(),
                                                           &input,
                                                           &config,
                                                           &inputDesc->mDevice,
                                                           address,
                                                           AUDIO_SOURCE_MIC,
                                                           AUDIO_INPUT_FLAG_NONE);

            if (status == NO_ERROR) {
                const DeviceVector &supportedDevices = inProfile->getSupportedDevices();
                for (size_t k = 0; k  < supportedDevices.size(); k++) {
                    ssize_t index =  mAvailableInputDevices.indexOf(supportedDevices[k]);
                    // give a valid ID to an attached device once confirmed it is reachable
                    if (index >= 0) {
                        sp<DeviceDescriptor> devDesc = mAvailableInputDevices[index];
                        if (!devDesc->isAttached()) {
                            devDesc->attach(mHwModules[i]);
                            devDesc->importAudioPort(inProfile);
                        }
                    }
                }
                mpClientInterface->closeInput(input);
            } else {
                ALOGW("Cannot open input stream for device %08x on hw module %s",
                      inputDesc->mDevice,
                      mHwModules[i]->getName());
            }
        }
    }
    // make sure all attached devices have been allocated a unique ID
    for (size_t i = 0; i  < mAvailableOutputDevices.size();) {
        if (!mAvailableOutputDevices[i]->isAttached()) {
            ALOGW("Output device %08x unreachable", mAvailableOutputDevices[i]->type());
            mAvailableOutputDevices.remove(mAvailableOutputDevices[i]);
            continue;
        }
        // The device is now validated and can be appended to the available devices of the engine
        mEngine->setDeviceConnectionState(mAvailableOutputDevices[i],
                                          AUDIO_POLICY_DEVICE_STATE_AVAILABLE);
        i++;
    }
    for (size_t i = 0; i  < mAvailableInputDevices.size();) {
        if (!mAvailableInputDevices[i]->isAttached()) {
            ALOGW("Input device %08x unreachable", mAvailableInputDevices[i]->type());
            mAvailableInputDevices.remove(mAvailableInputDevices[i]);
            continue;
        }
        // The device is now validated and can be appended to the available devices of the engine
        mEngine->setDeviceConnectionState(mAvailableInputDevices[i],
                                          AUDIO_POLICY_DEVICE_STATE_AVAILABLE);
        i++;
    }
    // make sure default device is reachable
    if (mDefaultOutputDevice == 0 || mAvailableOutputDevices.indexOf(mDefaultOutputDevice) < 0) {
        ALOGE("Default device %08x is unreachable", mDefaultOutputDevice->type());
    }

    ALOGE_IF((mPrimaryOutput == 0), "Failed to open primary output");

    updateDevicesAndOutputs();

#ifdef AUDIO_POLICY_TEST
    if (mPrimaryOutput != 0) {
        AudioParameter outputCmd = AudioParameter();
        outputCmd.addInt(String8("set_id"), 0);
        mpClientInterface->setParameters(mPrimaryOutput->mIoHandle, outputCmd.toString());

        mTestDevice = AUDIO_DEVICE_OUT_SPEAKER;
        mTestSamplingRate = 44100;
        mTestFormat = AUDIO_FORMAT_PCM_16_BIT;
        mTestChannels =  AUDIO_CHANNEL_OUT_STEREO;
        mTestLatencyMs = 0;
        mCurOutput = 0;
        mDirectOutput = false;
        for (int i = 0; i < NUM_TEST_OUTPUTS; i++) {
            mTestOutputs[i] = 0;
        }

        const size_t SIZE = 256;
        char buffer[SIZE];
        snprintf(buffer, SIZE, "AudioPolicyManagerTest");
        run(buffer, ANDROID_PRIORITY_AUDIO);
    }
#endif //AUDIO_POLICY_TEST
}

AudioPolicyManager::~AudioPolicyManager()
{
#ifdef AUDIO_POLICY_TEST
    exit();
#endif //AUDIO_POLICY_TEST
   for (size_t i = 0; i < mOutputs.size(); i++) {
        mpClientInterface->closeOutput(mOutputs.keyAt(i));
   }
   for (size_t i = 0; i < mInputs.size(); i++) {
        mpClientInterface->closeInput(mInputs.keyAt(i));
   }
   mAvailableOutputDevices.clear();
   mAvailableInputDevices.clear();
   mOutputs.clear();
   mInputs.clear();
   mHwModules.clear();
}

status_t AudioPolicyManager::initCheck()
{
    return hasPrimaryOutput() ? NO_ERROR : NO_INIT;
}

#ifdef AUDIO_POLICY_TEST
bool AudioPolicyManager::threadLoop()
{
    ALOGV("entering threadLoop()");
    while (!exitPending())
    {
        String8 command;
        int valueInt;
        String8 value;

        Mutex::Autolock _l(mLock);
        mWaitWorkCV.waitRelative(mLock, milliseconds(50));

        command = mpClientInterface->getParameters(0, String8("test_cmd_policy"));
        AudioParameter param = AudioParameter(command);

        if (param.getInt(String8("test_cmd_policy"), valueInt) == NO_ERROR &&
            valueInt != 0) {
            ALOGV("Test command %s received", command.string());
            String8 target;
            if (param.get(String8("target"), target) != NO_ERROR) {
                target = "Manager";
            }
            if (param.getInt(String8("test_cmd_policy_output"), valueInt) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_output"));
                mCurOutput = valueInt;
            }
            if (param.get(String8("test_cmd_policy_direct"), value) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_direct"));
                if (value == "false") {
                    mDirectOutput = false;
                } else if (value == "true") {
                    mDirectOutput = true;
                }
            }
            if (param.getInt(String8("test_cmd_policy_input"), valueInt) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_input"));
                mTestInput = valueInt;
            }

            if (param.get(String8("test_cmd_policy_format"), value) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_format"));
                int format = AUDIO_FORMAT_INVALID;
                if (value == "PCM 16 bits") {
                    format = AUDIO_FORMAT_PCM_16_BIT;
                } else if (value == "PCM 8 bits") {
                    format = AUDIO_FORMAT_PCM_8_BIT;
                } else if (value == "Compressed MP3") {
                    format = AUDIO_FORMAT_MP3;
                }
                if (format != AUDIO_FORMAT_INVALID) {
                    if (target == "Manager") {
                        mTestFormat = format;
                    } else if (mTestOutputs[mCurOutput] != 0) {
                        AudioParameter outputParam = AudioParameter();
                        outputParam.addInt(String8("format"), format);
                        mpClientInterface->setParameters(mTestOutputs[mCurOutput], outputParam.toString());
                    }
                }
            }
            if (param.get(String8("test_cmd_policy_channels"), value) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_channels"));
                int channels = 0;

                if (value == "Channels Stereo") {
                    channels =  AUDIO_CHANNEL_OUT_STEREO;
                } else if (value == "Channels Mono") {
                    channels =  AUDIO_CHANNEL_OUT_MONO;
                }
                if (channels != 0) {
                    if (target == "Manager") {
                        mTestChannels = channels;
                    } else if (mTestOutputs[mCurOutput] != 0) {
                        AudioParameter outputParam = AudioParameter();
                        outputParam.addInt(String8("channels"), channels);
                        mpClientInterface->setParameters(mTestOutputs[mCurOutput], outputParam.toString());
                    }
                }
            }
            if (param.getInt(String8("test_cmd_policy_sampleRate"), valueInt) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_sampleRate"));
                if (valueInt >= 0 && valueInt <= 96000) {
                    int samplingRate = valueInt;
                    if (target == "Manager") {
                        mTestSamplingRate = samplingRate;
                    } else if (mTestOutputs[mCurOutput] != 0) {
                        AudioParameter outputParam = AudioParameter();
                        outputParam.addInt(String8("sampling_rate"), samplingRate);
                        mpClientInterface->setParameters(mTestOutputs[mCurOutput], outputParam.toString());
                    }
                }
            }

            if (param.get(String8("test_cmd_policy_reopen"), value) == NO_ERROR) {
                param.remove(String8("test_cmd_policy_reopen"));

                mpClientInterface->closeOutput(mpClientInterface->closeOutput(mPrimaryOutput););

                audio_module_handle_t moduleHandle = mPrimaryOutput->getModuleHandle();

                removeOutput(mPrimaryOutput->mIoHandle);
                sp<SwAudioOutputDescriptor> outputDesc = new AudioOutputDescriptor(NULL,
                                                                               mpClientInterface);
                outputDesc->mDevice = AUDIO_DEVICE_OUT_SPEAKER;
                audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                config.sample_rate = outputDesc->mSamplingRate;
                config.channel_mask = outputDesc->mChannelMask;
                config.format = outputDesc->mFormat;
                audio_io_handle_t handle;
                status_t status = mpClientInterface->openOutput(moduleHandle,
                                                                &handle,
                                                                &config,
                                                                &outputDesc->mDevice,
                                                                String8(""),
                                                                &outputDesc->mLatency,
                                                                outputDesc->mFlags);
                if (status != NO_ERROR) {
                    ALOGE("Failed to reopen hardware output stream, "
                        "samplingRate: %d, format %d, channels %d",
                        outputDesc->mSamplingRate, outputDesc->mFormat, outputDesc->mChannelMask);
                } else {
                    outputDesc->mSamplingRate = config.sample_rate;
                    outputDesc->mChannelMask = config.channel_mask;
                    outputDesc->mFormat = config.format;
                    mPrimaryOutput = outputDesc;
                    AudioParameter outputCmd = AudioParameter();
                    outputCmd.addInt(String8("set_id"), 0);
                    mpClientInterface->setParameters(handle, outputCmd.toString());
                    addOutput(handle, outputDesc);
                }
            }


            mpClientInterface->setParameters(0, String8("test_cmd_policy="));
        }
    }
    return false;
}

void AudioPolicyManager::exit()
{
    {
        AutoMutex _l(mLock);
        requestExit();
        mWaitWorkCV.signal();
    }
    requestExitAndWait();
}

int AudioPolicyManager::testOutputIndex(audio_io_handle_t output)
{
    for (int i = 0; i < NUM_TEST_OUTPUTS; i++) {
        if (output == mTestOutputs[i]) return i;
    }
    return 0;
}
#endif //AUDIO_POLICY_TEST

// ---

void AudioPolicyManager::addOutput(audio_io_handle_t output, sp<SwAudioOutputDescriptor> outputDesc)
{
    outputDesc->setIoHandle(output);
    mOutputs.add(output, outputDesc);
    updateMono(output); // update mono status when adding to output list
    nextAudioPortGeneration();
}

void AudioPolicyManager::removeOutput(audio_io_handle_t output)
{
    mOutputs.removeItem(output);
}

void AudioPolicyManager::addInput(audio_io_handle_t input, sp<AudioInputDescriptor> inputDesc)
{
    inputDesc->setIoHandle(input);
    mInputs.add(input, inputDesc);
    nextAudioPortGeneration();
}

void AudioPolicyManager::findIoHandlesByAddress(sp<SwAudioOutputDescriptor> desc /*in*/,
        const audio_devices_t device /*in*/,
        const String8 address /*in*/,
        SortedVector<audio_io_handle_t>& outputs /*out*/) {
    sp<DeviceDescriptor> devDesc =
        desc->mProfile->getSupportedDeviceByAddress(device, address);
    if (devDesc != 0) {
        ALOGV("findIoHandlesByAddress(): adding opened output %d on same address %s",
              desc->mIoHandle, address.string());
        outputs.add(desc->mIoHandle);
    }
}

status_t AudioPolicyManager::checkOutputsForDevice(const sp<DeviceDescriptor> devDesc,
                                                   audio_policy_dev_state_t state,
                                                   SortedVector<audio_io_handle_t>& outputs,
                                                   const String8 address)
{
    audio_devices_t device = devDesc->type();
    sp<SwAudioOutputDescriptor> desc;

    if (audio_device_is_digital(device)) {
        // erase all current sample rates, formats and channel masks
        devDesc->clearAudioProfiles();
    }

    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        // first list already open outputs that can be routed to this device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (desc->supportedDevices() & device)) {
                if (!device_distinguishes_on_address(device)) {
                    ALOGV("checkOutputsForDevice(): adding opened output %d", mOutputs.keyAt(i));
                    outputs.add(mOutputs.keyAt(i));
                } else {
                    ALOGV("  checking address match due to device 0x%x", device);
                    findIoHandlesByAddress(desc, device, address, outputs);
                }
            }
        }
        // then look for output profiles that can be routed to this device
        SortedVector< sp<IOProfile> > profiles;
        for (size_t i = 0; i < mHwModules.size(); i++)
        {
            if (mHwModules[i]->mHandle == 0) {
                continue;
            }
            for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
            {
                sp<IOProfile> profile = mHwModules[i]->mOutputProfiles[j];
                if (profile->supportDevice(device)) {
                    if (!device_distinguishes_on_address(device) ||
                            profile->supportDeviceAddress(address)) {
                        profiles.add(profile);
                        ALOGV("checkOutputsForDevice(): adding profile %zu from module %zu", j, i);
                    }
                }
            }
        }

        ALOGV("  found %zu profiles, %zu outputs", profiles.size(), outputs.size());

        if (profiles.isEmpty() && outputs.isEmpty()) {
            ALOGW("checkOutputsForDevice(): No output available for device %04x", device);
            return BAD_VALUE;
        }

        // open outputs for matching profiles if needed. Direct outputs are also opened to
        // query for dynamic parameters and will be closed later by setDeviceConnectionState()
        for (ssize_t profile_index = 0; profile_index < (ssize_t)profiles.size(); profile_index++) {
            sp<IOProfile> profile = profiles[profile_index];

            // nothing to do if one output is already opened for this profile
            size_t j;
            for (j = 0; j < outputs.size(); j++) {
                desc = mOutputs.valueFor(outputs.itemAt(j));
                if (!desc->isDuplicated() && desc->mProfile == profile) {
                    // matching profile: save the sample rates, format and channel masks supported
                    // by the profile in our device descriptor
                    if (audio_device_is_digital(device)) {
                        devDesc->importAudioPort(profile);
                    }
                    break;
                }
            }
            if (j != outputs.size()) {
                continue;
            }

            ALOGV("opening output for device %08x with params %s profile %p",
                                                      device, address.string(), profile.get());
            desc = new SwAudioOutputDescriptor(profile, mpClientInterface);
            desc->mDevice = device;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = desc->mSamplingRate;
            config.channel_mask = desc->mChannelMask;
            config.format = desc->mFormat;
            config.offload_info.sample_rate = desc->mSamplingRate;
            config.offload_info.channel_mask = desc->mChannelMask;
            config.offload_info.format = desc->mFormat;
            audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
            status_t status = mpClientInterface->openOutput(profile->getModuleHandle(),
                                                            &output,
                                                            &config,
                                                            &desc->mDevice,
                                                            address,
                                                            &desc->mLatency,
                                                            desc->mFlags);
            if (status == NO_ERROR) {
                desc->mSamplingRate = config.sample_rate;
                desc->mChannelMask = config.channel_mask;
                desc->mFormat = config.format;

                // Here is where the out_set_parameters() for card & device gets called
                if (!address.isEmpty()) {
                    char *param = audio_device_address_to_parameter(device, address);
                    mpClientInterface->setParameters(output, String8(param));
                    free(param);
                }
                updateAudioProfiles(device, output, profile->getAudioProfiles());
                if (!profile->hasValidAudioProfile()) {
                    ALOGW("checkOutputsForDevice() missing param");
                    mpClientInterface->closeOutput(output);
                    output = AUDIO_IO_HANDLE_NONE;
                } else if (profile->hasDynamicAudioProfile()) {
                    mpClientInterface->closeOutput(output);
                    output = AUDIO_IO_HANDLE_NONE;
                    profile->pickAudioProfile(config.sample_rate, config.channel_mask, config.format);
                    config.offload_info.sample_rate = config.sample_rate;
                    config.offload_info.channel_mask = config.channel_mask;
                    config.offload_info.format = config.format;
                    status = mpClientInterface->openOutput(profile->getModuleHandle(),
                                                           &output,
                                                           &config,
                                                           &desc->mDevice,
                                                           address,
                                                           &desc->mLatency,
                                                           desc->mFlags);
                    if (status == NO_ERROR) {
                        desc->mSamplingRate = config.sample_rate;
                        desc->mChannelMask = config.channel_mask;
                        desc->mFormat = config.format;
                    } else {
                        output = AUDIO_IO_HANDLE_NONE;
                    }
                }

                if (output != AUDIO_IO_HANDLE_NONE) {
                    addOutput(output, desc);
                    if (device_distinguishes_on_address(device) && address != "0") {
                        sp<AudioPolicyMix> policyMix;
                        if (mPolicyMixes.getAudioPolicyMix(address, policyMix) != NO_ERROR) {
                            ALOGE("checkOutputsForDevice() cannot find policy for address %s",
                                  address.string());
                        }
                        policyMix->setOutput(desc);
                        desc->mPolicyMix = policyMix->getMix();

                    } else if (((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) &&
                                    hasPrimaryOutput()) {
                        // no duplicated output for direct outputs and
                        // outputs used by dynamic policy mixes
                        audio_io_handle_t duplicatedOutput = AUDIO_IO_HANDLE_NONE;

                        // set initial stream volume for device
                        applyStreamVolumes(desc, device, 0, true);

                        //TODO: configure audio effect output stage here

                        // open a duplicating output thread for the new output and the primary output
                        duplicatedOutput =
                                mpClientInterface->openDuplicateOutput(output,
                                                                       mPrimaryOutput->mIoHandle);
                        if (duplicatedOutput != AUDIO_IO_HANDLE_NONE) {
                            // add duplicated output descriptor
                            sp<SwAudioOutputDescriptor> dupOutputDesc =
                                    new SwAudioOutputDescriptor(NULL, mpClientInterface);
                            dupOutputDesc->mOutput1 = mPrimaryOutput;
                            dupOutputDesc->mOutput2 = desc;
                            dupOutputDesc->mSamplingRate = desc->mSamplingRate;
                            dupOutputDesc->mFormat = desc->mFormat;
                            dupOutputDesc->mChannelMask = desc->mChannelMask;
                            dupOutputDesc->mLatency = desc->mLatency;
                            addOutput(duplicatedOutput, dupOutputDesc);
                            applyStreamVolumes(dupOutputDesc, device, 0, true);
                        } else {
                            ALOGW("checkOutputsForDevice() could not open dup output for %d and %d",
                                    mPrimaryOutput->mIoHandle, output);
                            mpClientInterface->closeOutput(output);
                            removeOutput(output);
                            nextAudioPortGeneration();
                            output = AUDIO_IO_HANDLE_NONE;
                        }
                    }
                }
            } else {
                output = AUDIO_IO_HANDLE_NONE;
            }
            if (output == AUDIO_IO_HANDLE_NONE) {
                ALOGW("checkOutputsForDevice() could not open output for device %x", device);
                profiles.removeAt(profile_index);
                profile_index--;
            } else {
                outputs.add(output);
                // Load digital format info only for digital devices
                if (audio_device_is_digital(device)) {
                    devDesc->importAudioPort(profile);
                }

                if (device_distinguishes_on_address(device)) {
                    ALOGV("checkOutputsForDevice(): setOutputDevice(dev=0x%x, addr=%s)",
                            device, address.string());
                    setOutputDevice(desc, device, true/*force*/, 0/*delay*/,
                            NULL/*patch handle*/, address.string());
                }
                ALOGV("checkOutputsForDevice(): adding output %d", output);
            }
        }

        if (profiles.isEmpty()) {
            ALOGW("checkOutputsForDevice(): No output available for device %04x", device);
            return BAD_VALUE;
        }
    } else { // Disconnect
        // check if one opened output is not needed any more after disconnecting one device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated()) {
                // exact match on device
                if (device_distinguishes_on_address(device) &&
                        (desc->supportedDevices() == device)) {
                    findIoHandlesByAddress(desc, device, address, outputs);
                } else if (!(desc->supportedDevices() & mAvailableOutputDevices.types())) {
                    ALOGV("checkOutputsForDevice(): disconnecting adding output %d",
                            mOutputs.keyAt(i));
                    outputs.add(mOutputs.keyAt(i));
                }
            }
        }
        // Clear any profiles associated with the disconnected device.
        for (size_t i = 0; i < mHwModules.size(); i++)
        {
            if (mHwModules[i]->mHandle == 0) {
                continue;
            }
            for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
            {
                sp<IOProfile> profile = mHwModules[i]->mOutputProfiles[j];
                if (profile->supportDevice(device)) {
                    ALOGV("checkOutputsForDevice(): "
                            "clearing direct output profile %zu on module %zu", j, i);
                    profile->clearAudioProfiles();
                }
            }
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::checkInputsForDevice(const sp<DeviceDescriptor> devDesc,
                                                  audio_policy_dev_state_t state,
                                                  SortedVector<audio_io_handle_t>& inputs,
                                                  const String8 address)
{
    audio_devices_t device = devDesc->type();
    sp<AudioInputDescriptor> desc;

    if (audio_device_is_digital(device)) {
        // erase all current sample rates, formats and channel masks
        devDesc->clearAudioProfiles();
    }

    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        // first list already open inputs that can be routed to this device
        for (size_t input_index = 0; input_index < mInputs.size(); input_index++) {
            desc = mInputs.valueAt(input_index);
            if (desc->mProfile->supportDevice(device)) {
                ALOGV("checkInputsForDevice(): adding opened input %d", mInputs.keyAt(input_index));
               inputs.add(mInputs.keyAt(input_index));
            }
        }

        // then look for input profiles that can be routed to this device
        SortedVector< sp<IOProfile> > profiles;
        for (size_t module_idx = 0; module_idx < mHwModules.size(); module_idx++)
        {
            if (mHwModules[module_idx]->mHandle == 0) {
                continue;
            }
            for (size_t profile_index = 0;
                 profile_index < mHwModules[module_idx]->mInputProfiles.size();
                 profile_index++)
            {
                sp<IOProfile> profile = mHwModules[module_idx]->mInputProfiles[profile_index];

                if (profile->supportDevice(device)) {
                    if (!device_distinguishes_on_address(device) ||
                            profile->supportDeviceAddress(address)) {
                        profiles.add(profile);
                        ALOGV("checkInputsForDevice(): adding profile %zu from module %zu",
                              profile_index, module_idx);
                    }
                }
            }
        }

        if (profiles.isEmpty() && inputs.isEmpty()) {
            ALOGW("checkInputsForDevice(): No input available for device 0x%X", device);
            return BAD_VALUE;
        }

        // open inputs for matching profiles if needed. Direct inputs are also opened to
        // query for dynamic parameters and will be closed later by setDeviceConnectionState()
        for (ssize_t profile_index = 0; profile_index < (ssize_t)profiles.size(); profile_index++) {

            sp<IOProfile> profile = profiles[profile_index];
            // nothing to do if one input is already opened for this profile
            size_t input_index;
            for (input_index = 0; input_index < mInputs.size(); input_index++) {
                desc = mInputs.valueAt(input_index);
                if (desc->mProfile == profile) {
                    if (audio_device_is_digital(device)) {
                        devDesc->importAudioPort(profile);
                    }
                    break;
                }
            }
            if (input_index != mInputs.size()) {
                continue;
            }

            ALOGV("opening input for device 0x%X with params %s", device, address.string());
            desc = new AudioInputDescriptor(profile);
            desc->mDevice = device;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = desc->mSamplingRate;
            config.channel_mask = desc->mChannelMask;
            config.format = desc->mFormat;
            audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
            status_t status = mpClientInterface->openInput(profile->getModuleHandle(),
                                                           &input,
                                                           &config,
                                                           &desc->mDevice,
                                                           address,
                                                           AUDIO_SOURCE_MIC,
                                                           AUDIO_INPUT_FLAG_NONE /*FIXME*/);

            if (status == NO_ERROR) {
                desc->mSamplingRate = config.sample_rate;
                desc->mChannelMask = config.channel_mask;
                desc->mFormat = config.format;

                if (!address.isEmpty()) {
                    char *param = audio_device_address_to_parameter(device, address);
                    mpClientInterface->setParameters(input, String8(param));
                    free(param);
                }
                updateAudioProfiles(device, input, profile->getAudioProfiles());
                if (!profile->hasValidAudioProfile()) {
                    ALOGW("checkInputsForDevice() direct input missing param");
                    mpClientInterface->closeInput(input);
                    input = AUDIO_IO_HANDLE_NONE;
                }

                if (input != 0) {
                    addInput(input, desc);
                }
            } // endif input != 0

            if (input == AUDIO_IO_HANDLE_NONE) {
                ALOGW("checkInputsForDevice() could not open input for device 0x%X", device);
                profiles.removeAt(profile_index);
                profile_index--;
            } else {
                inputs.add(input);
                if (audio_device_is_digital(device)) {
                    devDesc->importAudioPort(profile);
                }
                ALOGV("checkInputsForDevice(): adding input %d", input);
            }
        } // end scan profiles

        if (profiles.isEmpty()) {
            ALOGW("checkInputsForDevice(): No input available for device 0x%X", device);
            return BAD_VALUE;
        }
    } else {
        // Disconnect
        // check if one opened input is not needed any more after disconnecting one device
        for (size_t input_index = 0; input_index < mInputs.size(); input_index++) {
            desc = mInputs.valueAt(input_index);
            if (!(desc->mProfile->supportDevice(mAvailableInputDevices.types()))) {
                ALOGV("checkInputsForDevice(): disconnecting adding input %d",
                      mInputs.keyAt(input_index));
                inputs.add(mInputs.keyAt(input_index));
            }
        }
        // Clear any profiles associated with the disconnected device.
        for (size_t module_index = 0; module_index < mHwModules.size(); module_index++) {
            if (mHwModules[module_index]->mHandle == 0) {
                continue;
            }
            for (size_t profile_index = 0;
                 profile_index < mHwModules[module_index]->mInputProfiles.size();
                 profile_index++) {
                sp<IOProfile> profile = mHwModules[module_index]->mInputProfiles[profile_index];
                if (profile->supportDevice(device)) {
                    ALOGV("checkInputsForDevice(): clearing direct input profile %zu on module %zu",
                          profile_index, module_index);
                    profile->clearAudioProfiles();
                }
            }
        }
    } // end disconnect

    return NO_ERROR;
}


void AudioPolicyManager::closeOutput(audio_io_handle_t output)
{
    ALOGV("closeOutput(%d)", output);

    sp<SwAudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    if (outputDesc == NULL) {
        ALOGW("closeOutput() unknown output %d", output);
        return;
    }
    mPolicyMixes.closeOutput(outputDesc);

    // look for duplicated outputs connected to the output being removed.
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> dupOutputDesc = mOutputs.valueAt(i);
        if (dupOutputDesc->isDuplicated() &&
                (dupOutputDesc->mOutput1 == outputDesc ||
                dupOutputDesc->mOutput2 == outputDesc)) {
            sp<AudioOutputDescriptor> outputDesc2;
            if (dupOutputDesc->mOutput1 == outputDesc) {
                outputDesc2 = dupOutputDesc->mOutput2;
            } else {
                outputDesc2 = dupOutputDesc->mOutput1;
            }
            // As all active tracks on duplicated output will be deleted,
            // and as they were also referenced on the other output, the reference
            // count for their stream type must be adjusted accordingly on
            // the other output.
            for (int j = 0; j < AUDIO_STREAM_CNT; j++) {
                int refCount = dupOutputDesc->mRefCount[j];
                outputDesc2->changeRefCount((audio_stream_type_t)j,-refCount);
            }
            audio_io_handle_t duplicatedOutput = mOutputs.keyAt(i);
            ALOGV("closeOutput() closing also duplicated output %d", duplicatedOutput);

            mpClientInterface->closeOutput(duplicatedOutput);
            removeOutput(duplicatedOutput);
        }
    }

    nextAudioPortGeneration();

    ssize_t index = mAudioPatches.indexOfKey(outputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        (void) /*status_t status*/ mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
        mAudioPatches.removeItemsAt(index);
        mpClientInterface->onAudioPatchListUpdate();
    }

    AudioParameter param;
    param.add(String8("closing"), String8("true"));
    mpClientInterface->setParameters(output, param.toString());

    mpClientInterface->closeOutput(output);
    removeOutput(output);
    mPreviousOutputs = mOutputs;
}

void AudioPolicyManager::closeInput(audio_io_handle_t input)
{
    ALOGV("closeInput(%d)", input);

    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);
    if (inputDesc == NULL) {
        ALOGW("closeInput() unknown input %d", input);
        return;
    }

    nextAudioPortGeneration();

    ssize_t index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        (void) /*status_t status*/ mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
        mAudioPatches.removeItemsAt(index);
        mpClientInterface->onAudioPatchListUpdate();
    }

    mpClientInterface->closeInput(input);
    mInputs.removeItem(input);
}

SortedVector<audio_io_handle_t> AudioPolicyManager::getOutputsForDevice(
                                                                audio_devices_t device,
                                                                SwAudioOutputCollection openOutputs)
{
    SortedVector<audio_io_handle_t> outputs;

    ALOGVV("getOutputsForDevice() device %04x", device);
    for (size_t i = 0; i < openOutputs.size(); i++) {
        ALOGVV("output %d isDuplicated=%d device=%04x",
                i, openOutputs.valueAt(i)->isDuplicated(),
                openOutputs.valueAt(i)->supportedDevices());
        if ((device & openOutputs.valueAt(i)->supportedDevices()) == device) {
            ALOGVV("getOutputsForDevice() found output %d", openOutputs.keyAt(i));
            outputs.add(openOutputs.keyAt(i));
        }
    }
    return outputs;
}

bool AudioPolicyManager::vectorsEqual(SortedVector<audio_io_handle_t>& outputs1,
                                      SortedVector<audio_io_handle_t>& outputs2)
{
    if (outputs1.size() != outputs2.size()) {
        return false;
    }
    for (size_t i = 0; i < outputs1.size(); i++) {
        if (outputs1[i] != outputs2[i]) {
            return false;
        }
    }
    return true;
}

void AudioPolicyManager::checkOutputForStrategy(routing_strategy strategy)
{
    audio_devices_t oldDevice = getDeviceForStrategy(strategy, true /*fromCache*/);
    audio_devices_t newDevice = getDeviceForStrategy(strategy, false /*fromCache*/);
    SortedVector<audio_io_handle_t> srcOutputs = getOutputsForDevice(oldDevice, mPreviousOutputs);
    SortedVector<audio_io_handle_t> dstOutputs = getOutputsForDevice(newDevice, mOutputs);

    // also take into account external policy-related changes: add all outputs which are
    // associated with policies in the "before" and "after" output vectors
    ALOGVV("checkOutputForStrategy(): policy related outputs");
    for (size_t i = 0 ; i < mPreviousOutputs.size() ; i++) {
        const sp<SwAudioOutputDescriptor> desc = mPreviousOutputs.valueAt(i);
        if (desc != 0 && desc->mPolicyMix != NULL) {
            srcOutputs.add(desc->mIoHandle);
            ALOGVV(" previous outputs: adding %d", desc->mIoHandle);
        }
    }
    for (size_t i = 0 ; i < mOutputs.size() ; i++) {
        const sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if (desc != 0 && desc->mPolicyMix != NULL) {
            dstOutputs.add(desc->mIoHandle);
            ALOGVV(" new outputs: adding %d", desc->mIoHandle);
        }
    }

    if (!vectorsEqual(srcOutputs,dstOutputs)) {
        ALOGV("checkOutputForStrategy() strategy %d, moving from output %d to output %d",
              strategy, srcOutputs[0], dstOutputs[0]);
        // mute strategy while moving tracks from one output to another
        for (size_t i = 0; i < srcOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueFor(srcOutputs[i]);
            if (isStrategyActive(desc, strategy)) {
                setStrategyMute(strategy, true, desc);
                setStrategyMute(strategy, false, desc, MUTE_TIME_MS, newDevice);
            }
            sp<AudioSourceDescriptor> source =
                    getSourceForStrategyOnOutput(srcOutputs[i], strategy);
            if (source != 0){
                connectAudioSource(source);
            }
        }

        // Move effects associated to this strategy from previous output to new output
        if (strategy == STRATEGY_MEDIA) {
            audio_io_handle_t fxOutput = selectOutputForEffects(dstOutputs);
            SortedVector<audio_io_handle_t> moved;
            for (size_t i = 0; i < mEffects.size(); i++) {
                sp<EffectDescriptor> effectDesc = mEffects.valueAt(i);
                if (effectDesc->mSession == AUDIO_SESSION_OUTPUT_MIX &&
                        effectDesc->mIo != fxOutput) {
                    if (moved.indexOf(effectDesc->mIo) < 0) {
                        ALOGV("checkOutputForStrategy() moving effect %d to output %d",
                              mEffects.keyAt(i), fxOutput);
                        mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, effectDesc->mIo,
                                                       fxOutput);
                        moved.add(effectDesc->mIo);
                    }
                    effectDesc->mIo = fxOutput;
                }
            }
        }
        // Move tracks associated to this strategy from previous output to new output
        for (int i = 0; i < AUDIO_STREAM_FOR_POLICY_CNT; i++) {
            if (getStrategy((audio_stream_type_t)i) == strategy) {
                mpClientInterface->invalidateStream((audio_stream_type_t)i);
            }
        }
    }
}

void AudioPolicyManager::checkOutputForAllStrategies()
{
    if (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED)
        checkOutputForStrategy(STRATEGY_ENFORCED_AUDIBLE);
    checkOutputForStrategy(STRATEGY_PHONE);
    if (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) != AUDIO_POLICY_FORCE_SYSTEM_ENFORCED)
        checkOutputForStrategy(STRATEGY_ENFORCED_AUDIBLE);
    checkOutputForStrategy(STRATEGY_SONIFICATION);
    checkOutputForStrategy(STRATEGY_SONIFICATION_RESPECTFUL);
    checkOutputForStrategy(STRATEGY_ACCESSIBILITY);
    checkOutputForStrategy(STRATEGY_MEDIA);
    checkOutputForStrategy(STRATEGY_DTMF);
    checkOutputForStrategy(STRATEGY_REROUTING);
}

void AudioPolicyManager::checkA2dpSuspend()
{
    audio_io_handle_t a2dpOutput = mOutputs.getA2dpOutput();
    if (a2dpOutput == 0) {
        mA2dpSuspended = false;
        return;
    }

    bool isScoConnected =
            ((mAvailableInputDevices.types() & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET &
                    ~AUDIO_DEVICE_BIT_IN) != 0) ||
            ((mAvailableOutputDevices.types() & AUDIO_DEVICE_OUT_ALL_SCO) != 0);
    // suspend A2DP output if:
    //      (NOT already suspended) &&
    //      ((SCO device is connected &&
    //       (forced usage for communication || for record is SCO))) ||
    //      (phone state is ringing || in call)
    //
    // restore A2DP output if:
    //      (Already suspended) &&
    //      ((SCO device is NOT connected ||
    //       (forced usage NOT for communication && NOT for record is SCO))) &&
    //      (phone state is NOT ringing && NOT in call)
    //
    if (mA2dpSuspended) {
        if ((!isScoConnected ||
             ((mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION) != AUDIO_POLICY_FORCE_BT_SCO) &&
              (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_RECORD) != AUDIO_POLICY_FORCE_BT_SCO))) &&
             ((mEngine->getPhoneState() != AUDIO_MODE_IN_CALL) &&
              (mEngine->getPhoneState() != AUDIO_MODE_RINGTONE))) {

            mpClientInterface->restoreOutput(a2dpOutput);
            mA2dpSuspended = false;
        }
    } else {
        if ((isScoConnected &&
             ((mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION) == AUDIO_POLICY_FORCE_BT_SCO) ||
              (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_RECORD) == AUDIO_POLICY_FORCE_BT_SCO))) ||
             ((mEngine->getPhoneState() == AUDIO_MODE_IN_CALL) ||
              (mEngine->getPhoneState() == AUDIO_MODE_RINGTONE))) {

            mpClientInterface->suspendOutput(a2dpOutput);
            mA2dpSuspended = true;
        }
    }
}

audio_devices_t AudioPolicyManager::getNewOutputDevice(const sp<AudioOutputDescriptor>& outputDesc,
                                                       bool fromCache)
{
    audio_devices_t device = AUDIO_DEVICE_NONE;

    ssize_t index = mAudioPatches.indexOfKey(outputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        if (patchDesc->mUid != mUidCached) {
            ALOGV("getNewOutputDevice() device %08x forced by patch %d",
                  outputDesc->device(), outputDesc->getPatchHandle());
            return outputDesc->device();
        }
    }

    // check the following by order of priority to request a routing change if necessary:
    // 1: the strategy enforced audible is active and enforced on the output:
    //      use device for strategy enforced audible
    // 2: we are in call or the strategy phone is active on the output:
    //      use device for strategy phone
    // 3: the strategy for enforced audible is active but not enforced on the output:
    //      use the device for strategy enforced audible
    // 4: the strategy sonification is active on the output:
    //      use device for strategy sonification
    // 5: the strategy accessibility is active on the output:
    //      use device for strategy accessibility
    // 6: the strategy "respectful" sonification is active on the output:
    //      use device for strategy "respectful" sonification
    // 7: the strategy media is active on the output:
    //      use device for strategy media
    // 8: the strategy DTMF is active on the output:
    //      use device for strategy DTMF
    // 9: the strategy for beacon, a.k.a. "transmitted through speaker" is active on the output:
    //      use device for strategy t-t-s
    if (isStrategyActive(outputDesc, STRATEGY_ENFORCED_AUDIBLE) &&
        mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
        device = getDeviceForStrategy(STRATEGY_ENFORCED_AUDIBLE, fromCache);
    } else if (isInCall() ||
                    isStrategyActive(outputDesc, STRATEGY_PHONE)) {
        device = getDeviceForStrategy(STRATEGY_PHONE, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_ENFORCED_AUDIBLE)) {
        device = getDeviceForStrategy(STRATEGY_ENFORCED_AUDIBLE, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_SONIFICATION)) {
        device = getDeviceForStrategy(STRATEGY_SONIFICATION, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_ACCESSIBILITY)) {
        device = getDeviceForStrategy(STRATEGY_ACCESSIBILITY, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_SONIFICATION_RESPECTFUL)) {
        device = getDeviceForStrategy(STRATEGY_SONIFICATION_RESPECTFUL, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_MEDIA)) {
        device = getDeviceForStrategy(STRATEGY_MEDIA, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_DTMF)) {
        device = getDeviceForStrategy(STRATEGY_DTMF, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_TRANSMITTED_THROUGH_SPEAKER)) {
        device = getDeviceForStrategy(STRATEGY_TRANSMITTED_THROUGH_SPEAKER, fromCache);
    } else if (isStrategyActive(outputDesc, STRATEGY_REROUTING)) {
        device = getDeviceForStrategy(STRATEGY_REROUTING, fromCache);
    }

    ALOGV("getNewOutputDevice() selected device %x", device);
    return device;
}

audio_devices_t AudioPolicyManager::getNewInputDevice(audio_io_handle_t input)
{
    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);

    ssize_t index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        if (patchDesc->mUid != mUidCached) {
            ALOGV("getNewInputDevice() device %08x forced by patch %d",
                  inputDesc->mDevice, inputDesc->getPatchHandle());
            return inputDesc->mDevice;
        }
    }

    audio_devices_t device = getDeviceAndMixForInputSource(inputDesc->inputSource());

    return device;
}

bool AudioPolicyManager::streamsMatchForvolume(audio_stream_type_t stream1,
                                               audio_stream_type_t stream2) {
    return ((stream1 == stream2) ||
            ((stream1 == AUDIO_STREAM_ACCESSIBILITY) && (stream2 == AUDIO_STREAM_MUSIC)) ||
            ((stream1 == AUDIO_STREAM_MUSIC) && (stream2 == AUDIO_STREAM_ACCESSIBILITY)));
}

uint32_t AudioPolicyManager::getStrategyForStream(audio_stream_type_t stream) {
    return (uint32_t)getStrategy(stream);
}

audio_devices_t AudioPolicyManager::getDevicesForStream(audio_stream_type_t stream) {
    // By checking the range of stream before calling getStrategy, we avoid
    // getStrategy's behavior for invalid streams.  getStrategy would do a ALOGE
    // and then return STRATEGY_MEDIA, but we want to return the empty set.
    if (stream < (audio_stream_type_t) 0 || stream >= AUDIO_STREAM_PUBLIC_CNT) {
        return AUDIO_DEVICE_NONE;
    }
    audio_devices_t devices = AUDIO_DEVICE_NONE;
    for (int curStream = 0; curStream < AUDIO_STREAM_FOR_POLICY_CNT; curStream++) {
        if (!streamsMatchForvolume(stream, (audio_stream_type_t)curStream)) {
            continue;
        }
        routing_strategy curStrategy = getStrategy((audio_stream_type_t)curStream);
        audio_devices_t curDevices =
                getDeviceForStrategy((routing_strategy)curStrategy, false /*fromCache*/);
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(curDevices, mOutputs);
        for (size_t i = 0; i < outputs.size(); i++) {
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(outputs[i]);
            if (outputDesc->isStreamActive((audio_stream_type_t)curStream)) {
                curDevices |= outputDesc->device();
            }
        }
        devices |= curDevices;
    }

    /*Filter SPEAKER_SAFE out of results, as AudioService doesn't know about it
      and doesn't really need to.*/
    if (devices & AUDIO_DEVICE_OUT_SPEAKER_SAFE) {
        devices |= AUDIO_DEVICE_OUT_SPEAKER;
        devices &= ~AUDIO_DEVICE_OUT_SPEAKER_SAFE;
    }
    return devices;
}

routing_strategy AudioPolicyManager::getStrategy(audio_stream_type_t stream) const
{
    ALOG_ASSERT(stream != AUDIO_STREAM_PATCH,"getStrategy() called for AUDIO_STREAM_PATCH");
    return mEngine->getStrategyForStream(stream);
}

uint32_t AudioPolicyManager::getStrategyForAttr(const audio_attributes_t *attr) {
    // flags to strategy mapping
    if ((attr->flags & AUDIO_FLAG_BEACON) == AUDIO_FLAG_BEACON) {
        return (uint32_t) STRATEGY_TRANSMITTED_THROUGH_SPEAKER;
    }
    if ((attr->flags & AUDIO_FLAG_AUDIBILITY_ENFORCED) == AUDIO_FLAG_AUDIBILITY_ENFORCED) {
        return (uint32_t) STRATEGY_ENFORCED_AUDIBLE;
    }
    // usage to strategy mapping
    return static_cast<uint32_t>(mEngine->getStrategyForUsage(attr->usage));
}

void AudioPolicyManager::handleNotificationRoutingForStream(audio_stream_type_t stream) {
    switch(stream) {
    case AUDIO_STREAM_MUSIC:
        checkOutputForStrategy(STRATEGY_SONIFICATION_RESPECTFUL);
        updateDevicesAndOutputs();
        break;
    default:
        break;
    }
}

uint32_t AudioPolicyManager::handleEventForBeacon(int event) {

    // skip beacon mute management if a dedicated TTS output is available
    if (mTtsOutputAvailable) {
        return 0;
    }

    switch(event) {
    case STARTING_OUTPUT:
        mBeaconMuteRefCount++;
        break;
    case STOPPING_OUTPUT:
        if (mBeaconMuteRefCount > 0) {
            mBeaconMuteRefCount--;
        }
        break;
    case STARTING_BEACON:
        mBeaconPlayingRefCount++;
        break;
    case STOPPING_BEACON:
        if (mBeaconPlayingRefCount > 0) {
            mBeaconPlayingRefCount--;
        }
        break;
    }

    if (mBeaconMuteRefCount > 0) {
        // any playback causes beacon to be muted
        return setBeaconMute(true);
    } else {
        // no other playback: unmute when beacon starts playing, mute when it stops
        return setBeaconMute(mBeaconPlayingRefCount == 0);
    }
}

uint32_t AudioPolicyManager::setBeaconMute(bool mute) {
    ALOGV("setBeaconMute(%d) mBeaconMuteRefCount=%d mBeaconPlayingRefCount=%d",
            mute, mBeaconMuteRefCount, mBeaconPlayingRefCount);
    // keep track of muted state to avoid repeating mute/unmute operations
    if (mBeaconMuted != mute) {
        // mute/unmute AUDIO_STREAM_TTS on all outputs
        ALOGV("\t muting %d", mute);
        uint32_t maxLatency = 0;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            setStreamMute(AUDIO_STREAM_TTS, mute/*on*/,
                    desc,
                    0 /*delay*/, AUDIO_DEVICE_NONE);
            const uint32_t latency = desc->latency() * 2;
            if (latency > maxLatency) {
                maxLatency = latency;
            }
        }
        mBeaconMuted = mute;
        return maxLatency;
    }
    return 0;
}

audio_devices_t AudioPolicyManager::getDeviceForStrategy(routing_strategy strategy,
                                                         bool fromCache)
{
    // Routing
    // see if we have an explicit route
    // scan the whole RouteMap, for each entry, convert the stream type to a strategy
    // (getStrategy(stream)).
    // if the strategy from the stream type in the RouteMap is the same as the argument above,
    // and activity count is non-zero
    // the device = the device from the descriptor in the RouteMap, and exit.
    for (size_t routeIndex = 0; routeIndex < mOutputRoutes.size(); routeIndex++) {
        sp<SessionRoute> route = mOutputRoutes.valueAt(routeIndex);
        routing_strategy routeStrategy = getStrategy(route->mStreamType);
        if ((routeStrategy == strategy) && route->isActive()) {
            return route->mDeviceDescriptor->type();
        }
    }

    if (fromCache) {
        ALOGVV("getDeviceForStrategy() from cache strategy %d, device %x",
              strategy, mDeviceForStrategy[strategy]);
        return mDeviceForStrategy[strategy];
    }
    return mEngine->getDeviceForStrategy(strategy);
}

void AudioPolicyManager::updateDevicesAndOutputs()
{
    for (int i = 0; i < NUM_STRATEGIES; i++) {
        mDeviceForStrategy[i] = getDeviceForStrategy((routing_strategy)i, false /*fromCache*/);
    }
    mPreviousOutputs = mOutputs;
}

uint32_t AudioPolicyManager::checkDeviceMuteStrategies(sp<AudioOutputDescriptor> outputDesc,
                                                       audio_devices_t prevDevice,
                                                       uint32_t delayMs)
{
    // mute/unmute strategies using an incompatible device combination
    // if muting, wait for the audio in pcm buffer to be drained before proceeding
    // if unmuting, unmute only after the specified delay
    if (outputDesc->isDuplicated()) {
        return 0;
    }

    uint32_t muteWaitMs = 0;
    audio_devices_t device = outputDesc->device();
    bool shouldMute = outputDesc->isActive() && (popcount(device) >= 2);

    for (size_t i = 0; i < NUM_STRATEGIES; i++) {
        audio_devices_t curDevice = getDeviceForStrategy((routing_strategy)i, false /*fromCache*/);
        curDevice = curDevice & outputDesc->supportedDevices();
        bool mute = shouldMute && (curDevice & device) && (curDevice != device);
        bool doMute = false;

        if (mute && !outputDesc->mStrategyMutedByDevice[i]) {
            doMute = true;
            outputDesc->mStrategyMutedByDevice[i] = true;
        } else if (!mute && outputDesc->mStrategyMutedByDevice[i]){
            doMute = true;
            outputDesc->mStrategyMutedByDevice[i] = false;
        }
        if (doMute) {
            for (size_t j = 0; j < mOutputs.size(); j++) {
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(j);
                // skip output if it does not share any device with current output
                if ((desc->supportedDevices() & outputDesc->supportedDevices())
                        == AUDIO_DEVICE_NONE) {
                    continue;
                }
                ALOGVV("checkDeviceMuteStrategies() %s strategy %d (curDevice %04x)",
                      mute ? "muting" : "unmuting", i, curDevice);
                setStrategyMute((routing_strategy)i, mute, desc, mute ? 0 : delayMs);
                if (isStrategyActive(desc, (routing_strategy)i)) {
                    if (mute) {
                        // FIXME: should not need to double latency if volume could be applied
                        // immediately by the audioflinger mixer. We must account for the delay
                        // between now and the next time the audioflinger thread for this output
                        // will process a buffer (which corresponds to one buffer size,
                        // usually 1/2 or 1/4 of the latency).
                        if (muteWaitMs < desc->latency() * 2) {
                            muteWaitMs = desc->latency() * 2;
                        }
                    }
                }
            }
        }
    }

    // temporary mute output if device selection changes to avoid volume bursts due to
    // different per device volumes
    if (outputDesc->isActive() && (device != prevDevice)) {
        uint32_t tempMuteWaitMs = outputDesc->latency() * 2;
        // temporary mute duration is conservatively set to 4 times the reported latency
        uint32_t tempMuteDurationMs = outputDesc->latency() * 4;
        if (muteWaitMs < tempMuteWaitMs) {
            muteWaitMs = tempMuteWaitMs;
        }

        for (size_t i = 0; i < NUM_STRATEGIES; i++) {
            if (isStrategyActive(outputDesc, (routing_strategy)i)) {
                // make sure that we do not start the temporary mute period too early in case of
                // delayed device change
                setStrategyMute((routing_strategy)i, true, outputDesc, delayMs);
                setStrategyMute((routing_strategy)i, false, outputDesc,
                                delayMs + tempMuteDurationMs, device);
            }
        }
    }

    // wait for the PCM output buffers to empty before proceeding with the rest of the command
    if (muteWaitMs > delayMs) {
        muteWaitMs -= delayMs;
        usleep(muteWaitMs * 1000);
        return muteWaitMs;
    }
    return 0;
}

uint32_t AudioPolicyManager::setOutputDevice(const sp<AudioOutputDescriptor>& outputDesc,
                                             audio_devices_t device,
                                             bool force,
                                             int delayMs,
                                             audio_patch_handle_t *patchHandle,
                                             const char* address)
{
    ALOGV("setOutputDevice() device %04x delayMs %d", device, delayMs);
    AudioParameter param;
    uint32_t muteWaitMs;

    if (outputDesc->isDuplicated()) {
        muteWaitMs = setOutputDevice(outputDesc->subOutput1(), device, force, delayMs);
        muteWaitMs += setOutputDevice(outputDesc->subOutput2(), device, force, delayMs);
        return muteWaitMs;
    }
    // no need to proceed if new device is not AUDIO_DEVICE_NONE and not supported by current
    // output profile
    if ((device != AUDIO_DEVICE_NONE) &&
            ((device & outputDesc->supportedDevices()) == 0)) {
        return 0;
    }

    // filter devices according to output selected
    device = (audio_devices_t)(device & outputDesc->supportedDevices());

    audio_devices_t prevDevice = outputDesc->mDevice;

    ALOGV("setOutputDevice() prevDevice 0x%04x", prevDevice);

    if (device != AUDIO_DEVICE_NONE) {
        outputDesc->mDevice = device;
    }
    muteWaitMs = checkDeviceMuteStrategies(outputDesc, prevDevice, delayMs);

    // Do not change the routing if:
    //      the requested device is AUDIO_DEVICE_NONE
    //      OR the requested device is the same as current device
    //  AND force is not specified
    //  AND the output is connected by a valid audio patch.
    // Doing this check here allows the caller to call setOutputDevice() without conditions
    if ((device == AUDIO_DEVICE_NONE || device == prevDevice) &&
        !force &&
        outputDesc->getPatchHandle() != 0) {
        ALOGV("setOutputDevice() setting same device 0x%04x or null device", device);
        return muteWaitMs;
    }

    ALOGV("setOutputDevice() changing device");

    // do the routing
    if (device == AUDIO_DEVICE_NONE) {
        resetOutputDevice(outputDesc, delayMs, NULL);
    } else {
        DeviceVector deviceList;
        if ((address == NULL) || (strlen(address) == 0)) {
            deviceList = mAvailableOutputDevices.getDevicesFromType(device);
        } else {
            deviceList = mAvailableOutputDevices.getDevicesFromTypeAddr(device, String8(address));
        }

        if (!deviceList.isEmpty()) {
            struct audio_patch patch;
            outputDesc->toAudioPortConfig(&patch.sources[0]);
            patch.num_sources = 1;
            patch.num_sinks = 0;
            for (size_t i = 0; i < deviceList.size() && i < AUDIO_PATCH_PORTS_MAX; i++) {
                deviceList.itemAt(i)->toAudioPortConfig(&patch.sinks[i]);
                patch.num_sinks++;
            }
            ssize_t index;
            if (patchHandle && *patchHandle != AUDIO_PATCH_HANDLE_NONE) {
                index = mAudioPatches.indexOfKey(*patchHandle);
            } else {
                index = mAudioPatches.indexOfKey(outputDesc->getPatchHandle());
            }
            sp< AudioPatch> patchDesc;
            audio_patch_handle_t afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
            if (index >= 0) {
                patchDesc = mAudioPatches.valueAt(index);
                afPatchHandle = patchDesc->mAfPatchHandle;
            }

            status_t status = mpClientInterface->createAudioPatch(&patch,
                                                                   &afPatchHandle,
                                                                   delayMs);
            ALOGV("setOutputDevice() createAudioPatch returned %d patchHandle %d"
                    "num_sources %d num_sinks %d",
                                       status, afPatchHandle, patch.num_sources, patch.num_sinks);
            if (status == NO_ERROR) {
                if (index < 0) {
                    patchDesc = new AudioPatch(&patch, mUidCached);
                    addAudioPatch(patchDesc->mHandle, patchDesc);
                } else {
                    patchDesc->mPatch = patch;
                }
                patchDesc->mAfPatchHandle = afPatchHandle;
                if (patchHandle) {
                    *patchHandle = patchDesc->mHandle;
                }
                outputDesc->setPatchHandle(patchDesc->mHandle);
                nextAudioPortGeneration();
                mpClientInterface->onAudioPatchListUpdate();
            }
        }

        // inform all input as well
        for (size_t i = 0; i < mInputs.size(); i++) {
            const sp<AudioInputDescriptor>  inputDescriptor = mInputs.valueAt(i);
            if (!is_virtual_input_device(inputDescriptor->mDevice)) {
                AudioParameter inputCmd = AudioParameter();
                ALOGV("%s: inform input %d of device:%d", __func__,
                      inputDescriptor->mIoHandle, device);
                inputCmd.addInt(String8(AudioParameter::keyRouting),device);
                mpClientInterface->setParameters(inputDescriptor->mIoHandle,
                                                 inputCmd.toString(),
                                                 delayMs);
            }
        }
    }

    // update stream volumes according to new device
    applyStreamVolumes(outputDesc, device, delayMs);

    return muteWaitMs;
}

status_t AudioPolicyManager::resetOutputDevice(const sp<AudioOutputDescriptor>& outputDesc,
                                               int delayMs,
                                               audio_patch_handle_t *patchHandle)
{
    ssize_t index;
    if (patchHandle) {
        index = mAudioPatches.indexOfKey(*patchHandle);
    } else {
        index = mAudioPatches.indexOfKey(outputDesc->getPatchHandle());
    }
    if (index < 0) {
        return INVALID_OPERATION;
    }
    sp< AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, delayMs);
    ALOGV("resetOutputDevice() releaseAudioPatch returned %d", status);
    outputDesc->setPatchHandle(AUDIO_PATCH_HANDLE_NONE);
    removeAudioPatch(patchDesc->mHandle);
    nextAudioPortGeneration();
    mpClientInterface->onAudioPatchListUpdate();
    return status;
}

status_t AudioPolicyManager::setInputDevice(audio_io_handle_t input,
                                            audio_devices_t device,
                                            bool force,
                                            audio_patch_handle_t *patchHandle)
{
    status_t status = NO_ERROR;

    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);
    if ((device != AUDIO_DEVICE_NONE) && ((device != inputDesc->mDevice) || force)) {
        inputDesc->mDevice = device;

        DeviceVector deviceList = mAvailableInputDevices.getDevicesFromType(device);
        if (!deviceList.isEmpty()) {
            struct audio_patch patch;
            inputDesc->toAudioPortConfig(&patch.sinks[0]);
            // AUDIO_SOURCE_HOTWORD is for internal use only:
            // handled as AUDIO_SOURCE_VOICE_RECOGNITION by the audio HAL
            if (patch.sinks[0].ext.mix.usecase.source == AUDIO_SOURCE_HOTWORD &&
                    !inputDesc->isSoundTrigger()) {
                patch.sinks[0].ext.mix.usecase.source = AUDIO_SOURCE_VOICE_RECOGNITION;
            }
            patch.num_sinks = 1;
            //only one input device for now
            deviceList.itemAt(0)->toAudioPortConfig(&patch.sources[0]);
            patch.num_sources = 1;
            ssize_t index;
            if (patchHandle && *patchHandle != AUDIO_PATCH_HANDLE_NONE) {
                index = mAudioPatches.indexOfKey(*patchHandle);
            } else {
                index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
            }
            sp< AudioPatch> patchDesc;
            audio_patch_handle_t afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
            if (index >= 0) {
                patchDesc = mAudioPatches.valueAt(index);
                afPatchHandle = patchDesc->mAfPatchHandle;
            }

            status_t status = mpClientInterface->createAudioPatch(&patch,
                                                                  &afPatchHandle,
                                                                  0);
            ALOGV("setInputDevice() createAudioPatch returned %d patchHandle %d",
                                                                          status, afPatchHandle);
            if (status == NO_ERROR) {
                if (index < 0) {
                    patchDesc = new AudioPatch(&patch, mUidCached);
                    addAudioPatch(patchDesc->mHandle, patchDesc);
                } else {
                    patchDesc->mPatch = patch;
                }
                patchDesc->mAfPatchHandle = afPatchHandle;
                if (patchHandle) {
                    *patchHandle = patchDesc->mHandle;
                }
                inputDesc->setPatchHandle(patchDesc->mHandle);
                nextAudioPortGeneration();
                mpClientInterface->onAudioPatchListUpdate();
            }
        }
    }
    return status;
}

status_t AudioPolicyManager::resetInputDevice(audio_io_handle_t input,
                                              audio_patch_handle_t *patchHandle)
{
    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);
    ssize_t index;
    if (patchHandle) {
        index = mAudioPatches.indexOfKey(*patchHandle);
    } else {
        index = mAudioPatches.indexOfKey(inputDesc->getPatchHandle());
    }
    if (index < 0) {
        return INVALID_OPERATION;
    }
    sp< AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
    ALOGV("resetInputDevice() releaseAudioPatch returned %d", status);
    inputDesc->setPatchHandle(AUDIO_PATCH_HANDLE_NONE);
    removeAudioPatch(patchDesc->mHandle);
    nextAudioPortGeneration();
    mpClientInterface->onAudioPatchListUpdate();
    return status;
}

sp<IOProfile> AudioPolicyManager::getInputProfile(audio_devices_t device,
                                                  String8 address,
                                                  uint32_t& samplingRate,
                                                  audio_format_t& format,
                                                  audio_channel_mask_t& channelMask,
                                                  audio_input_flags_t flags)
{
    // Choose an input profile based on the requested capture parameters: select the first available
    // profile supporting all requested parameters.
    //
    // TODO: perhaps isCompatibleProfile should return a "matching" score so we can return
    // the best matching profile, not the first one.

    for (size_t i = 0; i < mHwModules.size(); i++)
    {
        if (mHwModules[i]->mHandle == 0) {
            continue;
        }
        for (size_t j = 0; j < mHwModules[i]->mInputProfiles.size(); j++)
        {
            sp<IOProfile> profile = mHwModules[i]->mInputProfiles[j];
            // profile->log();
            if (profile->isCompatibleProfile(device, address, samplingRate,
                                             &samplingRate /*updatedSamplingRate*/,
                                             format,
                                             &format /*updatedFormat*/,
                                             channelMask,
                                             &channelMask /*updatedChannelMask*/,
                                             (audio_output_flags_t) flags)) {

                return profile;
            }
        }
    }
    return NULL;
}


audio_devices_t AudioPolicyManager::getDeviceAndMixForInputSource(audio_source_t inputSource,
                                                                  AudioMix **policyMix)
{
    audio_devices_t availableDeviceTypes = mAvailableInputDevices.types() & ~AUDIO_DEVICE_BIT_IN;
    audio_devices_t selectedDeviceFromMix =
           mPolicyMixes.getDeviceAndMixForInputSource(inputSource, availableDeviceTypes, policyMix);

    if (selectedDeviceFromMix != AUDIO_DEVICE_NONE) {
        return selectedDeviceFromMix;
    }
    return getDeviceForInputSource(inputSource);
}

audio_devices_t AudioPolicyManager::getDeviceForInputSource(audio_source_t inputSource)
{
    for (size_t routeIndex = 0; routeIndex < mInputRoutes.size(); routeIndex++) {
         sp<SessionRoute> route = mInputRoutes.valueAt(routeIndex);
         if (inputSource == route->mSource && route->isActive()) {
             return route->mDeviceDescriptor->type();
         }
     }

     return mEngine->getDeviceForInputSource(inputSource);
}

float AudioPolicyManager::computeVolume(audio_stream_type_t stream,
                                        int index,
                                        audio_devices_t device)
{
    float volumeDB = mVolumeCurves->volIndexToDb(stream, Volume::getDeviceCategory(device), index);

    // handle the case of accessibility active while a ringtone is playing: if the ringtone is much
    // louder than the accessibility prompt, the prompt cannot be heard, thus masking the touch
    // exploration of the dialer UI. In this situation, bring the accessibility volume closer to
    // the ringtone volume
    if ((stream == AUDIO_STREAM_ACCESSIBILITY)
            && (AUDIO_MODE_RINGTONE == mEngine->getPhoneState())
            && isStreamActive(AUDIO_STREAM_RING, 0)) {
        const float ringVolumeDB = computeVolume(AUDIO_STREAM_RING, index, device);
        return ringVolumeDB - 4 > volumeDB ? ringVolumeDB - 4 : volumeDB;
    }

    // if a headset is connected, apply the following rules to ring tones and notifications
    // to avoid sound level bursts in user's ears:
    // - always attenuate notifications volume by 6dB
    // - attenuate ring tones volume by 6dB unless music is not playing and
    // speaker is part of the select devices
    // - if music is playing, always limit the volume to current music volume,
    // with a minimum threshold at -36dB so that notification is always perceived.
    const routing_strategy stream_strategy = getStrategy(stream);
    if ((device & (AUDIO_DEVICE_OUT_BLUETOOTH_A2DP |
            AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES |
            AUDIO_DEVICE_OUT_WIRED_HEADSET |
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE)) &&
        ((stream_strategy == STRATEGY_SONIFICATION)
                || (stream_strategy == STRATEGY_SONIFICATION_RESPECTFUL)
                || (stream == AUDIO_STREAM_SYSTEM)
                || ((stream_strategy == STRATEGY_ENFORCED_AUDIBLE) &&
                    (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_NONE))) &&
            mVolumeCurves->canBeMuted(stream)) {
        // when the phone is ringing we must consider that music could have been paused just before
        // by the music application and behave as if music was active if the last music track was
        // just stopped
        if (isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY) ||
                mLimitRingtoneVolume) {
            volumeDB += SONIFICATION_HEADSET_VOLUME_FACTOR_DB;
            audio_devices_t musicDevice = getDeviceForStrategy(STRATEGY_MEDIA, true /*fromCache*/);
            float musicVolDB = computeVolume(AUDIO_STREAM_MUSIC,
                                             mVolumeCurves->getVolumeIndex(AUDIO_STREAM_MUSIC,
                                                                              musicDevice),
                                             musicDevice);
            float minVolDB = (musicVolDB > SONIFICATION_HEADSET_VOLUME_MIN_DB) ?
                    musicVolDB : SONIFICATION_HEADSET_VOLUME_MIN_DB;
            if (volumeDB > minVolDB) {
                volumeDB = minVolDB;
                ALOGV("computeVolume limiting volume to %f musicVol %f", minVolDB, musicVolDB);
            }
            if (device & (AUDIO_DEVICE_OUT_BLUETOOTH_A2DP |
                    AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES)) {
                // on A2DP, also ensure notification volume is not too low compared to media when
                // intended to be played
                if ((volumeDB > -96.0f) &&
                        (musicVolDB - SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB > volumeDB)) {
                    ALOGV("computeVolume increasing volume for stream=%d device=0x%X from %f to %f",
                            stream, device,
                            volumeDB, musicVolDB - SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB);
                    volumeDB = musicVolDB - SONIFICATION_A2DP_MAX_MEDIA_DIFF_DB;
                }
            }
        } else if ((Volume::getDeviceForVolume(device) != AUDIO_DEVICE_OUT_SPEAKER) ||
                stream_strategy != STRATEGY_SONIFICATION) {
            volumeDB += SONIFICATION_HEADSET_VOLUME_FACTOR_DB;
        }
    }

    return volumeDB;
}

status_t AudioPolicyManager::checkAndSetVolume(audio_stream_type_t stream,
                                                   int index,
                                                   const sp<AudioOutputDescriptor>& outputDesc,
                                                   audio_devices_t device,
                                                   int delayMs,
                                                   bool force)
{
    // do not change actual stream volume if the stream is muted
    if (outputDesc->mMuteCount[stream] != 0) {
        ALOGVV("checkAndSetVolume() stream %d muted count %d",
              stream, outputDesc->mMuteCount[stream]);
        return NO_ERROR;
    }
    audio_policy_forced_cfg_t forceUseForComm =
            mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION);
    // do not change in call volume if bluetooth is connected and vice versa
    if ((stream == AUDIO_STREAM_VOICE_CALL && forceUseForComm == AUDIO_POLICY_FORCE_BT_SCO) ||
        (stream == AUDIO_STREAM_BLUETOOTH_SCO && forceUseForComm != AUDIO_POLICY_FORCE_BT_SCO)) {
        ALOGV("checkAndSetVolume() cannot set stream %d volume with force use = %d for comm",
             stream, forceUseForComm);
        return INVALID_OPERATION;
    }

    if (device == AUDIO_DEVICE_NONE) {
        device = outputDesc->device();
    }

    float volumeDb = computeVolume(stream, index, device);
    if (outputDesc->isFixedVolume(device)) {
        volumeDb = 0.0f;
    }

    outputDesc->setVolume(volumeDb, stream, device, delayMs, force);

    if (stream == AUDIO_STREAM_VOICE_CALL ||
        stream == AUDIO_STREAM_BLUETOOTH_SCO) {
        float voiceVolume;
        // Force voice volume to max for bluetooth SCO as volume is managed by the headset
        if (stream == AUDIO_STREAM_VOICE_CALL) {
            voiceVolume = (float)index/(float)mVolumeCurves->getVolumeIndexMax(stream);
        } else {
            voiceVolume = 1.0;
        }

        if (voiceVolume != mLastVoiceVolume) {
            mpClientInterface->setVoiceVolume(voiceVolume, delayMs);
            mLastVoiceVolume = voiceVolume;
        }
    }

    return NO_ERROR;
}

void AudioPolicyManager::applyStreamVolumes(const sp<AudioOutputDescriptor>& outputDesc,
                                                audio_devices_t device,
                                                int delayMs,
                                                bool force)
{
    ALOGVV("applyStreamVolumes() for device %08x", device);

    for (int stream = 0; stream < AUDIO_STREAM_FOR_POLICY_CNT; stream++) {
        checkAndSetVolume((audio_stream_type_t)stream,
                          mVolumeCurves->getVolumeIndex((audio_stream_type_t)stream, device),
                          outputDesc,
                          device,
                          delayMs,
                          force);
    }
}

void AudioPolicyManager::setStrategyMute(routing_strategy strategy,
                                             bool on,
                                             const sp<AudioOutputDescriptor>& outputDesc,
                                             int delayMs,
                                             audio_devices_t device)
{
    ALOGVV("setStrategyMute() strategy %d, mute %d, output ID %d",
           strategy, on, outputDesc->getId());
    for (int stream = 0; stream < AUDIO_STREAM_FOR_POLICY_CNT; stream++) {
        if (getStrategy((audio_stream_type_t)stream) == strategy) {
            setStreamMute((audio_stream_type_t)stream, on, outputDesc, delayMs, device);
        }
    }
}

void AudioPolicyManager::setStreamMute(audio_stream_type_t stream,
                                           bool on,
                                           const sp<AudioOutputDescriptor>& outputDesc,
                                           int delayMs,
                                           audio_devices_t device)
{
    if (device == AUDIO_DEVICE_NONE) {
        device = outputDesc->device();
    }

    ALOGVV("setStreamMute() stream %d, mute %d, mMuteCount %d device %04x",
          stream, on, outputDesc->mMuteCount[stream], device);

    if (on) {
        if (outputDesc->mMuteCount[stream] == 0) {
            if (mVolumeCurves->canBeMuted(stream) &&
                    ((stream != AUDIO_STREAM_ENFORCED_AUDIBLE) ||
                     (mEngine->getForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM) == AUDIO_POLICY_FORCE_NONE))) {
                checkAndSetVolume(stream, 0, outputDesc, device, delayMs);
            }
        }
        // increment mMuteCount after calling checkAndSetVolume() so that volume change is not ignored
        outputDesc->mMuteCount[stream]++;
    } else {
        if (outputDesc->mMuteCount[stream] == 0) {
            ALOGV("setStreamMute() unmuting non muted stream!");
            return;
        }
        if (--outputDesc->mMuteCount[stream] == 0) {
            checkAndSetVolume(stream,
                              mVolumeCurves->getVolumeIndex(stream, device),
                              outputDesc,
                              device,
                              delayMs);
        }
    }
}

void AudioPolicyManager::handleIncallSonification(audio_stream_type_t stream,
                                                      bool starting, bool stateChange)
{
    if(!hasPrimaryOutput()) {
        return;
    }

    // if the stream pertains to sonification strategy and we are in call we must
    // mute the stream if it is low visibility. If it is high visibility, we must play a tone
    // in the device used for phone strategy and play the tone if the selected device does not
    // interfere with the device used for phone strategy
    // if stateChange is true, we are called from setPhoneState() and we must mute or unmute as
    // many times as there are active tracks on the output
    const routing_strategy stream_strategy = getStrategy(stream);
    if ((stream_strategy == STRATEGY_SONIFICATION) ||
            ((stream_strategy == STRATEGY_SONIFICATION_RESPECTFUL))) {
        sp<SwAudioOutputDescriptor> outputDesc = mPrimaryOutput;
        ALOGV("handleIncallSonification() stream %d starting %d device %x stateChange %d",
                stream, starting, outputDesc->mDevice, stateChange);
        if (outputDesc->mRefCount[stream]) {
            int muteCount = 1;
            if (stateChange) {
                muteCount = outputDesc->mRefCount[stream];
            }
            if (audio_is_low_visibility(stream)) {
                ALOGV("handleIncallSonification() low visibility, muteCount %d", muteCount);
                for (int i = 0; i < muteCount; i++) {
                    setStreamMute(stream, starting, mPrimaryOutput);
                }
            } else {
                ALOGV("handleIncallSonification() high visibility");
                if (outputDesc->device() &
                        getDeviceForStrategy(STRATEGY_PHONE, true /*fromCache*/)) {
                    ALOGV("handleIncallSonification() high visibility muted, muteCount %d", muteCount);
                    for (int i = 0; i < muteCount; i++) {
                        setStreamMute(stream, starting, mPrimaryOutput);
                    }
                }
                if (starting) {
                    mpClientInterface->startTone(AUDIO_POLICY_TONE_IN_CALL_NOTIFICATION,
                                                 AUDIO_STREAM_VOICE_CALL);
                } else {
                    mpClientInterface->stopTone();
                }
            }
        }
    }
}

audio_stream_type_t AudioPolicyManager::streamTypefromAttributesInt(const audio_attributes_t *attr)
{
    // flags to stream type mapping
    if ((attr->flags & AUDIO_FLAG_AUDIBILITY_ENFORCED) == AUDIO_FLAG_AUDIBILITY_ENFORCED) {
        return AUDIO_STREAM_ENFORCED_AUDIBLE;
    }
    if ((attr->flags & AUDIO_FLAG_SCO) == AUDIO_FLAG_SCO) {
        return AUDIO_STREAM_BLUETOOTH_SCO;
    }
    if ((attr->flags & AUDIO_FLAG_BEACON) == AUDIO_FLAG_BEACON) {
        return AUDIO_STREAM_TTS;
    }

    // usage to stream type mapping
    switch (attr->usage) {
    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
        return AUDIO_STREAM_MUSIC;
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
        return AUDIO_STREAM_ACCESSIBILITY;
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
        return AUDIO_STREAM_SYSTEM;
    case AUDIO_USAGE_VOICE_COMMUNICATION:
        return AUDIO_STREAM_VOICE_CALL;

    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
        return AUDIO_STREAM_DTMF;

    case AUDIO_USAGE_ALARM:
        return AUDIO_STREAM_ALARM;
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
        return AUDIO_STREAM_RING;

    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
        return AUDIO_STREAM_NOTIFICATION;

    case AUDIO_USAGE_UNKNOWN:
    default:
        return AUDIO_STREAM_MUSIC;
    }
}

bool AudioPolicyManager::isValidAttributes(const audio_attributes_t *paa)
{
    // has flags that map to a strategy?
    if ((paa->flags & (AUDIO_FLAG_AUDIBILITY_ENFORCED | AUDIO_FLAG_SCO | AUDIO_FLAG_BEACON)) != 0) {
        return true;
    }

    // has known usage?
    switch (paa->usage) {
    case AUDIO_USAGE_UNKNOWN:
    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_VOICE_COMMUNICATION:
    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
    case AUDIO_USAGE_ALARM:
    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_VIRTUAL_SOURCE:
        break;
    default:
        return false;
    }
    return true;
}

bool AudioPolicyManager::isStrategyActive(const sp<AudioOutputDescriptor> outputDesc,
                                          routing_strategy strategy, uint32_t inPastMs,
                                          nsecs_t sysTime) const
{
    if ((sysTime == 0) && (inPastMs != 0)) {
        sysTime = systemTime();
    }
    for (int i = 0; i < (int)AUDIO_STREAM_FOR_POLICY_CNT; i++) {
        if (((getStrategy((audio_stream_type_t)i) == strategy) ||
                (NUM_STRATEGIES == strategy)) &&
                outputDesc->isStreamActive((audio_stream_type_t)i, inPastMs, sysTime)) {
            return true;
        }
    }
    return false;
}

audio_policy_forced_cfg_t AudioPolicyManager::getForceUse(audio_policy_force_use_t usage)
{
    return mEngine->getForceUse(usage);
}

bool AudioPolicyManager::isInCall()
{
    return isStateInCall(mEngine->getPhoneState());
}

bool AudioPolicyManager::isStateInCall(int state)
{
    return is_state_in_call(state);
}

void AudioPolicyManager::cleanUpForDevice(const sp<DeviceDescriptor>& deviceDesc)
{
    for (ssize_t i = (ssize_t)mAudioSources.size() - 1; i >= 0; i--)  {
        sp<AudioSourceDescriptor> sourceDesc = mAudioSources.valueAt(i);
        if (sourceDesc->mDevice->equals(deviceDesc)) {
            ALOGV("%s releasing audio source %d", __FUNCTION__, sourceDesc->getHandle());
            stopAudioSource(sourceDesc->getHandle());
        }
    }

    for (ssize_t i = (ssize_t)mAudioPatches.size() - 1; i >= 0; i--)  {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(i);
        bool release = false;
        for (size_t j = 0; j < patchDesc->mPatch.num_sources && !release; j++)  {
            const struct audio_port_config *source = &patchDesc->mPatch.sources[j];
            if (source->type == AUDIO_PORT_TYPE_DEVICE &&
                    source->ext.device.type == deviceDesc->type()) {
                release = true;
            }
        }
        for (size_t j = 0; j < patchDesc->mPatch.num_sinks && !release; j++)  {
            const struct audio_port_config *sink = &patchDesc->mPatch.sinks[j];
            if (sink->type == AUDIO_PORT_TYPE_DEVICE &&
                    sink->ext.device.type == deviceDesc->type()) {
                release = true;
            }
        }
        if (release) {
            ALOGV("%s releasing patch %u", __FUNCTION__, patchDesc->mHandle);
            releaseAudioPatch(patchDesc->mHandle, patchDesc->mUid);
        }
    }
}

// Modify the list of surround sound formats supported.
void AudioPolicyManager::filterSurroundFormats(FormatVector *formatsPtr) {
    FormatVector &formats = *formatsPtr;
    // TODO Set this based on Config properties.
    const bool alwaysForceAC3 = true;

    audio_policy_forced_cfg_t forceUse = mEngine->getForceUse(
            AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND);
    ALOGD("%s: forced use = %d", __FUNCTION__, forceUse);

    // Analyze original support for various formats.
    bool supportsAC3 = false;
    bool supportsOtherSurround = false;
    bool supportsIEC61937 = false;
    for (size_t formatIndex = 0; formatIndex < formats.size(); formatIndex++) {
        audio_format_t format = formats[formatIndex];
        switch (format) {
            case AUDIO_FORMAT_AC3:
                supportsAC3 = true;
                break;
            case AUDIO_FORMAT_E_AC3:
            case AUDIO_FORMAT_DTS:
            case AUDIO_FORMAT_DTS_HD:
                supportsOtherSurround = true;
                break;
            case AUDIO_FORMAT_IEC61937:
                supportsIEC61937 = true;
                break;
            default:
                break;
        }
    }

    // Modify formats based on surround preferences.
    // If NEVER, remove support for surround formats.
    if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER) {
        if (supportsAC3 || supportsOtherSurround || supportsIEC61937) {
            // Remove surround sound related formats.
            for (size_t formatIndex = 0; formatIndex < formats.size(); ) {
                audio_format_t format = formats[formatIndex];
                switch(format) {
                    case AUDIO_FORMAT_AC3:
                    case AUDIO_FORMAT_E_AC3:
                    case AUDIO_FORMAT_DTS:
                    case AUDIO_FORMAT_DTS_HD:
                    case AUDIO_FORMAT_IEC61937:
                        formats.removeAt(formatIndex);
                        break;
                    default:
                        formatIndex++; // keep it
                        break;
                }
            }
            supportsAC3 = false;
            supportsOtherSurround = false;
            supportsIEC61937 = false;
        }
    } else { // AUTO or ALWAYS
        // Most TVs support AC3 even if they do not report it in the EDID.
        if ((alwaysForceAC3 || (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS))
                && !supportsAC3) {
            formats.add(AUDIO_FORMAT_AC3);
            supportsAC3 = true;
        }

        // If ALWAYS, add support for raw surround formats if all are missing.
        // This assumes that if any of these formats are reported by the HAL
        // then the report is valid and should not be modified.
        if ((forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS)
                && !supportsOtherSurround) {
            formats.add(AUDIO_FORMAT_E_AC3);
            formats.add(AUDIO_FORMAT_DTS);
            formats.add(AUDIO_FORMAT_DTS_HD);
            supportsOtherSurround = true;
        }

        // Add support for IEC61937 if any raw surround supported.
        // The HAL could do this but add it here, just in case.
        if ((supportsAC3 || supportsOtherSurround) && !supportsIEC61937) {
            formats.add(AUDIO_FORMAT_IEC61937);
            supportsIEC61937 = true;
        }
    }
}

// Modify the list of channel masks supported.
void AudioPolicyManager::filterSurroundChannelMasks(ChannelsVector *channelMasksPtr) {
    ChannelsVector &channelMasks = *channelMasksPtr;
    audio_policy_forced_cfg_t forceUse = mEngine->getForceUse(
            AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND);

    // If NEVER, then remove support for channelMasks > stereo.
    if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER) {
        for (size_t maskIndex = 0; maskIndex < channelMasks.size(); ) {
            audio_channel_mask_t channelMask = channelMasks[maskIndex];
            if (channelMask & ~AUDIO_CHANNEL_OUT_STEREO) {
                ALOGI("%s: force NEVER, so remove channelMask 0x%08x", __FUNCTION__, channelMask);
                channelMasks.removeAt(maskIndex);
            } else {
                maskIndex++;
            }
        }
    // If ALWAYS, then make sure we at least support 5.1
    } else if (forceUse == AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS) {
        bool supports5dot1 = false;
        // Are there any channel masks that can be considered "surround"?
        for (size_t maskIndex = 0; maskIndex < channelMasks.size(); maskIndex++) {
            audio_channel_mask_t channelMask = channelMasks[maskIndex];
            if ((channelMask & AUDIO_CHANNEL_OUT_5POINT1) == AUDIO_CHANNEL_OUT_5POINT1) {
                supports5dot1 = true;
                break;
            }
        }
        // If not then add 5.1 support.
        if (!supports5dot1) {
            channelMasks.add(AUDIO_CHANNEL_OUT_5POINT1);
            ALOGI("%s: force ALWAYS, so adding channelMask for 5.1 surround", __FUNCTION__);
        }
    }
}

void AudioPolicyManager::updateAudioProfiles(audio_devices_t device,
                                             audio_io_handle_t ioHandle,
                                             AudioProfileVector &profiles)
{
    String8 reply;

    // Format MUST be checked first to update the list of AudioProfile
    if (profiles.hasDynamicFormat()) {
        reply = mpClientInterface->getParameters(ioHandle,
                                                 String8(AUDIO_PARAMETER_STREAM_SUP_FORMATS));
        ALOGV("%s: supported formats %s", __FUNCTION__, reply.string());
        AudioParameter repliedParameters(reply);
        if (repliedParameters.get(
                String8(AUDIO_PARAMETER_STREAM_SUP_FORMATS), reply) != NO_ERROR) {
            ALOGE("%s: failed to retrieve format, bailing out", __FUNCTION__);
            return;
        }
        FormatVector formats = formatsFromString(reply.string());
        if (device == AUDIO_DEVICE_OUT_HDMI) {
            filterSurroundFormats(&formats);
        }
        profiles.setFormats(formats);
    }
    const FormatVector &supportedFormats = profiles.getSupportedFormats();

    for (size_t formatIndex = 0; formatIndex < supportedFormats.size(); formatIndex++) {
        audio_format_t format = supportedFormats[formatIndex];
        ChannelsVector channelMasks;
        SampleRateVector samplingRates;
        AudioParameter requestedParameters;
        requestedParameters.addInt(String8(AUDIO_PARAMETER_STREAM_FORMAT), format);

        if (profiles.hasDynamicRateFor(format)) {
            reply = mpClientInterface->getParameters(ioHandle,
                                                     requestedParameters.toString() + ";" +
                                                     AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES);
            ALOGV("%s: supported sampling rates %s", __FUNCTION__, reply.string());
            AudioParameter repliedParameters(reply);
            if (repliedParameters.get(
                    String8(AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES), reply) == NO_ERROR) {
                samplingRates = samplingRatesFromString(reply.string());
            }
        }
        if (profiles.hasDynamicChannelsFor(format)) {
            reply = mpClientInterface->getParameters(ioHandle,
                                                     requestedParameters.toString() + ";" +
                                                     AUDIO_PARAMETER_STREAM_SUP_CHANNELS);
            ALOGV("%s: supported channel masks %s", __FUNCTION__, reply.string());
            AudioParameter repliedParameters(reply);
            if (repliedParameters.get(
                    String8(AUDIO_PARAMETER_STREAM_SUP_CHANNELS), reply) == NO_ERROR) {
                channelMasks = channelMasksFromString(reply.string());
                if (device == AUDIO_DEVICE_OUT_HDMI) {
                    filterSurroundChannelMasks(&channelMasks);
                }
            }
        }
        profiles.addProfileFromHal(new AudioProfile(format, channelMasks, samplingRates));
    }
}

}; // namespace android
