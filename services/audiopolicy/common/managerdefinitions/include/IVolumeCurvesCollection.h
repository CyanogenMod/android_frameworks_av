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
#include <Volume.h>
#include <utils/Errors.h>

namespace android {

class IVolumeCurvesCollection
{
public:
    virtual void clearCurrentVolumeIndex(audio_stream_type_t stream) = 0;
    virtual void addCurrentVolumeIndex(audio_stream_type_t stream, audio_devices_t device,
                                       int index) = 0;
    virtual bool canBeMuted(audio_stream_type_t stream) = 0;
    virtual int getVolumeIndexMin(audio_stream_type_t stream) const = 0;
    virtual int getVolumeIndex(audio_stream_type_t stream, audio_devices_t device) = 0;
    virtual int getVolumeIndexMax(audio_stream_type_t stream) const = 0;
    virtual float volIndexToDb(audio_stream_type_t stream, device_category device,
                               int indexInUi) const = 0;
    virtual status_t initStreamVolume(audio_stream_type_t stream, int indexMin, int indexMax) = 0;

    virtual void initializeVolumeCurves(bool /*isSpeakerDrcEnabled*/) {}
    virtual void switchVolumeCurve(audio_stream_type_t src, audio_stream_type_t dst) = 0;
    virtual void restoreOriginVolumeCurve(audio_stream_type_t stream)
    {
        switchVolumeCurve(stream, stream);
    }

    virtual status_t dump(int fd) const = 0;

protected:
    virtual ~IVolumeCurvesCollection() {}
};

}; // namespace android
