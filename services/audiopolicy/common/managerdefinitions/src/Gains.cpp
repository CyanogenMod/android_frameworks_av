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

#define LOG_TAG "APM::Gains"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include "Gains.h"
#include <Volume.h>
#include <math.h>
#include <utils/String8.h>

namespace android {

// Enginedefault
const VolumeCurvePoint
Gains::sDefaultVolumeCurve[Volume::VOLCNT] = {
    {1, -49.5f}, {33, -33.5f}, {66, -17.0f}, {100, 0.0f}
};


const VolumeCurvePoint
Gains::sDefaultMediaVolumeCurve[Volume::VOLCNT] = {
    {1, -58.0f}, {20, -40.0f}, {60, -17.0f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sExtMediaSystemVolumeCurve[Volume::VOLCNT] = {
    {1, -58.0f}, {20, -40.0f}, {60, -21.0f}, {100, -10.0f}
};

const VolumeCurvePoint
Gains::sSpeakerMediaVolumeCurve[Volume::VOLCNT] = {
    {1, -56.0f}, {20, -34.0f}, {60, -11.0f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sSpeakerMediaVolumeCurveDrc[Volume::VOLCNT] = {
    {1, -55.0f}, {20, -43.0f}, {86, -12.0f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sSpeakerSonificationVolumeCurve[Volume::VOLCNT] = {
    {1, -29.7f}, {33, -20.1f}, {66, -10.2f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sSpeakerSonificationVolumeCurveDrc[Volume::VOLCNT] = {
    {1, -35.7f}, {33, -26.1f}, {66, -13.2f}, {100, 0.0f}
};

// AUDIO_STREAM_SYSTEM, AUDIO_STREAM_ENFORCED_AUDIBLE and AUDIO_STREAM_DTMF volume tracks
// AUDIO_STREAM_RING on phones and AUDIO_STREAM_MUSIC on tablets.
// AUDIO_STREAM_DTMF tracks AUDIO_STREAM_VOICE_CALL while in call (See AudioService.java).
// The range is constrained between -24dB and -6dB over speaker and -30dB and -18dB over headset.

const VolumeCurvePoint
Gains::sDefaultSystemVolumeCurve[Volume::VOLCNT] = {
    {1, -24.0f}, {33, -18.0f}, {66, -12.0f}, {100, -6.0f}
};

const VolumeCurvePoint
Gains::sDefaultSystemVolumeCurveDrc[Volume::VOLCNT] = {
    {1, -34.0f}, {33, -24.0f}, {66, -15.0f}, {100, -6.0f}
};

const VolumeCurvePoint
Gains::sHeadsetSystemVolumeCurve[Volume::VOLCNT] = {
    {1, -30.0f}, {33, -26.0f}, {66, -22.0f}, {100, -18.0f}
};

const VolumeCurvePoint
Gains::sDefaultVoiceVolumeCurve[Volume::VOLCNT] = {
    {0, -42.0f}, {33, -28.0f}, {66, -14.0f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sSpeakerVoiceVolumeCurve[Volume::VOLCNT] = {
    {0, -24.0f}, {33, -16.0f}, {66, -8.0f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sLinearVolumeCurve[Volume::VOLCNT] = {
    {0, -96.0f}, {33, -68.0f}, {66, -34.0f}, {100, 0.0f}
};

const VolumeCurvePoint
Gains::sSilentVolumeCurve[Volume::VOLCNT] = {
    {0, -96.0f}, {1, -96.0f}, {2, -96.0f}, {100, -96.0f}
};

const VolumeCurvePoint
Gains::sFullScaleVolumeCurve[Volume::VOLCNT] = {
    {0, 0.0f}, {1, 0.0f}, {2, 0.0f}, {100, 0.0f}
};

const VolumeCurvePoint *Gains::sVolumeProfiles[AUDIO_STREAM_CNT]
                                                  [DEVICE_CATEGORY_CNT] = {
    { // AUDIO_STREAM_VOICE_CALL
        Gains::sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_SYSTEM
        Gains::sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultSystemVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        Gains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_RING
        Gains::sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        Gains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_MUSIC
        Gains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ALARM
        Gains::sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        Gains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_NOTIFICATION
        Gains::sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        Gains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_BLUETOOTH_SCO
        Gains::sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ENFORCED_AUDIBLE
        Gains::sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    {  // AUDIO_STREAM_DTMF
        Gains::sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_TTS
      // "Transmitted Through Speaker": always silent except on DEVICE_CATEGORY_SPEAKER
        Gains::sSilentVolumeCurve,    // DEVICE_CATEGORY_HEADSET
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sSilentVolumeCurve,    // DEVICE_CATEGORY_EARPIECE
        Gains::sSilentVolumeCurve     // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ACCESSIBILITY
        Gains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_REROUTING
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sFullScaleVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_PATCH
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_HEADSET
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        Gains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        Gains::sFullScaleVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
};

//static
float Gains::volIndexToDb(const VolumeCurvePoint *curve, int indexMin, int indexMax, int indexInUi)
{
    // the volume index in the UI is relative to the min and max volume indices for this stream type
    int nbSteps = 1 + curve[Volume::VOLMAX].mIndex - curve[Volume::VOLMIN].mIndex;
    int volIdx = (nbSteps * (indexInUi - indexMin)) / (indexMax - indexMin);

    // find what part of the curve this index volume belongs to, or if it's out of bounds
    int segment = 0;
    if (volIdx < curve[Volume::VOLMIN].mIndex) {         // out of bounds
        return VOLUME_MIN_DB;
    } else if (volIdx < curve[Volume::VOLKNEE1].mIndex) {
        segment = 0;
    } else if (volIdx < curve[Volume::VOLKNEE2].mIndex) {
        segment = 1;
    } else if (volIdx <= curve[Volume::VOLMAX].mIndex) {
        segment = 2;
    } else {                                                               // out of bounds
        return 0.0f;
    }

    // linear interpolation in the attenuation table in dB
    float decibels = curve[segment].mDBAttenuation +
            ((float)(volIdx - curve[segment].mIndex)) *
                ( (curve[segment+1].mDBAttenuation -
                        curve[segment].mDBAttenuation) /
                    ((float)(curve[segment+1].mIndex -
                            curve[segment].mIndex)) );

    ALOGVV("VOLUME vol index=[%d %d %d], dB=[%.1f %.1f %.1f]",
            curve[segment].mIndex, volIdx,
            curve[segment+1].mIndex,
            curve[segment].mDBAttenuation,
            decibels,
            curve[segment+1].mDBAttenuation);

    return decibels;
}

}; // namespace android
