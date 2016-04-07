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
#include <utils/String8.h>
#include <utils/SortedVector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <system/audio.h>
#include <cutils/config_utils.h>

namespace android {

typedef SortedVector<uint32_t> SampleRateVector;
typedef SortedVector<audio_channel_mask_t> ChannelsVector;
typedef Vector<audio_format_t> FormatVector;

template <typename T>
bool operator == (const SortedVector<T> &left, const SortedVector<T> &right);

class AudioProfile : public virtual RefBase
{
public:
    AudioProfile(audio_format_t format,
                 audio_channel_mask_t channelMasks,
                 uint32_t samplingRate) :
        mName(String8("")),
        mFormat(format)
    {
        mChannelMasks.add(channelMasks);
        mSamplingRates.add(samplingRate);
    }

    AudioProfile(audio_format_t format,
                 const ChannelsVector &channelMasks,
                 const SampleRateVector &samplingRateCollection) :
        mName(String8("")),
        mFormat(format),
        mChannelMasks(channelMasks),
        mSamplingRates(samplingRateCollection)
    {}

    audio_format_t getFormat() const { return mFormat; }

    void setChannels(const ChannelsVector &channelMasks)
    {
        if (mIsDynamicChannels) {
            mChannelMasks = channelMasks;
        }
    }
    const ChannelsVector &getChannels() const { return mChannelMasks; }

    void setSampleRates(const SampleRateVector &sampleRates)
    {
        if (mIsDynamicRate) {
            mSamplingRates = sampleRates;
        }
    }
    const SampleRateVector &getSampleRates() const { return mSamplingRates; }

    bool isValid() const { return hasValidFormat() && hasValidRates() && hasValidChannels(); }

    void clear()
    {
        if (mIsDynamicChannels) {
            mChannelMasks.clear();
        }
        if (mIsDynamicRate) {
            mSamplingRates.clear();
        }
    }

    inline bool supportsChannels(audio_channel_mask_t channels) const
    {
        return mChannelMasks.indexOf(channels) >= 0;
    }
    inline bool supportsRate(uint32_t rate) const
    {
        return mSamplingRates.indexOf(rate) >= 0;
    }

    status_t checkExact(uint32_t rate, audio_channel_mask_t channels, audio_format_t format) const;

    status_t checkCompatibleChannelMask(audio_channel_mask_t channelMask,
                                        audio_channel_mask_t &updatedChannelMask,
                                        audio_port_type_t portType,
                                        audio_port_role_t portRole) const;

    status_t checkCompatibleSamplingRate(uint32_t samplingRate,
                                         uint32_t &updatedSamplingRate) const;

    bool hasValidFormat() const { return mFormat != AUDIO_FORMAT_DEFAULT; }
    bool hasValidRates() const { return !mSamplingRates.isEmpty(); }
    bool hasValidChannels() const { return !mChannelMasks.isEmpty(); }

    void setDynamicChannels(bool dynamic) { mIsDynamicChannels = dynamic; }
    bool isDynamicChannels() const { return mIsDynamicChannels; }

    void setDynamicRate(bool dynamic) { mIsDynamicRate = dynamic; }
    bool isDynamicRate() const { return mIsDynamicRate; }

    void setDynamicFormat(bool dynamic) { mIsDynamicFormat = dynamic; }
    bool isDynamicFormat() const { return mIsDynamicFormat; }

    bool isDynamic() { return mIsDynamicFormat || mIsDynamicChannels || mIsDynamicRate; }

    void dump(int fd, int spaces) const;

private:
    String8  mName;
    audio_format_t mFormat;
    ChannelsVector mChannelMasks;
    SampleRateVector mSamplingRates;

    bool mIsDynamicFormat = false;
    bool mIsDynamicChannels = false;
    bool mIsDynamicRate = false;
};


class AudioProfileVector : public Vector<sp<AudioProfile> >
{
public:
    ssize_t add(const sp<AudioProfile> &profile)
    {
        ssize_t index = Vector::add(profile);
        // we sort from worst to best, so that AUDIO_FORMAT_DEFAULT is always the first entry.
        // TODO: compareFormats could be a lambda to convert between pointer-to-format to format:
        // [](const audio_format_t *format1, const audio_format_t *format2) {
        //     return compareFormats(*format1, *format2);
        // }
        sort(compareFormats);
        return index;
    }

    // This API is intended to be used by the policy manager once retrieving capabilities
    // for a profile with dynamic format, rate and channels attributes
    ssize_t addProfileFromHal(const sp<AudioProfile> &profileToAdd)
    {
        // Check valid profile to add:
        if (!profileToAdd->hasValidFormat()) {
            return -1;
        }
        if (!profileToAdd->hasValidChannels() && !profileToAdd->hasValidRates()) {
            FormatVector formats;
            formats.add(profileToAdd->getFormat());
            setFormats(FormatVector(formats));
            return 0;
        }
        if (!profileToAdd->hasValidChannels() && profileToAdd->hasValidRates()) {
            setSampleRatesFor(profileToAdd->getSampleRates(), profileToAdd->getFormat());
            return 0;
        }
        if (profileToAdd->hasValidChannels() && !profileToAdd->hasValidRates()) {
            setChannelsFor(profileToAdd->getChannels(), profileToAdd->getFormat());
            return 0;
        }
        // Go through the list of profile to avoid duplicates
        for (size_t profileIndex = 0; profileIndex < size(); profileIndex++) {
            const sp<AudioProfile> &profile = itemAt(profileIndex);
            if (profile->isValid() && profile == profileToAdd) {
                // Nothing to do
                return profileIndex;
            }
        }
        profileToAdd->setDynamicFormat(true); // set the format as dynamic to allow removal
        return add(profileToAdd);
    }

    sp<AudioProfile> getFirstValidProfile() const
    {
        for (size_t i = 0; i < size(); i++) {
            if (itemAt(i)->isValid()) {
                return itemAt(i);
            }
        }
        return 0;
    }

    bool hasValidProfile() const { return getFirstValidProfile() != 0; }

    status_t checkExactProfile(uint32_t samplingRate, audio_channel_mask_t channelMask,
                               audio_format_t format) const;

    status_t checkCompatibleProfile(uint32_t &samplingRate, audio_channel_mask_t &channelMask,
                                    audio_format_t &format,
                                    audio_port_type_t portType,
                                    audio_port_role_t portRole,
                                    bool checkExactFormat = false) const;

    FormatVector getSupportedFormats() const
    {
        FormatVector supportedFormats;
        for (size_t i = 0; i < size(); i++) {
            if (itemAt(i)->hasValidFormat()) {
                supportedFormats.add(itemAt(i)->getFormat());
            }
        }
        return supportedFormats;
    }

    bool hasDynamicProfile() const
    {
        for (size_t i = 0; i < size(); i++) {
            if (itemAt(i)->isDynamic()) {
                return true;
            }
        }
        return false;
    }

    bool hasDynamicFormat() const
    {
        return getProfileFor(gDynamicFormat) != 0;
    }

    bool hasDynamicChannelsFor(audio_format_t format) const
    {
       for (size_t i = 0; i < size(); i++) {
           sp<AudioProfile> profile = itemAt(i);
           if (profile->getFormat() == format && profile->isDynamicChannels()) {
               return true;
           }
       }
       return false;
    }

    bool hasDynamicRateFor(audio_format_t format) const
    {
        for (size_t i = 0; i < size(); i++) {
            sp<AudioProfile> profile = itemAt(i);
            if (profile->getFormat() == format && profile->isDynamicRate()) {
                return true;
            }
        }
        return false;
    }

    // One audio profile will be added for each format supported by Audio HAL
    void setFormats(const FormatVector &formats)
    {
        // Only allow to change the format of dynamic profile
        sp<AudioProfile> dynamicFormatProfile = getProfileFor(gDynamicFormat);
        if (dynamicFormatProfile == 0) {
            return;
        }
        for (size_t i = 0; i < formats.size(); i++) {
            sp<AudioProfile> profile = new AudioProfile(formats[i],
                                                        dynamicFormatProfile->getChannels(),
                                                        dynamicFormatProfile->getSampleRates());
            profile->setDynamicFormat(true);
            profile->setDynamicChannels(dynamicFormatProfile->isDynamicChannels());
            profile->setDynamicRate(dynamicFormatProfile->isDynamicRate());
            add(profile);
        }
    }

    void clearProfiles()
    {
        for (size_t i = size(); i != 0; ) {
            sp<AudioProfile> profile = itemAt(--i);
            if (profile->isDynamicFormat() && profile->hasValidFormat()) {
                removeAt(i);
                continue;
            }
            profile->clear();
        }
    }

    void dump(int fd, int spaces) const
    {
        const size_t SIZE = 256;
        char buffer[SIZE];

        snprintf(buffer, SIZE, "%*s- Profiles:\n", spaces, "");
        write(fd, buffer, strlen(buffer));
        for (size_t i = 0; i < size(); i++) {
            snprintf(buffer, SIZE, "%*sProfile %zu:", spaces + 4, "", i);
            write(fd, buffer, strlen(buffer));
            itemAt(i)->dump(fd, spaces + 8);
        }
    }

private:
    void setSampleRatesFor(const SampleRateVector &sampleRates, audio_format_t format)
    {
        for (size_t i = 0; i < size(); i++) {
            sp<AudioProfile> profile = itemAt(i);
            if (profile->getFormat() == format && profile->isDynamicRate()) {
                if (profile->hasValidRates()) {
                    // Need to create a new profile with same format
                    sp<AudioProfile> profileToAdd = new AudioProfile(format, profile->getChannels(),
                                                                     sampleRates);
                    profileToAdd->setDynamicFormat(true); // need to set to allow cleaning
                    add(profileToAdd);
                } else {
                    profile->setSampleRates(sampleRates);
                }
                return;
            }
        }
    }

    void setChannelsFor(const ChannelsVector &channelMasks, audio_format_t format)
    {
        for (size_t i = 0; i < size(); i++) {
            sp<AudioProfile> profile = itemAt(i);
            if (profile->getFormat() == format && profile->isDynamicChannels()) {
                if (profile->hasValidChannels()) {
                    // Need to create a new profile with same format
                    sp<AudioProfile> profileToAdd = new AudioProfile(format, channelMasks,
                                                                     profile->getSampleRates());
                    profileToAdd->setDynamicFormat(true); // need to set to allow cleaning
                    add(profileToAdd);
                } else {
                    profile->setChannels(channelMasks);
                }
                return;
            }
        }
    }

    sp<AudioProfile> getProfileFor(audio_format_t format) const
    {
        for (size_t i = 0; i < size(); i++) {
            if (itemAt(i)->getFormat() == format) {
                return itemAt(i);
            }
        }
        return 0;
    }

    static int compareFormats(const sp<AudioProfile> *profile1, const sp<AudioProfile> *profile2);
};

bool operator == (const AudioProfile &left, const AudioProfile &right);

}; // namespace android
