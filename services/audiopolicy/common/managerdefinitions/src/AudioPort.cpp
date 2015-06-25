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

#define LOG_TAG "APM::AudioPort"
//#define LOG_NDEBUG 0
#include <media/AudioResamplerPublic.h>
#include "AudioPort.h"
#include "HwModule.h"
#include "AudioGain.h"
#include "ConfigParsingUtils.h"
#include "audio_policy_conf.h"
#include <policy.h>

namespace android {

int32_t volatile AudioPort::mNextUniqueId = 1;

// --- AudioPort class implementation

AudioPort::AudioPort(const String8& name, audio_port_type_t type,
                     audio_port_role_t role) :
    mName(name), mType(type), mRole(role), mFlags(0)
{
    mUseInChannelMask = ((type == AUDIO_PORT_TYPE_DEVICE) && (role == AUDIO_PORT_ROLE_SOURCE)) ||
                    ((type == AUDIO_PORT_TYPE_MIX) && (role == AUDIO_PORT_ROLE_SINK));
}

void AudioPort::attach(const sp<HwModule>& module)
{
    mModule = module;
}

audio_port_handle_t AudioPort::getNextUniqueId()
{
    return static_cast<audio_port_handle_t>(android_atomic_inc(&mNextUniqueId));
}

audio_module_handle_t AudioPort::getModuleHandle() const
{
    if (mModule == 0) {
        return 0;
    }
    return mModule->mHandle;
}

uint32_t AudioPort::getModuleVersion() const
{
    if (mModule == 0) {
        return 0;
    }
    return mModule->mHalVersion;
}

const char *AudioPort::getModuleName() const
{
    if (mModule == 0) {
        return "";
    }
    return mModule->mName;
}

void AudioPort::toAudioPort(struct audio_port *port) const
{
    port->role = mRole;
    port->type = mType;
    strlcpy(port->name, mName, AUDIO_PORT_MAX_NAME_LEN);
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

void AudioPort::importAudioPort(const sp<AudioPort> port) {
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
            if (!hasFormat) { // never import a format twice
                mFormats.add(format);
            }
        }
    }
}

void AudioPort::clearCapabilities() {
    mChannelMasks.clear();
    mFormats.clear();
    mSamplingRates.clear();
}

void AudioPort::loadSamplingRates(char *name)
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

void AudioPort::loadFormats(char *name)
{
    char *str = strtok(name, "|");

    // by convention, "0' in the first entry in mFormats indicates the supported formats
    // should be read from the output stream after it is opened for the first time
    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0) {
        mFormats.add(AUDIO_FORMAT_DEFAULT);
        return;
    }

    while (str != NULL) {
        audio_format_t format = (audio_format_t)ConfigParsingUtils::stringToEnum(sFormatNameToEnumTable,
                                                             ARRAY_SIZE(sFormatNameToEnumTable),
                                                             str);
        if (format != AUDIO_FORMAT_DEFAULT) {
            mFormats.add(format);
        }
        str = strtok(NULL, "|");
    }
    // we sort from worst to best, so that AUDIO_FORMAT_DEFAULT is always the first entry.
    // TODO: compareFormats could be a lambda to convert between pointer-to-format to format:
    // [](const audio_format_t *format1, const audio_format_t *format2) {
    //     return compareFormats(*format1, *format2);
    // }
    mFormats.sort(compareFormats);
}

void AudioPort::loadInChannels(char *name)
{
    const char *str = strtok(name, "|");

    ALOGV("loadInChannels() %s", name);

    if (str != NULL && strcmp(str, DYNAMIC_VALUE_TAG) == 0) {
        mChannelMasks.add(0);
        return;
    }

    while (str != NULL) {
        audio_channel_mask_t channelMask =
                (audio_channel_mask_t)ConfigParsingUtils::stringToEnum(sInChannelsNameToEnumTable,
                                                   ARRAY_SIZE(sInChannelsNameToEnumTable),
                                                   str);
        if (channelMask == 0) { // if not found, check the channel index table
            channelMask = (audio_channel_mask_t)
                      ConfigParsingUtils::stringToEnum(sIndexChannelsNameToEnumTable,
                              ARRAY_SIZE(sIndexChannelsNameToEnumTable),
                              str);
        }
        if (channelMask != 0) {
            ALOGV("loadInChannels() adding channelMask %#x", channelMask);
            mChannelMasks.add(channelMask);
        }
        str = strtok(NULL, "|");
    }
}

void AudioPort::loadOutChannels(char *name)
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
                (audio_channel_mask_t)ConfigParsingUtils::stringToEnum(sOutChannelsNameToEnumTable,
                                                   ARRAY_SIZE(sOutChannelsNameToEnumTable),
                                                   str);
        if (channelMask == 0) { // if not found, check the channel index table
            channelMask = (audio_channel_mask_t)
                      ConfigParsingUtils::stringToEnum(sIndexChannelsNameToEnumTable,
                              ARRAY_SIZE(sIndexChannelsNameToEnumTable),
                              str);
        }
        if (channelMask != 0) {
            mChannelMasks.add(channelMask);
        }
        str = strtok(NULL, "|");
    }
    return;
}

audio_gain_mode_t AudioPort::loadGainMode(char *name)
{
    const char *str = strtok(name, "|");

    ALOGV("loadGainMode() %s", name);
    audio_gain_mode_t mode = 0;
    while (str != NULL) {
        mode |= (audio_gain_mode_t)ConfigParsingUtils::stringToEnum(sGainModeNameToEnumTable,
                                                ARRAY_SIZE(sGainModeNameToEnumTable),
                                                str);
        str = strtok(NULL, "|");
    }
    return mode;
}

void AudioPort::loadGain(cnode *root, int index)
{
    cnode *node = root->first_child;

    sp<AudioGain> gain = new AudioGain(index, mUseInChannelMask);

    while (node) {
        if (strcmp(node->name, GAIN_MODE) == 0) {
            gain->mGain.mode = loadGainMode((char *)node->value);
        } else if (strcmp(node->name, GAIN_CHANNELS) == 0) {
            if (mUseInChannelMask) {
                gain->mGain.channel_mask =
                        (audio_channel_mask_t)ConfigParsingUtils::stringToEnum(sInChannelsNameToEnumTable,
                                                           ARRAY_SIZE(sInChannelsNameToEnumTable),
                                                           (char *)node->value);
            } else {
                gain->mGain.channel_mask =
                        (audio_channel_mask_t)ConfigParsingUtils::stringToEnum(sOutChannelsNameToEnumTable,
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

void AudioPort::loadGains(cnode *root)
{
    cnode *node = root->first_child;
    int index = 0;
    while (node) {
        ALOGV("loadGains() loading gain %s", node->name);
        loadGain(node, index++);
        node = node->next;
    }
}

status_t AudioPort::checkExactSamplingRate(uint32_t samplingRate) const
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

status_t AudioPort::checkCompatibleSamplingRate(uint32_t samplingRate,
        uint32_t *updatedSamplingRate) const
{
    if (mSamplingRates.isEmpty()) {
        if (updatedSamplingRate != NULL) {
            *updatedSamplingRate = samplingRate;
        }
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

    // Prefer to down-sample from a higher sampling rate, as we get the desired frequency spectrum.
    if (minAbove >= 0) {
        candidate = mSamplingRates[minAbove];
        if (candidate / AUDIO_RESAMPLER_DOWN_RATIO_MAX <= samplingRate) {
            if (updatedSamplingRate != NULL) {
                *updatedSamplingRate = candidate;
            }
            return NO_ERROR;
        }
    }
    // But if we have to up-sample from a lower sampling rate, that's OK.
    if (maxBelow >= 0) {
        candidate = mSamplingRates[maxBelow];
        if (candidate * AUDIO_RESAMPLER_UP_RATIO_MAX >= samplingRate) {
            if (updatedSamplingRate != NULL) {
                *updatedSamplingRate = candidate;
            }
            return NO_ERROR;
        }
    }
    // leave updatedSamplingRate unmodified
    return BAD_VALUE;
}

status_t AudioPort::checkExactChannelMask(audio_channel_mask_t channelMask) const
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

status_t AudioPort::checkCompatibleChannelMask(audio_channel_mask_t channelMask,
        audio_channel_mask_t *updatedChannelMask) const
{
    if (mChannelMasks.isEmpty()) {
        if (updatedChannelMask != NULL) {
            *updatedChannelMask = channelMask;
        }
        return NO_ERROR;
    }

    const bool isRecordThread = mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SINK;
    const bool isIndex = audio_channel_mask_get_representation(channelMask)
            == AUDIO_CHANNEL_REPRESENTATION_INDEX;
    int bestMatch = 0;
    for (size_t i = 0; i < mChannelMasks.size(); i ++) {
        audio_channel_mask_t supported = mChannelMasks[i];
        if (supported == channelMask) {
            // Exact matches always taken.
            if (updatedChannelMask != NULL) {
                *updatedChannelMask = channelMask;
            }
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
                if (updatedChannelMask != NULL) {
                    *updatedChannelMask = supported;
                } else {
                    return NO_ERROR; // any match will do in this case.
                }
            }
        }
    }
    return bestMatch > 0 ? NO_ERROR : BAD_VALUE;
}

status_t AudioPort::checkExactFormat(audio_format_t format) const
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

status_t AudioPort::checkCompatibleFormat(audio_format_t format, audio_format_t *updatedFormat)
        const
{
    if (mFormats.isEmpty()) {
        if (updatedFormat != NULL) {
            *updatedFormat = format;
        }
        return NO_ERROR;
    }

    const bool checkInexact = // when port is input and format is linear pcm
            mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SINK
            && audio_is_linear_pcm(format);

    // iterate from best format to worst format (reverse order)
    for (ssize_t i = mFormats.size() - 1; i >= 0 ; --i) {
        if (mFormats[i] == format ||
                (checkInexact
                        && mFormats[i] != AUDIO_FORMAT_DEFAULT
                        && audio_is_linear_pcm(mFormats[i]))) {
            // for inexact checks we take the first linear pcm format due to sorting.
            if (updatedFormat != NULL) {
                *updatedFormat = mFormats[i];
            }
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

uint32_t AudioPort::pickSamplingRate() const
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
    // For inputs, also see checkCompatibleSamplingRate().
    if (mType != AUDIO_PORT_TYPE_MIX) {
        maxRate = UINT_MAX;
    }
    // TODO: should mSamplingRates[] be ordered in terms of our preference
    // and we return the first (and hence most preferred) match?  This is of concern if
    // we want to choose 96kHz over 192kHz for USB driver stability or resource constraints.
    for (size_t i = 0; i < mSamplingRates.size(); i ++) {
        if ((mSamplingRates[i] > samplingRate) && (mSamplingRates[i] <= maxRate)) {
            samplingRate = mSamplingRates[i];
        }
    }
    return samplingRate;
}

audio_channel_mask_t AudioPort::pickChannelMask() const
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
const audio_format_t AudioPort::sPcmFormatCompareTable[] = {
        AUDIO_FORMAT_DEFAULT,
        AUDIO_FORMAT_PCM_16_BIT,
        AUDIO_FORMAT_PCM_8_24_BIT,
        AUDIO_FORMAT_PCM_24_BIT_PACKED,
        AUDIO_FORMAT_PCM_32_BIT,
        AUDIO_FORMAT_PCM_FLOAT,
};

int AudioPort::compareFormats(audio_format_t format1,
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

audio_format_t AudioPort::pickFormat() const
{
    // special case for uninitialized dynamic profile
    if (mFormats.size() == 1 && mFormats[0] == 0) {
        return AUDIO_FORMAT_DEFAULT;
    }

    audio_format_t format = AUDIO_FORMAT_DEFAULT;
    audio_format_t bestFormat =
            AudioPort::sPcmFormatCompareTable[
                ARRAY_SIZE(AudioPort::sPcmFormatCompareTable) - 1];
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

status_t AudioPort::checkGain(const struct audio_gain_config *gainConfig,
                                                  int index) const
{
    if (index < 0 || (size_t)index >= mGains.size()) {
        return BAD_VALUE;
    }
    return mGains[index]->checkConfig(gainConfig);
}

void AudioPort::dump(int fd, int spaces) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    if (mName.length() != 0) {
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
            const char *formatStr = ConfigParsingUtils::enumToString(sFormatNameToEnumTable,
                                                 ARRAY_SIZE(sFormatNameToEnumTable),
                                                 mFormats[i]);
            const bool isEmptyStr = formatStr[0] == 0;
            if (i == 0 && isEmptyStr) {
                snprintf(buffer, SIZE, "Dynamic");
            } else {
                if (isEmptyStr) {
                    snprintf(buffer, SIZE, "%#x", mFormats[i]);
                } else {
                    snprintf(buffer, SIZE, "%s", formatStr);
                }
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
        for (size_t i = 0; i < mGains.size(); i++) {
            mGains[i]->dump(fd, spaces + 2, i);
        }
    }
}

void AudioPort::log(const char* indent) const
{
    ALOGI("%s Port[nm:%s, type:%d, role:%d]", indent, mName.string(), mType, mRole);
}

// --- AudioPortConfig class implementation

AudioPortConfig::AudioPortConfig()
{
    mSamplingRate = 0;
    mChannelMask = AUDIO_CHANNEL_NONE;
    mFormat = AUDIO_FORMAT_INVALID;
    mGain.index = -1;
}

status_t AudioPortConfig::applyAudioPortConfig(
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
        status = audioport->checkExactFormat(config->format);
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

void AudioPortConfig::toAudioPortConfig(struct audio_port_config *dstConfig,
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

}; // namespace android
