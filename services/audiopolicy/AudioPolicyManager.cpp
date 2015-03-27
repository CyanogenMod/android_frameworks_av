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
 *
 * This file was modified by Dolby Laboratories, Inc. The portions of the
 * code that are surrounded by "DOLBY..." are copyrighted and
 * licensed separately, as follows:
 *
 *  (C) 2014 Dolby Laboratories, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file was modified by DTS, Inc. The portions of the
 * code modified by DTS, Inc are copyrighted and
 * licensed separately, as follows:
 *
 *  (C) 2014 DTS, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioPolicyManager"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

// A device mask for all audio input devices that are considered "virtual" when evaluating
// active inputs in getActiveInput()
//FIXME::  Audio team
#ifdef AUDIO_EXTN_FM_ENABLED
#define APM_AUDIO_IN_DEVICE_VIRTUAL_ALL  (AUDIO_DEVICE_IN_REMOTE_SUBMIX | AUDIO_DEVICE_IN_FM_RX_A2DP)
#else
#define APM_AUDIO_IN_DEVICE_VIRTUAL_ALL  (AUDIO_DEVICE_IN_REMOTE_SUBMIX|AUDIO_DEVICE_IN_FM_TUNER)
#endif

// A device mask for all audio output devices that are considered "remote" when evaluating
// active output devices in isStreamActiveRemotely()
#define APM_AUDIO_OUT_DEVICE_REMOTE_ALL  AUDIO_DEVICE_OUT_REMOTE_SUBMIX
// A device mask for all audio input and output devices where matching inputs/outputs on device
// type alone is not enough: the address must match too
#define APM_AUDIO_DEVICE_MATCH_ADDRESS_ALL (AUDIO_DEVICE_IN_REMOTE_SUBMIX | \
                                            AUDIO_DEVICE_OUT_REMOTE_SUBMIX)

// Following delay should be used if the calculated routing delay from all active
// input streams is higher than this value
#define MAX_VOICE_CALL_START_DELAY_MS 100

#include <inttypes.h>
#include <math.h>

#include <cutils/properties.h>
#include <utils/Log.h>
#include <hardware/audio.h>
#include <hardware/audio_effect.h>
#include <media/AudioParameter.h>
#include <media/AudioPolicyHelper.h>
#include <soundtrigger/SoundTrigger.h>
#include "AudioPolicyManager.h"
#include "audio_policy_conf.h"
#if defined(DOLBY_UDC) || defined(DOLBY_DAP_MOVE_EFFECT)
#include "DolbyAudioPolicy_impl.h"
#endif // DOLBY_END

extern "C" {
#include "AudioUtil.h"
}

namespace android {

// ----------------------------------------------------------------------------
// Definitions for audio_policy.conf file parsing
// ----------------------------------------------------------------------------

struct StringToEnum {
    const char *name;
    uint32_t value;
};

#define STRING_TO_ENUM(string) { #string, string }
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

const StringToEnum sDeviceNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_EARPIECE),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_SPEAKER),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_SPEAKER_SAFE),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_WIRED_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_WIRED_HEADPHONE),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_SCO),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_ALL_SCO),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_ALL_A2DP),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_AUX_DIGITAL),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_HDMI),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_USB_ACCESSORY),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_USB_DEVICE),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_ALL_USB),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_REMOTE_SUBMIX),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_TELEPHONY_TX),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_LINE),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_HDMI_ARC),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_SPDIF),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_FM),
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_AUX_LINE),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_AMBIENT),
#ifdef AUDIO_EXTN_AFE_PROXY_ENABLED
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_PROXY),
#endif
#ifdef AUDIO_EXTN_FM_ENABLED
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_FM_TX),
#endif
    STRING_TO_ENUM(AUDIO_DEVICE_IN_BUILTIN_MIC),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_ALL_SCO),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_WIRED_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_AUX_DIGITAL),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_HDMI),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_TELEPHONY_RX),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_VOICE_CALL),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_BACK_MIC),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_REMOTE_SUBMIX),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_USB_ACCESSORY),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_USB_DEVICE),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_FM_TUNER),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_TV_TUNER),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_LINE),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_SPDIF),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_BLUETOOTH_A2DP),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_LOOPBACK),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_COMMUNICATION),
#ifdef AUDIO_EXTN_FM_ENABLED
    STRING_TO_ENUM(AUDIO_DEVICE_IN_FM_RX),
    STRING_TO_ENUM(AUDIO_DEVICE_IN_FM_RX_A2DP),
#endif
};

const StringToEnum sOutputFlagNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DIRECT),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_PRIMARY),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_FAST),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DEEP_BUFFER),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_NON_BLOCKING),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_HW_AV_SYNC),
#ifdef AUDIO_EXTN_INCALL_MUSIC_ENABLED
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_INCALL_MUSIC),
#endif
#ifdef AUDIO_EXTN_COMPRESS_VOIP_ENABLED
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_VOIP_RX),
#endif
#ifdef HDMI_PASSTHROUGH_ENABLED
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH),
#endif
#ifdef QCOM_DIRECTTRACK
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_LPA),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_TUNNEL),
#endif
};

const StringToEnum sInputFlagNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_FAST),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_HW_HOTWORD),
};

const StringToEnum sFormatNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_8_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_32_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_8_24_BIT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_FLOAT),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_24_BIT_PACKED),
    STRING_TO_ENUM(AUDIO_FORMAT_MP3),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_MAIN),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_LC),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_SSR),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_LTP),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_HE_V1),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_SCALABLE),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ERLC),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_LD),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_HE_V2),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ELD),
    STRING_TO_ENUM(AUDIO_FORMAT_VORBIS),
    STRING_TO_ENUM(AUDIO_FORMAT_HE_AAC_V1),
    STRING_TO_ENUM(AUDIO_FORMAT_HE_AAC_V2),
    STRING_TO_ENUM(AUDIO_FORMAT_OPUS),
    STRING_TO_ENUM(AUDIO_FORMAT_AC3),
    STRING_TO_ENUM(AUDIO_FORMAT_E_AC3),
#ifdef AUDIO_EXTN_FORMATS_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_DTS),
    STRING_TO_ENUM(AUDIO_FORMAT_DTS_LBR),
    STRING_TO_ENUM(AUDIO_FORMAT_WMA),
    STRING_TO_ENUM(AUDIO_FORMAT_WMA_PRO),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADIF),
    STRING_TO_ENUM(AUDIO_FORMAT_AMR_NB),
    STRING_TO_ENUM(AUDIO_FORMAT_AMR_WB),
    STRING_TO_ENUM(AUDIO_FORMAT_AMR_WB_PLUS),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRC),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRCB),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRCWB),
    STRING_TO_ENUM(AUDIO_FORMAT_QCELP),
    STRING_TO_ENUM(AUDIO_FORMAT_MP2),
    STRING_TO_ENUM(AUDIO_FORMAT_EVRCNW),
    STRING_TO_ENUM(AUDIO_FORMAT_FLAC),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT_OFFLOAD),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_24_BIT_OFFLOAD),
    STRING_TO_ENUM(AUDIO_FORMAT_E_AC3_JOC),
#endif
};

const StringToEnum sOutChannelsNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_QUAD),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
#if defined(DOLBY_UDC) || defined(DOLBY_DDP)
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_2POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_QUAD),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_SURROUND),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_PENTA),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_6POINT1),
#endif
};

const StringToEnum sInChannelsNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_FRONT_BACK),
#ifdef AUDIO_EXTN_SSR_ENABLED
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_5POINT1),
#endif
#ifdef QCOM_DIRECTTRACK
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_VOICE_CALL_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_VOICE_DNLINK_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_VOICE_UPLINK_MONO),
#endif
};

const StringToEnum sGainModeNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_GAIN_MODE_JOINT),
    STRING_TO_ENUM(AUDIO_GAIN_MODE_CHANNELS),
    STRING_TO_ENUM(AUDIO_GAIN_MODE_RAMP),
};


uint32_t AudioPolicyManager::stringToEnum(const struct StringToEnum *table,
                                              size_t size,
                                              const char *name)
{
    for (size_t i = 0; i < size; i++) {
        if (strcmp(table[i].name, name) == 0) {
            ALOGV("stringToEnum() found %s", table[i].name);
            return table[i].value;
        }
    }
    return 0;
}

const char *AudioPolicyManager::enumToString(const struct StringToEnum *table,
                                              size_t size,
                                              uint32_t value)
{
    for (size_t i = 0; i < size; i++) {
        if (table[i].value == value) {
            return table[i].name;
        }
    }
    return "";
}

bool AudioPolicyManager::stringToBool(const char *value)
{
    return ((strcasecmp("true", value) == 0) || (strcmp("1", value) == 0));
}


// ----------------------------------------------------------------------------
// AudioPolicyInterface implementation
// ----------------------------------------------------------------------------

status_t AudioPolicyManager::setDeviceConnectionState(audio_devices_t device,
                                                          audio_policy_dev_state_t state,
                                                  const char *device_address)
{
    return setDeviceConnectionStateInt(device, state, device_address);
}

status_t AudioPolicyManager::setDeviceConnectionStateInt(audio_devices_t device,
                                                         audio_policy_dev_state_t state,
                                                         const char *device_address)
{
    ALOGV("setDeviceConnectionState() device: %x, state %d, address %s",
            device, state, device_address != NULL ? device_address : "");

    // connect/disconnect only 1 device at a time
    if (!audio_is_output_device(device) && !audio_is_input_device(device)) return BAD_VALUE;

    sp<DeviceDescriptor> devDesc = getDeviceDescriptor(device, device_address);

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
#ifdef AUDIO_EXTN_HDMI_SPK_ENABLED
                if ((popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                   if (!strncmp(device_address, "hdmi_spkr", 9)) {
                        mHdmiAudioDisabled = false;
                    } else {
                        mHdmiAudioEvent = true;
                    }
                }
#endif
                ALOGW("setDeviceConnectionState() device already connected: %x", device);
                return INVALID_OPERATION;
            }
            ALOGV("setDeviceConnectionState() connecting device %x", device);

            // register new device as available
            index = mAvailableOutputDevices.add(devDesc);

#ifdef AUDIO_EXTN_HDMI_SPK_ENABLED
            if ((popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                if (!strncmp(device_address, "hdmi_spkr", 9)) {
                    mHdmiAudioDisabled = false;
                } else {
                    mHdmiAudioEvent = true;
                }
                if (mHdmiAudioDisabled || !mHdmiAudioEvent) {
                    mAvailableOutputDevices.remove(devDesc);
                }
            }
#endif
            if (audio_is_a2dp_device(device)) {
               AudioParameter param;
               param.add(String8("a2dp_connected"), String8("true"));
               mpClientInterface->setParameters(0, param.toString());
            }

#ifdef QCOM_DIRECTTRACK
            if (device & AUDIO_DEVICE_OUT_USB_ACCESSORY) {
               AudioParameter param;
               param.add(String8("usb_connected"), String8("true"));
               mpClientInterface->setParameters(0, param.toString());
            }
#endif

            if (index >= 0) {
                sp<HwModule> module = getModuleForDevice(device);
                if (module == 0) {
                    ALOGD("setDeviceConnectionState() could not find HW module for device %08x",
                          device);
                    mAvailableOutputDevices.remove(devDesc);
                    return INVALID_OPERATION;
                }
                mAvailableOutputDevices[index]->mId = nextUniqueId();
                mAvailableOutputDevices[index]->mModule = module;
            } else {
                return NO_MEMORY;
            }

            if (checkOutputsForDevice(devDesc, state, outputs, devDesc->mAddress) != NO_ERROR) {
                mAvailableOutputDevices.remove(devDesc);
                return INVALID_OPERATION;
            }
            // outputs should never be empty here
            ALOG_ASSERT(outputs.size() != 0, "setDeviceConnectionState():"
                    "checkOutputsForDevice() returned no outputs but status OK");
            ALOGV("setDeviceConnectionState() checkOutputsForDevice() returned %zu outputs",
                  outputs.size());


            // Set connect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_CONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

        } break;
        // handle output device disconnection
        case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE: {
            if (index < 0) {
#ifdef AUDIO_EXTN_HDMI_SPK_ENABLED
                if ((popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                    if (!strncmp(device_address, "hdmi_spkr", 9)) {
                        mHdmiAudioDisabled = true;
                    } else {
                        mHdmiAudioEvent = false;
                    }
                }
#endif
                ALOGW("setDeviceConnectionState() device not connected: %x", device);
                return INVALID_OPERATION;
            }

            ALOGV("setDeviceConnectionState() disconnecting output device %x", device);

            // Set Disconnect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_DISCONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

            // remove device from available output devices
            mAvailableOutputDevices.remove(devDesc);

#ifdef AUDIO_EXTN_HDMI_SPK_ENABLED
            if ((popcount(device) == 1) && (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
                if (!strncmp(device_address, "hdmi_spkr", 9)) {
                    mHdmiAudioDisabled = true;
                } else {
                    mHdmiAudioEvent = false;
                }
            }
#endif
            if (audio_is_a2dp_device(device)) {
               AudioParameter param;
               param.add(String8("a2dp_connected"), String8("false"));
               mpClientInterface->setParameters(0, param.toString());
            }

#ifdef QCOM_DIRECTTRACK
            if (device & AUDIO_DEVICE_OUT_USB_ACCESSORY) {
                // handle USB device disconnection
                AudioParameter param;
                param.add(String8("usb_connected"), String8("false"));
                mpClientInterface->setParameters(0, param.toString());
            }
#endif

            checkOutputsForDevice(devDesc, state, outputs, devDesc->mAddress);
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
                sp<AudioOutputDescriptor> desc = mOutputs.valueFor(outputs[i]);
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
#ifdef DTS_EAGLE
        audio_devices_t ndev = getDeviceForStrategy(STRATEGY_MEDIA, true);
        audio_devices_t availableOutputDevices = availablePrimaryOutputDevices();
        ALOGV("setDeviceConnectionState: device 0x%x all_devices 0x%x", ndev, availableOutputDevices);
        notify_route_node(ndev, availableOutputDevices);
#endif
        audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
#ifdef DOLBY_UDC
        // Before closing the opened outputs, update endpoint property with device capabilities
        audio_devices_t audioOutputDevice = getDeviceForStrategy(getStrategy(AUDIO_STREAM_MUSIC), true);
        mDolbyAudioPolicy.setEndpointSystemProperty(audioOutputDevice, mHwModules);
#endif // DOLBY_END
        if (mPhoneState == AUDIO_MODE_IN_CALL) {
            updateCallRouting(newDevice);
        }

#ifdef AUDIO_EXTN_FM_ENABLED
        if(device == AUDIO_DEVICE_OUT_FM) {
            if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
                mOutputs.valueFor(mPrimaryOutput)->changeRefCount(AUDIO_STREAM_MUSIC, 1);
                newDevice = (audio_devices_t)(getNewOutputDevice(mPrimaryOutput, false) | AUDIO_DEVICE_OUT_FM);
            } else {
                mOutputs.valueFor(mPrimaryOutput)->changeRefCount(AUDIO_STREAM_MUSIC, -1);
            }

            AudioParameter param = AudioParameter();
            param.addInt(String8("handle_fm"), (int)newDevice);
            ALOGV("setDeviceConnectionState() setParameters handle_fm");
            mpClientInterface->setParameters(mPrimaryOutput, param.toString());
        }
#endif
        for (size_t i = 0; i < mOutputs.size(); i++) {
            audio_io_handle_t output = mOutputs.keyAt(i);
            if ((mPhoneState != AUDIO_MODE_IN_CALL) || (output != mPrimaryOutput)) {
                audio_devices_t newDevice = getNewOutputDevice(mOutputs.keyAt(i),
                                                               true /*fromCache*/);

#ifdef HDMI_PASSTHROUGH_ENABLED
                // Change the device to speaker for ringtone/touch tone/alarm etc.
                newDevice = handleHDMIPassthrough(newDevice, mOutputs.keyAt(i));
#endif
                // do not force device change on duplicated output because if device is 0, it will
                // also force a device 0 for the two outputs it is duplicated to which may override
                // a valid device selection on those outputs.
                bool force = !mOutputs.valueAt(i)->isDuplicated()
                        && (!deviceDistinguishesOnAddress(device)
                                // always force when disconnecting (a non-duplicated device)
                                || (state == AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE));
                setOutputDevice(output, newDevice, force, 0);
            }
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
            sp<HwModule> module = getModuleForDevice(device);
            if (module == NULL) {
                ALOGW("setDeviceConnectionState(): could not find HW module for device %08x",
                      device);
                return INVALID_OPERATION;
            }
            if (checkInputsForDevice(device, state, inputs, devDesc->mAddress) != NO_ERROR) {
                return INVALID_OPERATION;
            }

            index = mAvailableInputDevices.add(devDesc);
            if (index >= 0) {
                mAvailableInputDevices[index]->mId = nextUniqueId();
                mAvailableInputDevices[index]->mModule = module;
            } else {
                return NO_MEMORY;
            }

            // Set connect to HALs
            AudioParameter param = AudioParameter(devDesc->mAddress);
            param.addInt(String8(AUDIO_PARAMETER_DEVICE_CONNECT), device);
            mpClientInterface->setParameters(AUDIO_IO_HANDLE_NONE, param.toString());

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

            checkInputsForDevice(device, state, inputs, devDesc->mAddress);
            mAvailableInputDevices.remove(devDesc);

        } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        closeAllInputs();

        if (mPhoneState == AUDIO_MODE_IN_CALL) {
            audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
            updateCallRouting(newDevice);
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
    sp<DeviceDescriptor> devDesc = getDeviceDescriptor(device, device_address);
    DeviceVector *deviceVector;

    if (audio_is_output_device(device)) {
        deviceVector = &mAvailableOutputDevices;
    } else if (audio_is_input_device(device)) {
        deviceVector = &mAvailableInputDevices;
    } else {
        ALOGW("getDeviceConnectionState() invalid device type %08x", device);
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }

    ssize_t index = deviceVector->indexOf(devDesc);
    if (index >= 0) {
        return AUDIO_POLICY_DEVICE_STATE_AVAILABLE;
    } else {
        return AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
    }
}

sp<AudioPolicyManager::DeviceDescriptor>  AudioPolicyManager::getDeviceDescriptor(
                                                                    const audio_devices_t device,
                                                                    const char *device_address)
{
    String8 address = (device_address == NULL) ? String8("") : String8(device_address);
    // handle legacy remote submix case where the address was not always specified
    if (deviceDistinguishesOnAddress(device) && (address.length() == 0)) {
        address = String8("0");
    }

    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (mHwModules[i]->mHandle == 0) {
            continue;
        }
        DeviceVector deviceList =
                mHwModules[i]->mDeclaredDevices.getDevicesFromTypeAddr(device, address);
        if (!deviceList.isEmpty()) {
            return deviceList.itemAt(0);
        }
        deviceList = mHwModules[i]->mDeclaredDevices.getDevicesFromType(device);
        if (!deviceList.isEmpty()) {
            return deviceList.itemAt(0);
        }
    }

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(String8(""), device);
    devDesc->mAddress = address;
    return devDesc;
}

void AudioPolicyManager::updateCallRouting(audio_devices_t rxDevice, int delayMs)
{
    bool createTxPatch = false;
    struct audio_patch patch;
    patch.num_sources = 1;
    patch.num_sinks = 1;
    status_t status;
    audio_patch_handle_t afPatchHandle;
    DeviceVector deviceList;

#ifndef QCOM_DIRECTTRACK
    audio_devices_t txDevice = getDeviceAndMixForInputSource(AUDIO_SOURCE_VOICE_COMMUNICATION);
#else
    audio_devices_t txDevice = getDeviceAndMixForInputSource(AUDIO_SOURCE_VOICE_CALL);
#endif
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
        setOutputDevice(mPrimaryOutput, rxDevice, true, delayMs);
        // If the TX device is also on the primary HW module, setOutputDevice() will take care
        // of it due to legacy implementation. If not, create a patch.
        if ((availablePrimaryInputDevices() & txDevice & ~AUDIO_DEVICE_BIT_IN)
                == AUDIO_DEVICE_NONE) {
            createTxPatch = true;
        }
    } else {
        // create RX path audio patch
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
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "updateCallRouting() RX device output is duplicated");
            outputDesc->toAudioPortConfig(&patch.sources[1]);
            patch.num_sources = 2;
        }

        afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
        status = mpClientInterface->createAudioPatch(&patch, &afPatchHandle, 0);
        ALOGW_IF(status != NO_ERROR, "updateCallRouting() error %d creating RX audio patch",
                                               status);
        if (status == NO_ERROR) {
            mCallRxPatch = new AudioPatch((audio_patch_handle_t)nextUniqueId(),
                                       &patch, mUidCached);
            mCallRxPatch->mAfPatchHandle = afPatchHandle;
            mCallRxPatch->mUid = mUidCached;
        }
        createTxPatch = true;
    }
    if (createTxPatch) {

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
            // close active input (if any) before opening new input
            audio_io_handle_t activeInput = getActiveInput();
            if (activeInput != 0) {
                ALOGV("updateCallRouting() close active input before opening new input");
                sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
                stopInput(activeInput, activeDesc->mSessions.itemAt(0));
                releaseInput(activeInput, activeDesc->mSessions.itemAt(0));
            }
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "updateCallRouting() RX device output is duplicated");
            outputDesc->toAudioPortConfig(&patch.sources[1]);
            patch.num_sources = 2;
        }

        afPatchHandle = AUDIO_PATCH_HANDLE_NONE;
        status = mpClientInterface->createAudioPatch(&patch, &afPatchHandle, 0);
        ALOGW_IF(status != NO_ERROR, "setPhoneState() error %d creating TX audio patch",
                                               status);
        if (status == NO_ERROR) {
            mCallTxPatch = new AudioPatch((audio_patch_handle_t)nextUniqueId(),
                                       &patch, mUidCached);
            mCallTxPatch->mAfPatchHandle = afPatchHandle;
            mCallTxPatch->mUid = mUidCached;
        }
    }
}

void AudioPolicyManager::setPhoneState(audio_mode_t state)
{
    ALOGD("setPhoneState() state %d", state);
    audio_devices_t newDevice = AUDIO_DEVICE_NONE;

    if (state < 0 || state >= AUDIO_MODE_CNT) {
        ALOGW("setPhoneState() invalid state %d", state);
        return;
    }

    if (state == mPhoneState ) {
        ALOGW("setPhoneState() setting same state %d", state);
        return;
    }

    // if leaving call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (isInCall()) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AUDIO_STREAM_CNT; stream++) {
            if (stream == AUDIO_STREAM_PATCH) {
                continue;
            }
            handleIncallSonification((audio_stream_type_t)stream, false, true);
        }

        // force reevaluating accessibility routing when call starts
        mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }

    // store previous phone state for management of sonification strategy below
    int oldState = mPhoneState;
    mPhoneState = state;
    bool force = false;

    // are we entering or starting a call
    if (!isStateInCall(oldState) && isStateInCall(state)) {
        ALOGV("  Entering call in setPhoneState()");
        // force routing command to audio hardware when starting a call
        // even if no device change is needed
        force = true;
        for (int j = 0; j < DEVICE_CATEGORY_CNT; j++) {
            mStreams[AUDIO_STREAM_DTMF].mVolumeCurve[j] =
                    sVolumeProfiles[AUDIO_STREAM_VOICE_CALL][j];
        }
    } else if (isStateInCall(oldState) && !isStateInCall(state)) {
        ALOGV("  Exiting call in setPhoneState()");
        // force routing command to audio hardware when exiting a call
        // even if no device change is needed
        force = true;
        for (int j = 0; j < DEVICE_CATEGORY_CNT; j++) {
            mStreams[AUDIO_STREAM_DTMF].mVolumeCurve[j] =
                    sVolumeProfiles[AUDIO_STREAM_DTMF][j];
        }
    } else if (isStateInCall(state) && (state != oldState)) {
        ALOGV("  Switching between telephony and VoIP in setPhoneState()");
        // force routing command to audio hardware when switching between telephony and VoIP
        // even if no device change is needed
        force = true;
    }

    // check for device and output changes triggered by new phone state
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();

    sp<AudioOutputDescriptor> hwOutputDesc = mOutputs.valueFor(mPrimaryOutput);

#ifdef VOICE_CONCURRENCY
    int voice_call_state = 0;
    bool prop_playback_enabled = !property_get_bool("voice.playback.conc.disabled", false);
    bool prop_rec_enabled = !property_get_bool("voice.record.conc.disabled", false);
    bool prop_voip_enabled = !property_get_bool("voice.voip.conc.disabled", false);
    bool mode_in_call = (AUDIO_MODE_IN_CALL != oldState) && (AUDIO_MODE_IN_CALL == state);
    //query if it is a actual voice call initiated by telephony
    if (mode_in_call) {
        String8 valueStr = mpClientInterface->getParameters((audio_io_handle_t)0, String8("in_call"));
        AudioParameter result = AudioParameter(valueStr);
        if (result.getInt(String8("in_call"), voice_call_state) == NO_ERROR)
            ALOGD("SetPhoneState: Voice call state = %d", voice_call_state);
    }

    if (mode_in_call && voice_call_state && !mvoice_call_state) {
        ALOGD("Entering to call mode oldState :: %d state::%d ",oldState, state);
        mvoice_call_state = voice_call_state;
        if (prop_playback_enabled) {
            //Call invalidate to reset all opened non ULL audio tracks
            // Move tracks associated to this strategy from previous output to new output
            for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
                ALOGV(" Invalidate on call mode for stream :: %d ", i);
                if (i == AUDIO_STREAM_PATCH) {
                    ALOGV("not calling invalidate for AUDIO_STREAM_PATCH");
                    continue;
                }
                mpClientInterface->invalidateStream((audio_stream_type_t)i);
            }
        }

        if (prop_rec_enabled) {
            //Close all active inputs
            audio_io_handle_t activeInput = getActiveInput();
            if (activeInput != 0) {
               sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
               switch(activeDesc->mInputSource) {
                   case AUDIO_SOURCE_VOICE_UPLINK:
                   case AUDIO_SOURCE_VOICE_DOWNLINK:
                   case AUDIO_SOURCE_VOICE_CALL:
                       ALOGD("FOUND active input during call active: %d",activeDesc->mInputSource);
                   break;

                   case  AUDIO_SOURCE_VOICE_COMMUNICATION:
                        if(prop_voip_enabled) {
                            ALOGD("CLOSING VoIP input source on call setup :%d ",activeDesc->mInputSource);
                            stopInput(activeInput, activeDesc->mSessions.itemAt(0));
                            releaseInput(activeInput, activeDesc->mSessions.itemAt(0));
                        }
                   break;

                   default:
                       ALOGD("CLOSING input on call setup  for inputSource: %d",activeDesc->mInputSource);
                       stopInput(activeInput, activeDesc->mSessions.itemAt(0));
                       releaseInput(activeInput, activeDesc->mSessions.itemAt(0));
                   break;
               }
           }
        } else if (prop_voip_enabled) {
            audio_io_handle_t activeInput = getActiveInput();
            if (activeInput != 0) {
                sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
                if (AUDIO_SOURCE_VOICE_COMMUNICATION == activeDesc->mInputSource) {
                    ALOGD("CLOSING VoIP on call setup : %d",activeDesc->mInputSource);
                    stopInput(activeInput, activeDesc->mSessions.itemAt(0));
                    releaseInput(activeInput, activeDesc->mSessions.itemAt(0));
                }
            }
        }

        //suspend  PCM (deep-buffer) output & close  compress & direct tracks
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
            if ( (outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
               ALOGD("ouput desc / profile is NULL");
               continue;
            }
            if (((!outputDesc->isDuplicated() &&outputDesc->mProfile->mFlags & AUDIO_OUTPUT_FLAG_PRIMARY))
                        && prop_playback_enabled) {
                ALOGD(" calling suspendOutput on call mode for primary output");
                mpClientInterface->suspendOutput(mOutputs.keyAt(i));
            } //Close compress all sessions
            else if ((outputDesc->mProfile->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                            &&  prop_playback_enabled) {
                ALOGD(" calling closeOutput on call mode for COMPRESS output");
                closeOutput(mOutputs.keyAt(i));
            }
            else if ((outputDesc->mProfile->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)
                            && prop_voip_enabled) {
                ALOGD(" calling closeOutput on call mode for DIRECT  output");
                closeOutput(mOutputs.keyAt(i));
            }
        }
   }

   if ((AUDIO_MODE_IN_CALL == oldState || AUDIO_MODE_IN_COMMUNICATION == oldState) &&
       (AUDIO_MODE_NORMAL == state) && prop_playback_enabled && mvoice_call_state) {
        ALOGD("EXITING from call mode oldState :: %d state::%d \n",oldState, state);
        mvoice_call_state = 0;
        //restore PCM (deep-buffer) output after call termination
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
            if ( (outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
               ALOGD("ouput desc / profile is NULL");
               continue;
            }
            if (!outputDesc->isDuplicated() && outputDesc->mProfile->mFlags & AUDIO_OUTPUT_FLAG_PRIMARY) {
                ALOGD("calling restoreOutput after call mode for primary output");
                mpClientInterface->restoreOutput(mOutputs.keyAt(i));
            }
       }
       //call invalidate tracks so that any open streams can fall back to deep buffer/compress path from ULL
       for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
           ALOGD("Invalidate after call ends for stream :: %d ", i);
           if (i == AUDIO_STREAM_PATCH) {
               ALOGV("not calling invalidate for AUDIO_STREAM_PATCH");
               continue;
           }
           mpClientInterface->invalidateStream((audio_stream_type_t)i);
       }
    }
#endif
#ifdef RECORD_PLAY_CONCURRENCY
    char recConcPropValue[PROPERTY_VALUE_MAX];
    bool prop_rec_play_enabled = false;

    if (property_get("rec.playback.conc.disabled", recConcPropValue, NULL)) {
        prop_rec_play_enabled = atoi(recConcPropValue) || !strncmp("true", recConcPropValue, 4);
    }
    if (prop_rec_play_enabled) {
        if (AUDIO_MODE_IN_COMMUNICATION == mPhoneState) {
            ALOGD("phone state changed to MODE_IN_COMM invlaidating music and voice streams");
            // call invalidate for voice streams, so that it can use deepbuffer with VoIP out device from HAL
            mpClientInterface->invalidateStream(AUDIO_STREAM_VOICE_CALL);
            // call invalidate for music, so that compress will fallback to deep-buffer with VoIP out device
            mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);

            // close compress output to make sure session will be closed before timeout(60sec)
            for (size_t i = 0; i < mOutputs.size(); i++) {

                sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
                if ((outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
                   ALOGD("ouput desc / profile is NULL");
                   continue;
                }

                if (outputDesc->mProfile->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                    ALOGD("calling closeOutput on call mode for COMPRESS output");
                    closeOutput(mOutputs.keyAt(i));
                }
            }
        } else if ((oldState == AUDIO_MODE_IN_COMMUNICATION) &&
                    (mPhoneState == AUDIO_MODE_NORMAL)) {
            // call invalidate for music so that music can fallback to compress
            mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);
        }
    }
#endif

    mPrevPhoneState = oldState;

    int delayMs = 0;
    if (isStateInCall(state)) {
        nsecs_t sysTime = systemTime();
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            // mute media and sonification strategies and delay device switch by the largest
            // latency of any output where either strategy is active.
            // This avoid sending the ring tone or music tail into the earpiece or headset.
            if ((desc->isStrategyActive(STRATEGY_MEDIA,
                                     SONIFICATION_HEADSET_MUSIC_DELAY,
                                     sysTime) ||
                    desc->isStrategyActive(STRATEGY_SONIFICATION,
                                         SONIFICATION_HEADSET_MUSIC_DELAY,
                                         sysTime)) &&
                    (delayMs < (int)desc->mLatency*2)) {
                delayMs = desc->mLatency*2;
            }
            setStrategyMute(STRATEGY_MEDIA, true, mOutputs.keyAt(i));
            setStrategyMute(STRATEGY_MEDIA, false, mOutputs.keyAt(i), MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_MEDIA, true /*fromCache*/));
            setStrategyMute(STRATEGY_SONIFICATION, true, mOutputs.keyAt(i));
            setStrategyMute(STRATEGY_SONIFICATION, false, mOutputs.keyAt(i), MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_SONIFICATION, true /*fromCache*/));
        }
         ALOGV("Setting the delay from %dms to %dms", delayMs,
               MIN(delayMs, MAX_VOICE_CALL_START_DELAY_MS));
         delayMs = MIN(delayMs, MAX_VOICE_CALL_START_DELAY_MS);
    }

    // Note that despite the fact that getNewOutputDevice() is called on the primary output,
    // the device returned is not necessarily reachable via this output
    audio_devices_t rxDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
    // force routing command to audio hardware when ending call
    // even if no device change is needed
    if (isStateInCall(oldState) && rxDevice == AUDIO_DEVICE_NONE) {
        rxDevice = hwOutputDesc->device();
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
    //update device for all non-primary outputs
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_io_handle_t output = mOutputs.keyAt(i);
        if (output != mPrimaryOutput) {
            newDevice = getNewOutputDevice(output, false /*fromCache*/);
#ifdef HDMI_PASSTHROUGH_ENABLED
        {
            newDevice = handleHDMIPassthrough(newDevice, output);
        }
#endif
            setOutputDevice(output, newDevice, (newDevice != AUDIO_DEVICE_NONE));
        }
    }

    // if entering in call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (isStateInCall(state)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AUDIO_STREAM_CNT; stream++) {
            if (stream == AUDIO_STREAM_PATCH) {
                continue;
            }
            handleIncallSonification((audio_stream_type_t)stream, true, true);
        }
    }

    // Flag that ringtone volume must be limited to music volume until we exit MODE_RINGTONE
    if (state == AUDIO_MODE_RINGTONE &&
        isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY)) {
        mLimitRingtoneVolume = true;
    } else {
        mLimitRingtoneVolume = false;
    }
}

void AudioPolicyManager::setForceUse(audio_policy_force_use_t usage,
                                         audio_policy_forced_cfg_t config)
{
    ALOGV("setForceUse() usage %d, config %d, mPhoneState %d", usage, config, mPhoneState);

    bool forceVolumeReeval = false;
    switch(usage) {
    case AUDIO_POLICY_FORCE_FOR_COMMUNICATION:
        if (config != AUDIO_POLICY_FORCE_SPEAKER && config != AUDIO_POLICY_FORCE_BT_SCO &&
            config != AUDIO_POLICY_FORCE_NONE) {
            ALOGW("setForceUse() invalid config %d for FOR_COMMUNICATION", config);
            return;
        }
        forceVolumeReeval = true;
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_MEDIA:
        if (config != AUDIO_POLICY_FORCE_HEADPHONES && config != AUDIO_POLICY_FORCE_BT_A2DP &&
#ifdef AUDIO_EXTN_FM_ENABLED
            config != AUDIO_POLICY_FORCE_SPEAKER &&
#endif
            config != AUDIO_POLICY_FORCE_WIRED_ACCESSORY &&
            config != AUDIO_POLICY_FORCE_ANALOG_DOCK &&
            config != AUDIO_POLICY_FORCE_DIGITAL_DOCK && config != AUDIO_POLICY_FORCE_NONE &&
            config != AUDIO_POLICY_FORCE_NO_BT_A2DP && config != AUDIO_POLICY_FORCE_SPEAKER ) {
            ALOGW("setForceUse() invalid config %d for FOR_MEDIA", config);
            return;
        }
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_RECORD:
        if (config != AUDIO_POLICY_FORCE_BT_SCO && config != AUDIO_POLICY_FORCE_WIRED_ACCESSORY &&
            config != AUDIO_POLICY_FORCE_NONE) {
            ALOGW("setForceUse() invalid config %d for FOR_RECORD", config);
            return;
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
        forceVolumeReeval = true;
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_SYSTEM:
        if (config != AUDIO_POLICY_FORCE_NONE &&
            config != AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
            ALOGW("setForceUse() invalid config %d for FOR_SYSTEM", config);
        }
        forceVolumeReeval = true;
        mForceUse[usage] = config;
        break;
    case AUDIO_POLICY_FORCE_FOR_HDMI_SYSTEM_AUDIO:
        if (config != AUDIO_POLICY_FORCE_NONE &&
            config != AUDIO_POLICY_FORCE_HDMI_SYSTEM_AUDIO_ENFORCED) {
            ALOGW("setForceUse() invalid config %d forHDMI_SYSTEM_AUDIO", config);
        }
        mForceUse[usage] = config;
        break;
    default:
        ALOGW("setForceUse() invalid usage %d", usage);
        break;
    }

    // check for device and output changes triggered by new force usage
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();
    if (mPhoneState == AUDIO_MODE_IN_CALL) {
        audio_devices_t newDevice = getNewOutputDevice(mPrimaryOutput, true /*fromCache*/);
        updateCallRouting(newDevice);
    }
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_io_handle_t output = mOutputs.keyAt(i);
        audio_devices_t newDevice = getNewOutputDevice(output, true /*fromCache*/);
#ifdef HDMI_PASSTHROUGH_ENABLED
        newDevice = handleHDMIPassthrough(newDevice, output);
#endif
        if ((mPhoneState != AUDIO_MODE_IN_CALL) || (output != mPrimaryOutput)) {
            setOutputDevice(output, newDevice, (newDevice != AUDIO_DEVICE_NONE));
        }
        if (forceVolumeReeval && (newDevice != AUDIO_DEVICE_NONE)) {
            applyStreamVolumes(output, newDevice, 0, true);
        }
    }

    audio_io_handle_t activeInput = getActiveInput();
    if (activeInput != 0) {
        setInputDevice(activeInput, getNewInputDevice(activeInput));
    }

}

audio_policy_forced_cfg_t AudioPolicyManager::getForceUse(audio_policy_force_use_t usage)
{
    return mForceUse[usage];
}

void AudioPolicyManager::setSystemProperty(const char* property, const char* value)
{
    ALOGV("setSystemProperty() property %s, value %s", property, value);
}

// Find a direct output profile compatible with the parameters passed, even if the input flags do
// not explicitly request a direct output
sp<AudioPolicyManager::IOProfile> AudioPolicyManager::getProfileForDirectOutput(
                                                               audio_devices_t device,
                                                               uint32_t samplingRate,
                                                               audio_format_t format,
                                                               audio_channel_mask_t channelMask,
                                                               audio_output_flags_t flags)
{
#ifdef QCOM_DIRECTTRACK
    if( !((flags & AUDIO_OUTPUT_FLAG_LPA)   ||
          (flags & AUDIO_OUTPUT_FLAG_TUNNEL)||
          (flags & AUDIO_OUTPUT_FLAG_VOIP_RX)) )
        flags = AUDIO_OUTPUT_FLAG_DIRECT;
#endif

    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (mHwModules[i]->mHandle == 0) {
            continue;
        }
        for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++) {
            sp<IOProfile> profile = mHwModules[i]->mOutputProfiles[j];
            bool found = profile->isCompatibleProfile(device, String8(""), samplingRate,
                    NULL /*updatedSamplingRate*/, format, channelMask,
                    flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD ?
                        flags : (audio_output_flags_t) (AUDIO_OUTPUT_FLAG_DIRECT | flags));
            if (found && (mAvailableOutputDevices.types() & profile->mSupportedDevices.types())) {
                return profile;
            }
        }
    }
    return 0;
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


#ifdef HDMI_PASSTHROUGH_ENABLED
    if (device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        if (((flags & AUDIO_OUTPUT_FLAG_FAST) &&
            (stream == AUDIO_STREAM_SYSTEM)) ||
            (strategy == STRATEGY_SONIFICATION_RESPECTFUL))
            // Change device to speaker in case of system tone & message tone
            device = AUDIO_DEVICE_OUT_SPEAKER;
    ALOGV("getOutput() after device change device %d, stream %d, samplingRate %d, format %x, channelMask %x, flags %x",
          device, stream, samplingRate, format, channelMask, flags);
    }
#endif
    return getOutputForDevice(device, AUDIO_SESSION_ALLOCATE,
                              stream, samplingRate,format, channelMask,
                              flags, offloadInfo);
}

status_t AudioPolicyManager::getOutputForAttr(const audio_attributes_t *attr,
                                              audio_io_handle_t *output,
                                              audio_session_t session,
                                              audio_stream_type_t *stream,
                                              uint32_t samplingRate,
                                              audio_format_t format,
                                              audio_channel_mask_t channelMask,
                                              audio_output_flags_t flags,
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

    for (size_t i = 0; i < mPolicyMixes.size(); i++) {
        sp<AudioOutputDescriptor> desc;
        if (mPolicyMixes[i]->mMix.mMixType == MIX_TYPE_PLAYERS) {
            for (size_t j = 0; j < mPolicyMixes[i]->mMix.mCriteria.size(); j++) {
                if ((RULE_MATCH_ATTRIBUTE_USAGE == mPolicyMixes[i]->mMix.mCriteria[j].mRule &&
                        mPolicyMixes[i]->mMix.mCriteria[j].mAttr.mUsage == attributes.usage) ||
                    (RULE_EXCLUDE_ATTRIBUTE_USAGE == mPolicyMixes[i]->mMix.mCriteria[j].mRule &&
                        mPolicyMixes[i]->mMix.mCriteria[j].mAttr.mUsage != attributes.usage)) {
                    desc = mPolicyMixes[i]->mOutput;
                    break;
                }
                if (strncmp(attributes.tags, "addr=", strlen("addr=")) == 0 &&
                        strncmp(attributes.tags + strlen("addr="),
                                mPolicyMixes[i]->mMix.mRegistrationId.string(),
                                AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - strlen("addr=") - 1) == 0) {
                    desc = mPolicyMixes[i]->mOutput;
                    break;
                }
            }
        } else if (mPolicyMixes[i]->mMix.mMixType == MIX_TYPE_RECORDERS) {
            if (attributes.usage == AUDIO_USAGE_VIRTUAL_SOURCE &&
                    strncmp(attributes.tags, "addr=", strlen("addr=")) == 0 &&
                    strncmp(attributes.tags + strlen("addr="),
                            mPolicyMixes[i]->mMix.mRegistrationId.string(),
                            AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - strlen("addr=") - 1) == 0) {
                desc = mPolicyMixes[i]->mOutput;
            }
        }
        if (desc != 0) {
            if (!audio_is_linear_pcm(format)) {
                return BAD_VALUE;
            }
            desc->mPolicyMix = &mPolicyMixes[i]->mMix;
            *stream = streamTypefromAttributesInt(&attributes);
            *output = desc->mIoHandle;
            ALOGV("getOutputForAttr() returns output %d", *output);
            return NO_ERROR;
        }
    }
    if (attributes.usage == AUDIO_USAGE_VIRTUAL_SOURCE) {
        ALOGW("getOutputForAttr() no policy mix found for usage AUDIO_USAGE_VIRTUAL_SOURCE");
        return BAD_VALUE;
    }

    ALOGV("getOutputForAttr() usage=%d, content=%d, tag=%s flags=%08x",
            attributes.usage, attributes.content_type, attributes.tags, attributes.flags);

    routing_strategy strategy = (routing_strategy) getStrategyForAttr(&attributes);
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);

    if ((attributes.flags & AUDIO_FLAG_HW_AV_SYNC) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
    }

    ALOGV("getOutputForAttr() device 0x%x, samplingRate %d, format %x, channelMask %x, flags %x",
          device, samplingRate, format, channelMask, flags);

#ifdef HDMI_PASSTHROUGH_ENABLED
    if (device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        if (((flags & AUDIO_OUTPUT_FLAG_FAST) &&
            (stream == AUDIO_STREAM_SYSTEM)) ||
            (strategy == STRATEGY_SONIFICATION_RESPECTFUL))
            // Change device to speaker in case of system tone & message tone
            device = AUDIO_DEVICE_OUT_SPEAKER;
            ALOGV("getOutputForAttr() after device change device %d, samplingRate %d, format %x, channelMask %x, flags %x",
          device, samplingRate, format, channelMask, flags);
    }
#endif

    *stream = streamTypefromAttributesInt(&attributes);

#ifdef DEEP_BUFFER_RINGTONE
    // don't use low latency for ringtones as it could cause i/o starvation
    // in usecases like camera where a device may be in thermal mitigation
    if (attributes.usage == AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE) {
        flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    }
#endif

    *output = getOutputForDevice(device, session, *stream,
                                 samplingRate, format, channelMask,
                                 flags, offloadInfo);
    if (*output == AUDIO_IO_HANDLE_NONE) {
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
    uint32_t latency = 0;
    status_t status;


#ifdef AUDIO_POLICY_TEST
    if (mCurOutput != 0) {
        ALOGV("getOutput() test output mCurOutput %d, samplingRate %d, format %d, channelMask %x, mDirectOutput %d",
                mCurOutput, mTestSamplingRate, mTestFormat, mTestChannels, mDirectOutput);

        if (mTestOutputs[mCurOutput] == 0) {
            ALOGV("getOutput() opening test output");
            sp<AudioOutputDescriptor> outputDesc = new AudioOutputDescriptor(NULL);
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

    if (((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) &&
            (stream != AUDIO_STREAM_MUSIC)) {
        // compress should not be used for non-music streams
        ALOGE("Offloading only allowed with music stream");
        return 0;
    }

#ifdef VOICE_CONCURRENCY
    char propValue[PROPERTY_VALUE_MAX];
    bool prop_play_enabled=false, prop_voip_enabled = false;

    if(property_get("voice.playback.conc.disabled", propValue, NULL)) {
       prop_play_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
    }

    if(property_get("voice.voip.conc.disabled", propValue, NULL)) {
       prop_voip_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
    }

    if (prop_play_enabled && mvoice_call_state) {
        //check if voice call is active  / running in background
        if((AUDIO_MODE_IN_CALL == mPhoneState) ||
             ((AUDIO_MODE_IN_CALL == mPrevPhoneState)
                && (AUDIO_MODE_IN_COMMUNICATION == mPhoneState)))
        {
            if(AUDIO_OUTPUT_FLAG_VOIP_RX  & flags) {
                if(prop_voip_enabled) {
                    ALOGD(" IN call mode returing no output .. for VoIP usecase flags: %x ", flags );
                   // flags = (AudioSystem::output_flags)AUDIO_OUTPUT_FLAG_FAST;
                   return 0;
                }
            }
            else {
                ALOGD(" IN call mode adding ULL flags .. flags: %x ", flags );
                flags = AUDIO_OUTPUT_FLAG_FAST;
            }
        }
    } else if (prop_voip_enabled && mvoice_call_state) {
        //check if voice call is active  / running in background
        //some of VoIP apps(like SIP2SIP call) supports resume of VoIP call when call in progress
        //return only ULL ouput
        if((AUDIO_MODE_IN_CALL == mPhoneState) ||
             ((AUDIO_MODE_IN_CALL == mPrevPhoneState)
                && (AUDIO_MODE_IN_COMMUNICATION == mPhoneState)))
        {
            if(AUDIO_OUTPUT_FLAG_VOIP_RX  & flags) {
                ALOGD(" IN call mode returing no output .. for VoIP usecase flags: %x ", flags );
               // flags = (AudioSystem::output_flags)AUDIO_OUTPUT_FLAG_FAST;
               return 0;
            }
        }
     }
#endif

#ifdef HDMI_PASSTHROUGH_ENABLED
    output = getPassthroughOutput(stream, samplingRate, format, channelMask,
                                  flags, offloadInfo, device);
    if (output < 0) {
        // Error returned if client requests for passthrough and output not available
        ALOGE("getPassthroughOutput() returns error %d", output);
        return 0;
    } else if (output != 0) {
        ALOGE("getPassthroughOutput() returns new direct output %d", output);
        return output;
    } else {
        ALOGE("getPassthroughOutput:No passthrough o/p returned,try other o/p");
    }

    if (device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        // Invalidate and close the passthrough output if there is an incoming
        // non passthrough music streams.
        // Condition is ignores for for system  streams and message tone as they
        // will be played out on speaker.
        if ((stream == AUDIO_STREAM_SYSTEM) ||
            (stream == AUDIO_STREAM_RING) ||
             (stream == AUDIO_STREAM_ALARM)
            /*(strategy == STRATEGY_SONIFICATION_RESPECTFUL)*/) {
            ALOGV("Do not update and close output system tones/sonification");
        } else {
            ALOGV("check and invalidate");
            updateAndCloseOutputs();
        }
    }
#endif

#ifdef AUDIO_EXTN_AFE_PROXY_ENABLED
    /*
    * WFD audio routes back to target speaker when starting a ringtone playback.
    * This is because primary output is reused for ringtone, so output device is
    * updated based on SONIFICATION strategy for both ringtone and music playback.
    * The same issue is not seen on remoted_submix HAL based WFD audio because
    * primary output is not reused and a new output is created for ringtone playback.
    * Issue is fixed by updating output flag to AUDIO_OUTPUT_FLAG_FAST when there is
    * a non-music stream playback on WFD, so primary output is not reused for ringtone.
    */
    audio_devices_t availableOutputDeviceTypes = mAvailableOutputDevices.types();
    if ((availableOutputDeviceTypes & AUDIO_DEVICE_OUT_PROXY)
          && (stream != AUDIO_STREAM_MUSIC)) {
        ALOGD("WFD audio: use OUTPUT_FLAG_FAST for non music stream. flags:%x", flags );
        //For voip paths
        if(flags & AUDIO_OUTPUT_FLAG_DIRECT)
            flags = AUDIO_OUTPUT_FLAG_DIRECT;
        else //route every thing else to ULL path
            flags = AUDIO_OUTPUT_FLAG_FAST;
    }
#endif

#ifdef RECORD_PLAY_CONCURRENCY
    char recConcPropValue[PROPERTY_VALUE_MAX];
    bool prop_rec_play_enabled = false;

    if (property_get("rec.playback.conc.disabled", recConcPropValue, NULL)) {
        prop_rec_play_enabled = atoi(recConcPropValue) || !strncmp("true", recConcPropValue, 4);
    }
    if ((prop_rec_play_enabled) &&
            ((true == mIsInputRequestOnProgress) || (activeInputsCount() > 0))) {
        if (AUDIO_MODE_IN_COMMUNICATION == mPhoneState) {
            if (AUDIO_OUTPUT_FLAG_VOIP_RX & flags) {
                // allow VoIP using voice path
                // Do nothing
            } else if((flags & AUDIO_OUTPUT_FLAG_FAST) == 0) {
                ALOGD(" MODE_IN_COMM is setforcing deep buffer output for non ULL... flags: %x", flags);
                // use deep buffer path for all non ULL outputs
                flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
            }
        } else if ((flags & AUDIO_OUTPUT_FLAG_FAST) == 0) {
            ALOGD(" Record mode is on forcing deep buffer output for non ULL... flags: %x ", flags);
            // use deep buffer path for all non ULL outputs
            flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
        }
    }
    if (prop_rec_play_enabled &&
            (stream == AUDIO_STREAM_ENFORCED_AUDIBLE)) {
           ALOGD("Record conc is on forcing ULL output for ENFORCED_AUDIBLE");
           flags = AUDIO_OUTPUT_FLAG_FAST;
    }
#endif
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
    } else if (flags == AUDIO_OUTPUT_FLAG_NONE) {
        //use DEEP_BUFFER as default output for music stream type
        flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    }

    sp<IOProfile> profile;

    // skip direct output selection if the request can obviously be attached to a mixed output
    // and not explicitly requested
    if (((flags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) &&
            audio_is_linear_pcm(format) && samplingRate <= MAX_MIXER_SAMPLING_RATE &&
            audio_channel_count_from_out_mask(channelMask) <= 2) {
        goto non_direct_output;
    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.

    if (((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) == 0) ||
            !isNonOffloadableEffectEnabled()) {
        profile = getProfileForDirectOutput(device,
                                           samplingRate,
                                           format,
                                           channelMask,
                                           (audio_output_flags_t)flags);
    }

    if (profile != 0) {
        sp<AudioOutputDescriptor> outputDesc = NULL;
#ifdef MULTIPLE_OFFLOAD_ENABLED
        bool multiOffloadEnabled = false;
        char value[PROPERTY_VALUE_MAX] = {0};
        property_get("audio.offload.multiple.enabled", value, NULL);
        if (atoi(value) || !strncmp("true", value, 4))
            multiOffloadEnabled = true;
        // if multiple concurrent offload decode is supported
        // do no check for reuse and also don't close previous output if its offload
        // previous output will be closed during track destruction
        if (multiOffloadEnabled && ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0))
            goto get_output__new_output_desc;
#endif
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (profile == desc->mProfile)) {
                outputDesc = desc;
                // reuse direct output if currently open and configured with same parameters
                if ((samplingRate == outputDesc->mSamplingRate) &&
                        (format == outputDesc->mFormat) &&
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
get_output__new_output_desc:
        outputDesc = new AudioOutputDescriptor(profile);
        outputDesc->mDevice = device;
        outputDesc->mLatency = 0;
        outputDesc->mFlags =(audio_output_flags_t) (outputDesc->mFlags | flags);
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        config.sample_rate = samplingRate;
        config.channel_mask = channelMask;
        config.format = format;
        if (offloadInfo != NULL) {
            config.offload_info = *offloadInfo;
        }
        status = mpClientInterface->openOutput(profile->mModule->mHandle,
                                               &output,
                                               &config,
                                               &outputDesc->mDevice,
                                               String8(""),
                                               &outputDesc->mLatency,
                                               outputDesc->mFlags);

        // only accept an output with the requested parameters
        if (status != NO_ERROR ||
            (samplingRate != 0 && samplingRate != config.sample_rate) ||
            (format != AUDIO_FORMAT_DEFAULT && format != config.format) ||
            (channelMask != 0 && channelMask != config.channel_mask)) {
            ALOGV("getOutput() failed opening direct output: output %d samplingRate %d %d,"
                    "format %d %d, channelMask %04x %04x status =%d", output, samplingRate,
                    outputDesc->mSamplingRate, format, outputDesc->mFormat, channelMask,
                    outputDesc->mChannelMask,status);
            if (output != AUDIO_IO_HANDLE_NONE) {
                mpClientInterface->closeOutput(output);
            }
            // fall back to mixer output if possible when the direct output could not be open
            if (audio_is_linear_pcm(format) && samplingRate <= MAX_MIXER_SAMPLING_RATE) {
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
#ifdef DOLBY_DAP
            status_t status = mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, srcOutput, dstOutput);
            if (status == NO_ERROR) {
                for (size_t i = 0; i < mEffects.size(); i++) {
                    sp<EffectDescriptor> desc = mEffects.valueAt(i);
                    if (desc->mSession == AUDIO_SESSION_OUTPUT_MIX) {
                        // update the mIo member of EffectDescriptor for the global effect
                        ALOGV("%s updating mIo", __FUNCTION__);
                        desc->mIo = dstOutput;
                    }
                }
            } else {
                ALOGW("%s moveEffects from %d to %d failed", __FUNCTION__, srcOutput, dstOutput);
            }
#else // DOLBY_END
            mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, srcOutput, dstOutput);
#endif // LINE_ADDED_BY_DOLBY
        }
        mPreviousOutputs = mOutputs;
        ALOGV("getOutput() returns new direct output %d", output);
        mpClientInterface->onAudioPortListUpdate();
        return output;
    }

non_direct_output:

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

    ALOGV("getOutput() returns output %d", output);

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
    // 2: the primary output
    // 3: the first output in the list

    if (outputs.size() == 0) {
        return 0;
    }
    if (outputs.size() == 1) {
        return outputs[0];
    }

    int maxCommonFlags = 0;
    audio_io_handle_t outputFlags = 0;
    audio_io_handle_t outputPrimary = 0;

    for (size_t i = 0; i < outputs.size(); i++) {
        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(outputs[i]);
        if (!outputDesc->isDuplicated()) {
            // if a valid format is specified, skip output if not compatible
            if (format != AUDIO_FORMAT_INVALID) {
                if (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
                    if (format != outputDesc->mFormat) {
                        continue;
                    }
                } else if (!audio_is_linear_pcm(format)) {
                    continue;
                }
            }

            int commonFlags = popcount(outputDesc->mProfile->mFlags & flags);
            if (commonFlags > maxCommonFlags) {
                outputFlags = outputs[i];
                maxCommonFlags = commonFlags;
                ALOGV("selectOutput() commonFlags for output %d, %04x", outputs[i], commonFlags);
            }
            if (outputDesc->mProfile->mFlags & AUDIO_OUTPUT_FLAG_PRIMARY) {
                outputPrimary = outputs[i];
            }
        }
    }

    if (outputFlags != 0) {
        return outputFlags;
    }
    if (outputPrimary != 0) {
        return outputPrimary;
    }

    return outputs[0];
}

status_t AudioPolicyManager::startOutput(audio_io_handle_t output,
                                             audio_stream_type_t stream,
                                             audio_session_t session)
{
    ALOGV("startOutput() output %d, stream %d, session %d", output, stream, session);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("startOutput() unknown output %d", output);
        return BAD_VALUE;
    }

    // cannot start playback of STREAM_TTS if any other output is being used
    uint32_t beaconMuteLatency = 0;
    if (stream == AUDIO_STREAM_TTS) {
        ALOGV("\t found BEACON stream");
        if (isAnyOutputActive(AUDIO_STREAM_TTS /*streamToIgnore*/)) {
            return INVALID_OPERATION;
        } else {
            beaconMuteLatency = handleEventForBeacon(STARTING_BEACON);
        }
    } else {
        // some playback other than beacon starts
        beaconMuteLatency = handleEventForBeacon(STARTING_OUTPUT);
    }

    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);

    // increment usage count for this stream on the requested output:
    // NOTE that the usage count is the same for duplicated output and hardware output which is
    // necessary for a correct control of hardware output routing by startOutput() and stopOutput()
    outputDesc->changeRefCount(stream, 1);

    if (outputDesc->mRefCount[stream] == 1) {
        // starting an output being rerouted?
        audio_devices_t newDevice;
        if (outputDesc->mPolicyMix != NULL) {
            newDevice = AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
        } else {
            newDevice = getNewOutputDevice(output, false /*fromCache*/);
        }
        routing_strategy strategy = getStrategy(stream);
        bool shouldWait = (strategy == STRATEGY_SONIFICATION) ||
                            (strategy == STRATEGY_SONIFICATION_RESPECTFUL) ||
                            (beaconMuteLatency > 0);
        uint32_t waitMs = beaconMuteLatency;
#ifdef HDMI_PASSTHROUGH_ENABLED
        newDevice = handleHDMIPassthrough(newDevice, output, stream, strategy);
#endif
        bool force = false;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc != outputDesc) {
                // force a device change if any other output is managed by the same hw
                // module and has a current device selection that differs from selected device.
                // In this case, the audio HAL must receive the new device selection so that it can
                // change the device currently selected by the other active output.
                if (outputDesc->sharesHwModuleWith(desc) &&
                    desc->device() != newDevice) {
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
        uint32_t muteWaitMs = setOutputDevice(output, newDevice, force);

        // handle special case for sonification while in call
        if (isInCall()) {
            handleIncallSonification(stream, true, false);
        }

        // apply volume rules for current stream and device if necessary
        checkAndSetVolume(stream,
                          mStreams[stream].getVolumeIndex(newDevice),
                          output,
                          newDevice);

        // update the outputs if starting an output with a stream that can affect notification
        // routing
        handleNotificationRoutingForStream(stream);

        // Automatically enable the remote submix input when output is started on a re routing mix
        // of type MIX_TYPE_RECORDERS
        if (audio_is_remote_submix_device(newDevice) && outputDesc->mPolicyMix != NULL &&
                outputDesc->mPolicyMix->mMixType == MIX_TYPE_RECORDERS) {
                setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        outputDesc->mPolicyMix->mRegistrationId);
        }

        // force reevaluating accessibility routing when ringtone or alarm starts
        if (strategy == STRATEGY_SONIFICATION) {
            mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
        }

        if (waitMs > muteWaitMs) {
            usleep((waitMs - muteWaitMs) * 2 * 1000);
        }
    } else {
        // handle special case for sonification while in call
        if (isInCall()) {
            handleIncallSonification(stream, true, false);
        }
    }
#ifdef DOLBY_UDC
    // It is observed that in some use-cases where multiple outputs are present eg. bluetooth and headphone,
    // the output for particular stream type is decided in this routine. Hence we must call
    // getDeviceForStrategy in order to get the current active output for this stream type and update
    // the dolby system property.
    if (stream == AUDIO_STREAM_MUSIC)
    {
        audio_devices_t audioOutputDevice = getDeviceForStrategy(getStrategy(AUDIO_STREAM_MUSIC), true);
        mDolbyAudioPolicy.setEndpointSystemProperty(audioOutputDevice, mHwModules);
    }
#endif // DOLBY_UDC
#ifdef DOLBY_DAP_MOVE_EFFECT
    // Note: The global effect can't be taken away from the deep-buffered output (source output) if there're still
    //       music playing on the deep-buffered output.
    sp<AudioOutputDescriptor> srcOutputDesc = mOutputs.valueFor(mDolbyAudioPolicy.output());
    if ((stream == AUDIO_STREAM_MUSIC) && mDolbyAudioPolicy.shouldMoveToOutput(output, outputDesc->mFlags) && srcOutputDesc != NULL &&
        ((srcOutputDesc->mFlags & AUDIO_OUTPUT_FLAG_DEEP_BUFFER) == 0 || srcOutputDesc->mRefCount[AUDIO_STREAM_MUSIC] == 0)) {
        status_t status = mpClientInterface->moveEffects(DOLBY_MOVE_EFFECT_SIGNAL, mDolbyAudioPolicy.output(), output);
        if (status == NO_ERROR) {
            mDolbyAudioPolicy.movedToOutput(output);
        }
    }
#endif //DOLBY_END
#ifdef DTS_EAGLE
    audio_devices_t ndev = getDeviceForStrategy(STRATEGY_MEDIA, true);
    audio_devices_t availableOutputDevices = availablePrimaryOutputDevices();
    ALOGV("startOutput: device 0x%x all_devices 0x%x", ndev, availableOutputDevices);
    notify_route_node(ndev, availableOutputDevices);
#endif
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

    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);

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
        if (outputDesc->mRefCount[stream] == 0) {
            // Automatically disable the remote submix input when output is stopped on a
            // re routing mix of type MIX_TYPE_RECORDERS
            if (audio_is_remote_submix_device(outputDesc->mDevice) &&
                    outputDesc->mPolicyMix != NULL &&
                    outputDesc->mPolicyMix->mMixType == MIX_TYPE_RECORDERS) {
                setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                        outputDesc->mPolicyMix->mRegistrationId);
            }

            outputDesc->mStopTime[stream] = systemTime();
            audio_devices_t prevDevice = outputDesc->device();
            audio_devices_t newDevice = getNewOutputDevice(output, false /*fromCache*/);
#ifdef HDMI_PASSTHROUGH_ENABLED
            // Use the stream ref count to check if ringtone/ notification
            // needs to be on speaker. Input stream type should be used only
            // when a particular stream is started. On stop if stream type is
            // used, the device is changed to speaker even when ringtone ends.
            newDevice = handleHDMIPassthrough(newDevice, output);
#endif
            // delay the device switch by twice the latency because stopOutput() is executed when
            // the track stop() command is received and at that time the audio track buffer can
            // still contain data that needs to be drained. The latency only covers the audio HAL
            // and kernel buffers. Also the latency does not always include additional delay in the
            // audio path (audio DSP, CODEC ...)
            setOutputDevice(output, newDevice, false, outputDesc->mLatency*2);

#ifdef HDMI_PASSTHROUGH_ENABLED
            if (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH) {
                checkAndRestoreOutputs();
            }
#endif
            // force restoring the device selection on other active outputs if it differs from the
            // one being selected for this output
            for (size_t i = 0; i < mOutputs.size(); i++) {
                audio_io_handle_t curOutput = mOutputs.keyAt(i);
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
                if (curOutput != output &&
                        desc->isActive() &&
                        outputDesc->sharesHwModuleWith(desc) &&
                        (newDevice != desc->device())) {
                        audio_devices_t dev = getNewOutputDevice(curOutput, false /*fromCache*/);
#ifdef HDMI_PASSTHROUGH_ENABLED
                        dev = handleHDMIPassthrough(dev, curOutput);
#endif
                    uint32_t delayMs;
                    if (dev == prevDevice) {
                        delayMs = 0;
                    } else {
                        delayMs = outputDesc->mLatency*2;
                    }
                    setOutputDevice(curOutput,
                                    dev,
                                    true,
                                    delayMs);
                }
            }
            // update the outputs if stopping one with a stream that can affect notification routing
            handleNotificationRoutingForStream(stream);
        }
        return NO_ERROR;
    } else {
        ALOGW("stopOutput() refcount is already 0 for output %d", output);
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
            mOutputs.removeItem(output);
            mTestOutputs[testIndex] = 0;
        }
        return;
    }
#endif //AUDIO_POLICY_TEST

    sp<AudioOutputDescriptor> desc = mOutputs.valueAt(index);
    if (desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
#ifdef QCOM_DIRECTTRACK
        if (desc->mDirectOpenCount <= 0 &&!(desc -> mFlags &AUDIO_OUTPUT_FLAG_LPA || desc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                desc->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)) {
#else
        if (desc->mDirectOpenCount <= 0) {
#endif
            ALOGW("releaseOutput() invalid open count %d for output %d",
                                                              desc->mDirectOpenCount, output);
            return;
        }
#ifdef QCOM_DIRECTTRACK
         if ((--desc->mDirectOpenCount == 0) && ((desc->mFlags & AUDIO_OUTPUT_FLAG_LPA || desc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                desc->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX))) {
            ALOGV("releaseOutput() closing output");
            closeOutput(output);
        } else
#endif
        if (--desc->mDirectOpenCount == 0) {
            closeOutput(output);
            // If effects where present on the output, audioflinger moved them to the primary
            // output by default: move them back to the appropriate output.
            audio_io_handle_t dstOutput = getOutputForEffect();
            if (dstOutput != mPrimaryOutput) {
#ifdef DOLBY_DAP
                status_t status = mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, mPrimaryOutput, dstOutput);
                if (status == NO_ERROR) {
                    for (size_t i = 0; i < mEffects.size(); i++) {
                        sp<EffectDescriptor> desc = mEffects.valueAt(i);
                        if (desc->mSession == AUDIO_SESSION_OUTPUT_MIX) {
                            // update the mIo member of EffectDescriptor for the global effect
                            ALOGV("%s updating mIo", __FUNCTION__);
                            desc->mIo = dstOutput;
                        }
                    }
                } else {
                    ALOGW("%s moveEffects from %d to %d failed", __FUNCTION__, mPrimaryOutput, dstOutput);
                }
#else // DOLBY_END
                mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, mPrimaryOutput, dstOutput);
#endif // LINE_ADDED_BY_DOLBY
            }
            mpClientInterface->onAudioPortListUpdate();
        }
    }
}


status_t AudioPolicyManager::getInputForAttr(const audio_attributes_t *attr,
                                             audio_io_handle_t *input,
                                             audio_session_t session,
                                             uint32_t samplingRate,
                                             audio_format_t format,
                                             audio_channel_mask_t channelMask,
                                             audio_input_flags_t flags,
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
    bool isSoundTrigger = false;
    audio_source_t inputSource = attr->source;
    audio_source_t halInputSource;
    AudioMix *policyMix = NULL;

    if (inputSource == AUDIO_SOURCE_DEFAULT) {
        inputSource = AUDIO_SOURCE_MIC;
    }
    halInputSource = inputSource;

#ifdef VOICE_CONCURRENCY

    char propValue[PROPERTY_VALUE_MAX];
    bool prop_rec_enabled=false, prop_voip_enabled = false;

    if(property_get("voice.record.conc.disabled", propValue, NULL)) {
        prop_rec_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
    }
    halInputSource = inputSource;

    if(property_get("voice.voip.conc.disabled", propValue, NULL)) {
        prop_voip_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
    }

    if (prop_rec_enabled && mvoice_call_state) {
         //check if voice call is active  / running in background
         //some of VoIP apps(like SIP2SIP call) supports resume of VoIP call when call in progress
         //Need to block input request
        if((AUDIO_MODE_IN_CALL == mPhoneState) ||
           ((AUDIO_MODE_IN_CALL == mPrevPhoneState) &&
             (AUDIO_MODE_IN_COMMUNICATION == mPhoneState)))
        {
            switch(inputSource) {
                case AUDIO_SOURCE_VOICE_UPLINK:
                case AUDIO_SOURCE_VOICE_DOWNLINK:
                case AUDIO_SOURCE_VOICE_CALL:
                    ALOGD("Creating input during incall mode for inputSource: %d ",inputSource);
                break;

                case AUDIO_SOURCE_VOICE_COMMUNICATION:
                    if(prop_voip_enabled) {
                       ALOGD("BLOCKING VoIP request during incall mode for inputSource: %d ",inputSource);
                       return NO_INIT;
                    }
                break;
                default:
                    ALOGD("BLOCKING input during incall mode for inputSource: %d ",inputSource);
                return NO_INIT;
            }
        }
    }//check for VoIP flag
    else if(prop_voip_enabled && mvoice_call_state) {
         //check if voice call is active  / running in background
         //some of VoIP apps(like SIP2SIP call) supports resume of VoIP call when call in progress
         //Need to block input request
        if((AUDIO_MODE_IN_CALL == mPhoneState) ||
           ((AUDIO_MODE_IN_CALL == mPrevPhoneState) &&
             (AUDIO_MODE_IN_COMMUNICATION == mPhoneState)))
        {
            if(inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                ALOGD("BLOCKING VoIP request during incall mode for inputSource: %d ",inputSource);
                return NO_INIT;
            }
        }
    }

#endif
    if (inputSource == AUDIO_SOURCE_REMOTE_SUBMIX &&
        strncmp(attr->tags, "addr=", strlen("addr=")) == 0) {
        device = AUDIO_DEVICE_IN_REMOTE_SUBMIX;
        address = String8(attr->tags + strlen("addr="));
        ssize_t index = mPolicyMixes.indexOfKey(address);
        if (index < 0) {
            ALOGW("getInputForAttr() no policy for address %s", address.string());
            return BAD_VALUE;
        }
        if (mPolicyMixes[index]->mMix.mMixType != MIX_TYPE_PLAYERS) {
            ALOGW("getInputForAttr() bad policy mix type for address %s", address.string());
            return BAD_VALUE;
        }
        policyMix = &mPolicyMixes[index]->mMix;
        *inputType = API_INPUT_MIX_EXT_POLICY_REROUTE;
    } else {
        device = getDeviceAndMixForInputSource(inputSource, &policyMix);
        if (device == AUDIO_DEVICE_NONE) {
            ALOGW("getInputForAttr() could not find device for source %d", inputSource);
            return BAD_VALUE;
        }
        // block request to open input on USB during voice call
        if((AUDIO_MODE_IN_CALL == mPhoneState) &&
            (device == AUDIO_DEVICE_IN_USB_DEVICE)) {
            ALOGV("getInputForAttr(): blocking the request to open input on USB device");
            return BAD_VALUE;
        }
        if (policyMix != NULL) {
            address = policyMix->mRegistrationId;
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

        } else {
            *inputType = API_INPUT_LEGACY;
        }
#ifdef QCOM_DIRECTTRACK
        // adapt channel selection to input source
        switch (inputSource) {
        case AUDIO_SOURCE_VOICE_UPLINK:
            channelMask |= AUDIO_CHANNEL_IN_VOICE_UPLINK;
            break;
        case AUDIO_SOURCE_VOICE_DOWNLINK:
            channelMask |= AUDIO_CHANNEL_IN_VOICE_DNLINK;
            break;
        case AUDIO_SOURCE_VOICE_CALL:
            channelMask |= AUDIO_CHANNEL_IN_VOICE_UPLINK | AUDIO_CHANNEL_IN_VOICE_DNLINK;
            break;
        default:
            break;
        }
#endif
        if (inputSource == AUDIO_SOURCE_HOTWORD) {
            ssize_t index = mSoundTriggerSessions.indexOfKey(session);
            if (index >= 0) {
                *input = mSoundTriggerSessions.valueFor(session);
                isSoundTrigger = true;
                flags = (audio_input_flags_t)(flags | AUDIO_INPUT_FLAG_HW_HOTWORD);
                ALOGV("SoundTrigger capture on session %d input %d", session, *input);
            } else {
                halInputSource = AUDIO_SOURCE_VOICE_RECOGNITION;
            }
        }
    }

    sp<IOProfile> profile = getInputProfile(device, address,
                                            samplingRate, format, channelMask,
                                            flags);
    if (profile == 0) {
        //retry without flags
        audio_input_flags_t log_flags = flags;
        flags = AUDIO_INPUT_FLAG_NONE;
        profile = getInputProfile(device, address,
                                  samplingRate, format, channelMask,
                                  flags);
        if (profile == 0) {
            ALOGW("getInputForAttr() could not find profile for device 0x%X, samplingRate %u,"
                    "format %#x, channelMask 0x%X, flags %#x",
                    device, samplingRate, format, channelMask, log_flags);
            return BAD_VALUE;
        }
    }

    if (profile->mModule->mHandle == 0) {
        ALOGE("getInputForAttr(): HW module %s not opened", profile->mModule->mName);
        return NO_INIT;
    }

    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
    config.sample_rate = samplingRate;
    config.channel_mask = channelMask;
    config.format = format;

    status_t status = mpClientInterface->openInput(profile->mModule->mHandle,
                                                   input,
                                                   &config,
                                                   &device,
                                                   address,
                                                   halInputSource,
                                                   flags);

    // only accept input with the exact requested set of parameters
    if (status != NO_ERROR || *input == AUDIO_IO_HANDLE_NONE ||
        (samplingRate != config.sample_rate) ||
        (format != config.format) ||
        (channelMask != config.channel_mask)) {
        ALOGW("getInputForAttr() failed opening input: samplingRate %d, format %d, channelMask %x",
                samplingRate, format, channelMask);
        if (*input != AUDIO_IO_HANDLE_NONE) {
            mpClientInterface->closeInput(*input);
        }
        return BAD_VALUE;
    }

    sp<AudioInputDescriptor> inputDesc = new AudioInputDescriptor(profile);
    inputDesc->mInputSource = inputSource;
    inputDesc->mRefCount = 0;
    inputDesc->mOpenRefCount = 1;
    inputDesc->mSamplingRate = samplingRate;
    inputDesc->mFormat = format;
    inputDesc->mChannelMask = channelMask;
    inputDesc->mDevice = device;
    inputDesc->mSessions.add(session);
    inputDesc->mIsSoundTrigger = isSoundTrigger;
    inputDesc->mPolicyMix = policyMix;

    ALOGV("getInputForAttr() returns input type = %d", *inputType);

    addInput(*input, inputDesc);
    mpClientInterface->onAudioPortListUpdate();
    return NO_ERROR;
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

    index = inputDesc->mSessions.indexOf(session);
    if (index < 0) {
        ALOGW("startInput() unknown session %d on input %d", session, input);
        return BAD_VALUE;
    }

    // virtual input devices are compatible with other input devices
    if (!isVirtualInputDevice(inputDesc->mDevice)) {

        // for a non-virtual input device, check if there is another (non-virtual) active input
        audio_io_handle_t activeInput = getActiveInput();
        if (activeInput != 0 && activeInput != input) {

            // If the already active input uses AUDIO_SOURCE_HOTWORD then it is closed,
            // otherwise the active input continues and the new input cannot be started.
            sp<AudioInputDescriptor> activeDesc = mInputs.valueFor(activeInput);
            if (activeDesc->mInputSource == AUDIO_SOURCE_HOTWORD) {
                ALOGW("startInput(%d) preempting low-priority input %d", input, activeInput);
                stopInput(activeInput, activeDesc->mSessions.itemAt(0));
                releaseInput(activeInput, activeDesc->mSessions.itemAt(0));
            } else {
                ALOGE("startInput(%d) failed: other input %d already started", input, activeInput);
                return INVALID_OPERATION;
            }
        }
    }

#ifdef RECORD_PLAY_CONCURRENCY
    mIsInputRequestOnProgress = true;

    char getPropValue[PROPERTY_VALUE_MAX];
    bool prop_rec_play_enabled = false;

    if (property_get("rec.playback.conc.disabled", getPropValue, NULL)) {
        prop_rec_play_enabled = atoi(getPropValue) || !strncmp("true", getPropValue, 4);
    }

    if ((prop_rec_play_enabled) &&(activeInputsCount() == 0)){
        // send update to HAL on record playback concurrency
        AudioParameter param = AudioParameter();
        param.add(String8("rec_play_conc_on"), String8("true"));
        ALOGD("startInput() setParameters rec_play_conc is setting to ON ");
        mpClientInterface->setParameters(0, param.toString());

        // Call invalidate to reset all opened non ULL audio tracks
        // Move tracks associated to this strategy from previous output to new output
        for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
            // Do not call invalidate for ENFORCED_AUDIBLE (otherwise pops are seen for camcorder)
            if ((i != AUDIO_STREAM_ENFORCED_AUDIBLE) && (i != AUDIO_STREAM_PATCH)) {
               ALOGD("Invalidate on releaseInput for stream :: %d ", i);
               //FIXME see fixme on name change
               mpClientInterface->invalidateStream((audio_stream_type_t)i);
            }
        }
        // close compress tracks
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
            if ((outputDesc == NULL) || (outputDesc->mProfile == NULL)) {
               ALOGD("ouput desc / profile is NULL");
               continue;
            }
            if (outputDesc->mProfile->mFlags
                            & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                // close compress  sessions
                ALOGD("calling closeOutput on record conc for COMPRESS output");
                closeOutput(mOutputs.keyAt(i));
            }
        }
    }
#endif

    if (inputDesc->mRefCount == 0) {
        if (activeInputsCount() == 0) {
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
                address = inputDesc->mPolicyMix->mRegistrationId;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                        AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                        address);
            }
        }
    }

    ALOGV("AudioPolicyManager::startInput() input source = %d", inputDesc->mInputSource);

    inputDesc->mRefCount++;
#ifdef RECORD_PLAY_CONCURRENCY
    mIsInputRequestOnProgress = false;
#endif
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

    index = inputDesc->mSessions.indexOf(session);
    if (index < 0) {
        ALOGW("stopInput() unknown session %d on input %d", session, input);
        return BAD_VALUE;
    }

    if (inputDesc->mRefCount == 0) {
        ALOGW("stopInput() input %d already stopped", input);
        return INVALID_OPERATION;
    }

    inputDesc->mRefCount--;
    if (inputDesc->mRefCount == 0) {

        // automatically disable the remote submix output when input is stopped if not
        // used by a policy mix of type MIX_TYPE_RECORDERS
        if (audio_is_remote_submix_device(inputDesc->mDevice)) {
            String8 address = String8("");
            if (inputDesc->mPolicyMix == NULL) {
                address = String8("0");
            } else if (inputDesc->mPolicyMix->mMixType == MIX_TYPE_PLAYERS) {
                address = inputDesc->mPolicyMix->mRegistrationId;
            }
            if (address != "") {
                setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                                         AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                         address);
            }
        }

        resetInputDevice(input);

        if (activeInputsCount() == 0) {
            SoundTrigger::setCaptureState(false);
        }
    }

#ifdef RECORD_PLAY_CONCURRENCY
    char propValue[PROPERTY_VALUE_MAX];
    bool prop_rec_play_enabled = false;

    if (property_get("rec.playback.conc.disabled", propValue, NULL)) {
        prop_rec_play_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
    }

    if ((prop_rec_play_enabled) && (activeInputsCount() == 0)) {

        //send update to HAL on record playback concurrency
        AudioParameter param = AudioParameter();
        param.add(String8("rec_play_conc_on"), String8("false"));
        ALOGD("stopInput() setParameters rec_play_conc is setting to OFF ");
        mpClientInterface->setParameters(0, param.toString());

        //call invalidate tracks so that any open streams can fall back to deep buffer/compress path from ULL
        for (int i = AUDIO_STREAM_SYSTEM; i < (int)AUDIO_STREAM_CNT; i++) {
            //Do not call invalidate for ENFORCED_AUDIBLE (otherwise pops are seen for camcorder stop tone)
            if ((i != AUDIO_STREAM_ENFORCED_AUDIBLE) && (i != AUDIO_STREAM_PATCH)) {
                ALOGD("Invalidate on stopInput for stream :: %d ", i);
               //FIXME see fixme on name change
               mpClientInterface->invalidateStream((audio_stream_type_t)i);
            }
        }
    }
#endif
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
    sp<AudioInputDescriptor> inputDesc = mInputs.valueAt(index);
    ALOG_ASSERT(inputDesc != 0);

    index = inputDesc->mSessions.indexOf(session);
    if (index < 0) {
        ALOGW("releaseInput() unknown session %d on input %d", session, input);
        return;
    }
    inputDesc->mSessions.remove(session);
    if (inputDesc->mOpenRefCount == 0) {
        ALOGW("releaseInput() invalid open ref count %d", inputDesc->mOpenRefCount);
        return;
    }
    inputDesc->mOpenRefCount--;
    if (inputDesc->mOpenRefCount > 0) {
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
        ssize_t patch_index = mAudioPatches.indexOfKey(inputDesc->mPatchHandle);
        if (patch_index >= 0) {
            sp<AudioPatch> patchDesc = mAudioPatches.valueAt(patch_index);
            status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
            mAudioPatches.removeItemsAt(patch_index);
            patchRemoved = true;
        }
        if ((inputDesc->mIsSoundTrigger) && (mInputs.size() == 1)) {
            ALOGD("Do not close sound trigger input handle");
        } else {
            mpClientInterface->closeInput(mInputs.keyAt(input_index));
            mInputs.removeItem(mInputs.keyAt(input_index));
        }
    }
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
    if (indexMin < 0 || indexMin >= indexMax) {
        ALOGW("initStreamVolume() invalid index limits for stream %d, min %d, max %d", stream , indexMin, indexMax);
        return;
    }
    mStreams[stream].mIndexMin = indexMin;
    mStreams[stream].mIndexMax = indexMax;
    //FIXME: AUDIO_STREAM_ACCESSIBILITY volume follows AUDIO_STREAM_MUSIC for now
    if (stream == AUDIO_STREAM_MUSIC) {
        mStreams[AUDIO_STREAM_ACCESSIBILITY].mIndexMin = indexMin;
        mStreams[AUDIO_STREAM_ACCESSIBILITY].mIndexMax = indexMax;
    }
}

status_t AudioPolicyManager::setStreamVolumeIndex(audio_stream_type_t stream,
                                                      int index,
                                                      audio_devices_t device)
{

    if ((index < mStreams[stream].mIndexMin) || (index > mStreams[stream].mIndexMax)) {
        return BAD_VALUE;
    }
    if (!audio_is_output_device(device)) {
        return BAD_VALUE;
    }

    // Force max volume if stream cannot be muted
    if (!mStreams[stream].mCanBeMuted) index = mStreams[stream].mIndexMax;

    ALOGV("setStreamVolumeIndex() stream %d, device %04x, index %d",
          stream, device, index);

    // if device is AUDIO_DEVICE_OUT_DEFAULT set default value and
    // clear all device specific values
    if (device == AUDIO_DEVICE_OUT_DEFAULT) {
        mStreams[stream].mIndexCur.clear();
    }
    mStreams[stream].mIndexCur.add(device, index);

    // update volume on all outputs whose current device is also selected by the same
    // strategy as the device specified by the caller
    audio_devices_t strategyDevice = getDeviceForStrategy(getStrategy(stream), true /*fromCache*/);


    //FIXME: AUDIO_STREAM_ACCESSIBILITY volume follows AUDIO_STREAM_MUSIC for now
    audio_devices_t accessibilityDevice = AUDIO_DEVICE_NONE;
    if (stream == AUDIO_STREAM_MUSIC) {
        mStreams[AUDIO_STREAM_ACCESSIBILITY].mIndexCur.add(device, index);
        accessibilityDevice = getDeviceForStrategy(STRATEGY_ACCESSIBILITY, true /*fromCache*/);
    }
    if ((device != AUDIO_DEVICE_OUT_DEFAULT) &&
            (device & (strategyDevice | accessibilityDevice)) == 0) {
        return NO_ERROR;
    }
    status_t status = NO_ERROR;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_devices_t curDevice =
                getDeviceForVolume(mOutputs.valueAt(i)->device());

#ifdef AUDIO_EXTN_FM_ENABLED
        audio_devices_t availableOutputDeviceTypes = mAvailableOutputDevices.types();
        if (((device == AUDIO_DEVICE_OUT_DEFAULT) &&
              ((availableOutputDeviceTypes & AUDIO_DEVICE_OUT_FM) != AUDIO_DEVICE_OUT_FM)) ||
              ((curDevice & strategyDevice) != 0)) {
#else
        if ((device == AUDIO_DEVICE_OUT_DEFAULT) || ((curDevice & strategyDevice) != 0)) {
#endif
            status_t volStatus = checkAndSetVolume(stream, index, mOutputs.keyAt(i), curDevice);
            if (volStatus != NO_ERROR) {
                status = volStatus;
            }
        }
        if ((device == AUDIO_DEVICE_OUT_DEFAULT) || ((curDevice & accessibilityDevice) != 0)) {
            status_t volStatus = checkAndSetVolume(AUDIO_STREAM_ACCESSIBILITY,
                                                   index, mOutputs.keyAt(i), curDevice);
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
    // if device is AUDIO_DEVICE_OUT_DEFAULT, return volume for device corresponding to
    // the strategy the stream belongs to.
    if (device == AUDIO_DEVICE_OUT_DEFAULT) {
        device = getDeviceForStrategy(getStrategy(stream), true /*fromCache*/);
    }
    device = getDeviceForVolume(device);

    *index =  mStreams[stream].getVolumeIndex(device);
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
        sp<AudioOutputDescriptor> desc = mOutputs.valueFor(outputs[i]);
        ALOGV("selectOutputForEffects outputs[%zu] flags %x", i, desc->mFlags);
        if (((desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0)
#ifdef HDMI_PASSTHROUGH_ENABLED
            && ((desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH) == 0)
#endif
            ) {
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

    if (mTotalEffectsMemory + desc->memoryUsage > getMaxEffectsMemory()) {
        ALOGW("registerEffect() memory limit exceeded for Fx %s, Memory %d KB",
                desc->name, desc->memoryUsage);
        return INVALID_OPERATION;
    }
    mTotalEffectsMemory += desc->memoryUsage;
    ALOGV("registerEffect() effect %s, io %d, strategy %d session %d id %d",
            desc->name, io, strategy, session, id);
    ALOGV("registerEffect() memory %d, total memory %d", desc->memoryUsage, mTotalEffectsMemory);

    sp<EffectDescriptor> effectDesc = new EffectDescriptor();
    memcpy (&effectDesc->mDesc, desc, sizeof(effect_descriptor_t));
    effectDesc->mIo = io;
    effectDesc->mStrategy = (routing_strategy)strategy;
    effectDesc->mSession = session;
    effectDesc->mEnabled = false;

    mEffects.add(id, effectDesc);

#ifdef DOLBY_DAP_MOVE_EFFECT
    mDolbyAudioPolicy.effectRegistered(effectDesc);
#endif // DOLBY_END
    return NO_ERROR;
}

status_t AudioPolicyManager::unregisterEffect(int id)
{
    ssize_t index = mEffects.indexOfKey(id);
    if (index < 0) {
        ALOGW("unregisterEffect() unknown effect ID %d", id);
        return INVALID_OPERATION;
    }

    sp<EffectDescriptor> effectDesc = mEffects.valueAt(index);

    setEffectEnabled(effectDesc, false);

    if (mTotalEffectsMemory < effectDesc->mDesc.memoryUsage) {
        ALOGW("unregisterEffect() memory %d too big for total %d",
                effectDesc->mDesc.memoryUsage, mTotalEffectsMemory);
        effectDesc->mDesc.memoryUsage = mTotalEffectsMemory;
    }
    mTotalEffectsMemory -= effectDesc->mDesc.memoryUsage;
    ALOGV("unregisterEffect() effect %s, ID %d, memory %d total memory %d",
            effectDesc->mDesc.name, id, effectDesc->mDesc.memoryUsage, mTotalEffectsMemory);

    mEffects.removeItem(id);
#ifdef DOLBY_DAP_MOVE_EFFECT
    mDolbyAudioPolicy.effectRemoved(effectDesc);
#endif // DOLBY_END

    return NO_ERROR;
}

status_t AudioPolicyManager::setEffectEnabled(int id, bool enabled)
{
    ssize_t index = mEffects.indexOfKey(id);
    if (index < 0) {
        ALOGW("unregisterEffect() unknown effect ID %d", id);
        return INVALID_OPERATION;
    }

    return setEffectEnabled(mEffects.valueAt(index), enabled);
}

status_t AudioPolicyManager::setEffectEnabled(const sp<EffectDescriptor>& effectDesc, bool enabled)
{
    if (enabled == effectDesc->mEnabled) {
        ALOGV("setEffectEnabled(%s) effect already %s",
             enabled?"true":"false", enabled?"enabled":"disabled");
        return INVALID_OPERATION;
    }

    if (enabled) {
        if (mTotalEffectsCpuLoad + effectDesc->mDesc.cpuLoad > getMaxEffectsCpuLoad()) {
            ALOGW("setEffectEnabled(true) CPU Load limit exceeded for Fx %s, CPU %f MIPS",
                 effectDesc->mDesc.name, (float)effectDesc->mDesc.cpuLoad/10);
            return INVALID_OPERATION;
        }
        mTotalEffectsCpuLoad += effectDesc->mDesc.cpuLoad;
        ALOGV("setEffectEnabled(true) total CPU %d", mTotalEffectsCpuLoad);
    } else {
        if (mTotalEffectsCpuLoad < effectDesc->mDesc.cpuLoad) {
            ALOGW("setEffectEnabled(false) CPU load %d too high for total %d",
                    effectDesc->mDesc.cpuLoad, mTotalEffectsCpuLoad);
            effectDesc->mDesc.cpuLoad = mTotalEffectsCpuLoad;
        }
        mTotalEffectsCpuLoad -= effectDesc->mDesc.cpuLoad;
        ALOGV("setEffectEnabled(false) total CPU %d", mTotalEffectsCpuLoad);
    }
    effectDesc->mEnabled = enabled;
    return NO_ERROR;
}

bool AudioPolicyManager::isNonOffloadableEffectEnabled()
{
    for (size_t i = 0; i < mEffects.size(); i++) {
        sp<EffectDescriptor> effectDesc = mEffects.valueAt(i);
        if (effectDesc->mEnabled && (effectDesc->mStrategy == STRATEGY_MEDIA) &&
                ((effectDesc->mDesc.flags & EFFECT_FLAG_OFFLOAD_SUPPORTED) == 0)) {
            ALOGV("isNonOffloadableEffectEnabled() non offloadable effect %s enabled on session %d",
                  effectDesc->mDesc.name, effectDesc->mSession);
            return true;
        }
    }
    return false;
}

bool AudioPolicyManager::isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
{
    nsecs_t sysTime = systemTime();
    for (size_t i = 0; i < mOutputs.size(); i++) {
        const sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
        if (outputDesc->isStreamActive(stream, inPastMs, sysTime)) {
            return true;
        }
    }
    return false;
}

bool AudioPolicyManager::isStreamActiveRemotely(audio_stream_type_t stream,
                                                    uint32_t inPastMs) const
{
    nsecs_t sysTime = systemTime();
    for (size_t i = 0; i < mOutputs.size(); i++) {
        const sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
        if (((outputDesc->device() & APM_AUDIO_OUT_DEVICE_REMOTE_ALL) != 0) &&
                outputDesc->isStreamActive(stream, inPastMs, sysTime)) {
            // do not consider re routing (when the output is going to a dynamic policy)
            // as "remote playback"
            if (outputDesc->mPolicyMix == NULL) {
                return true;
            }
        }
    }
    return false;
}

bool AudioPolicyManager::isSourceActive(audio_source_t source) const
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = mInputs.valueAt(i);
        if (inputDescriptor->mRefCount == 0) {
            continue;
        }
        if (inputDescriptor->mInputSource == (int)source) {
            return true;
        }
        // AUDIO_SOURCE_HOTWORD is equivalent to AUDIO_SOURCE_VOICE_RECOGNITION only if it
        // corresponds to an active capture triggered by a hardware hotword recognition
        if ((source == AUDIO_SOURCE_VOICE_RECOGNITION) &&
                 (inputDescriptor->mInputSource == AUDIO_SOURCE_HOTWORD)) {
            // FIXME: we should not assume that the first session is the active one and keep
            // activity count per session. Same in startInput().
            ssize_t index = mSoundTriggerSessions.indexOfKey(inputDescriptor->mSessions.itemAt(0));
            if (index >= 0) {
                return true;
            }
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
    sp<HwModule> module;
    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (strcmp(AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX, mHwModules[i]->mName) == 0 &&
                mHwModules[i]->mHandle != 0) {
            module = mHwModules[i];
            break;
        }
    }

    if (module == 0) {
        return INVALID_OPERATION;
    }

    ALOGV("registerPolicyMixes() num mixes %d", mixes.size());

    for (size_t i = 0; i < mixes.size(); i++) {
        String8 address = mixes[i].mRegistrationId;
        ssize_t index = mPolicyMixes.indexOfKey(address);
        if (index >= 0) {
            ALOGE("registerPolicyMixes(): mix for address %s already registered", address.string());
            continue;
        }
        audio_config_t outputConfig = mixes[i].mFormat;
        audio_config_t inputConfig = mixes[i].mFormat;
        // NOTE: audio flinger mixer does not support mono output: configure remote submix HAL in
        // stereo and let audio flinger do the channel conversion if needed.
        outputConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        inputConfig.channel_mask = AUDIO_CHANNEL_IN_STEREO;
        module->addOutputProfile(address, &outputConfig,
                                 AUDIO_DEVICE_OUT_REMOTE_SUBMIX, address);
        module->addInputProfile(address, &inputConfig,
                                 AUDIO_DEVICE_IN_REMOTE_SUBMIX, address);
        sp<AudioPolicyMix> policyMix = new AudioPolicyMix();
        policyMix->mMix = mixes[i];
        mPolicyMixes.add(address, policyMix);
        if (mixes[i].mMixType == MIX_TYPE_PLAYERS) {
            setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                                     AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                                     address.string());
        } else {
            setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                                     AUDIO_POLICY_DEVICE_STATE_AVAILABLE,
                                     address.string());
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::unregisterPolicyMixes(Vector<AudioMix> mixes)
{
    sp<HwModule> module;
    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (strcmp(AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX, mHwModules[i]->mName) == 0 &&
                mHwModules[i]->mHandle != 0) {
            module = mHwModules[i];
            break;
        }
    }

    if (module == 0) {
        return INVALID_OPERATION;
    }

    ALOGV("unregisterPolicyMixes() num mixes %d", mixes.size());

    for (size_t i = 0; i < mixes.size(); i++) {
        String8 address = mixes[i].mRegistrationId;
        ssize_t index = mPolicyMixes.indexOfKey(address);
        if (index < 0) {
            ALOGE("unregisterPolicyMixes(): mix for address %s not registered", address.string());
            continue;
        }

        mPolicyMixes.removeItemsAt(index);

        if (getDeviceConnectionState(AUDIO_DEVICE_IN_REMOTE_SUBMIX, address.string()) ==
                                             AUDIO_POLICY_DEVICE_STATE_AVAILABLE)
        {
            setDeviceConnectionStateInt(AUDIO_DEVICE_IN_REMOTE_SUBMIX,
                                     AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                     address.string());
        }

        if (getDeviceConnectionState(AUDIO_DEVICE_OUT_REMOTE_SUBMIX, address.string()) ==
                                             AUDIO_POLICY_DEVICE_STATE_AVAILABLE)
        {
            setDeviceConnectionStateInt(AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
                                     AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                     address.string());
        }
        module->removeOutputProfile(address);
        module->removeInputProfile(address);
    }
    return NO_ERROR;
}


status_t AudioPolicyManager::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "\nAudioPolicyManager Dump: %p\n", this);
    result.append(buffer);

    snprintf(buffer, SIZE, " Primary Output: %d\n", mPrimaryOutput);
    result.append(buffer);
    snprintf(buffer, SIZE, " Phone state: %d\n", mPhoneState);
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for communications %d\n",
             mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION]);
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for media %d\n", mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA]);
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for record %d\n", mForceUse[AUDIO_POLICY_FORCE_FOR_RECORD]);
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for dock %d\n", mForceUse[AUDIO_POLICY_FORCE_FOR_DOCK]);
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for system %d\n", mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM]);
    result.append(buffer);
    snprintf(buffer, SIZE, " Force use for hdmi system audio %d\n",
            mForceUse[AUDIO_POLICY_FORCE_FOR_HDMI_SYSTEM_AUDIO]);
    result.append(buffer);

    snprintf(buffer, SIZE, " Available output devices:\n");
    result.append(buffer);
    write(fd, result.string(), result.size());
    for (size_t i = 0; i < mAvailableOutputDevices.size(); i++) {
        mAvailableOutputDevices[i]->dump(fd, 2, i);
    }
    snprintf(buffer, SIZE, "\n Available input devices:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < mAvailableInputDevices.size(); i++) {
        mAvailableInputDevices[i]->dump(fd, 2, i);
    }

    snprintf(buffer, SIZE, "\nHW Modules dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < mHwModules.size(); i++) {
        snprintf(buffer, SIZE, "- HW Module %zu:\n", i + 1);
        write(fd, buffer, strlen(buffer));
        mHwModules[i]->dump(fd);
    }

    snprintf(buffer, SIZE, "\nOutputs dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < mOutputs.size(); i++) {
        snprintf(buffer, SIZE, "- Output %d dump:\n", mOutputs.keyAt(i));
        write(fd, buffer, strlen(buffer));
        mOutputs.valueAt(i)->dump(fd);
    }

    snprintf(buffer, SIZE, "\nInputs dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < mInputs.size(); i++) {
        snprintf(buffer, SIZE, "- Input %d dump:\n", mInputs.keyAt(i));
        write(fd, buffer, strlen(buffer));
        mInputs.valueAt(i)->dump(fd);
    }

    snprintf(buffer, SIZE, "\nStreams dump:\n");
    write(fd, buffer, strlen(buffer));
    snprintf(buffer, SIZE,
             " Stream  Can be muted  Index Min  Index Max  Index Cur [device : index]...\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < AUDIO_STREAM_CNT; i++) {
        snprintf(buffer, SIZE, " %02zu      ", i);
        write(fd, buffer, strlen(buffer));
        mStreams[i].dump(fd);
    }

    snprintf(buffer, SIZE, "\nTotal Effects CPU: %f MIPS, Total Effects memory: %d KB\n",
            (float)mTotalEffectsCpuLoad/10, mTotalEffectsMemory);
    write(fd, buffer, strlen(buffer));

    snprintf(buffer, SIZE, "Registered effects:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < mEffects.size(); i++) {
        snprintf(buffer, SIZE, "- Effect %d dump:\n", mEffects.keyAt(i));
        write(fd, buffer, strlen(buffer));
        mEffects.valueAt(i)->dump(fd);
    }

    snprintf(buffer, SIZE, "\nAudio Patches:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < mAudioPatches.size(); i++) {
        mAudioPatches[i]->dump(fd, 2, i);
    }

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

#ifdef QCOM_DIRECTTRACK
return false;
#endif
#ifdef VOICE_CONCURRENCY
    char concpropValue[PROPERTY_VALUE_MAX];
    if (property_get("voice.playback.conc.disabled", concpropValue, NULL)) {
         bool propenabled = atoi(concpropValue) || !strncmp("true", concpropValue, 4);
         if (propenabled) {
            if (isInCall())
            {
                ALOGD("\n copl: blocking  compress offload on call mode\n");
                return false;
            }
         }
    }
#endif
#ifdef RECORD_PLAY_CONCURRENCY
    char recConcPropValue[PROPERTY_VALUE_MAX];
    bool prop_rec_play_enabled = false;

    if (property_get("rec.playback.conc.disabled", recConcPropValue, NULL)) {
        prop_rec_play_enabled = atoi(recConcPropValue) || !strncmp("true", recConcPropValue, 4);
    }

    if ((prop_rec_play_enabled) &&
         ((true == mIsInputRequestOnProgress) || (activeInputsCount() > 0))) {
        ALOGD("copl: blocking  compress offload for record concurrency");
        return false;
    }
#endif
    // Check if stream type is music, then only allow offload as of now.
    if (offloadInfo.stream_type != AUDIO_STREAM_MUSIC)
    {
        ALOGE("isOffloadSupported: stream_type != MUSIC, returning false");
        return false;
    }

    char propValue[PROPERTY_VALUE_MAX];
    bool pcmOffload = false;

#ifdef ENABLE_AV_ENHANCEMENTS
    if (audio_is_offload_pcm(offloadInfo.format)) {
        bool prop_enabled = false;
        if ((AUDIO_FORMAT_PCM_16_BIT_OFFLOAD == offloadInfo.format) &&
               property_get("audio.offload.pcm.16bit.enable", propValue, NULL)) {
            prop_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
        }
        if ((AUDIO_FORMAT_PCM_24_BIT_OFFLOAD == offloadInfo.format) &&
               property_get("audio.offload.pcm.24bit.enable", propValue, NULL)) {
            prop_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
        }

        if (prop_enabled) {
            ALOGI("PCM offload property is enabled");
            pcmOffload = true;
        }

        if (!pcmOffload) {
            ALOGV("system property not enabled for PCM offload format[%x]",offloadInfo.format);
            return false;
        }
    }
#endif

    if (!pcmOffload) {
        // Check if offload has been disabled
        if (property_get("audio.offload.disable", propValue, "0")) {
            if (atoi(propValue) != 0) {
                ALOGD("offload disabled by audio.offload.disable=%s", propValue );
                return false;
            }
        }

        //check if it's multi-channel AAC (includes sub formats) and FLAC format
        if ((popcount(offloadInfo.channel_mask) > 2) &&
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_FLAC))) {
            ALOGD("offload disabled for multi-channel AAC and FLAC format");
            return false;
        }

        if (offloadInfo.has_video)
        {
            if(property_get("av.offload.enable", propValue, NULL)) {
                bool prop_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
                if (!prop_enabled) {
                    ALOGW("offload disabled by av.offload.enable = %s ", propValue );
                    return false;
                }
            } else {
                return false;
            }

            if(offloadInfo.is_streaming) {
                if (property_get("av.streaming.offload.enable", propValue, NULL)) {
                    bool prop_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
                    if (!prop_enabled) {
                       ALOGW("offload disabled by av.streaming.offload.enable = %s ", propValue );
                       return false;
                    }
                } else {
                    //Do not offload AV streamnig if the property is not defined
                    return false;
                }
            }
            ALOGD("isOffloadSupported: has_video == true, property\
                    set to enable offload");
        }
    }

    //If duration is less than minimum value defined in property, return false
    if (property_get("audio.offload.min.duration.secs", propValue, NULL)) {
        if (offloadInfo.duration_us < (atoi(propValue) * 1000000 )) {
            ALOGV("Offload denied by duration < audio.offload.min.duration.secs(=%s)", propValue);
            return false;
        }
    } else if (offloadInfo.duration_us < OFFLOAD_DEFAULT_MIN_DURATION_SECS * 1000000) {
        ALOGV("Offload denied by duration < default min(=%u)", OFFLOAD_DEFAULT_MIN_DURATION_SECS);
        //duration checks only valid for MP3/AAC formats,
        //do not check duration for other audio formats, e.g. dolby AAC/AC3 and amrwb+ formats
        if ((offloadInfo.format == AUDIO_FORMAT_MP3) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_FLAC) ||
            pcmOffload)
            return false;
    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    if (isNonOffloadableEffectEnabled()) {
        return false;
    }

    // Check for soundcard status
    String8 valueStr = mpClientInterface->getParameters((audio_io_handle_t)0,
                                String8("SND_CARD_STATUS"));
    AudioParameter result = AudioParameter(valueStr);
    int isonline = 0;
    if ((result.getInt(String8("SND_CARD_STATUS"), isonline) == NO_ERROR)
           && !isonline) {
        ALOGD("copl: soundcard is offline rejecting offload request");
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
        if (role == AUDIO_PORT_ROLE_SINK || role == AUDIO_PORT_ROLE_NONE) {
            for (size_t i = 0;
                    i  < mAvailableOutputDevices.size() && portsWritten < portsMax; i++) {
                mAvailableOutputDevices[i]->toAudioPort(&ports[portsWritten++]);
            }
            *num_ports += mAvailableOutputDevices.size();
        }
        if (role == AUDIO_PORT_ROLE_SOURCE || role == AUDIO_PORT_ROLE_NONE) {
            for (size_t i = 0;
                    i  < mAvailableInputDevices.size() && portsWritten < portsMax; i++) {
                mAvailableInputDevices[i]->toAudioPort(&ports[portsWritten++]);
            }
            *num_ports += mAvailableInputDevices.size();
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

sp<AudioPolicyManager::AudioOutputDescriptor> AudioPolicyManager::getOutputFromId(
                                                                    audio_port_handle_t id) const
{
    sp<AudioOutputDescriptor> outputDesc = NULL;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        outputDesc = mOutputs.valueAt(i);
        if (outputDesc->mId == id) {
            break;
        }
    }
    return outputDesc;
}

sp<AudioPolicyManager::AudioInputDescriptor> AudioPolicyManager::getInputFromId(
                                                                    audio_port_handle_t id) const
{
    sp<AudioInputDescriptor> inputDesc = NULL;
    for (size_t i = 0; i < mInputs.size(); i++) {
        inputDesc = mInputs.valueAt(i);
        if (inputDesc->mId == id) {
            break;
        }
    }
    return inputDesc;
}

sp <AudioPolicyManager::HwModule> AudioPolicyManager::getModuleForDevice(
                                                                    audio_devices_t device) const
{
    sp <HwModule> module;

    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (mHwModules[i]->mHandle == 0) {
            continue;
        }
        if (audio_is_output_device(device)) {
            for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
            {
                if (mHwModules[i]->mOutputProfiles[j]->mSupportedDevices.types() & device) {
                    return mHwModules[i];
                }
            }
        } else {
            for (size_t j = 0; j < mHwModules[i]->mInputProfiles.size(); j++) {
                if (mHwModules[i]->mInputProfiles[j]->mSupportedDevices.types() &
                        device & ~AUDIO_DEVICE_BIT_IN) {
                    return mHwModules[i];
                }
            }
        }
    }
    return module;
}

sp <AudioPolicyManager::HwModule> AudioPolicyManager::getModuleFromName(const char *name) const
{
    sp <HwModule> module;

    for (size_t i = 0; i < mHwModules.size(); i++)
    {
        if (strcmp(mHwModules[i]->mName, name) == 0) {
            return mHwModules[i];
        }
    }
    return module;
}

audio_devices_t AudioPolicyManager::availablePrimaryOutputDevices()
{
    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(mPrimaryOutput);
    audio_devices_t devices = outputDesc->mProfile->mSupportedDevices.types();
    return devices & mAvailableOutputDevices.types();
}

audio_devices_t AudioPolicyManager::availablePrimaryInputDevices()
{
    audio_module_handle_t primaryHandle =
                                mOutputs.valueFor(mPrimaryOutput)->mProfile->mModule->mHandle;
    audio_devices_t devices = AUDIO_DEVICE_NONE;
    for (size_t i = 0; i < mAvailableInputDevices.size(); i++) {
        if (mAvailableInputDevices[i]->mModule->mHandle == primaryHandle) {
            devices |= mAvailableInputDevices[i]->mDeviceType;
        }
    }
    return devices;
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
        ALOGV("createAudioPatch sink %d: id %d role %d type %d", i, patch->sinks[i].id,
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
        *handle = 0;
    }

    if (patch->sources[0].type == AUDIO_PORT_TYPE_MIX) {
        sp<AudioOutputDescriptor> outputDesc = getOutputFromId(patch->sources[0].id);
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

            if (!outputDesc->mProfile->isCompatibleProfile(devDesc->mDeviceType,
                                                           devDesc->mAddress,
                                                           patch->sources[0].sample_rate,
                                                         NULL,  // updatedSamplingRate
                                                         patch->sources[0].format,
                                                         patch->sources[0].channel_mask,
                                                         AUDIO_OUTPUT_FLAG_NONE /*FIXME*/)) {
                ALOGV("createAudioPatch() profile not supported for device %08x",
                      devDesc->mDeviceType);
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
        setOutputDevice(outputDesc->mIoHandle, devices.types(), true, 0, handle);
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
            sp<AudioInputDescriptor> inputDesc = getInputFromId(patch->sinks[0].id);
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

            if (!inputDesc->mProfile->isCompatibleProfile(devDesc->mDeviceType,
                                                          devDesc->mAddress,
                                                          patch->sinks[0].sample_rate,
                                                          NULL, /*updatedSampleRate*/
                                                          patch->sinks[0].format,
                                                          patch->sinks[0].channel_mask,
                                                          // FIXME for the parameter type,
                                                          // and the NONE
                                                          (audio_output_flags_t)
                                                            AUDIO_INPUT_FLAG_NONE)) {
                return INVALID_OPERATION;
            }
            // TODO: reconfigure output format and channels here
            ALOGV("createAudioPatch() setting device %08x on output %d",
                                                  devDesc->mDeviceType, inputDesc->mIoHandle);
            setInputDevice(inputDesc->mIoHandle, devDesc->mDeviceType, true, handle);
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

                if (srcDeviceDesc->mModule != sinkDeviceDesc->mModule) {
                    // only one sink supported when connected devices across HW modules
                    if (patch->num_sinks > 1) {
                        return INVALID_OPERATION;
                    }
                    SortedVector<audio_io_handle_t> outputs =
                                            getOutputsForDevice(sinkDeviceDesc->mDeviceType,
                                                                mOutputs);
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
                    patchDesc = new AudioPatch((audio_patch_handle_t)nextUniqueId(),
                                               &newPatch, uid);
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
        sp<AudioOutputDescriptor> outputDesc = getOutputFromId(patch->sources[0].id);
        if (outputDesc == NULL) {
            ALOGV("releaseAudioPatch() output not found for id %d", patch->sources[0].id);
            return BAD_VALUE;
        }
        //TODO: Audio patch  - do we need to change the device to speaker during passthrough
        //Decision to be taken based on patch code
        setOutputDevice(outputDesc->mIoHandle,
                        getNewOutputDevice(outputDesc->mIoHandle, true /*fromCache*/),
                       true,
                       0,
                       NULL);
    } else if (patch->sources[0].type == AUDIO_PORT_TYPE_DEVICE) {
        if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
            sp<AudioInputDescriptor> inputDesc = getInputFromId(patch->sinks[0].id);
            if (inputDesc == NULL) {
                ALOGV("releaseAudioPatch() input not found for id %d", patch->sinks[0].id);
                return BAD_VALUE;
            }
            setInputDevice(inputDesc->mIoHandle,
                           getNewInputDevice(inputDesc->mIoHandle),
                           true,
                           NULL);
        } else if (patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) {
            audio_patch_handle_t afPatchHandle = patchDesc->mAfPatchHandle;
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
    if (num_patches == NULL || (*num_patches != 0 && patches == NULL) ||
            generation == NULL) {
        return BAD_VALUE;
    }
    ALOGV("listAudioPatches() num_patches %d patches %p available patches %zu",
          *num_patches, patches, mAudioPatches.size());
    if (patches == NULL) {
        *num_patches = 0;
    }

    size_t patchesWritten = 0;
    size_t patchesMax = *num_patches;
    for (size_t i = 0;
            i  < mAudioPatches.size() && patchesWritten < patchesMax; i++) {
        patches[patchesWritten] = mAudioPatches[i]->mPatch;
        patches[patchesWritten++].id = mAudioPatches[i]->mHandle;
        ALOGV("listAudioPatches() patch %zu num_sources %d num_sinks %d",
              i, mAudioPatches[i]->mPatch.num_sources, mAudioPatches[i]->mPatch.num_sinks);
    }
    *num_patches = mAudioPatches.size();

    *generation = curAudioPortGeneration();
    ALOGV("listAudioPatches() got %zu patches needed %d", patchesWritten, *num_patches);
    return NO_ERROR;
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
            sp<AudioOutputDescriptor> outputDesc = getOutputFromId(config->id);
            if (outputDesc == NULL) {
                return BAD_VALUE;
            }
            ALOG_ASSERT(!outputDesc->isDuplicated(),
                        "setAudioPortConfig() called on duplicated output %d",
                        outputDesc->mIoHandle);
            audioPortConfig = outputDesc;
        } else if (config->role == AUDIO_PORT_ROLE_SINK) {
            sp<AudioInputDescriptor> inputDesc = getInputFromId(config->id);
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

void AudioPolicyManager::clearAudioPatches(uid_t uid)
{
    for (ssize_t i = (ssize_t)mAudioPatches.size() - 1; i >= 0; i--)  {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(i);
        if (patchDesc->mUid == uid) {
            releaseAudioPatch(mAudioPatches.keyAt(i), uid);
        }
    }
}

status_t AudioPolicyManager::acquireSoundTriggerSession(audio_session_t *session,
                                       audio_io_handle_t *ioHandle,
                                       audio_devices_t *device)
{
    *session = (audio_session_t)mpClientInterface->newAudioUniqueId();
    *ioHandle = (audio_io_handle_t)mpClientInterface->newAudioUniqueId();
    *device = getDeviceAndMixForInputSource(AUDIO_SOURCE_HOTWORD);

    mSoundTriggerSessions.add(*session, *ioHandle);

    return NO_ERROR;
}

status_t AudioPolicyManager::releaseSoundTriggerSession(audio_session_t session)
{
    ssize_t index = mSoundTriggerSessions.indexOfKey(session);
    if (index < 0) {
        ALOGW("acquireSoundTriggerSession() session %d not registered", session);
        return BAD_VALUE;
    }

    mSoundTriggerSessions.removeItem(session);
    return NO_ERROR;
}

status_t AudioPolicyManager::addAudioPatch(audio_patch_handle_t handle,
                                           const sp<AudioPatch>& patch)
{
    ssize_t index = mAudioPatches.indexOfKey(handle);

    if (index >= 0) {
        ALOGW("addAudioPatch() patch %d already in", handle);
        return ALREADY_EXISTS;
    }
    mAudioPatches.add(handle, patch);
    ALOGV("addAudioPatch() handle %d af handle %d num_sources %d num_sinks %d source handle %d"
            "sink handle %d",
          handle, patch->mAfPatchHandle, patch->mPatch.num_sources, patch->mPatch.num_sinks,
          patch->mPatch.sources[0].id, patch->mPatch.sinks[0].id);
    return NO_ERROR;
}

status_t AudioPolicyManager::removeAudioPatch(audio_patch_handle_t handle)
{
    ssize_t index = mAudioPatches.indexOfKey(handle);

    if (index < 0) {
        ALOGW("removeAudioPatch() patch %d not in", handle);
        return ALREADY_EXISTS;
    }
    ALOGV("removeAudioPatch() handle %d af handle %d", handle,
                      mAudioPatches.valueAt(index)->mAfPatchHandle);
    mAudioPatches.removeItemsAt(index);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------
// AudioPolicyManager
// ----------------------------------------------------------------------------

uint32_t AudioPolicyManager::nextUniqueId()
{
    return android_atomic_inc(&mNextUniqueId);
}

uint32_t AudioPolicyManager::nextAudioPortGeneration()
{
    return android_atomic_inc(&mAudioPortGeneration);
}

AudioPolicyManager::AudioPolicyManager(AudioPolicyClientInterface *clientInterface)
    :
#ifdef AUDIO_POLICY_TEST
    Thread(false),
#endif //AUDIO_POLICY_TEST
    mPrimaryOutput((audio_io_handle_t)0),
    mPhoneState(AUDIO_MODE_NORMAL),
    mLimitRingtoneVolume(false), mLastVoiceVolume(-1.0f),
    mTotalEffectsCpuLoad(0), mTotalEffectsMemory(0),
    mA2dpSuspended(false),
    mSpeakerDrcEnabled(false), mNextUniqueId(1),
    mAudioPortGeneration(1),
    mBeaconMuteRefCount(0),
    mBeaconPlayingRefCount(0),
    mBeaconMuted(false),
    mHdmiAudioDisabled(false), mHdmiAudioEvent(false),
    mPrevPhoneState(0),
    mPrimarySuspended (0), mFastSuspended(0),
    mMultiChannelSuspended(0)
{
    mUidCached = getuid();
    mpClientInterface = clientInterface;

    for (int i = 0; i < AUDIO_POLICY_FORCE_USE_CNT; i++) {
        mForceUse[i] = AUDIO_POLICY_FORCE_NONE;
    }

    mDefaultOutputDevice = new DeviceDescriptor(String8(""), AUDIO_DEVICE_OUT_SPEAKER);
    if (loadAudioPolicyConfig(AUDIO_POLICY_VENDOR_CONFIG_FILE) != NO_ERROR) {
        if (loadAudioPolicyConfig(AUDIO_POLICY_CONFIG_FILE) != NO_ERROR) {
            ALOGE("could not load audio policy configuration file, setting defaults");
            defaultAudioPolicyConfig();
        }
    }
    // mAvailableOutputDevices and mAvailableInputDevices now contain all attached devices

    // must be done after reading the policy
    initializeVolumeCurves();

    // open all output streams needed to access attached devices
    audio_devices_t outputDeviceTypes = mAvailableOutputDevices.types();
    audio_devices_t inputDeviceTypes = mAvailableInputDevices.types() & ~AUDIO_DEVICE_BIT_IN;
    for (size_t i = 0; i < mHwModules.size(); i++) {
        mHwModules[i]->mHandle = mpClientInterface->loadHwModule(mHwModules[i]->mName);
        if (mHwModules[i]->mHandle == 0) {
            ALOGW("could not open HW module %s", mHwModules[i]->mName);
            continue;
        }
        // open all output streams needed to access attached devices
        // except for direct output streams that are only opened when they are actually
        // required by an app.
        // This also validates mAvailableOutputDevices list
        for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
        {
            const sp<IOProfile> outProfile = mHwModules[i]->mOutputProfiles[j];

            if (outProfile->mSupportedDevices.isEmpty()) {
                ALOGW("Output profile contains no device on module %s", mHwModules[i]->mName);
                continue;
            }

            if ((outProfile->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) {
                continue;
            }
            audio_devices_t profileType = outProfile->mSupportedDevices.types();
            if ((profileType & mDefaultOutputDevice->mDeviceType) != AUDIO_DEVICE_NONE) {
                profileType = mDefaultOutputDevice->mDeviceType;
            } else {
                // chose first device present in mSupportedDevices also part of
                // outputDeviceTypes
                for (size_t k = 0; k  < outProfile->mSupportedDevices.size(); k++) {
                    profileType = outProfile->mSupportedDevices[k]->mDeviceType;
                    if ((profileType & outputDeviceTypes) != 0) {
                        break;
                    }
                }
            }
            if ((profileType & outputDeviceTypes) == 0) {
                continue;
            }
            sp<AudioOutputDescriptor> outputDesc = new AudioOutputDescriptor(outProfile);

            outputDesc->mDevice = profileType;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = outputDesc->mSamplingRate;
            config.channel_mask = outputDesc->mChannelMask;
            config.format = outputDesc->mFormat;
            audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
            status_t status = mpClientInterface->openOutput(outProfile->mModule->mHandle,
                                                            &output,
                                                            &config,
                                                            &outputDesc->mDevice,
                                                            String8(""),
                                                            &outputDesc->mLatency,
                                                            outputDesc->mFlags);

            if (status != NO_ERROR) {
                ALOGW("Cannot open output stream for device %08x on hw module %s",
                      outputDesc->mDevice,
                      mHwModules[i]->mName);
            } else {
                outputDesc->mSamplingRate = config.sample_rate;
                outputDesc->mChannelMask = config.channel_mask;
                outputDesc->mFormat = config.format;

                for (size_t k = 0; k  < outProfile->mSupportedDevices.size(); k++) {
                    audio_devices_t type = outProfile->mSupportedDevices[k]->mDeviceType;
                    ssize_t index =
                            mAvailableOutputDevices.indexOf(outProfile->mSupportedDevices[k]);
                    // give a valid ID to an attached device once confirmed it is reachable
                    if ((index >= 0) && (mAvailableOutputDevices[index]->mId == 0)) {
                        mAvailableOutputDevices[index]->mId = nextUniqueId();
                        mAvailableOutputDevices[index]->mModule = mHwModules[i];
                    }
                }
                if (mPrimaryOutput == 0 &&
                        outProfile->mFlags & AUDIO_OUTPUT_FLAG_PRIMARY) {
                    mPrimaryOutput = output;
                }
                addOutput(output, outputDesc);
                setOutputDevice(output,
                                outputDesc->mDevice,
                                true);
            }
        }
        // open input streams needed to access attached devices to validate
        // mAvailableInputDevices list
        for (size_t j = 0; j < mHwModules[i]->mInputProfiles.size(); j++)
        {
            const sp<IOProfile> inProfile = mHwModules[i]->mInputProfiles[j];

            if (inProfile->mSupportedDevices.isEmpty()) {
                ALOGW("Input profile contains no device on module %s", mHwModules[i]->mName);
                continue;
            }
            // chose first device present in mSupportedDevices also part of
            // inputDeviceTypes
            audio_devices_t profileType = AUDIO_DEVICE_NONE;
            for (size_t k = 0; k  < inProfile->mSupportedDevices.size(); k++) {
                profileType = inProfile->mSupportedDevices[k]->mDeviceType;
                if (profileType & inputDeviceTypes) {
                    break;
                }
            }
            if ((profileType & inputDeviceTypes) == 0) {
                continue;
            }
            sp<AudioInputDescriptor> inputDesc = new AudioInputDescriptor(inProfile);

            inputDesc->mInputSource = AUDIO_SOURCE_MIC;
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
            status_t status = mpClientInterface->openInput(inProfile->mModule->mHandle,
                                                           &input,
                                                           &config,
                                                           &inputDesc->mDevice,
                                                           address,
                                                           AUDIO_SOURCE_MIC,
                                                           AUDIO_INPUT_FLAG_NONE);

            if (status == NO_ERROR) {
                for (size_t k = 0; k  < inProfile->mSupportedDevices.size(); k++) {
                    audio_devices_t type = inProfile->mSupportedDevices[k]->mDeviceType;
                    ssize_t index =
                            mAvailableInputDevices.indexOf(inProfile->mSupportedDevices[k]);
                    // give a valid ID to an attached device once confirmed it is reachable
                    if ((index >= 0) && (mAvailableInputDevices[index]->mId == 0)) {
                        mAvailableInputDevices[index]->mId = nextUniqueId();
                        mAvailableInputDevices[index]->mModule = mHwModules[i];
                    }
                }
                mpClientInterface->closeInput(input);
            } else {
                ALOGW("Cannot open input stream for device %08x on hw module %s",
                      inputDesc->mDevice,
                      mHwModules[i]->mName);
            }
        }
    }
    // make sure all attached devices have been allocated a unique ID
    for (size_t i = 0; i  < mAvailableOutputDevices.size();) {
        if (mAvailableOutputDevices[i]->mId == 0) {
            ALOGW("Input device %08x unreachable", mAvailableOutputDevices[i]->mDeviceType);
            mAvailableOutputDevices.remove(mAvailableOutputDevices[i]);
            continue;
        }
        i++;
    }
    for (size_t i = 0; i  < mAvailableInputDevices.size();) {
        if (mAvailableInputDevices[i]->mId == 0) {
            ALOGW("Input device %08x unreachable", mAvailableInputDevices[i]->mDeviceType);
            mAvailableInputDevices.remove(mAvailableInputDevices[i]);
            continue;
        }
        i++;
    }
    // make sure default device is reachable
    if (mAvailableOutputDevices.indexOf(mDefaultOutputDevice) < 0) {
        ALOGE("Default device %08x is unreachable", mDefaultOutputDevice->mDeviceType);
    }

    ALOGE_IF((mPrimaryOutput == 0), "Failed to open primary output");

    updateDevicesAndOutputs();

    mvoice_call_state = 0;
#ifdef RECORD_PLAY_CONCURRENCY
    mIsInputRequestOnProgress = false;
#endif

#ifdef AUDIO_POLICY_TEST
    if (mPrimaryOutput != 0) {
        AudioParameter outputCmd = AudioParameter();
        outputCmd.addInt(String8("set_id"), 0);
        mpClientInterface->setParameters(mPrimaryOutput, outputCmd.toString());

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
#ifdef DTS_EAGLE
    create_route_node();
#endif
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
#ifdef DTS_EAGLE
    remove_route_node();
#endif
}

status_t AudioPolicyManager::initCheck()
{
    return (mPrimaryOutput == 0) ? NO_INIT : NO_ERROR;
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

                sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(mPrimaryOutput);
                mpClientInterface->closeOutput(mPrimaryOutput);

                audio_module_handle_t moduleHandle = outputDesc->mModule->mHandle;

                mOutputs.removeItem(mPrimaryOutput);

                sp<AudioOutputDescriptor> outputDesc = new AudioOutputDescriptor(NULL);
                outputDesc->mDevice = AUDIO_DEVICE_OUT_SPEAKER;
                audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                config.sample_rate = outputDesc->mSamplingRate;
                config.channel_mask = outputDesc->mChannelMask;
                config.format = outputDesc->mFormat;
                status_t status = mpClientInterface->openOutput(moduleHandle,
                                                                &mPrimaryOutput,
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
                    AudioParameter outputCmd = AudioParameter();
                    outputCmd.addInt(String8("set_id"), 0);
                    mpClientInterface->setParameters(mPrimaryOutput, outputCmd.toString());
                    addOutput(mPrimaryOutput, outputDesc);
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

void AudioPolicyManager::addOutput(audio_io_handle_t output, sp<AudioOutputDescriptor> outputDesc)
{
    outputDesc->mIoHandle = output;
    outputDesc->mId = nextUniqueId();
    mOutputs.add(output, outputDesc);
    nextAudioPortGeneration();
}

void AudioPolicyManager::addInput(audio_io_handle_t input, sp<AudioInputDescriptor> inputDesc)
{
    inputDesc->mIoHandle = input;
    inputDesc->mId = nextUniqueId();
    mInputs.add(input, inputDesc);
    nextAudioPortGeneration();
}

void AudioPolicyManager::findIoHandlesByAddress(sp<AudioOutputDescriptor> desc /*in*/,
        const audio_devices_t device /*in*/,
        const String8 address /*in*/,
        SortedVector<audio_io_handle_t>& outputs /*out*/) {
    sp<DeviceDescriptor> devDesc =
        desc->mProfile->mSupportedDevices.getDevice(device, address);
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
    audio_devices_t device = devDesc->mDeviceType;
    sp<AudioOutputDescriptor> desc;
    // erase all current sample rates, formats and channel masks
    devDesc->clearCapabilities();

    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        // first list already open outputs that can be routed to this device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (desc->mProfile->mSupportedDevices.types() & device)) {
                if (!deviceDistinguishesOnAddress(device)) {
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
                if (profile->mSupportedDevices.types() & device) {
                    if (!deviceDistinguishesOnAddress(device) ||
                            address == profile->mSupportedDevices[0]->mAddress) {
                        profiles.add(profile);
                        ALOGV("checkOutputsForDevice(): adding profile %zu from module %zu", j, i);
                    }
                }
            }
        }

        ALOGV("  found %d profiles, %d outputs", profiles.size(), outputs.size());

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
                    devDesc->importAudioPort(profile);
                    break;
                }
            }
            if (j != outputs.size()) {
                continue;
            }

            ALOGV("opening output for device %08x with params %s profile %p",
                                                      device, address.string(), profile.get());
            desc = new AudioOutputDescriptor(profile);
            desc->mDevice = device;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = desc->mSamplingRate;
            config.channel_mask = desc->mChannelMask;
            config.format = desc->mFormat;
            config.offload_info.sample_rate = desc->mSamplingRate;
            config.offload_info.channel_mask = desc->mChannelMask;
            config.offload_info.format = desc->mFormat;
            audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
#ifdef QCOM_DIRECTTRACK
            if (!(desc->mFlags & AUDIO_OUTPUT_FLAG_LPA || desc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                desc->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)) {
#endif

            status_t status = mpClientInterface->openOutput(profile->mModule->mHandle,
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

                // Here is where we step through and resolve any "dynamic" fields
                String8 reply;
                char *value;
                if (profile->mSamplingRates[0] == 0) {
                    reply = mpClientInterface->getParameters(output,
                                            String8(AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES));
                    ALOGV("checkOutputsForDevice() supported sampling rates %s",
                              reply.string());
                    value = strpbrk((char *)reply.string(), "=");
                    if (value != NULL) {
                        profile->loadSamplingRates(value + 1);
                    }
                }
                if (profile->mFormats[0] == AUDIO_FORMAT_DEFAULT) {
                    reply = mpClientInterface->getParameters(output,
                                                   String8(AUDIO_PARAMETER_STREAM_SUP_FORMATS));
                    ALOGV("checkOutputsForDevice() supported formats %s",
                              reply.string());
                    value = strpbrk((char *)reply.string(), "=");
                    if (value != NULL) {
                        profile->loadFormats(value + 1);
                    }
                }
                if (profile->mChannelMasks[0] == 0) {
                    reply = mpClientInterface->getParameters(output,
                                                  String8(AUDIO_PARAMETER_STREAM_SUP_CHANNELS));
                    ALOGV("checkOutputsForDevice() supported channel masks %s",
                              reply.string());
                    value = strpbrk((char *)reply.string(), "=");
                    if (value != NULL) {
                        profile->loadOutChannels(value + 1);
                    }
                }
                if (((profile->mSamplingRates[0] == 0) &&
                         (profile->mSamplingRates.size() < 2)) ||
                     ((profile->mFormats[0] == AUDIO_FORMAT_DEFAULT) &&
                         (profile->mFormats.size() < 2)) ||
                     ((profile->mChannelMasks[0] == 0) &&
                         (profile->mChannelMasks.size() < 2))) {
                    ALOGW("checkOutputsForDevice() missing param");
                    mpClientInterface->closeOutput(output);
                    output = AUDIO_IO_HANDLE_NONE;
                } else if (profile->mSamplingRates[0] == 0 || profile->mFormats[0] == 0 ||
                            profile->mChannelMasks[0] == 0) {
                    mpClientInterface->closeOutput(output);
                    config.sample_rate = profile->pickSamplingRate();
                    config.channel_mask = profile->pickChannelMask();
                    config.format = profile->pickFormat();
                    config.offload_info.sample_rate = config.sample_rate;
                    config.offload_info.channel_mask = config.channel_mask;
                    config.offload_info.format = config.format;
                    status = mpClientInterface->openOutput(profile->mModule->mHandle,
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
                    if (deviceDistinguishesOnAddress(device) && address != "0") {
                        ssize_t index = mPolicyMixes.indexOfKey(address);
                        if (index >= 0) {
                            mPolicyMixes[index]->mOutput = desc;
                            desc->mPolicyMix = &mPolicyMixes[index]->mMix;
                        } else {
                            ALOGE("checkOutputsForDevice() cannot find policy for address %s",
                                  address.string());
                        }
                    } else if ((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) {
                        // no duplicated output for direct outputs and
                        // outputs used by dynamic policy mixes
                        audio_io_handle_t duplicatedOutput = AUDIO_IO_HANDLE_NONE;

                        // set initial stream volume for device
                        applyStreamVolumes(output, device, 0, true);

                        //TODO: configure audio effect output stage here

                        // open a duplicating output thread for the new output and the primary output
                        duplicatedOutput = mpClientInterface->openDuplicateOutput(output,
                                                                                  mPrimaryOutput);
                        if (duplicatedOutput != AUDIO_IO_HANDLE_NONE) {
                            // add duplicated output descriptor
                            sp<AudioOutputDescriptor> dupOutputDesc =
                                    new AudioOutputDescriptor(NULL);
                            dupOutputDesc->mOutput1 = mOutputs.valueFor(mPrimaryOutput);
                            dupOutputDesc->mOutput2 = mOutputs.valueFor(output);
                            dupOutputDesc->mSamplingRate = desc->mSamplingRate;
                            dupOutputDesc->mFormat = desc->mFormat;
                            dupOutputDesc->mChannelMask = desc->mChannelMask;
                            dupOutputDesc->mLatency = desc->mLatency;
                            addOutput(duplicatedOutput, dupOutputDesc);
                            applyStreamVolumes(duplicatedOutput, device, 0, true);
                        } else {
                            ALOGW("checkOutputsForDevice() could not open dup output for %d and %d",
                                    mPrimaryOutput, output);
                            mpClientInterface->closeOutput(output);
                            mOutputs.removeItem(output);
                            nextAudioPortGeneration();
                            output = AUDIO_IO_HANDLE_NONE;
                        }
                    }
                }
            } else {
                output = AUDIO_IO_HANDLE_NONE;
            }
#ifdef QCOM_DIRECTTRACK
            }
#endif
            if (output == AUDIO_IO_HANDLE_NONE) {
                ALOGW("checkOutputsForDevice() could not open output for device %x", device);
                profiles.removeAt(profile_index);
                profile_index--;
            } else {
                outputs.add(output);
                devDesc->importAudioPort(profile);

                if (deviceDistinguishesOnAddress(device)) {
                    ALOGV("checkOutputsForDevice(): setOutputDevice(dev=0x%x, addr=%s)",
                            device, address.string());
                    setOutputDevice(output, device, true/*force*/, 0/*delay*/,
                            NULL/*patch handle*/, address.string());
                }
                ALOGV("checkOutputsForDevice(): adding output %d", output);
            }
        }

        if (profiles.isEmpty()) {
            ALOGE("checkOutputsForDevice(): No output available for device %04x", device);
            return BAD_VALUE;
        }
    } else { // Disconnect
        // check if one opened output is not needed any more after disconnecting one device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated()) {
                // exact match on device
                if (deviceDistinguishesOnAddress(device) &&
                        (desc->mProfile->mSupportedDevices.types() == device)) {
                    findIoHandlesByAddress(desc, device, address, outputs);
                } else if (!(desc->mProfile->mSupportedDevices.types()
                        & mAvailableOutputDevices.types())) {
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
                if (profile->mSupportedDevices.types() & device) {
                    ALOGV("checkOutputsForDevice(): "
                            "clearing direct output profile %zu on module %zu", j, i);
                    if (profile->mSamplingRates[0] == 0) {
                        profile->mSamplingRates.clear();
                        profile->mSamplingRates.add(0);
                    }
                    if (profile->mFormats[0] == AUDIO_FORMAT_DEFAULT) {
                        profile->mFormats.clear();
                        profile->mFormats.add(AUDIO_FORMAT_DEFAULT);
                    }
                    if (profile->mChannelMasks[0] == 0) {
                        profile->mChannelMasks.clear();
                        profile->mChannelMasks.add(0);
                    }
                }
            }
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::checkInputsForDevice(audio_devices_t device,
                                                      audio_policy_dev_state_t state,
                                                      SortedVector<audio_io_handle_t>& inputs,
                                                      const String8 address)
{
    sp<AudioInputDescriptor> desc;
    if (state == AUDIO_POLICY_DEVICE_STATE_AVAILABLE) {
        // first list already open inputs that can be routed to this device
        for (size_t input_index = 0; input_index < mInputs.size(); input_index++) {
            desc = mInputs.valueAt(input_index);
            if (desc->mProfile->mSupportedDevices.types() & (device & ~AUDIO_DEVICE_BIT_IN)) {
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

                if (profile->mSupportedDevices.types() & (device & ~AUDIO_DEVICE_BIT_IN)) {
                    if (!deviceDistinguishesOnAddress(device) ||
                            address == profile->mSupportedDevices[0]->mAddress) {
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
            status_t status = mpClientInterface->openInput(profile->mModule->mHandle,
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

                // Here is where we step through and resolve any "dynamic" fields
                String8 reply;
                char *value;
                if (profile->mSamplingRates[0] == 0) {
                    reply = mpClientInterface->getParameters(input,
                                            String8(AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES));
                    ALOGV("checkInputsForDevice() direct input sup sampling rates %s",
                              reply.string());
                    value = strpbrk((char *)reply.string(), "=");
                    if (value != NULL) {
                        profile->loadSamplingRates(value + 1);
                    }
                }
                if (profile->mFormats[0] == AUDIO_FORMAT_DEFAULT) {
                    reply = mpClientInterface->getParameters(input,
                                                   String8(AUDIO_PARAMETER_STREAM_SUP_FORMATS));
                    ALOGV("checkInputsForDevice() direct input sup formats %s", reply.string());
                    value = strpbrk((char *)reply.string(), "=");
                    if (value != NULL) {
                        profile->loadFormats(value + 1);
                    }
                }
                if (profile->mChannelMasks[0] == 0) {
                    reply = mpClientInterface->getParameters(input,
                                                  String8(AUDIO_PARAMETER_STREAM_SUP_CHANNELS));
                    ALOGV("checkInputsForDevice() direct input sup channel masks %s",
                              reply.string());
                    value = strpbrk((char *)reply.string(), "=");
                    if (value != NULL) {
                        profile->loadInChannels(value + 1);
                    }
                }
                if (((profile->mSamplingRates[0] == 0) && (profile->mSamplingRates.size() < 2)) ||
                     ((profile->mFormats[0] == 0) && (profile->mFormats.size() < 2)) ||
                     ((profile->mChannelMasks[0] == 0) && (profile->mChannelMasks.size() < 2))) {
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
//FIXME::  Audio team			
            if (!(desc->mProfile->mSupportedDevices.types() & mAvailableInputDevices.types() &
                    ~AUDIO_DEVICE_BIT_IN)) {
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
                if (profile->mSupportedDevices.types() & device & ~AUDIO_DEVICE_BIT_IN) {
                    ALOGV("checkInputsForDevice(): clearing direct input profile %zu on module %zu",
                          profile_index, module_index);
                    if (profile->mSamplingRates[0] == 0) {
                        profile->mSamplingRates.clear();
                        profile->mSamplingRates.add(0);
                    }
                    if (profile->mFormats[0] == AUDIO_FORMAT_DEFAULT) {
                        profile->mFormats.clear();
                        profile->mFormats.add(AUDIO_FORMAT_DEFAULT);
                    }
                    if (profile->mChannelMasks[0] == 0) {
                        profile->mChannelMasks.clear();
                        profile->mChannelMasks.add(0);
                    }
                }
            }
        }
    } // end disconnect

    return NO_ERROR;
}


void AudioPolicyManager::closeOutput(audio_io_handle_t output)
{
    ALOGV("closeOutput(%d)", output);

    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    if (outputDesc == NULL) {
        ALOGW("closeOutput() unknown output %d", output);
        return;
    }

    for (size_t i = 0; i < mPolicyMixes.size(); i++) {
        if (mPolicyMixes[i]->mOutput == outputDesc) {
            mPolicyMixes[i]->mOutput.clear();
        }
    }

    // look for duplicated outputs connected to the output being removed.
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<AudioOutputDescriptor> dupOutputDesc = mOutputs.valueAt(i);
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
            mOutputs.removeItem(duplicatedOutput);
        }
    }

    nextAudioPortGeneration();

    ssize_t index = mAudioPatches.indexOfKey(outputDesc->mPatchHandle);
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
        mAudioPatches.removeItemsAt(index);
        mpClientInterface->onAudioPatchListUpdate();
    }

#ifdef HDMI_PASSTHROUGH_ENABLED
    if (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH) {
        ALOGV("check and restore output");
        checkAndRestoreOutputs();
    }
#endif

    AudioParameter param;
    param.add(String8("closing"), String8("true"));
    mpClientInterface->setParameters(output, param.toString());

    mpClientInterface->closeOutput(output);
    mOutputs.removeItem(output);
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

    ssize_t index = mAudioPatches.indexOfKey(inputDesc->mPatchHandle);
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
        mAudioPatches.removeItemsAt(index);
        mpClientInterface->onAudioPatchListUpdate();
    }

    mpClientInterface->closeInput(input);
    mInputs.removeItem(input);
}

SortedVector<audio_io_handle_t> AudioPolicyManager::getOutputsForDevice(audio_devices_t device,
                        DefaultKeyedVector<audio_io_handle_t, sp<AudioOutputDescriptor> > openOutputs)
{
    SortedVector<audio_io_handle_t> outputs;

    ALOGVV("getOutputsForDevice() device %04x", device);
    for (size_t i = 0; i < openOutputs.size(); i++) {
        ALOGVV("output %d isDuplicated=%d device=%04x",
                i, openOutputs.valueAt(i)->isDuplicated(), openOutputs.valueAt(i)->supportedDevices());
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
    SortedVector<audio_io_handle_t> srcOutputs = getOutputsForDevice(oldDevice, mOutputs);
    SortedVector<audio_io_handle_t> dstOutputs = getOutputsForDevice(newDevice, mOutputs);

    // also take into account external policy-related changes: add all outputs which are
    // associated with policies in the "before" and "after" output vectors
    ALOGVV("checkOutputForStrategy(): policy related outputs");
    for (size_t i = 0 ; i < mPreviousOutputs.size() ; i++) {
        const sp<AudioOutputDescriptor> desc = mPreviousOutputs.valueAt(i);
        if (desc != 0 && desc->mPolicyMix != NULL) {
            srcOutputs.add(desc->mIoHandle);
            ALOGVV(" previous outputs: adding %d", desc->mIoHandle);
        }
    }
    for (size_t i = 0 ; i < mOutputs.size() ; i++) {
        const sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
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
            sp<AudioOutputDescriptor> desc = mOutputs.valueFor(srcOutputs[i]);
            if (desc->isStrategyActive(strategy)) {
                setStrategyMute(strategy, true, srcOutputs[i]);
                setStrategyMute(strategy, false, srcOutputs[i], MUTE_TIME_MS, newDevice);
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
#ifdef DOLBY_DAP
                        status_t status = mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, effectDesc->mIo,
                                                       fxOutput);
                        if (status != NO_ERROR) {
                            ALOGW("%s moveEffects from %d to %d failed", __FUNCTION__, effectDesc->mIo, fxOutput);
                            continue;
                        }
#else // DOLBY_END
                        mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, effectDesc->mIo,
                                                       fxOutput);
#endif // LINE_ADDED_BY_DOLBY
                        moved.add(effectDesc->mIo);
                    }
                    effectDesc->mIo = fxOutput;
                }
            }
        }
        // Move tracks associated to this strategy from previous output to new output
        for (int i = 0; i < AUDIO_STREAM_CNT; i++) {
            if (i == AUDIO_STREAM_PATCH) {
                continue;
            }
            if (getStrategy((audio_stream_type_t)i) == strategy) {
                mpClientInterface->invalidateStream((audio_stream_type_t)i);
            }
        }
    }
}

void AudioPolicyManager::checkOutputForAllStrategies()
{
    if (mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM] == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED)
        checkOutputForStrategy(STRATEGY_ENFORCED_AUDIBLE);
    checkOutputForStrategy(STRATEGY_PHONE);
    if (mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM] != AUDIO_POLICY_FORCE_SYSTEM_ENFORCED)
        checkOutputForStrategy(STRATEGY_ENFORCED_AUDIBLE);
    checkOutputForStrategy(STRATEGY_SONIFICATION);
    checkOutputForStrategy(STRATEGY_SONIFICATION_RESPECTFUL);
    checkOutputForStrategy(STRATEGY_ACCESSIBILITY);
    checkOutputForStrategy(STRATEGY_MEDIA);
    checkOutputForStrategy(STRATEGY_DTMF);
    checkOutputForStrategy(STRATEGY_REROUTING);
}

audio_io_handle_t AudioPolicyManager::getA2dpOutput()
{
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
        if (!outputDesc->isDuplicated() && outputDesc->device() & AUDIO_DEVICE_OUT_ALL_A2DP) {
            return mOutputs.keyAt(i);
        }
    }
    DeviceVector deviceList = mAvailableOutputDevices.getDevicesFromType(AUDIO_DEVICE_OUT_ALL_A2DP);
        if (!deviceList.isEmpty()) {
             return 1;
        } else
             return 0;
}

void AudioPolicyManager::checkA2dpSuspend()
{
#ifndef QCOM_DIRECTTRACK
    audio_io_handle_t a2dpOutput = getA2dpOutput();
    if (a2dpOutput == 0) {
        mA2dpSuspended = false;
        return;
    }
#endif

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
             ((mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION] != AUDIO_POLICY_FORCE_BT_SCO) &&
              (mForceUse[AUDIO_POLICY_FORCE_FOR_RECORD] != AUDIO_POLICY_FORCE_BT_SCO))) &&
             ((mPhoneState != AUDIO_MODE_IN_CALL) &&
              (mPhoneState != AUDIO_MODE_RINGTONE))) {

#ifndef QCOM_DIRECTTRACK
            mpClientInterface->restoreOutput(a2dpOutput);
#endif
            mA2dpSuspended = false;
        }
    } else {
        if ((isScoConnected &&
             ((mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION] == AUDIO_POLICY_FORCE_BT_SCO) ||
              (mForceUse[AUDIO_POLICY_FORCE_FOR_RECORD] == AUDIO_POLICY_FORCE_BT_SCO))) ||
             ((mPhoneState == AUDIO_MODE_IN_CALL) ||
              (mPhoneState == AUDIO_MODE_RINGTONE))) {

#ifndef QCOM_DIRECTTRACK
            mpClientInterface->suspendOutput(a2dpOutput);
#endif
            mA2dpSuspended = true;
        }
    }
}

audio_devices_t AudioPolicyManager::getNewOutputDevice(audio_io_handle_t output, bool fromCache)
{
    audio_devices_t device = AUDIO_DEVICE_NONE;

    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    sp<AudioOutputDescriptor> primaryOutputDesc = mOutputs.valueFor(mPrimaryOutput);

    ssize_t index = mAudioPatches.indexOfKey(outputDesc->mPatchHandle);
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        if (patchDesc->mUid != mUidCached) {
            ALOGV("getNewOutputDevice() device %08x forced by patch %d",
                  outputDesc->device(), outputDesc->mPatchHandle);
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
    // 5: the strategy "respectful" sonification is active on the output:
    //      use device for strategy "respectful" sonification
    // 6: the strategy accessibility is active on the output:
    //      use device for strategy accessibility
    // 7: the strategy media is active on the output:
    //      use device for strategy media
    // 8: the strategy DTMF is active on the output:
    //      use device for strategy DTMF
    // 9: the strategy for beacon, a.k.a. "transmitted through speaker" is active on the output:
    //      use device for strategy t-t-s
    if (outputDesc->isStrategyActive(STRATEGY_ENFORCED_AUDIBLE) &&
        mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM] == AUDIO_POLICY_FORCE_SYSTEM_ENFORCED) {
        device = getDeviceForStrategy(STRATEGY_ENFORCED_AUDIBLE, fromCache);
    } else if (isInCall() ||
                 outputDesc->isStrategyActive(STRATEGY_PHONE) ||
                 primaryOutputDesc->isStrategyActive(STRATEGY_PHONE)) {
        device = getDeviceForStrategy(STRATEGY_PHONE, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_ENFORCED_AUDIBLE)) {
        device = getDeviceForStrategy(STRATEGY_ENFORCED_AUDIBLE, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_SONIFICATION) ||
                (primaryOutputDesc->isStrategyActive(STRATEGY_SONIFICATION)
                && (!primaryOutputDesc->isStrategyActive(STRATEGY_MEDIA)))) {
        device = getDeviceForStrategy(STRATEGY_SONIFICATION, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_SONIFICATION_RESPECTFUL) ||
               primaryOutputDesc->isStrategyActive(STRATEGY_SONIFICATION_RESPECTFUL)) {
        device = getDeviceForStrategy(STRATEGY_SONIFICATION_RESPECTFUL, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_ACCESSIBILITY)) {
        device = getDeviceForStrategy(STRATEGY_ACCESSIBILITY, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_MEDIA)) {
        device = getDeviceForStrategy(STRATEGY_MEDIA, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_DTMF)) {
        device = getDeviceForStrategy(STRATEGY_DTMF, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_TRANSMITTED_THROUGH_SPEAKER)) {
        device = getDeviceForStrategy(STRATEGY_TRANSMITTED_THROUGH_SPEAKER, fromCache);
    } else if (outputDesc->isStrategyActive(STRATEGY_REROUTING)) {
        device = getDeviceForStrategy(STRATEGY_REROUTING, fromCache);
    }

    ALOGV("getNewOutputDevice() selected device %x", device);
    return device;
}

audio_devices_t AudioPolicyManager::getNewInputDevice(audio_io_handle_t input)
{
    sp<AudioInputDescriptor> inputDesc = mInputs.valueFor(input);

    ssize_t index = mAudioPatches.indexOfKey(inputDesc->mPatchHandle);
    if (index >= 0) {
        sp<AudioPatch> patchDesc = mAudioPatches.valueAt(index);
        if (patchDesc->mUid != mUidCached) {
            ALOGV("getNewInputDevice() device %08x forced by patch %d",
                  inputDesc->mDevice, inputDesc->mPatchHandle);
            return inputDesc->mDevice;
        }
    }

    audio_devices_t device = getDeviceAndMixForInputSource(inputDesc->mInputSource);

    ALOGV("getNewInputDevice() selected device %x", device);
    return device;
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
    audio_devices_t devices;
    AudioPolicyManager::routing_strategy strategy = getStrategy(stream);
    devices = getDeviceForStrategy(strategy, true /*fromCache*/);
    SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(devices, mOutputs);
    for (size_t i = 0; i < outputs.size(); i++) {
        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(outputs[i]);
        if (outputDesc->isStrategyActive(strategy)) {
            devices = outputDesc->device();
            break;
        }
    }

    /*Filter SPEAKER_SAFE out of results, as AudioService doesn't know about it
      and doesn't really need to.*/
    if (devices & AUDIO_DEVICE_OUT_SPEAKER_SAFE) {
        devices |= AUDIO_DEVICE_OUT_SPEAKER;
        devices &= ~AUDIO_DEVICE_OUT_SPEAKER_SAFE;
    }

    return devices;
}

AudioPolicyManager::routing_strategy AudioPolicyManager::getStrategy(
        audio_stream_type_t stream) {

    ALOG_ASSERT(stream != AUDIO_STREAM_PATCH,"getStrategy() called for AUDIO_STREAM_PATCH");

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
    case AUDIO_STREAM_SYSTEM:
        // NOTE: SYSTEM stream uses MEDIA strategy because muting music and switching outputs
        // while key clicks are played produces a poor result
    case AUDIO_STREAM_MUSIC:
#ifdef AUDIO_EXTN_INCALL_MUSIC_ENABLED
    case AUDIO_STREAM_INCALL_MUSIC:
#endif
        return STRATEGY_MEDIA;
    case AUDIO_STREAM_ENFORCED_AUDIBLE:
        return STRATEGY_ENFORCED_AUDIBLE;
    case AUDIO_STREAM_TTS:
        return STRATEGY_TRANSMITTED_THROUGH_SPEAKER;
    case AUDIO_STREAM_ACCESSIBILITY:
        return STRATEGY_ACCESSIBILITY;
    case AUDIO_STREAM_REROUTING:
        return STRATEGY_REROUTING;
    default:
        ALOGE("unknown stream type %d", stream);
    }
    return STRATEGY_MEDIA;
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
    switch (attr->usage) {
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
        if (isStreamActive(AUDIO_STREAM_RING) || isStreamActive(AUDIO_STREAM_ALARM)) {
            return (uint32_t) STRATEGY_SONIFICATION;
        }
        if (isInCall()) {
            return (uint32_t) STRATEGY_PHONE;
        }
        return (uint32_t) STRATEGY_ACCESSIBILITY;

    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
        return (uint32_t) STRATEGY_MEDIA;

    case AUDIO_USAGE_VOICE_COMMUNICATION:
        return (uint32_t) STRATEGY_PHONE;

    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
        return (uint32_t) STRATEGY_DTMF;

    case AUDIO_USAGE_ALARM:
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
        return (uint32_t) STRATEGY_SONIFICATION;

    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
        return (uint32_t) STRATEGY_SONIFICATION_RESPECTFUL;

    case AUDIO_USAGE_UNKNOWN:
    default:
        return (uint32_t) STRATEGY_MEDIA;
    }
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

bool AudioPolicyManager::isAnyOutputActive(audio_stream_type_t streamToIgnore) {
    for (size_t s = 0 ; s < AUDIO_STREAM_CNT ; s++) {
        if (s == (size_t) streamToIgnore) {
            continue;
        }
        for (size_t i = 0; i < mOutputs.size(); i++) {
            const sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(i);
            if (outputDesc->mRefCount[s] != 0) {
                return true;
            }
        }
    }
    return false;
}

uint32_t AudioPolicyManager::handleEventForBeacon(int event) {
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
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            setStreamMute(AUDIO_STREAM_TTS, mute/*on*/,
                    desc->mIoHandle,
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
    uint32_t device = AUDIO_DEVICE_NONE;

    if (fromCache) {
        ALOGVV("getDeviceForStrategy() from cache strategy %d, device %x",
              strategy, mDeviceForStrategy[strategy]);
        return mDeviceForStrategy[strategy];
    }
    audio_devices_t availableOutputDeviceTypes = mAvailableOutputDevices.types();
    switch (strategy) {

    case STRATEGY_TRANSMITTED_THROUGH_SPEAKER:
        device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER;
        if (!device) {
            ALOGE("getDeviceForStrategy() no device found for "\
                    "STRATEGY_TRANSMITTED_THROUGH_SPEAKER");
        }
        break;

    case STRATEGY_SONIFICATION_RESPECTFUL:
        if (isInCall()) {
            device = getDeviceForStrategy(STRATEGY_SONIFICATION, false /*fromCache*/);
        } else if (isStreamActiveRemotely(AUDIO_STREAM_MUSIC,
                SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY)) {
            // while media is playing on a remote device, use the the sonification behavior.
            // Note that we test this usecase before testing if media is playing because
            //   the isStreamActive() method only informs about the activity of a stream, not
            //   if it's for local playback. Note also that we use the same delay between both tests
            device = getDeviceForStrategy(STRATEGY_SONIFICATION, false /*fromCache*/);
            //user "safe" speaker if available instead of normal speaker to avoid triggering
            //other acoustic safety mechanisms for notification
            if (device == AUDIO_DEVICE_OUT_SPEAKER && (availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER_SAFE))
                device = AUDIO_DEVICE_OUT_SPEAKER_SAFE;
        } else if (isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY)) {
            // while media is playing (or has recently played), use the same device
            device = getDeviceForStrategy(STRATEGY_MEDIA, false /*fromCache*/);
        } else {
            // when media is not playing anymore, fall back on the sonification behavior
            device = getDeviceForStrategy(STRATEGY_SONIFICATION, false /*fromCache*/);
            //user "safe" speaker if available instead of normal speaker to avoid triggering
            //other acoustic safety mechanisms for notification
            if (device == AUDIO_DEVICE_OUT_SPEAKER && (availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER_SAFE))
                device = AUDIO_DEVICE_OUT_SPEAKER_SAFE;
        }

        break;

    case STRATEGY_DTMF:
        if (!isInCall()) {
            // when off call, DTMF strategy follows the same rules as MEDIA strategy
            device = getDeviceForStrategy(STRATEGY_MEDIA, false /*fromCache*/);
            break;
        }
        // when in call, DTMF and PHONE strategies follow the same rules
        // FALL THROUGH

    case STRATEGY_PHONE:
        // Force use of only devices on primary output if:
        // - in call AND
        //   - cannot route from voice call RX OR
        //   - audio HAL version is < 3.0 and TX device is on the primary HW module
        if (mPhoneState == AUDIO_MODE_IN_CALL) {
            audio_devices_t txDevice =
                    getDeviceAndMixForInputSource(AUDIO_SOURCE_VOICE_COMMUNICATION);
            sp<AudioOutputDescriptor> hwOutputDesc = mOutputs.valueFor(mPrimaryOutput);
            if (((mAvailableInputDevices.types() &
                    AUDIO_DEVICE_IN_TELEPHONY_RX & ~AUDIO_DEVICE_BIT_IN) == 0) ||
                    (((txDevice & availablePrimaryInputDevices() & ~AUDIO_DEVICE_BIT_IN) != 0) &&
                         (hwOutputDesc->getAudioPort()->mModule->mHalVersion <
                             AUDIO_DEVICE_API_VERSION_3_0))) {
                availableOutputDeviceTypes = availablePrimaryOutputDevices();
            }
        }
        // for phone strategy, we first consider the forced use and then the available devices by order
        // of priority
        switch (mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION]) {
        case AUDIO_POLICY_FORCE_BT_SCO:
            if (!isInCall() || strategy != STRATEGY_DTMF) {
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT;
                if (device) break;
            }
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET;
            if (device) break;
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_SCO;
            if (device) break;
            // if SCO device is requested but no SCO device is available, fall back to default case
            // FALL THROUGH

        default:    // FORCE_NONE
            // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to A2DP
            if (!isInCall() &&
                    (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] != AUDIO_POLICY_FORCE_NO_BT_A2DP) &&
#ifndef QCOM_DIRECTTRACK
                    (getA2dpOutput() != 0) &&
#endif
                    !mA2dpSuspended) {
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
                if (device) break;
            }
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
            if (device) break;
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_WIRED_HEADSET;
            if (device) break;
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_USB_DEVICE;
            if (device) break;
            if (!isInCall()) {
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_USB_ACCESSORY;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_AUX_DIGITAL;
                if (device) break;
            }

            // Allow voice call on USB ANLG DOCK headset
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
            if (device) break;

            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_EARPIECE;
            if (device) break;
            device = mDefaultOutputDevice->mDeviceType;
            if (device == AUDIO_DEVICE_NONE) {
                ALOGE("getDeviceForStrategy() no device found for STRATEGY_PHONE");
            }
            break;

        case AUDIO_POLICY_FORCE_SPEAKER:
            // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to
            // A2DP speaker when forcing to speaker output
            if (!isInCall() &&
                    (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] != AUDIO_POLICY_FORCE_NO_BT_A2DP) &&
#ifndef QCOM_DIRECTTRACK
                    (getA2dpOutput() != 0) &&
#endif
                    !mA2dpSuspended) {
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
                if (device) break;
            }
//FIXME::  Audio team
            if (mPhoneState != AUDIO_MODE_IN_CALL && mPhoneState != AUDIO_MODE_IN_COMMUNICATION) {
//            if (!isInCall()) {

                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_USB_ACCESSORY;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_USB_DEVICE;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_AUX_DIGITAL;
                if (device) break;
                device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
                if (device) break;
            }
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_LINE;
            if (device) break;
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER;
            if (device) break;
            device = mDefaultOutputDevice->mDeviceType;
            if (device == AUDIO_DEVICE_NONE) {
                ALOGE("getDeviceForStrategy() no device found for STRATEGY_PHONE, FORCE_SPEAKER");
            }
            break;
        }

        if (isInCall() && (device == AUDIO_DEVICE_NONE)) {
            // when in call, get the device for Phone strategy
            device = getDeviceForStrategy(STRATEGY_PHONE, false /*fromCache*/);
            break;
        }

#ifdef AUDIO_EXTN_FM_ENABLED
        if (availableOutputDeviceTypes & AUDIO_DEVICE_OUT_FM) {
            if (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] == AUDIO_POLICY_FORCE_SPEAKER) {
                device = AUDIO_DEVICE_OUT_SPEAKER;
            }
        }
#endif
    break;

    case STRATEGY_SONIFICATION:

        // If incall, just select the STRATEGY_PHONE device: The rest of the behavior is handled by
        // handleIncallSonification().
        if (isInCall()) {
            device = getDeviceForStrategy(STRATEGY_PHONE, false /*fromCache*/);
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
            device = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER;
            if (device == AUDIO_DEVICE_NONE) {
                ALOGE("getDeviceForStrategy() speaker device not found for STRATEGY_SONIFICATION");
            }
        }
        // The second device used for sonification is the same as the device used by media strategy
        // FALL THROUGH

    // FIXME: STRATEGY_ACCESSIBILITY and STRATEGY_REROUTING follow STRATEGY_MEDIA for now
    case STRATEGY_ACCESSIBILITY:
        if (strategy == STRATEGY_ACCESSIBILITY) {
            // do not route accessibility prompts to a digital output currently configured with a
            // compressed format as they would likely not be mixed and dropped.
            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
                audio_devices_t devices = desc->device() &
                    (AUDIO_DEVICE_OUT_HDMI | AUDIO_DEVICE_OUT_SPDIF | AUDIO_DEVICE_OUT_HDMI_ARC);
                if (desc->isActive() && !audio_is_linear_pcm(desc->mFormat) &&
                        devices != AUDIO_DEVICE_NONE) {
                    availableOutputDeviceTypes = availableOutputDeviceTypes & ~devices;
                }
            }
        }
        // FALL THROUGH

    case STRATEGY_REROUTING:
    case STRATEGY_MEDIA: {
        uint32_t device2 = AUDIO_DEVICE_NONE;

        if (isInCall() && (device == AUDIO_DEVICE_NONE)) {
            // when in call, get the device for Phone strategy
            device = getDeviceForStrategy(STRATEGY_PHONE, false /*fromCache*/);
            break;
        }
#ifdef AUDIO_EXTN_FM_ENABLED
        if (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] == AUDIO_POLICY_FORCE_SPEAKER) {
            device = AUDIO_DEVICE_OUT_SPEAKER;
            break;
        }
#endif

        if (strategy != STRATEGY_SONIFICATION) {
            // no sonification on remote submix (e.g. WFD)
            if (mAvailableOutputDevices.getDevice(AUDIO_DEVICE_OUT_REMOTE_SUBMIX, String8("0")) != 0) {
                device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_REMOTE_SUBMIX;
            }
        }
        if ((device2 == AUDIO_DEVICE_NONE) &&
                (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] != AUDIO_POLICY_FORCE_NO_BT_A2DP) &&
#ifndef QCOM_DIRECTTRACK
                (getA2dpOutput() != 0) &&
#endif
                !mA2dpSuspended) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP;
            if (device2 == AUDIO_DEVICE_NONE) {
                device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
            }
            if (device2 == AUDIO_DEVICE_NONE) {
                device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
            }
        }
        if ((device2 == AUDIO_DEVICE_NONE) &&
            (mForceUse[AUDIO_POLICY_FORCE_FOR_MEDIA] == AUDIO_POLICY_FORCE_SPEAKER)) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
        }
        if ((device2 == AUDIO_DEVICE_NONE)) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_LINE;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_WIRED_HEADSET;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_USB_ACCESSORY;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_USB_DEVICE;
        }
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET;
        }
        if ((strategy != STRATEGY_SONIFICATION) && (device == AUDIO_DEVICE_NONE)
             && (device2 == AUDIO_DEVICE_NONE)
#ifdef HDMI_PASSTHROUGH_ENABLED
             && (strategy != STRATEGY_ENFORCED_AUDIBLE)
#endif
        ) {
            // no sonification on aux digital (e.g. HDMI)
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_AUX_DIGITAL;
        }
        if ((device2 == AUDIO_DEVICE_NONE) &&
                (mForceUse[AUDIO_POLICY_FORCE_FOR_DOCK] == AUDIO_POLICY_FORCE_ANALOG_DOCK)
                && (strategy != STRATEGY_SONIFICATION)) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET;
        }
#ifdef AUDIO_EXTN_FM_ENABLED
            if ((strategy != STRATEGY_SONIFICATION) && (device == AUDIO_DEVICE_NONE)
                 && (device2 == AUDIO_DEVICE_NONE)) {
                device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_FM_TX;
            }
#endif
#ifdef AUDIO_EXTN_AFE_PROXY_ENABLED
            if ((strategy != STRATEGY_SONIFICATION) && (device == AUDIO_DEVICE_NONE)
                 && (device2 == AUDIO_DEVICE_NONE)) {
                // no sonification on WFD sink
                device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_PROXY;
            }
#endif
        if (device2 == AUDIO_DEVICE_NONE) {
            device2 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPEAKER;
        }
        int device3 = AUDIO_DEVICE_NONE;
        if (strategy == STRATEGY_MEDIA) {
            // ARC, SPDIF and AUX_LINE can co-exist with others.
            device3 = availableOutputDeviceTypes & AUDIO_DEVICE_OUT_HDMI_ARC;
            device3 |= (availableOutputDeviceTypes & AUDIO_DEVICE_OUT_SPDIF);
            device3 |= (availableOutputDeviceTypes & AUDIO_DEVICE_OUT_AUX_LINE);
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

        if (device) break;
        device = mDefaultOutputDevice->mDeviceType;
        if (device == AUDIO_DEVICE_NONE) {
            ALOGE("getDeviceForStrategy() no device found for STRATEGY_MEDIA");
        }
        } break;

    default:
        ALOGW("getDeviceForStrategy() unknown strategy: %d", strategy);
        break;
    }

    ALOGVV("getDeviceForStrategy() strategy %d, device %x", strategy, device);
    return device;
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
        curDevice = curDevice & outputDesc->mProfile->mSupportedDevices.types();
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
                audio_io_handle_t curOutput = mOutputs.keyAt(j);
                ALOGVV("checkDeviceMuteStrategies() %s strategy %d (curDevice %04x) on output %d",
                      mute ? "muting" : "unmuting", i, curDevice, curOutput);
                setStrategyMute((routing_strategy)i, mute, curOutput, mute ? 0 : delayMs);
                if (desc->isStrategyActive((routing_strategy)i)) {
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
        if (muteWaitMs < outputDesc->latency() * 2) {
            muteWaitMs = outputDesc->latency() * 2;
        }
        for (size_t i = 0; i < NUM_STRATEGIES; i++) {
            if (outputDesc->isStrategyActive((routing_strategy)i)) {
                setStrategyMute((routing_strategy)i, true, outputDesc->mIoHandle);
                // do tempMute unmute after twice the mute wait time
#ifdef QCOM_DIRECTTRACK
                if((outputDesc->mFlags & AUDIO_OUTPUT_FLAG_LPA) || (outputDesc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL))
                    {
                     setStrategyMute((routing_strategy)i, false, outputDesc->mIoHandle,muteWaitMs*4 ,device);
                }
                else
#endif
                setStrategyMute((routing_strategy)i, false, outputDesc->mIoHandle,
                                muteWaitMs *2, device);
            }
        }
    }

    // wait for the PCM output buffers to empty before proceeding with the rest of the command
    if (muteWaitMs > delayMs) {
        muteWaitMs -= delayMs;
        if(outputDesc->mDevice == AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET) {
           muteWaitMs = muteWaitMs + 10;
        }
        usleep(muteWaitMs * 1000);
        return muteWaitMs;
    }
    return 0;
}

uint32_t AudioPolicyManager::setOutputDevice(audio_io_handle_t output,
                                             audio_devices_t device,
                                             bool force,
                                             int delayMs,
                                             audio_patch_handle_t *patchHandle,
                                             const char* address)
{
    ALOGV("setOutputDevice() output %d device %04x delayMs %d", output, device, delayMs);
    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    AudioParameter param;
    uint32_t muteWaitMs;

    if (outputDesc->isDuplicated()) {
        muteWaitMs = setOutputDevice(outputDesc->mOutput1->mIoHandle, device, force, delayMs);
        muteWaitMs += setOutputDevice(outputDesc->mOutput2->mIoHandle, device, force, delayMs);
        return muteWaitMs;
    }
    // no need to proceed if new device is not AUDIO_DEVICE_NONE and not supported by current
    // output profile
    if ((device != AUDIO_DEVICE_NONE) &&
            ((device & outputDesc->mProfile->mSupportedDevices.types()) == 0)) {
        return 0;
    }

    // filter devices according to output selected
    device = (audio_devices_t)(device & outputDesc->mProfile->mSupportedDevices.types());

    audio_devices_t prevDevice = outputDesc->mDevice;

    ALOGV("setOutputDevice() prevDevice %04x", prevDevice);

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
    if ((device == AUDIO_DEVICE_NONE || device == prevDevice) && !force &&
            outputDesc->mPatchHandle != 0) {
        ALOGV("setOutputDevice() setting same device %04x or null device for output %d",
              device, output);
        return muteWaitMs;
    }

    ALOGV("setOutputDevice() changing device");

    // do the routing
    if (device == AUDIO_DEVICE_NONE) {
        resetOutputDevice(output, delayMs, NULL);
    } else {
        DeviceVector deviceList = (address == NULL) ?
                mAvailableOutputDevices.getDevicesFromType(device)
                : mAvailableOutputDevices.getDevicesFromTypeAddr(device, String8(address));
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
                index = mAudioPatches.indexOfKey(outputDesc->mPatchHandle);
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
                    patchDesc = new AudioPatch((audio_patch_handle_t)nextUniqueId(),
                                               &patch, mUidCached);
                    addAudioPatch(patchDesc->mHandle, patchDesc);
                } else {
                    patchDesc->mPatch = patch;
                }
                patchDesc->mAfPatchHandle = afPatchHandle;
                patchDesc->mUid = mUidCached;
                if (patchHandle) {
                    *patchHandle = patchDesc->mHandle;
                }
                outputDesc->mPatchHandle = patchDesc->mHandle;
                nextAudioPortGeneration();
                mpClientInterface->onAudioPatchListUpdate();
            }
        }

        // inform all input as well
        for (size_t i = 0; i < mInputs.size(); i++) {
            const sp<AudioInputDescriptor>  inputDescriptor = mInputs.valueAt(i);
            if (!isVirtualInputDevice(inputDescriptor->mDevice)) {
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
    applyStreamVolumes(output, device, delayMs);

    return muteWaitMs;
}

status_t AudioPolicyManager::resetOutputDevice(audio_io_handle_t output,
                                               int delayMs,
                                               audio_patch_handle_t *patchHandle)
{
    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    ssize_t index;
    if (patchHandle) {
        index = mAudioPatches.indexOfKey(*patchHandle);
    } else {
        index = mAudioPatches.indexOfKey(outputDesc->mPatchHandle);
    }
    if (index < 0) {
        return INVALID_OPERATION;
    }
    sp< AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, delayMs);
    ALOGV("resetOutputDevice() releaseAudioPatch returned %d", status);
    outputDesc->mPatchHandle = 0;
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
                    !inputDesc->mIsSoundTrigger) {
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
                index = mAudioPatches.indexOfKey(inputDesc->mPatchHandle);
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
                    patchDesc = new AudioPatch((audio_patch_handle_t)nextUniqueId(),
                                               &patch, mUidCached);
                    addAudioPatch(patchDesc->mHandle, patchDesc);
                } else {
                    patchDesc->mPatch = patch;
                }
                patchDesc->mAfPatchHandle = afPatchHandle;
                patchDesc->mUid = mUidCached;
                if (patchHandle) {
                    *patchHandle = patchDesc->mHandle;
                }
                inputDesc->mPatchHandle = patchDesc->mHandle;
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
        index = mAudioPatches.indexOfKey(inputDesc->mPatchHandle);
    }
    if (index < 0) {
        return INVALID_OPERATION;
    }
    sp< AudioPatch> patchDesc = mAudioPatches.valueAt(index);
    status_t status = mpClientInterface->releaseAudioPatch(patchDesc->mAfPatchHandle, 0);
    ALOGV("resetInputDevice() releaseAudioPatch returned %d", status);
    inputDesc->mPatchHandle = 0;
    removeAudioPatch(patchDesc->mHandle);
    nextAudioPortGeneration();
    mpClientInterface->onAudioPatchListUpdate();
    return status;
}

sp<AudioPolicyManager::IOProfile> AudioPolicyManager::getInputProfile(audio_devices_t device,
                                                   String8 address,
                                                   uint32_t& samplingRate,
                                                   audio_format_t format,
                                                   audio_channel_mask_t channelMask,
                                                   audio_input_flags_t flags)
{
    // Choose an input profile based on the requested capture parameters: select the first available
    // profile supporting all requested parameters.

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
                                             format, channelMask, (audio_output_flags_t) flags)) {

                return profile;
            }
        }
    }
    return NULL;
}


audio_devices_t AudioPolicyManager::getDeviceAndMixForInputSource(audio_source_t inputSource,
                                                            AudioMix **policyMix)
{
    audio_devices_t availableDeviceTypes = mAvailableInputDevices.types() &
                                            ~AUDIO_DEVICE_BIT_IN;

    for (size_t i = 0; i < mPolicyMixes.size(); i++) {
        if (mPolicyMixes[i]->mMix.mMixType != MIX_TYPE_RECORDERS) {
            continue;
        }
        for (size_t j = 0; j < mPolicyMixes[i]->mMix.mCriteria.size(); j++) {
            if ((RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET == mPolicyMixes[i]->mMix.mCriteria[j].mRule &&
                    mPolicyMixes[i]->mMix.mCriteria[j].mAttr.mSource == inputSource) ||
               (RULE_EXCLUDE_ATTRIBUTE_CAPTURE_PRESET == mPolicyMixes[i]->mMix.mCriteria[j].mRule &&
                    mPolicyMixes[i]->mMix.mCriteria[j].mAttr.mSource != inputSource)) {
                if (availableDeviceTypes & AUDIO_DEVICE_IN_REMOTE_SUBMIX) {
                    if (policyMix != NULL) {
                        *policyMix = &mPolicyMixes[i]->mMix;
                    }
                    return AUDIO_DEVICE_IN_REMOTE_SUBMIX;
                }
                break;
            }
        }
    }

    return getDeviceForInputSource(inputSource);
}

audio_devices_t AudioPolicyManager::getDeviceForInputSource(audio_source_t inputSource)
{
    uint32_t device = AUDIO_DEVICE_NONE;
    audio_devices_t availableDeviceTypes = mAvailableInputDevices.types() &
                                            ~AUDIO_DEVICE_BIT_IN;

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
#ifndef QCOM_DIRECTTRACK
        // Allow only use of devices on primary input if in call and HAL does not support routing
        // to voice call path.
        if ((mPhoneState == AUDIO_MODE_IN_CALL) &&
                (mAvailableOutputDevices.types() & AUDIO_DEVICE_OUT_TELEPHONY_TX) == 0) {
            availableDeviceTypes = availablePrimaryInputDevices() & ~AUDIO_DEVICE_BIT_IN;
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
#else
        device = AUDIO_DEVICE_IN_COMMUNICATION;
#endif
        break;

    case AUDIO_SOURCE_VOICE_RECOGNITION:
    case AUDIO_SOURCE_HOTWORD:
        if (mForceUse[AUDIO_POLICY_FORCE_FOR_RECORD] == AUDIO_POLICY_FORCE_BT_SCO &&
                availableDeviceTypes & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            device = AUDIO_DEVICE_IN_WIRED_HEADSET;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_USB_DEVICE) {
            device = AUDIO_DEVICE_IN_USB_DEVICE;
        } else if (availableDeviceTypes & AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET) {
            device = AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET;
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
#ifdef AUDIO_EXTN_FM_ENABLED
    case AUDIO_SOURCE_FM_RX:
        device = AUDIO_DEVICE_IN_FM_RX;
        break;
    case AUDIO_SOURCE_FM_RX_A2DP:
        device = AUDIO_DEVICE_IN_FM_RX_A2DP;
        break;
#endif

     case AUDIO_SOURCE_FM_TUNER:
        if (availableDeviceTypes & AUDIO_DEVICE_IN_FM_TUNER) {
            device = AUDIO_DEVICE_IN_FM_TUNER;
        }
        break;
    default:
        ALOGW("getDeviceForInputSource() invalid input source %d", inputSource);
        break;
    }
    ALOGV("getDeviceForInputSource()input source %d, device %08x", inputSource, device);
    return device;
}

bool AudioPolicyManager::isVirtualInputDevice(audio_devices_t device)
{
    if ((device & AUDIO_DEVICE_BIT_IN) != 0) {
        device &= ~AUDIO_DEVICE_BIT_IN;
        if ((popcount(device) == 1) && ((device & ~APM_AUDIO_IN_DEVICE_VIRTUAL_ALL) == 0))
            return true;
    }
    return false;
}

bool AudioPolicyManager::deviceDistinguishesOnAddress(audio_devices_t device) {
    return ((device & APM_AUDIO_DEVICE_MATCH_ADDRESS_ALL & ~AUDIO_DEVICE_BIT_IN) != 0);
}

audio_io_handle_t AudioPolicyManager::getActiveInput(bool ignoreVirtualInputs)
{
    for (size_t i = 0; i < mInputs.size(); i++) {
        const sp<AudioInputDescriptor>  input_descriptor = mInputs.valueAt(i);
        if ((input_descriptor->mRefCount > 0)
                && (!ignoreVirtualInputs || !isVirtualInputDevice(input_descriptor->mDevice))) {
            return mInputs.keyAt(i);
        }
    }
    return 0;
}

uint32_t AudioPolicyManager::activeInputsCount() const
{
    uint32_t count = 0;
    for (size_t i = 0; i < mInputs.size(); i++) {
        const sp<AudioInputDescriptor>  desc = mInputs.valueAt(i);
        if (desc->mRefCount > 0) {
            count++;
        }
    }
    return count;
}


audio_devices_t AudioPolicyManager::getDeviceForVolume(audio_devices_t device)
{
    if (device == AUDIO_DEVICE_NONE) {
        // this happens when forcing a route update and no track is active on an output.
        // In this case the returned category is not important.
        device =  AUDIO_DEVICE_OUT_SPEAKER;
    } else if (popcount(device) > 1) {
        // Multiple device selection is either:
        //  - speaker + one other device: give priority to speaker in this case.
        //  - one A2DP device + another device: happens with duplicated output. In this case
        // retain the device on the A2DP output as the other must not correspond to an active
        // selection if not the speaker.
        //  - HDMI-CEC system audio mode only output: give priority to available item in order.
        if (device & AUDIO_DEVICE_OUT_SPEAKER) {
            device = AUDIO_DEVICE_OUT_SPEAKER;
        } else if (device & AUDIO_DEVICE_OUT_HDMI_ARC) {
            device = AUDIO_DEVICE_OUT_HDMI_ARC;
        } else if (device & AUDIO_DEVICE_OUT_AUX_LINE) {
            device = AUDIO_DEVICE_OUT_AUX_LINE;
        } else if (device & AUDIO_DEVICE_OUT_SPDIF) {
            device = AUDIO_DEVICE_OUT_SPDIF;
        } else {
            device = (audio_devices_t)(device & AUDIO_DEVICE_OUT_ALL_A2DP);
        }
    }

    /*SPEAKER_SAFE is an alias of SPEAKER for purposes of volume control*/
    if (device == AUDIO_DEVICE_OUT_SPEAKER_SAFE)
        device = AUDIO_DEVICE_OUT_SPEAKER;

    ALOGW_IF(popcount(device) != 1,
            "getDeviceForVolume() invalid device combination: %08x",
            device);

    return device;
}

AudioPolicyManager::device_category AudioPolicyManager::getDeviceCategory(audio_devices_t device)
{
    switch(getDeviceForVolume(device)) {
        case AUDIO_DEVICE_OUT_EARPIECE:
            return DEVICE_CATEGORY_EARPIECE;
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
#ifdef AUDIO_EXTN_FM_ENABLED
        case AUDIO_DEVICE_OUT_FM:
#endif
            return DEVICE_CATEGORY_HEADSET;
        case AUDIO_DEVICE_OUT_LINE:
        case AUDIO_DEVICE_OUT_AUX_DIGITAL:
        /*USB?  Remote submix?*/
            return DEVICE_CATEGORY_EXT_MEDIA;
        case AUDIO_DEVICE_OUT_SPEAKER:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER:
        case AUDIO_DEVICE_OUT_USB_ACCESSORY:
        case AUDIO_DEVICE_OUT_USB_DEVICE:
        case AUDIO_DEVICE_OUT_REMOTE_SUBMIX:
#ifdef AUDIO_EXTN_AFE_PROXY_ENABLED
        case AUDIO_DEVICE_OUT_PROXY:
#endif
        default:
            return DEVICE_CATEGORY_SPEAKER;
    }
}
bool AudioPolicyManager::isDirectOutput(audio_io_handle_t output) {
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_io_handle_t curOutput = mOutputs.keyAt(i);
        sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
        if ((curOutput == output) && (desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            return true;
        }
    }
    return false;
}


/* static */
float AudioPolicyManager::volIndexToAmpl(audio_devices_t device, const StreamDescriptor& streamDesc,
        int indexInUi)
{
    device_category deviceCategory = getDeviceCategory(device);
    const VolumeCurvePoint *curve = streamDesc.mVolumeCurve[deviceCategory];

    // the volume index in the UI is relative to the min and max volume indices for this stream type
    int nbSteps = 1 + curve[VOLMAX].mIndex -
            curve[VOLMIN].mIndex;
    int volIdx = (nbSteps * (indexInUi - streamDesc.mIndexMin)) /
            (streamDesc.mIndexMax - streamDesc.mIndexMin);

    // find what part of the curve this index volume belongs to, or if it's out of bounds
    int segment = 0;
    if (volIdx < curve[VOLMIN].mIndex) {         // out of bounds
        return 0.0f;
    } else if (volIdx < curve[VOLKNEE1].mIndex) {
        segment = 0;
    } else if (volIdx < curve[VOLKNEE2].mIndex) {
        segment = 1;
    } else if (volIdx <= curve[VOLMAX].mIndex) {
        segment = 2;
    } else {                                                               // out of bounds
        return 1.0f;
    }

    // linear interpolation in the attenuation table in dB
    float decibels = curve[segment].mDBAttenuation +
            ((float)(volIdx - curve[segment].mIndex)) *
                ( (curve[segment+1].mDBAttenuation -
                        curve[segment].mDBAttenuation) /
                    ((float)(curve[segment+1].mIndex -
                            curve[segment].mIndex)) );

    float amplification = exp( decibels * 0.115129f); // exp( dB * ln(10) / 20 )

    ALOGVV("VOLUME vol index=[%d %d %d], dB=[%.1f %.1f %.1f] ampl=%.5f",
            curve[segment].mIndex, volIdx,
            curve[segment+1].mIndex,
            curve[segment].mDBAttenuation,
            decibels,
            curve[segment+1].mDBAttenuation,
            amplification);

    return amplification;
}

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sDefaultVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -49.5f}, {33, -33.5f}, {66, -17.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sDefaultMediaVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -58.0f}, {20, -40.0f}, {60, -17.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sExtMediaSystemVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -58.0f}, {20, -40.0f}, {60, -21.0f}, {100, -10.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sSpeakerMediaVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -56.0f}, {20, -34.0f}, {60, -11.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sSpeakerMediaVolumeCurveDrc[AudioPolicyManager::VOLCNT] = {
    {1, -55.0f}, {20, -43.0f}, {86, -12.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sSpeakerSonificationVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -29.7f}, {33, -20.1f}, {66, -10.2f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sSpeakerSonificationVolumeCurveDrc[AudioPolicyManager::VOLCNT] = {
    {1, -35.7f}, {33, -26.1f}, {66, -13.2f}, {100, 0.0f}
};

// AUDIO_STREAM_SYSTEM, AUDIO_STREAM_ENFORCED_AUDIBLE and AUDIO_STREAM_DTMF volume tracks
// AUDIO_STREAM_RING on phones and AUDIO_STREAM_MUSIC on tablets.
// AUDIO_STREAM_DTMF tracks AUDIO_STREAM_VOICE_CALL while in call (See AudioService.java).
// The range is constrained between -24dB and -6dB over speaker and -30dB and -18dB over headset.

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sDefaultSystemVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -24.0f}, {33, -18.0f}, {66, -12.0f}, {100, -6.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sDefaultSystemVolumeCurveDrc[AudioPolicyManager::VOLCNT] = {
    {1, -34.0f}, {33, -24.0f}, {66, -15.0f}, {100, -6.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sHeadsetSystemVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {1, -30.0f}, {33, -26.0f}, {66, -22.0f}, {100, -18.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sDefaultVoiceVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {0, -42.0f}, {33, -28.0f}, {66, -14.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sSpeakerVoiceVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {0, -24.0f}, {33, -16.0f}, {66, -8.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sLinearVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {0, -96.0f}, {33, -68.0f}, {66, -34.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sSilentVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {0, -96.0f}, {1, -96.0f}, {2, -96.0f}, {100, -96.0f}
};

const AudioPolicyManager::VolumeCurvePoint
    AudioPolicyManager::sFullScaleVolumeCurve[AudioPolicyManager::VOLCNT] = {
    {0, 0.0f}, {1, 0.0f}, {2, 0.0f}, {100, 0.0f}
};

const AudioPolicyManager::VolumeCurvePoint
            *AudioPolicyManager::sVolumeProfiles[AUDIO_STREAM_CNT]
                                                   [AudioPolicyManager::DEVICE_CATEGORY_CNT] = {
    { // AUDIO_STREAM_VOICE_CALL
        sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_SYSTEM
        sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultSystemVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_RING
        sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_MUSIC
        sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ALARM
        sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_NOTIFICATION
        sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_BLUETOOTH_SCO
        sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ENFORCED_AUDIBLE
        sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    {  // AUDIO_STREAM_DTMF
        sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_TTS
      // "Transmitted Through Speaker": always silent except on DEVICE_CATEGORY_SPEAKER
        sSilentVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sLinearVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sSilentVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sSilentVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ACCESSIBILITY
        sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
#if defined(QCOM_HARDWARE) && !defined(QCOM_DIRECTTRACK)
    { // AUDIO_STREAM_INCALL_MUSIC
        sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sDefaultMediaVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
#endif
    { // AUDIO_STREAM_REROUTING
        sFullScaleVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sFullScaleVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sFullScaleVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_PATCH
        sFullScaleVolumeCurve, // DEVICE_CATEGORY_HEADSET
        sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        sFullScaleVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        sFullScaleVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
};

void AudioPolicyManager::initializeVolumeCurves()
{
    for (int i = 0; i < AUDIO_STREAM_CNT; i++) {
        for (int j = 0; j < DEVICE_CATEGORY_CNT; j++) {
            mStreams[i].mVolumeCurve[j] =
                    sVolumeProfiles[i][j];
        }
    }

    // Check availability of DRC on speaker path: if available, override some of the speaker curves
    if (mSpeakerDrcEnabled) {
        mStreams[AUDIO_STREAM_SYSTEM].mVolumeCurve[DEVICE_CATEGORY_SPEAKER] =
                sDefaultSystemVolumeCurveDrc;
        mStreams[AUDIO_STREAM_RING].mVolumeCurve[DEVICE_CATEGORY_SPEAKER] =
                sSpeakerSonificationVolumeCurveDrc;
        mStreams[AUDIO_STREAM_ALARM].mVolumeCurve[DEVICE_CATEGORY_SPEAKER] =
                sSpeakerSonificationVolumeCurveDrc;
        mStreams[AUDIO_STREAM_NOTIFICATION].mVolumeCurve[DEVICE_CATEGORY_SPEAKER] =
                sSpeakerSonificationVolumeCurveDrc;
        mStreams[AUDIO_STREAM_MUSIC].mVolumeCurve[DEVICE_CATEGORY_SPEAKER] =
                sSpeakerMediaVolumeCurveDrc;
        mStreams[AUDIO_STREAM_ACCESSIBILITY].mVolumeCurve[DEVICE_CATEGORY_SPEAKER] =
                sSpeakerMediaVolumeCurveDrc;
    }
}

float AudioPolicyManager::computeVolume(audio_stream_type_t stream,
                                            int index,
                                            audio_io_handle_t output,
                                            audio_devices_t device)
{
    float volume = 1.0;
    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    StreamDescriptor &streamDesc = mStreams[stream];

    if (device == AUDIO_DEVICE_NONE) {
        device = outputDesc->device();
    }

    // if volume is not 0 (not muted), force media volume to max on digital output
    if (stream == AUDIO_STREAM_MUSIC &&
        index != mStreams[stream].mIndexMin &&
        (device == AUDIO_DEVICE_OUT_AUX_DIGITAL ||
#ifdef AUDIO_EXTN_AFE_PROXY_ENABLED
         device == AUDIO_DEVICE_OUT_PROXY ||
#endif
         device == AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)) {
        return 1.0;
    }

#ifdef AUDIO_EXTN_INCALL_MUSIC_ENABLED
    if (stream == AUDIO_STREAM_INCALL_MUSIC) {
        return 1.0;
    }
#endif

    volume = volIndexToAmpl(device, streamDesc, index);

    // if a headset is connected, apply the following rules to ring tones and notifications
    // to avoid sound level bursts in user's ears:
    // - always attenuate ring tones and notifications volume by 6dB
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
                    (mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM] == AUDIO_POLICY_FORCE_NONE))) &&
        streamDesc.mCanBeMuted) {
        volume *= SONIFICATION_HEADSET_VOLUME_FACTOR;
        // when the phone is ringing we must consider that music could have been paused just before
        // by the music application and behave as if music was active if the last music track was
        // just stopped
        if (isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY) ||
                mLimitRingtoneVolume) {
            audio_devices_t musicDevice = getDeviceForStrategy(STRATEGY_MEDIA, true /*fromCache*/);
            float musicVol = computeVolume(AUDIO_STREAM_MUSIC,
                               mStreams[AUDIO_STREAM_MUSIC].getVolumeIndex(musicDevice),
                               output,
                               musicDevice);
            float minVol = (musicVol > SONIFICATION_HEADSET_VOLUME_MIN) ?
                                musicVol : SONIFICATION_HEADSET_VOLUME_MIN;
            if (volume > minVol) {
                volume = minVol;
                ALOGV("computeVolume limiting volume to %f musicVol %f", minVol, musicVol);
            }
        }
    }

    return volume;
}

status_t AudioPolicyManager::checkAndSetVolume(audio_stream_type_t stream,
                                                   int index,
                                                   audio_io_handle_t output,
                                                   audio_devices_t device,
                                                   int delayMs,
                                                   bool force)
{

    // do not change actual stream volume if the stream is muted
    if (mOutputs.valueFor(output)->mMuteCount[stream] != 0) {
        ALOGVV("checkAndSetVolume() stream %d muted count %d",
              stream, mOutputs.valueFor(output)->mMuteCount[stream]);
        return NO_ERROR;
    }

    // do not change in call volume if bluetooth is connected and vice versa
    if ((stream == AUDIO_STREAM_VOICE_CALL &&
            mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION] == AUDIO_POLICY_FORCE_BT_SCO) ||
        (stream == AUDIO_STREAM_BLUETOOTH_SCO &&
                mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION] != AUDIO_POLICY_FORCE_BT_SCO)) {
        ALOGV("checkAndSetVolume() cannot set stream %d volume with force use = %d for comm",
             stream, mForceUse[AUDIO_POLICY_FORCE_FOR_COMMUNICATION]);
        return INVALID_OPERATION;
    }

    float volume = computeVolume(stream, index, output, device);
    // unit gain if rerouting to external policy
    if (device == AUDIO_DEVICE_OUT_REMOTE_SUBMIX) {
        ssize_t index = mOutputs.indexOfKey(output);
        if (index >= 0) {
            sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);
            if (outputDesc->mPolicyMix != NULL) {
                ALOGV("max gain when rerouting for output=%d", output);
                volume = 1.0f;
            }
        }

    }
    // We actually change the volume if:
    // - the float value returned by computeVolume() changed
    // - the force flag is set
    if (volume != mOutputs.valueFor(output)->mCurVolume[stream] ||
            force) {
        mOutputs.valueFor(output)->mCurVolume[stream] = volume;
        ALOGVV("checkAndSetVolume() for output %d stream %d, volume %f, delay %d", output, stream, volume, delayMs);
        // Force VOICE_CALL to track BLUETOOTH_SCO stream volume when bluetooth audio is
        // enabled
        if (stream == AUDIO_STREAM_BLUETOOTH_SCO) {
            mpClientInterface->setStreamVolume(AUDIO_STREAM_VOICE_CALL, volume, output, delayMs);
#if defined(AUDIO_EXTN_FM_ENABLED) || defined(MTK_HARDWARE)
        } else if (stream == AUDIO_STREAM_MUSIC &&
                   output == mPrimaryOutput) {
            if (volume >= 0) {
                AudioParameter param = AudioParameter();
                param.addFloat(String8("fm_volume"), volume);
                ALOGV("checkAndSetVolume setParameters volume, volume=:%f delay=:%d",volume,delayMs*2);
                //Double delayMs to avoid sound burst while device switch.
                mpClientInterface->setParameters(mPrimaryOutput, param.toString(), delayMs*2);
            }
#endif
        }
        mpClientInterface->setStreamVolume(stream, volume, output, delayMs);
    }

    if (stream == AUDIO_STREAM_VOICE_CALL ||
        stream == AUDIO_STREAM_BLUETOOTH_SCO) {
        float voiceVolume;
        // Force voice volume to max for bluetooth SCO as volume is managed by the headset
        if (stream == AUDIO_STREAM_VOICE_CALL) {
            voiceVolume = (float)index/(float)mStreams[stream].mIndexMax;
        } else {
            voiceVolume = 1.0;
        }

        if (voiceVolume != mLastVoiceVolume && ((output == mPrimaryOutput) ||
            isDirectOutput(output) || device & AUDIO_DEVICE_OUT_ALL_USB)) {
            mpClientInterface->setVoiceVolume(voiceVolume, delayMs);
            mLastVoiceVolume = voiceVolume;
        }
    }

    return NO_ERROR;
}

void AudioPolicyManager::applyStreamVolumes(audio_io_handle_t output,
                                                audio_devices_t device,
                                                int delayMs,
                                                bool force)
{
    ALOGVV("applyStreamVolumes() for output %d and device %x", output, device);

    for (int stream = 0; stream < AUDIO_STREAM_CNT; stream++) {
        if (stream == AUDIO_STREAM_PATCH) {
            continue;
        }
        checkAndSetVolume((audio_stream_type_t)stream,
                          mStreams[stream].getVolumeIndex(device),
                          output,
                          device,
                          delayMs,
                          force);
    }
}

void AudioPolicyManager::setStrategyMute(routing_strategy strategy,
                                             bool on,
                                             audio_io_handle_t output,
                                             int delayMs,
                                             audio_devices_t device)
{
    ALOGVV("setStrategyMute() strategy %d, mute %d, output %d", strategy, on, output);
    for (int stream = 0; stream < AUDIO_STREAM_CNT; stream++) {
        if (stream == AUDIO_STREAM_PATCH) {
            continue;
        }
        if (getStrategy((audio_stream_type_t)stream) == strategy) {
            setStreamMute((audio_stream_type_t)stream, on, output, delayMs, device);
        }
    }
}

void AudioPolicyManager::setStreamMute(audio_stream_type_t stream,
                                           bool on,
                                           audio_io_handle_t output,
                                           int delayMs,
                                           audio_devices_t device)
{
    StreamDescriptor &streamDesc = mStreams[stream];
    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(output);
    if (device == AUDIO_DEVICE_NONE) {
        device = outputDesc->device();
    }

    ALOGVV("setStreamMute() stream %d, mute %d, output %d, mMuteCount %d device %04x",
          stream, on, output, outputDesc->mMuteCount[stream], device);

    if (on) {
        if (outputDesc->mMuteCount[stream] == 0) {
            if (streamDesc.mCanBeMuted &&
                    ((stream != AUDIO_STREAM_ENFORCED_AUDIBLE) ||
                     (mForceUse[AUDIO_POLICY_FORCE_FOR_SYSTEM] == AUDIO_POLICY_FORCE_NONE))) {
                checkAndSetVolume(stream, 0, output, device, delayMs);
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
                              streamDesc.getVolumeIndex(device),
                              output,
                              device,
                              delayMs);
        }
    }
}

void AudioPolicyManager::handleIncallSonification(audio_stream_type_t stream,
                                                      bool starting, bool stateChange)
{
    // if the stream pertains to sonification strategy and we are in call we must
    // mute the stream if it is low visibility. If it is high visibility, we must play a tone
    // in the device used for phone strategy and play the tone if the selected device does not
    // interfere with the device used for phone strategy
    // if stateChange is true, we are called from setPhoneState() and we must mute or unmute as
    // many times as there are active tracks on the output

    // no action needed for AUDIO_STREAM_PATCH stream type, it's for internal flinger tracks
    if (stream == AUDIO_STREAM_PATCH) {
        return;
    }

    const routing_strategy stream_strategy = getStrategy(stream);
    if ((stream_strategy == STRATEGY_SONIFICATION) ||
            ((stream_strategy == STRATEGY_SONIFICATION_RESPECTFUL))) {
        sp<AudioOutputDescriptor> outputDesc = mOutputs.valueFor(mPrimaryOutput);
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

bool AudioPolicyManager::isInCall()
{
    return isStateInCall(mPhoneState);
}

bool AudioPolicyManager::isStateInCall(int state) {
    return ((state == AUDIO_MODE_IN_CALL) || (state == AUDIO_MODE_IN_COMMUNICATION));
}

uint32_t AudioPolicyManager::getMaxEffectsCpuLoad()
{
    return MAX_EFFECTS_CPU_LOAD;
}

uint32_t AudioPolicyManager::getMaxEffectsMemory()
{
    return MAX_EFFECTS_MEMORY;
}


// --- AudioOutputDescriptor class implementation

AudioPolicyManager::AudioOutputDescriptor::AudioOutputDescriptor(
        const sp<IOProfile>& profile)
    : mId(0), mIoHandle(0), mLatency(0),
    mFlags((audio_output_flags_t)0), mDevice(AUDIO_DEVICE_NONE), mPolicyMix(NULL),
    mPatchHandle(0),
    mOutput1(0), mOutput2(0), mProfile(profile), mDirectOpenCount(0)
{
    // clear usage count for all stream types
    for (int i = 0; i < AUDIO_STREAM_CNT; i++) {
        mRefCount[i] = 0;
        mCurVolume[i] = -1.0;
        mMuteCount[i] = 0;
        mStopTime[i] = 0;
    }
    for (int i = 0; i < NUM_STRATEGIES; i++) {
        mStrategyMutedByDevice[i] = false;
    }
    if (profile != NULL) {
        mFlags = (audio_output_flags_t)profile->mFlags;
        mSamplingRate = profile->pickSamplingRate();
        mFormat = profile->pickFormat();
        mChannelMask = profile->pickChannelMask();
        if (profile->mGains.size() > 0) {
            profile->mGains[0]->getDefaultConfig(&mGain);
        }
    }
}

audio_devices_t AudioPolicyManager::AudioOutputDescriptor::device() const
{
    if (isDuplicated()) {
        return (audio_devices_t)(mOutput1->mDevice | mOutput2->mDevice);
    } else {
        return mDevice;
    }
}

uint32_t AudioPolicyManager::AudioOutputDescriptor::latency()
{
    if (isDuplicated()) {
        return (mOutput1->mLatency > mOutput2->mLatency) ? mOutput1->mLatency : mOutput2->mLatency;
    } else {
        return mLatency;
    }
}

bool AudioPolicyManager::AudioOutputDescriptor::sharesHwModuleWith(
        const sp<AudioOutputDescriptor> outputDesc)
{
    if (isDuplicated()) {
        return mOutput1->sharesHwModuleWith(outputDesc) || mOutput2->sharesHwModuleWith(outputDesc);
    } else if (outputDesc->isDuplicated()){
        return sharesHwModuleWith(outputDesc->mOutput1) || sharesHwModuleWith(outputDesc->mOutput2);
    } else {
        return (mProfile->mModule == outputDesc->mProfile->mModule);
    }
}

void AudioPolicyManager::AudioOutputDescriptor::changeRefCount(audio_stream_type_t stream,
                                                                   int delta)
{
    // forward usage count change to attached outputs
    if (isDuplicated()) {
        mOutput1->changeRefCount(stream, delta);
        mOutput2->changeRefCount(stream, delta);
    }
    if ((delta + (int)mRefCount[stream]) < 0) {
        ALOGW("changeRefCount() invalid delta %d for stream %d, refCount %d",
              delta, stream, mRefCount[stream]);
        mRefCount[stream] = 0;
        return;
    }
    mRefCount[stream] += delta;
    ALOGV("changeRefCount() stream %d, count %d", stream, mRefCount[stream]);
}

audio_devices_t AudioPolicyManager::AudioOutputDescriptor::supportedDevices()
{
    if (isDuplicated()) {
        return (audio_devices_t)(mOutput1->supportedDevices() | mOutput2->supportedDevices());
    } else {
        return mProfile->mSupportedDevices.types() ;
    }
}

bool AudioPolicyManager::AudioOutputDescriptor::isActive(uint32_t inPastMs) const
{
    return isStrategyActive(NUM_STRATEGIES, inPastMs);
}

bool AudioPolicyManager::AudioOutputDescriptor::isStrategyActive(routing_strategy strategy,
                                                                       uint32_t inPastMs,
                                                                       nsecs_t sysTime) const
{
    if ((sysTime == 0) && (inPastMs != 0)) {
        sysTime = systemTime();
    }
    for (int i = 0; i < (int)AUDIO_STREAM_CNT; i++) {
        if (i == AUDIO_STREAM_PATCH) {
            continue;
        }
        if (((getStrategy((audio_stream_type_t)i) == strategy) ||
                (NUM_STRATEGIES == strategy)) &&
                isStreamActive((audio_stream_type_t)i, inPastMs, sysTime)) {
            return true;
        }
    }
    return false;
}

bool AudioPolicyManager::AudioOutputDescriptor::isStreamActive(audio_stream_type_t stream,
                                                                       uint32_t inPastMs,
                                                                       nsecs_t sysTime) const
{
    if (mRefCount[stream] != 0) {
        return true;
    }
    if (inPastMs == 0) {
        return false;
    }
    if (sysTime == 0) {
        sysTime = systemTime();
    }
    if (ns2ms(sysTime - mStopTime[stream]) < inPastMs) {
        return true;
    }
    return false;
}

void AudioPolicyManager::AudioOutputDescriptor::toAudioPortConfig(
                                                 struct audio_port_config *dstConfig,
                                                 const struct audio_port_config *srcConfig) const
{
    ALOG_ASSERT(!isDuplicated(), "toAudioPortConfig() called on duplicated output %d", mIoHandle);

    dstConfig->config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE|AUDIO_PORT_CONFIG_CHANNEL_MASK|
                            AUDIO_PORT_CONFIG_FORMAT|AUDIO_PORT_CONFIG_GAIN;
    if (srcConfig != NULL) {
        dstConfig->config_mask |= srcConfig->config_mask;
    }
    AudioPortConfig::toAudioPortConfig(dstConfig, srcConfig);

    dstConfig->id = mId;
    dstConfig->role = AUDIO_PORT_ROLE_SOURCE;
    dstConfig->type = AUDIO_PORT_TYPE_MIX;
    dstConfig->ext.mix.hw_module = mProfile->mModule->mHandle;
    dstConfig->ext.mix.handle = mIoHandle;
    dstConfig->ext.mix.usecase.stream = AUDIO_STREAM_DEFAULT;
}

void AudioPolicyManager::AudioOutputDescriptor::toAudioPort(
                                                    struct audio_port *port) const
{
    ALOG_ASSERT(!isDuplicated(), "toAudioPort() called on duplicated output %d", mIoHandle);
    mProfile->toAudioPort(port);
    port->id = mId;
    toAudioPortConfig(&port->active_config);
    port->ext.mix.hw_module = mProfile->mModule->mHandle;
    port->ext.mix.handle = mIoHandle;
    port->ext.mix.latency_class =
            mFlags & AUDIO_OUTPUT_FLAG_FAST ? AUDIO_LATENCY_LOW : AUDIO_LATENCY_NORMAL;
}

status_t AudioPolicyManager::AudioOutputDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, " ID: %d\n", mId);
    result.append(buffer);
    snprintf(buffer, SIZE, " Sampling rate: %d\n", mSamplingRate);
    result.append(buffer);
    snprintf(buffer, SIZE, " Format: %08x\n", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, " Channels: %08x\n", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, " Latency: %d\n", mLatency);
    result.append(buffer);
    snprintf(buffer, SIZE, " Flags %08x\n", mFlags);
    result.append(buffer);
    snprintf(buffer, SIZE, " Devices %08x\n", device());
    result.append(buffer);
    snprintf(buffer, SIZE, " Stream volume refCount muteCount\n");
    result.append(buffer);
    for (int i = 0; i < (int)AUDIO_STREAM_CNT; i++) {
        snprintf(buffer, SIZE, " %02d     %.03f     %02d       %02d\n",
                 i, mCurVolume[i], mRefCount[i], mMuteCount[i]);
        result.append(buffer);
    }
    write(fd, result.string(), result.size());

    return NO_ERROR;
}

// --- AudioInputDescriptor class implementation

AudioPolicyManager::AudioInputDescriptor::AudioInputDescriptor(const sp<IOProfile>& profile)
    : mId(0), mIoHandle(0),
      mDevice(AUDIO_DEVICE_NONE), mPolicyMix(NULL), mPatchHandle(0), mRefCount(0),
      mInputSource(AUDIO_SOURCE_DEFAULT), mProfile(profile), mIsSoundTrigger(false)
{
    if (profile != NULL) {
        mSamplingRate = profile->pickSamplingRate();
        mFormat = profile->pickFormat();
        mChannelMask = profile->pickChannelMask();
        if (profile->mGains.size() > 0) {
            profile->mGains[0]->getDefaultConfig(&mGain);
        }
    }
}

void AudioPolicyManager::AudioInputDescriptor::toAudioPortConfig(
                                                   struct audio_port_config *dstConfig,
                                                   const struct audio_port_config *srcConfig) const
{
    ALOG_ASSERT(mProfile != 0,
                "toAudioPortConfig() called on input with null profile %d", mIoHandle);
    dstConfig->config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE|AUDIO_PORT_CONFIG_CHANNEL_MASK|
                            AUDIO_PORT_CONFIG_FORMAT|AUDIO_PORT_CONFIG_GAIN;
    if (srcConfig != NULL) {
        dstConfig->config_mask |= srcConfig->config_mask;
    }

    AudioPortConfig::toAudioPortConfig(dstConfig, srcConfig);

    dstConfig->id = mId;
    dstConfig->role = AUDIO_PORT_ROLE_SINK;
    dstConfig->type = AUDIO_PORT_TYPE_MIX;
    dstConfig->ext.mix.hw_module = mProfile->mModule->mHandle;
    dstConfig->ext.mix.handle = mIoHandle;
    dstConfig->ext.mix.usecase.source = mInputSource;
}

void AudioPolicyManager::AudioInputDescriptor::toAudioPort(
                                                    struct audio_port *port) const
{
    ALOG_ASSERT(mProfile != 0, "toAudioPort() called on input with null profile %d", mIoHandle);

    mProfile->toAudioPort(port);
    port->id = mId;
    toAudioPortConfig(&port->active_config);
    port->ext.mix.hw_module = mProfile->mModule->mHandle;
    port->ext.mix.handle = mIoHandle;
    port->ext.mix.latency_class = AUDIO_LATENCY_NORMAL;
}

status_t AudioPolicyManager::AudioInputDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, " ID: %d\n", mId);
    result.append(buffer);
    snprintf(buffer, SIZE, " Sampling rate: %d\n", mSamplingRate);
    result.append(buffer);
    snprintf(buffer, SIZE, " Format: %d\n", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, " Channels: %08x\n", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, " Devices %08x\n", mDevice);
    result.append(buffer);
    snprintf(buffer, SIZE, " Ref Count %d\n", mRefCount);
    result.append(buffer);
    snprintf(buffer, SIZE, " Open Ref Count %d\n", mOpenRefCount);
    result.append(buffer);

    write(fd, result.string(), result.size());

    return NO_ERROR;
}

// --- StreamDescriptor class implementation

AudioPolicyManager::StreamDescriptor::StreamDescriptor()
    :   mIndexMin(0), mIndexMax(1), mCanBeMuted(true)
{
    mIndexCur.add(AUDIO_DEVICE_OUT_DEFAULT, 0);
}

int AudioPolicyManager::StreamDescriptor::getVolumeIndex(audio_devices_t device)
{
    device = AudioPolicyManager::getDeviceForVolume(device);
    // there is always a valid entry for AUDIO_DEVICE_OUT_DEFAULT
    if (mIndexCur.indexOfKey(device) < 0) {
        device = AUDIO_DEVICE_OUT_DEFAULT;
    }
    return mIndexCur.valueFor(device);
}

void AudioPolicyManager::StreamDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%s         %02d         %02d         ",
             mCanBeMuted ? "true " : "false", mIndexMin, mIndexMax);
    result.append(buffer);
    for (size_t i = 0; i < mIndexCur.size(); i++) {
        snprintf(buffer, SIZE, "%04x : %02d, ",
                 mIndexCur.keyAt(i),
                 mIndexCur.valueAt(i));
        result.append(buffer);
    }
    result.append("\n");

    write(fd, result.string(), result.size());
}

// --- EffectDescriptor class implementation

status_t AudioPolicyManager::EffectDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, " I/O: %d\n", mIo);
    result.append(buffer);
    snprintf(buffer, SIZE, " Strategy: %d\n", mStrategy);
    result.append(buffer);
    snprintf(buffer, SIZE, " Session: %d\n", mSession);
    result.append(buffer);
    snprintf(buffer, SIZE, " Name: %s\n",  mDesc.name);
    result.append(buffer);
    snprintf(buffer, SIZE, " %s\n",  mEnabled ? "Enabled" : "Disabled");
    result.append(buffer);
    write(fd, result.string(), result.size());

    return NO_ERROR;
}

// --- HwModule class implementation

AudioPolicyManager::HwModule::HwModule(const char *name)
    : mName(strndup(name, AUDIO_HARDWARE_MODULE_ID_MAX_LEN)),
      mHalVersion(AUDIO_DEVICE_API_VERSION_MIN), mHandle(0)
{
}

AudioPolicyManager::HwModule::~HwModule()
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        mOutputProfiles[i]->mSupportedDevices.clear();
    }
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        mInputProfiles[i]->mSupportedDevices.clear();
    }
    free((void *)mName);
}

status_t AudioPolicyManager::HwModule::loadInput(cnode *root)
{
    cnode *node = root->first_child;

    sp<IOProfile> profile = new IOProfile(String8(root->name), AUDIO_PORT_ROLE_SINK, this);

    while (node) {
        if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            profile->loadSamplingRates((char *)node->value);
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            profile->loadFormats((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            profile->loadInChannels((char *)node->value);
        } else if (strcmp(node->name, DEVICES_TAG) == 0) {
            profile->mSupportedDevices.loadDevicesFromName((char *)node->value,
                                                           mDeclaredDevices);
        } else if (strcmp(node->name, FLAGS_TAG) == 0) {
            profile->mFlags = parseInputFlagNames((char *)node->value);
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            profile->loadGains(node);
        }
        node = node->next;
    }
    ALOGW_IF(profile->mSupportedDevices.isEmpty(),
            "loadInput() invalid supported devices");
    ALOGW_IF(profile->mChannelMasks.size() == 0,
            "loadInput() invalid supported channel masks");
    ALOGW_IF(profile->mSamplingRates.size() == 0,
            "loadInput() invalid supported sampling rates");
    ALOGW_IF(profile->mFormats.size() == 0,
            "loadInput() invalid supported formats");
    if (!profile->mSupportedDevices.isEmpty() &&
            (profile->mChannelMasks.size() != 0) &&
            (profile->mSamplingRates.size() != 0) &&
            (profile->mFormats.size() != 0)) {

        ALOGV("loadInput() adding input Supported Devices %04x",
              profile->mSupportedDevices.types());

        mInputProfiles.add(profile);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

status_t AudioPolicyManager::HwModule::loadOutput(cnode *root)
{
    cnode *node = root->first_child;

    sp<IOProfile> profile = new IOProfile(String8(root->name), AUDIO_PORT_ROLE_SOURCE, this);

    while (node) {
        if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            profile->loadSamplingRates((char *)node->value);
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            profile->loadFormats((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            profile->loadOutChannels((char *)node->value);
        } else if (strcmp(node->name, DEVICES_TAG) == 0) {
            profile->mSupportedDevices.loadDevicesFromName((char *)node->value,
                                                           mDeclaredDevices);
        } else if (strcmp(node->name, FLAGS_TAG) == 0) {
            profile->mFlags = parseOutputFlagNames((char *)node->value);
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            profile->loadGains(node);
        }
        node = node->next;
    }
    ALOGW_IF(profile->mSupportedDevices.isEmpty(),
            "loadOutput() invalid supported devices");
    ALOGW_IF(profile->mChannelMasks.size() == 0,
            "loadOutput() invalid supported channel masks");
    ALOGW_IF(profile->mSamplingRates.size() == 0,
            "loadOutput() invalid supported sampling rates");
    ALOGW_IF(profile->mFormats.size() == 0,
            "loadOutput() invalid supported formats");
    if (!profile->mSupportedDevices.isEmpty() &&
            (profile->mChannelMasks.size() != 0) &&
            (profile->mSamplingRates.size() != 0) &&
            (profile->mFormats.size() != 0)) {

        ALOGV("loadOutput() adding output Supported Devices %04x, mFlags %04x",
              profile->mSupportedDevices.types(), profile->mFlags);

        mOutputProfiles.add(profile);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

status_t AudioPolicyManager::HwModule::loadDevice(cnode *root)
{
    cnode *node = root->first_child;

    audio_devices_t type = AUDIO_DEVICE_NONE;
    while (node) {
        if (strcmp(node->name, DEVICE_TYPE) == 0) {
            type = parseDeviceNames((char *)node->value);
            break;
        }
        node = node->next;
    }
    if (type == AUDIO_DEVICE_NONE ||
            (!audio_is_input_device(type) && !audio_is_output_device(type))) {
        ALOGW("loadDevice() bad type %08x", type);
        return BAD_VALUE;
    }
    sp<DeviceDescriptor> deviceDesc = new DeviceDescriptor(String8(root->name), type);
    deviceDesc->mModule = this;

    node = root->first_child;
    while (node) {
        if (strcmp(node->name, DEVICE_ADDRESS) == 0) {
            deviceDesc->mAddress = String8((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            if (audio_is_input_device(type)) {
                deviceDesc->loadInChannels((char *)node->value);
            } else {
                deviceDesc->loadOutChannels((char *)node->value);
            }
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            deviceDesc->loadGains(node);
        }
        node = node->next;
    }

    ALOGV("loadDevice() adding device name %s type %08x address %s",
          deviceDesc->mName.string(), type, deviceDesc->mAddress.string());

    mDeclaredDevices.add(deviceDesc);

    return NO_ERROR;
}

status_t AudioPolicyManager::HwModule::addOutputProfile(String8 name, const audio_config_t *config,
                                                  audio_devices_t device, String8 address)
{
    sp<IOProfile> profile = new IOProfile(name, AUDIO_PORT_ROLE_SOURCE, this);

    profile->mSamplingRates.add(config->sample_rate);
    profile->mChannelMasks.add(config->channel_mask);
    profile->mFormats.add(config->format);

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(String8(""), device);
    devDesc->mAddress = address;
    profile->mSupportedDevices.add(devDesc);

    mOutputProfiles.add(profile);

    return NO_ERROR;
}

status_t AudioPolicyManager::HwModule::removeOutputProfile(String8 name)
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        if (mOutputProfiles[i]->mName == name) {
            mOutputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}

status_t AudioPolicyManager::HwModule::addInputProfile(String8 name, const audio_config_t *config,
                                                  audio_devices_t device, String8 address)
{
    sp<IOProfile> profile = new IOProfile(name, AUDIO_PORT_ROLE_SINK, this);

    profile->mSamplingRates.add(config->sample_rate);
    profile->mChannelMasks.add(config->channel_mask);
    profile->mFormats.add(config->format);

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(String8(""), device);
    devDesc->mAddress = address;
    profile->mSupportedDevices.add(devDesc);

    ALOGV("addInputProfile() name %s rate %d mask 0x08", name.string(), config->sample_rate, config->channel_mask);

    mInputProfiles.add(profile);

    return NO_ERROR;
}

status_t AudioPolicyManager::HwModule::removeInputProfile(String8 name)
{
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        if (mInputProfiles[i]->mName == name) {
            mInputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}


void AudioPolicyManager::HwModule::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "  - name: %s\n", mName);
    result.append(buffer);
    snprintf(buffer, SIZE, "  - handle: %d\n", mHandle);
    result.append(buffer);
    snprintf(buffer, SIZE, "  - version: %u.%u\n", mHalVersion >> 8, mHalVersion & 0xFF);
    result.append(buffer);
    write(fd, result.string(), result.size());
    if (mOutputProfiles.size()) {
        write(fd, "  - outputs:\n", strlen("  - outputs:\n"));
        for (size_t i = 0; i < mOutputProfiles.size(); i++) {
            snprintf(buffer, SIZE, "    output %zu:\n", i);
            write(fd, buffer, strlen(buffer));
            mOutputProfiles[i]->dump(fd);
        }
    }
    if (mInputProfiles.size()) {
        write(fd, "  - inputs:\n", strlen("  - inputs:\n"));
        for (size_t i = 0; i < mInputProfiles.size(); i++) {
            snprintf(buffer, SIZE, "    input %zu:\n", i);
            write(fd, buffer, strlen(buffer));
            mInputProfiles[i]->dump(fd);
        }
    }
    if (mDeclaredDevices.size()) {
        write(fd, "  - devices:\n", strlen("  - devices:\n"));
        for (size_t i = 0; i < mDeclaredDevices.size(); i++) {
            mDeclaredDevices[i]->dump(fd, 4, i);
        }
    }
}

// --- AudioPort class implementation


AudioPolicyManager::AudioPort::AudioPort(const String8& name, audio_port_type_t type,
          audio_port_role_t role, const sp<HwModule>& module) :
    mName(name), mType(type), mRole(role), mModule(module), mFlags(0)
{
    mUseInChannelMask = ((type == AUDIO_PORT_TYPE_DEVICE) && (role == AUDIO_PORT_ROLE_SOURCE)) ||
                    ((type == AUDIO_PORT_TYPE_MIX) && (role == AUDIO_PORT_ROLE_SINK));
}

void AudioPolicyManager::AudioPort::toAudioPort(struct audio_port *port) const
{
    port->role = mRole;
    port->type = mType;
    unsigned int i;
    for (i = 0; i < mSamplingRates.size() && i < AUDIO_PORT_MAX_SAMPLING_RATES; i++) {
        if (mSamplingRates[i] != 0) {
            port->sample_rates[i] = mSamplingRates[i];
        }
    }
    port->num_sample_rates = i;
    for (i = 0; i < mChannelMasks.size() && i < AUDIO_PORT_MAX_CHANNEL_MASKS; i++) {
        if (mChannelMasks[i] != 0) {
            port->channel_masks[i] = mChannelMasks[i];
        }
    }
    port->num_channel_masks = i;
    for (i = 0; i < mFormats.size() && i < AUDIO_PORT_MAX_FORMATS; i++) {
        if (mFormats[i] != 0) {
            port->formats[i] = mFormats[i];
        }
    }
    port->num_formats = i;

    ALOGV("AudioPort::toAudioPort() num gains %zu", mGains.size());

    for (i = 0; i < mGains.size() && i < AUDIO_PORT_MAX_GAINS; i++) {
        port->gains[i] = mGains[i]->mGain;
    }
    port->num_gains = i;
}

void AudioPolicyManager::AudioPort::importAudioPort(const sp<AudioPort> port) {
    for (size_t k = 0 ; k < port->mSamplingRates.size() ; k++) {
        const uint32_t rate = port->mSamplingRates.itemAt(k);
        if (rate != 0) { // skip "dynamic" rates
            bool hasRate = false;
            for (size_t l = 0 ; l < mSamplingRates.size() ; l++) {
                if (rate == mSamplingRates.itemAt(l)) {
                    hasRate = true;
                    break;
                }
            }
            if (!hasRate) { // never import a sampling rate twice
                mSamplingRates.add(rate);
            }
        }
    }
    for (size_t k = 0 ; k < port->mChannelMasks.size() ; k++) {
        const audio_channel_mask_t mask = port->mChannelMasks.itemAt(k);
        if (mask != 0) { // skip "dynamic" masks
            bool hasMask = false;
            for (size_t l = 0 ; l < mChannelMasks.size() ; l++) {
                if (mask == mChannelMasks.itemAt(l)) {
                    hasMask = true;
                    break;
                }
            }
            if (!hasMask) { // never import a channel mask twice
                mChannelMasks.add(mask);
            }
        }
    }
    for (size_t k = 0 ; k < port->mFormats.size() ; k++) {
        const audio_format_t format = port->mFormats.itemAt(k);
        if (format != 0) { // skip "dynamic" formats
            bool hasFormat = false;
            for (size_t l = 0 ; l < mFormats.size() ; l++) {
                if (format == mFormats.itemAt(l)) {
                    hasFormat = true;
                    break;
                }
            }
            if (!hasFormat) { // never import a channel mask twice
                mFormats.add(format);
            }
        }
    }
    for (size_t k = 0 ; k < port->mGains.size() ; k++) {
        sp<AudioGain> gain = port->mGains.itemAt(k);
        if (gain != 0) {
            bool hasGain = false;
            for (size_t l = 0 ; l < mGains.size() ; l++) {
                if (gain == mGains.itemAt(l)) {
                    hasGain = true;
                    break;
                }
            }
            if (!hasGain) { // never import a gain twice
                mGains.add(gain);
            }
        }
    }
}

void AudioPolicyManager::AudioPort::clearCapabilities() {
    mChannelMasks.clear();
    mFormats.clear();
    mSamplingRates.clear();
    mGains.clear();
}

void AudioPolicyManager::AudioPort::loadSamplingRates(char *name)
{
    char *str = strtok(name, "|");

    // by convention, "0' in the first entry in mSamplingRates indicates the supported sampling
    // rates should be read from the output stream after it is opened for the first time
    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0) {
        mSamplingRates.add(0);
        return;
    }

    while (str != NULL) {
        uint32_t rate = atoi(str);
        if (rate != 0) {
            ALOGV("loadSamplingRates() adding rate %d", rate);
            mSamplingRates.add(rate);
        }
        str = strtok(NULL, "|");
    }
}

void AudioPolicyManager::AudioPort::loadFormats(char *name)
{
    char *str = strtok(name, "|");

    // by convention, "0' in the first entry in mFormats indicates the supported formats
    // should be read from the output stream after it is opened for the first time
    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0) {
        mFormats.add(AUDIO_FORMAT_DEFAULT);
        return;
    }

    while (str != NULL) {
        audio_format_t format = (audio_format_t)stringToEnum(sFormatNameToEnumTable,
                                                             ARRAY_SIZE(sFormatNameToEnumTable),
                                                             str);
        if (format != AUDIO_FORMAT_DEFAULT) {
            mFormats.add(format);
        }
        str = strtok(NULL, "|");
    }
}

void AudioPolicyManager::AudioPort::loadInChannels(char *name)
{
    const char *str = strtok(name, "|");

    ALOGV("loadInChannels() %s", name);

    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0) {
        mChannelMasks.add(0);
        return;
    }

    while (str != NULL) {
        audio_channel_mask_t channelMask =
                (audio_channel_mask_t)stringToEnum(sInChannelsNameToEnumTable,
                                                   ARRAY_SIZE(sInChannelsNameToEnumTable),
                                                   str);
        if (channelMask != 0) {
            ALOGV("loadInChannels() adding channelMask %04x", channelMask);
            mChannelMasks.add(channelMask);
        }
        str = strtok(NULL, "|");
    }
}

void AudioPolicyManager::AudioPort::loadOutChannels(char *name)
{
    const char *str = strtok(name, "|");

    ALOGV("loadOutChannels() %s", name);

    // by convention, "0' in the first entry in mChannelMasks indicates the supported channel
    // masks should be read from the output stream after it is opened for the first time
    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0) {
        mChannelMasks.add(0);
        return;
    }

    while (str != NULL) {
        audio_channel_mask_t channelMask =
                (audio_channel_mask_t)stringToEnum(sOutChannelsNameToEnumTable,
                                                   ARRAY_SIZE(sOutChannelsNameToEnumTable),
                                                   str);
        if (channelMask != 0) {
            mChannelMasks.add(channelMask);
        }
        str = strtok(NULL, "|");
    }
    return;
}

audio_gain_mode_t AudioPolicyManager::AudioPort::loadGainMode(char *name)
{
    const char *str = strtok(name, "|");

    ALOGV("loadGainMode() %s", name);
    audio_gain_mode_t mode = 0;
    while (str != NULL) {
        mode |= (audio_gain_mode_t)stringToEnum(sGainModeNameToEnumTable,
                                                ARRAY_SIZE(sGainModeNameToEnumTable),
                                                str);
        str = strtok(NULL, "|");
    }
    return mode;
}

void AudioPolicyManager::AudioPort::loadGain(cnode *root, int index)
{
    cnode *node = root->first_child;

    sp<AudioGain> gain = new AudioGain(index, mUseInChannelMask);

    while (node) {
        if (strcmp(node->name, GAIN_MODE) == 0) {
            gain->mGain.mode = loadGainMode((char *)node->value);
        } else if (strcmp(node->name, GAIN_CHANNELS) == 0) {
            if (mUseInChannelMask) {
                gain->mGain.channel_mask =
                        (audio_channel_mask_t)stringToEnum(sInChannelsNameToEnumTable,
                                                           ARRAY_SIZE(sInChannelsNameToEnumTable),
                                                           (char *)node->value);
            } else {
                gain->mGain.channel_mask =
                        (audio_channel_mask_t)stringToEnum(sOutChannelsNameToEnumTable,
                                                           ARRAY_SIZE(sOutChannelsNameToEnumTable),
                                                           (char *)node->value);
            }
        } else if (strcmp(node->name, GAIN_MIN_VALUE) == 0) {
            gain->mGain.min_value = atoi((char *)node->value);
        } else if (strcmp(node->name, GAIN_MAX_VALUE) == 0) {
            gain->mGain.max_value = atoi((char *)node->value);
        } else if (strcmp(node->name, GAIN_DEFAULT_VALUE) == 0) {
            gain->mGain.default_value = atoi((char *)node->value);
        } else if (strcmp(node->name, GAIN_STEP_VALUE) == 0) {
            gain->mGain.step_value = atoi((char *)node->value);
        } else if (strcmp(node->name, GAIN_MIN_RAMP_MS) == 0) {
            gain->mGain.min_ramp_ms = atoi((char *)node->value);
        } else if (strcmp(node->name, GAIN_MAX_RAMP_MS) == 0) {
            gain->mGain.max_ramp_ms = atoi((char *)node->value);
        }
        node = node->next;
    }

    ALOGV("loadGain() adding new gain mode %08x channel mask %08x min mB %d max mB %d",
          gain->mGain.mode, gain->mGain.channel_mask, gain->mGain.min_value, gain->mGain.max_value);

    if (gain->mGain.mode == 0) {
        return;
    }
    mGains.add(gain);
}

void AudioPolicyManager::AudioPort::loadGains(cnode *root)
{
    cnode *node = root->first_child;
    int index = 0;
    while (node) {
        ALOGV("loadGains() loading gain %s", node->name);
        loadGain(node, index++);
        node = node->next;
    }
}

status_t AudioPolicyManager::AudioPort::checkExactSamplingRate(uint32_t samplingRate) const
{
    if (mSamplingRates.isEmpty()) {
        return NO_ERROR;
    }

    for (size_t i = 0; i < mSamplingRates.size(); i ++) {
        if (mSamplingRates[i] == samplingRate) {
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

status_t AudioPolicyManager::AudioPort::checkCompatibleSamplingRate(uint32_t samplingRate,
        uint32_t *updatedSamplingRate) const
{
    if (mSamplingRates.isEmpty()) {
        return NO_ERROR;
    }

    // Search for the closest supported sampling rate that is above (preferred)
    // or below (acceptable) the desired sampling rate, within a permitted ratio.
    // The sampling rates do not need to be sorted in ascending order.
    ssize_t maxBelow = -1;
    ssize_t minAbove = -1;
    uint32_t candidate;
    for (size_t i = 0; i < mSamplingRates.size(); i++) {
        candidate = mSamplingRates[i];
        if (candidate == samplingRate) {
            if (updatedSamplingRate != NULL) {
                *updatedSamplingRate = candidate;
            }
            return NO_ERROR;
        }
        // candidate < desired
        if (candidate < samplingRate) {
            if (maxBelow < 0 || candidate > mSamplingRates[maxBelow]) {
                maxBelow = i;
            }
        // candidate > desired
        } else {
            if (minAbove < 0 || candidate < mSamplingRates[minAbove]) {
                minAbove = i;
            }
        }
    }
    // This uses hard-coded knowledge about AudioFlinger resampling ratios.
    // TODO Move these assumptions out.
    static const uint32_t kMaxDownSampleRatio = 6;  // beyond this aliasing occurs
    static const uint32_t kMaxUpSampleRatio = 256;  // beyond this sample rate inaccuracies occur
                                                    // due to approximation by an int32_t of the
                                                    // phase increments
    // Prefer to down-sample from a higher sampling rate, as we get the desired frequency spectrum.
    if (minAbove >= 0) {
        candidate = mSamplingRates[minAbove];
        if (candidate / kMaxDownSampleRatio <= samplingRate) {
            if (updatedSamplingRate != NULL) {
                *updatedSamplingRate = candidate;
            }
            return NO_ERROR;
        }
    }
    // But if we have to up-sample from a lower sampling rate, that's OK.
    if (maxBelow >= 0) {
        candidate = mSamplingRates[maxBelow];
        if (candidate * kMaxUpSampleRatio >= samplingRate) {
            if (updatedSamplingRate != NULL) {
                *updatedSamplingRate = candidate;
            }
            return NO_ERROR;
        }
    }
    // leave updatedSamplingRate unmodified
    return BAD_VALUE;
}

status_t AudioPolicyManager::AudioPort::checkExactChannelMask(audio_channel_mask_t channelMask) const
{
    if (mChannelMasks.isEmpty()) {
        return NO_ERROR;
    }

    for (size_t i = 0; i < mChannelMasks.size(); i++) {
        if (mChannelMasks[i] == channelMask) {
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

status_t AudioPolicyManager::AudioPort::checkCompatibleChannelMask(audio_channel_mask_t channelMask)
        const
{
    if (mChannelMasks.isEmpty()) {
        return NO_ERROR;
    }

    const bool isRecordThread = mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SINK;
    for (size_t i = 0; i < mChannelMasks.size(); i ++) {
        // FIXME Does not handle multi-channel automatic conversions yet
        audio_channel_mask_t supported = mChannelMasks[i];
        if (supported == channelMask) {
            return NO_ERROR;
        }
        if (isRecordThread) {
            // This uses hard-coded knowledge that AudioFlinger can silently down-mix and up-mix.
            // FIXME Abstract this out to a table.
            if (((supported == AUDIO_CHANNEL_IN_FRONT_BACK || supported == AUDIO_CHANNEL_IN_STEREO)
                    && channelMask == AUDIO_CHANNEL_IN_MONO) ||
                (supported == AUDIO_CHANNEL_IN_MONO && (channelMask == AUDIO_CHANNEL_IN_FRONT_BACK
                    || channelMask == AUDIO_CHANNEL_IN_STEREO))) {
                return NO_ERROR;
            }
        }
    }
    return BAD_VALUE;
}

status_t AudioPolicyManager::AudioPort::checkFormat(audio_format_t format) const
{
    if (mFormats.isEmpty()) {
        return NO_ERROR;
    }

    for (size_t i = 0; i < mFormats.size(); i ++) {
        if (mFormats[i] == format) {
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}


uint32_t AudioPolicyManager::AudioPort::pickSamplingRate() const
{
    // special case for uninitialized dynamic profile
    if (mSamplingRates.size() == 1 && mSamplingRates[0] == 0) {
        return 0;
    }

    // For direct outputs, pick minimum sampling rate: this helps ensuring that the
    // channel count / sampling rate combination chosen will be supported by the connected
    // sink
    if ((mType == AUDIO_PORT_TYPE_MIX) && (mRole == AUDIO_PORT_ROLE_SOURCE) &&
            (mFlags & (AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD))) {
        uint32_t samplingRate = UINT_MAX;
        for (size_t i = 0; i < mSamplingRates.size(); i ++) {
            if ((mSamplingRates[i] < samplingRate) && (mSamplingRates[i] > 0)) {
                samplingRate = mSamplingRates[i];
            }
        }
        return (samplingRate == UINT_MAX) ? 0 : samplingRate;
    }

    uint32_t samplingRate = 0;
    uint32_t maxRate = MAX_MIXER_SAMPLING_RATE;

    // For mixed output and inputs, use max mixer sampling rates. Do not
    // limit sampling rate otherwise
    if (mType != AUDIO_PORT_TYPE_MIX) {
        maxRate = UINT_MAX;
    }
    for (size_t i = 0; i < mSamplingRates.size(); i ++) {
        if ((mSamplingRates[i] > samplingRate) && (mSamplingRates[i] <= maxRate)) {
            samplingRate = mSamplingRates[i];
        }
    }
    return samplingRate;
}

audio_channel_mask_t AudioPolicyManager::AudioPort::pickChannelMask() const
{
    // special case for uninitialized dynamic profile
    if (mChannelMasks.size() == 1 && mChannelMasks[0] == 0) {
        return AUDIO_CHANNEL_NONE;
    }
    audio_channel_mask_t channelMask = AUDIO_CHANNEL_NONE;

    // For direct outputs, pick minimum channel count: this helps ensuring that the
    // channel count / sampling rate combination chosen will be supported by the connected
    // sink
    if ((mType == AUDIO_PORT_TYPE_MIX) && (mRole == AUDIO_PORT_ROLE_SOURCE) &&
            (mFlags & (AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD))) {
        uint32_t channelCount = UINT_MAX;
        for (size_t i = 0; i < mChannelMasks.size(); i ++) {
            uint32_t cnlCount;
            if (mUseInChannelMask) {
                cnlCount = audio_channel_count_from_in_mask(mChannelMasks[i]);
            } else {
                cnlCount = audio_channel_count_from_out_mask(mChannelMasks[i]);
            }
            if ((cnlCount < channelCount) && (cnlCount > 0)) {
                channelMask = mChannelMasks[i];
                channelCount = cnlCount;
            }
        }
        return channelMask;
    }

    uint32_t channelCount = 0;
    uint32_t maxCount = MAX_MIXER_CHANNEL_COUNT;

    // For mixed output and inputs, use max mixer channel count. Do not
    // limit channel count otherwise
    if (mType != AUDIO_PORT_TYPE_MIX) {
        maxCount = UINT_MAX;
    }
    for (size_t i = 0; i < mChannelMasks.size(); i ++) {
        uint32_t cnlCount;
        if (mUseInChannelMask) {
            cnlCount = audio_channel_count_from_in_mask(mChannelMasks[i]);
        } else {
            cnlCount = audio_channel_count_from_out_mask(mChannelMasks[i]);
        }
        if ((cnlCount > channelCount) && (cnlCount <= maxCount)) {
            channelMask = mChannelMasks[i];
            channelCount = cnlCount;
        }
    }
    return channelMask;
}

/* format in order of increasing preference */
const audio_format_t AudioPolicyManager::AudioPort::sPcmFormatCompareTable[] = {
        AUDIO_FORMAT_DEFAULT,
        AUDIO_FORMAT_PCM_16_BIT,
        AUDIO_FORMAT_PCM_8_24_BIT,
        AUDIO_FORMAT_PCM_24_BIT_PACKED,
        AUDIO_FORMAT_PCM_32_BIT,
        AUDIO_FORMAT_PCM_FLOAT,
};

int AudioPolicyManager::AudioPort::compareFormats(audio_format_t format1,
                                                  audio_format_t format2)
{
    // NOTE: AUDIO_FORMAT_INVALID is also considered not PCM and will be compared equal to any
    // compressed format and better than any PCM format. This is by design of pickFormat()
    if (!audio_is_linear_pcm(format1)) {
        if (!audio_is_linear_pcm(format2)) {
            return 0;
        }
        return 1;
    }
    if (!audio_is_linear_pcm(format2)) {
        return -1;
    }

    int index1 = -1, index2 = -1;
    for (size_t i = 0;
            (i < ARRAY_SIZE(sPcmFormatCompareTable)) && ((index1 == -1) || (index2 == -1));
            i ++) {
        if (sPcmFormatCompareTable[i] == format1) {
            index1 = i;
        }
        if (sPcmFormatCompareTable[i] == format2) {
            index2 = i;
        }
    }
    // format1 not found => index1 < 0 => format2 > format1
    // format2 not found => index2 < 0 => format2 < format1
    return index1 - index2;
}

audio_format_t AudioPolicyManager::AudioPort::pickFormat() const
{
    // special case for uninitialized dynamic profile
    if (mFormats.size() == 1 && mFormats[0] == 0) {
        return AUDIO_FORMAT_DEFAULT;
    }

    audio_format_t format = AUDIO_FORMAT_DEFAULT;
    audio_format_t bestFormat =
            AudioPolicyManager::AudioPort::sPcmFormatCompareTable[
                ARRAY_SIZE(AudioPolicyManager::AudioPort::sPcmFormatCompareTable) - 1];
    // For mixed output and inputs, use best mixer output format. Do not
    // limit format otherwise
    if ((mType != AUDIO_PORT_TYPE_MIX) ||
            ((mRole == AUDIO_PORT_ROLE_SOURCE) &&
             (((mFlags & (AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) != 0)))) {
        bestFormat = AUDIO_FORMAT_INVALID;
    }

    for (size_t i = 0; i < mFormats.size(); i ++) {
        if ((compareFormats(mFormats[i], format) > 0) &&
                (compareFormats(mFormats[i], bestFormat) <= 0)) {
            format = mFormats[i];
        }
    }
    return format;
}

status_t AudioPolicyManager::AudioPort::checkGain(const struct audio_gain_config *gainConfig,
                                                  int index) const
{
    if (index < 0 || (size_t)index >= mGains.size()) {
        return BAD_VALUE;
    }
    return mGains[index]->checkConfig(gainConfig);
}

void AudioPolicyManager::AudioPort::dump(int fd, int spaces) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    if (mName.size() != 0) {
        snprintf(buffer, SIZE, "%*s- name: %s\n", spaces, "", mName.string());
        result.append(buffer);
    }

    if (mSamplingRates.size() != 0) {
        snprintf(buffer, SIZE, "%*s- sampling rates: ", spaces, "");
        result.append(buffer);
        for (size_t i = 0; i < mSamplingRates.size(); i++) {
            if (i == 0 && mSamplingRates[i] == 0) {
                snprintf(buffer, SIZE, "Dynamic");
            } else {
                snprintf(buffer, SIZE, "%d", mSamplingRates[i]);
            }
            result.append(buffer);
            result.append(i == (mSamplingRates.size() - 1) ? "" : ", ");
        }
        result.append("\n");
    }

    if (mChannelMasks.size() != 0) {
        snprintf(buffer, SIZE, "%*s- channel masks: ", spaces, "");
        result.append(buffer);
        for (size_t i = 0; i < mChannelMasks.size(); i++) {
            ALOGV("AudioPort::dump mChannelMasks %zu %08x", i, mChannelMasks[i]);

            if (i == 0 && mChannelMasks[i] == 0) {
                snprintf(buffer, SIZE, "Dynamic");
            } else {
                snprintf(buffer, SIZE, "0x%04x", mChannelMasks[i]);
            }
            result.append(buffer);
            result.append(i == (mChannelMasks.size() - 1) ? "" : ", ");
        }
        result.append("\n");
    }

    if (mFormats.size() != 0) {
        snprintf(buffer, SIZE, "%*s- formats: ", spaces, "");
        result.append(buffer);
        for (size_t i = 0; i < mFormats.size(); i++) {
            const char *formatStr = enumToString(sFormatNameToEnumTable,
                                                 ARRAY_SIZE(sFormatNameToEnumTable),
                                                 mFormats[i]);
            if (i == 0 && strcmp(formatStr, "") == 0) {
                snprintf(buffer, SIZE, "Dynamic");
            } else {
                snprintf(buffer, SIZE, "%s", formatStr);
            }
            result.append(buffer);
            result.append(i == (mFormats.size() - 1) ? "" : ", ");
        }
        result.append("\n");
    }
    write(fd, result.string(), result.size());
    if (mGains.size() != 0) {
        snprintf(buffer, SIZE, "%*s- gains:\n", spaces, "");
        write(fd, buffer, strlen(buffer) + 1);
        result.append(buffer);
        for (size_t i = 0; i < mGains.size(); i++) {
            mGains[i]->dump(fd, spaces + 2, i);
        }
    }
}

// --- AudioGain class implementation

AudioPolicyManager::AudioGain::AudioGain(int index, bool useInChannelMask)
{
    mIndex = index;
    mUseInChannelMask = useInChannelMask;
    memset(&mGain, 0, sizeof(struct audio_gain));
}

void AudioPolicyManager::AudioGain::getDefaultConfig(struct audio_gain_config *config)
{
    config->index = mIndex;
    config->mode = mGain.mode;
    config->channel_mask = mGain.channel_mask;
    if ((mGain.mode & AUDIO_GAIN_MODE_JOINT) == AUDIO_GAIN_MODE_JOINT) {
        config->values[0] = mGain.default_value;
    } else {
        uint32_t numValues;
        if (mUseInChannelMask) {
            numValues = audio_channel_count_from_in_mask(mGain.channel_mask);
        } else {
            numValues = audio_channel_count_from_out_mask(mGain.channel_mask);
        }
        for (size_t i = 0; i < numValues; i++) {
            config->values[i] = mGain.default_value;
        }
    }
    if ((mGain.mode & AUDIO_GAIN_MODE_RAMP) == AUDIO_GAIN_MODE_RAMP) {
        config->ramp_duration_ms = mGain.min_ramp_ms;
    }
}

status_t AudioPolicyManager::AudioGain::checkConfig(const struct audio_gain_config *config)
{
    if ((config->mode & ~mGain.mode) != 0) {
        return BAD_VALUE;
    }
    if ((config->mode & AUDIO_GAIN_MODE_JOINT) == AUDIO_GAIN_MODE_JOINT) {
        if ((config->values[0] < mGain.min_value) ||
                    (config->values[0] > mGain.max_value)) {
            return BAD_VALUE;
        }
    } else {
        if ((config->channel_mask & ~mGain.channel_mask) != 0) {
            return BAD_VALUE;
        }
        uint32_t numValues;
        if (mUseInChannelMask) {
            numValues = audio_channel_count_from_in_mask(config->channel_mask);
        } else {
            numValues = audio_channel_count_from_out_mask(config->channel_mask);
        }
        for (size_t i = 0; i < numValues; i++) {
            if ((config->values[i] < mGain.min_value) ||
                    (config->values[i] > mGain.max_value)) {
                return BAD_VALUE;
            }
        }
    }
    if ((config->mode & AUDIO_GAIN_MODE_RAMP) == AUDIO_GAIN_MODE_RAMP) {
        if ((config->ramp_duration_ms < mGain.min_ramp_ms) ||
                    (config->ramp_duration_ms > mGain.max_ramp_ms)) {
            return BAD_VALUE;
        }
    }
    return NO_ERROR;
}

void AudioPolicyManager::AudioGain::dump(int fd, int spaces, int index) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%*sGain %d:\n", spaces, "", index+1);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- mode: %08x\n", spaces, "", mGain.mode);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- channel_mask: %08x\n", spaces, "", mGain.channel_mask);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- min_value: %d mB\n", spaces, "", mGain.min_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- max_value: %d mB\n", spaces, "", mGain.max_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- default_value: %d mB\n", spaces, "", mGain.default_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- step_value: %d mB\n", spaces, "", mGain.step_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- min_ramp_ms: %d ms\n", spaces, "", mGain.min_ramp_ms);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- max_ramp_ms: %d ms\n", spaces, "", mGain.max_ramp_ms);
    result.append(buffer);

    write(fd, result.string(), result.size());
}

// --- AudioPortConfig class implementation

AudioPolicyManager::AudioPortConfig::AudioPortConfig()
{
    mSamplingRate = 0;
    mChannelMask = AUDIO_CHANNEL_NONE;
    mFormat = AUDIO_FORMAT_INVALID;
    mGain.index = -1;
}

status_t AudioPolicyManager::AudioPortConfig::applyAudioPortConfig(
                                                        const struct audio_port_config *config,
                                                        struct audio_port_config *backupConfig)
{
    struct audio_port_config localBackupConfig;
    status_t status = NO_ERROR;

    localBackupConfig.config_mask = config->config_mask;
    toAudioPortConfig(&localBackupConfig);

    sp<AudioPort> audioport = getAudioPort();
    if (audioport == 0) {
        status = NO_INIT;
        goto exit;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
        status = audioport->checkExactSamplingRate(config->sample_rate);
        if (status != NO_ERROR) {
            goto exit;
        }
        mSamplingRate = config->sample_rate;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
        status = audioport->checkExactChannelMask(config->channel_mask);
        if (status != NO_ERROR) {
            goto exit;
        }
        mChannelMask = config->channel_mask;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_FORMAT) {
        status = audioport->checkFormat(config->format);
        if (status != NO_ERROR) {
            goto exit;
        }
        mFormat = config->format;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_GAIN) {
        status = audioport->checkGain(&config->gain, config->gain.index);
        if (status != NO_ERROR) {
            goto exit;
        }
        mGain = config->gain;
    }

exit:
    if (status != NO_ERROR) {
        applyAudioPortConfig(&localBackupConfig);
    }
    if (backupConfig != NULL) {
        *backupConfig = localBackupConfig;
    }
    return status;
}

void AudioPolicyManager::AudioPortConfig::toAudioPortConfig(
                                                    struct audio_port_config *dstConfig,
                                                    const struct audio_port_config *srcConfig) const
{
    if (dstConfig->config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
        dstConfig->sample_rate = mSamplingRate;
        if ((srcConfig != NULL) && (srcConfig->config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE)) {
            dstConfig->sample_rate = srcConfig->sample_rate;
        }
    } else {
        dstConfig->sample_rate = 0;
    }
    if (dstConfig->config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
        dstConfig->channel_mask = mChannelMask;
        if ((srcConfig != NULL) && (srcConfig->config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK)) {
            dstConfig->channel_mask = srcConfig->channel_mask;
        }
    } else {
        dstConfig->channel_mask = AUDIO_CHANNEL_NONE;
    }
    if (dstConfig->config_mask & AUDIO_PORT_CONFIG_FORMAT) {
        dstConfig->format = mFormat;
        if ((srcConfig != NULL) && (srcConfig->config_mask & AUDIO_PORT_CONFIG_FORMAT)) {
            dstConfig->format = srcConfig->format;
        }
    } else {
        dstConfig->format = AUDIO_FORMAT_INVALID;
    }
    if (dstConfig->config_mask & AUDIO_PORT_CONFIG_GAIN) {
        dstConfig->gain = mGain;
        if ((srcConfig != NULL) && (srcConfig->config_mask & AUDIO_PORT_CONFIG_GAIN)) {
            dstConfig->gain = srcConfig->gain;
        }
    } else {
        dstConfig->gain.index = -1;
    }
    if (dstConfig->gain.index != -1) {
        dstConfig->config_mask |= AUDIO_PORT_CONFIG_GAIN;
    } else {
        dstConfig->config_mask &= ~AUDIO_PORT_CONFIG_GAIN;
    }
}

// --- IOProfile class implementation

AudioPolicyManager::IOProfile::IOProfile(const String8& name, audio_port_role_t role,
                                         const sp<HwModule>& module)
    : AudioPort(name, AUDIO_PORT_TYPE_MIX, role, module)
{
}

AudioPolicyManager::IOProfile::~IOProfile()
{
}

// checks if the IO profile is compatible with specified parameters.
// Sampling rate, format and channel mask must be specified in order to
// get a valid a match
bool AudioPolicyManager::IOProfile::isCompatibleProfile(audio_devices_t device,
                                                        String8 address,
                                                        uint32_t samplingRate,
                                                        uint32_t *updatedSamplingRate,
                                                        audio_format_t format,
                                                        audio_channel_mask_t channelMask,
                                                        uint32_t flags) const
{
    const bool isPlaybackThread = mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SOURCE;
    const bool isRecordThread = mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SINK;
    ALOGV("isPlaybackThread %d,isRecordThread %d", isPlaybackThread, isRecordThread);
    ALOG_ASSERT(isPlaybackThread != isRecordThread);

    if (device != AUDIO_DEVICE_NONE && mSupportedDevices.getDevice(device, address) == 0) {
        return false;
    }

    if (samplingRate == 0) {
         return false;
    }
    uint32_t myUpdatedSamplingRate = samplingRate;
    if (isPlaybackThread && checkExactSamplingRate(samplingRate) != NO_ERROR) {
         return false;
    }
    if (isRecordThread && checkCompatibleSamplingRate(samplingRate, &myUpdatedSamplingRate) !=
            NO_ERROR) {
         return false;
    }

    if (!audio_is_valid_format(format) || checkFormat(format) != NO_ERROR) {
        return false;
    }

    if (isPlaybackThread && (!audio_is_output_channel(channelMask) ||
            checkExactChannelMask(channelMask) != NO_ERROR)) {
        return false;
    }
    if (isRecordThread && (!audio_is_input_channel(channelMask) ||
            checkCompatibleChannelMask(channelMask) != NO_ERROR)) {
        return false;
    }

    if (isPlaybackThread) {
        if ((mFlags & flags) != flags) {
            return false;
#ifdef HDMI_PASSTHROUGH_ENABLED
        } else {
            // Work around to check for passthrough output support.
            // We are using same flags as compress offload with addition
            // of passthrough flag. So we need to have an additional check
            // if the input flags has passthrough
            if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH) {
                if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH) == 0) {
                    ALOGE("isCompatibleProfile:flags not equal, Not passthrough");
                    return false;
                }
            }
#endif
      }
    }
    // The only input flag that is allowed to be different is the fast flag.
    // An existing fast stream is compatible with a normal track request.
    // An existing normal stream is compatible with a fast track request,
    // but the fast request will be denied by AudioFlinger and converted to normal track.
    if (isRecordThread && ((mFlags ^ flags) &
            ~AUDIO_INPUT_FLAG_FAST)) {
        return false;
    }
    if (updatedSamplingRate != NULL) {
        *updatedSamplingRate = myUpdatedSamplingRate;
    }
    return true;
}

void AudioPolicyManager::IOProfile::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    AudioPort::dump(fd, 4);

    snprintf(buffer, SIZE, "    - flags: 0x%04x\n", mFlags);
    result.append(buffer);
    snprintf(buffer, SIZE, "    - devices:\n");
    result.append(buffer);
    write(fd, result.string(), result.size());
    for (size_t i = 0; i < mSupportedDevices.size(); i++) {
        mSupportedDevices[i]->dump(fd, 6, i);
    }
}

void AudioPolicyManager::IOProfile::log()
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    ALOGV("    - sampling rates: ");
    for (size_t i = 0; i < mSamplingRates.size(); i++) {
        ALOGV("  %d", mSamplingRates[i]);
    }

    ALOGV("    - channel masks: ");
    for (size_t i = 0; i < mChannelMasks.size(); i++) {
        ALOGV("  0x%04x", mChannelMasks[i]);
    }

    ALOGV("    - formats: ");
    for (size_t i = 0; i < mFormats.size(); i++) {
        ALOGV("  0x%08x", mFormats[i]);
    }

    ALOGV("    - devices: 0x%04x\n", mSupportedDevices.types());
    ALOGV("    - flags: 0x%04x\n", mFlags);
}


// --- DeviceDescriptor implementation


AudioPolicyManager::DeviceDescriptor::DeviceDescriptor(const String8& name, audio_devices_t type) :
                     AudioPort(name, AUDIO_PORT_TYPE_DEVICE,
                               audio_is_output_device(type) ? AUDIO_PORT_ROLE_SINK :
                                                              AUDIO_PORT_ROLE_SOURCE,
                             NULL),
                     mDeviceType(type), mAddress(""), mId(0)
{
}

bool AudioPolicyManager::DeviceDescriptor::equals(const sp<DeviceDescriptor>& other) const
{
    // Devices are considered equal if they:
    // - are of the same type (a device type cannot be AUDIO_DEVICE_NONE)
    // - have the same address or one device does not specify the address
    // - have the same channel mask or one device does not specify the channel mask
    return (mDeviceType == other->mDeviceType) &&
           (mAddress == "" || other->mAddress == "" || mAddress == other->mAddress) &&
           (mChannelMask == 0 || other->mChannelMask == 0 ||
                mChannelMask == other->mChannelMask);
}

void AudioPolicyManager::DeviceDescriptor::loadGains(cnode *root)
{
    AudioPort::loadGains(root);
    if (mGains.size() > 0) {
        mGains[0]->getDefaultConfig(&mGain);
    }
}


void AudioPolicyManager::DeviceVector::refreshTypes()
{
    mDeviceTypes = AUDIO_DEVICE_NONE;
    for(size_t i = 0; i < size(); i++) {
        mDeviceTypes |= itemAt(i)->mDeviceType;
    }
    ALOGV("DeviceVector::refreshTypes() mDeviceTypes %08x", mDeviceTypes);
}

ssize_t AudioPolicyManager::DeviceVector::indexOf(const sp<DeviceDescriptor>& item) const
{
    for(size_t i = 0; i < size(); i++) {
        if (item->equals(itemAt(i))) {
            return i;
        }
    }
    return -1;
}

ssize_t AudioPolicyManager::DeviceVector::add(const sp<DeviceDescriptor>& item)
{
    ssize_t ret = indexOf(item);

    if (ret < 0) {
        ret = SortedVector::add(item);
        if (ret >= 0) {
            refreshTypes();
        }
    } else {
        ALOGW("DeviceVector::add device %08x already in", item->mDeviceType);
        ret = -1;
    }
    return ret;
}

ssize_t AudioPolicyManager::DeviceVector::remove(const sp<DeviceDescriptor>& item)
{
    size_t i;
    ssize_t ret = indexOf(item);

    if (ret < 0) {
        ALOGW("DeviceVector::remove device %08x not in", item->mDeviceType);
    } else {
        ret = SortedVector::removeAt(ret);
        if (ret >= 0) {
            refreshTypes();
        }
    }
    return ret;
}

void AudioPolicyManager::DeviceVector::loadDevicesFromType(audio_devices_t types)
{
    DeviceVector deviceList;

    uint32_t role_bit = AUDIO_DEVICE_BIT_IN & types;
    types &= ~role_bit;

    while (types) {
        uint32_t i = 31 - __builtin_clz(types);
        uint32_t type = 1 << i;
        types &= ~type;
        add(new DeviceDescriptor(String8(""), type | role_bit));
    }
}

void AudioPolicyManager::DeviceVector::loadDevicesFromName(char *name,
                                                           const DeviceVector& declaredDevices)
{
    char *devName = strtok(name, "|");
    while (devName != NULL) {
        if (strlen(devName) != 0) {
            audio_devices_t type = stringToEnum(sDeviceNameToEnumTable,
                                 ARRAY_SIZE(sDeviceNameToEnumTable),
                                 devName);
            if (type != AUDIO_DEVICE_NONE) {
                sp<DeviceDescriptor> dev = new DeviceDescriptor(String8(""), type);
                if (type == AUDIO_DEVICE_IN_REMOTE_SUBMIX ||
                        type == AUDIO_DEVICE_OUT_REMOTE_SUBMIX ) {
                    dev->mAddress = String8("0");
                }
                add(dev);
            } else {
                sp<DeviceDescriptor> deviceDesc =
                        declaredDevices.getDeviceFromName(String8(devName));
                if (deviceDesc != 0) {
                    add(deviceDesc);
                }
            }
         }
         devName = strtok(NULL, "|");
     }
}

sp<AudioPolicyManager::DeviceDescriptor> AudioPolicyManager::DeviceVector::getDevice(
                                                        audio_devices_t type, String8 address) const
{
    sp<DeviceDescriptor> device;
    for (size_t i = 0; i < size(); i++) {
        if (itemAt(i)->mDeviceType == type) {
            if (address == "" || itemAt(i)->mAddress == address) {
                device = itemAt(i);
                if (itemAt(i)->mAddress == address) {
                    break;
                }
            }
        }
    }
    ALOGV("DeviceVector::getDevice() for type %08x address %s found %p",
          type, address.string(), device.get());
    return device;
}

sp<AudioPolicyManager::DeviceDescriptor> AudioPolicyManager::DeviceVector::getDeviceFromId(
                                                                    audio_port_handle_t id) const
{
    sp<DeviceDescriptor> device;
    for (size_t i = 0; i < size(); i++) {
        ALOGV("DeviceVector::getDeviceFromId(%d) itemAt(%zu)->mId %d", id, i, itemAt(i)->mId);
        if (itemAt(i)->mId == id) {
            device = itemAt(i);
            break;
        }
    }
    return device;
}

AudioPolicyManager::DeviceVector AudioPolicyManager::DeviceVector::getDevicesFromType(
                                                                        audio_devices_t type) const
{
    DeviceVector devices;
    bool isOutput = audio_is_output_devices(type);
    type &= ~AUDIO_DEVICE_BIT_IN;
    for (size_t i = 0; (i < size()) && (type != AUDIO_DEVICE_NONE); i++) {
        bool curIsOutput = audio_is_output_devices(itemAt(i)->mDeviceType);
        audio_devices_t curType = itemAt(i)->mDeviceType & ~AUDIO_DEVICE_BIT_IN;
        if ((isOutput == curIsOutput) && ((type & curType) != 0)) {
            devices.add(itemAt(i));
            type &= ~curType;
            ALOGV("DeviceVector::getDevicesFromType() for type %x found %p",
                  itemAt(i)->mDeviceType, itemAt(i).get());
        }
    }
    return devices;
}

AudioPolicyManager::DeviceVector AudioPolicyManager::DeviceVector::getDevicesFromTypeAddr(
        audio_devices_t type, String8 address) const
{
    DeviceVector devices;
    for (size_t i = 0; i < size(); i++) {
        if (itemAt(i)->mDeviceType == type) {
            if (itemAt(i)->mAddress == address) {
                devices.add(itemAt(i));
            }
        }
    }
    return devices;
}

sp<AudioPolicyManager::DeviceDescriptor> AudioPolicyManager::DeviceVector::getDeviceFromName(
        const String8& name) const
{
    sp<DeviceDescriptor> device;
    for (size_t i = 0; i < size(); i++) {
        if (itemAt(i)->mName == name) {
            device = itemAt(i);
            break;
        }
    }
    return device;
}

void AudioPolicyManager::DeviceDescriptor::toAudioPortConfig(
                                                    struct audio_port_config *dstConfig,
                                                    const struct audio_port_config *srcConfig) const
{
    dstConfig->config_mask = AUDIO_PORT_CONFIG_CHANNEL_MASK|AUDIO_PORT_CONFIG_GAIN;
    if (srcConfig != NULL) {
        dstConfig->config_mask |= srcConfig->config_mask;
    }

    AudioPortConfig::toAudioPortConfig(dstConfig, srcConfig);

    dstConfig->id = mId;
    dstConfig->role = audio_is_output_device(mDeviceType) ?
                        AUDIO_PORT_ROLE_SINK : AUDIO_PORT_ROLE_SOURCE;
    dstConfig->type = AUDIO_PORT_TYPE_DEVICE;
    dstConfig->ext.device.type = mDeviceType;
    dstConfig->ext.device.hw_module = mModule->mHandle;
    strncpy(dstConfig->ext.device.address, mAddress.string(), AUDIO_DEVICE_MAX_ADDRESS_LEN);
}

void AudioPolicyManager::DeviceDescriptor::toAudioPort(struct audio_port *port) const
{
    ALOGV("DeviceDescriptor::toAudioPort() handle %d type %x", mId, mDeviceType);
    AudioPort::toAudioPort(port);
    port->id = mId;
    toAudioPortConfig(&port->active_config);
    port->ext.device.type = mDeviceType;
    port->ext.device.hw_module = mModule->mHandle;
    strncpy(port->ext.device.address, mAddress.string(), AUDIO_DEVICE_MAX_ADDRESS_LEN);
}

status_t AudioPolicyManager::DeviceDescriptor::dump(int fd, int spaces, int index) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%*sDevice %d:\n", spaces, "", index+1);
    result.append(buffer);
    if (mId != 0) {
        snprintf(buffer, SIZE, "%*s- id: %2d\n", spaces, "", mId);
        result.append(buffer);
    }
    snprintf(buffer, SIZE, "%*s- type: %-48s\n", spaces, "",
                                              enumToString(sDeviceNameToEnumTable,
                                                           ARRAY_SIZE(sDeviceNameToEnumTable),
                                                           mDeviceType));
    result.append(buffer);
    if (mAddress.size() != 0) {
        snprintf(buffer, SIZE, "%*s- address: %-32s\n", spaces, "", mAddress.string());
        result.append(buffer);
    }
    write(fd, result.string(), result.size());
    AudioPort::dump(fd, spaces);

    return NO_ERROR;
}

status_t AudioPolicyManager::AudioPatch::dump(int fd, int spaces, int index) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;


    snprintf(buffer, SIZE, "%*sAudio patch %d:\n", spaces, "", index+1);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- handle: %2d\n", spaces, "", mHandle);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- audio flinger handle: %2d\n", spaces, "", mAfPatchHandle);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- owner uid: %2d\n", spaces, "", mUid);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- %d sources:\n", spaces, "", mPatch.num_sources);
    result.append(buffer);
    for (size_t i = 0; i < mPatch.num_sources; i++) {
        if (mPatch.sources[i].type == AUDIO_PORT_TYPE_DEVICE) {
            snprintf(buffer, SIZE, "%*s- Device ID %d %s\n", spaces + 2, "",
                     mPatch.sources[i].id, enumToString(sDeviceNameToEnumTable,
                                                        ARRAY_SIZE(sDeviceNameToEnumTable),
                                                        mPatch.sources[i].ext.device.type));
        } else {
            snprintf(buffer, SIZE, "%*s- Mix ID %d I/O handle %d\n", spaces + 2, "",
                     mPatch.sources[i].id, mPatch.sources[i].ext.mix.handle);
        }
        result.append(buffer);
    }
    snprintf(buffer, SIZE, "%*s- %d sinks:\n", spaces, "", mPatch.num_sinks);
    result.append(buffer);
    for (size_t i = 0; i < mPatch.num_sinks; i++) {
        if (mPatch.sinks[i].type == AUDIO_PORT_TYPE_DEVICE) {
            snprintf(buffer, SIZE, "%*s- Device ID %d %s\n", spaces + 2, "",
                     mPatch.sinks[i].id, enumToString(sDeviceNameToEnumTable,
                                                        ARRAY_SIZE(sDeviceNameToEnumTable),
                                                        mPatch.sinks[i].ext.device.type));
        } else {
            snprintf(buffer, SIZE, "%*s- Mix ID %d I/O handle %d\n", spaces + 2, "",
                     mPatch.sinks[i].id, mPatch.sinks[i].ext.mix.handle);
        }
        result.append(buffer);
    }

    write(fd, result.string(), result.size());
    return NO_ERROR;
}

// --- audio_policy.conf file parsing

uint32_t AudioPolicyManager::parseOutputFlagNames(char *name)
{
    uint32_t flag = 0;

    // it is OK to cast name to non const here as we are not going to use it after
    // strtok() modifies it
    char *flagName = strtok(name, "|");
    while (flagName != NULL) {
        if (strlen(flagName) != 0) {
            flag |= stringToEnum(sOutputFlagNameToEnumTable,
                               ARRAY_SIZE(sOutputFlagNameToEnumTable),
                               flagName);
        }
        flagName = strtok(NULL, "|");
    }
    //force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((flag & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        flag |= AUDIO_OUTPUT_FLAG_DIRECT;
    }

    return flag;
}

uint32_t AudioPolicyManager::parseInputFlagNames(char *name)
{
    uint32_t flag = 0;

    // it is OK to cast name to non const here as we are not going to use it after
    // strtok() modifies it
    char *flagName = strtok(name, "|");
    while (flagName != NULL) {
        if (strlen(flagName) != 0) {
            flag |= stringToEnum(sInputFlagNameToEnumTable,
                               ARRAY_SIZE(sInputFlagNameToEnumTable),
                               flagName);
        }
        flagName = strtok(NULL, "|");
    }
    return flag;
}

audio_devices_t AudioPolicyManager::parseDeviceNames(char *name)
{
    uint32_t device = 0;

    char *devName = strtok(name, "|");
    while (devName != NULL) {
        if (strlen(devName) != 0) {
            device |= stringToEnum(sDeviceNameToEnumTable,
                                 ARRAY_SIZE(sDeviceNameToEnumTable),
                                 devName);
         }
        devName = strtok(NULL, "|");
     }
    return device;
}

void AudioPolicyManager::loadHwModule(cnode *root)
{
    status_t status = NAME_NOT_FOUND;
    cnode *node;
    sp<HwModule> module = new HwModule(root->name);

    node = config_find(root, DEVICES_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading device %s", node->name);
            status_t tmpStatus = module->loadDevice(node);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    node = config_find(root, OUTPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading output %s", node->name);
            status_t tmpStatus = module->loadOutput(node);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    node = config_find(root, INPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading input %s", node->name);
            status_t tmpStatus = module->loadInput(node);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    loadGlobalConfig(root, module);

    if (status == NO_ERROR) {
        mHwModules.add(module);
    }
}

void AudioPolicyManager::loadHwModules(cnode *root)
{
    cnode *node = config_find(root, AUDIO_HW_MODULE_TAG);
    if (node == NULL) {
        return;
    }

    node = node->first_child;
    while (node) {
        ALOGV("loadHwModules() loading module %s", node->name);
        loadHwModule(node);
        node = node->next;
    }
}

void AudioPolicyManager::loadGlobalConfig(cnode *root, const sp<HwModule>& module)
{
    cnode *node = config_find(root, GLOBAL_CONFIG_TAG);

    if (node == NULL) {
        return;
    }
    DeviceVector declaredDevices;
    if (module != NULL) {
        declaredDevices = module->mDeclaredDevices;
    }

    node = node->first_child;
    while (node) {
        if (strcmp(ATTACHED_OUTPUT_DEVICES_TAG, node->name) == 0) {
            mAvailableOutputDevices.loadDevicesFromName((char *)node->value,
                                                        declaredDevices);
            ALOGV("loadGlobalConfig() Attached Output Devices %08x",
                  mAvailableOutputDevices.types());
        } else if (strcmp(DEFAULT_OUTPUT_DEVICE_TAG, node->name) == 0) {
            audio_devices_t device = (audio_devices_t)stringToEnum(sDeviceNameToEnumTable,
                                              ARRAY_SIZE(sDeviceNameToEnumTable),
                                              (char *)node->value);
            if (device != AUDIO_DEVICE_NONE) {
                mDefaultOutputDevice = new DeviceDescriptor(String8(""), device);
            } else {
                ALOGW("loadGlobalConfig() default device not specified");
            }
            ALOGV("loadGlobalConfig() mDefaultOutputDevice %08x", mDefaultOutputDevice->mDeviceType);
        } else if (strcmp(ATTACHED_INPUT_DEVICES_TAG, node->name) == 0) {
            mAvailableInputDevices.loadDevicesFromName((char *)node->value,
                                                       declaredDevices);
            ALOGV("loadGlobalConfig() Available InputDevices %08x", mAvailableInputDevices.types());
        } else if (strcmp(SPEAKER_DRC_ENABLED_TAG, node->name) == 0) {
            mSpeakerDrcEnabled = stringToBool((char *)node->value);
            ALOGV("loadGlobalConfig() mSpeakerDrcEnabled = %d", mSpeakerDrcEnabled);
        } else if (strcmp(AUDIO_HAL_VERSION_TAG, node->name) == 0) {
            uint32_t major, minor;
            sscanf((char *)node->value, "%u.%u", &major, &minor);
            module->mHalVersion = HARDWARE_DEVICE_API_VERSION(major, minor);
            ALOGV("loadGlobalConfig() mHalVersion = %04x major %u minor %u",
                  module->mHalVersion, major, minor);
        }
        node = node->next;
    }
}

status_t AudioPolicyManager::loadAudioPolicyConfig(const char *path)
{
    cnode *root;
    char *data;

    data = (char *)load_file(path, NULL);
    if (data == NULL) {
        return -ENODEV;
    }
    root = config_node("", "");
    config_load(root, data);

    loadHwModules(root);
    // legacy audio_policy.conf files have one global_configuration section
    loadGlobalConfig(root, getModuleFromName(AUDIO_HARDWARE_MODULE_ID_PRIMARY));
    config_free(root);
    free(root);
    free(data);

    ALOGI("loadAudioPolicyConfig() loaded %s\n", path);

    return NO_ERROR;
}

void AudioPolicyManager::defaultAudioPolicyConfig(void)
{
    sp<HwModule> module;
    sp<IOProfile> profile;
    sp<DeviceDescriptor> defaultInputDevice = new DeviceDescriptor(String8(""),
                                                                   AUDIO_DEVICE_IN_BUILTIN_MIC);
    mAvailableOutputDevices.add(mDefaultOutputDevice);
    mAvailableInputDevices.add(defaultInputDevice);

    module = new HwModule("primary");

    profile = new IOProfile(String8("primary"), AUDIO_PORT_ROLE_SOURCE, module);
    profile->mSamplingRates.add(44100);
    profile->mFormats.add(AUDIO_FORMAT_PCM_16_BIT);
    profile->mChannelMasks.add(AUDIO_CHANNEL_OUT_STEREO);
    profile->mSupportedDevices.add(mDefaultOutputDevice);
    profile->mFlags = AUDIO_OUTPUT_FLAG_PRIMARY;
    module->mOutputProfiles.add(profile);

    profile = new IOProfile(String8("primary"), AUDIO_PORT_ROLE_SINK, module);
    profile->mSamplingRates.add(8000);
    profile->mFormats.add(AUDIO_FORMAT_PCM_16_BIT);
    profile->mChannelMasks.add(AUDIO_CHANNEL_IN_MONO);
    profile->mSupportedDevices.add(defaultInputDevice);
    module->mInputProfiles.add(profile);

    mHwModules.add(module);
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
        if (isStreamActive(AUDIO_STREAM_ALARM)) {
            return AUDIO_STREAM_ALARM;
        }
        if (isStreamActive(AUDIO_STREAM_RING)) {
            return AUDIO_STREAM_RING;
        }
        if (isInCall()) {
            return AUDIO_STREAM_VOICE_CALL;
        }
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

#ifdef HDMI_PASSTHROUGH_ENABLED

void AudioPolicyManager::checkAndSuspendOutputs() {

    sp<AudioOutputDescriptor> desc;

    if (!isHDMIPassthroughEnabled()) {
        ALOGV("checkAndSuspendOutputs: passthrough not enabled");
        return;
    }

    for (size_t i = 0; i < mOutputs.size(); i++) {
        desc = mOutputs.valueAt(i);
        ALOGV("checkAndSuspendOutputs:device 0x%x, flag %x, music refcount %d",
             desc->mDevice, desc->mFlags, desc->mRefCount[AUDIO_STREAM_MUSIC]);
        if (desc->mDevice & AUDIO_DEVICE_OUT_AUX_DIGITAL ||
            desc->mDevice == AUDIO_DEVICE_NONE) {
            // check if  fast/deep buffer/multichannel outputs are already
            // suspended before suspending the output
            if (((desc->mFlags &
                        AUDIO_OUTPUT_FLAG_FAST) &&
                        !mFastSuspended) ||
                ((desc->mFlags &
                        AUDIO_OUTPUT_FLAG_DEEP_BUFFER) &&
                        !mPrimarySuspended) ||
                ((desc->mFlags &
                        AUDIO_OUTPUT_FLAG_PRIMARY) &&
                        !mPrimarySuspended) ||
                ((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) &&
                        !(desc->mFlags &  AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
                        !mMultiChannelSuspended)) {
                /*TODO : is other streams needed here. */
                ALOGD("suspend Ouput");
                mpClientInterface->suspendOutput(mOutputs.keyAt(i));
                // Update the reference count of fast/deep buffer/multichannel
                // after suspend.
                if (desc->mFlags &
                    AUDIO_OUTPUT_FLAG_DEEP_BUFFER ||
                    desc->mFlags &
                    AUDIO_OUTPUT_FLAG_PRIMARY) {
                    mPrimarySuspended++;
                } else if (desc->mFlags &
                    AUDIO_OUTPUT_FLAG_FAST) {
                    mFastSuspended++;
                } else if ((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) &&
                    !(desc->mFlags &  AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) {
                    mMultiChannelSuspended++;
                }
            } else {
                continue;
                ALOGD("Ignore  suspend");
            }
        }
    }
    ALOGV("Suspend count: primary %d, fast %d, multichannel %d",
           mPrimarySuspended, mFastSuspended, mMultiChannelSuspended);
}

void AudioPolicyManager::checkAndRestoreOutputs() {

    sp<AudioOutputDescriptor> desc;

    if (!isHDMIPassthroughEnabled()) {
        ALOGV("checkAndRestoreOutputs: passthrough not enabled");
        return;
    }

    for (size_t i = 0; i < mOutputs.size(); i++) {
        desc = mOutputs.valueAt(i);
        ALOGV("checkAndRestoreOutputs: device 0x%x, flag %x, music refcount %d",
             desc->mDevice, desc->mFlags, desc->mRefCount[AUDIO_STREAM_MUSIC]);
        if (desc->mDevice & AUDIO_DEVICE_OUT_AUX_DIGITAL ||
            desc->mDevice == AUDIO_DEVICE_NONE) {
            // check if the deep buffer/fast/multichannel outputs were
            // suspended before restore.
            if (((desc->mFlags &
                    AUDIO_OUTPUT_FLAG_FAST) &&
                    mFastSuspended) ||
                ((desc->mFlags &
                    AUDIO_OUTPUT_FLAG_DEEP_BUFFER) &&
                    mPrimarySuspended) ||
                ((desc->mFlags &
                    AUDIO_OUTPUT_FLAG_PRIMARY) &&
                    mPrimarySuspended) ||
                ((desc->mFlags &  AUDIO_OUTPUT_FLAG_DIRECT) &&
                    !(desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
                    mMultiChannelSuspended)) {

                ALOGD("restore Output");
                mpClientInterface->restoreOutput(mOutputs.keyAt(i));
                // update the reference count for fast/deep buffer/multichannel
                // after restore.
                if (desc->mFlags &
                    AUDIO_OUTPUT_FLAG_DEEP_BUFFER ||
                    desc->mFlags &
                    AUDIO_OUTPUT_FLAG_PRIMARY) {
                    mPrimarySuspended--;
                } else if (desc->mFlags &
                    AUDIO_OUTPUT_FLAG_FAST) {
                    mFastSuspended--;
                } else if ((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) &&
                    !(desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) {
                    mMultiChannelSuspended--;
                }
            } else {
                continue;
                ALOGD("Ignore  restore");
            }
        }
    }
    ALOGV("Restore count: primary %d, fast %d, multichannel %d",
          mPrimarySuspended, mFastSuspended, mMultiChannelSuspended);
}

audio_devices_t AudioPolicyManager::handleHDMIPassthrough(audio_devices_t device,
                         audio_io_handle_t output, int audio_stream,
                         int audio_strategy) {

    routing_strategy strategy = (routing_strategy)audio_strategy;
    audio_stream_type_t stream =  (audio_stream_type_t)audio_stream;

    if (!isHDMIPassthroughEnabled()) {
        ALOGV("handleHDMIPassthrough: passthrough not enabled");
        return device;
    }

    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("handleHDMIPassthrough() unknow output %d, return same device",
               output);
        return device;
    }

    sp<AudioOutputDescriptor> outputDesc = mOutputs.valueAt(index);

    ALOGV("handleHDMIPassthrough:device %x, flags %d, stream %d, strategy %d",
           device, outputDesc->mFlags, stream, strategy);
    // check for HDMI device
    if (device & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
        // check for deep buffer and fast flags
        if ((outputDesc->mFlags &
            AUDIO_OUTPUT_FLAG_FAST) ||
            (outputDesc->mFlags &
            AUDIO_OUTPUT_FLAG_PRIMARY) ||
            (outputDesc->mFlags &
            AUDIO_OUTPUT_FLAG_DEEP_BUFFER)) {
            //Stream info not available, so need to check for ref count in desc
            if (stream == -1) {
                if ((outputDesc->mRefCount[AUDIO_STREAM_SYSTEM] > 0) ||
                    (outputDesc->mRefCount[AUDIO_STREAM_RING] > 0) ||
                    (outputDesc->mRefCount[AUDIO_STREAM_ALARM] > 0) ||
                    (outputDesc->mRefCount[AUDIO_STREAM_DTMF] > 0) ||
                    (outputDesc->mRefCount[AUDIO_STREAM_NOTIFICATION] > 0)) {

                    device = AUDIO_DEVICE_OUT_SPEAKER;
                    ALOGV("refcount system %d, refcount ring %d"
                          ",refcount Alarm %d, refcount notification %d",
                            outputDesc->mRefCount[AUDIO_STREAM_SYSTEM],
                            outputDesc->mRefCount[AUDIO_STREAM_RING],
                            outputDesc->mRefCount[AUDIO_STREAM_ALARM],
                            outputDesc->mRefCount[AUDIO_STREAM_NOTIFICATION]);
                    ALOGV("handleHDMIPassthrough:with refcount Use speaker");
                }

            } else {
                switch(stream) {
                    case AUDIO_STREAM_SYSTEM:
                    case AUDIO_STREAM_RING:
                    case AUDIO_STREAM_DTMF:
                    case AUDIO_STREAM_ALARM:
                    case AUDIO_STREAM_NOTIFICATION:
                        device = AUDIO_DEVICE_OUT_SPEAKER;
                        ALOGV("handleHDMIPassthrough:Use speaker");
                        break;
                    default:
                        ALOGV("handleHDMIPassthrough:Use same device");
                        break;
               }
           }
       } else {
           ALOGV("no change in device");
       }
    }
    return device;
}

audio_io_handle_t AudioPolicyManager::getPassthroughOutput(
                                    audio_stream_type_t stream,
                                    uint32_t samplingRate,
                                    audio_format_t format,
                                    audio_channel_mask_t channelMask,
                                    audio_output_flags_t flags,
                                    const audio_offload_info_t *offloadInfo,
                                    audio_devices_t device)
{

    audio_io_handle_t output = 0;
    bool passthrough = false;
    sp<IOProfile> profile;
    status_t status;
    // The function should return error if it cannot find valid passthrough
    // output. This is required if client sets passthrough flag directly.
    bool shouldReturnError = false;

    if (!isHDMIPassthroughEnabled()) {
        ALOGE("getPassthroughOutput: passthrough not enabled");
        goto noPassthrough;
    }

    if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH)
        shouldReturnError = true;

    // Passthrough used for dolby formats and if device is HDMI
    if ((format == AUDIO_FORMAT_E_AC3 || format == AUDIO_FORMAT_AC3 ||
         format == AUDIO_FORMAT_E_AC3_JOC) &&
         (device & AUDIO_DEVICE_OUT_AUX_DIGITAL)) {

        //stream based effects enabled, ignore passthrough
        if (isEffectEnabled()) {
            ALOGE("No PASSTHROUGH - effect enabled");
            goto noPassthrough;
        }

        for (size_t i = 0; i < mOutputs.size(); i++) {
            audio_io_handle_t output = mOutputs.keyAt(i);
            sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (desc->mDevice & AUDIO_DEVICE_OUT_AUX_DIGITAL) {
                /*if (((desc->mFlags & (AudioSystem::output_flags)
                                    AUDIO_OUTPUT_FLAG_FAST) ||
                    (desc->mFlags & (AudioSystem::output_flags)
                                    AUDIO_OUTPUT_FLAG_PRIMARY) ||
                    (desc->mFlags & (AudioSystem::output_flags)
                                    AUDIO_OUTPUT_FLAG_DEEP_BUFFER)) &&
                    (desc->mRefCount[AudioSystem::MUSIC] > 0))
                            goto no_passthrough;
                else*/
                // Check is compress offload stream is active before allowing
                // passthrough stream
                if ((desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
                       (!desc->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH)
                       && desc->mDirectOpenCount > 0) {
                    ALOGE("Ignore passthrough,offload session active");
                    goto noPassthrough;
                }
            }
        }

        checkAndSuspendOutputs();
        closeOffloadOutputs();
        flags = (audio_output_flags_t)(flags|AUDIO_OUTPUT_FLAG_DIRECT|
                 AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH);

        profile = getProfileForDirectOutput(device, samplingRate, format,
                                            channelMask, flags);
        if (profile != NULL) {
            sp<AudioOutputDescriptor> outputDesc = NULL;

            for (size_t i = 0; i < mOutputs.size(); i++) {
                sp<AudioOutputDescriptor> desc = mOutputs.valueAt(i);
                if (!desc->isDuplicated() && (profile == desc->mProfile)) {
                    outputDesc = desc;
                    // reuse direct output if currently open and configured
                    // with same parameters
                    if ((samplingRate == outputDesc->mSamplingRate) &&
                            (format == outputDesc->mFormat) &&
                            (channelMask == outputDesc->mChannelMask)) {
                            outputDesc->mDirectOpenCount++;
                        ALOGE("getPassthroughOutput() reusing direct output %d",
                               mOutputs.keyAt(i));
                        return mOutputs.keyAt(i);
                    }
               }
           }
           // close direct output if currently open and configured with
           // different parameters
           if (outputDesc != NULL) {
               closeOutput(outputDesc->mIoHandle);
           }
           outputDesc = new AudioOutputDescriptor(profile);
           outputDesc->mDevice = device;
           outputDesc->mLatency = 0;
           outputDesc->mFlags =(audio_output_flags_t) (outputDesc->mFlags | flags);
           audio_config_t config = AUDIO_CONFIG_INITIALIZER;
           config.sample_rate = samplingRate;
           config.channel_mask = channelMask;
           config.format = format; 
           if (offloadInfo != NULL) {
               config.offload_info = *offloadInfo;
           }
           //outputDesc->mRefCount[stream] = 0;
           //outputDesc->mDirectOpenCount = 1;
           status = mpClientInterface->openOutput(profile->mModule->mHandle,
                                               &output,
                                               &config,
                                               &outputDesc->mDevice,
                                               String8(""),
                                               &outputDesc->mLatency,
                                               outputDesc->mFlags);

            // only accept an output with the requested parameters
            if (status != NO_ERROR ||
                (samplingRate != 0 &&
                samplingRate != config.sample_rate) ||
                (format != AUDIO_FORMAT_DEFAULT && format != config.format) ||
                (channelMask != 0 &&
                channelMask != config.channel_mask)) {
                ALOGE("getPassthroughOutput() failed opening direct output:"
                      " output %d samplingRate %d %d, format %d %d,"
                      " channelMask %04x %04x", output, samplingRate,
                      outputDesc->mSamplingRate, format, outputDesc->mFormat,
                      channelMask, outputDesc->mChannelMask);
                if (output != 0) {
                    mpClientInterface->closeOutput(output);
                }
                ALOGE("getPassthroughOutput return 0");
                    goto noPassthrough;
            }
            outputDesc->mSamplingRate = config.sample_rate;
            outputDesc->mChannelMask = config.channel_mask;
            outputDesc->mFormat = config.format;
            outputDesc->mRefCount[stream] = 0;
            outputDesc->mStopTime[stream] = 0;
            outputDesc->mDirectOpenCount = 1;

            addOutput(output, outputDesc);
            mPreviousOutputs = mOutputs;
            ALOGE("getPassthroughOutput() returns new direct o/p %d", output);
            mpClientInterface->onAudioPortListUpdate();
            return output;
        }
    }

noPassthrough:
    ALOGE("getPassthroughOutput return 0, format %x, flags %x",
           format, passthrough, flags);
    if (shouldReturnError)
        return -EINVAL;
    return AUDIO_IO_HANDLE_NONE;
}

bool AudioPolicyManager::isEffectEnabled()
{
    for (size_t i = 0; i < mEffects.size(); i++) {
        sp<EffectDescriptor> pDesc = mEffects.valueAt(i);
        ALOGVV("isEffectEnabled:Io %d,strategy %d,session %d,enabled %d,name %s"
            ",implementor %s, uuid type 0x%x uid 0x%x, flags 0x%x\n",
            pDesc->mIo, pDesc->mStrategy, pDesc->mSession, pDesc->mEnabled,
            pDesc->mDesc.name, pDesc->mDesc.implementor, pDesc->mDesc.type,
            pDesc->mDesc.uuid,pDesc->mDesc.flags);

        if  (pDesc->mSession == AUDIO_SESSION_OUTPUT_MIX &&
             !strcmp(pDesc->mDesc.name, "DAP")) {
            ALOGV("Ignore DAP enable global session and dap effect");
            continue;
        }
        if (pDesc->mEnabled) {
            ALOGV("isEffectEnabled() effect %s enabled on session %d",
                  pDesc->mDesc.name, pDesc->mSession);
            return true;
        }
    }
    return false;
}

void AudioPolicyManager::updateAndCloseOutputs() {

    bool passthroughActive = false;
    sp<AudioOutputDescriptor> desc;

    if (!isHDMIPassthroughEnabled()) {
        ALOGV("updateAndCloseOutputs: passthrough not enabled");
        return;
    }

    ALOGV("updateAndCloseOutputs");
    for (size_t i = 0; i < mOutputs.size(); i++) {
        desc = mOutputs.valueAt(i);
        ALOGV("updateAndCloseOutputs:desc->mFlags %x, refCount %d",
               desc->mFlags, desc->mRefCount[AUDIO_STREAM_MUSIC]);
        if ((desc->mFlags &
            AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH)
            && desc->mRefCount[AUDIO_STREAM_MUSIC] > 0) {
            passthroughActive = true;
            break;
        }
    }

    ALOGV("updateAndCloseOutputs:passthrough_active %d", passthroughActive);
    if (passthroughActive) {
        // Move tracks associated to this strategy from previous
        // output to new output
        ALOGV("\n Invalidate stream\n");
        mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);
    }
}

void AudioPolicyManager::closeOffloadOutputs() {

    bool passthroughActive = false;
    sp<AudioOutputDescriptor> desc;

    if (!isHDMIPassthroughEnabled())
        return;
    ALOGV("closeOffloadOutputs");
    // Invalidate and close offload output before starting a pasthrough output.
    // This allows compressed stream to be restarted from correct position.
    // If compressed output is not closed, passthrough session is stopped on
    // de-routing offload session when offload session runs into standy after
    // 1 minute standby time.
    for (size_t i = 0; i < mOutputs.size(); i++) {
        desc = mOutputs.valueAt(i);
        ALOGV("closeOffloadOutputs:desc->mFlags %x, refCount %d",
               desc->mFlags, desc->mRefCount[AUDIO_STREAM_MUSIC]);
        if (((desc->mFlags &
            AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) &&
            (!(desc->mFlags &
            AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH))
            ) && desc->mRefCount[AUDIO_STREAM_MUSIC] == 0) {
            mpClientInterface->invalidateStream(AUDIO_STREAM_MUSIC);
            closeOutput(desc->mIoHandle);
        }
    }
}
bool AudioPolicyManager::isHDMIPassthroughEnabled() {

    char value[PROPERTY_VALUE_MAX] = {0};

    property_get("audio.offload.passthrough", value, NULL);
    if (atoi(value) || !strncmp("true", value, 4)) {
        ALOGD("HDMI Passthrough is enabled");
        return true;
    }
    ALOGV("HDMI Passthrough is not enabled");
    return false;
}
#endif
bool AudioPolicyManager::isValidAttributes(const audio_attributes_t *paa) {
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

}; // namespace android
