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

#include <Volume.h>
#include <hardware/audio.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Errors.h>
#include <utils/Vector.h>
#include <system/audio.h>
#include <cutils/config_utils.h>
#include <string>
#include <utility>

namespace android {

typedef std::pair<uint32_t, uint32_t> CurvePoint;
typedef Vector<CurvePoint> CurvePoints;

class VolumeCurve : public RefBase
{
public:
    void setStreamType(audio_stream_type_t streamType) { mStreamType = streamType; }
    audio_stream_type_t getStreamType() const { return mStreamType; }

    void setDeviceCategory(device_category devCat) { mDeviceCategory = devCat; }
    device_category getDeviceCategory() const { return mDeviceCategory; }

    void setCurvePoints(const CurvePoints &points) { mCurvePoints = points; }

private:
    CurvePoints mCurvePoints;
    device_category mDeviceCategory;
    audio_stream_type_t mStreamType;
};

typedef Vector<sp<VolumeCurve> > VolumeCurveCollection;

}; // namespace android
