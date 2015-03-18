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

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <system/audio.h>
#include <utils/KeyedVector.h>

namespace android {

class VolumeCurvePoint
{
public:
    int mIndex;
    float mDBAttenuation;
};

class StreamDescriptor;

class ApmGains
{
public :
    // 4 points to define the volume attenuation curve, each characterized by the volume
    // index (from 0 to 100) at which they apply, and the attenuation in dB at that index.
    // we use 100 steps to avoid rounding errors when computing the volume in volIndexToAmpl()
    enum { VOLMIN = 0, VOLKNEE1 = 1, VOLKNEE2 = 2, VOLMAX = 3, VOLCNT = 4};

    // device categories used for volume curve management.
    enum device_category {
        DEVICE_CATEGORY_HEADSET,
        DEVICE_CATEGORY_SPEAKER,
        DEVICE_CATEGORY_EARPIECE,
        DEVICE_CATEGORY_EXT_MEDIA,
        DEVICE_CATEGORY_CNT
    };

    // returns the category the device belongs to with regard to volume curve management
    static ApmGains::device_category getDeviceCategory(audio_devices_t device);

    // extract one device relevant for volume control from multiple device selection
    static audio_devices_t getDeviceForVolume(audio_devices_t device);

    static float volIndexToAmpl(audio_devices_t device, const StreamDescriptor& streamDesc,
                    int indexInUi);

    // default volume curve
    static const VolumeCurvePoint sDefaultVolumeCurve[ApmGains::VOLCNT];
    // default volume curve for media strategy
    static const VolumeCurvePoint sDefaultMediaVolumeCurve[ApmGains::VOLCNT];
    // volume curve for non-media audio on ext media outputs (HDMI, Line, etc)
    static const VolumeCurvePoint sExtMediaSystemVolumeCurve[ApmGains::VOLCNT];
    // volume curve for media strategy on speakers
    static const VolumeCurvePoint sSpeakerMediaVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sSpeakerMediaVolumeCurveDrc[ApmGains::VOLCNT];
    // volume curve for sonification strategy on speakers
    static const VolumeCurvePoint sSpeakerSonificationVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sSpeakerSonificationVolumeCurveDrc[ApmGains::VOLCNT];
    static const VolumeCurvePoint sDefaultSystemVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sDefaultSystemVolumeCurveDrc[ApmGains::VOLCNT];
    static const VolumeCurvePoint sHeadsetSystemVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sDefaultVoiceVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sSpeakerVoiceVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sLinearVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sSilentVolumeCurve[ApmGains::VOLCNT];
    static const VolumeCurvePoint sFullScaleVolumeCurve[ApmGains::VOLCNT];
    // default volume curves per stream and device category. See initializeVolumeCurves()
    static const VolumeCurvePoint *sVolumeProfiles[AUDIO_STREAM_CNT][ApmGains::DEVICE_CATEGORY_CNT];
};


class AudioGain: public RefBase
{
public:
    AudioGain(int index, bool useInChannelMask);
    virtual ~AudioGain() {}

    void dump(int fd, int spaces, int index) const;

    void getDefaultConfig(struct audio_gain_config *config);
    status_t checkConfig(const struct audio_gain_config *config);
    int               mIndex;
    struct audio_gain mGain;
    bool              mUseInChannelMask;
};


// stream descriptor used for volume control
class StreamDescriptor
{
public:
    StreamDescriptor();

    int getVolumeIndex(audio_devices_t device);
    void dump(int fd);

    int mIndexMin;      // min volume index
    int mIndexMax;      // max volume index
    KeyedVector<audio_devices_t, int> mIndexCur;   // current volume index per device
    bool mCanBeMuted;   // true is the stream can be muted

    const VolumeCurvePoint *mVolumeCurve[ApmGains::DEVICE_CATEGORY_CNT];
};

}; // namespace android
