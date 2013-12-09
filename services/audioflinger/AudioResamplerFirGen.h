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

#ifndef ANDROID_AUDIO_RESAMPLER_FIR_GEN_H
#define ANDROID_AUDIO_RESAMPLER_FIR_GEN_H

namespace android {

/*
 * Sinc function is the traditional variant.
 *
 * TODO: Investigate optimizations (regular sampling grid, NEON vector accelerations)
 * TODO: Remove comparison at 0 and trap at a higher level.
 *
 */

static inline double sinc(double x) {
    if (fabs(x) < FLT_MIN) {
        return 1.;
    }
    return sin(x) / x;
}

static inline double sqr(double x) {
    return x * x;
}

/*
 * rounds a double to the nearest integer for FIR coefficients.
 *
 * One variant uses noise shaping, which must keep error history
 * to work (the err parameter, initialized to 0).
 * The other variant is a non-noise shaped version for
 * S32 coefficients (noise shaping doesn't gain much).
 *
 * Caution: No bounds saturation is applied, but isn't needed in
 * this case.
 *
 * @param x is the value to round.
 *
 * @param maxval is the maximum integer scale factor expressed as an int64 (for headroom).
 * Typically this may be the maximum positive integer+1 (using the fact that double precision
 * FIR coefficients generated here are never that close to 1.0 to pose an overflow condition).
 *
 * @param err is the previous error (actual - rounded) for the previous rounding op.
 *
 */

static inline int64_t toint(double x, int64_t maxval, double& err) {
    double val = x * maxval;
    double ival = floor(val + 0.5 + err*0.17);
    err = val - ival;
    return static_cast<int64_t>(ival);
}

static inline int64_t toint(double x, int64_t maxval) {
    return static_cast<int64_t>(floor(x * maxval + 0.5));
}

/*
 * Modified Bessel function of the first kind
 * http://en.wikipedia.org/wiki/Bessel_function
 *
 * The formulas are taken from Abramowitz and Stegun:
 *
 * http://people.math.sfu.ca/~cbm/aands/page_375.htm
 * http://people.math.sfu.ca/~cbm/aands/page_378.htm
 *
 * http://dlmf.nist.gov/10.25
 * http://dlmf.nist.gov/10.40
 *
 * Note we assume x is nonnegative (the function is symmetric,
 * pass in the absolute value as needed).
 *
 * Constants are compile time derived with templates I0Term<> and
 * I0ATerm<> to the precision of the compiler.  The series can be expanded
 * to any precision needed, but currently set around 24b precision.
 *
 * We use a bit of template math here, constexpr would probably be
 * more appropriate for a C++11 compiler.
 *
 */

template <int N>
struct I0Term {
    static const double value = I0Term<N-1>::value/ (4. * N * N);
};

template <>
struct I0Term<0> {
    static const double value = 1.;
};

template <int N>
struct I0ATerm {
    static const double value = I0ATerm<N-1>::value * (2.*N-1.) * (2.*N-1.) / (8. * N);
};

template <>
struct I0ATerm<0> { // 1/sqrt(2*PI);
    static const double value = 0.398942280401432677939946059934381868475858631164934657665925;
};

static inline double I0(double x) {
    if (x < 3.75) { // TODO: Estrin's method instead of Horner's method?
        x *= x;
        return I0Term<0>::value + x*(
                I0Term<1>::value + x*(
                I0Term<2>::value + x*(
                I0Term<3>::value + x*(
                I0Term<4>::value + x*(
                I0Term<5>::value + x*(
                I0Term<6>::value)))))); // e < 1.6e-7
    }
    // a bit ugly here - perhaps we expand the top series
    // to permit computation to x < 20 (a reasonable range)
    double y = 1./x;
    return exp(x) * sqrt(y) * (
            // note: reciprocal squareroot may be easier!
            // http://en.wikipedia.org/wiki/Fast_inverse_square_root
            I0ATerm<0>::value + y*(
            I0ATerm<1>::value + y*(
            I0ATerm<2>::value + y*(
            I0ATerm<3>::value + y*(
            I0ATerm<4>::value + y*(
            I0ATerm<5>::value + y*(
            I0ATerm<6>::value + y*(
            I0ATerm<7>::value + y*(
            I0ATerm<8>::value))))))))); // (... e) < 1.9e-7
}

/*
 * calculates the transition bandwidth for a Kaiser filter
 *
 * Formula 3.2.8, Multirate Systems and Filter Banks, PP Vaidyanathan, pg. 48
 *
 * @param halfNumCoef is half the number of coefficients per filter phase.
 * @param stopBandAtten is the stop band attenuation desired.
 * @return the transition bandwidth in normalized frequency (0 <= f <= 0.5)
 */
static inline double firKaiserTbw(int halfNumCoef, double stopBandAtten) {
    return (stopBandAtten - 7.95)/(2.*14.36*halfNumCoef);
}

/*
 * calculates the fir transfer response.
 *
 * calculates the transfer coefficient H(w) for 0 <= w <= PI.
 * Be careful be careful to consider the fact that this is an interpolated filter
 * of length L, so normalizing H(w)/L is probably what you expect.
 */
template <typename T>
static inline double firTransfer(const T* coef, int L, int halfNumCoef, double w) {
    double accum = static_cast<double>(coef[0])*0.5;
    coef += halfNumCoef;    // skip first row.
    for (int i=1 ; i<=L ; ++i) {
        for (int j=0, ix=i ; j<halfNumCoef ; ++j, ix+=L) {
            accum += cos(ix*w)*static_cast<double>(*coef++);
        }
    }
    return accum*2.;
}

/*
 * returns the minimum and maximum |H(f)| bounds
 *
 * @param coef is the designed polyphase filter banks
 *
 * @param L is the number of phases (for interpolation)
 *
 * @param halfNumCoef should be half the number of coefficients for a single
 * polyphase.
 *
 * @param fstart is the normalized frequency start.
 *
 * @param fend is the normalized frequency end.
 *
 * @param steps is the number of steps to take (sampling) between frequency start and end
 *
 * @param firMin returns the minimum transfer |H(f)| found
 *
 * @param firMax returns the maximum transfer |H(f)| found
 *
 * 0 <= f <= 0.5.
 * This is used to test passband and stopband performance.
 */
template <typename T>
static void testFir(const T* coef, int L, int halfNumCoef,
        double fstart, double fend, int steps, double &firMin, double &firMax) {
    double wstart = fstart*(2.*M_PI);
    double wend = fend*(2.*M_PI);
    double wstep = (wend - wstart)/steps;
    double fmax, fmin;
    double trf = firTransfer(coef, L, halfNumCoef, wstart);
    if (trf<0) {
        trf = -trf;
    }
    fmin = fmax = trf;
    wstart += wstep;
    for (int i=1; i<steps; ++i) {
        trf = firTransfer(coef, L, halfNumCoef, wstart);
        if (trf<0) {
            trf = -trf;
        }
        if (trf>fmax) {
            fmax = trf;
        }
        else if (trf<fmin) {
            fmin = trf;
        }
        wstart += wstep;
    }
    // renormalize - this is only needed for integer filter types
    double norm = 1./((1ULL<<(sizeof(T)*8-1))*L);

    firMin = fmin * norm;
    firMax = fmax * norm;
}

/*
 * Calculates the polyphase filter banks based on a windowed sinc function.
 *
 * The windowed sinc is an odd length symmetric filter of exactly L*halfNumCoef*2+1
 * taps for the entire kernel.  This is then decomposed into L+1 polyphase filterbanks.
 * The last filterbank is used for interpolation purposes (and is mostly composed
 * of the first bank shifted by one sample), and is unnecessary if one does
 * not do interpolation.
 *
 * @param coef is the caller allocated space for coefficients.  This should be
 * exactly (L+1)*halfNumCoef in size.
 *
 * @param L is the number of phases (for interpolation)
 *
 * @param halfNumCoef should be half the number of coefficients for a single
 * polyphase.
 *
 * @param stopBandAtten is the stopband value, should be >50dB.
 *
 * @param fcr is cutoff frequency/sampling rate (<0.5).  At this point, the energy
 * should be 6dB less. (fcr is where the amplitude drops by half).  Use the
 * firKaiserTbw() to calculate the transition bandwidth.  fcr is the midpoint
 * between the stop band and the pass band (fstop+fpass)/2.
 *
 * @param atten is the attenuation (generally slightly less than 1).
 */

template <typename T>
static inline void firKaiserGen(T* coef, int L, int halfNumCoef,
        double stopBandAtten, double fcr, double atten) {
    //
    // Formula 3.2.5, 3.2.7, Multirate Systems and Filter Banks, PP Vaidyanathan, pg. 48
    //
    // See also: http://melodi.ee.washington.edu/courses/ee518/notes/lec17.pdf
    //
    // Kaiser window and beta parameter
    //
    //         | 0.1102*(A - 8.7)                         A > 50
    //  beta = | 0.5842*(A - 21)^0.4 + 0.07886*(A - 21)   21 <= A <= 50
    //         | 0.                                       A < 21
    //
    // with A is the desired stop-band attenuation in dBFS
    //
    //    30 dB    2.210
    //    40 dB    3.384
    //    50 dB    4.538
    //    60 dB    5.658
    //    70 dB    6.764
    //    80 dB    7.865
    //    90 dB    8.960
    //   100 dB   10.056

    const int N = L * halfNumCoef; // non-negative half
    const double beta = 0.1102 * (stopBandAtten - 8.7); // >= 50dB always
    const double yscale = 2. * atten * fcr / I0(beta);
    const double xstep = 2. * M_PI * fcr / L;
    const double xfrac = 1. / N;
    double err = 0; // for noise shaping on int16_t coefficients
    for (int i=0 ; i<=L ; ++i) { // generate an extra set of coefs for interpolation
        for (int j=0, ix=i ; j<halfNumCoef ; ++j, ix+=L) {
            double y = I0(beta * sqrt(1.0 - sqr(ix * xfrac))) * sinc(ix * xstep) * yscale;

            // (caution!) float version does not need rounding
            if (is_same<T, int16_t>::value) { // int16_t needs noise shaping
                *coef++ = static_cast<T>(toint(y, 1ULL<<(sizeof(T)*8-1), err));
            } else {
                *coef++ = static_cast<T>(toint(y, 1ULL<<(sizeof(T)*8-1)));
            }
        }
    }
}

}; // namespace android

#endif /*ANDROID_AUDIO_RESAMPLER_FIR_GEN_H*/
