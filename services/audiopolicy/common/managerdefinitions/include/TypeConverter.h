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

#include <system/audio.h>
#include <convert/convert.h>
#include <utils/Log.h>
#include <string>
#include <utils/Vector.h>

namespace android {

/**
 * As far as we do not have real type (only typedef on uint32_t for some types, we
 * will need this trick to handle template specialization.
 */
class Devices;
class InputFlags;
class OutputFlags;
class Formats;
class OutputChannel;
class InputChannel;
class ChannelIndex;
class GainMode;

#define DYNAMIC_VALUE_TAG "dynamic" // special value for "channel_masks", "sampling_rates" and
                                    // "formats" in outputs descriptors indicating that supported
                                    // values should be queried after opening the output.

template <typename T>
static void collectionFromString(const std::string &str, Vector<T> &collection)
{
    char *literal = strdup(str.c_str());
    for (const char *cstr = strtok(literal, "|"); cstr != NULL; cstr = strtok(NULL, "|")) {
        T value;
        if (utilities::convertTo<std::string, T>(cstr, value)) {
            collection.add(value);
        }
    }
    free(literal);
}

template <typename T, typename SupportedType>
class TypeConverter
{
public:
    static bool toString(const T &value, std::string &str);

    static bool fromString(const std::string &str, T &result);

    static void collectionFromString(const std::string &str, Vector<T> &collection);

    static uint32_t maskFromString(const std::string &str);

protected:
    struct Table {
        const char *literal;
        T value;
    };

    static const Table mTable[];
    static const size_t mSize;
};

typedef TypeConverter<audio_devices_t, Devices> DeviceConverter;
typedef TypeConverter<audio_output_flags_t, OutputFlags> OutputFlagConverter;
typedef TypeConverter<audio_input_flags_t, InputFlags> InputFlagConverter;
typedef TypeConverter<audio_format_t, Formats> FormatConverter;
typedef TypeConverter<audio_channel_mask_t, OutputChannel> OutputChannelConverter;
typedef TypeConverter<audio_channel_mask_t, InputChannel> InputChannelConverter;
typedef TypeConverter<audio_channel_mask_t, ChannelIndex> ChannelIndexConverter;
typedef TypeConverter<audio_gain_mode_t, GainMode> GainModeConverter;

static Vector<uint32_t> samplingRatesFromString(const std::string &samplingRates)
{
    Vector<uint32_t> samplingRateCollection;
    // by convention, "0' in the first entry in mSamplingRates indicates the supported sampling
    // rates should be read from the output stream after it is opened for the first time
    if (samplingRates == DYNAMIC_VALUE_TAG) {
        samplingRateCollection.add(0);
    } else {
        collectionFromString<uint32_t>(samplingRates, samplingRateCollection);
    }
    return samplingRateCollection;
}

static Vector<audio_format_t> formatsFromString(const std::string &formats)
{
    Vector<audio_format_t> formatCollection;
    // by convention, "0' in the first entry in mFormats indicates the supported formats
    // should be read from the output stream after it is opened for the first time
    if (formats == DYNAMIC_VALUE_TAG) {
        formatCollection.add(AUDIO_FORMAT_DEFAULT);
    } else {
        FormatConverter::collectionFromString(formats, formatCollection);
    }
    return formatCollection;
}

static Vector<audio_channel_mask_t> inputChannelMasksFromString(const std::string &inChannels)
{
    Vector <audio_channel_mask_t> inputChannelMaskCollection;
    if (inChannels == DYNAMIC_VALUE_TAG) {
        inputChannelMaskCollection.add(0);
    } else {
        InputChannelConverter::collectionFromString(inChannels, inputChannelMaskCollection);
        ChannelIndexConverter::collectionFromString(inChannels, inputChannelMaskCollection);
    }
    return inputChannelMaskCollection;
}

static Vector<audio_channel_mask_t> outputChannelMasksFromString(const std::string &outChannels)
{
    Vector <audio_channel_mask_t> outputChannelMaskCollection;
    // by convention, "0' in the first entry in mChannelMasks indicates the supported channel
    // masks should be read from the output stream after it is opened for the first time
    if (outChannels == DYNAMIC_VALUE_TAG) {
        outputChannelMaskCollection.add(0);
    } else {
        OutputChannelConverter::collectionFromString(outChannels, outputChannelMaskCollection);
        ChannelIndexConverter::collectionFromString(outChannels, outputChannelMaskCollection);
    }
    return outputChannelMaskCollection;
}

}; // namespace android

