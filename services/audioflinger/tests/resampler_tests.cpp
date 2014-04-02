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

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

template<typename T, typename U>
struct is_same
{
    static const bool value = false;
};

template<typename T>
struct is_same<T, T>  // partial specialization
{
    static const bool value = true;
};

template<typename T>
static inline T convertValue(double val)
{
    if (is_same<T, int16_t>::value) {
        return floor(val * 32767.0 + 0.5);
    } else if (is_same<T, int32_t>::value) {
        return floor(val * (1UL<<31) + 0.5);
    }
    return val; // assume float or double
}

/* Creates a type-independent audio buffer provider from
 * a buffer base address, size, framesize, and input increment array.
 *
 * No allocation or deallocation of the provided buffer is done.
 */
class TestProvider : public android::AudioBufferProvider {
public:
    TestProvider(const void* addr, size_t frames, size_t frameSize,
            const std::vector<size_t>& inputIncr)
    : mAddr(addr),
      mNumFrames(frames),
      mFrameSize(frameSize),
      mNextFrame(0), mUnrel(0), mInputIncr(inputIncr), mNextIdx(0)
    {
    }

    virtual android::status_t getNextBuffer(Buffer* buffer, int64_t pts __unused = kInvalidPTS )
    {
        size_t requestedFrames = buffer->frameCount;
        if (requestedFrames > mNumFrames - mNextFrame) {
            buffer->frameCount = mNumFrames - mNextFrame;
        }
        if (!mInputIncr.empty()) {
            size_t provided = mInputIncr[mNextIdx++];
            ALOGV("getNextBuffer() mValue[%d]=%u not %u",
                    mNextIdx-1, provided, buffer->frameCount);
            if (provided < buffer->frameCount) {
                buffer->frameCount = provided;
            }
            if (mNextIdx >= mInputIncr.size()) {
                mNextIdx = 0;
            }
        }
        ALOGV("getNextBuffer() requested %u frames out of %u frames available"
                " and returned %u frames\n",
                requestedFrames, mNumFrames - mNextFrame, buffer->frameCount);
        mUnrel = buffer->frameCount;
        if (buffer->frameCount > 0) {
            buffer->raw = (char *)mAddr + mFrameSize * mNextFrame;
            return android::NO_ERROR;
        } else {
            buffer->raw = NULL;
            return android::NOT_ENOUGH_DATA;
        }
    }

    virtual void releaseBuffer(Buffer* buffer)
    {
        if (buffer->frameCount > mUnrel) {
            ALOGE("releaseBuffer() released %u frames but only %u available "
                    "to release\n", buffer->frameCount, mUnrel);
            mNextFrame += mUnrel;
            mUnrel = 0;
        } else {

            ALOGV("releaseBuffer() released %u frames out of %u frames available "
                    "to release\n", buffer->frameCount, mUnrel);
            mNextFrame += buffer->frameCount;
            mUnrel -= buffer->frameCount;
        }
        buffer->frameCount = 0;
        buffer->raw = NULL;
    }

    void reset()
    {
        mNextFrame = 0;
    }

    size_t getNumFrames()
    {
        return mNumFrames;
    }

    void setIncr(const std::vector<size_t> inputIncr)
    {
        mNextIdx = 0;
        mInputIncr = inputIncr;
    }

protected:
    const void* mAddr;   // base address
    size_t mNumFrames;   // total frames
    int mFrameSize;      // frame size (# channels * bytes per sample)
    size_t mNextFrame;   // index of next frame to provide
    size_t mUnrel;       // number of frames not yet released
    std::vector<size_t> mInputIncr; // number of frames provided per call
    size_t mNextIdx;     // index of next entry in mInputIncr to use
};

/* Creates a buffer filled with a sine wave.
 *
 * Returns a pair consisting of the sine signal buffer and the number of frames.
 * The caller must delete[] the buffer when no longer needed (no shared_ptr<>).
 */
template<typename T>
static std::pair<T*, size_t> createSine(size_t channels,
        double freq, double samplingRate, double time)
{
    double tscale = 1. / samplingRate;
    size_t frames = static_cast<size_t>(samplingRate * time);
    T* buffer = new T[frames * channels];
    for (size_t i = 0; i < frames; ++i) {
        double t = i * tscale;
        double y = sin(2. * M_PI * freq * t);
        T yt = convertValue<T>(y);

        for (size_t j = 0; j < channels; ++j) {
            buffer[i*channels + j] = yt / (j + 1);
        }
    }
    return std::make_pair(buffer, frames);
}

/* Creates a buffer filled with a chirp signal (a sine wave sweep).
 *
 * Returns a pair consisting of the chirp signal buffer and the number of frames.
 * The caller must delete[] the buffer when no longer needed (no shared_ptr<>).
 *
 * When creating the Chirp, note that the frequency is the true sinusoidal
 * frequency not the sampling rate.
 *
 * http://en.wikipedia.org/wiki/Chirp
 */
template<typename T>
static std::pair<T*, size_t> createChirp(size_t channels,
        double minfreq, double maxfreq, double samplingRate, double time)
{
    double tscale = 1. / samplingRate;
    size_t frames = static_cast<size_t>(samplingRate * time);
    T *buffer = new T[frames * channels];
    // note the chirp constant k has a divide-by-two.
    double k = (maxfreq - minfreq) / (2. * time);
    for (size_t i = 0; i < frames; ++i) {
        double t = i * tscale;
        double y = sin(2. * M_PI * (k * t + minfreq) * t);
        T yt = convertValue<T>(y);

        for (size_t j = 0; j < channels; ++j) {
            buffer[i*channels + j] = yt / (j + 1);
        }
    }
    return std::make_pair(buffer, frames);
}

/* This derived class creates a buffer provider of datatype T,
 * consisting of an input signal, e.g. from createChirp().
 * The number of frames can be obtained from the base class
 * TestProvider::getNumFrames().
 */
template <typename T>
class SignalProvider : public TestProvider {
public:
    SignalProvider(const std::pair<T*, size_t>& bufferInfo, size_t channels,
            const std::vector<size_t>& values)
    : TestProvider(bufferInfo.first, bufferInfo.second, channels * sizeof(T), values),
      mManagedPtr(bufferInfo.first)
    {
    }

    virtual ~SignalProvider()
    {
        delete[] mManagedPtr;
    }

protected:
    T* mManagedPtr;
};

void resample(void *output, size_t outputFrames, const std::vector<size_t> &outputIncr,
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
        resampler->resample((int32_t*) output + 2*i, thisFrames, provider);
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

void testBufferIncrement(size_t channels, unsigned inputFreq, unsigned outputFreq,
        enum android::AudioResampler::src_quality quality)
{
    // create the provider
    std::vector<size_t> inputIncr;
    SignalProvider<int16_t> provider(createChirp<int16_t>(channels,
            0., outputFreq/2., outputFreq, outputFreq/2000.),
            channels, inputIncr);

    // calculate the output size
    size_t outputFrames = ((int64_t) provider.getNumFrames() * outputFreq) / inputFreq;
    size_t outputFrameSize = 2 * sizeof(int32_t);
    size_t outputSize = outputFrameSize * outputFrames;
    outputSize &= ~7;

    // create the resampler
    const int volumePrecision = 12; /* typical unity gain */
    android::AudioResampler* resampler;

    resampler = android::AudioResampler::create(16, channels, outputFreq, quality);
    resampler->setSampleRate(inputFreq);
    resampler->setVolume(1 << volumePrecision, 1 << volumePrecision);

    // set up the reference run
    std::vector<size_t> refIncr;
    refIncr.push_back(outputFrames);
    void* reference = malloc(outputSize);
    resample(reference, outputFrames, refIncr, &provider, resampler);

    provider.reset();

#if 0
    /* this test will fail - API interface issue: reset() does not clear internal buffers */
    resampler->reset();
#else
    delete resampler;
    resampler = android::AudioResampler::create(16, channels, outputFreq, quality);
    resampler->setSampleRate(inputFreq);
    resampler->setVolume(1 << volumePrecision, 1 << volumePrecision);
#endif

    // set up the test run
    std::vector<size_t> outIncr;
    outIncr.push_back(1);
    outIncr.push_back(2);
    outIncr.push_back(3);
    void* test = malloc(outputSize);
    resample(test, outputFrames, outIncr, &provider, resampler);

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
    std::vector<size_t> inputIncr;
    SignalProvider<int16_t> provider(createChirp<int16_t>(channels,
            0., inputFreq/2., inputFreq, inputFreq/2000.),
            channels, inputIncr);

    // calculate the output size
    size_t outputFrames = ((int64_t) provider.getNumFrames() * outputFreq) / inputFreq;
    size_t outputFrameSize = 2 * sizeof(int32_t);
    size_t outputSize = outputFrameSize * outputFrames;
    outputSize &= ~7;

    // create the resampler
    const int volumePrecision = 12; /* typical unity gain */
    android::AudioResampler* resampler;

    resampler = android::AudioResampler::create(16, channels, outputFreq, quality);
    resampler->setSampleRate(inputFreq);
    resampler->setVolume(1 << volumePrecision, 1 << volumePrecision);

    // set up the reference run
    std::vector<size_t> refIncr;
    refIncr.push_back(outputFrames);
    void* reference = malloc(outputSize);
    resample(reference, outputFrames, refIncr, &provider, resampler);

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
        testBufferIncrement(2, 48000, 32000, kQualityArray[i]);
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
        testBufferIncrement(2, 22050, 48000, kQualityArray[i]);
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
