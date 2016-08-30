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

#define LOG_TAG "APM::AudioProfile"
//#define LOG_NDEBUG 0

#include "AudioProfile.h"
#include "AudioPort.h"
#include "HwModule.h"
#include "AudioGain.h"
#include <utils/SortedVector.h>
#include "TypeConverter.h"
#include <media/AudioResamplerPublic.h>
#include <algorithm>

namespace android {

status_t AudioProfile::checkExact(uint32_t samplingRate, audio_channel_mask_t channelMask,
                                  audio_format_t format) const
{
    if (audio_formats_match(format, mFormat) &&
            supportsChannels(channelMask) &&
            supportsRate(samplingRate)) {
        return NO_ERROR;
    }
    return BAD_VALUE;
}

template <typename T>
bool operator == (const SortedVector<T> &left, const SortedVector<T> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for(size_t index = 0; index < right.size(); index++) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

bool operator == (const AudioProfile &left, const AudioProfile &compareTo)
{
    return (left.getFormat() == compareTo.getFormat()) &&
            (left.getChannels() == compareTo.getChannels()) &&
            (left.getSampleRates() == compareTo.getSampleRates());
}

status_t AudioProfile::checkCompatibleSamplingRate(uint32_t samplingRate,
                                                   uint32_t &updatedSamplingRate) const
{
    ALOG_ASSERT(samplingRate > 0);

    if (mSamplingRates.isEmpty()) {
        updatedSamplingRate = samplingRate;
        return NO_ERROR;
    }

    // Search for the closest supported sampling rate that is above (preferred)
    // or below (acceptable) the desired sampling rate, within a permitted ratio.
    // The sampling rates are sorted in ascending order.
    size_t orderOfDesiredRate = mSamplingRates.orderOf(samplingRate);

    // Prefer to down-sample from a higher sampling rate, as we get the desired frequency spectrum.
    if (orderOfDesiredRate < mSamplingRates.size()) {
        uint32_t candidate = mSamplingRates[orderOfDesiredRate];
        if (candidate / AUDIO_RESAMPLER_DOWN_RATIO_MAX <= samplingRate) {
            updatedSamplingRate = candidate;
            return NO_ERROR;
        }
    }
    // But if we have to up-sample from a lower sampling rate, that's OK.
    if (orderOfDesiredRate != 0) {
        uint32_t candidate = mSamplingRates[orderOfDesiredRate - 1];
        if (candidate * AUDIO_RESAMPLER_UP_RATIO_MAX >= samplingRate) {
            updatedSamplingRate = candidate;
            return NO_ERROR;
        }
    }
    // leave updatedSamplingRate unmodified
    return BAD_VALUE;
}

status_t AudioProfile::checkCompatibleChannelMask(audio_channel_mask_t channelMask,
                                                  audio_channel_mask_t &updatedChannelMask,
                                                  audio_port_type_t portType,
                                                  audio_port_role_t portRole) const
{
    if (mChannelMasks.isEmpty()) {
        updatedChannelMask = channelMask;
        return NO_ERROR;
    }
    const bool isRecordThread = portType == AUDIO_PORT_TYPE_MIX && portRole == AUDIO_PORT_ROLE_SINK;
    const bool isIndex = audio_channel_mask_get_representation(channelMask)
            == AUDIO_CHANNEL_REPRESENTATION_INDEX;
    int bestMatch = 0;
    for (size_t i = 0; i < mChannelMasks.size(); i ++) {
        audio_channel_mask_t supported = mChannelMasks[i];
        if (supported == channelMask) {
            // Exact matches always taken.
            updatedChannelMask = channelMask;
            return NO_ERROR;
        }

        // AUDIO_CHANNEL_NONE (value: 0) is used for dynamic channel support
        if (isRecordThread && supported != AUDIO_CHANNEL_NONE) {
            // Approximate (best) match:
            // The match score measures how well the supported channel mask matches the
            // desired mask, where increasing-is-better.
            //
            // TODO: Some tweaks may be needed.
            // Should be a static function of the data processing library.
            //
            // In priority:
            // match score = 1000 if legacy channel conversion equivalent (always prefer this)
            // OR
            // match score += 100 if the channel mask representations match
            // match score += number of channels matched.
            //
            // If there are no matched channels, the mask may still be accepted
            // but the playback or record will be silent.
            const bool isSupportedIndex = (audio_channel_mask_get_representation(supported)
                    == AUDIO_CHANNEL_REPRESENTATION_INDEX);
            int match;
            if (isIndex && isSupportedIndex) {
                // index equivalence
                match = 100 + __builtin_popcount(
                        audio_channel_mask_get_bits(channelMask)
                            & audio_channel_mask_get_bits(supported));
            } else if (isIndex && !isSupportedIndex) {
                const uint32_t equivalentBits =
                        (1 << audio_channel_count_from_in_mask(supported)) - 1 ;
                match = __builtin_popcount(
                        audio_channel_mask_get_bits(channelMask) & equivalentBits);
            } else if (!isIndex && isSupportedIndex) {
                const uint32_t equivalentBits =
                        (1 << audio_channel_count_from_in_mask(channelMask)) - 1;
                match = __builtin_popcount(
                        equivalentBits & audio_channel_mask_get_bits(supported));
            } else {
                // positional equivalence
                match = 100 + __builtin_popcount(
                        audio_channel_mask_get_bits(channelMask)
                            & audio_channel_mask_get_bits(supported));
                switch (supported) {
                case AUDIO_CHANNEL_IN_FRONT_BACK:
                case AUDIO_CHANNEL_IN_STEREO:
                    if (channelMask == AUDIO_CHANNEL_IN_MONO) {
                        match = 1000;
                    }
                    break;
                case AUDIO_CHANNEL_IN_MONO:
                    if (channelMask == AUDIO_CHANNEL_IN_FRONT_BACK
                            || channelMask == AUDIO_CHANNEL_IN_STEREO) {
                        match = 1000;
                    }
                    break;
                default:
                    break;
                }
            }
            if (match > bestMatch) {
                bestMatch = match;
                updatedChannelMask = supported;
            }
        }
    }
    return bestMatch > 0 ? NO_ERROR : BAD_VALUE;
}

void AudioProfile::dump(int fd, int spaces) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%s%s%s\n", mIsDynamicFormat ? "[dynamic format]" : "",
             mIsDynamicChannels ? "[dynamic channels]" : "",
             mIsDynamicRate ? "[dynamic rates]" : "");
    result.append(buffer);
    if (mName.length() != 0) {
        snprintf(buffer, SIZE, "%*s- name: %s\n", spaces, "", mName.string());
        result.append(buffer);
    }
    std::string formatLiteral;
    if (FormatConverter::toString(mFormat, formatLiteral)) {
        snprintf(buffer, SIZE, "%*s- format: %s\n", spaces, "", formatLiteral.c_str());
        result.append(buffer);
    }
    if (!mSamplingRates.isEmpty()) {
        snprintf(buffer, SIZE, "%*s- sampling rates:", spaces, "");
        result.append(buffer);
        for (size_t i = 0; i < mSamplingRates.size(); i++) {
            snprintf(buffer, SIZE, "%d", mSamplingRates[i]);
            result.append(buffer);
            result.append(i == (mSamplingRates.size() - 1) ? "" : ", ");
        }
        result.append("\n");
    }

    if (!mChannelMasks.isEmpty()) {
        snprintf(buffer, SIZE, "%*s- channel masks:", spaces, "");
        result.append(buffer);
        for (size_t i = 0; i < mChannelMasks.size(); i++) {
            snprintf(buffer, SIZE, "0x%04x", mChannelMasks[i]);
            result.append(buffer);
            result.append(i == (mChannelMasks.size() - 1) ? "" : ", ");
        }
        result.append("\n");
    }
    write(fd, result.string(), result.size());
}

status_t AudioProfileVector::checkExactProfile(uint32_t samplingRate,
                                               audio_channel_mask_t channelMask,
                                               audio_format_t format) const
{
    if (isEmpty()) {
        return NO_ERROR;
    }

    for (size_t i = 0; i < size(); i++) {
        const sp<AudioProfile> profile = itemAt(i);
        if (profile->checkExact(samplingRate, channelMask, format) == NO_ERROR) {
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

status_t AudioProfileVector::checkCompatibleProfile(uint32_t &samplingRate,
                                                    audio_channel_mask_t &channelMask,
                                                    audio_format_t &format,
                                                    audio_port_type_t portType,
                                                    audio_port_role_t portRole,
                                                    bool checkExactFormat) const
{
    if (isEmpty()) {
        return NO_ERROR;
    }

    const bool checkInexact = // when port is input and format is linear pcm
            portType == AUDIO_PORT_TYPE_MIX && portRole == AUDIO_PORT_ROLE_SINK
            && audio_is_linear_pcm(format);

    // iterate from best format to worst format (reverse order)
    for (ssize_t i = size() - 1; i >= 0 ; --i) {
        const sp<AudioProfile> profile = itemAt(i);
        audio_format_t formatToCompare = profile->getFormat();
        if (formatToCompare == format ||
                (checkInexact
                        && formatToCompare != AUDIO_FORMAT_DEFAULT
                        && audio_is_linear_pcm(formatToCompare))) {
            // Compatible profile has been found, checks if this profile has compatible
            // rate and channels as well
            audio_channel_mask_t updatedChannels;
            uint32_t updatedRate;

            if ((checkExactFormat) && (formatToCompare != format))
                continue;
            if (profile->checkCompatibleChannelMask(channelMask, updatedChannels,
                                                    portType, portRole) == NO_ERROR &&
                    profile->checkCompatibleSamplingRate(samplingRate, updatedRate) == NO_ERROR) {
                // for inexact checks we take the first linear pcm format due to sorting.
                format = formatToCompare;
                channelMask = updatedChannels;
                samplingRate = updatedRate;
                return NO_ERROR;
            }
        }
    }
    return BAD_VALUE;
}

int AudioProfileVector::compareFormats(const sp<AudioProfile> *profile1,
                                       const sp<AudioProfile> *profile2)
{
    return AudioPort::compareFormats((*profile1)->getFormat(), (*profile2)->getFormat());
}

}; // namespace android
