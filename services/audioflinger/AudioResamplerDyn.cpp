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

#define LOG_TAG "AudioResamplerDyn"
//#define LOG_NDEBUG 0

#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <math.h>

#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <utils/Log.h>

#include "AudioResamplerFirOps.h" // USE_NEON and USE_INLINE_ASSEMBLY defined here
#include "AudioResamplerFirProcess.h"
#include "AudioResamplerFirProcessNeon.h"
#include "AudioResamplerFirGen.h" // requires math.h
#include "AudioResamplerDyn.h"

//#define DEBUG_RESAMPLER

namespace android {

// generate a unique resample type compile-time constant (constexpr)
#define RESAMPLETYPE(CHANNELS, LOCKED, STRIDE, COEFTYPE) \
    ((((CHANNELS)-1)&1) | !!(LOCKED)<<1 | (COEFTYPE)<<2 \
    | ((STRIDE)==8 ? 1 : (STRIDE)==16 ? 2 : 0)<<3)

/*
 * InBuffer is a type agnostic input buffer.
 *
 * Layout of the state buffer for halfNumCoefs=8.
 *
 * [rrrrrrppppppppnnnnnnnnrrrrrrrrrrrrrrrrrrr.... rrrrrrr]
 *  S            I                                R
 *
 * S = mState
 * I = mImpulse
 * R = mRingFull
 * p = past samples, convoluted with the (p)ositive side of sinc()
 * n = future samples, convoluted with the (n)egative side of sinc()
 * r = extra space for implementing the ring buffer
 */

template<typename TI>
AudioResamplerDyn::InBuffer<TI>::InBuffer()
    : mState(NULL), mImpulse(NULL), mRingFull(NULL), mStateSize(0) {
}

template<typename TI>
AudioResamplerDyn::InBuffer<TI>::~InBuffer() {
    init();
}

template<typename TI>
void AudioResamplerDyn::InBuffer<TI>::init() {
    free(mState);
    mState = NULL;
    mImpulse = NULL;
    mRingFull = NULL;
    mStateSize = 0;
}

// resizes the state buffer to accommodate the appropriate filter length
template<typename TI>
void AudioResamplerDyn::InBuffer<TI>::resize(int CHANNELS, int halfNumCoefs) {
    // calculate desired state size
    int stateSize = halfNumCoefs * CHANNELS * 2
            * kStateSizeMultipleOfFilterLength;

    // check if buffer needs resizing
    if (mState
            && stateSize == mStateSize
            && mRingFull-mState == mStateSize-halfNumCoefs*CHANNELS) {
        return;
    }

    // create new buffer
    TI* state = (int16_t*)memalign(32, stateSize*sizeof(*state));
    memset(state, 0, stateSize*sizeof(*state));

    // attempt to preserve state
    if (mState) {
        TI* srcLo = mImpulse - halfNumCoefs*CHANNELS;
        TI* srcHi = mImpulse + halfNumCoefs*CHANNELS;
        TI* dst = state;

        if (srcLo < mState) {
            dst += mState-srcLo;
            srcLo = mState;
        }
        if (srcHi > mState + mStateSize) {
            srcHi = mState + mStateSize;
        }
        memcpy(dst, srcLo, (srcHi - srcLo) * sizeof(*srcLo));
        free(mState);
    }

    // set class member vars
    mState = state;
    mStateSize = stateSize;
    mImpulse = mState + halfNumCoefs*CHANNELS; // actually one sample greater than needed
    mRingFull = mState + mStateSize - halfNumCoefs*CHANNELS;
}

// copy in the input data into the head (impulse+halfNumCoefs) of the buffer.
template<typename TI>
template<int CHANNELS>
void AudioResamplerDyn::InBuffer<TI>::readAgain(TI*& impulse, const int halfNumCoefs,
        const TI* const in, const size_t inputIndex) {
    int16_t* head = impulse + halfNumCoefs*CHANNELS;
    for (size_t i=0 ; i<CHANNELS ; i++) {
        head[i] = in[inputIndex*CHANNELS + i];
    }
}

// advance the impulse pointer, and load in data into the head (impulse+halfNumCoefs)
template<typename TI>
template<int CHANNELS>
void AudioResamplerDyn::InBuffer<TI>::readAdvance(TI*& impulse, const int halfNumCoefs,
        const TI* const in, const size_t inputIndex) {
    impulse += CHANNELS;

    if (CC_UNLIKELY(impulse >= mRingFull)) {
        const size_t shiftDown = mRingFull - mState - halfNumCoefs*CHANNELS;
        memcpy(mState, mState+shiftDown, halfNumCoefs*CHANNELS*2*sizeof(TI));
        impulse -= shiftDown;
    }
    readAgain<CHANNELS>(impulse, halfNumCoefs, in, inputIndex);
}

void AudioResamplerDyn::Constants::set(
        int L, int halfNumCoefs, int inSampleRate, int outSampleRate)
{
    int bits = 0;
    int lscale = inSampleRate/outSampleRate < 2 ? L - 1 :
            static_cast<int>(static_cast<uint64_t>(L)*inSampleRate/outSampleRate);
    for (int i=lscale; i; ++bits, i>>=1)
        ;
    mL = L;
    mShift = kNumPhaseBits - bits;
    mHalfNumCoefs = halfNumCoefs;
}

AudioResamplerDyn::AudioResamplerDyn(int bitDepth,
        int inChannelCount, int32_t sampleRate, src_quality quality)
    : AudioResampler(bitDepth, inChannelCount, sampleRate, quality),
    mResampleType(0), mFilterSampleRate(0), mCoefBuffer(NULL)
{
    mVolumeSimd[0] = mVolumeSimd[1] = 0;
    mConstants.set(128, 8, mSampleRate, mSampleRate); // TODO: set better
}

AudioResamplerDyn::~AudioResamplerDyn() {
    free(mCoefBuffer);
}

void AudioResamplerDyn::init() {
    mFilterSampleRate = 0; // always trigger new filter generation
    mInBuffer.init();
}

void AudioResamplerDyn::setVolume(int16_t left, int16_t right) {
    AudioResampler::setVolume(left, right);
    mVolumeSimd[0] = static_cast<int32_t>(left)<<16;
    mVolumeSimd[1] = static_cast<int32_t>(right)<<16;
}

template <typename T> T max(T a, T b) {return a > b ? a : b;}

template <typename T> T absdiff(T a, T b) {return a > b ? a - b : b - a;}

template<typename T>
void AudioResamplerDyn::createKaiserFir(Constants &c, double stopBandAtten,
        int inSampleRate, int outSampleRate, double tbwCheat) {
    T* buf = reinterpret_cast<T*>(memalign(32, (c.mL+1)*c.mHalfNumCoefs*sizeof(T)));
    static const double atten = 0.9998;   // to avoid ripple overflow
    double fcr;
    double tbw = firKaiserTbw(c.mHalfNumCoefs, stopBandAtten);

    if (inSampleRate < outSampleRate) { // upsample
        fcr = max(0.5*tbwCheat - tbw/2, tbw/2);
    } else { // downsample
        fcr = max(0.5*tbwCheat*outSampleRate/inSampleRate - tbw/2, tbw/2);
    }
    // create and set filter
    firKaiserGen(buf, c.mL, c.mHalfNumCoefs, stopBandAtten, fcr, atten);
    c.setBuf(buf);
    if (mCoefBuffer) {
        free(mCoefBuffer);
    }
    mCoefBuffer = buf;
#ifdef DEBUG_RESAMPLER
    // print basic filter stats
    printf("L:%d  hnc:%d  stopBandAtten:%lf  fcr:%lf  atten:%lf  tbw:%lf\n",
            c.mL, c.mHalfNumCoefs, stopBandAtten, fcr, atten, tbw);
    // test the filter and report results
    double fp = (fcr - tbw/2)/c.mL;
    double fs = (fcr + tbw/2)/c.mL;
    double fmin, fmax;
    testFir(buf, c.mL, c.mHalfNumCoefs, 0., fp, 100, fmin, fmax);
    double d1 = (fmax - fmin)/2.;
    double ap = -20.*log10(1. - d1); // passband ripple
    printf("passband(%lf, %lf): %.8lf %.8lf %.8lf\n", 0., fp, (fmax + fmin)/2., d1, ap);
    testFir(buf, c.mL, c.mHalfNumCoefs, fs, 0.5, 100, fmin, fmax);
    double d2 = fmax;
    double as = -20.*log10(d2); // stopband attenuation
    printf("stopband(%lf, %lf): %.8lf %.8lf %.3lf\n", fs, 0.5, (fmax + fmin)/2., d2, as);
#endif
}

// recursive gcd (TODO: verify tail recursion elimination should make this iterate)
static int gcd(int n, int m) {
    if (m == 0) {
        return n;
    }
    return gcd(m, n % m);
}

static bool isClose(int32_t newSampleRate, int32_t prevSampleRate, int32_t filterSampleRate) {
    int pdiff = absdiff(newSampleRate, prevSampleRate);
    int adiff = absdiff(newSampleRate, filterSampleRate);

    // allow up to 6% relative change increments.
    // allow up to 12% absolute change increments (from filter design)
    return pdiff < prevSampleRate>>4 && adiff < filterSampleRate>>3;
}

void AudioResamplerDyn::setSampleRate(int32_t inSampleRate) {
    if (mInSampleRate == inSampleRate) {
        return;
    }
    int32_t oldSampleRate = mInSampleRate;
    int32_t oldHalfNumCoefs = mConstants.mHalfNumCoefs;
    uint32_t oldPhaseWrapLimit = mConstants.mL << mConstants.mShift;
    bool useS32 = false;

    mInSampleRate = inSampleRate;

    // TODO: Add precalculated Equiripple filters

    if (!isClose(inSampleRate, oldSampleRate, mFilterSampleRate)) {
        mFilterSampleRate = inSampleRate;

        // Begin Kaiser Filter computation
        //
        // The quantization floor for S16 is about 96db - 10*log_10(#length) + 3dB.
        // Keep the stop band attenuation no greater than 84-85dB for 32 length S16 filters
        //
        // For s32 we keep the stop band attenuation at the same as 16b resolution, about
        // 96-98dB
        //

        double stopBandAtten;
        double tbwCheat = 1.; // how much we "cheat" into aliasing
        int halfLength;
        if (getQuality() == DYN_HIGH_QUALITY) {
            // 32b coefficients, 64 length
            useS32 = true;
            stopBandAtten = 98.;
            halfLength = 32;
        } else if (getQuality() == DYN_LOW_QUALITY) {
            // 16b coefficients, 16-32 length
            useS32 = false;
            stopBandAtten = 80.;
            if (mSampleRate >= inSampleRate * 2) {
                halfLength = 16;
            } else {
                halfLength = 8;
            }
            if (mSampleRate >= inSampleRate) {
                tbwCheat = 1.05;
            } else {
                tbwCheat = 1.03;
            }
        } else { // medium quality
            // 16b coefficients, 32-64 length
            useS32 = false;
            stopBandAtten = 84.;
            if (mSampleRate >= inSampleRate * 4) {
                halfLength = 32;
            } else if (mSampleRate >= inSampleRate * 2) {
                halfLength = 24;
            } else {
                halfLength = 16;
            }
            if (mSampleRate >= inSampleRate) {
                tbwCheat = 1.03;
            } else {
                tbwCheat = 1.01;
            }
        }

        // determine the number of polyphases in the filterbank.
        // for 16b, it is desirable to have 2^(16/2) = 256 phases.
        // https://ccrma.stanford.edu/~jos/resample/Relation_Interpolation_Error_Quantization.html
        //
        // We are a bit more lax on this.

        int phases = mSampleRate / gcd(mSampleRate, inSampleRate);

        while (phases<63) { // too few phases, allow room for interpolation
            phases *= 2; // this code only needed to support dynamic rate changes
        }
        if (phases>=256) {  // too many phases, always interpolate
            phases = 127;
        }

        // create the filter
        mConstants.set(phases, halfLength, inSampleRate, mSampleRate);
        if (useS32) {
            createKaiserFir<int32_t>(mConstants, stopBandAtten,
                    inSampleRate, mSampleRate, tbwCheat);
        } else {
            createKaiserFir<int16_t>(mConstants, stopBandAtten,
                    inSampleRate, mSampleRate, tbwCheat);
        }
    } // End Kaiser filter

    // update phase and state based on the new filter.
    const Constants& c(mConstants);
    mInBuffer.resize(mChannelCount, c.mHalfNumCoefs);
    const uint32_t phaseWrapLimit = c.mL << c.mShift;
    // try to preserve as much of the phase fraction as possible for on-the-fly changes
    mPhaseFraction = static_cast<unsigned long long>(mPhaseFraction)
            * phaseWrapLimit / oldPhaseWrapLimit;
    mPhaseFraction %= phaseWrapLimit; // should not do anything, but just in case.
    mPhaseIncrement = static_cast<uint32_t>(static_cast<double>(phaseWrapLimit)
            * inSampleRate / mSampleRate);

    // determine which resampler to use
    // check if locked phase (works only if mPhaseIncrement has no "fractional phase bits")
    int locked = (mPhaseIncrement << (sizeof(mPhaseIncrement)*8 - c.mShift)) == 0;
    int stride = (c.mHalfNumCoefs&7)==0 ? 16 : (c.mHalfNumCoefs&3)==0 ? 8 : 2;
    if (locked) {
        mPhaseFraction = mPhaseFraction >> c.mShift << c.mShift; // remove fractional phase
    }
    if (!USE_NEON) {
        stride = 2; // C version only
    }
    // TODO: Remove this for testing
    //stride = 2;
    mResampleType = RESAMPLETYPE(mChannelCount, locked, stride, !!useS32);
#ifdef DEBUG_RESAMPLER
    printf("channels:%d  %s  stride:%d  %s  coef:%d  shift:%d\n",
            mChannelCount, locked ? "locked" : "interpolated",
            stride, useS32 ? "S32" : "S16", 2*c.mHalfNumCoefs, c.mShift);
#endif
}

void AudioResamplerDyn::resample(int32_t* out, size_t outFrameCount,
            AudioBufferProvider* provider)
{
    // TODO:
    // 24 cases - this perhaps can be reduced later, as testing might take too long
    switch (mResampleType) {

    // stride 16 (stride 2 for machines that do not support NEON)
    case RESAMPLETYPE(1, true, 16, 0):
        return resample<1, true, 16>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(2, true, 16, 0):
        return resample<2, true, 16>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(1, false, 16, 0):
        return resample<1, false, 16>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(2, false, 16, 0):
        return resample<2, false, 16>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(1, true, 16, 1):
        return resample<1, true, 16>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(2, true, 16, 1):
        return resample<2, true, 16>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(1, false, 16, 1):
        return resample<1, false, 16>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(2, false, 16, 1):
        return resample<2, false, 16>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
#if 0
    // TODO: Remove these?
    // stride 8
    case RESAMPLETYPE(1, true, 8, 0):
        return resample<1, true, 8>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(2, true, 8, 0):
        return resample<2, true, 8>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(1, false, 8, 0):
        return resample<1, false, 8>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(2, false, 8, 0):
        return resample<2, false, 8>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(1, true, 8, 1):
        return resample<1, true, 8>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(2, true, 8, 1):
        return resample<2, true, 8>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(1, false, 8, 1):
        return resample<1, false, 8>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(2, false, 8, 1):
        return resample<2, false, 8>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    // stride 2 (can handle any filter length)
    case RESAMPLETYPE(1, true, 2, 0):
        return resample<1, true, 2>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(2, true, 2, 0):
        return resample<2, true, 2>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(1, false, 2, 0):
        return resample<1, false, 2>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(2, false, 2, 0):
        return resample<2, false, 2>(out, outFrameCount, mConstants.mFirCoefsS16, provider);
    case RESAMPLETYPE(1, true, 2, 1):
        return resample<1, true, 2>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(2, true, 2, 1):
        return resample<2, true, 2>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(1, false, 2, 1):
        return resample<1, false, 2>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
    case RESAMPLETYPE(2, false, 2, 1):
        return resample<2, false, 2>(out, outFrameCount, mConstants.mFirCoefsS32, provider);
#endif
    default:
        ; // error
    }
}

template<int CHANNELS, bool LOCKED, int STRIDE, typename TC>
void AudioResamplerDyn::resample(int32_t* out, size_t outFrameCount,
        const TC* const coefs,  AudioBufferProvider* provider)
{
    const Constants& c(mConstants);
    int16_t* impulse = mInBuffer.getImpulse();
    size_t inputIndex = mInputIndex;
    uint32_t phaseFraction = mPhaseFraction;
    const uint32_t phaseIncrement = mPhaseIncrement;
    size_t outputIndex = 0;
    size_t outputSampleCount = outFrameCount * 2;   // stereo output
    size_t inFrameCount = (outFrameCount*mInSampleRate)/mSampleRate;
    const uint32_t phaseWrapLimit = c.mL << c.mShift;

    // NOTE: be very careful when modifying the code here. register
    // pressure is very high and a small change might cause the compiler
    // to generate far less efficient code.
    // Always sanity check the result with objdump or test-resample.

    // the following logic is a bit convoluted to keep the main processing loop
    // as tight as possible with register allocation.
    while (outputIndex < outputSampleCount) {
        // buffer is empty, fetch a new one
        while (mBuffer.frameCount == 0) {
            mBuffer.frameCount = inFrameCount;
            provider->getNextBuffer(&mBuffer,
                    calculateOutputPTS(outputIndex / 2));
            if (mBuffer.raw == NULL) {
                goto resample_exit;
            }
            if (phaseFraction >= phaseWrapLimit) { // read in data
                mInBuffer.readAdvance<CHANNELS>(
                        impulse, c.mHalfNumCoefs, mBuffer.i16, inputIndex);
                phaseFraction -= phaseWrapLimit;
                while (phaseFraction >= phaseWrapLimit) {
                    inputIndex++;
                    if (inputIndex >= mBuffer.frameCount) {
                        inputIndex -= mBuffer.frameCount;
                        provider->releaseBuffer(&mBuffer);
                        break;
                    }
                    mInBuffer.readAdvance<CHANNELS>(
                            impulse, c.mHalfNumCoefs, mBuffer.i16, inputIndex);
                    phaseFraction -= phaseWrapLimit;
                }
            }
        }
        const int16_t* const in = mBuffer.i16;
        const size_t frameCount = mBuffer.frameCount;
        const int coefShift = c.mShift;
        const int halfNumCoefs = c.mHalfNumCoefs;
        const int32_t* const volumeSimd = mVolumeSimd;

        // reread the last input in.
        mInBuffer.readAgain<CHANNELS>(impulse, halfNumCoefs, in, inputIndex);

        // main processing loop
        while (CC_LIKELY(outputIndex < outputSampleCount)) {
            // caution: fir() is inlined and may be large.
            // output will be loaded with the appropriate values
            //
            // from the input samples in impulse[-halfNumCoefs+1]... impulse[halfNumCoefs]
            // from the polyphase filter of (phaseFraction / phaseWrapLimit) in coefs.
            //
            fir<CHANNELS, LOCKED, STRIDE>(
                    &out[outputIndex],
                    phaseFraction, phaseWrapLimit,
                    coefShift, halfNumCoefs, coefs,
                    impulse, volumeSimd);
            outputIndex += 2;

            phaseFraction += phaseIncrement;
            while (phaseFraction >= phaseWrapLimit) {
                inputIndex++;
                if (inputIndex >= frameCount) {
                    goto done;  // need a new buffer
                }
                mInBuffer.readAdvance<CHANNELS>(impulse, halfNumCoefs, in, inputIndex);
                phaseFraction -= phaseWrapLimit;
            }
        }
done:
        // often arrives here when input buffer runs out
        if (inputIndex >= frameCount) {
            inputIndex -= frameCount;
            provider->releaseBuffer(&mBuffer);
            // mBuffer.frameCount MUST be zero here.
        }
    }

resample_exit:
    mInBuffer.setImpulse(impulse);
    mInputIndex = inputIndex;
    mPhaseFraction = phaseFraction;
}

// ----------------------------------------------------------------------------
}; // namespace android
