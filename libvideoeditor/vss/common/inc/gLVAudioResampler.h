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



#ifndef GLVAUDIORESAMPLER_H
#define GLVAUDIORESAMPLER_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef int8_t
#define  int8_t signed char
#endif

#ifndef int32_t
#define int32_t long int
#endif

#ifndef uint32_t
#define uint32_t unsigned long int
#endif

#ifndef int16_t
#define int16_t signed short
#endif

#ifndef uint16_t
#define uint16_t unsigned short
#endif

#ifndef status_t
#define status_t long int
#endif

    static const int kNumPhaseBits = 30;
    // phase mask for fraction
    static const uint32_t kPhaseMask = (1<<30)-1;
    // multiplier to calculate fixed point phase increment
    static const uint32_t kPhaseMultiplier = (1 << 30);

    static const int kNumInterpBits = 15;

    // bits to shift the phase fraction down to avoid overflow
    static const int kPreInterpShift = 15; //=kNumPhaseBits - kNumInterpBits;

typedef struct Buffer {
            void*       raw;
            short*      i16;
            int8_t*     i8;
            long frameCount;
        }Buffer;

typedef enum src_quality {
            DEFAULT=0,
            LOW_QUALITY=1,
            MED_QUALITY=2,
            HIGH_QUALITY=3
        }src_quality;

typedef struct LVAudioResampler
{

    int32_t mBitDepth;
    int32_t mChannelCount;
    int32_t mSampleRate;
    int32_t mInSampleRate;
    Buffer mBuffer;
    int16_t mVolume[2];
    int16_t mTargetVolume[2];
    int mFormat;
    long mInputIndex;
    int32_t mPhaseIncrement;
    uint32_t mPhaseFraction;
    int mX0L;
    int mX0R;
    int32_t kPreInterpShift;
    int32_t kNumInterpBits;
    src_quality mQuality;
}LVAudioResampler;


int32_t LVAudioResamplerCreate(int bitDepth, int inChannelCount,
        int32_t sampleRate, int quality);
void LVAudiosetSampleRate(int32_t context,int32_t inSampleRate);
void LVAudiosetVolume(int32_t context, int16_t left, int16_t right) ;

void LVAudioresample_LowQuality(int16_t* out, int16_t* input, long outFrameCount, int32_t context);
void LVResampler_LowQualityInit(int bitDepth, int inChannelCount,
        int32_t sampleRate, int32_t context);


void MonoTo2I_16( const short *src,
                        short *dst,
                        short n);

void From2iToMono_16( const short *src,
                            short *dst,
                            short n);
#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* GLVAUDIORESAMPLER_H */


