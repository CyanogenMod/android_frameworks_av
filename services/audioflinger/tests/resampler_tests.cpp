/*
 * Copyright (C) 2014 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "audioflinger_resampler_tests"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <vector>
#include <utility>
#include <cutils/log.h>
#include <gtest/gtest.h>
#include <media/AudioBufferProvider.h>
#include "AudioResampler.h"
#include "test_utils.h"

void resample(int channels, void *output,
        size_t outputFrames, const std::vector<size_t> &outputIncr,
        android::AudioBufferProvider *provider, android::AudioResampler *resampler)
{
    for (size_t i = 0, j = 0; i < outputFrames; ) {
        size_t thisFrames = outputIncr[j++];
        if (j >= outputIncr.size()) {
            j = 0;
        }
        if (thisFrames == 0 || thisFrames > outputFrames - i) {
            thisFrames = outputFrames - i;
        }
        resampler->resample((int32_t*) output + channels*i, thisFrames, provider);
        i += thisFrames;
    }
}

void buffercmp(const void *reference, const void *test,
        size_t outputFrameSize, size_t outputFrames)
{
    for (size_t i = 0; i < outputFrames; ++i) {
        int check = memcmp((const char*)reference + i * outputFrameSize,
                (const char*)test + i * outputFrameSize, outputFrameSize);
        if (check) {
            ALOGE("Failure at frame %d", i);
            ASSERT_EQ(check, 0); /* fails */
        }
    }
}

void testBufferIncrement(size_t channels, bool useFloat,
        unsigned inputFreq, unsigned outputFreq,
        enum android::AudioResampler::src_quality quality)
{
    const audio_format_t format = useFloat ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT;
    // create the provider
    std::vector<int> inputIncr;
    SignalProvider provider;
    if (useFloat) {
        provider.setChirp<float>(channels,
                0., outputFreq/2., outputFreq, outputFreq/2000.);
    } else {
        provider.setChirp<int16_t>(channels,
                0., outputFreq/2., outputFreq, outputFreq/2000.);
    }
    provider.setIncr(inputIncr);

    // calculate the output size
    size_t outputFrames = ((int64_t) provider.getNumFrames() * outputFreq) / inputFreq;
    size_t outputFrameSize = channels * (useFloat ? sizeof(float) : sizeof(int32_t));
    size_t outputSize = outputFrameSize * outputFrames;
    outputSize &= ~7;

    // create the resampler
    const int volumePrecision = 12; /* typical unity gain */
    android::AudioResampler* resampler;

    resampler = android::AudioResampler::create(format, channels, outputFreq, quality);
    resampler->setSampleRate(inputFreq);
    resampler->setVolume(1 << volumePrecision, 1 << volumePrecision);

    // set up the reference run
    std::vector<size_t> refIncr;
    refIncr.push_back(outputFrames);
    void* reference = malloc(outputSize);
    resample(channels, reference, outputFrames, refIncr, &provider, resampler);

    provider.reset();

#if 0
    /* this test will fail - API interface issue: reset() does not clear internal buffers */
    resampler->reset();
#else
    delete resampler;
    resampler = android::AudioResampler::create(format, channels, outputFreq, quality);
    resampler->setSampleRate(inputFreq);
    resampler->setVolume(1 << volumePrecision, 1 << volumePrecision);
#endif

    // set up the test run
    std::vector<size_t> outIncr;
    outIncr.push_back(1);
    outIncr.push_back(2);
    outIncr.push_back(3);
    void* test = malloc(outputSize);
    inputIncr.push_back(1);
    inputIncr.push_back(3);
    provider.setIncr(inputIncr);
    resample(channels, test, outputFrames, outIncr, &provider, resampler);

    // check
    buffercmp(reference, test, outputFrameSize, outputFrames);

    free(reference);
    free(test);
    delete resampler;
}

template <typename T>
inline double sqr(T v)
{
    double dv = static_cast<double>(v);
    return dv * dv;
}

template <typename T>
double signalEnergy(T *start, T *end, unsigned stride)
{
    double accum = 0;

    for (T *p = start; p < end; p += stride) {
        accum += sqr(*p);
    }
    unsigned count = (end - start + stride - 1) / stride;
    return accum / count;
}

void testStopbandDownconversion(size_t channels,
        unsigned inputFreq, unsigned outputFreq,
        unsigned passband, unsigned stopband,
        enum android::AudioResampler::src_quality quality)
{
    // create the provider
    std::vector<int> inputIncr;
    SignalProvider provider;
    provider.setChirp<int16_t>(channels,
            0., inputFreq/2., inputFreq, inputFreq/2000.);
    provider.setIncr(inputIncr);

    // calculate the output size
    size_t outputFrames = ((int64_t) provider.getNumFrames() * outputFreq) / inputFreq;
    size_t outputFrameSize = channels * sizeof(int32_t);
    size_t outputSize = outputFrameSize * outputFrames;
    outputSize &= ~7;

    // create the resampler
    const int volumePrecision = 12; /* typical unity gain */
    android::AudioResampler* resampler;

    resampler = android::AudioResampler::create(AUDIO_FORMAT_PCM_16_BIT,
            channels, outputFreq, quality);
    resampler->setSampleRate(inputFreq);
    resampler->setVolume(1 << volumePrecision, 1 << volumePrecision);

    // set up the reference run
    std::vector<size_t> refIncr;
    refIncr.push_back(outputFrames);
    void* reference = malloc(outputSize);
    resample(channels, reference, outputFrames, refIncr, &provider, resampler);

    int32_t *out = reinterpret_cast<int32_t *>(reference);

    // check signal energy in passband
    const unsigned passbandFrame = passband * outputFreq / 1000.;
    const unsigned stopbandFrame = stopband * outputFreq / 1000.;

    // check each channel separately
    for (size_t i = 0; i < channels; ++i) {
        double passbandEnergy = signalEnergy(out, out + passbandFrame * channels, channels);
        double stopbandEnergy = signalEnergy(out + stopbandFrame * channels,
                out + outputFrames * channels, channels);
        double dbAtten = -10. * log10(stopbandEnergy / passbandEnergy);
        ASSERT_GT(dbAtten, 60.);

#if 0
        // internal verification
        printf("if:%d  of:%d  pbf:%d  sbf:%d  sbe: %f  pbe: %f  db: %.2f\n",
                provider.getNumFrames(), outputFrames,
                passbandFrame, stopbandFrame, stopbandEnergy, passbandEnergy, dbAtten);
        for (size_t i = 0; i < 10; ++i) {
            printf("%d\n", out[i+passbandFrame*channels]);
        }
        for (size_t i = 0; i < 10; ++i) {
            printf("%d\n", out[i+stopbandFrame*channels]);
        }
#endif
    }

    free(reference);
    delete resampler;
}

/* Buffer increment test
 *
 * We compare a reference output, where we consume and process the entire
 * buffer at a time, and a test output, where we provide small chunks of input
 * data and process small chunks of output (which may not be equivalent in size).
 *
 * Two subtests - fixed phase (3:2 down) and interpolated phase (147:320 up)
 */
TEST(audioflinger_resampler, bufferincrement_fixedphase) {
    // all of these work
    static const enum android::AudioResampler::src_quality kQualityArray[] = {
            android::AudioResampler::LOW_QUALITY,
            android::AudioResampler::MED_QUALITY,
            android::AudioResampler::HIGH_QUALITY,
            android::AudioResampler::VERY_HIGH_QUALITY,
            android::AudioResampler::DYN_LOW_QUALITY,
            android::AudioResampler::DYN_MED_QUALITY,
            android::AudioResampler::DYN_HIGH_QUALITY,
    };

    for (size_t i = 0; i < ARRAY_SIZE(kQualityArray); ++i) {
        testBufferIncrement(2, false, 48000, 32000, kQualityArray[i]);
    }
}

TEST(audioflinger_resampler, bufferincrement_interpolatedphase) {
    // all of these work except low quality
    static const enum android::AudioResampler::src_quality kQualityArray[] = {
//           android::AudioResampler::LOW_QUALITY,
            android::AudioResampler::MED_QUALITY,
            android::AudioResampler::HIGH_QUALITY,
            android::AudioResampler::VERY_HIGH_QUALITY,
            android::AudioResampler::DYN_LOW_QUALITY,
            android::AudioResampler::DYN_MED_QUALITY,
            android::AudioResampler::DYN_HIGH_QUALITY,
    };

    for (size_t i = 0; i < ARRAY_SIZE(kQualityArray); ++i) {
        testBufferIncrement(2, false, 22050, 48000, kQualityArray[i]);
    }
}

TEST(audioflinger_resampler, bufferincrement_fixedphase_multi) {
    // only dynamic quality
    static const enum android::AudioResampler::src_quality kQualityArray[] = {
            android::AudioResampler::DYN_LOW_QUALITY,
            android::AudioResampler::DYN_MED_QUALITY,
            android::AudioResampler::DYN_HIGH_QUALITY,
    };

    for (size_t i = 0; i < ARRAY_SIZE(kQualityArray); ++i) {
        testBufferIncrement(4, false, 48000, 32000, kQualityArray[i]);
    }
}

TEST(audioflinger_resampler, bufferincrement_interpolatedphase_multi_float) {
    // only dynamic quality
    static const enum android::AudioResampler::src_quality kQualityArray[] = {
            android::AudioResampler::DYN_LOW_QUALITY,
            android::AudioResampler::DYN_MED_QUALITY,
            android::AudioResampler::DYN_HIGH_QUALITY,
    };

    for (size_t i = 0; i < ARRAY_SIZE(kQualityArray); ++i) {
        testBufferIncrement(8, true, 22050, 48000, kQualityArray[i]);
    }
}

/* Simple aliasing test
 *
 * This checks stopband response of the chirp signal to make sure frequencies
 * are properly suppressed.  It uses downsampling because the stopband can be
 * clearly isolated by input frequencies exceeding the output sample rate (nyquist).
 */
TEST(audioflinger_resampler, stopbandresponse) {
    // not all of these may work (old resamplers fail on downsampling)
    static const enum android::AudioResampler::src_quality kQualityArray[] = {
            //android::AudioResampler::LOW_QUALITY,
            //android::AudioResampler::MED_QUALITY,
            //android::AudioResampler::HIGH_QUALITY,
            //android::AudioResampler::VERY_HIGH_QUALITY,
            android::AudioResampler::DYN_LOW_QUALITY,
            android::AudioResampler::DYN_MED_QUALITY,
            android::AudioResampler::DYN_HIGH_QUALITY,
    };

    // in this test we assume a maximum transition band between 12kHz and 20kHz.
    // there must be at least 60dB relative attenuation between stopband and passband.
    for (size_t i = 0; i < ARRAY_SIZE(kQualityArray); ++i) {
        testStopbandDownconversion(2, 48000, 32000, 12000, 20000, kQualityArray[i]);
    }

    // in this test we assume a maximum transition band between 7kHz and 15kHz.
    // there must be at least 60dB relative attenuation between stopband and passband.
    // (the weird ratio triggers interpolative resampling)
    for (size_t i = 0; i < ARRAY_SIZE(kQualityArray); ++i) {
        testStopbandDownconversion(2, 48000, 22101, 7000, 15000, kQualityArray[i]);
    }
}
