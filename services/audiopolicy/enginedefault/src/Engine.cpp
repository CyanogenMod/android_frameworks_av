/*
 * Copyright (C) 2015 The Android Open Source Project
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

#define LOG_TAG "APM::AudioPolicyEngine"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include "Engine.h"
#include <AudioPolicyManagerObserver.h>
#include <AudioPort.h>
#include <IOProfile.h>
#include <policy.h>
#include <utils/String8.h>
#include <utils/Log.h>

namespace android
{
namespace audio_policy
{

Engine::Engine()
    : mManagerInterface(this),
      mPhoneState(AUDIO_MODE_NORMAL),
      mApmObserver(NULL)
{
    for (int i = 0; i < AUDIO_POLICY_FORCE_USE_CNT; i++) {
        mForceUse[i] = AUDIO_POLICY_FORCE_NONE;
    }
}

Engine::~Engine()
{
}

void Engine::setObserver(AudioPolicyManagerObserver *observer)
{
    ALOG_ASSERT(observer != NULL, "Invalid Audio Policy Manager observer");
    mApmObserver = observer;
}

status_t Engine::initCheck()
{
    return (mApmObserver != NULL) ?  NO_ERROR : NO_INIT;
}

status_t Engine::setPhoneState(audio_mode_t state)
{
    ALOGV("setPhoneState() state %d", state);

    if (state < 0 || state >= AUDIO_MODE_CNT) {
        ALOGW("setPhoneState() invalid state %d", state);
        return BAD_VALUE;
    }

    if (state == mPhoneState ) {
        ALOGW("setPhoneState() setting same state %d", state);
        return BAD_VALUE;
    }

    // store previous phone state for management of sonification strategy below
    int oldState = mPhoneState;
    mPhoneState = state;

    if (!is_state_in_call(oldState) && is_state_in_call(state)) {
        ALOGV("  Entering call in setPhoneState()");
        mApmObserver->getVolumeCurves().switchVolumeCurve(AUDIO_STREAM_VOICE_CALL,
                                                          AUDIO_STREAM_DTMF);
    } else if (is_state_in_call(oldState) && !is_state_in_call(state)) {
        ALOGV("  Exiting call in setPhoneState()");
        mApmObserver->getVolumeCurves().restoreOriginVolumeCurve(AUDIO_STREAM_DTMF);
    }
    return NO_ERROR;
}

status_t Engine::setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config)
{
    switch(usage) {
    case AUDIO_POLICY_FORCE_FOR_COMMUNICATION:
        if (config != AUDIO_POLICY_FORCE_SPEAKER && config != AUDIO_POLICY_FORCE_BT_SCO &&
            config != AUDIO_POLICY_FORCE_NONE) {
            ALOGW("setForceUse() invalid config %d for FOR_COMMUNICATION", config);
            return BAD_VALUE;
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_MEDIA:
        if (config != AUDIO_POLICY_FORCE_HEADPHONES && config != AUDIO_POLICY_FORCE_BT_A2DP &&
            config != AUDIO_POLICY_FORCE_WIRED_ACCESSORY &&
            config != AUDIO_POLICY_FORCE_ANALOG_DOCK &&
            config != AUDIO_POLICY_FORCE_DIGITAL_DOCK && config != AUDIO_POLICY_FORCE_NONE &&
            config != AUDIO_POLICY_FORCE_NO_BT_A2DP && config != AUDIO_POLICY_FORCE_SPEAKER ) {
            ALOGW("setForceUse() invalid config %d for FOR_MEDIA", config);
            return BAD_VALUE;
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_RECORD:
        if (config != AUDIO_POLICY_FORCE_BT_SCO && config != AUDIO_POLICY_FORCE_WIRED_ACCESSORY &&
            config != AUDIO_POLICY_FORCE_NONE) {
            ALOGW("setForceUse() invalid config %d for FOR_RECORD", config);
            return BAD_VALUE;
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_DOCK:
        if (config != AUDIO_POLICY_FORCE_NONE && config != AUDIO_POLICY_FORCE_BT_CAR_DOCK &&
            config != AUDIO_POLICY_FORCE_BT_DESK_DOCK &&
            config != AUDIO_POLICY_FORCE_WIRED_ACCESSORY &&
            config != AUDIO_POLICY_FORCE_ANALOG_DOCK &&
            config != AUDIO_POLICY_FORCE_DIGITAL_DOCK) {
            ALOGW("setForceUse() invalid config %d for FOR_DOCK", config);
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_SYSTEM:
        if (config != AUDIO_POLICY_FORCE_NONE &&
            config != AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
            ALOGW("setForceUse() invalid config %d for FOR_SYSTEM", config);
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_HDMI_SYSTEM_AUDIO:
        if (config != AUDIO_POLICY_FORCE_NONE &&
            config != AUDIO_POLICY_FORCE_HDMI_SYSTEM_AUDIO_ENFORCED) {
            ALOGW("setForceUse() invalid config %d for HDMI_SYSTEM_AUDIO", config);
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND:
        if (config != AUDIO_POLICY_FORCE_NONE &&
                config != AUDIO_POLICY_FORCE_ENCODED_SURROUND_NEVER &&
                config != AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS) {
            ALOGW("setForceUse() invalid config %d for ENCODED_SURROUND", config);
            return BAD_VALUE;
        }
        mForceUse[usage] = config;
        break;
    default:
        ALOGW("setForceUse() invalid usage %d", usage);
        break; // TODO return BAD_VALUE?
    }
    return NO_ERROR;
}

routing_strategy Engine::getStrategyForStream(audio_stream_type_t stream)
{
    // stream to strategy mapping
    switch (stream) {
    case AUDIO_STREAM_VOICE_CALL:
    case AUDIO_STREAM_BLUETOOTH_SCO:
        return STRATEGY_PHONE;
    case AUDIO_STREAM_RING:
    case AUDIO_STREAM_ALARM:
        return STRATEGY_SONIFICATION;
    case AUDIO_STREAM_NOTIFICATION:
        return STRATEGY_SONIFICATION_RESPECTFUL;
    case AUDIO_STREAM_DTMF:
        return STRATEGY_DTMF;
    default:
        ALOGE("unknown stream type %d", stream);
    case AUDIO_STREAM_SYSTEM:
        // NOTE: SYSTEM stream uses MEDIA strategy because muting music and switching outputs
        // while key clicks are played produces a poor result
    case AUDIO_STREAM_MUSIC:
        return STRATEGY_MEDIA;
    case AUDIO_STREAM_ENFORCED_AUDIBLE:
        return STRATEGY_ENFORCED_AUDIBLE;
    case AUDIO_STREAM_TTS:
        return STRATEGY_TRANSMITTED_THROUGH_SPEAKER;
    case AUDIO_STREAM_ACCESSIBILITY:
        return STRATEGY_ACCESSIBILITY;
    case AUDIO_STREAM_REROUTING:
        return STRATEGY_REROUTING;
    }
}

routing_strategy Engine::getStrategyForUsage(audio_usage_t usage)
{
    // usage to strategy mapping
    switch (usage) {
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
        return STRATEGY_ACCESSIBILITY;

    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
        return STRATEGY_MEDIA;

    case AUDIO_USAGE_VOICE_COMMUNICATION:
        return STRATEGY_PHONE;

    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
        return STRATEGY_DTMF;

    case AUDIO_USAGE_ALARM:
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
        return STRATEGY_SONIFICATION;

    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
        return STRATEGY_SONIFICATION_RESPECTFUL;

    case AUDIO_USAGE_UNKNOWN:
    default:
        return STRATEGY_MEDIA;
    }
}

audio_devices_t Engine::getDeviceForStrategy(routing_strategy strategy) const
{
    DeviceVector availableOutputDevices = mApmObserver->getAvailableOutputDevices();
    DeviceVector availableInputDevices = mApmObserver->getAvailableInputDevices();

    const SwAudioOutputCollection &outputs = mApmObserver->getOutputs();

    return getDeviceForStrategyInt(strategy, availableOutputDevices,
                                   availableInputDevices, outputs);
}



audio_devices_t Engine::getDeviceForStrategyInt(routing_strategy strategy,
                                                DeviceVector availableOutputDevices,
                                                DeviceVector availableInputDevices,
                                                const SwAudioOutputCollection &outputs) const
{
    uint32_t device = AUDIO_DEVICE_NONE;
    uint32_t availableOutputDevicesType = availableOutputDevices.types();

    switch (strategy) {

    case STRATEGY_TRANSMITTED_THROUGH_SPEAKER:
        device = availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER;
        break;

    case STRATEGY_SONIFICATION_RESPECTFUL:
        if (isInCall()) {
            device = getDeviceForStrategyInt(
                    STRATEGY_SONIFICATION, availableOutputDevices, availableInputDevices, outputs);
        } else if (outputs.isStreamActiveRemotely(AUDIO_STREAM_MUSIC,
                SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY)) {
            // while media is playing on a remote device, use the the sonification behavior.
            // Note that we test this usecase before testing if media is playing because
            //   the isStreamActive() method only informs about the activity of a stream, not
            //   if it's for local playback. Note also that we use the same delay between both tests
            device = getDeviceForStrategyInt(
                    STRATEGY_SONIFICATION, availableOutputDevices, availableInputDevices, outputs);
            //user "safe" speaker if available instead of normal speaker to avoid triggering
            //other acoustic safety mechanisms for notification
            if ((device & AUDIO_DEVICE_OUT_SPEAKER) &&
                    (availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER_SAFE)) {
                device |= AUDIO_DEVICE_OUT_SPEAKER_SAFE;
                device &= ~AUDIO_DEVICE_OUT_SPEAKER;
            }
        } else if (outputs.isStreamActive(
                                AUDIO_STREAM_MUSIC, SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY)) {
            // while media is playing (or has recently played), use the same device
            device = getDeviceForStrategyInt(
                    STRATEGY_MEDIA, availableOutputDevices, availableInputDevices, outputs);
        } else {
            // when media is not playing anymore, fall back on the sonification behavior
            device = getDeviceForStrategyInt(
                    STRATEGY_SONIFICATION, availableOutputDevices, availableInputDevices, outputs);
            //user "safe" speaker if available instead of normal speaker to avoid triggering
            //other acoustic safety mechanisms for notification
            if ((device & AUDIO_DEVICE_OUT_SPEAKER) &&
                    (availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER_SAFE)) {
                device |= AUDIO_DEVICE_OUT_SPEAKER_SAFE;
                device &= ~AUDIO_DEVICE_OUT_SPEAKER;
            }
        }
        break;

    case STRATEGY_DTMF:
        if (!isInCall()) {
            // when off call, DTMF strategy follows the same rules as MEDIA strategy
            device = getDeviceForStrategyInt(
                    STRATEGY_MEDIA, availableOutputDevices, availableInputDevices, outputs);
            break;
        }
        // when in call, DTMF and PHONE strategies follow the same rules
        // FALL THROUGH

    case STRATEGY_PHONE:
        // Force use of only devices on primary output if:
        // - in call AND
        //   - cannot route from voice call RX OR
        //   - audio HAL version is < 3.0 and TX device is on the primary HW module
        if (getPhoneState() == AUDIO_MODE_IN_CALL) {
            audio_devices_t txDevice = getDeviceForInputSource(AUDIO_SOURCE_VOICE_COMMUNICATION);
            sp<AudioOutputDescriptor> primaryOutput = outputs.getPrimaryOutput();
            audio_devices_t availPrimaryInputDevices =
                 availableInputDevices.getDevicesFromHwModule(primaryOutput->getModuleHandle());
            audio_devices_t availPrimaryOutputDevices =
                    primaryOutput->supportedDevices() & availableOutputDevices.types();

            if (((availableInputDevices.types() &
                    AUDIO_DEVICE_IN_TELEPHONY_RX & ~AUDIO_DEVICE_BIT_IN) == 0) ||
                    (((txDevice & availPrimaryInputDevices & ~AUDIO_DEVICE_BIT_IN) != 0) &&
                         (primaryOutput->getAudioPort()->getModuleVersion() <
                             AUDIO_DEVICE_API_VERSION_3_0))) {
                availableOutputDevicesType = availPrimaryOutputDevices;
            }
        }
        // for phone strategy, we first consider the forced use and then the available devices by
        // order of priority
        switch (mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION]) {
        case AUDIO_POLICY_FORCE_BT_SCO:
            if (!isInCall() || strategy != STRATEGY_DTMF) {
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT;
                if (device) break;
            }
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET;
            if (device) break;
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_SCO;
            if (device) break;
            // if SCO device is requested but no SCO device is available, fall back to default case
            // FALL THROUGH

        default:    // FORCE_NONE
            // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to A2DP
            if (!isInCall() &&
                    (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] != AUDIO_POLICY_FORCE_NO_BT_A2DP) &&
                    (outputs.getA2dpOutput() != 0)) {
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
                if (device) break;
            }
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
            if (device) break;
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_WIRED_HEADSET;
            if (device) break;
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_LINE;
            if (device) break;
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_USB_DEVICE;
            if (device) break;
            if (!isInCall()) {
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_USB_ACCESSORY;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_AUX_DIGITAL;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
                if (device) break;
            }
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_EARPIECE;
            break;

        case AUDIO_POLICY_FORCE_SPEAKER:
            // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to
            // A2DP speaker when forcing to speaker output
            if (!isInCall() &&
                    (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] != AUDIO_POLICY_FORCE_NO_BT_A2DP) &&
                    (outputs.getA2dpOutput() != 0)) {
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
                if (device) break;
            }
            if (!isInCall()) {
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_USB_ACCESSORY;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_USB_DEVICE;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_AUX_DIGITAL;
                if (device) break;
                device = availableOutputDevicesType & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
                if (device) break;
            }
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER;
            break;
        }
    break;

    case STRATEGY_SONIFICATION:

        // If incall, just select the STRATEGY_PHONE device: The rest of the behavior is handled by
        // handleIncallSonification().
        if (isInCall()) {
            device = getDeviceForStrategyInt(
                    STRATEGY_PHONE, availableOutputDevices, availableInputDevices, outputs);
            break;
        }
        // FALL THROUGH

    case STRATEGY_ENFORCED_AUDIBLE:
        // strategy STRATEGY_ENFORCED_AUDIBLE uses same routing policy as STRATEGY_SONIFICATION
        // except:
        //   - when in call where it doesn't default to STRATEGY_PHONE behavior
        //   - in countries where not enforced in which case it follows STRATEGY_MEDIA

        if ((strategy == STRATEGY_SONIFICATION) ||
                (mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM] == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED)) {
            device = availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER;
        }
        // The second device used for sonification is the same as the device used by media strategy
        // FALL THROUGH

    case STRATEGY_ACCESSIBILITY:
        if (strategy == STRATEGY_ACCESSIBILITY) {
            // do not route accessibility prompts to a digital output currently configured with a
            // compressed format as they would likely not be mixed and dropped.
            for (size_t i = 0; i < outputs.size(); i++) {
                sp<AudioOutputDescriptor> desc = outputs.valueAt(i);
                audio_devices_t devices = desc->device() &
                    (AUDIO_DEVICE_OUT_HDMI | AUDIO_DEVICE_OUT_SPDIF | AUDIO_DEVICE_OUT_HDMI_ARC);
                if (desc->isActive() && !audio_is_linear_pcm(desc->mFormat) &&
                        devices != AUDIO_DEVICE_NONE) {
                    availableOutputDevicesType = availableOutputDevices.types() & ~devices;
                }
            }
            availableOutputDevices =
                    availableOutputDevices.getDevicesFromType(availableOutputDevicesType);
            if (outputs.isStreamActive(AUDIO_STREAM_RING) ||
                    outputs.isStreamActive(AUDIO_STREAM_ALARM)) {
                return getDeviceForStrategyInt(
                    STRATEGY_SONIFICATION, availableOutputDevices, availableInputDevices, outputs);
            }
            if (isInCall()) {
                return getDeviceForStrategyInt(
                        STRATEGY_PHONE, availableOutputDevices, availableInputDevices, outputs);
            }
        }
        // For other cases, STRATEGY_ACCESSIBILITY behaves like STRATEGY_MEDIA
        // FALL THROUGH

    // FIXME: STRATEGY_REROUTING follow STRATEGY_MEDIA for now
    case STRATEGY_REROUTING:
    case STRATEGY_MEDIA: {
        uint32_t device2 = AUDIO_DEVICE_NONE;
        if (strategy != STRATEGY_SONIFICATION) {
            // no sonification on remote submix (e.g. WFD)
            if (availableOutputDevices.getDevice(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                                                 String8("0")) != 0) {
                device2 = availableOutputDevices.types() & AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
            }
        }
        if (isInCall() && (strategy == STRATEGY_MEDIA)) {
            device = getDeviceForStrategyInt(
                    STRATEGY_PHONE, availableOutputDevices, availableInputDevices, outputs);
            break;
        }
        if ((device2 == AUDIO_DEVICE_NONE) &&
                (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] != AUDIO_POLICY_FORCE_NO_BT_A2DP) &&
                (outputs.getA2dpOutput() != 0)) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
            if (device2 == AUDIO_DEVICE_NONE) {
                device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
            }
            if (device2 == AUDIO_DEVICE_NONE) {
                device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
            }
        }
        if ((device2 == AUDIO_DEVICE_NONE) &&
            (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] == AUDIO_POLICY_FORCE_SPEAKER)) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_LINE;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_WIRED_HEADSET;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_USB_ACCESSORY;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_USB_DEVICE;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
        }
        if ((device2 == AUDIO_DEVICE_NONE) && (strategy != STRATEGY_SONIFICATION)) {
            // no sonification on aux digital (e.g. HDMI)
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_AUX_DIGITAL;
        }
        if ((device2 == AUDIO_DEVICE_NONE) &&
                (mForceUse[AUDIO_POLICY_FORCE_FOR_DOCK] == AUDIO_POLICY_FORCE_ANALOG_DOCK)) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDevicesType & AUDIO_DEVICE_OUT_SPEAKER;
        }
        int device3 = AUDIO_DEVICE_NONE;
        if (strategy == STRATEGY_MEDIA) {
            // ARC, SPDIF and AUX_LINE can co-exist with others.
            device3 = availableOutputDevicesType & AUDIO_DEVICE_OUT_HDMI_ARC;
            device3 |= (availableOutputDevicesType & AUDIO_DEVICE_OUT_SPDIF);
            device3 |= (availableOutputDevicesType & AUDIO_DEVICE_OUT_AUX_LINE);
        }

        device2 |= device3;
        // device is DEVICE_OUT_SPEAKER if we come from case STRATEGY_SONIFICATION or
        // STRATEGY_ENFORCED_AUDIBLE, AUDIO_DEVICE_NONE otherwise
        device |= device2;

        // If hdmi system audio mode is on, remove speaker out of output list.
        if ((strategy == STRATEGY_MEDIA) &&
            (mForceUse[AUDIO_POLICY_FORCE_FOR_HDMI_SYSTEM_AUDIO] ==
                AUDIO_POLICY_FORCE_HDMI_SYSTEM_AUDIO_ENFORCED)) {
            device &= ~AUDIO_DEVICE_OUT_SPEAKER;
        }
        } break;

    default:
        ALOGW("getDeviceForStrategy() unknown strategy: %d", strategy);
        break;
    }

    if (device == AUDIO_DEVICE_NONE) {
        ALOGV("getDeviceForStrategy() no device found for strategy %d", strategy);
        device = mApmObserver->getDefaultOutputDevice()->type();
        ALOGE_IF(device == AUDIO_DEVICE_NONE,
                 "getDeviceForStrategy() no default device defined");
    }
    ALOGVV("getDeviceForStrategy() strategy %d, device %x", strategy, device);
    return device;
}


audio_devices_t Engine::getDeviceForInputSource(audio_source_t inputSource) const
{
    const DeviceVector &availableOutputDevices = mApmObserver->getAvailableOutputDevices();
    const DeviceVector &availableInputDevices = mApmObserver->getAvailableInputDevices();
    const SwAudioOutputCollection &outputs = mApmObserver->getOutputs();
    audio_devices_t availableDeviceTypes = availableInputDevices.types() & ~AUDIO_DEVICE_BIT_IN;

    uint32_t device = AUDIO_DEVICE_NONE;

    switch (inputSource) {
    case AUDIO_SOURCE_VOICE_UPLINK:
      if (availableDeviceTypes & AUDIO_DEVICE_IN_VOICE_CALL) {
          device = AUDIO_DEVICE_IN_VOICE_CALL;
          break;
      }
      break;

    case AUDIO_SOURCE_DEFAULT:
    case AUDIO_SOURCE_MIC:
    if (availableDeviceTypes & AUDIO_DEVICE_IN_BLUETOOTH_A2DP) {
        device = AUDIO_DEVICE_IN_BLUETOOTH_A2DP;
    } else if ((mForceUse[AUDIO_POLICY_FORCE_FOR_RECORD] == AUDIO_POLICY_FORCE_BT_SCO) &&
        (availableDeviceTypes & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
        device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    } else if (availableDeviceTypes & AUDIO_DEVICE_IN_WIRED_HEADSET) {
        device = AUDIO_DEVICE_IN_WIRED_HEADSET;
    } else if (availableDeviceTypes & AUDIO_DEVICE_IN_USB_DEVICE) {
        device = AUDIO_DEVICE_IN_USB_DEVICE;
    } else if (availableDeviceTypes & AUDIO_DEVICE_IN_BUILTIN_MIC) {
        device = AUDIO_DEVICE_IN_BUILTIN_MIC;
    }
    break;

    case AUDIO_SOURCE_VOICE_COMMUNICATION:
        // Allow only use of devices on primary input if in call and HAL does not support routing
        // to voice call path.
        if ((getPhoneState() == AUDIO_MODE_IN_CALL) &&
                (availableOutputDevices.types() & AUDIO_DEVICE_OUT_TELEPHONY_TX) == 0) {
            sp<AudioOutputDescriptor> primaryOutput = outputs.getPrimaryOutput();
            availableDeviceTypes =
                    availableInputDevices.getDevicesFromHwModule(primaryOutput->getModuleHandle())
                    & ~AUDIO_DEVICE_BIT_IN;
        }

        switch (mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION]) {
        case AUDIO_POLICY_FORCE_BT_SCO:
            // if SCO device is requested but no SCO device is available, fall back to default case
            if (availableDeviceTypes & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
                device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
                break;
            }
            // FALL THROUGH

        default:    // FORCE_NONE
            if (availableDeviceTypes & AUDIO_DEVICE_IN_WIRED_HEADSET) {
                device = AUDIO_DEVICE_IN_WIRED_HEADSET;
            } else if (availableDeviceTypes & AUDIO_DEVICE_IN_USB_DEVICE) {
                device = AUDIO_DEVICE_IN_USB_DEVICE;
            } else if (availableDeviceTypes & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                device = AUDIO_DEVICE_IN_BUILTIN_MIC;
            }
            break;

        case AUDIO_POLICY_FORCE_SPEAKER:
            if (availableDeviceTypes & AUDIO_DEVICE_IN_BACK_MIC) {
                device = AUDIO_DEVICE_IN_BACK_MIC;
            } else if (availableDeviceTypes & AUDIO_DEVICE_IN_BUILTIN_MIC) {
                device = AUDIO_DEVICE_IN_BUILTIN_MIC;
            }
            break;
        }
        break;

    case AUDIO_SOURCE_VOICE_RECOGNITION:
    case AUDIO_SOURCE_UNPROCESSED:
    case AUDIO_SOURCE_HOTWORD:
        if (mForceUse[AUDIO_POLICY_FORCE_FOR_RECORD] == AUDIO_POLICY_FORCE_BT_SCO &&
                availableDeviceTypes & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            device = AUDIO_DEVICE_IN_WIRED_HEADSET;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_USB_DEVICE) {
            device = AUDIO_DEVICE_IN_USB_DEVICE;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            device = AUDIO_DEVICE_IN_BUILTIN_MIC;
        }
        break;
    case AUDIO_SOURCE_CAMCORDER:
        if (availableDeviceTypes & AUDIO_DEVICE_IN_BACK_MIC) {
            device = AUDIO_DEVICE_IN_BACK_MIC;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            device = AUDIO_DEVICE_IN_BUILTIN_MIC;
        }
        break;
    case AUDIO_SOURCE_VOICE_DOWNLINK:
    case AUDIO_SOURCE_VOICE_CALL:
        if (availableDeviceTypes & AUDIO_DEVICE_IN_VOICE_CALL) {
            device = AUDIO_DEVICE_IN_VOICE_CALL;
        }
        break;
    case AUDIO_SOURCE_REMOTE_SUBMIX:
        if (availableDeviceTypes & AUDIO_DEVICE_IN_REMOTE_SUBMIX) {
            device = AUDIO_DEVICE_IN_REMOTE_SUBMIX;
        }
        break;
     case AUDIO_SOURCE_FM_TUNER:
        if (availableDeviceTypes & AUDIO_DEVICE_IN_FM_TUNER) {
            device = AUDIO_DEVICE_IN_FM_TUNER;
        }
        break;
    default:
        ALOGW("getDeviceForInputSource() invalid input source %d", inputSource);
        break;
    }
    if (device == AUDIO_DEVICE_NONE) {
        ALOGV("getDeviceForInputSource() no device found for source %d", inputSource);
        if (availableDeviceTypes & AUDIO_DEVICE_IN_STUB) {
            device = AUDIO_DEVICE_IN_STUB;
        }
        ALOGE_IF(device == AUDIO_DEVICE_NONE,
                 "getDeviceForInputSource() no default device defined");
    }
    ALOGV("getDeviceForInputSource()input source %d, device %08x", inputSource, device);
    return device;
}

template <>
AudioPolicyManagerInterface *Engine::queryInterface()
{
    return &mManagerInterface;
}

} // namespace audio_policy
} // namespace android


