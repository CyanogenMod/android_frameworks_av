/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2006-2011 The Android Open Source Project
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

#define LOG_TAG "AudioParameter"
//#define LOG_NDEBUG 0

#include <utils/Log.h>

#include <hardware/audio.h>
#include <media/AudioParameter.h>

namespace android {

// static
const char * const AudioParameter::keyRouting = AUDIO_PARAMETER_STREAM_ROUTING;
const char * const AudioParameter::keySamplingRate = AUDIO_PARAMETER_STREAM_SAMPLING_RATE;
const char * const AudioParameter::keyFormat = AUDIO_PARAMETER_STREAM_FORMAT;
const char * const AudioParameter::keyChannels = AUDIO_PARAMETER_STREAM_CHANNELS;
const char * const AudioParameter::keyFrameCount = AUDIO_PARAMETER_STREAM_FRAME_COUNT;
const char * const AudioParameter::keyInputSource = AUDIO_PARAMETER_STREAM_INPUT_SOURCE;
const char * const AudioParameter::keyScreenState = AUDIO_PARAMETER_KEY_SCREEN_STATE;
#ifdef QCOM_HARDWARE
const char * const AudioParameter::keyHandleFm = AUDIO_PARAMETER_KEY_HANDLE_FM;
const char * const AudioParameter::keyVoipCheck = AUDIO_PARAMETER_KEY_VOIP_CHECK;
const char * const AudioParameter::keyFluenceType = AUDIO_PARAMETER_KEY_FLUENCE_TYPE;
const char * const AudioParameter::keySSR = AUDIO_PARAMETER_KEY_SSR;
const char * const AudioParameter::keyHandleA2dpDevice = AUDIO_PARAMETER_KEY_HANDLE_A2DP_DEVICE;
const char * const AudioParameter::keyADSPStatus = AUDIO_PARAMETER_KEY_ADSP_STATUS;
const char * const AudioParameter::keyCanOpenProxy = AUDIO_CAN_OPEN_PROXY;
const char * const AudioParameter::keyFmVolume = AUDIO_PARAMETER_KEY_FM_VOLUME;
#endif

const char * const AudioParameter::keySoundCardStatus = AUDIO_PARAMETER_KEY_SND_CARD_STATUS;

AudioParameter::AudioParameter(const String8& keyValuePairs)
{
    char *str = new char[keyValuePairs.length()+1];
    mKeyValuePairs = keyValuePairs;

    strcpy(str, keyValuePairs.string());
    char *pair = strtok(str, ";");
    while (pair != NULL) {
        if (strlen(pair) != 0) {
            size_t eqIdx = strcspn(pair, "=");
            String8 key = String8(pair, eqIdx);
            String8 value;
            if (eqIdx == strlen(pair)) {
                value = String8("");
            } else {
                value = String8(pair + eqIdx + 1);
            }
            if (mParameters.indexOfKey(key) < 0) {
                mParameters.add(key, value);
            } else {
                mParameters.replaceValueFor(key, value);
            }
        } else {
            ALOGV("AudioParameter() cstor empty key value pair");
        }
        pair = strtok(NULL, ";");
    }

    delete[] str;
}

AudioParameter::~AudioParameter()
{
    mParameters.clear();
}

String8 AudioParameter::toString()
{
    String8 str = String8("");

    size_t size = mParameters.size();
    for (size_t i = 0; i < size; i++) {
        str += mParameters.keyAt(i);
        str += "=";
        str += mParameters.valueAt(i);
        if (i < (size - 1)) str += ";";
    }
    return str;
}

status_t AudioParameter::add(const String8& key, const String8& value)
{
    if (mParameters.indexOfKey(key) < 0) {
        mParameters.add(key, value);
        return NO_ERROR;
    } else {
        mParameters.replaceValueFor(key, value);
        return ALREADY_EXISTS;
    }
}

status_t AudioParameter::addInt(const String8& key, const int value)
{
    char str[12];
    if (snprintf(str, 12, "%d", value) > 0) {
        String8 str8 = String8(str);
        return add(key, str8);
    } else {
        return BAD_VALUE;
    }
}

status_t AudioParameter::addFloat(const String8& key, const float value)
{
    char str[23];
    if (snprintf(str, 23, "%.10f", value) > 0) {
        String8 str8 = String8(str);
        return add(key, str8);
    } else {
        return BAD_VALUE;
    }
}

status_t AudioParameter::remove(const String8& key)
{
    if (mParameters.indexOfKey(key) >= 0) {
        mParameters.removeItem(key);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

status_t AudioParameter::get(const String8& key, String8& value)
{
    if (mParameters.indexOfKey(key) >= 0) {
        value = mParameters.valueFor(key);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

status_t AudioParameter::getInt(const String8& key, int& value)
{
    String8 str8;
    status_t result = get(key, str8);
    value = 0;
    if (result == NO_ERROR) {
        int val;
        if (sscanf(str8.string(), "%d", &val) == 1) {
            value = val;
        } else {
            result = INVALID_OPERATION;
        }
    }
    return result;
}

status_t AudioParameter::getFloat(const String8& key, float& value)
{
    String8 str8;
    status_t result = get(key, str8);
    value = 0;
    if (result == NO_ERROR) {
        float val;
        if (sscanf(str8.string(), "%f", &val) == 1) {
            value = val;
        } else {
            result = INVALID_OPERATION;
        }
    }
    return result;
}

status_t AudioParameter::getAt(size_t index, String8& key, String8& value)
{
    if (mParameters.size() > index) {
        key = mParameters.keyAt(index);
        value = mParameters.valueAt(index);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

};  // namespace android
