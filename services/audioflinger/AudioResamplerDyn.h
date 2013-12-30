/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_RESAMPLER_DYN_H
#define ANDROID_AUDIO_RESAMPLER_DYN_H

#include <stdint.h>
#include <sys/types.h>
#include <cutils/log.h>

#include "AudioResampler.h"

namespace android {

class AudioResamplerDyn: public AudioResampler {
public:
    AudioResamplerDyn(int bitDepth, int inChannelCount, int32_t sampleRate,
            src_quality quality);

    virtual ~AudioResamplerDyn();

    virtual void init();

    virtual void setSampleRate(int32_t inSampleRate);

    virtual void setVolume(int16_t left, int16_t right);

    virtual void resample(int32_t* out, size_t outFrameCount,
            AudioBufferProvider* provider);

private:

    class Constants { // stores the filter constants.
    public:
        Constants() :
            mL(0), mShift(0), mHalfNumCoefs(0), mFirCoefsS16(NULL)
        {}
        void set(int L, int halfNumCoefs,
                int inSampleRate, int outSampleRate);
        inline void setBuf(int16_t* buf) {
            mFirCoefsS16 = buf;
        }
        inline void setBuf(int32_t* buf) {
            mFirCoefsS32 = buf;
        }

        int mL;       // interpolation phases in the filter.
        int mShift;   // right shift to get polyphase index
        unsigned int mHalfNumCoefs; // filter half #coefs
        union {       // polyphase filter bank
            const int16_t* mFirCoefsS16;
            const int32_t* mFirCoefsS32;
        };
    };

    // Input buffer management for a given input type TI, now (int16_t)
    // Is agnostic of the actual type, can work with int32_t and float.
    template<typename TI>
    class InBuffer {
    public:
        InBuffer();
        ~InBuffer();
        void init();
        void resize(int CHANNELS, int halfNumCoefs);

        // used for direct management of the mImpulse pointer
        inline TI* getImpulse() {
            return mImpulse;
        }
        inline void setImpulse(TI *impulse) {
            mImpulse = impulse;
        }
        template<int CHANNELS>
        inline void readAgain(TI*& impulse, const int halfNumCoefs,
                const TI* const in, const size_t inputIndex);
        template<int CHANNELS>
        inline void readAdvance(TI*& impulse, const int halfNumCoefs,
                const TI* const in, const size_t inputIndex);

    private:
        // tuning parameter guidelines: 2 <= multiple <= 8
        static const int kStateSizeMultipleOfFilterLength = 4;

        TI* mState;    // base pointer for the input buffer storage
        TI* mImpulse;  // current location of the impulse response (centered)
        TI* mRingFull; // mState <= mImpulse < mRingFull
        // in general, mRingFull = mState + mStateSize - halfNumCoefs*CHANNELS.
        size_t mStateSize; // in units of TI.
    };

    template<int CHANNELS, bool LOCKED, int STRIDE, typename TC>
    void resample(int32_t* out, size_t outFrameCount,
            const TC* const coefs, AudioBufferProvider* provider);

    template<typename T>
    void createKaiserFir(Constants &c, double stopBandAtten,
            int inSampleRate, int outSampleRate, double tbwCheat);

    InBuffer<int16_t> mInBuffer;
    Constants mConstants;  // current set of coefficient parameters
    int32_t __attribute__ ((aligned (8))) mVolumeSimd[2];
    int32_t mResampleType; // contains the resample type.
    int32_t mFilterSampleRate; // designed sample rate for the filter
    void* mCoefBuffer; // if a filter is created, this is not null
};

// ----------------------------------------------------------------------------
}; // namespace android

#endif /*ANDROID_AUDIO_RESAMPLER_DYN_H*/
