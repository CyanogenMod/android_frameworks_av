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
#include <media/AudioResamplerPublic.h>
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

status_t CopyBufferProvider::getNextBuffer(AudioBufferProvider::Buffer *pBuffer)
{
    //ALOGV("CopyBufferProvider(%p)::getNextBuffer(%p (%zu))",
    //        this, pBuffer, pBuffer->frameCount);
    if (mLocalBufferFrameCount == 0) {
        status_t res = mTrackBufferProvider->getNextBuffer(pBuffer);
        if (res == OK) {
            copyFrames(pBuffer->raw, pBuffer->raw, pBuffer->frameCount);
        }
        return res;
    }
    if (mBuffer.frameCount == 0) {
        mBuffer.frameCount = pBuffer->frameCount;
        status_t res = mTrackBufferProvider->getNextBuffer(&mBuffer);
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
    (void) memcpy_by_index_array_initialization_from_channel_mask(
            mIdxAry, ARRAY_SIZE(mIdxAry), outputChannelMask, inputChannelMask);
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

TimestretchBufferProvider::TimestretchBufferProvider(int32_t channelCount,
        audio_format_t format, uint32_t sampleRate, const AudioPlaybackRate &playbackRate) :
        mChannelCount(channelCount),
        mFormat(format),
        mSampleRate(sampleRate),
        mFrameSize(channelCount * audio_bytes_per_sample(format)),
        mLocalBufferFrameCount(0),
        mLocalBufferData(NULL),
        mRemaining(0),
        mSonicStream(sonicCreateStream(sampleRate, mChannelCount)),
        mFallbackFailErrorShown(false),
        mAudioPlaybackRateValid(false)
{
    LOG_ALWAYS_FATAL_IF(mSonicStream == NULL,
            "TimestretchBufferProvider can't allocate Sonic stream");

    setPlaybackRate(playbackRate);
    ALOGV("TimestretchBufferProvider(%p)(%u, %#x, %u %f %f %d %d)",
            this, channelCount, format, sampleRate, playbackRate.mSpeed,
            playbackRate.mPitch, playbackRate.mStretchMode, playbackRate.mFallbackMode);
    mBuffer.frameCount = 0;
}

TimestretchBufferProvider::~TimestretchBufferProvider()
{
    ALOGV("~TimestretchBufferProvider(%p)", this);
    sonicDestroyStream(mSonicStream);
    if (mBuffer.frameCount != 0) {
        mTrackBufferProvider->releaseBuffer(&mBuffer);
    }
    free(mLocalBufferData);
}

status_t TimestretchBufferProvider::getNextBuffer(
        AudioBufferProvider::Buffer *pBuffer)
{
    ALOGV("TimestretchBufferProvider(%p)::getNextBuffer(%p (%zu))",
            this, pBuffer, pBuffer->frameCount);

    // BYPASS
    //return mTrackBufferProvider->getNextBuffer(pBuffer);

    // check if previously processed data is sufficient.
    if (pBuffer->frameCount <= mRemaining) {
        ALOGV("previous sufficient");
        pBuffer->raw = mLocalBufferData;
        return OK;
    }

    // do we need to resize our buffer?
    if (pBuffer->frameCount > mLocalBufferFrameCount) {
        void *newmem;
        if (posix_memalign(&newmem, 32, pBuffer->frameCount * mFrameSize) == OK) {
            if (mRemaining != 0) {
                memcpy(newmem, mLocalBufferData, mRemaining * mFrameSize);
            }
            free(mLocalBufferData);
            mLocalBufferData = newmem;
            mLocalBufferFrameCount = pBuffer->frameCount;
        }
    }

    // need to fetch more data
    const size_t outputDesired = pBuffer->frameCount - mRemaining;
    size_t dstAvailable;
    do {
        mBuffer.frameCount = mPlaybackRate.mSpeed == AUDIO_TIMESTRETCH_SPEED_NORMAL
                ? outputDesired : outputDesired * mPlaybackRate.mSpeed + 1;

        status_t res = mTrackBufferProvider->getNextBuffer(&mBuffer);

        ALOG_ASSERT(res == OK || mBuffer.frameCount == 0);
        if (res != OK || mBuffer.frameCount == 0) { // not needed by API spec, but to be safe.
            ALOGV("upstream provider cannot provide data");
            if (mRemaining == 0) {
                pBuffer->raw = NULL;
                pBuffer->frameCount = 0;
                return res;
            } else { // return partial count
                pBuffer->raw = mLocalBufferData;
                pBuffer->frameCount = mRemaining;
                return OK;
            }
        }

        // time-stretch the data
        dstAvailable = min(mLocalBufferFrameCount - mRemaining, outputDesired);
        size_t srcAvailable = mBuffer.frameCount;
        processFrames((uint8_t*)mLocalBufferData + mRemaining * mFrameSize, &dstAvailable,
                mBuffer.raw, &srcAvailable);

        // release all data consumed
        mBuffer.frameCount = srcAvailable;
        mTrackBufferProvider->releaseBuffer(&mBuffer);
    } while (dstAvailable == 0); // try until we get output data or upstream provider fails.

    // update buffer vars with the actual data processed and return with buffer
    mRemaining += dstAvailable;

    pBuffer->raw = mLocalBufferData;
    pBuffer->frameCount = mRemaining;

    return OK;
}

void TimestretchBufferProvider::releaseBuffer(AudioBufferProvider::Buffer *pBuffer)
{
    ALOGV("TimestretchBufferProvider(%p)::releaseBuffer(%p (%zu))",
       this, pBuffer, pBuffer->frameCount);

    // BYPASS
    //return mTrackBufferProvider->releaseBuffer(pBuffer);

    // LOG_ALWAYS_FATAL_IF(pBuffer->frameCount == 0, "Invalid framecount");
    if (pBuffer->frameCount < mRemaining) {
        memcpy(mLocalBufferData,
                (uint8_t*)mLocalBufferData + pBuffer->frameCount * mFrameSize,
                (mRemaining - pBuffer->frameCount) * mFrameSize);
        mRemaining -= pBuffer->frameCount;
    } else if (pBuffer->frameCount == mRemaining) {
        mRemaining = 0;
    } else {
        LOG_ALWAYS_FATAL("Releasing more frames(%zu) than available(%zu)",
                pBuffer->frameCount, mRemaining);
    }

    pBuffer->raw = NULL;
    pBuffer->frameCount = 0;
}

void TimestretchBufferProvider::reset()
{
    mRemaining = 0;
}

status_t TimestretchBufferProvider::setPlaybackRate(const AudioPlaybackRate &playbackRate)
{
    mPlaybackRate = playbackRate;
    mFallbackFailErrorShown = false;
    sonicSetSpeed(mSonicStream, mPlaybackRate.mSpeed);
    //TODO: pitch is ignored for now
    //TODO: optimize: if parameters are the same, don't do any extra computation.

    mAudioPlaybackRateValid = isAudioPlaybackRateValid(mPlaybackRate);
    return OK;
}

void TimestretchBufferProvider::processFrames(void *dstBuffer, size_t *dstFrames,
        const void *srcBuffer, size_t *srcFrames)
{
    ALOGV("processFrames(%zu %zu)  remaining(%zu)", *dstFrames, *srcFrames, mRemaining);
    // Note dstFrames is the required number of frames.

    if (!mAudioPlaybackRateValid) {
        //fallback mode
        // Ensure consumption from src is as expected.
        // TODO: add logic to track "very accurate" consumption related to speed, original sampling
        // rate, actual frames processed.

        const size_t targetSrc = *dstFrames * mPlaybackRate.mSpeed;
        if (*srcFrames < targetSrc) { // limit dst frames to that possible
            *dstFrames = *srcFrames / mPlaybackRate.mSpeed;
        } else if (*srcFrames > targetSrc + 1) {
            *srcFrames = targetSrc + 1;
        }
        if (*dstFrames > 0) {
            switch(mPlaybackRate.mFallbackMode) {
            case AUDIO_TIMESTRETCH_FALLBACK_CUT_REPEAT:
                if (*dstFrames <= *srcFrames) {
                      size_t copySize = mFrameSize * *dstFrames;
                      memcpy(dstBuffer, srcBuffer, copySize);
                  } else {
                      // cyclically repeat the source.
                      for (size_t count = 0; count < *dstFrames; count += *srcFrames) {
                          size_t remaining = min(*srcFrames, *dstFrames - count);
                          memcpy((uint8_t*)dstBuffer + mFrameSize * count,
                                  srcBuffer, mFrameSize * remaining);
                      }
                  }
                break;
            case AUDIO_TIMESTRETCH_FALLBACK_DEFAULT:
            case AUDIO_TIMESTRETCH_FALLBACK_MUTE:
                memset(dstBuffer,0, mFrameSize * *dstFrames);
                break;
            case AUDIO_TIMESTRETCH_FALLBACK_FAIL:
            default:
                if(!mFallbackFailErrorShown) {
                    ALOGE("invalid parameters in TimestretchBufferProvider fallbackMode:%d",
                            mPlaybackRate.mFallbackMode);
                    mFallbackFailErrorShown = true;
                }
                break;
            }
        }
    } else {
        switch (mFormat) {
        case AUDIO_FORMAT_PCM_FLOAT:
            if (sonicWriteFloatToStream(mSonicStream, (float*)srcBuffer, *srcFrames) != 1) {
                ALOGE("sonicWriteFloatToStream cannot realloc");
                *srcFrames = 0; // cannot consume all of srcBuffer
            }
            *dstFrames = sonicReadFloatFromStream(mSonicStream, (float*)dstBuffer, *dstFrames);
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            if (sonicWriteShortToStream(mSonicStream, (short*)srcBuffer, *srcFrames) != 1) {
                ALOGE("sonicWriteShortToStream cannot realloc");
                *srcFrames = 0; // cannot consume all of srcBuffer
            }
            *dstFrames = sonicReadShortFromStream(mSonicStream, (short*)dstBuffer, *dstFrames);
            break;
        default:
            // could also be caught on construction
            LOG_ALWAYS_FATAL("invalid format %#x for TimestretchBufferProvider", mFormat);
        }
    }
}
// ----------------------------------------------------------------------------
} // namespace android
