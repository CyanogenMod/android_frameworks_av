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

#define LOG_TAG "BufferProvider"
//#define LOG_NDEBUG 0

#include <audio_effects/effect_downmix.h>
#include <audio_utils/primitives.h>
#include <audio_utils/format.h>
#include <media/EffectsFactoryApi.h>
#include <utils/Log.h>

#include "Configuration.h"
#include "BufferProviders.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

namespace android {

// ----------------------------------------------------------------------------

template <typename T>
static inline T min(const T& a, const T& b)
{
    return a < b ? a : b;
}

CopyBufferProvider::CopyBufferProvider(size_t inputFrameSize,
        size_t outputFrameSize, size_t bufferFrameCount) :
        mInputFrameSize(inputFrameSize),
        mOutputFrameSize(outputFrameSize),
        mLocalBufferFrameCount(bufferFrameCount),
        mLocalBufferData(NULL),
        mConsumed(0)
{
    ALOGV("CopyBufferProvider(%p)(%zu, %zu, %zu)", this,
            inputFrameSize, outputFrameSize, bufferFrameCount);
    LOG_ALWAYS_FATAL_IF(inputFrameSize < outputFrameSize && bufferFrameCount == 0,
            "Requires local buffer if inputFrameSize(%zu) < outputFrameSize(%zu)",
            inputFrameSize, outputFrameSize);
    if (mLocalBufferFrameCount) {
        (void)posix_memalign(&mLocalBufferData, 32, mLocalBufferFrameCount * mOutputFrameSize);
    }
    mBuffer.frameCount = 0;
}

CopyBufferProvider::~CopyBufferProvider()
{
    ALOGV("~CopyBufferProvider(%p)", this);
    if (mBuffer.frameCount != 0) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
    }
    free(mLocalBufferData);
}

status_t CopyBufferProvider::getNextBuffer(AudioBufferProvider::Buffer *pBuffer,
        int64_t pts)
{
    //ALOGV("CopyBufferProvider(%p)::getNextBuffer(%p (%zu), %lld)",
    //        this, pBuffer, pBuffer->frameCount, pts);
    if (mLocalBufferFrameCount == 0) {
        status_t res = mTrackBufferProvider->getNextBuffer(pBuffer, pts);
        if (res == OK) {
            copyFrames(pBuffer->raw, pBuffer->raw, pBuffer->frameCount);
        }
        return res;
    }
    if (mBuffer.frameCount == 0) {
        mBuffer.frameCount = pBuffer->frameCount;
        status_t res = mTrackBufferProvider->getNextBuffer(&mBuffer, pts);
        // At one time an upstream buffer provider had
        // res == OK and mBuffer.frameCount == 0, doesn't seem to happen now 7/18/2014.
        //
        // By API spec, if res != OK, then mBuffer.frameCount == 0.
        // but there may be improper implementations.
        ALOG_ASSERT(res == OK || mBuffer.frameCount == 0);
        if (res != OK || mBuffer.frameCount == 0) { // not needed by API spec, but to be safe.
            pBuffer->raw = NULL;
            pBuffer->frameCount = 0;
            return res;
        }
        mConsumed = 0;
    }
    ALOG_ASSERT(mConsumed < mBuffer.frameCount);
    size_t count = min(mLocalBufferFrameCount, mBuffer.frameCount - mConsumed);
    count = min(count, pBuffer->frameCount);
    pBuffer->raw = mLocalBufferData;
    pBuffer->frameCount = count;
    copyFrames(pBuffer->raw, (uint8_t*)mBuffer.raw + mConsumed * mInputFrameSize,
            pBuffer->frameCount);
    return OK;
}

void CopyBufferProvider::releaseBuffer(AudioBufferProvider::Buffer *pBuffer)
{
    //ALOGV("CopyBufferProvider(%p)::releaseBuffer(%p(%zu))",
    //        this, pBuffer, pBuffer->frameCount);
    if (mLocalBufferFrameCount == 0) {
        mTrackBufferProvider->releaseBuffer(pBuffer);
        return;
    }
    // LOG_ALWAYS_FATAL_IF(pBuffer->frameCount == 0, "Invalid framecount");
    mConsumed += pBuffer->frameCount; // TODO: update for efficiency to reuse existing content
    if (mConsumed != 0 && mConsumed >= mBuffer.frameCount) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
        ALOG_ASSERT(mBuffer.frameCount == 0);
    }
    pBuffer->raw = NULL;
    pBuffer->frameCount = 0;
}

void CopyBufferProvider::reset()
{
    if (mBuffer.frameCount != 0) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
    }
    mConsumed = 0;
}

DownmixerBufferProvider::DownmixerBufferProvider(
        audio_channel_mask_t inputChannelMask,
        audio_channel_mask_t outputChannelMask, audio_format_t format,
        uint32_t sampleRate, int32_t sessionId, size_t bufferFrameCount) :
        CopyBufferProvider(
            audio_bytes_per_sample(format) * audio_channel_count_from_out_mask(inputChannelMask),
            audio_bytes_per_sample(format) * audio_channel_count_from_out_mask(outputChannelMask),
            bufferFrameCount)  // set bufferFrameCount to 0 to do in-place
{
    ALOGV("DownmixerBufferProvider(%p)(%#x, %#x, %#x %u %d)",
            this, inputChannelMask, outputChannelMask, format,
            sampleRate, sessionId);
    if (!sIsMultichannelCapable
            || EffectCreate(&sDwnmFxDesc.uuid,
                    sessionId,
                    SESSION_ID_INVALID_AND_IGNORED,
                    &mDownmixHandle) != 0) {
         ALOGE("DownmixerBufferProvider() error creating downmixer effect");
         mDownmixHandle = NULL;
         return;
     }
     // channel input configuration will be overridden per-track
     mDownmixConfig.inputCfg.channels = inputChannelMask;   // FIXME: Should be bits
     mDownmixConfig.outputCfg.channels = outputChannelMask; // FIXME: should be bits
     mDownmixConfig.inputCfg.format = format;
     mDownmixConfig.outputCfg.format = format;
     mDownmixConfig.inputCfg.samplingRate = sampleRate;
     mDownmixConfig.outputCfg.samplingRate = sampleRate;
     mDownmixConfig.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
     mDownmixConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
     // input and output buffer provider, and frame count will not be used as the downmix effect
     // process() function is called directly (see DownmixerBufferProvider::getNextBuffer())
     mDownmixConfig.inputCfg.mask = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS |
             EFFECT_CONFIG_FORMAT | EFFECT_CONFIG_ACC_MODE;
     mDownmixConfig.outputCfg.mask = mDownmixConfig.inputCfg.mask;

     int cmdStatus;
     uint32_t replySize = sizeof(int);

     // Configure downmixer
     status_t status = (*mDownmixHandle)->command(mDownmixHandle,
             EFFECT_CMD_SET_CONFIG /*cmdCode*/, sizeof(effect_config_t) /*cmdSize*/,
             &mDownmixConfig /*pCmdData*/,
             &replySize, &cmdStatus /*pReplyData*/);
     if (status != 0 || cmdStatus != 0) {
         ALOGE("DownmixerBufferProvider() error %d cmdStatus %d while configuring downmixer",
                 status, cmdStatus);
         EffectRelease(mDownmixHandle);
         mDownmixHandle = NULL;
         return;
     }

     // Enable downmixer
     replySize = sizeof(int);
     status = (*mDownmixHandle)->command(mDownmixHandle,
             EFFECT_CMD_ENABLE /*cmdCode*/, 0 /*cmdSize*/, NULL /*pCmdData*/,
             &replySize, &cmdStatus /*pReplyData*/);
     if (status != 0 || cmdStatus != 0) {
         ALOGE("DownmixerBufferProvider() error %d cmdStatus %d while enabling downmixer",
                 status, cmdStatus);
         EffectRelease(mDownmixHandle);
         mDownmixHandle = NULL;
         return;
     }

     // Set downmix type
     // parameter size rounded for padding on 32bit boundary
     const int psizePadded = ((sizeof(downmix_params_t) - 1)/sizeof(int) + 1) * sizeof(int);
     const int downmixParamSize =
             sizeof(effect_param_t) + psizePadded + sizeof(downmix_type_t);
     effect_param_t * const param = (effect_param_t *) malloc(downmixParamSize);
     param->psize = sizeof(downmix_params_t);
     const downmix_params_t downmixParam = DOWNMIX_PARAM_TYPE;
     memcpy(param->data, &downmixParam, param->psize);
     const downmix_type_t downmixType = DOWNMIX_TYPE_FOLD;
     param->vsize = sizeof(downmix_type_t);
     memcpy(param->data + psizePadded, &downmixType, param->vsize);
     replySize = sizeof(int);
     status = (*mDownmixHandle)->command(mDownmixHandle,
             EFFECT_CMD_SET_PARAM /* cmdCode */, downmixParamSize /* cmdSize */,
             param /*pCmdData*/, &replySize, &cmdStatus /*pReplyData*/);
     free(param);
     if (status != 0 || cmdStatus != 0) {
         ALOGE("DownmixerBufferProvider() error %d cmdStatus %d while setting downmix type",
                 status, cmdStatus);
         EffectRelease(mDownmixHandle);
         mDownmixHandle = NULL;
         return;
     }
     ALOGV("DownmixerBufferProvider() downmix type set to %d", (int) downmixType);
}

DownmixerBufferProvider::~DownmixerBufferProvider()
{
    ALOGV("~DownmixerBufferProvider (%p)", this);
    EffectRelease(mDownmixHandle);
    mDownmixHandle = NULL;
}

void DownmixerBufferProvider::copyFrames(void *dst, const void *src, size_t frames)
{
    mDownmixConfig.inputCfg.buffer.frameCount = frames;
    mDownmixConfig.inputCfg.buffer.raw = const_cast<void *>(src);
    mDownmixConfig.outputCfg.buffer.frameCount = frames;
    mDownmixConfig.outputCfg.buffer.raw = dst;
    // may be in-place if src == dst.
    status_t res = (*mDownmixHandle)->process(mDownmixHandle,
            &mDownmixConfig.inputCfg.buffer, &mDownmixConfig.outputCfg.buffer);
    ALOGE_IF(res != OK, "DownmixBufferProvider error %d", res);
}

/* call once in a pthread_once handler. */
/*static*/ status_t DownmixerBufferProvider::init()
{
    // find multichannel downmix effect if we have to play multichannel content
    uint32_t numEffects = 0;
    int ret = EffectQueryNumberEffects(&numEffects);
    if (ret != 0) {
        ALOGE("AudioMixer() error %d querying number of effects", ret);
        return NO_INIT;
    }
    ALOGV("EffectQueryNumberEffects() numEffects=%d", numEffects);

    for (uint32_t i = 0 ; i < numEffects ; i++) {
        if (EffectQueryEffect(i, &sDwnmFxDesc) == 0) {
            ALOGV("effect %d is called %s", i, sDwnmFxDesc.name);
            if (memcmp(&sDwnmFxDesc.type, EFFECT_UIID_DOWNMIX, sizeof(effect_uuid_t)) == 0) {
                ALOGI("found effect \"%s\" from %s",
                        sDwnmFxDesc.name, sDwnmFxDesc.implementor);
                sIsMultichannelCapable = true;
                break;
            }
        }
    }
    ALOGW_IF(!sIsMultichannelCapable, "unable to find downmix effect");
    return NO_INIT;
}

/*static*/ bool DownmixerBufferProvider::sIsMultichannelCapable = false;
/*static*/ effect_descriptor_t DownmixerBufferProvider::sDwnmFxDesc;

RemixBufferProvider::RemixBufferProvider(audio_channel_mask_t inputChannelMask,
        audio_channel_mask_t outputChannelMask, audio_format_t format,
        size_t bufferFrameCount) :
        CopyBufferProvider(
                audio_bytes_per_sample(format)
                    * audio_channel_count_from_out_mask(inputChannelMask),
                audio_bytes_per_sample(format)
                    * audio_channel_count_from_out_mask(outputChannelMask),
                bufferFrameCount),
        mFormat(format),
        mSampleSize(audio_bytes_per_sample(format)),
        mInputChannels(audio_channel_count_from_out_mask(inputChannelMask)),
        mOutputChannels(audio_channel_count_from_out_mask(outputChannelMask))
{
    ALOGV("RemixBufferProvider(%p)(%#x, %#x, %#x) %zu %zu",
            this, format, inputChannelMask, outputChannelMask,
            mInputChannels, mOutputChannels);

    const audio_channel_representation_t inputRepresentation =
            audio_channel_mask_get_representation(inputChannelMask);
    const audio_channel_representation_t outputRepresentation =
            audio_channel_mask_get_representation(outputChannelMask);
    const uint32_t inputBits = audio_channel_mask_get_bits(inputChannelMask);
    const uint32_t outputBits = audio_channel_mask_get_bits(outputChannelMask);

    switch (inputRepresentation) {
    case AUDIO_CHANNEL_REPRESENTATION_POSITION:
        switch (outputRepresentation) {
        case AUDIO_CHANNEL_REPRESENTATION_POSITION:
            memcpy_by_index_array_initialization(mIdxAry, ARRAY_SIZE(mIdxAry),
                    outputBits, inputBits);
            return;
        case AUDIO_CHANNEL_REPRESENTATION_INDEX:
            // TODO: output channel index mask not currently allowed
            // fall through
        default:
            break;
        }
        break;
    case AUDIO_CHANNEL_REPRESENTATION_INDEX:
        switch (outputRepresentation) {
        case AUDIO_CHANNEL_REPRESENTATION_POSITION:
            memcpy_by_index_array_initialization_src_index(mIdxAry, ARRAY_SIZE(mIdxAry),
                    outputBits, inputBits);
            return;
        case AUDIO_CHANNEL_REPRESENTATION_INDEX:
            // TODO: output channel index mask not currently allowed
            // fall through
        default:
            break;
        }
        break;
    default:
        break;
    }
    LOG_ALWAYS_FATAL("invalid channel mask conversion from %#x to %#x",
            inputChannelMask, outputChannelMask);
}

void RemixBufferProvider::copyFrames(void *dst, const void *src, size_t frames)
{
    memcpy_by_index_array(dst, mOutputChannels,
            src, mInputChannels, mIdxAry, mSampleSize, frames);
}

ReformatBufferProvider::ReformatBufferProvider(int32_t channelCount,
        audio_format_t inputFormat, audio_format_t outputFormat,
        size_t bufferFrameCount) :
        CopyBufferProvider(
                channelCount * audio_bytes_per_sample(inputFormat),
                channelCount * audio_bytes_per_sample(outputFormat),
                bufferFrameCount),
        mChannelCount(channelCount),
        mInputFormat(inputFormat),
        mOutputFormat(outputFormat)
{
    ALOGV("ReformatBufferProvider(%p)(%u, %#x, %#x)",
            this, channelCount, inputFormat, outputFormat);
}

void ReformatBufferProvider::copyFrames(void *dst, const void *src, size_t frames)
{
    memcpy_by_audio_format(dst, mOutputFormat, src, mInputFormat, frames * mChannelCount);
}

// ----------------------------------------------------------------------------
} // namespace android
