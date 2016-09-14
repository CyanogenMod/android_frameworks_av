/*
 * Copyright (C) 2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 * Copyright (C) 2007 The Android Open Source Project
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

#include "AudioResamplerQTI.h"
#include "QCT_Resampler.h"
#include <sys/time.h>
#include <audio_utils/primitives.h>

namespace android {
AudioResamplerQTI::AudioResamplerQTI(int format,
        int inChannelCount, int32_t sampleRate)
    :AudioResampler(inChannelCount, sampleRate, QTI_QUALITY),
     mTmpBuf(0), mResamplerOutBuf(0), mFrameIndex(0),mOutFrameCount(0)
{
    stateSize = QCT_Resampler::MemAlloc(format, inChannelCount, sampleRate, sampleRate);
    mState = new int16_t[stateSize];
    mVolume[0] = mVolume[1] = 0;
    mBuffer.frameCount = 0;
}

AudioResamplerQTI::~AudioResamplerQTI()
{
    if (mState) {
        delete [] mState;
    }
    if (mTmpBuf) {
        delete [] mTmpBuf;
    }
    if(mResamplerOutBuf) {
        delete [] mResamplerOutBuf;
    }
}

size_t AudioResamplerQTI::resample(int32_t* out, size_t outFrameCount,
            AudioBufferProvider* provider)
{
    int16_t vl = mVolume[0];
    int16_t vr = mVolume[1];
    int32_t *pBuf;

    int64_t tempL, tempR;
    size_t inFrameRequest;
    size_t inFrameCount = getNumInSample(outFrameCount);
    size_t index = 0;
    size_t frameIndex = mFrameIndex;
    size_t out_count = outFrameCount * 2;
    float *fout = reinterpret_cast<float *>(out);

    if (mChannelCount == 1) {
        inFrameRequest = inFrameCount;
    } else {
        inFrameRequest = inFrameCount * 2;
    }

    if (mOutFrameCount < outFrameCount) {
        mOutFrameCount = outFrameCount;
        if (mTmpBuf) {
            delete [] mTmpBuf;
        }
        if(mResamplerOutBuf) {
            delete [] mResamplerOutBuf;
        }
        mTmpBuf = new int32_t[inFrameRequest + 16];
        mResamplerOutBuf = new int32_t[out_count];
    }

    if (mChannelCount == 1) {
        // buffer is empty, fetch a new one
        while (index < inFrameCount) {
            if (!mBuffer.frameCount) {
                mBuffer.frameCount = inFrameCount;
                provider->getNextBuffer(&mBuffer);
                frameIndex = 0;
            }

            if (mBuffer.raw == NULL) {
                while (index < inFrameCount) {
                    mTmpBuf[index++] = 0;
                }
                QCT_Resampler::Resample90dB(mState, mTmpBuf, mResamplerOutBuf, inFrameCount, outFrameCount);
                goto resample_exit;
            }

            mTmpBuf[index++] = clampq4_27_from_float(*((float *)mBuffer.raw + frameIndex++));

            if (frameIndex >= mBuffer.frameCount) {
                provider->releaseBuffer(&mBuffer);
                mInputIndex = 0;
            }
        }

        QCT_Resampler::Resample90dB(mState, mTmpBuf, mResamplerOutBuf, inFrameCount, outFrameCount);
    } else {
        pBuf = &mTmpBuf[inFrameCount];
        // buffer is empty, fetch a new one
        while (index < inFrameCount) {
            if (!mBuffer.frameCount) {
                mBuffer.frameCount = inFrameCount;
                provider->getNextBuffer(&mBuffer);
                frameIndex = 0;
            }
            if (mBuffer.raw == NULL) {
                while (index < inFrameCount) {
                    mTmpBuf[index] = 0;
                    pBuf[index++] = 0;
                }
                QCT_Resampler::Resample90dB(mState, mTmpBuf, mResamplerOutBuf, inFrameCount, outFrameCount);
                goto resample_exit;
            }

            mTmpBuf[index] = clampq4_27_from_float(*((float *)mBuffer.raw + frameIndex++));
            pBuf[index++] = clampq4_27_from_float(*((float *)mBuffer.raw + frameIndex++));
            if (frameIndex >= mBuffer.frameCount * 2) {
                provider->releaseBuffer(&mBuffer);
                mInputIndex = 0;
            }
       }

       QCT_Resampler::Resample90dB(mState, mTmpBuf, mResamplerOutBuf, inFrameCount, outFrameCount);
    }

resample_exit:
    for (uint32_t i = 0; i < out_count; i += 2) {
        // Multiplying q4.27 data with u4.12 gain could result in 39 fractional bit data(27+12)
        // To get back the 27 fractional bit format output data, do right shift by 12
        tempL = (int64_t)mResamplerOutBuf[i] * vl;
        tempR = (int64_t)mResamplerOutBuf[i+1] * vr;
        fout[i] += float_from_q4_27((int32_t)(tempL>>12));
        fout[i+1] += float_from_q4_27((int32_t)(tempR>>12));
    }

    // save the unreleased frame count to mInputIndex
    if (mBuffer.frameCount) {
        mInputIndex = mChannelCount == 1 ? frameIndex : frameIndex/2;
    }
    mFrameIndex = frameIndex;
    return index;
}

void AudioResamplerQTI::setSampleRate(int32_t inSampleRate)
{
    if (mInSampleRate != inSampleRate) {
        mInSampleRate = inSampleRate;
        init();
    }
}

void AudioResamplerQTI::init()
{
    QCT_Resampler::Init(mState, mChannelCount, mInSampleRate, mSampleRate, 1/*32bit in*/);
}

size_t AudioResamplerQTI::getNumInSample(size_t outFrameCount)
{
    size_t size = (size_t)QCT_Resampler::GetNumInSamp(mState, outFrameCount);
    return size;
}

void AudioResamplerQTI::reset()
{
    AudioResampler::reset();
}

}; // namespace android
