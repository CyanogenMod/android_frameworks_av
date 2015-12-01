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

#define LOG_TAG "APM::AudioPolicyEngine/Stream"

#include "Stream.h"
#include <system/audio.h>

using std::string;

namespace android
{
namespace audio_policy
{

status_t Element<audio_stream_type_t>::setIdentifier(audio_stream_type_t identifier)
{
    if (identifier > AUDIO_STREAM_CNT) {
        return BAD_VALUE;
    }
    mIdentifier = identifier;
    ALOGD("%s: Stream %s identifier 0x%X", __FUNCTION__, getName().c_str(), identifier);
    return NO_ERROR;
}

/**
* Set the strategy to follow for this stream.
* It checks if the strategy is valid.
*
* @param[in] strategy to be followed.
*
* @return NO_ERROR if the strategy is set correctly, error code otherwise.
*/
template <>
status_t Element<audio_stream_type_t>::set<routing_strategy>(routing_strategy strategy)
{
    if (strategy >= NUM_STRATEGIES) {
        return BAD_VALUE;
    }
    mApplicableStrategy = strategy;
    ALOGD("%s: 0x%X for Stream %s", __FUNCTION__, strategy, getName().c_str());
    return NO_ERROR;
}

template <>
routing_strategy Element<audio_stream_type_t>::get<routing_strategy>() const
{
    ALOGV("%s: 0x%X for Stream %s", __FUNCTION__, mApplicableStrategy, getName().c_str());
    return mApplicableStrategy;
}

status_t Element<audio_stream_type_t>::setVolumeProfile(device_category category,
                                                        const VolumeCurvePoints &points)
{
    ALOGD("%s: adding volume profile for %s for device category %d, points nb =%d", __FUNCTION__,
          getName().c_str(), category, points.size());
    mVolumeProfiles[category] = points;

    for (size_t i = 0; i < points.size(); i++) {
        ALOGV("%s: %s cat=%d curve index =%d Index=%d dBAttenuation=%f",
              __FUNCTION__, getName().c_str(), category, i, points[i].mIndex,
             points[i].mDBAttenuation);
    }
    return NO_ERROR;
}

status_t Element<audio_stream_type_t>::initVolume(int indexMin, int indexMax)
{
    ALOGV("initStreamVolume() stream %s, min %d, max %d", getName().c_str(), indexMin, indexMax);
    if (indexMin < 0 || indexMin >= indexMax) {
        ALOGW("initStreamVolume() invalid index limits for stream %s, min %d, max %d",
              getName().c_str(), indexMin, indexMax);
        return BAD_VALUE;
    }
    mIndexMin = indexMin;
    mIndexMax = indexMax;

    return NO_ERROR;
}

float Element<audio_stream_type_t>::volIndexToDb(device_category deviceCategory, int indexInUi)
{
    VolumeProfileConstIterator it = mVolumeProfiles.find(deviceCategory);
    if (it == mVolumeProfiles.end()) {
        ALOGE("%s: device category %d not found for stream %s", __FUNCTION__, deviceCategory,
              getName().c_str());
        return 1.0f;
    }
    const VolumeCurvePoints curve = mVolumeProfiles[deviceCategory];
    if (curve.size() != Volume::VOLCNT) {
        ALOGE("%s: invalid profile for category %d and for stream %s", __FUNCTION__, deviceCategory,
              getName().c_str());
        return 1.0f;
    }

    // the volume index in the UI is relative to the min and max volume indices for this stream type
    int nbSteps = 1 + curve[Volume::VOLMAX].mIndex -
            curve[Volume::VOLMIN].mIndex;

    if (mIndexMax - mIndexMin == 0) {
        ALOGE("%s: Invalid volume indexes Min=Max=%d", __FUNCTION__, mIndexMin);
        return 1.0f;
    }
    int volIdx = (nbSteps * (indexInUi - mIndexMin)) /
            (mIndexMax - mIndexMin);

    // find what part of the curve this index volume belongs to, or if it's out of bounds
    int segment = 0;
    if (volIdx < curve[Volume::VOLMIN].mIndex) {         // out of bounds
        return 0.0f;
    } else if (volIdx < curve[Volume::VOLKNEE1].mIndex) {
        segment = 0;
    } else if (volIdx < curve[Volume::VOLKNEE2].mIndex) {
        segment = 1;
    } else if (volIdx <= curve[Volume::VOLMAX].mIndex) {
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

    ALOGV("VOLUME vol index=[%d %d %d], dB=[%.1f %.1f %.1f]",
            curve[segment].mIndex, volIdx,
            curve[segment+1].mIndex,
            curve[segment].mDBAttenuation,
            decibels,
            curve[segment+1].mDBAttenuation);

    return decibels;
}

} // namespace audio_policy
} // namespace android

