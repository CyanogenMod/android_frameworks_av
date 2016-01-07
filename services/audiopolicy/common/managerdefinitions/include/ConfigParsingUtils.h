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

#pragma once

#include "DeviceDescriptor.h"
#include "HwModule.h"
#include "audio_policy_conf.h"
#include <system/audio.h>
#include <utils/Log.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>
#include <cutils/config_utils.h>
#include <utils/RefBase.h>
#include <system/audio_policy.h>

namespace android {

// ----------------------------------------------------------------------------
// Definitions for audio_policy.conf file parsing
// ----------------------------------------------------------------------------

struct StringToEnum {
    const char *name;
    uint32_t value;
};

// TODO: move to a separate file. Should be in sync with audio.h.
#define STRING_TO_ENUM(string) { #string, (uint32_t)string } // uint32_t cast removes warning
#define NAME_TO_ENUM(name, value) { name, value }
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

const StringToEnum sDeviceTypeToEnumTable[] = {
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
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_IP),
#ifdef AUDIO_EXTN_AFE_PROXY_ENABLED
    STRING_TO_ENUM(AUDIO_DEVICE_OUT_PROXY),
#endif
    STRING_TO_ENUM(AUDIO_DEVICE_IN_AMBIENT),
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
    STRING_TO_ENUM(AUDIO_DEVICE_IN_IP),
#ifdef LEGACY_ALSA_AUDIO
    STRING_TO_ENUM(AUDIO_DEVICE_IN_COMMUNICATION),
#endif
};

const StringToEnum sDeviceNameToEnumTable[] = {
    NAME_TO_ENUM("Earpiece", AUDIO_DEVICE_OUT_EARPIECE),
    NAME_TO_ENUM("Speaker", AUDIO_DEVICE_OUT_SPEAKER),
    NAME_TO_ENUM("Speaker Protected", AUDIO_DEVICE_OUT_SPEAKER_SAFE),
    NAME_TO_ENUM("Wired Headset", AUDIO_DEVICE_OUT_WIRED_HEADSET),
    NAME_TO_ENUM("Wired Headphones", AUDIO_DEVICE_OUT_WIRED_HEADPHONE),
    NAME_TO_ENUM("BT SCO", AUDIO_DEVICE_OUT_BLUETOOTH_SCO),
    NAME_TO_ENUM("BT SCO Headset", AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET),
    NAME_TO_ENUM("BT SCO Car Kit", AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT),
    NAME_TO_ENUM("", AUDIO_DEVICE_OUT_ALL_SCO),
    NAME_TO_ENUM("BT A2DP Out", AUDIO_DEVICE_OUT_BLUETOOTH_A2DP),
    NAME_TO_ENUM("BT A2DP Headphones", AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES),
    NAME_TO_ENUM("BT A2DP Speaker", AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER),
    NAME_TO_ENUM("", AUDIO_DEVICE_OUT_ALL_A2DP),
    NAME_TO_ENUM("HDMI Out", AUDIO_DEVICE_OUT_AUX_DIGITAL),
    NAME_TO_ENUM("HDMI Out", AUDIO_DEVICE_OUT_HDMI),
    NAME_TO_ENUM("Analog Dock Out", AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET),
    NAME_TO_ENUM("Digital Dock Out", AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET),
    NAME_TO_ENUM("USB Host Out", AUDIO_DEVICE_OUT_USB_ACCESSORY),
    NAME_TO_ENUM("USB Device Out", AUDIO_DEVICE_OUT_USB_DEVICE),
    NAME_TO_ENUM("", AUDIO_DEVICE_OUT_ALL_USB),
    NAME_TO_ENUM("Reroute Submix Out", AUDIO_DEVICE_OUT_REMOTE_SUBMIX),
    NAME_TO_ENUM("Telephony Tx", AUDIO_DEVICE_OUT_TELEPHONY_TX),
    NAME_TO_ENUM("Line Out", AUDIO_DEVICE_OUT_LINE),
    NAME_TO_ENUM("HDMI ARC Out", AUDIO_DEVICE_OUT_HDMI_ARC),
    NAME_TO_ENUM("S/PDIF Out", AUDIO_DEVICE_OUT_SPDIF),
    NAME_TO_ENUM("FM transceiver Out", AUDIO_DEVICE_OUT_FM),
    NAME_TO_ENUM("Aux Line Out", AUDIO_DEVICE_OUT_AUX_LINE),
    NAME_TO_ENUM("IP Out", AUDIO_DEVICE_OUT_IP),
    NAME_TO_ENUM("Ambient Mic", AUDIO_DEVICE_IN_AMBIENT),
    NAME_TO_ENUM("Built-In Mic", AUDIO_DEVICE_IN_BUILTIN_MIC),
    NAME_TO_ENUM("BT SCO Headset Mic", AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET),
    NAME_TO_ENUM("", AUDIO_DEVICE_IN_ALL_SCO),
    NAME_TO_ENUM("Wired Headset Mic", AUDIO_DEVICE_IN_WIRED_HEADSET),
    NAME_TO_ENUM("HDMI In", AUDIO_DEVICE_IN_AUX_DIGITAL),
    NAME_TO_ENUM("HDMI In", AUDIO_DEVICE_IN_HDMI),
    NAME_TO_ENUM("Telephony Rx", AUDIO_DEVICE_IN_TELEPHONY_RX),
    NAME_TO_ENUM("Telephony Rx", AUDIO_DEVICE_IN_VOICE_CALL),
    NAME_TO_ENUM("Built-In Back Mic", AUDIO_DEVICE_IN_BACK_MIC),
    NAME_TO_ENUM("Reroute Submix In", AUDIO_DEVICE_IN_REMOTE_SUBMIX),
    NAME_TO_ENUM("Analog Dock In", AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET),
    NAME_TO_ENUM("Digital Dock In", AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET),
    NAME_TO_ENUM("USB Host In", AUDIO_DEVICE_IN_USB_ACCESSORY),
    NAME_TO_ENUM("USB Device In", AUDIO_DEVICE_IN_USB_DEVICE),
    NAME_TO_ENUM("FM Tuner In", AUDIO_DEVICE_IN_FM_TUNER),
    NAME_TO_ENUM("TV Tuner In", AUDIO_DEVICE_IN_TV_TUNER),
    NAME_TO_ENUM("Line In", AUDIO_DEVICE_IN_LINE),
    NAME_TO_ENUM("S/PDIF In", AUDIO_DEVICE_IN_SPDIF),
    NAME_TO_ENUM("BT A2DP In", AUDIO_DEVICE_IN_BLUETOOTH_A2DP),
    NAME_TO_ENUM("Loopback In", AUDIO_DEVICE_IN_LOOPBACK),
    NAME_TO_ENUM("IP In", AUDIO_DEVICE_IN_IP),
};

const StringToEnum sOutputFlagNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DIRECT),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DIRECT_PCM),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_PRIMARY),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_FAST),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_DEEP_BUFFER),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_NON_BLOCKING),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_HW_AV_SYNC),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_TTS),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_RAW),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_SYNC),
    STRING_TO_ENUM(AUDIO_OUTPUT_FLAG_VOIP_RX),
};

const StringToEnum sInputFlagNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_FAST),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_HW_HOTWORD),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_RAW),
    STRING_TO_ENUM(AUDIO_INPUT_FLAG_SYNC),
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
    STRING_TO_ENUM(AUDIO_FORMAT_DTS),
    STRING_TO_ENUM(AUDIO_FORMAT_DTS_HD),
#ifdef FLAC_OFFLOAD_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_FLAC),
#endif
#ifdef WMA_OFFLOAD_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_WMA),
    STRING_TO_ENUM(AUDIO_FORMAT_WMA_PRO),
#endif
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_16_BIT_OFFLOAD),
    STRING_TO_ENUM(AUDIO_FORMAT_PCM_24_BIT_OFFLOAD),
#ifdef ALAC_OFFLOAD_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_ALAC),
#endif
#ifdef APE_OFFLOAD_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_APE),
#endif
#ifdef AAC_ADTS_OFFLOAD_ENABLED
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_MAIN),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_LC),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_SSR),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_LTP),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_HE_V1),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_SCALABLE),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_ERLC),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_LD),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_HE_V2),
    STRING_TO_ENUM(AUDIO_FORMAT_AAC_ADTS_ELD),
#endif
};

const StringToEnum sOutChannelsNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_QUAD),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_2POINT1),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_SURROUND),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_PENTA),
    STRING_TO_ENUM(AUDIO_CHANNEL_OUT_6POINT1),
};

const StringToEnum sInChannelsNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_STEREO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_FRONT_BACK),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_5POINT1),
#ifdef LEGACY_ALSA_AUDIO
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_VOICE_CALL_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_VOICE_DNLINK_MONO),
    STRING_TO_ENUM(AUDIO_CHANNEL_IN_VOICE_UPLINK_MONO),
#endif
};

const StringToEnum sIndexChannelsNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_1),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_2),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_3),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_4),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_5),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_6),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_7),
    STRING_TO_ENUM(AUDIO_CHANNEL_INDEX_MASK_8),
};

const StringToEnum sGainModeNameToEnumTable[] = {
    STRING_TO_ENUM(AUDIO_GAIN_MODE_JOINT),
    STRING_TO_ENUM(AUDIO_GAIN_MODE_CHANNELS),
    STRING_TO_ENUM(AUDIO_GAIN_MODE_RAMP),
};

class ConfigParsingUtils
{
public:
    static uint32_t stringToEnum(const struct StringToEnum *table,
            size_t size,
            const char *name);
    static const char *enumToString(const struct StringToEnum *table,
            size_t size,
            uint32_t value);
    static bool stringToBool(const char *value);
    static uint32_t parseOutputFlagNames(char *name);
    static uint32_t parseInputFlagNames(char *name);
    static audio_devices_t parseDeviceNames(char *name);

    static void loadHwModules(cnode *root, HwModuleCollection &hwModules,
                              DeviceVector &availableInputDevices,
                              DeviceVector &availableOutputDevices,
                              sp<DeviceDescriptor> &defaultOutputDevices,
                              bool &isSpeakerDrcEnabled);

    static void loadGlobalConfig(cnode *root, const sp<HwModule>& module,
                                 DeviceVector &availableInputDevices,
                                 DeviceVector &availableOutputDevices,
                                 sp<DeviceDescriptor> &defaultOutputDevices,
                                 bool &isSpeakerDrcEnabled);

    static status_t loadAudioPolicyConfig(const char *path,
                                          HwModuleCollection &hwModules,
                                          DeviceVector &availableInputDevices,
                                          DeviceVector &availableOutputDevices,
                                          sp<DeviceDescriptor> &defaultOutputDevices,
                                          bool &isSpeakerDrcEnabled);

private:
    static void loadHwModule(cnode *root, HwModuleCollection &hwModules,
                             DeviceVector &availableInputDevices,
                             DeviceVector &availableOutputDevices,
                             sp<DeviceDescriptor> &defaultOutputDevices,
                             bool &isSpeakerDrcEnabled);
};

}; // namespace android
