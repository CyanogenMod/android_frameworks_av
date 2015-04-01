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

#define LOG_TAG "APM::AudioGain"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include "AudioGain.h"
#include <utils/Log.h>
#include <utils/String8.h>

#include <math.h>

namespace android {

const VolumeCurvePoint
ApmGains::sDefaultVolumeCurve[ApmGains::VOLCNT] = {
    {1, -49.5f}, {33, -33.5f}, {66, -17.0f}, {100, 0.0f}
};


const VolumeCurvePoint
ApmGains::sDefaultMediaVolumeCurve[ApmGains::VOLCNT] = {
    {1, -58.0f}, {20, -40.0f}, {60, -17.0f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sExtMediaSystemVolumeCurve[ApmGains::VOLCNT] = {
    {1, -58.0f}, {20, -40.0f}, {60, -21.0f}, {100, -10.0f}
};

const VolumeCurvePoint
ApmGains::sSpeakerMediaVolumeCurve[ApmGains::VOLCNT] = {
    {1, -56.0f}, {20, -34.0f}, {60, -11.0f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sSpeakerMediaVolumeCurveDrc[ApmGains::VOLCNT] = {
    {1, -55.0f}, {20, -43.0f}, {86, -12.0f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sSpeakerSonificationVolumeCurve[ApmGains::VOLCNT] = {
    {1, -29.7f}, {33, -20.1f}, {66, -10.2f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sSpeakerSonificationVolumeCurveDrc[ApmGains::VOLCNT] = {
    {1, -35.7f}, {33, -26.1f}, {66, -13.2f}, {100, 0.0f}
};

// AUDIO_STREAM_SYSTEM, AUDIO_STREAM_ENFORCED_AUDIBLE and AUDIO_STREAM_DTMF volume tracks
// AUDIO_STREAM_RING on phones and AUDIO_STREAM_MUSIC on tablets.
// AUDIO_STREAM_DTMF tracks AUDIO_STREAM_VOICE_CALL while in call (See AudioService.java).
// The range is constrained between -24dB and -6dB over speaker and -30dB and -18dB over headset.

const VolumeCurvePoint
ApmGains::sDefaultSystemVolumeCurve[ApmGains::VOLCNT] = {
    {1, -24.0f}, {33, -18.0f}, {66, -12.0f}, {100, -6.0f}
};

const VolumeCurvePoint
ApmGains::sDefaultSystemVolumeCurveDrc[ApmGains::VOLCNT] = {
    {1, -34.0f}, {33, -24.0f}, {66, -15.0f}, {100, -6.0f}
};

const VolumeCurvePoint
ApmGains::sHeadsetSystemVolumeCurve[ApmGains::VOLCNT] = {
    {1, -30.0f}, {33, -26.0f}, {66, -22.0f}, {100, -18.0f}
};

const VolumeCurvePoint
ApmGains::sDefaultVoiceVolumeCurve[ApmGains::VOLCNT] = {
    {0, -42.0f}, {33, -28.0f}, {66, -14.0f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sSpeakerVoiceVolumeCurve[ApmGains::VOLCNT] = {
    {0, -24.0f}, {33, -16.0f}, {66, -8.0f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sLinearVolumeCurve[ApmGains::VOLCNT] = {
    {0, -96.0f}, {33, -68.0f}, {66, -34.0f}, {100, 0.0f}
};

const VolumeCurvePoint
ApmGains::sSilentVolumeCurve[ApmGains::VOLCNT] = {
    {0, -96.0f}, {1, -96.0f}, {2, -96.0f}, {100, -96.0f}
};

const VolumeCurvePoint
ApmGains::sFullScaleVolumeCurve[ApmGains::VOLCNT] = {
    {0, 0.0f}, {1, 0.0f}, {2, 0.0f}, {100, 0.0f}
};

const VolumeCurvePoint *ApmGains::sVolumeProfiles[AUDIO_STREAM_CNT]
                                                  [ApmGains::DEVICE_CATEGORY_CNT] = {
    { // AUDIO_STREAM_VOICE_CALL
        ApmGains::sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_SYSTEM
        ApmGains::sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultSystemVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        ApmGains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_RING
        ApmGains::sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        ApmGains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_MUSIC
        ApmGains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ALARM
        ApmGains::sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        ApmGains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_NOTIFICATION
        ApmGains::sDefaultVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerSonificationVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultVolumeCurve,  // DEVICE_CATEGORY_EARPIECE
        ApmGains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_BLUETOOTH_SCO
        ApmGains::sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerVoiceVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultVoiceVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ENFORCED_AUDIBLE
        ApmGains::sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    {  // AUDIO_STREAM_DTMF
        ApmGains::sHeadsetSystemVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultSystemVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sExtMediaSystemVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_TTS
      // "Transmitted Through Speaker": always silent except on DEVICE_CATEGORY_SPEAKER
        ApmGains::sSilentVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sLinearVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sSilentVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sSilentVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_ACCESSIBILITY
        ApmGains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sSpeakerMediaVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sDefaultMediaVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sDefaultMediaVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_REROUTING
        ApmGains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sFullScaleVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
    { // AUDIO_STREAM_PATCH
        ApmGains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_HEADSET
        ApmGains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_SPEAKER
        ApmGains::sFullScaleVolumeCurve, // DEVICE_CATEGORY_EARPIECE
        ApmGains::sFullScaleVolumeCurve  // DEVICE_CATEGORY_EXT_MEDIA
    },
};

//static
audio_devices_t ApmGains::getDeviceForVolume(audio_devices_t device)
{
    if (device == AUDIO_DEVICE_NONE) {
        // this happens when forcing a route update and no track is active on an output.
        // In this case the returned category is not important.
        device =  AUDIO_DEVICE_OUT_SPEAKER;
    } else if (popcount(device) > 1) {
        // Multiple device selection is either:
        //  - speaker + one other device: give priority to speaker in this case.
        //  - one A2DP device + another device: happens with duplicated output. In this case
        // retain the device on the A2DP output as the other must not correspond to an active
        // selection if not the speaker.
        //  - HDMI-CEC system audio mode only output: give priority to available item in order.
        if (device & AUDIO_DEVICE_OUT_SPEAKER) {
            device = AUDIO_DEVICE_OUT_SPEAKER;
        } else if (device & AUDIO_DEVICE_OUT_HDMI_ARC) {
            device = AUDIO_DEVICE_OUT_HDMI_ARC;
        } else if (device & AUDIO_DEVICE_OUT_AUX_LINE) {
            device = AUDIO_DEVICE_OUT_AUX_LINE;
        } else if (device & AUDIO_DEVICE_OUT_SPDIF) {
            device = AUDIO_DEVICE_OUT_SPDIF;
        } else {
            device = (audio_devices_t)(device & AUDIO_DEVICE_OUT_ALL_A2DP);
        }
    }

    /*SPEAKER_SAFE is an alias of SPEAKER for purposes of volume control*/
    if (device == AUDIO_DEVICE_OUT_SPEAKER_SAFE)
        device = AUDIO_DEVICE_OUT_SPEAKER;

    ALOGW_IF(popcount(device) != 1,
            "getDeviceForVolume() invalid device combination: %08x",
            device);

    return device;
}

//static
ApmGains::device_category ApmGains::getDeviceCategory(audio_devices_t device)
{
    switch(getDeviceForVolume(device)) {
        case AUDIO_DEVICE_OUT_EARPIECE:
            return ApmGains::DEVICE_CATEGORY_EARPIECE;
        case AUDIO_DEVICE_OUT_WIRED_HEADSET:
        case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
            return ApmGains::DEVICE_CATEGORY_HEADSET;
        case AUDIO_DEVICE_OUT_LINE:
        case AUDIO_DEVICE_OUT_AUX_DIGITAL:
        /*USB?  Remote submix?*/
            return ApmGains::DEVICE_CATEGORY_EXT_MEDIA;
        case AUDIO_DEVICE_OUT_SPEAKER:
        case AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
        case AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER:
        case AUDIO_DEVICE_OUT_USB_ACCESSORY:
        case AUDIO_DEVICE_OUT_USB_DEVICE:
        case AUDIO_DEVICE_OUT_REMOTE_SUBMIX:
        default:
            return ApmGains::DEVICE_CATEGORY_SPEAKER;
    }
}

//static
float ApmGains::volIndexToAmpl(audio_devices_t device, const StreamDescriptor& streamDesc,
        int indexInUi)
{
    ApmGains::device_category deviceCategory = ApmGains::getDeviceCategory(device);
    const VolumeCurvePoint *curve = streamDesc.mVolumeCurve[deviceCategory];

    // the volume index in the UI is relative to the min and max volume indices for this stream type
    int nbSteps = 1 + curve[ApmGains::VOLMAX].mIndex -
            curve[ApmGains::VOLMIN].mIndex;
    int volIdx = (nbSteps * (indexInUi - streamDesc.mIndexMin)) /
            (streamDesc.mIndexMax - streamDesc.mIndexMin);

    // find what part of the curve this index volume belongs to, or if it's out of bounds
    int segment = 0;
    if (volIdx < curve[ApmGains::VOLMIN].mIndex) {         // out of bounds
        return 0.0f;
    } else if (volIdx < curve[ApmGains::VOLKNEE1].mIndex) {
        segment = 0;
    } else if (volIdx < curve[ApmGains::VOLKNEE2].mIndex) {
        segment = 1;
    } else if (volIdx <= curve[ApmGains::VOLMAX].mIndex) {
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

    float amplification = exp( decibels * 0.115129f); // exp( dB * ln(10) / 20 )

    ALOGVV("VOLUME vol index=[%d %d %d], dB=[%.1f %.1f %.1f] ampl=%.5f",
            curve[segment].mIndex, volIdx,
            curve[segment+1].mIndex,
            curve[segment].mDBAttenuation,
            decibels,
            curve[segment+1].mDBAttenuation,
            amplification);

    return amplification;
}



AudioGain::AudioGain(int index, bool useInChannelMask)
{
    mIndex = index;
    mUseInChannelMask = useInChannelMask;
    memset(&mGain, 0, sizeof(struct audio_gain));
}

void AudioGain::getDefaultConfig(struct audio_gain_config *config)
{
    config->index = mIndex;
    config->mode = mGain.mode;
    config->channel_mask = mGain.channel_mask;
    if ((mGain.mode & AUDIO_GAIN_MODE_JOINT) == AUDIO_GAIN_MODE_JOINT) {
        config->values[0] = mGain.default_value;
    } else {
        uint32_t numValues;
        if (mUseInChannelMask) {
            numValues = audio_channel_count_from_in_mask(mGain.channel_mask);
        } else {
            numValues = audio_channel_count_from_out_mask(mGain.channel_mask);
        }
        for (size_t i = 0; i < numValues; i++) {
            config->values[i] = mGain.default_value;
        }
    }
    if ((mGain.mode & AUDIO_GAIN_MODE_RAMP) == AUDIO_GAIN_MODE_RAMP) {
        config->ramp_duration_ms = mGain.min_ramp_ms;
    }
}

status_t AudioGain::checkConfig(const struct audio_gain_config *config)
{
    if ((config->mode & ~mGain.mode) != 0) {
        return BAD_VALUE;
    }
    if ((config->mode & AUDIO_GAIN_MODE_JOINT) == AUDIO_GAIN_MODE_JOINT) {
        if ((config->values[0] < mGain.min_value) ||
                    (config->values[0] > mGain.max_value)) {
            return BAD_VALUE;
        }
    } else {
        if ((config->channel_mask & ~mGain.channel_mask) != 0) {
            return BAD_VALUE;
        }
        uint32_t numValues;
        if (mUseInChannelMask) {
            numValues = audio_channel_count_from_in_mask(config->channel_mask);
        } else {
            numValues = audio_channel_count_from_out_mask(config->channel_mask);
        }
        for (size_t i = 0; i < numValues; i++) {
            if ((config->values[i] < mGain.min_value) ||
                    (config->values[i] > mGain.max_value)) {
                return BAD_VALUE;
            }
        }
    }
    if ((config->mode & AUDIO_GAIN_MODE_RAMP) == AUDIO_GAIN_MODE_RAMP) {
        if ((config->ramp_duration_ms < mGain.min_ramp_ms) ||
                    (config->ramp_duration_ms > mGain.max_ramp_ms)) {
            return BAD_VALUE;
        }
    }
    return NO_ERROR;
}

void AudioGain::dump(int fd, int spaces, int index) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%*sGain %d:\n", spaces, "", index+1);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- mode: %08x\n", spaces, "", mGain.mode);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- channel_mask: %08x\n", spaces, "", mGain.channel_mask);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- min_value: %d mB\n", spaces, "", mGain.min_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- max_value: %d mB\n", spaces, "", mGain.max_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- default_value: %d mB\n", spaces, "", mGain.default_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- step_value: %d mB\n", spaces, "", mGain.step_value);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- min_ramp_ms: %d ms\n", spaces, "", mGain.min_ramp_ms);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- max_ramp_ms: %d ms\n", spaces, "", mGain.max_ramp_ms);
    result.append(buffer);

    write(fd, result.string(), result.size());
}


// --- StreamDescriptor class implementation

StreamDescriptor::StreamDescriptor()
    :   mIndexMin(0), mIndexMax(1), mCanBeMuted(true)
{
    mIndexCur.add(AUDIO_DEVICE_OUT_DEFAULT, 0);
}

int StreamDescriptor::getVolumeIndex(audio_devices_t device)
{
    device = ApmGains::getDeviceForVolume(device);
    // there is always a valid entry for AUDIO_DEVICE_OUT_DEFAULT
    if (mIndexCur.indexOfKey(device) < 0) {
        device = AUDIO_DEVICE_OUT_DEFAULT;
    }
    return mIndexCur.valueFor(device);
}

void StreamDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%s         %02d         %02d         ",
             mCanBeMuted ? "true " : "false", mIndexMin, mIndexMax);
    result.append(buffer);
    for (size_t i = 0; i < mIndexCur.size(); i++) {
        snprintf(buffer, SIZE, "%04x : %02d, ",
                 mIndexCur.keyAt(i),
                 mIndexCur.valueAt(i));
        result.append(buffer);
    }
    result.append("\n");

    write(fd, result.string(), result.size());
}

}; // namespace android
