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

#ifndef ANDROID_AUDIO_RESAMPLER_FIR_PROCESS_H
#define ANDROID_AUDIO_RESAMPLER_FIR_PROCESS_H

namespace android {

// depends on AudioResamplerFirOps.h

template<int CHANNELS, typename TC>
static inline
void mac(
        int32_t& l, int32_t& r,
        const TC coef,
        const int16_t* samples)
{
    if (CHANNELS == 2) {
        uint32_t rl = *reinterpret_cast<const uint32_t*>(samples);
        l = mulAddRL(1, rl, coef, l);
        r = mulAddRL(0, rl, coef, r);
    } else {
        r = l = mulAdd(samples[0], coef, l);
    }
}

template<int CHANNELS, typename TC>
static inline
void interpolate(
        int32_t& l, int32_t& r,
        const TC coef_0, const TC coef_1,
        const int16_t lerp, const int16_t* samples)
{
    TC sinc;

    if (is_same<TC, int16_t>::value) {
        sinc = (lerp * ((coef_1-coef_0)<<1)>>16) + coef_0;
    } else {
        sinc = mulAdd(lerp, (coef_1-coef_0)<<1, coef_0);
    }
    if (CHANNELS == 2) {
        uint32_t rl = *reinterpret_cast<const uint32_t*>(samples);
        l = mulAddRL(1, rl, sinc, l);
        r = mulAddRL(0, rl, sinc, r);
    } else {
        r = l = mulAdd(samples[0], sinc, l);
    }
}

/*
 * Calculates a single output sample (two stereo frames).
 *
 * This function computes both the positive half FIR dot product and
 * the negative half FIR dot product, accumulates, and then applies the volume.
 *
 * This is a locked phase filter (it does not compute the interpolation).
 *
 * Use fir() to compute the proper coefficient pointers for a polyphase
 * filter bank.
 */

template <int CHANNELS, int STRIDE, typename TC>
static inline
void ProcessL(int32_t* const out,
        int count,
        const TC* coefsP,
        const TC* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    int32_t l = 0;
    int32_t r = 0;
    do {
        mac<CHANNELS>(l, r, *coefsP++, sP);
        sP -= CHANNELS;
        mac<CHANNELS>(l, r, *coefsN++, sN);
        sN += CHANNELS;
    } while (--count > 0);
    out[0] += 2 * mulRL(0, l, volumeLR[0]); // Note: only use top 16b
    out[1] += 2 * mulRL(0, r, volumeLR[1]); // Note: only use top 16b
}

/*
 * Calculates a single output sample (two stereo frames) interpolating phase.
 *
 * This function computes both the positive half FIR dot product and
 * the negative half FIR dot product, accumulates, and then applies the volume.
 *
 * This is an interpolated phase filter.
 *
 * Use fir() to compute the proper coefficient pointers for a polyphase
 * filter bank.
 */

template <int CHANNELS, int STRIDE, typename TC>
static inline
void Process(int32_t* const out,
        int count,
        const TC* coefsP,
        const TC* coefsN,
        const TC* coefsP1,
        const TC* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    (void) coefsP1; // suppress unused parameter warning
    (void) coefsN1;
    if (sizeof(*coefsP)==4) {
        lerpP >>= 16;   // ensure lerpP is 16b
    }
    int32_t l = 0;
    int32_t r = 0;
    for (size_t i = 0; i < count; ++i) {
        interpolate<CHANNELS>(l, r, coefsP[0], coefsP[count], lerpP, sP);
        coefsP++;
        sP -= CHANNELS;
        interpolate<CHANNELS>(l, r, coefsN[count], coefsN[0], lerpP, sN);
        coefsN++;
        sN += CHANNELS;
    }
    out[0] += 2 * mulRL(0, l, volumeLR[0]); // Note: only use top 16b
    out[1] += 2 * mulRL(0, r, volumeLR[1]); // Note: only use top 16b
}

/*
 * Calculates a single output sample (two stereo frames) from input sample pointer.
 *
 * This sets up the params for the accelerated Process() and ProcessL()
 * functions to do the appropriate dot products.
 *
 * @param out should point to the output buffer with at least enough space for 2 output frames.
 *
 * @param phase is the fractional distance between input samples for interpolation:
 * phase >= 0  && phase < phaseWrapLimit.  It can be thought of as a rational fraction
 * of phase/phaseWrapLimit.
 *
 * @param phaseWrapLimit is #polyphases<<coefShift, where #polyphases is the number of polyphases
 * in the polyphase filter. Likewise, #polyphases can be obtained as (phaseWrapLimit>>coefShift).
 *
 * @param coefShift gives the bit alignment of the polyphase index in the phase parameter.
 *
 * @param halfNumCoefs is the half the number of coefficients per polyphase filter. Since the
 * overall filterbank is odd-length symmetric, only halfNumCoefs need be stored.
 *
 * @param coefs is the polyphase filter bank, starting at from polyphase index 0, and ranging to
 * and including the #polyphases.  Each polyphase of the filter has half-length halfNumCoefs
 * (due to symmetry).  The total size of the filter bank in coefficients is
 * (#polyphases+1)*halfNumCoefs.
 *
 * The filter bank coefs should be aligned to a minimum of 16 bytes (preferrably to cache line).
 *
 * The coefs should be attenuated (to compensate for passband ripple)
 * if storing back into the native format.
 *
 * @param samples are unaligned input samples.  The position is in the "middle" of the
 * sample array with respect to the FIR filter:
 * the negative half of the filter is dot product from samples+1 to samples+halfNumCoefs;
 * the positive half of the filter is dot product from samples to samples-halfNumCoefs+1.
 *
 * @param volumeLR is a pointer to an array of two 32 bit volume values, one per stereo channel,
 * expressed as a S32 integer.  A negative value inverts the channel 180 degrees.
 * The pointer volumeLR should be aligned to a minimum of 8 bytes.
 * A typical value for volume is 0x1000 to align to a unity gain output of 20.12.
 *
 * In between calls to filterCoefficient, the phase is incremented by phaseIncrement, where
 * phaseIncrement is calculated as inputSampling * phaseWrapLimit / outputSampling.
 *
 * The filter polyphase index is given by indexP = phase >> coefShift. Due to
 * odd length symmetric filter, the polyphase index of the negative half depends on
 * whether interpolation is used.
 *
 * The fractional siting between the polyphase indices is given by the bits below coefShift:
 *
 * lerpP = phase << 32 - coefShift >> 1;  // for 32 bit unsigned phase multiply
 * lerpP = phase << 32 - coefShift >> 17; // for 16 bit unsigned phase multiply
 *
 * For integer types, this is expressed as:
 *
 * lerpP = phase << sizeof(phase)*8 - coefShift
 *              >> (sizeof(phase)-sizeof(*coefs))*8 + 1;
 *
 */

template<int CHANNELS, bool LOCKED, int STRIDE, typename TC>
static inline
void fir(int32_t* const out,
        const uint32_t phase, const uint32_t phaseWrapLimit,
        const int coefShift, const int halfNumCoefs, const TC* const coefs,
        const int16_t* const samples, const int32_t* const volumeLR)
{
    // NOTE: be very careful when modifying the code here. register
    // pressure is very high and a small change might cause the compiler
    // to generate far less efficient code.
    // Always sanity check the result with objdump or test-resample.

    if (LOCKED) {
        // locked polyphase (no interpolation)
        // Compute the polyphase filter index on the positive and negative side.
        uint32_t indexP = phase >> coefShift;
        uint32_t indexN = (phaseWrapLimit - phase) >> coefShift;
        const TC* coefsP = coefs + indexP*halfNumCoefs;
        const TC* coefsN = coefs + indexN*halfNumCoefs;
        const int16_t* sP = samples;
        const int16_t* sN = samples + CHANNELS;

        // dot product filter.
        ProcessL<CHANNELS, STRIDE>(out,
                halfNumCoefs, coefsP, coefsN, sP, sN, volumeLR);
    } else {
        // interpolated polyphase
        // Compute the polyphase filter index on the positive and negative side.
        uint32_t indexP = phase >> coefShift;
        uint32_t indexN = (phaseWrapLimit - phase - 1) >> coefShift; // one's complement.
        const TC* coefsP = coefs + indexP*halfNumCoefs;
        const TC* coefsN = coefs + indexN*halfNumCoefs;
        const TC* coefsP1 = coefsP + halfNumCoefs;
        const TC* coefsN1 = coefsN + halfNumCoefs;
        const int16_t* sP = samples;
        const int16_t* sN = samples + CHANNELS;

        // Interpolation fraction lerpP derived by shifting all the way up and down
        // to clear the appropriate bits and align to the appropriate level
        // for the integer multiply.  The constants should resolve in compile time.
        //
        // The interpolated filter coefficient is derived as follows for the pos/neg half:
        //
        // interpolated[P] = index[P]*lerpP + index[P+1]*(1-lerpP)
        // interpolated[N] = index[N+1]*lerpP + index[N]*(1-lerpP)
        uint32_t lerpP = phase << (sizeof(phase)*8 - coefShift)
                >> ((sizeof(phase)-sizeof(*coefs))*8 + 1);

        // on-the-fly interpolated dot product filter
        Process<CHANNELS, STRIDE>(out,
                halfNumCoefs, coefsP, coefsN, coefsP1, coefsN1, sP, sN, lerpP, volumeLR);
    }
}

}; // namespace android

#endif /*ANDROID_AUDIO_RESAMPLER_FIR_PROCESS_H*/
