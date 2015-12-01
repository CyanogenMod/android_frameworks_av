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

#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <system/audio.h>
#include <cutils/config_utils.h>

namespace android {

class HwModule;
class AudioGain;
typedef Vector<sp<AudioGain> > AudioGainCollection;

class AudioPort : public virtual RefBase
{
public:
    AudioPort(const String8& name, audio_port_type_t type,  audio_port_role_t role) :
        mName(name), mType(type), mRole(role), mFlags(AUDIO_OUTPUT_FLAG_NONE) {}

    virtual ~AudioPort() {}

    void setName(const String8 &name) { mName = name; }
    const String8 &getName() const { return mName; }

    audio_port_type_t getType() const { return mType; }
    audio_port_role_t getRole() const { return mRole; }

    void setGains(const AudioGainCollection &gains) { mGains = gains; }
    const AudioGainCollection &getGains() const { return mGains; }

    void setFlags(uint32_t flags) { mFlags = flags; }
    uint32_t getFlags() const { return mFlags; }

    virtual void attach(const sp<HwModule>& module);
    bool isAttached() { return mModule != 0; }

    static audio_port_handle_t getNextUniqueId();

    virtual void toAudioPort(struct audio_port *port) const;

    virtual void importAudioPort(const sp<AudioPort> port);
    void clearCapabilities();

    void setSupportedFormats(const Vector <audio_format_t> &formats);
    void setSupportedSamplingRates(const Vector <uint32_t> &sampleRates)
    {
        mSamplingRates = sampleRates;
    }
    void setSupportedChannelMasks(const Vector <audio_channel_mask_t> &channelMasks)
    {
        mChannelMasks = channelMasks;
    }

    // searches for an exact match
    status_t checkExactSamplingRate(uint32_t samplingRate) const;
    // searches for a compatible match, and returns the best match via updatedSamplingRate
    status_t checkCompatibleSamplingRate(uint32_t samplingRate,
            uint32_t *updatedSamplingRate) const;
    // searches for an exact match
    status_t checkExactChannelMask(audio_channel_mask_t channelMask) const;
    // searches for a compatible match, currently implemented for input channel masks only
    status_t checkCompatibleChannelMask(audio_channel_mask_t channelMask,
            audio_channel_mask_t *updatedChannelMask) const;

    status_t checkExactFormat(audio_format_t format) const;
    // searches for a compatible match, currently implemented for input formats only
    status_t checkCompatibleFormat(audio_format_t format, audio_format_t *updatedFormat) const;
    status_t checkGain(const struct audio_gain_config *gainConfig, int index) const;

    uint32_t pickSamplingRate() const;
    audio_channel_mask_t pickChannelMask() const;
    audio_format_t pickFormat() const;

    static const audio_format_t sPcmFormatCompareTable[];
    static int compareFormats(const audio_format_t *format1, const audio_format_t *format2) {
        return compareFormats(*format1, *format2);
    }
    static int compareFormats(audio_format_t format1, audio_format_t format2);

    audio_module_handle_t getModuleHandle() const;
    uint32_t getModuleVersion() const;
    const char *getModuleName() const;

    bool useInputChannelMask() const
    {
        return ((mType == AUDIO_PORT_TYPE_DEVICE) && (mRole == AUDIO_PORT_ROLE_SOURCE)) ||
                ((mType == AUDIO_PORT_TYPE_MIX) && (mRole == AUDIO_PORT_ROLE_SINK));
    }

    void dump(int fd, int spaces) const;
    void log(const char* indent) const;

    // by convention, "0' in the first entry in mSamplingRates, mChannelMasks or mFormats
    // indicates the supported parameters should be read from the output stream
    // after it is opened for the first time
    Vector <uint32_t> mSamplingRates; // supported sampling rates
    Vector <audio_channel_mask_t> mChannelMasks; // supported channel masks
    Vector <audio_format_t> mFormats; // supported audio formats
    AudioGainCollection mGains; // gain controllers
    sp<HwModule> mModule;                 // audio HW module exposing this I/O stream

private:
    String8           mName;
    audio_port_type_t mType;
    audio_port_role_t mRole;
    uint32_t mFlags; // attribute flags mask (e.g primary output, direct output...).
    static volatile int32_t mNextUniqueId;
};

class AudioPortConfig : public virtual RefBase
{
public:
    AudioPortConfig();
    virtual ~AudioPortConfig() {}

    status_t applyAudioPortConfig(const struct audio_port_config *config,
            struct audio_port_config *backupConfig = NULL);
    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
            const struct audio_port_config *srcConfig = NULL) const = 0;
    virtual sp<AudioPort> getAudioPort() const = 0;
    uint32_t mSamplingRate;
    audio_format_t mFormat;
    audio_channel_mask_t mChannelMask;
    struct audio_gain_config mGain;
};

}; // namespace android
