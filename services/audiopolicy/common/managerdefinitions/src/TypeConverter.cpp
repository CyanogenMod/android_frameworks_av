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

#include "TypeConverter.h"

namespace android {

#define MAKE_STRING_FROM_ENUM(string) { #string, string }

template <>
const DeviceConverter::Table DeviceConverter::mTable[] = {
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_EARPIECE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_SPEAKER),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_SPEAKER_SAFE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_WIRED_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_WIRED_HEADPHONE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_SCO),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_ALL_SCO),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_ALL_A2DP),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_AUX_DIGITAL),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_HDMI),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_USB_ACCESSORY),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_USB_DEVICE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_ALL_USB),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_REMOTE_SUBMIX),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_TELEPHONY_TX),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_LINE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_HDMI_ARC),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_SPDIF),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_FM),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_AUX_LINE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_IP),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_BUS),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_OUT_STUB),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_AMBIENT),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_BUILTIN_MIC),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_ALL_SCO),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_WIRED_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_AUX_DIGITAL),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_HDMI),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_TELEPHONY_RX),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_VOICE_CALL),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_BACK_MIC),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_REMOTE_SUBMIX),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_USB_ACCESSORY),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_USB_DEVICE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_FM_TUNER),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_TV_TUNER),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_LINE),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_SPDIF),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_BLUETOOTH_A2DP),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_LOOPBACK),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_IP),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_BUS),
        MAKE_STRING_FROM_ENUM(AUDIO_DEVICE_IN_STUB),
};

template<>
const size_t DeviceConverter::mSize = sizeof(DeviceConverter::mTable) /
        sizeof(DeviceConverter::mTable[0]);


template <>
const OutputFlagConverter::Table OutputFlagConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_DIRECT),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_PRIMARY),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_FAST),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_DEEP_BUFFER),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_NON_BLOCKING),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_HW_AV_SYNC),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_TTS),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_RAW),
    MAKE_STRING_FROM_ENUM(AUDIO_OUTPUT_FLAG_SYNC),
};
template<>
const size_t OutputFlagConverter::mSize = sizeof(OutputFlagConverter::mTable) /
        sizeof(OutputFlagConverter::mTable[0]);


template <>
const InputFlagConverter::Table InputFlagConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_INPUT_FLAG_FAST),
    MAKE_STRING_FROM_ENUM(AUDIO_INPUT_FLAG_HW_HOTWORD),
    MAKE_STRING_FROM_ENUM(AUDIO_INPUT_FLAG_RAW),
    MAKE_STRING_FROM_ENUM(AUDIO_INPUT_FLAG_SYNC),
};
template<>
const size_t InputFlagConverter::mSize = sizeof(InputFlagConverter::mTable) /
        sizeof(InputFlagConverter::mTable[0]);


template <>
const FormatConverter::Table FormatConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_PCM_16_BIT),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_PCM_8_BIT),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_PCM_32_BIT),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_PCM_8_24_BIT),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_PCM_FLOAT),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_PCM_24_BIT_PACKED),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_MP3),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_MAIN),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_LC),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_SSR),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_LTP),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_HE_V1),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_SCALABLE),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_ERLC),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_LD),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_HE_V2),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AAC_ELD),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_VORBIS),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_HE_AAC_V1),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_HE_AAC_V2),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_OPUS),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_AC3),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_E_AC3),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_DTS),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_DTS_HD),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_IEC61937),
    MAKE_STRING_FROM_ENUM(AUDIO_FORMAT_DOLBY_TRUEHD),
};
template<>
const size_t FormatConverter::mSize = sizeof(FormatConverter::mTable) /
        sizeof(FormatConverter::mTable[0]);


template <>
const OutputChannelConverter::Table OutputChannelConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_OUT_MONO),
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_OUT_STEREO),
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_OUT_QUAD),
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_OUT_5POINT1),
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_OUT_7POINT1),
};
template<>
const size_t OutputChannelConverter::mSize = sizeof(OutputChannelConverter::mTable) /
        sizeof(OutputChannelConverter::mTable[0]);


template <>
const InputChannelConverter::Table InputChannelConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_IN_MONO),
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_IN_STEREO),
    MAKE_STRING_FROM_ENUM(AUDIO_CHANNEL_IN_FRONT_BACK),
};
template<>
const size_t InputChannelConverter::mSize = sizeof(InputChannelConverter::mTable) /
        sizeof(InputChannelConverter::mTable[0]);

template <>
const ChannelIndexConverter::Table ChannelIndexConverter::mTable[] = {
    {"AUDIO_CHANNEL_INDEX_MASK_1", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_1)},
    {"AUDIO_CHANNEL_INDEX_MASK_2", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_2)},
    {"AUDIO_CHANNEL_INDEX_MASK_3", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_3)},
    {"AUDIO_CHANNEL_INDEX_MASK_4", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_4)},
    {"AUDIO_CHANNEL_INDEX_MASK_5", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_5)},
    {"AUDIO_CHANNEL_INDEX_MASK_6", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_6)},
    {"AUDIO_CHANNEL_INDEX_MASK_7", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_7)},
    {"AUDIO_CHANNEL_INDEX_MASK_8", static_cast<audio_channel_mask_t>(AUDIO_CHANNEL_INDEX_MASK_8)},
};
template<>
const size_t ChannelIndexConverter::mSize = sizeof(ChannelIndexConverter::mTable) /
        sizeof(ChannelIndexConverter::mTable[0]);


template <>
const GainModeConverter::Table GainModeConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_GAIN_MODE_JOINT),
    MAKE_STRING_FROM_ENUM(AUDIO_GAIN_MODE_CHANNELS),
    MAKE_STRING_FROM_ENUM(AUDIO_GAIN_MODE_RAMP),
};

template<>
const size_t GainModeConverter::mSize = sizeof(GainModeConverter::mTable) /
        sizeof(GainModeConverter::mTable[0]);

template <>
const DeviceCategoryConverter::Table DeviceCategoryConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(DEVICE_CATEGORY_HEADSET),
    MAKE_STRING_FROM_ENUM(DEVICE_CATEGORY_SPEAKER),
    MAKE_STRING_FROM_ENUM(DEVICE_CATEGORY_EARPIECE),
    MAKE_STRING_FROM_ENUM(DEVICE_CATEGORY_EXT_MEDIA)
};

template<>
const size_t DeviceCategoryConverter::mSize = sizeof(DeviceCategoryConverter::mTable) /
        sizeof(DeviceCategoryConverter::mTable[0]);

template <>
const StreamTypeConverter::Table StreamTypeConverter::mTable[] = {
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_VOICE_CALL),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_SYSTEM),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_RING),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_MUSIC),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_ALARM),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_NOTIFICATION),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_BLUETOOTH_SCO ),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_ENFORCED_AUDIBLE),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_DTMF),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_TTS),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_ACCESSIBILITY),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_REROUTING),
    MAKE_STRING_FROM_ENUM(AUDIO_STREAM_PATCH),
};

template<>
const size_t StreamTypeConverter::mSize = sizeof(StreamTypeConverter::mTable) /
        sizeof(StreamTypeConverter::mTable[0]);

template <class Traits>
bool TypeConverter<Traits>::toString(const typename Traits::Type &value, std::string &str)
{
    for (size_t i = 0; i < mSize; i++) {
        if (mTable[i].value == value) {
            str = mTable[i].literal;
            return true;
        }
    }
    return false;
}

template <class Traits>
bool TypeConverter<Traits>::fromString(const std::string &str, typename Traits::Type &result)
{
    for (size_t i = 0; i < mSize; i++) {
        if (strcmp(mTable[i].literal, str.c_str()) == 0) {
            ALOGV("stringToEnum() found %s", mTable[i].literal);
            result = mTable[i].value;
            return true;
        }
    }
    return false;
}

template <class Traits>
void TypeConverter<Traits>::collectionFromString(const std::string &str,
                                                 typename Traits::Collection &collection,
                                                 const char *del)
{
    char *literal = strdup(str.c_str());

    for (const char *cstr = strtok(literal, del); cstr != NULL; cstr = strtok(NULL, del)) {
        typename Traits::Type value;
        if (fromString(cstr, value)) {
            collection.add(value);
        }
    }
    free(literal);
}

template <class Traits>
uint32_t TypeConverter<Traits>::maskFromString(const std::string &str, const char *del)
{
    char *literal = strdup(str.c_str());
    uint32_t value = 0;
    for (const char *cstr = strtok(literal, del); cstr != NULL; cstr = strtok(NULL, del)) {
        typename Traits::Type type;
        if (fromString(cstr, type)) {
            value |= static_cast<uint32_t>(type);
        }
    }
    free(literal);
    return value;
}

template class TypeConverter<DeviceTraits>;
template class TypeConverter<OutputFlagTraits>;
template class TypeConverter<InputFlagTraits>;
template class TypeConverter<FormatTraits>;
template class TypeConverter<OutputChannelTraits>;
template class TypeConverter<InputChannelTraits>;
template class TypeConverter<ChannelIndexTraits>;
template class TypeConverter<GainModeTraits>;
template class TypeConverter<StreamTraits>;
template class TypeConverter<DeviceCategoryTraits>;

}; // namespace android

