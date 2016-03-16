/*
 * Copyright (C) 2014 The Android Open Source Project
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

#define LOG_TAG "AudioPolicy"
//#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <media/AudioPolicy.h>

namespace android {

//
//  AudioMixMatchCriterion implementation
//
AudioMixMatchCriterion::AudioMixMatchCriterion(audio_usage_t usage,
                                                 audio_source_t source,
                                                 uint32_t rule)
: mRule(rule)
{
    if (mRule == RULE_MATCH_ATTRIBUTE_USAGE ||
            mRule == RULE_EXCLUDE_ATTRIBUTE_USAGE) {
        mValue.mUsage = usage;
    } else {
        mValue.mSource = source;
    }
}

status_t AudioMixMatchCriterion::readFromParcel(Parcel *parcel)
{
    mRule = parcel->readInt32();
    if (mRule == RULE_MATCH_ATTRIBUTE_USAGE ||
            mRule == RULE_EXCLUDE_ATTRIBUTE_USAGE) {
        mValue.mUsage = (audio_usage_t)parcel->readInt32();
    } else {
        mValue.mSource = (audio_source_t)parcel->readInt32();
    }
    return NO_ERROR;
}

status_t AudioMixMatchCriterion::writeToParcel(Parcel *parcel) const
{
    parcel->writeInt32(mRule);
    parcel->writeInt32(mValue.mUsage);
    return NO_ERROR;
}

//
//  AudioMix implementation
//

status_t AudioMix::readFromParcel(Parcel *parcel)
{
    mMixType = parcel->readInt32();
    mFormat.sample_rate = (uint32_t)parcel->readInt32();
    mFormat.channel_mask = (audio_channel_mask_t)parcel->readInt32();
    mFormat.format = (audio_format_t)parcel->readInt32();
    mRouteFlags = parcel->readInt32();
    mDeviceType = (audio_devices_t) parcel->readInt32();
    mDeviceAddress = parcel->readString8();
    mCbFlags = (uint32_t)parcel->readInt32();
    size_t size = (size_t)parcel->readInt32();
    if (size > MAX_CRITERIA_PER_MIX) {
        size = MAX_CRITERIA_PER_MIX;
    }
    for (size_t i = 0; i < size; i++) {
        AudioMixMatchCriterion criterion;
        if (criterion.readFromParcel(parcel) == NO_ERROR) {
            mCriteria.add(criterion);
        }
    }
    return NO_ERROR;
}

status_t AudioMix::writeToParcel(Parcel *parcel) const
{
    parcel->writeInt32(mMixType);
    parcel->writeInt32(mFormat.sample_rate);
    parcel->writeInt32(mFormat.channel_mask);
    parcel->writeInt32(mFormat.format);
    parcel->writeInt32(mRouteFlags);
    parcel->writeInt32(mDeviceType);
    parcel->writeString8(mDeviceAddress);
    parcel->writeInt32(mCbFlags);
    size_t size = mCriteria.size();
    if (size > MAX_CRITERIA_PER_MIX) {
        size = MAX_CRITERIA_PER_MIX;
    }
    size_t sizePosition = parcel->dataPosition();
    parcel->writeInt32(size);
    size_t finalSize = size;
    for (size_t i = 0; i < size; i++) {
        size_t position = parcel->dataPosition();
        if (mCriteria[i].writeToParcel(parcel) != NO_ERROR) {
            parcel->setDataPosition(position);
            finalSize--;
        }
    }
    if (size != finalSize) {
        size_t position = parcel->dataPosition();
        parcel->setDataPosition(sizePosition);
        parcel->writeInt32(finalSize);
        parcel->setDataPosition(position);
    }
    return NO_ERROR;
}

} // namespace android
