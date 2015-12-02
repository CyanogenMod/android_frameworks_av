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

#include <StreamDescriptor.h>
#include <utils/KeyedVector.h>
#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>

namespace android {

class StreamDescriptor;

class Gains
{
public :
    static float volIndexToDb(const VolumeCurvePoint *point, int indexMin, int indexMax,
                              int indexInUi);

    // default volume curve
    static const VolumeCurvePoint sDefaultVolumeCurve[Volume::VOLCNT];
    // default volume curve for media strategy
    static const VolumeCurvePoint sDefaultMediaVolumeCurve[Volume::VOLCNT];
    // volume curve for non-media audio on ext media outputs (HDMI, Line, etc)
    static const VolumeCurvePoint sExtMediaSystemVolumeCurve[Volume::VOLCNT];
    // volume curve for media strategy on speakers
    static const VolumeCurvePoint sSpeakerMediaVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sSpeakerMediaVolumeCurveDrc[Volume::VOLCNT];
    // volume curve for sonification strategy on speakers
    static const VolumeCurvePoint sSpeakerSonificationVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sSpeakerSonificationVolumeCurveDrc[Volume::VOLCNT];
    static const VolumeCurvePoint sDefaultSystemVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sDefaultSystemVolumeCurveDrc[Volume::VOLCNT];
    static const VolumeCurvePoint sHeadsetSystemVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sDefaultVoiceVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sSpeakerVoiceVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sLinearVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sSilentVolumeCurve[Volume::VOLCNT];
    static const VolumeCurvePoint sFullScaleVolumeCurve[Volume::VOLCNT];
    // default volume curves per stream and device category. See initializeVolumeCurves()
    static const VolumeCurvePoint *sVolumeProfiles[AUDIO_STREAM_CNT][DEVICE_CATEGORY_CNT];
};

}; // namespace android
