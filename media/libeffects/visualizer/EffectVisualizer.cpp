/*
 * Copyright (C) 2010 The Android Open Source Project
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

#define LOG_TAG "EffectVisualizer"
//#define LOG_NDEBUG 0
#include <log/log.h>
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <time.h>
#include <math.h>
#include <audio_effects/effect_visualizer.h>


extern "C" {

// effect_handle_t interface implementation for visualizer effect
extern const struct effect_interface_s gVisualizerInterface;

// Google Visualizer UUID: d069d9e0-8329-11df-9168-0002a5d5c51b
const effect_descriptor_t gVisualizerDescriptor = {
        {0xe46b26a0, 0xdddd, 0x11db, 0x8afd, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}, // type
        {0xd069d9e0, 0x8329, 0x11df, 0x9168, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}, // uuid
        EFFECT_CONTROL_API_VERSION,
        (EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_FIRST),
        0, // TODO
        1,
        "Visualizer",
        "The Android Open Source Project",
};

enum visualizer_state_e {
    VISUALIZER_STATE_UNINITIALIZED,
    VISUALIZER_STATE_INITIALIZED,
    VISUALIZER_STATE_ACTIVE,
};

// maximum time since last capture buffer update before resetting capture buffer. This means
// that the framework has stopped playing audio and we must start returning silence
#define MAX_STALL_TIME_MS 1000

#define CAPTURE_BUF_SIZE 65536 // "64k should be enough for everyone"

#define DISCARD_MEASUREMENTS_TIME_MS 2000 // discard measurements older than this number of ms

// maximum number of buffers for which we keep track of the measurements
#define MEASUREMENT_WINDOW_MAX_SIZE_IN_BUFFERS 25 // note: buffer index is stored in uint8_t


struct BufferStats {
    bool mIsValid;
    uint16_t mPeakU16; // the positive peak of the absolute value of the samples in a buffer
    float mRmsSquared; // the average square of the samples in a buffer
};

struct VisualizerContext {
    const struct effect_interface_s *mItfe;
    effect_config_t mConfig;
    uint32_t mCaptureIdx;
    uint32_t mCaptureSize;
    uint32_t mScalingMode;
    uint8_t mState;
    uint32_t mLastCaptureIdx;
    uint32_t mLatency;
    struct timespec mBufferUpdateTime;
    uint8_t mCaptureBuf[CAPTURE_BUF_SIZE];
    // for measurements
    uint8_t mChannelCount; // to avoid recomputing it every time a buffer is processed
    uint32_t mMeasurementMode;
    uint8_t mMeasurementWindowSizeInBuffers;
    uint8_t mMeasurementBufferIdx;
    BufferStats mPastMeasurements[MEASUREMENT_WINDOW_MAX_SIZE_IN_BUFFERS];
};

//
//--- Local functions
//
uint32_t Visualizer_getDeltaTimeMsFromUpdatedTime(VisualizerContext* pContext) {
    uint32_t deltaMs = 0;
    if (pContext->mBufferUpdateTime.tv_sec != 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            time_t secs = ts.tv_sec - pContext->mBufferUpdateTime.tv_sec;
            long nsec = ts.tv_nsec - pContext->mBufferUpdateTime.tv_nsec;
            if (nsec < 0) {
                --secs;
                nsec += 1000000000;
            }
            deltaMs = secs * 1000 + nsec / 1000000;
        }
    }
    return deltaMs;
}


void Visualizer_reset(VisualizerContext *pContext)
{
    pContext->mCaptureIdx = 0;
    pContext->mLastCaptureIdx = 0;
    pContext->mBufferUpdateTime.tv_sec = 0;
    pContext->mLatency = 0;
    memset(pContext->mCaptureBuf, 0x80, CAPTURE_BUF_SIZE);
}

//----------------------------------------------------------------------------
// Visualizer_setConfig()
//----------------------------------------------------------------------------
// Purpose: Set input and output audio configuration.
//
// Inputs:
//  pContext:   effect engine context
//  pConfig:    pointer to effect_config_t structure holding input and output
//      configuration parameters
//
// Outputs:
//
//----------------------------------------------------------------------------

int Visualizer_setConfig(VisualizerContext *pContext, effect_config_t *pConfig)
{
    ALOGV("Visualizer_setConfig start");

    if (pConfig->inputCfg.samplingRate != pConfig->outputCfg.samplingRate) return -EINVAL;
    if (pConfig->inputCfg.channels != pConfig->outputCfg.channels) return -EINVAL;
    if (pConfig->inputCfg.format != pConfig->outputCfg.format) return -EINVAL;
    if (pConfig->inputCfg.channels != AUDIO_CHANNEL_OUT_STEREO) return -EINVAL;
    if (pConfig->outputCfg.accessMode != EFFECT_BUFFER_ACCESS_WRITE &&
            pConfig->outputCfg.accessMode != EFFECT_BUFFER_ACCESS_ACCUMULATE) return -EINVAL;
    if (pConfig->inputCfg.format != AUDIO_FORMAT_PCM_16_BIT) return -EINVAL;

    pContext->mConfig = *pConfig;

    Visualizer_reset(pContext);

    return 0;
}


//----------------------------------------------------------------------------
// Visualizer_getConfig()
//----------------------------------------------------------------------------
// Purpose: Get input and output audio configuration.
//
// Inputs:
//  pContext:   effect engine context
//  pConfig:    pointer to effect_config_t structure holding input and output
//      configuration parameters
//
// Outputs:
//
//----------------------------------------------------------------------------

void Visualizer_getConfig(VisualizerContext *pContext, effect_config_t *pConfig)
{
    *pConfig = pContext->mConfig;
}


//----------------------------------------------------------------------------
// Visualizer_init()
//----------------------------------------------------------------------------
// Purpose: Initialize engine with default configuration.
//
// Inputs:
//  pContext:   effect engine context
//
// Outputs:
//
//----------------------------------------------------------------------------

int Visualizer_init(VisualizerContext *pContext)
{
    pContext->mConfig.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    pContext->mConfig.inputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    pContext->mConfig.inputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    pContext->mConfig.inputCfg.samplingRate = 44100;
    pContext->mConfig.inputCfg.bufferProvider.getBuffer = NULL;
    pContext->mConfig.inputCfg.bufferProvider.releaseBuffer = NULL;
    pContext->mConfig.inputCfg.bufferProvider.cookie = NULL;
    pContext->mConfig.inputCfg.mask = EFFECT_CONFIG_ALL;
    pContext->mConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_ACCUMULATE;
    pContext->mConfig.outputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    pContext->mConfig.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    pContext->mConfig.outputCfg.samplingRate = 44100;
    pContext->mConfig.outputCfg.bufferProvider.getBuffer = NULL;
    pContext->mConfig.outputCfg.bufferProvider.releaseBuffer = NULL;
    pContext->mConfig.outputCfg.bufferProvider.cookie = NULL;
    pContext->mConfig.outputCfg.mask = EFFECT_CONFIG_ALL;

    // visualization initialization
    pContext->mCaptureSize = VISUALIZER_CAPTURE_SIZE_MAX;
    pContext->mScalingMode = VISUALIZER_SCALING_MODE_NORMALIZED;

    // measurement initialization
    pContext->mChannelCount =
            audio_channel_count_from_out_mask(pContext->mConfig.inputCfg.channels);
    pContext->mMeasurementMode = MEASUREMENT_MODE_NONE;
    pContext->mMeasurementWindowSizeInBuffers = MEASUREMENT_WINDOW_MAX_SIZE_IN_BUFFERS;
    pContext->mMeasurementBufferIdx = 0;
    for (uint32_t i=0 ; i<pContext->mMeasurementWindowSizeInBuffers ; i++) {
        pContext->mPastMeasurements[i].mIsValid = false;
        pContext->mPastMeasurements[i].mPeakU16 = 0;
        pContext->mPastMeasurements[i].mRmsSquared = 0;
    }

    Visualizer_setConfig(pContext, &pContext->mConfig);

    return 0;
}

//
//--- Effect Library Interface Implementation
//

int VisualizerLib_Create(const effect_uuid_t *uuid,
                         int32_t /*sessionId*/,
                         int32_t /*ioId*/,
                         effect_handle_t *pHandle) {
    int ret;
    int i;

    if (pHandle == NULL || uuid == NULL) {
        return -EINVAL;
    }

    if (memcmp(uuid, &gVisualizerDescriptor.uuid, sizeof(effect_uuid_t)) != 0) {
        return -EINVAL;
    }

    VisualizerContext *pContext = new VisualizerContext;

    pContext->mItfe = &gVisualizerInterface;
    pContext->mState = VISUALIZER_STATE_UNINITIALIZED;

    ret = Visualizer_init(pContext);
    if (ret < 0) {
        ALOGW("VisualizerLib_Create() init failed");
        delete pContext;
        return ret;
    }

    *pHandle = (effect_handle_t)pContext;

    pContext->mState = VISUALIZER_STATE_INITIALIZED;

    ALOGV("VisualizerLib_Create %p", pContext);

    return 0;

}

int VisualizerLib_Release(effect_handle_t handle) {
    VisualizerContext * pContext = (VisualizerContext *)handle;

    ALOGV("VisualizerLib_Release %p", handle);
    if (pContext == NULL) {
        return -EINVAL;
    }
    pContext->mState = VISUALIZER_STATE_UNINITIALIZED;
    delete pContext;

    return 0;
}

int VisualizerLib_GetDescriptor(const effect_uuid_t *uuid,
                                effect_descriptor_t *pDescriptor) {

    if (pDescriptor == NULL || uuid == NULL){
        ALOGV("VisualizerLib_GetDescriptor() called with NULL pointer");
        return -EINVAL;
    }

    if (memcmp(uuid, &gVisualizerDescriptor.uuid, sizeof(effect_uuid_t)) == 0) {
        *pDescriptor = gVisualizerDescriptor;
        return 0;
    }

    return  -EINVAL;
} /* end VisualizerLib_GetDescriptor */

//
//--- Effect Control Interface Implementation
//

static inline int16_t clamp16(int32_t sample)
{
    if ((sample>>15) ^ (sample>>31))
        sample = 0x7FFF ^ (sample>>31);
    return sample;
}

int Visualizer_process(
        effect_handle_t self,audio_buffer_t *inBuffer, audio_buffer_t *outBuffer)
{
    VisualizerContext * pContext = (VisualizerContext *)self;

    if (pContext == NULL) {
        return -EINVAL;
    }

    if (inBuffer == NULL || inBuffer->raw == NULL ||
        outBuffer == NULL || outBuffer->raw == NULL ||
        inBuffer->frameCount != outBuffer->frameCount ||
        inBuffer->frameCount == 0) {
        return -EINVAL;
    }

    // perform measurements if needed
    if (pContext->mMeasurementMode & MEASUREMENT_MODE_PEAK_RMS) {
        // find the peak and RMS squared for the new buffer
        uint32_t inIdx;
        int16_t maxSample = 0;
        float rmsSqAcc = 0;
        for (inIdx = 0 ; inIdx < inBuffer->frameCount * pContext->mChannelCount ; inIdx++) {
            if (inBuffer->s16[inIdx] > maxSample) {
                maxSample = inBuffer->s16[inIdx];
            } else if (-inBuffer->s16[inIdx] > maxSample) {
                maxSample = -inBuffer->s16[inIdx];
            }
            rmsSqAcc += (inBuffer->s16[inIdx] * inBuffer->s16[inIdx]);
        }
        // store the measurement
        pContext->mPastMeasurements[pContext->mMeasurementBufferIdx].mPeakU16 = (uint16_t)maxSample;
        pContext->mPastMeasurements[pContext->mMeasurementBufferIdx].mRmsSquared =
                rmsSqAcc / (inBuffer->frameCount * pContext->mChannelCount);
        pContext->mPastMeasurements[pContext->mMeasurementBufferIdx].mIsValid = true;
        if (++pContext->mMeasurementBufferIdx >= pContext->mMeasurementWindowSizeInBuffers) {
            pContext->mMeasurementBufferIdx = 0;
        }
    }

    // all code below assumes stereo 16 bit PCM output and input
    int32_t shift;

    if (pContext->mScalingMode == VISUALIZER_SCALING_MODE_NORMALIZED) {
        // derive capture scaling factor from peak value in current buffer
        // this gives more interesting captures for display.
        shift = 32;
        int len = inBuffer->frameCount * 2;
        for (int i = 0; i < len; i++) {
            int32_t smp = inBuffer->s16[i];
            if (smp < 0) smp = -smp - 1; // take care to keep the max negative in range
            int32_t clz = __builtin_clz(smp);
            if (shift > clz) shift = clz;
        }
        // A maximum amplitude signal will have 17 leading zeros, which we want to
        // translate to a shift of 8 (for converting 16 bit to 8 bit)
        shift = 25 - shift;
        // Never scale by less than 8 to avoid returning unaltered PCM signal.
        if (shift < 3) {
            shift = 3;
        }
        // add one to combine the division by 2 needed after summing left and right channels below
        shift++;
    } else {
        assert(pContext->mScalingMode == VISUALIZER_SCALING_MODE_AS_PLAYED);
        shift = 9;
    }

    uint32_t captIdx;
    uint32_t inIdx;
    uint8_t *buf = pContext->mCaptureBuf;
    for (inIdx = 0, captIdx = pContext->mCaptureIdx;
         inIdx < inBuffer->frameCount;
         inIdx++, captIdx++) {
        if (captIdx >= CAPTURE_BUF_SIZE) {
            // wrap around
            captIdx = 0;
        }
        int32_t smp = inBuffer->s16[2 * inIdx] + inBuffer->s16[2 * inIdx + 1];
        smp = smp >> shift;
        buf[captIdx] = ((uint8_t)smp)^0x80;
    }

    // XXX the following two should really be atomic, though it probably doesn't
    // matter much for visualization purposes
    pContext->mCaptureIdx = captIdx;
    // update last buffer update time stamp
    if (clock_gettime(CLOCK_MONOTONIC, &pContext->mBufferUpdateTime) < 0) {
        pContext->mBufferUpdateTime.tv_sec = 0;
    }

    if (inBuffer->raw != outBuffer->raw) {
        if (pContext->mConfig.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE) {
            for (size_t i = 0; i < outBuffer->frameCount*2; i++) {
                outBuffer->s16[i] = clamp16(outBuffer->s16[i] + inBuffer->s16[i]);
            }
        } else {
            memcpy(outBuffer->raw, inBuffer->raw, outBuffer->frameCount * 2 * sizeof(int16_t));
        }
    }
    if (pContext->mState != VISUALIZER_STATE_ACTIVE) {
        return -ENODATA;
    }
    return 0;
}   // end Visualizer_process

int Visualizer_command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
        void *pCmdData, uint32_t *replySize, void *pReplyData) {

    VisualizerContext * pContext = (VisualizerContext *)self;
    int retsize;

    if (pContext == NULL || pContext->mState == VISUALIZER_STATE_UNINITIALIZED) {
        return -EINVAL;
    }

//    ALOGV("Visualizer_command command %" PRIu32 " cmdSize %" PRIu32, cmdCode, cmdSize);

    switch (cmdCode) {
    case EFFECT_CMD_INIT:
        if (pReplyData == NULL || *replySize != sizeof(int)) {
            return -EINVAL;
        }
        *(int *) pReplyData = Visualizer_init(pContext);
        break;
    case EFFECT_CMD_SET_CONFIG:
        if (pCmdData == NULL || cmdSize != sizeof(effect_config_t)
                || pReplyData == NULL || *replySize != sizeof(int)) {
            return -EINVAL;
        }
        *(int *) pReplyData = Visualizer_setConfig(pContext,
                (effect_config_t *) pCmdData);
        break;
    case EFFECT_CMD_GET_CONFIG:
        if (pReplyData == NULL ||
            *replySize != sizeof(effect_config_t)) {
            return -EINVAL;
        }
        Visualizer_getConfig(pContext, (effect_config_t *)pReplyData);
        break;
    case EFFECT_CMD_RESET:
        Visualizer_reset(pContext);
        break;
    case EFFECT_CMD_ENABLE:
        if (pReplyData == NULL || *replySize != sizeof(int)) {
            return -EINVAL;
        }
        if (pContext->mState != VISUALIZER_STATE_INITIALIZED) {
            return -ENOSYS;
        }
        pContext->mState = VISUALIZER_STATE_ACTIVE;
        ALOGV("EFFECT_CMD_ENABLE() OK");
        *(int *)pReplyData = 0;
        break;
    case EFFECT_CMD_DISABLE:
        if (pReplyData == NULL || *replySize != sizeof(int)) {
            return -EINVAL;
        }
        if (pContext->mState != VISUALIZER_STATE_ACTIVE) {
            return -ENOSYS;
        }
        pContext->mState = VISUALIZER_STATE_INITIALIZED;
        ALOGV("EFFECT_CMD_DISABLE() OK");
        *(int *)pReplyData = 0;
        break;
    case EFFECT_CMD_GET_PARAM: {
        if (pCmdData == NULL ||
            cmdSize != (int)(sizeof(effect_param_t) + sizeof(uint32_t)) ||
            pReplyData == NULL ||
            *replySize < (int)(sizeof(effect_param_t) + sizeof(uint32_t) + sizeof(uint32_t))) {
            return -EINVAL;
        }
        memcpy(pReplyData, pCmdData, sizeof(effect_param_t) + sizeof(uint32_t));
        effect_param_t *p = (effect_param_t *)pReplyData;
        p->status = 0;
        *replySize = sizeof(effect_param_t) + sizeof(uint32_t);
        if (p->psize != sizeof(uint32_t)) {
            p->status = -EINVAL;
            break;
        }
        switch (*(uint32_t *)p->data) {
        case VISUALIZER_PARAM_CAPTURE_SIZE:
            ALOGV("get mCaptureSize = %" PRIu32, pContext->mCaptureSize);
            *((uint32_t *)p->data + 1) = pContext->mCaptureSize;
            p->vsize = sizeof(uint32_t);
            *replySize += sizeof(uint32_t);
            break;
        case VISUALIZER_PARAM_SCALING_MODE:
            ALOGV("get mScalingMode = %" PRIu32, pContext->mScalingMode);
            *((uint32_t *)p->data + 1) = pContext->mScalingMode;
            p->vsize = sizeof(uint32_t);
            *replySize += sizeof(uint32_t);
            break;
        case VISUALIZER_PARAM_MEASUREMENT_MODE:
            ALOGV("get mMeasurementMode = %" PRIu32, pContext->mMeasurementMode);
            *((uint32_t *)p->data + 1) = pContext->mMeasurementMode;
            p->vsize = sizeof(uint32_t);
            *replySize += sizeof(uint32_t);
            break;
        default:
            p->status = -EINVAL;
        }
        } break;
    case EFFECT_CMD_SET_PARAM: {
        if (pCmdData == NULL ||
            cmdSize != (int)(sizeof(effect_param_t) + sizeof(uint32_t) + sizeof(uint32_t)) ||
            pReplyData == NULL || *replySize != sizeof(int32_t)) {
            return -EINVAL;
        }
        *(int32_t *)pReplyData = 0;
        effect_param_t *p = (effect_param_t *)pCmdData;
        if (p->psize != sizeof(uint32_t) || p->vsize != sizeof(uint32_t)) {
            *(int32_t *)pReplyData = -EINVAL;
            break;
        }
        switch (*(uint32_t *)p->data) {
        case VISUALIZER_PARAM_CAPTURE_SIZE:
            pContext->mCaptureSize = *((uint32_t *)p->data + 1);
            ALOGV("set mCaptureSize = %" PRIu32, pContext->mCaptureSize);
            break;
        case VISUALIZER_PARAM_SCALING_MODE:
            pContext->mScalingMode = *((uint32_t *)p->data + 1);
            ALOGV("set mScalingMode = %" PRIu32, pContext->mScalingMode);
            break;
        case VISUALIZER_PARAM_LATENCY:
            pContext->mLatency = *((uint32_t *)p->data + 1);
            ALOGV("set mLatency = %" PRIu32, pContext->mLatency);
            break;
        case VISUALIZER_PARAM_MEASUREMENT_MODE:
            pContext->mMeasurementMode = *((uint32_t *)p->data + 1);
            ALOGV("set mMeasurementMode = %" PRIu32, pContext->mMeasurementMode);
            break;
        default:
            *(int32_t *)pReplyData = -EINVAL;
        }
        } break;
    case EFFECT_CMD_SET_DEVICE:
    case EFFECT_CMD_SET_VOLUME:
    case EFFECT_CMD_SET_AUDIO_MODE:
        break;


    case VISUALIZER_CMD_CAPTURE: {
        uint32_t captureSize = pContext->mCaptureSize;
        if (pReplyData == NULL || *replySize != captureSize) {
            ALOGV("VISUALIZER_CMD_CAPTURE() error *replySize %" PRIu32 " captureSize %" PRIu32,
                    *replySize, captureSize);
            return -EINVAL;
        }
        if (pContext->mState == VISUALIZER_STATE_ACTIVE) {
            const uint32_t deltaMs = Visualizer_getDeltaTimeMsFromUpdatedTime(pContext);

            // if audio framework has stopped playing audio although the effect is still
            // active we must clear the capture buffer to return silence
            if ((pContext->mLastCaptureIdx == pContext->mCaptureIdx) &&
                    (pContext->mBufferUpdateTime.tv_sec != 0) &&
                    (deltaMs > MAX_STALL_TIME_MS)) {
                    ALOGV("capture going to idle");
                    pContext->mBufferUpdateTime.tv_sec = 0;
                    memset(pReplyData, 0x80, captureSize);
            } else {
                int32_t latencyMs = pContext->mLatency;
                latencyMs -= deltaMs;
                if (latencyMs < 0) {
                    latencyMs = 0;
                }
                const uint32_t deltaSmpl =
                    pContext->mConfig.inputCfg.samplingRate * latencyMs / 1000;
                int32_t capturePoint = pContext->mCaptureIdx - captureSize - deltaSmpl;

                if (capturePoint < 0) {
                    uint32_t size = -capturePoint;
                    if (size > captureSize) {
                        size = captureSize;
                    }
                    memcpy(pReplyData,
                           pContext->mCaptureBuf + CAPTURE_BUF_SIZE + capturePoint,
                           size);
                    pReplyData = (char *)pReplyData + size;
                    captureSize -= size;
                    capturePoint = 0;
                }
                memcpy(pReplyData,
                       pContext->mCaptureBuf + capturePoint,
                       captureSize);
            }

            pContext->mLastCaptureIdx = pContext->mCaptureIdx;
        } else {
            memset(pReplyData, 0x80, captureSize);
        }

        } break;

    case VISUALIZER_CMD_MEASURE: {
        uint16_t peakU16 = 0;
        float sumRmsSquared = 0.0f;
        uint8_t nbValidMeasurements = 0;
        // reset measurements if last measurement was too long ago (which implies stored
        // measurements aren't relevant anymore and shouldn't bias the new one)
        const int32_t delayMs = Visualizer_getDeltaTimeMsFromUpdatedTime(pContext);
        if (delayMs > DISCARD_MEASUREMENTS_TIME_MS) {
            ALOGV("Discarding measurements, last measurement is %" PRId32 "ms old", delayMs);
            for (uint32_t i=0 ; i<pContext->mMeasurementWindowSizeInBuffers ; i++) {
                pContext->mPastMeasurements[i].mIsValid = false;
                pContext->mPastMeasurements[i].mPeakU16 = 0;
                pContext->mPastMeasurements[i].mRmsSquared = 0;
            }
            pContext->mMeasurementBufferIdx = 0;
        } else {
            // only use actual measurements, otherwise the first RMS measure happening before
            // MEASUREMENT_WINDOW_MAX_SIZE_IN_BUFFERS have been played will always be artificially
            // low
            for (uint32_t i=0 ; i < pContext->mMeasurementWindowSizeInBuffers ; i++) {
                if (pContext->mPastMeasurements[i].mIsValid) {
                    if (pContext->mPastMeasurements[i].mPeakU16 > peakU16) {
                        peakU16 = pContext->mPastMeasurements[i].mPeakU16;
                    }
                    sumRmsSquared += pContext->mPastMeasurements[i].mRmsSquared;
                    nbValidMeasurements++;
                }
            }
        }
        float rms = nbValidMeasurements == 0 ? 0.0f : sqrtf(sumRmsSquared / nbValidMeasurements);
        int32_t* pIntReplyData = (int32_t*)pReplyData;
        // convert from I16 sample values to mB and write results
        if (rms < 0.000016f) {
            pIntReplyData[MEASUREMENT_IDX_RMS] = -9600; //-96dB
        } else {
            pIntReplyData[MEASUREMENT_IDX_RMS] = (int32_t) (2000 * log10(rms / 32767.0f));
        }
        if (peakU16 == 0) {
            pIntReplyData[MEASUREMENT_IDX_PEAK] = -9600; //-96dB
        } else {
            pIntReplyData[MEASUREMENT_IDX_PEAK] = (int32_t) (2000 * log10(peakU16 / 32767.0f));
        }
        ALOGV("VISUALIZER_CMD_MEASURE peak=%" PRIu16 " (%" PRId32 "mB), rms=%.1f (%" PRId32 "mB)",
                peakU16, pIntReplyData[MEASUREMENT_IDX_PEAK],
                rms, pIntReplyData[MEASUREMENT_IDX_RMS]);
        }
        break;

    default:
        ALOGW("Visualizer_command invalid command %" PRIu32, cmdCode);
        return -EINVAL;
    }

    return 0;
}

/* Effect Control Interface Implementation: get_descriptor */
int Visualizer_getDescriptor(effect_handle_t   self,
                                    effect_descriptor_t *pDescriptor)
{
    VisualizerContext * pContext = (VisualizerContext *) self;

    if (pContext == NULL || pDescriptor == NULL) {
        ALOGV("Visualizer_getDescriptor() invalid param");
        return -EINVAL;
    }

    *pDescriptor = gVisualizerDescriptor;

    return 0;
}   /* end Visualizer_getDescriptor */

// effect_handle_t interface implementation for visualizer effect
const struct effect_interface_s gVisualizerInterface = {
        Visualizer_process,
        Visualizer_command,
        Visualizer_getDescriptor,
        NULL,
};

// This is the only symbol that needs to be exported
__attribute__ ((visibility ("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    .tag = AUDIO_EFFECT_LIBRARY_TAG,
    .version = EFFECT_LIBRARY_API_VERSION,
    .name = "Visualizer Library",
    .implementor = "The Android Open Source Project",
    .create_effect = VisualizerLib_Create,
    .release_effect = VisualizerLib_Release,
    .get_descriptor = VisualizerLib_GetDescriptor,
};

}; // extern "C"
