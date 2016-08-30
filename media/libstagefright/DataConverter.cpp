/*
 * Copyright (C) 2016 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "DataConverter"

#include "include/DataConverter.h"

#include <audio_utils/primitives.h>

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <system/audio.h>
#include <audio_utils/primitives.h>
#include <audio_utils/format.h>

namespace android {

status_t DataConverter::convert(const sp<ABuffer> &source, sp<ABuffer> &target) {
    CHECK(source->base() != target->base());
    size_t size = targetSize(source->size());
    status_t err = OK;
    if (size > target->capacity()) {
        ALOGE("data size (%zu) is greater than buffer capacity (%zu)",
                size,          // this is the data received/to be converted
                target->capacity()); // this is out buffer size
        err = FAILED_TRANSACTION;
    } else {
        err = safeConvert(source, target);
    }
    target->setRange(0, err == OK ? size : 0);
    return err;
}

status_t DataConverter::safeConvert(const sp<ABuffer> &source, sp<ABuffer> &target) {
    memcpy(target->base(), source->data(), source->size());
    return OK;
}

size_t DataConverter::sourceSize(size_t targetSize) {
    return targetSize;
}

size_t DataConverter::targetSize(size_t sourceSize) {
    return sourceSize;
}

DataConverter::~DataConverter() { }


size_t SampleConverterBase::sourceSize(size_t targetSize) {
    size_t numSamples = targetSize / mTargetSampleSize;
    if (numSamples > SIZE_MAX / mSourceSampleSize) {
        ALOGW("limiting source size due to overflow (%zu*%zu/%zu)",
                targetSize, mSourceSampleSize, mTargetSampleSize);
        return SIZE_MAX;
    }
    return numSamples * mSourceSampleSize;
}

size_t SampleConverterBase::targetSize(size_t sourceSize) {
    // we round up on conversion
    size_t numSamples = divUp(sourceSize, (size_t)mSourceSampleSize);
    if (numSamples > SIZE_MAX / mTargetSampleSize) {
        ALOGW("limiting target size due to overflow (%zu*%zu/%zu)",
                sourceSize, mTargetSampleSize, mSourceSampleSize);
        return SIZE_MAX;
    }
    return numSamples * mTargetSampleSize;
}

static audio_format_t getAudioFormat(AudioEncoding e) {
    audio_format_t format = AUDIO_FORMAT_INVALID;
    switch (e) {
        case kAudioEncodingPcm16bit:
            format = AUDIO_FORMAT_PCM_16_BIT;
            break;
        case kAudioEncodingPcm8bit:
            format = AUDIO_FORMAT_PCM_8_BIT;
            break;
        case kAudioEncodingPcmFloat:
            format = AUDIO_FORMAT_PCM_FLOAT;
            break;
       case kAudioEncodingPcm24bitPacked:
            format = AUDIO_FORMAT_PCM_24_BIT_PACKED;
            break;
        default:
            ALOGE("Invalid AudioEncoding %d", e);
        }
        return format;
}

static size_t getAudioSampleSize(AudioEncoding e) {
    switch (e) {
        case kAudioEncodingPcm16bit:
        case kAudioEncodingPcm8bit:
        case kAudioEncodingPcmFloat:
        case kAudioEncodingPcm24bitPacked:
            return audio_bytes_per_sample(getAudioFormat(e));
        default: return 0;
    }
}


// static
AudioConverter* AudioConverter::Create(AudioEncoding source, AudioEncoding target) {
    uint32_t sourceSampleSize = getAudioSampleSize(source);
    uint32_t targetSampleSize = getAudioSampleSize(target);
    if (sourceSampleSize && targetSampleSize && sourceSampleSize != targetSampleSize) {
        return new AudioConverter(source, sourceSampleSize, target, targetSampleSize);
    }
    return NULL;
}

status_t AudioConverter::safeConvert(const sp<ABuffer> &src, sp<ABuffer> &tgt) {
    if (mTo == kAudioEncodingPcm8bit && mFrom == kAudioEncodingPcm16bit) {
        memcpy_to_u8_from_i16((uint8_t*)tgt->base(), (const int16_t*)src->data(), src->size() / 2);
    } else if (mTo == kAudioEncodingPcm8bit && mFrom == kAudioEncodingPcmFloat) {
        memcpy_to_u8_from_float((uint8_t*)tgt->base(), (const float*)src->data(), src->size() / 4);
    } else if (mTo == kAudioEncodingPcm16bit && mFrom == kAudioEncodingPcm8bit) {
        memcpy_to_i16_from_u8((int16_t*)tgt->base(), (const uint8_t*)src->data(), src->size());
    } else if (mTo == kAudioEncodingPcm16bit && mFrom == kAudioEncodingPcmFloat) {
        memcpy_to_i16_from_float((int16_t*)tgt->base(), (const float*)src->data(), src->size() / 4);
    } else if (mTo == kAudioEncodingPcmFloat && mFrom == kAudioEncodingPcm8bit) {
        memcpy_to_float_from_u8((float*)tgt->base(), (const uint8_t*)src->data(), src->size());
    } else if (mTo == kAudioEncodingPcmFloat && mFrom == kAudioEncodingPcm16bit) {
        memcpy_to_float_from_i16((float*)tgt->base(), (const int16_t*)src->data(), src->size() / 2);
    } else {
        audio_format_t srcFormat = getAudioFormat(mFrom);
        audio_format_t dstFormat = getAudioFormat(mTo);

        if ((srcFormat == AUDIO_FORMAT_INVALID) || (dstFormat == AUDIO_FORMAT_INVALID))
            return INVALID_OPERATION;

        size_t frames = src->size() / audio_bytes_per_sample(srcFormat);
        memcpy_by_audio_format((void*)tgt->base(), dstFormat, (void*)src->data(),
                srcFormat, frames);
    }
    return OK;
}

} // namespace android
