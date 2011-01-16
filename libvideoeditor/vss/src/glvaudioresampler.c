/*
 * Copyright (C) 2004-2011 NXP Software
 * Copyright (C) 2011 The Android Open Source Project
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
/**
 ******************************************************************************
 * @file    glvaudioresampler.c
 * @brief
 * @note
 ******************************************************************************
 */
/**
 * OSAL headers */
#include "M4OSA_Memory.h"        /**< OSAL memory management */
#include "M4OSA_Debug.h"        /**< OSAL debug management */
#include "M4OSA_CoreID.h"
#include "gLVAudioResampler.h"


static void resampleStereo16(int32_t* out, int16_t* input, long outFrameCount,
                                LVAudioResampler *resampler) ;
static void resampleMono16(int32_t* out, int16_t* input, long outFrameCount,
                             LVAudioResampler *resampler) ;

int32_t LVAudioResamplerCreate(int bitDepth, int inChannelCount,
        int32_t sampleRate, int quality)
{
    int32_t context;
    LVAudioResampler *resampler;

    resampler = (LVAudioResampler *)M4OSA_malloc(sizeof(LVAudioResampler), M4VSS3GPP,
         (M4OSA_Char *)"LVAudioResampler");
    context = (int32_t)resampler;

    if (quality == DEFAULT)
        quality = LOW_QUALITY;


    switch (quality) {
    default:
    case LOW_QUALITY:
        resampler->mQuality = LOW_QUALITY;
        LVResampler_LowQualityInit(bitDepth, inChannelCount, sampleRate, context);
        break;
    case MED_QUALITY:
        resampler->mQuality = MED_QUALITY;
        break;
    case HIGH_QUALITY:
        resampler->mQuality = HIGH_QUALITY;
        break;
    }

    return (context);
}

static int32_t Interp(int32_t x0, int32_t x1, uint32_t f) {
    int32_t t_datta;
    t_datta = x0 + (((x1 - x0) * (int32_t)(f >> kPreInterpShift)) >> kNumInterpBits);
    return t_datta;
}
static void Advance(long* index, uint32_t* frac, uint32_t inc) {
    *frac += inc;
    *index += (long)(*frac >> kNumPhaseBits);
    *frac &= kPhaseMask;
}

void LVResampler_LowQualityInit(int bitDepth, int inChannelCount,
        int32_t sampleRate, int32_t context )
{
    LVAudioResampler *resampler = (LVAudioResampler *) context;
    resampler->mBitDepth = bitDepth;
    resampler->mChannelCount = inChannelCount;
    resampler->mSampleRate = sampleRate;
    resampler->mInSampleRate = sampleRate;
    resampler->mInputIndex = 0;
    resampler->mPhaseFraction = 0;
    // sanity check on format
    if ((bitDepth != 16) ||(inChannelCount < 1) || (inChannelCount > 2))
    {
        //LOGE("Unsupported sample format, %d bits, %d channels", bitDepth,
        //  inChannelCount);
        // LOG_ASSERT(0);
    }
    // initialize common members
    resampler->mVolume[0] =
        resampler->mVolume[1] = 0;
    resampler->mBuffer.frameCount = 0;
    // save format for quick lookup
    if (inChannelCount == 1)
    {
        resampler->mFormat = 1;//MONO_16_BIT;
    }
    else
    {
        resampler->mFormat = 2;//STEREO_16_BIT;
    }
}

void LVAudiosetSampleRate(int32_t context,int32_t inSampleRate)
{
    LVAudioResampler *resampler = (LVAudioResampler *)context;
    long temp;
    temp = kPhaseMultiplier;

    resampler->mInSampleRate = inSampleRate;
    resampler->mPhaseIncrement = (uint32_t)((temp / resampler->mSampleRate)* inSampleRate );
}
void LVAudiosetVolume(int32_t context, int16_t left, int16_t right)
{
    LVAudioResampler *resampler = (LVAudioResampler *)context;
    // TODO: Implement anti-zipper filter
    resampler->mVolume[0] = left;
    resampler->mVolume[1] = right;
}



static  int16_t clamp16(int32_t sample)
{
    if ((sample>>15) ^ (sample>>31))
        sample = 0x7FFF ^ (sample>>31);
    return sample;
}


static void DitherAndClamp(int32_t* out, int32_t const *sums, long c)
{
    long i;
        //ditherAndClamp((int32_t*)reSampledBuffer, pTmpBuffer, outBufferSize/2);
    for ( i=0 ; i<c ; i++)
    {
        int32_t l = *sums++;
        int32_t r = *sums++;
        int32_t nl = l >> 12;
        int32_t nr = r >> 12;
        l = clamp16(nl);
        r = clamp16(nr);
        *out++ = (r<<16) | (l & 0xFFFF);
    }

}

void LVAudioresample_LowQuality(int16_t* out,
                                int16_t* input,
                                long outFrameCount,
                                int32_t context)
{
    LVAudioResampler *resampler = (LVAudioResampler *)context;

    int32_t     *tempBuff = (int32_t *)M4OSA_malloc(
                (outFrameCount * sizeof(int32_t) * 2),
                M4VSS3GPP, (M4OSA_Char *)"tempBuff");

    M4OSA_memset((M4OSA_MemAddr8)tempBuff,
                (outFrameCount * sizeof(int32_t) * 2), 0);

    switch (resampler->mChannelCount)
    {
        case 1:
             resampleMono16(tempBuff, input, outFrameCount, resampler);
            break;
        case 2:
            resampleStereo16(tempBuff, input, outFrameCount, resampler);
            break;
    }

    // Dither and Clamp
    DitherAndClamp((int32_t*)out, tempBuff, outFrameCount);

    M4OSA_free((M4OSA_MemAddr32)tempBuff);
}

void resampleStereo16(int32_t* out, int16_t* input,long outFrameCount,
                        LVAudioResampler *resampler)
{

    int32_t vl = resampler->mVolume[0];
    int32_t vr = resampler->mVolume[1];

    long inputIndex = resampler->mInputIndex;
    uint32_t phaseFraction = resampler->mPhaseFraction;
    uint32_t phaseIncrement = resampler->mPhaseIncrement;
    long outputIndex = 0;


    long outputSampleCount = outFrameCount * 2;
    long inFrameCount = (outFrameCount* resampler->mInSampleRate)/resampler->mSampleRate;
    int16_t *in;

    resampler->mBuffer.i16 = input;

    // LOGE("starting resample %d frames, inputIndex=%d, phaseFraction=%d, phaseIncrement=%d\n",
    //      outFrameCount, inputIndex, phaseFraction, phaseIncrement);

    while (outputIndex < outputSampleCount)
    {
        resampler->mBuffer.frameCount = inFrameCount;
        resampler->mX0L = 0;
        resampler->mX0R = 0;
        inputIndex = 0;

        in = resampler->mBuffer.i16;

        // handle boundary case
        while (inputIndex == 0) {
            // LOGE("boundary case\n");
            out[outputIndex++] += vl * Interp(resampler->mX0L, in[0], phaseFraction);
            out[outputIndex++] += vr * Interp(resampler->mX0R, in[1], phaseFraction);
            Advance(&inputIndex, &phaseFraction, phaseIncrement);
            if (outputIndex == outputSampleCount)
                break;
        }

        // process input samples
        while (outputIndex < outputSampleCount && inputIndex < resampler->mBuffer.frameCount) {
            out[outputIndex++] += vl * Interp(in[inputIndex*2-2],
                    in[inputIndex*2], phaseFraction);
            out[outputIndex++] += vr * Interp(in[inputIndex*2-1],
                    in[inputIndex*2+1], phaseFraction);
            Advance(&inputIndex, &phaseFraction, phaseIncrement);
        }

        resampler->mX0L = resampler->mBuffer.i16[resampler->mBuffer.frameCount*2-2];
        resampler->mX0R = resampler->mBuffer.i16[resampler->mBuffer.frameCount*2-1];
    }

resampleStereo16_exit:
    // save state
    resampler->mInputIndex = inputIndex;
    resampler->mPhaseFraction = phaseFraction;
}


void resampleMono16(int32_t* out, int16_t* input,long outFrameCount, LVAudioResampler *resampler/*,
        AudioBufferProvider* provider*/)
{

    int32_t vl = resampler->mVolume[0];
    int32_t vr = resampler->mVolume[1];
    int16_t *in;

    long inputIndex = resampler->mInputIndex;
    uint32_t phaseFraction = resampler->mPhaseFraction;
    uint32_t phaseIncrement = resampler->mPhaseIncrement;
    long outputIndex = 0;
    long outputSampleCount = outFrameCount * 2;
    long inFrameCount = (outFrameCount*resampler->mInSampleRate)/resampler->mSampleRate;

    resampler->mBuffer.i16 = input;
    resampler->mBuffer.i8 = (int8_t *)input;
    resampler->mBuffer.raw = (void *)input;

    // LOGE("starting resample %d frames, inputIndex=%d, phaseFraction=%d, phaseIncrement=%d\n",
    //      outFrameCount, inputIndex, phaseFraction, phaseIncrement);
    while (outputIndex < outputSampleCount) {
        // buffer is empty, fetch a new one
        while (resampler->mBuffer.frameCount == 0) {
            resampler->mBuffer.frameCount = inFrameCount;
            //provider->getNextBuffer(&mBuffer);

            if (resampler->mBuffer.raw == M4OSA_NULL) {
                resampler->mInputIndex = inputIndex;
                resampler->mPhaseFraction = phaseFraction;
                goto resampleMono16_exit;
            }
            resampler->mX0L = 0;
            // LOGE("New buffer fetched: %d frames\n", mBuffer.frameCount);
            if (resampler->mBuffer.frameCount >  inputIndex)
                break;

            inputIndex -= resampler->mBuffer.frameCount;
            resampler->mX0L = resampler->mBuffer.i16[resampler->mBuffer.frameCount-1];
            //provider->releaseBuffer(&resampler->mBuffer);
            // mBuffer.frameCount == 0 now so we reload a new buffer
        }

        in = resampler->mBuffer.i16;

        // handle boundary case
        while (inputIndex == 0) {
            // LOGE("boundary case\n");
            int32_t sample = Interp(resampler->mX0L, in[0], phaseFraction);
            out[outputIndex++] += vl * sample;
            out[outputIndex++] += vr * sample;
            Advance(&inputIndex, &phaseFraction, phaseIncrement);
            if (outputIndex == outputSampleCount)
                break;
        }

        // process input samples
        while (outputIndex < outputSampleCount && inputIndex < resampler->mBuffer.frameCount) {
            int32_t sample = Interp(in[inputIndex-1], in[inputIndex],
                    phaseFraction);
            out[outputIndex++] += vl * sample;
            out[outputIndex++] += vr * sample;
            Advance(&inputIndex, &phaseFraction, phaseIncrement);
        }

        // LOGE("loop done - outputIndex=%d, inputIndex=%d\n", outputIndex, inputIndex);
        // if done with buffer, save samples
        if (inputIndex >= resampler->mBuffer.frameCount) {
            inputIndex -= resampler->mBuffer.frameCount;

            // LOGE("buffer done, new input index %d", inputIndex);
            resampler->mX0L = resampler->mBuffer.i16[resampler->mBuffer.frameCount-1];
        }
    }

resampleMono16_exit:
    // save state
    resampler->mInputIndex = inputIndex;
    resampler->mPhaseFraction = phaseFraction;
}

