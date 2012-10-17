/*
 * Copyright (C) 2007 Google Inc.
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

#include <math.h>
#include <stdio.h>

static double sinc(double x) {
    if (fabs(x) == 0.0f) return 1.0f;
    return sin(x) / x;
}

static double sqr(double x) {
    return x*x;
}

static double I0(double x) {
    // from the Numerical Recipes in C p. 237
    double ax,ans,y;
    ax=fabs(x);
    if (ax < 3.75) {
        y=x/3.75;
        y*=y;
        ans=1.0+y*(3.5156229+y*(3.0899424+y*(1.2067492
            +y*(0.2659732+y*(0.360768e-1+y*0.45813e-2)))));
    } else {
        y=3.75/ax;
        ans=(exp(ax)/sqrt(ax))*(0.39894228+y*(0.1328592e-1
            +y*(0.225319e-2+y*(-0.157565e-2+y*(0.916281e-2
            +y*(-0.2057706e-1+y*(0.2635537e-1+y*(-0.1647633e-1
            +y*0.392377e-2))))))));
    }
    return ans;
}

static double kaiser(int k, int N, double alpha) {
    if (k < 0 || k > N)
        return 0;
    return I0(M_PI*alpha * sqrt(1.0 - sqr((2.0*k)/N - 1.0))) / I0(M_PI*alpha);
}

int main(int argc, char** argv)
{
    // nc is the number of bits to store the coefficients
    int nc = 16;

    // ni is the minimum number of bits needed for interpolation
    // (not used for generating the coefficients)
    const int ni = nc / 2;

    // nzc is the number of zero-crossing on one half of the filter
    int nzc = 12;
    
    // alpha parameter of the kaiser window
    // Larger numbers reduce ripples in the rejection band but increase
    // the width of the transition band. In reality there doesn't seem to be
    // a good reason to choose a big number because of the limited range
    // of our coefficients (16 bits).
    double alpha = 3.0;
    
    // cut off frequency ratio Fc/Fs
    double Fcr = 20000.0 / 44100.0;
    
    // 2^nz is the number coefficients per zero-crossing
    // (int theory this should be 1<<(nc/2))
    const int nz = 4;
    
    // total number of coefficients
    const int N = (1 << 4) * nzc;

    // generate the right half of the filter

    printf("const int32_t RESAMPLE_FIR_SIZE           = %d;\n", N);
    printf("const int32_t RESAMPLE_FIR_NUM_COEF       = %d;\n", nzc);
    printf("const int32_t RESAMPLE_FIR_COEF_BITS      = %d;\n", nc);
    printf("const int32_t RESAMPLE_FIR_LERP_FRAC_BITS = %d;\n", ni);
    printf("const int32_t RESAMPLE_FIR_LERP_INT_BITS  = %d;\n", nz);
    printf("\n");
    printf("static int16_t resampleFIR[%d] = {", N);
    for (int i=0 ; i<N ; i++)
    {
        double x = (2.0 * M_PI * i * Fcr) / (1 << 4);
        double y = kaiser(i+N, 2*N, alpha) * sinc(x);

        int yi = floor(y * (1<<(nc-1)) + 0.5);
        if (yi >= (1<<(nc-1))) yi = (1<<(nc-1))-1;        

        //printf("%6d\n", yi);

        if ((i % (1 << 4)) == 0) printf("\n    ");
        printf("0x%04x, ", yi & 0xFFFF);
    }
    printf("\n};\n");

    return 0;
 }
 