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

#ifndef ANDROID_AUDIO_RESAMPLER_FIR_PROCESS_NEON_H
#define ANDROID_AUDIO_RESAMPLER_FIR_PROCESS_NEON_H

namespace android {

// depends on AudioResamplerFirOps.h, AudioResamplerFirProcess.h

#if USE_NEON
//
// NEON specializations are enabled for Process() and ProcessL()
//
// TODO: Stride 16 and Stride 8 can be combined with one pass stride 8 (if necessary)
// and looping stride 16 (or vice versa). This has some polyphase coef data alignment
// issues with S16 coefs. Consider this later.

// Macros to save a mono/stereo accumulator sample in q0 (and q4) as stereo out.
#define ASSEMBLY_ACCUMULATE_MONO \
        "vld1.s32       {d2}, [%[vLR]:64]        \n"/* (1) load volumes */\
        "vld1.s32       {d3}, %[out]             \n"/* (2) unaligned load the output */\
        "vpadd.s32      d0, d0, d1               \n"/* (1) add all 4 partial sums */\
        "vpadd.s32      d0, d0, d0               \n"/* (1+4d) and replicate L/R */\
        "vqrdmulh.s32   d0, d0, d2               \n"/* (2+3d) apply volume */\
        "vqadd.s32      d3, d3, d0               \n"/* (1+4d) accumulate result (saturating) */\
        "vst1.s32       {d3}, %[out]             \n"/* (2+2d) store result */

#define ASSEMBLY_ACCUMULATE_STEREO \
        "vld1.s32       {d2}, [%[vLR]:64]        \n"/* (1) load volumes*/\
        "vld1.s32       {d3}, %[out]             \n"/* (2) unaligned load the output*/\
        "vpadd.s32      d0, d0, d1               \n"/* (1) add all 4 partial sums from q0*/\
        "vpadd.s32      d8, d8, d9               \n"/* (1) add all 4 partial sums from q4*/\
        "vpadd.s32      d0, d0, d8               \n"/* (1+4d) combine into L/R*/\
        "vqrdmulh.s32   d0, d0, d2               \n"/* (2+3d) apply volume*/\
        "vqadd.s32      d3, d3, d0               \n"/* (1+4d) accumulate result (saturating)*/\
        "vst1.s32       {d3}, %[out]             \n"/* (2+2d)store result*/

template <>
inline void ProcessL<1, 16>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0               \n"// (0 - combines+) accumulator = 0

        "1:                                      \n"

        "vld1.16        {q2}, [%[sP]]            \n"// (2+0d) load 8 16-bits mono samples
        "vld1.16        {q3}, [%[sN]]!           \n"// (2) load 8 16-bits mono samples
        "vld1.16        {q8}, [%[coefsP0]:128]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {q10}, [%[coefsN0]:128]! \n"// (1) load 8 16-bits coefs

        "vrev64.16      q2, q2                   \n"// (1) reverse s3, s2, s1, s0, s7, s6, s5, s4

        // reordering the vmal to do d6, d7 before d4, d5 is slower(?)
        "vmlal.s16      q0, d4, d17              \n"// (1+0d) multiply (reversed)samples by coef
        "vmlal.s16      q0, d5, d16              \n"// (1) multiply (reversed)samples by coef
        "vmlal.s16      q0, d6, d20              \n"// (1) multiply neg samples
        "vmlal.s16      q0, d7, d21              \n"// (1) multiply neg samples

        // moving these ARM instructions before neon above seems to be slower
        "subs           %[count], %[count], #8   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #16        \n"// (0) move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

         ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q10"
    );
}

template <>
inline void ProcessL<2, 16>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0               \n"// (1) acc_L = 0
        "veor           q4, q4, q4               \n"// (0 combines+) acc_R = 0

        "1:                                      \n"

        "vld2.16        {q2, q3}, [%[sP]]        \n"// (3+0d) load 8 16-bits stereo samples
        "vld2.16        {q5, q6}, [%[sN]]!       \n"// (3) load 8 16-bits stereo samples
        "vld1.16        {q8}, [%[coefsP0]:128]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {q10}, [%[coefsN0]:128]! \n"// (1) load 8 16-bits coefs

        "vrev64.16      q2, q2                   \n"// (1) reverse 8 frames of the left positive
        "vrev64.16      q3, q3                   \n"// (0 combines+) reverse right positive

        "vmlal.s16      q0, d4, d17              \n"// (1) multiply (reversed) samples left
        "vmlal.s16      q0, d5, d16              \n"// (1) multiply (reversed) samples left
        "vmlal.s16      q4, d6, d17              \n"// (1) multiply (reversed) samples right
        "vmlal.s16      q4, d7, d16              \n"// (1) multiply (reversed) samples right
        "vmlal.s16      q0, d10, d20             \n"// (1) multiply samples left
        "vmlal.s16      q0, d11, d21             \n"// (1) multiply samples left
        "vmlal.s16      q4, d12, d20             \n"// (1) multiply samples right
        "vmlal.s16      q4, d13, d21             \n"// (1) multiply samples right

        // moving these ARM before neon seems to be slower
        "subs           %[count], %[count], #8   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #32        \n"// (0) move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out] "=Uv" (out[0]),
          [count] "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP] "+r" (sP),
          [sN] "+r" (sN)
        : [vLR] "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q4", "q5", "q6",
          "q8", "q10"
     );
}

template <>
inline void Process<1, 16>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* coefsP1,
        const int16_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]          \n"// load the positive phase S32 Q15
        "veor           q0, q0, q0               \n"// (0 - combines+) accumulator = 0

        "1:                                      \n"

        "vld1.16        {q2}, [%[sP]]            \n"// (2+0d) load 8 16-bits mono samples
        "vld1.16        {q3}, [%[sN]]!           \n"// (2) load 8 16-bits mono samples
        "vld1.16        {q8}, [%[coefsP0]:128]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {q9}, [%[coefsP1]:128]!  \n"// (1) load 8 16-bits coefs for interpolation
        "vld1.16        {q10}, [%[coefsN1]:128]! \n"// (1) load 8 16-bits coefs
        "vld1.16        {q11}, [%[coefsN0]:128]! \n"// (1) load 8 16-bits coefs for interpolation

        "vsub.s16       q9, q9, q8               \n"// (1) interpolate (step1) 1st set of coefs
        "vsub.s16       q11, q11, q10            \n"// (1) interpolate (step1) 2nd set of coets

        "vqrdmulh.s16   q9, q9, d2[0]            \n"// (2) interpolate (step2) 1st set of coefs
        "vqrdmulh.s16   q11, q11, d2[0]          \n"// (2) interpolate (step2) 2nd set of coefs

        "vrev64.16      q2, q2                   \n"// (1) reverse s3, s2, s1, s0, s7, s6, s5, s4

        "vadd.s16       q8, q8, q9               \n"// (1+2d) interpolate (step3) 1st set
        "vadd.s16       q10, q10, q11            \n"// (1+1d) interpolate (step3) 2nd set

        // reordering the vmal to do d6, d7 before d4, d5 is slower(?)
        "vmlal.s16      q0, d4, d17              \n"// (1+0d) multiply reversed samples by coef
        "vmlal.s16      q0, d5, d16              \n"// (1) multiply reversed samples by coef
        "vmlal.s16      q0, d6, d20              \n"// (1) multiply neg samples
        "vmlal.s16      q0, d7, d21              \n"// (1) multiply neg samples

        // moving these ARM instructions before neon above seems to be slower
        "subs           %[count], %[count], #8   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #16        \n"// (0) move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [coefsP1] "+r" (coefsP1),
          [coefsN1] "+r" (coefsN1),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q9", "q10", "q11"
    );
}

template <>
inline void Process<2, 16>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* coefsP1,
        const int16_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]          \n"// load the positive phase
        "veor           q0, q0, q0               \n"// (1) acc_L = 0
        "veor           q4, q4, q4               \n"// (0 combines+) acc_R = 0

        "1:                                      \n"

        "vld2.16        {q2, q3}, [%[sP]]        \n"// (3+0d) load 8 16-bits stereo samples
        "vld2.16        {q5, q6}, [%[sN]]!       \n"// (3) load 8 16-bits stereo samples
        "vld1.16        {q8}, [%[coefsP0]:128]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {q9}, [%[coefsP1]:128]!  \n"// (1) load 8 16-bits coefs for interpolation
        "vld1.16        {q10}, [%[coefsN1]:128]! \n"// (1) load 8 16-bits coefs
        "vld1.16        {q11}, [%[coefsN0]:128]! \n"// (1) load 8 16-bits coefs for interpolation

        "vsub.s16       q9, q9, q8               \n"// (1) interpolate (step1) 1st set of coefs
        "vsub.s16       q11, q11, q10            \n"// (1) interpolate (step1) 2nd set of coets

        "vqrdmulh.s16   q9, q9, d2[0]            \n"// (2) interpolate (step2) 1st set of coefs
        "vqrdmulh.s16   q11, q11, d2[0]          \n"// (2) interpolate (step2) 2nd set of coefs

        "vrev64.16      q2, q2                   \n"// (1) reverse 8 frames of the left positive
        "vrev64.16      q3, q3                   \n"// (1) reverse 8 frames of the right positive

        "vadd.s16       q8, q8, q9               \n"// (1+1d) interpolate (step3) 1st set
        "vadd.s16       q10, q10, q11            \n"// (1+1d) interpolate (step3) 2nd set

        "vmlal.s16      q0, d4, d17              \n"// (1) multiply reversed samples left
        "vmlal.s16      q0, d5, d16              \n"// (1) multiply reversed samples left
        "vmlal.s16      q4, d6, d17              \n"// (1) multiply reversed samples right
        "vmlal.s16      q4, d7, d16              \n"// (1) multiply reversed samples right
        "vmlal.s16      q0, d10, d20             \n"// (1) multiply samples left
        "vmlal.s16      q0, d11, d21             \n"// (1) multiply samples left
        "vmlal.s16      q4, d12, d20             \n"// (1) multiply samples right
        "vmlal.s16      q4, d13, d21             \n"// (1) multiply samples right

        // moving these ARM before neon seems to be slower
        "subs           %[count], %[count], #8   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #32        \n"// (0) move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out] "=Uv" (out[0]),
          [count] "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [coefsP1] "+r" (coefsP1),
          [coefsN1] "+r" (coefsN1),
          [sP] "+r" (sP),
          [sN] "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR] "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q4", "q5", "q6",
          "q8", "q9", "q10", "q11"
    );
}

template <>
inline void ProcessL<1, 16>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0                    \n"// result, initialize to 0

        "1:                                           \n"

        "vld1.16        {q2}, [%[sP]]                 \n"// load 8 16-bits mono samples
        "vld1.16        {q3}, [%[sN]]!                \n"// load 8 16-bits mono samples
        "vld1.32        {q8, q9}, [%[coefsP0]:128]!   \n"// load 8 32-bits coefs
        "vld1.32        {q10, q11}, [%[coefsN0]:128]! \n"// load 8 32-bits coefs

        "vrev64.16      q2, q2                        \n"// reverse 8 frames of the positive side

        "vshll.s16      q12, d4, #15                  \n"// extend samples to 31 bits
        "vshll.s16      q13, d5, #15                  \n"// extend samples to 31 bits

        "vshll.s16      q14, d6, #15                  \n"// extend samples to 31 bits
        "vshll.s16      q15, d7, #15                  \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q9                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10                 \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q11                 \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12                   \n"// accumulate result
        "vadd.s32       q13, q13, q14                 \n"// accumulate result
        "vadd.s32       q0, q0, q15                   \n"// accumulate result
        "vadd.s32       q0, q0, q13                   \n"// accumulate result

        "sub            %[sP], %[sP], #16             \n"// move pointer to next set of samples
        "subs           %[count], %[count], #8        \n"// update loop counter

        "bne            1b                            \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q9", "q10", "q11",
          "q12", "q13", "q14", "q15"
    );
}

template <>
inline void ProcessL<2, 16>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0                    \n"// result, initialize to 0
        "veor           q4, q4, q4                    \n"// result, initialize to 0

        "1:                                           \n"

        "vld2.16        {q2, q3}, [%[sP]]             \n"// load 4 16-bits stereo samples
        "vld2.16        {q5, q6}, [%[sN]]!            \n"// load 4 16-bits stereo samples
        "vld1.32        {q8, q9}, [%[coefsP0]:128]!   \n"// load 4 32-bits coefs
        "vld1.32        {q10, q11}, [%[coefsN0]:128]! \n"// load 4 32-bits coefs

        "vrev64.16      q2, q2                        \n"// reverse 8 frames of the positive side
        "vrev64.16      q3, q3                        \n"// reverse 8 frames of the positive side

        "vshll.s16      q12,  d4, #15                 \n"// extend samples to 31 bits
        "vshll.s16      q13,  d5, #15                 \n"// extend samples to 31 bits

        "vshll.s16      q14,  d10, #15                \n"// extend samples to 31 bits
        "vshll.s16      q15,  d11, #15                \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q9                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10                 \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q11                 \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12                   \n"// accumulate result
        "vadd.s32       q13, q13, q14                 \n"// accumulate result
        "vadd.s32       q0, q0, q15                   \n"// (+1) accumulate result
        "vadd.s32       q0, q0, q13                   \n"// (+1) accumulate result

        "vshll.s16      q12,  d6, #15                 \n"// extend samples to 31 bits
        "vshll.s16      q13,  d7, #15                 \n"// extend samples to 31 bits

        "vshll.s16      q14,  d12, #15                \n"// extend samples to 31 bits
        "vshll.s16      q15,  d13, #15                \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q9                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10                 \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q11                 \n"// multiply samples by interpolated coef

        "vadd.s32       q4, q4, q12                   \n"// accumulate result
        "vadd.s32       q13, q13, q14                 \n"// accumulate result
        "vadd.s32       q4, q4, q15                   \n"// (+1) accumulate result
        "vadd.s32       q4, q4, q13                   \n"// (+1) accumulate result

        "subs           %[count], %[count], #8        \n"// update loop counter
        "sub            %[sP], %[sP], #32             \n"// move pointer to next set of samples

        "bne            1b                            \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q4", "q5", "q6",
          "q8", "q9", "q10", "q11",
          "q12", "q13", "q14", "q15"
    );
}

template <>
inline void Process<1, 16>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int32_t* coefsP1,
        const int32_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]               \n"// load the positive phase
        "veor           q0, q0, q0                    \n"// result, initialize to 0

        "1:                                           \n"

        "vld1.16        {q2}, [%[sP]]                 \n"// load 8 16-bits mono samples
        "vld1.16        {q3}, [%[sN]]!                \n"// load 8 16-bits mono samples
        "vld1.32        {q8, q9}, [%[coefsP0]:128]!   \n"// load 8 32-bits coefs
        "vld1.32        {q12, q13}, [%[coefsP1]:128]! \n"// load 8 32-bits coefs
        "vld1.32        {q10, q11}, [%[coefsN1]:128]! \n"// load 8 32-bits coefs
        "vld1.32        {q14, q15}, [%[coefsN0]:128]! \n"// load 8 32-bits coefs

        "vsub.s32       q12, q12, q8                  \n"// interpolate (step1)
        "vsub.s32       q13, q13, q9                  \n"// interpolate (step1)
        "vsub.s32       q14, q14, q10                 \n"// interpolate (step1)
        "vsub.s32       q15, q15, q11                 \n"// interpolate (step1)

        "vqrdmulh.s32   q12, q12, d2[0]               \n"// interpolate (step2)
        "vqrdmulh.s32   q13, q13, d2[0]               \n"// interpolate (step2)
        "vqrdmulh.s32   q14, q14, d2[0]               \n"// interpolate (step2)
        "vqrdmulh.s32   q15, q15, d2[0]               \n"// interpolate (step2)

        "vadd.s32       q8, q8, q12                   \n"// interpolate (step3)
        "vadd.s32       q9, q9, q13                   \n"// interpolate (step3)
        "vadd.s32       q10, q10, q14                 \n"// interpolate (step3)
        "vadd.s32       q11, q11, q15                 \n"// interpolate (step3)

        "vrev64.16      q2, q2                        \n"// reverse 8 frames of the positive side

        "vshll.s16      q12,  d4, #15                 \n"// extend samples to 31 bits
        "vshll.s16      q13,  d5, #15                 \n"// extend samples to 31 bits

        "vshll.s16      q14,  d6, #15                 \n"// extend samples to 31 bits
        "vshll.s16      q15,  d7, #15                 \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q9                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10                 \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q11                 \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12                   \n"// accumulate result
        "vadd.s32       q13, q13, q14                 \n"// accumulate result
        "vadd.s32       q0, q0, q15                   \n"// accumulate result
        "vadd.s32       q0, q0, q13                   \n"// accumulate result

        "sub            %[sP], %[sP], #16             \n"// move pointer to next set of samples
        "subs           %[count], %[count], #8        \n"// update loop counter

        "bne            1b                            \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [coefsP1] "+r" (coefsP1),
          [coefsN1] "+r" (coefsN1),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q9", "q10", "q11",
          "q12", "q13", "q14", "q15"
    );
}

template <>
inline void Process<2, 16>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int32_t* coefsP1,
        const int32_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 16;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]               \n"// load the positive phase
        "veor           q0, q0, q0                    \n"// result, initialize to 0
        "veor           q4, q4, q4                    \n"// result, initialize to 0

        "1:                                           \n"

        "vld2.16        {q2, q3}, [%[sP]]             \n"// load 4 16-bits stereo samples
        "vld2.16        {q5, q6}, [%[sN]]!            \n"// load 4 16-bits stereo samples
        "vld1.32        {q8, q9}, [%[coefsP0]:128]!   \n"// load 8 32-bits coefs
        "vld1.32        {q12, q13}, [%[coefsP1]:128]! \n"// load 8 32-bits coefs
        "vld1.32        {q10, q11}, [%[coefsN1]:128]! \n"// load 8 32-bits coefs
        "vld1.32        {q14, q15}, [%[coefsN0]:128]! \n"// load 8 32-bits coefs

        "vsub.s32       q12, q12, q8                  \n"// interpolate (step1)
        "vsub.s32       q13, q13, q9                  \n"// interpolate (step1)
        "vsub.s32       q14, q14, q10                 \n"// interpolate (step1)
        "vsub.s32       q15, q15, q11                 \n"// interpolate (step1)

        "vqrdmulh.s32   q12, q12, d2[0]               \n"// interpolate (step2)
        "vqrdmulh.s32   q13, q13, d2[0]               \n"// interpolate (step2)
        "vqrdmulh.s32   q14, q14, d2[0]               \n"// interpolate (step2)
        "vqrdmulh.s32   q15, q15, d2[0]               \n"// interpolate (step2)

        "vadd.s32       q8, q8, q12                   \n"// interpolate (step3)
        "vadd.s32       q9, q9, q13                   \n"// interpolate (step3)
        "vadd.s32       q10, q10, q14                 \n"// interpolate (step3)
        "vadd.s32       q11, q11, q15                 \n"// interpolate (step3)

        "vrev64.16      q2, q2                        \n"// reverse 8 frames of the positive side
        "vrev64.16      q3, q3                        \n"// reverse 8 frames of the positive side

        "vshll.s16      q12,  d4, #15                 \n"// extend samples to 31 bits
        "vshll.s16      q13,  d5, #15                 \n"// extend samples to 31 bits

        "vshll.s16      q14,  d10, #15                \n"// extend samples to 31 bits
        "vshll.s16      q15,  d11, #15                \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q9                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10                 \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q11                 \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12                   \n"// accumulate result
        "vadd.s32       q13, q13, q14                 \n"// accumulate result
        "vadd.s32       q0, q0, q15                   \n"// (+1) accumulate result
        "vadd.s32       q0, q0, q13                   \n"// (+1) accumulate result

        "vshll.s16      q12,  d6, #15                 \n"// extend samples to 31 bits
        "vshll.s16      q13,  d7, #15                 \n"// extend samples to 31 bits

        "vshll.s16      q14,  d12, #15                \n"// extend samples to 31 bits
        "vshll.s16      q15,  d13, #15                \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q9                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8                  \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10                 \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q11                 \n"// multiply samples by interpolated coef

        "vadd.s32       q4, q4, q12                   \n"// accumulate result
        "vadd.s32       q13, q13, q14                 \n"// accumulate result
        "vadd.s32       q4, q4, q15                   \n"// (+1) accumulate result
        "vadd.s32       q4, q4, q13                   \n"// (+1) accumulate result

        "subs           %[count], %[count], #8        \n"// update loop counter
        "sub            %[sP], %[sP], #32             \n"// move pointer to next set of samples

        "bne            1b                            \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [coefsP1] "+r" (coefsP1),
          [coefsN1] "+r" (coefsN1),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q4", "q5", "q6",
          "q8", "q9", "q10", "q11",
          "q12", "q13", "q14", "q15"
    );
}

template <>
inline void ProcessL<1, 8>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0               \n"// (0 - combines+) accumulator = 0

        "1:                                      \n"

        "vld1.16        {d4}, [%[sP]]            \n"// (2+0d) load 4 16-bits mono samples
        "vld1.16        {d6}, [%[sN]]!           \n"// (2) load 4 16-bits mono samples
        "vld1.16        {d16}, [%[coefsP0]:64]!  \n"// (1) load 4 16-bits coefs
        "vld1.16        {d20}, [%[coefsN0]:64]!  \n"// (1) load 4 16-bits coefs

        "vrev64.16      d4, d4                   \n"// (1) reversed s3, s2, s1, s0, s7, s6, s5, s4

        // reordering the vmal to do d6, d7 before d4, d5 is slower(?)
        "vmlal.s16      q0, d4, d16              \n"// (1) multiply (reversed)samples by coef
        "vmlal.s16      q0, d6, d20              \n"// (1) multiply neg samples

        // moving these ARM instructions before neon above seems to be slower
        "subs           %[count], %[count], #4   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #8         \n"// (0) move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q10"
    );
}

template <>
inline void ProcessL<2, 8>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0               \n"// (1) acc_L = 0
        "veor           q4, q4, q4               \n"// (0 combines+) acc_R = 0

        "1:                                      \n"

        "vld2.16        {d4, d5}, [%[sP]]        \n"// (2+0d) load 8 16-bits stereo samples
        "vld2.16        {d6, d7}, [%[sN]]!       \n"// (2) load 8 16-bits stereo samples
        "vld1.16        {d16}, [%[coefsP0]:64]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {d20}, [%[coefsN0]:64]!  \n"// (1) load 8 16-bits coefs

        "vrev64.16      q2, q2                   \n"// (1) reverse 8 frames of the left positive

        "vmlal.s16      q0, d4, d16              \n"// (1) multiply (reversed) samples left
        "vmlal.s16      q4, d5, d16              \n"// (1) multiply (reversed) samples right
        "vmlal.s16      q0, d6, d20              \n"// (1) multiply samples left
        "vmlal.s16      q4, d7, d20              \n"// (1) multiply samples right

        // moving these ARM before neon seems to be slower
        "subs           %[count], %[count], #4   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #16        \n"// (0) move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out] "=Uv" (out[0]),
          [count] "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP] "+r" (sP),
          [sN] "+r" (sN)
        : [vLR] "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q4", "q5", "q6",
          "q8", "q10"
     );
}

template <>
inline void Process<1, 8>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* coefsP1,
        const int16_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]          \n"// load the positive phase S32 Q15
        "veor           q0, q0, q0               \n"// (0 - combines+) accumulator = 0

        "1:                                      \n"

        "vld1.16        {d4}, [%[sP]]            \n"// (2+0d) load 4 16-bits mono samples
        "vld1.16        {d6}, [%[sN]]!           \n"// (2) load 4 16-bits mono samples
        "vld1.16        {d16}, [%[coefsP0]:64]!  \n"// (1) load 4 16-bits coefs
        "vld1.16        {d17}, [%[coefsP1]:64]!  \n"// (1) load 4 16-bits coefs for interpolation
        "vld1.16        {d20}, [%[coefsN1]:64]!  \n"// (1) load 4 16-bits coefs
        "vld1.16        {d21}, [%[coefsN0]:64]!  \n"// (1) load 4 16-bits coefs for interpolation

        "vsub.s16       d17, d17, d16            \n"// (1) interpolate (step1) 1st set of coefs
        "vsub.s16       d21, d21, d20            \n"// (1) interpolate (step1) 2nd set of coets

        "vqrdmulh.s16   d17, d17, d2[0]          \n"// (2) interpolate (step2) 1st set of coefs
        "vqrdmulh.s16   d21, d21, d2[0]          \n"// (2) interpolate (step2) 2nd set of coefs

        "vrev64.16      d4, d4                   \n"// (1) reverse s3, s2, s1, s0, s7, s6, s5, s4

        "vadd.s16       d16, d16, d17            \n"// (1+2d) interpolate (step3) 1st set
        "vadd.s16       d20, d20, d21            \n"// (1+1d) interpolate (step3) 2nd set

        // reordering the vmal to do d6, d7 before d4, d5 is slower(?)
        "vmlal.s16      q0, d4, d16              \n"// (1+0d) multiply (reversed)by coef
        "vmlal.s16      q0, d6, d20              \n"// (1) multiply neg samples

        // moving these ARM instructions before neon above seems to be slower
        "subs           %[count], %[count], #4   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #8        \n"// move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [coefsP1] "+r" (coefsP1),
          [coefsN1] "+r" (coefsN1),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q9", "q10", "q11"
    );
}

template <>
inline void Process<2, 8>(int32_t* const out,
        int count,
        const int16_t* coefsP,
        const int16_t* coefsN,
        const int16_t* coefsP1,
        const int16_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]          \n"// load the positive phase
        "veor           q0, q0, q0               \n"// (1) acc_L = 0
        "veor           q4, q4, q4               \n"// (0 combines+) acc_R = 0

        "1:                                      \n"

        "vld2.16        {d4, d5}, [%[sP]]        \n"// (3+0d) load 8 16-bits stereo samples
        "vld2.16        {d6, d7}, [%[sN]]!       \n"// (3) load 8 16-bits stereo samples
        "vld1.16        {d16}, [%[coefsP0]:64]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {d17}, [%[coefsP1]:64]!  \n"// (1) load 8 16-bits coefs for interpolation
        "vld1.16        {d20}, [%[coefsN1]:64]!  \n"// (1) load 8 16-bits coefs
        "vld1.16        {d21}, [%[coefsN0]:64]!  \n"// (1) load 8 16-bits coefs for interpolation

        "vsub.s16       d17, d17, d16            \n"// (1) interpolate (step1) 1st set of coefs
        "vsub.s16       d21, d21, d20            \n"// (1) interpolate (step1) 2nd set of coets

        "vqrdmulh.s16   d17, d17, d2[0]          \n"// (2) interpolate (step2) 1st set of coefs
        "vqrdmulh.s16   d21, d21, d2[0]          \n"// (2) interpolate (step2) 2nd set of coefs

        "vrev64.16      q2, q2                   \n"// (1) reverse 8 frames of the left positive

        "vadd.s16       d16, d16, d17            \n"// (1+1d) interpolate (step3) 1st set
        "vadd.s16       d20, d20, d21            \n"// (1+1d) interpolate (step3) 2nd set

        "vmlal.s16      q0, d4, d16              \n"// (1) multiply (reversed) samples left
        "vmlal.s16      q4, d5, d16              \n"// (1) multiply (reversed) samples right
        "vmlal.s16      q0, d6, d20              \n"// (1) multiply samples left
        "vmlal.s16      q4, d7, d20              \n"// (1) multiply samples right

        // moving these ARM before neon seems to be slower
        "subs           %[count], %[count], #4   \n"// (1) update loop counter
        "sub            %[sP], %[sP], #16        \n"// move pointer to next set of samples

        // sP used after branch (warning)
        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out] "=Uv" (out[0]),
          [count] "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [coefsP1] "+r" (coefsP1),
          [coefsN1] "+r" (coefsN1),
          [sP] "+r" (sP),
          [sN] "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR] "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q4", "q5", "q6",
          "q8", "q9", "q10", "q11"
    );
}

template <>
inline void ProcessL<1, 8>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0               \n"// result, initialize to 0

        "1:                                      \n"

        "vld1.16        {d4}, [%[sP]]            \n"// load 4 16-bits mono samples
        "vld1.16        {d6}, [%[sN]]!           \n"// load 4 16-bits mono samples
        "vld1.32        {q8}, [%[coefsP0]:128]!  \n"// load 4 32-bits coefs
        "vld1.32        {q10}, [%[coefsN0]:128]! \n"// load 4 32-bits coefs

        "vrev64.16      d4, d4                   \n"// reverse 2 frames of the positive side

        "vshll.s16      q12, d4, #15             \n"// (stall) extend samples to 31 bits
        "vshll.s16      q14, d6, #15             \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q8             \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10            \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12              \n"// accumulate result
        "vadd.s32       q0, q0, q14              \n"// (stall) accumulate result

        "subs           %[count], %[count], #4   \n"// update loop counter
        "sub            %[sP], %[sP], #8         \n"// move pointer to next set of samples

        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out] "=Uv" (out[0]),
          [count] "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP] "+r" (sP),
          [sN] "+r" (sN)
        : [vLR] "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q9", "q10", "q11",
          "q12", "q14"
    );
}

template <>
inline void ProcessL<2, 8>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int16_t* sP,
        const int16_t* sN,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "veor           q0, q0, q0               \n"// result, initialize to 0
        "veor           q4, q4, q4               \n"// result, initialize to 0

        "1:                                      \n"

        "vld2.16        {d4, d5}, [%[sP]]        \n"// load 4 16-bits stereo samples
        "vld2.16        {d6, d7}, [%[sN]]!       \n"// load 4 16-bits stereo samples
        "vld1.32        {q8}, [%[coefsP0]:128]!  \n"// load 4 32-bits coefs
        "vld1.32        {q10}, [%[coefsN0]:128]! \n"// load 4 32-bits coefs

        "vrev64.16      q2, q2                   \n"// reverse 2 frames of the positive side

        "vshll.s16      q12, d4, #15             \n"// extend samples to 31 bits
        "vshll.s16      q13, d5, #15             \n"// extend samples to 31 bits

        "vshll.s16      q14, d6, #15             \n"// extend samples to 31 bits
        "vshll.s16      q15, d7, #15             \n"// extend samples to 31 bits

        "vqrdmulh.s32   q12, q12, q8             \n"// multiply samples by coef
        "vqrdmulh.s32   q13, q13, q8             \n"// multiply samples by coef
        "vqrdmulh.s32   q14, q14, q10            \n"// multiply samples by coef
        "vqrdmulh.s32   q15, q15, q10            \n"// multiply samples by coef

        "vadd.s32       q0, q0, q12              \n"// accumulate result
        "vadd.s32       q4, q4, q13              \n"// accumulate result
        "vadd.s32       q0, q0, q14              \n"// accumulate result
        "vadd.s32       q4, q4, q15              \n"// accumulate result

        "subs           %[count], %[count], #4   \n"// update loop counter
        "sub            %[sP], %[sP], #16        \n"// move pointer to next set of samples

        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsN0] "+r" (coefsN),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3", "q4",
          "q8", "q9", "q10", "q11",
          "q12", "q13", "q14", "q15"
    );
}

template <>
inline void Process<1, 8>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int32_t* coefsP1,
        const int32_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 1; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]          \n"// load the positive phase
        "veor           q0, q0, q0               \n"// result, initialize to 0

        "1:                                      \n"

        "vld1.16        {d4}, [%[sP]]            \n"// load 4 16-bits mono samples
        "vld1.16        {d6}, [%[sN]]!           \n"// load 4 16-bits mono samples
        "vld1.32        {q8}, [%[coefsP0]:128]!  \n"// load 4 32-bits coefs
        "vld1.32        {q9}, [%[coefsP1]:128]!  \n"// load 4 32-bits coefs for interpolation
        "vld1.32        {q10}, [%[coefsN1]:128]! \n"// load 4 32-bits coefs
        "vld1.32        {q11}, [%[coefsN0]:128]! \n"// load 4 32-bits coefs for interpolation

        "vrev64.16      d4, d4                   \n"// reverse 2 frames of the positive side

        "vsub.s32       q9, q9, q8               \n"// interpolate (step1) 1st set of coefs
        "vsub.s32       q11, q11, q10            \n"// interpolate (step1) 2nd set of coets
        "vshll.s16      q12, d4, #15             \n"// extend samples to 31 bits

        "vqrdmulh.s32   q9, q9, d2[0]            \n"// interpolate (step2) 1st set of coefs
        "vqrdmulh.s32   q11, q11, d2[0]          \n"// interpolate (step2) 2nd set of coefs
        "vshll.s16      q14, d6, #15             \n"// extend samples to 31 bits

        "vadd.s32       q8, q8, q9               \n"// interpolate (step3) 1st set
        "vadd.s32       q10, q10, q11            \n"// interpolate (step4) 2nd set

        "vqrdmulh.s32   q12, q12, q8             \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10            \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12              \n"// accumulate result
        "vadd.s32       q0, q0, q14              \n"// accumulate result

        "subs           %[count], %[count], #4   \n"// update loop counter
        "sub            %[sP], %[sP], #8         \n"// move pointer to next set of samples

        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_MONO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsP1] "+r" (coefsP1),
          [coefsN0] "+r" (coefsN),
          [coefsN1] "+r" (coefsN1),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3",
          "q8", "q9", "q10", "q11",
          "q12", "q14"
    );
}

template <>
inline
void Process<2, 8>(int32_t* const out,
        int count,
        const int32_t* coefsP,
        const int32_t* coefsN,
        const int32_t* coefsP1,
        const int32_t* coefsN1,
        const int16_t* sP,
        const int16_t* sN,
        uint32_t lerpP,
        const int32_t* const volumeLR)
{
    const int CHANNELS = 2; // template specialization does not preserve params
    const int STRIDE = 8;
    sP -= CHANNELS*((STRIDE>>1)-1);
    asm (
        "vmov.32        d2[0], %[lerpP]          \n"// load the positive phase
        "veor           q0, q0, q0               \n"// result, initialize to 0
        "veor           q4, q4, q4               \n"// result, initialize to 0

        "1:                                      \n"
        "vld2.16        {d4, d5}, [%[sP]]        \n"// load 4 16-bits stereo samples
        "vld2.16        {d6, d7}, [%[sN]]!       \n"// load 4 16-bits stereo samples
        "vld1.32        {q8}, [%[coefsP0]:128]!  \n"// load 4 32-bits coefs
        "vld1.32        {q9}, [%[coefsP1]:128]!  \n"// load 4 32-bits coefs for interpolation
        "vld1.32        {q10}, [%[coefsN1]:128]! \n"// load 4 32-bits coefs
        "vld1.32        {q11}, [%[coefsN0]:128]! \n"// load 4 32-bits coefs for interpolation

        "vrev64.16      q2, q2                   \n"// (reversed) 2 frames of the positive side

        "vsub.s32       q9, q9, q8               \n"// interpolate (step1) 1st set of coefs
        "vsub.s32       q11, q11, q10            \n"// interpolate (step1) 2nd set of coets
        "vshll.s16      q12, d4, #15             \n"// extend samples to 31 bits
        "vshll.s16      q13, d5, #15             \n"// extend samples to 31 bits

        "vqrdmulh.s32   q9, q9, d2[0]            \n"// interpolate (step2) 1st set of coefs
        "vqrdmulh.s32   q11, q11, d2[1]          \n"// interpolate (step3) 2nd set of coefs
        "vshll.s16      q14, d6, #15             \n"// extend samples to 31 bits
        "vshll.s16      q15, d7, #15             \n"// extend samples to 31 bits

        "vadd.s32       q8, q8, q9               \n"// interpolate (step3) 1st set
        "vadd.s32       q10, q10, q11            \n"// interpolate (step4) 2nd set

        "vqrdmulh.s32   q12, q12, q8             \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q13, q13, q8             \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q14, q14, q10            \n"// multiply samples by interpolated coef
        "vqrdmulh.s32   q15, q15, q10            \n"// multiply samples by interpolated coef

        "vadd.s32       q0, q0, q12              \n"// accumulate result
        "vadd.s32       q4, q4, q13              \n"// accumulate result
        "vadd.s32       q0, q0, q14              \n"// accumulate result
        "vadd.s32       q4, q4, q15              \n"// accumulate result

        "subs           %[count], %[count], #4   \n"// update loop counter
        "sub            %[sP], %[sP], #16        \n"// move pointer to next set of samples

        "bne            1b                       \n"// loop

        ASSEMBLY_ACCUMULATE_STEREO

        : [out]     "=Uv" (out[0]),
          [count]   "+r" (count),
          [coefsP0] "+r" (coefsP),
          [coefsP1] "+r" (coefsP1),
          [coefsN0] "+r" (coefsN),
          [coefsN1] "+r" (coefsN1),
          [sP]      "+r" (sP),
          [sN]      "+r" (sN)
        : [lerpP]   "r" (lerpP),
          [vLR]     "r" (volumeLR)
        : "cc", "memory",
          "q0", "q1", "q2", "q3", "q4",
          "q8", "q9", "q10", "q11",
          "q12", "q13", "q14", "q15"
    );
}

#endif //USE_NEON

}; // namespace android

#endif /*ANDROID_AUDIO_RESAMPLER_FIR_PROCESS_NEON_H*/
