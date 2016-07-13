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

#include "policy.h"
#include <Volume.h>
#include <system/audio.h>
#include <convert/convert.h>
#include <utils/Log.h>
#include <string>
#include <utils/Vector.h>
#include <utils/SortedVector.h>

namespace android {

struct SampleRateTraits
{
    typedef uint32_t Type;
    typedef SortedVector<Type> Collection;
};
struct DeviceTraits
{
    typedef audio_devices_t Type;
    typedef Vector<Type> Collection;
};
struct OutputFlagTraits
{
    typedef audio_output_flags_t Type;
    typedef Vector<Type> Collection;
};
struct InputFlagTraits
{
    typedef audio_input_flags_t Type;
    typedef Vector<Type> Collection;
};
struct FormatTraits
{
    typedef audio_format_t Type;
    typedef Vector<Type> Collection;
};
struct ChannelTraits
{
    typedef audio_channel_mask_t Type;
    typedef SortedVector<Type> Collection;
};
struct OutputChannelTraits : public ChannelTraits {};
struct InputChannelTraits : public ChannelTraits {};
struct ChannelIndexTraits : public ChannelTraits {};
struct GainModeTraits
{
    typedef audio_gain_mode_t Type;
    typedef Vector<Type> Collection;
};
struct StreamTraits
{
  typedef audio_stream_type_t Type;
  typedef Vector<Type> Collection;
};
struct DeviceCategoryTraits
{
  typedef device_category Type;
  typedef Vector<Type> Collection;
};
template <typename T>
struct DefaultTraits
{
  typedef T Type;
  typedef Vector<Type> Collection;
};

template <class Traits>
static void collectionFromString(const std::string &str, typename Traits::Collection &collection,
                                 const char *del = "|")
{
    char *literal = strdup(str.c_str());
    for (const char *cstr = strtok(literal, del); cstr != NULL; cstr = strtok(NULL, del)) {
        typename Traits::Type value;
        if (utilities::convertTo<std::string, typename Traits::Type >(cstr, value)) {
            collection.add(value);
        }
    }
    free(literal);
}

template <class Traits>
class TypeConverter
{
public:
    static bool toString(const typename Traits::Type &value, std::string &str);

    static bool fromString(const std::string &str, typename Traits::Type &result);

    static void collectionFromString(const std::string &str,
                                     typename Traits::Collection &collection,
                                     const char *del = "|");

    static uint32_t maskFromString(const std::string &str, const char *del = "|");

protected:
    struct Table {
        const char *literal;
        typename Traits::Type value;
    };

    static const Table mTable[];
    static const size_t mSize;
};

typedef TypeConverter<DeviceTraits> DeviceConverter;
typedef TypeConverter<OutputFlagTraits> OutputFlagConverter;
typedef TypeConverter<InputFlagTraits> InputFlagConverter;
typedef TypeConverter<FormatTraits> FormatConverter;
typedef TypeConverter<OutputChannelTraits> OutputChannelConverter;
typedef TypeConverter<InputChannelTraits> InputChannelConverter;
typedef TypeConverter<ChannelIndexTraits> ChannelIndexConverter;
typedef TypeConverter<GainModeTraits> GainModeConverter;
typedef TypeConverter<StreamTraits> StreamTypeConverter;
typedef TypeConverter<DeviceCategoryTraits> DeviceCategoryConverter;

inline
static SampleRateTraits::Collection samplingRatesFromString(const std::string &samplingRates,
                                                            const char *del = "|")
{
    SampleRateTraits::Collection samplingRateCollection;
    collectionFromString<SampleRateTraits>(samplingRates, samplingRateCollection, del);
    return samplingRateCollection;
}

inline
static FormatTraits::Collection formatsFromString(const std::string &formats, const char *del = "|")
{
    FormatTraits::Collection formatCollection;
    FormatConverter::collectionFromString(formats, formatCollection, del);
    return formatCollection;
}

inline
static audio_format_t formatFromString(const std::string &literalFormat)
{
    audio_format_t format;
    if (literalFormat.empty()) {
        return gDynamicFormat;
    }
    FormatConverter::fromString(literalFormat, format);
    return format;
}

inline
static audio_channel_mask_t channelMaskFromString(const std::string &literalChannels)
{
    audio_channel_mask_t channels;
    if (!OutputChannelConverter::fromString(literalChannels, channels) ||
            !InputChannelConverter::fromString(literalChannels, channels)) {
        return AUDIO_CHANNEL_INVALID;
    }
    return channels;
}

inline
static ChannelTraits::Collection channelMasksFromString(const std::string &channels,
                                                        const char *del = "|")
{
    ChannelTraits::Collection channelMaskCollection;
    OutputChannelConverter::collectionFromString(channels, channelMaskCollection, del);
    InputChannelConverter::collectionFromString(channels, channelMaskCollection, del);
    ChannelIndexConverter::collectionFromString(channels, channelMaskCollection, del);
    return channelMaskCollection;
}

inline
static InputChannelTraits::Collection inputChannelMasksFromString(const std::string &inChannels,
                                                                  const char *del = "|")
{
    InputChannelTraits::Collection inputChannelMaskCollection;
    InputChannelConverter::collectionFromString(inChannels, inputChannelMaskCollection, del);
    ChannelIndexConverter::collectionFromString(inChannels, inputChannelMaskCollection, del);
    return inputChannelMaskCollection;
}

inline
static OutputChannelTraits::Collection outputChannelMasksFromString(const std::string &outChannels,
                                                                    const char *del = "|")
{
    OutputChannelTraits::Collection outputChannelMaskCollection;
    OutputChannelConverter::collectionFromString(outChannels, outputChannelMaskCollection, del);
    ChannelIndexConverter::collectionFromString(outChannels, outputChannelMaskCollection, del);
    return outputChannelMaskCollection;
}

}; // namespace android

