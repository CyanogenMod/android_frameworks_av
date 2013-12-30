/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "AudioResampler.h"
#include <media/AudioBufferProvider.h>
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
#include <audio_utils/sndfile.h>

using namespace android;

bool gVerbose = false;

static int usage(const char* name) {
    fprintf(stderr,"Usage: %s [-p] [-h] [-v] [-s] [-q {dq|lq|mq|hq|vhq|dlq|dmq|dhq}]"
                   " [-i input-sample-rate] [-o output-sample-rate] [<input-file>]"
                   " <output-file>\n", name);
    fprintf(stderr,"    -p    enable profiling\n");
    fprintf(stderr,"    -h    create wav file\n");
    fprintf(stderr,"    -v    verbose : log buffer provider calls\n");
    fprintf(stderr,"    -s    stereo (ignored if input file is specified)\n");
    fprintf(stderr,"    -q    resampler quality\n");
    fprintf(stderr,"              dq  : default quality\n");
    fprintf(stderr,"              lq  : low quality\n");
    fprintf(stderr,"              mq  : medium quality\n");
    fprintf(stderr,"              hq  : high quality\n");
    fprintf(stderr,"              vhq : very high quality\n");
    fprintf(stderr,"              dlq : dynamic low quality\n");
    fprintf(stderr,"              dmq : dynamic medium quality\n");
    fprintf(stderr,"              dhq : dynamic high quality\n");
    fprintf(stderr,"    -i    input file sample rate (ignored if input file is specified)\n");
    fprintf(stderr,"    -o    output file sample rate\n");
    return -1;
}

int main(int argc, char* argv[]) {

    const char* const progname = argv[0];
    bool profiling = false;
    bool writeHeader = false;
    int channels = 1;
    int input_freq = 0;
    int output_freq = 0;
    AudioResampler::src_quality quality = AudioResampler::DEFAULT_QUALITY;

    int ch;
    while ((ch = getopt(argc, argv, "phvsq:i:o:")) != -1) {
        switch (ch) {
        case 'p':
            profiling = true;
            break;
        case 'h':
            writeHeader = true;
            break;
        case 'v':
            gVerbose = true;
            break;
        case 's':
            channels = 2;
            break;
        case 'q':
            if (!strcmp(optarg, "dq"))
                quality = AudioResampler::DEFAULT_QUALITY;
            else if (!strcmp(optarg, "lq"))
                quality = AudioResampler::LOW_QUALITY;
            else if (!strcmp(optarg, "mq"))
                quality = AudioResampler::MED_QUALITY;
            else if (!strcmp(optarg, "hq"))
                quality = AudioResampler::HIGH_QUALITY;
            else if (!strcmp(optarg, "vhq"))
                quality = AudioResampler::VERY_HIGH_QUALITY;
            else if (!strcmp(optarg, "dlq"))
                quality = AudioResampler::DYN_LOW_QUALITY;
            else if (!strcmp(optarg, "dmq"))
                quality = AudioResampler::DYN_MED_QUALITY;
            else if (!strcmp(optarg, "dhq"))
                quality = AudioResampler::DYN_HIGH_QUALITY;
            else {
                usage(progname);
                return -1;
            }
            break;
        case 'i':
            input_freq = atoi(optarg);
            break;
        case 'o':
            output_freq = atoi(optarg);
            break;
        case '?':
        default:
            usage(progname);
            return -1;
        }
    }
    argc -= optind;
    argv += optind;

    const char* file_in = NULL;
    const char* file_out = NULL;
    if (argc == 1) {
        file_out = argv[0];
    } else if (argc == 2) {
        file_in = argv[0];
        file_out = argv[1];
    } else {
        usage(progname);
        return -1;
    }

    // ----------------------------------------------------------

    size_t input_size;
    void* input_vaddr;
    if (argc == 2) {
        SF_INFO info;
        info.format = 0;
        SNDFILE *sf = sf_open(file_in, SFM_READ, &info);
        if (sf == NULL) {
            perror(file_in);
            return EXIT_FAILURE;
        }
        input_size = info.frames * info.channels * sizeof(short);
        input_vaddr = malloc(input_size);
        (void) sf_readf_short(sf, (short *) input_vaddr, info.frames);
        sf_close(sf);
        channels = info.channels;
        input_freq = info.samplerate;
    } else {
        // data for testing is exactly (input sampling rate/1000)/2 seconds
        // so 44.1khz input is 22.05 seconds
        double k = 1000; // Hz / s
        double time = (input_freq / 2) / k;
        size_t input_frames = size_t(input_freq * time);
        input_size = channels * sizeof(int16_t) * input_frames;
        input_vaddr = malloc(input_size);
        int16_t* in = (int16_t*)input_vaddr;
        for (size_t i=0 ; i<input_frames ; i++) {
            double t = double(i) / input_freq;
            double y = sin(M_PI * k * t * t);
            int16_t yi = floor(y * 32767.0 + 0.5);
            for (size_t j=0 ; j<(size_t)channels ; j++) {
                in[i*channels + j] = yi / (1+j); // right ch. 1/2 left ch.
            }
        }
    }

    // ----------------------------------------------------------

    class Provider: public AudioBufferProvider {
        int16_t* const  mAddr;      // base address
        const size_t    mNumFrames; // total frames
        const int       mChannels;
        size_t          mNextFrame; // index of next frame to provide
        size_t          mUnrel;     // number of frames not yet released
    public:
        Provider(const void* addr, size_t size, int channels)
          : mAddr((int16_t*) addr),
            mNumFrames(size / (channels*sizeof(int16_t))),
            mChannels(channels),
            mNextFrame(0), mUnrel(0) {
        }
        virtual status_t getNextBuffer(Buffer* buffer,
                int64_t pts = kInvalidPTS) {
            (void)pts; // suppress warning
            size_t requestedFrames = buffer->frameCount;
            if (requestedFrames > mNumFrames - mNextFrame) {
                buffer->frameCount = mNumFrames - mNextFrame;
            }
            if (gVerbose) {
                printf("getNextBuffer() requested %u frames out of %u frames available,"
                        " and returned %u frames\n",
                        requestedFrames, mNumFrames - mNextFrame, buffer->frameCount);
            }
            mUnrel = buffer->frameCount;
            if (buffer->frameCount > 0) {
                buffer->i16 = &mAddr[mChannels * mNextFrame];
                return NO_ERROR;
            } else {
                buffer->i16 = NULL;
                return NOT_ENOUGH_DATA;
            }
        }
        virtual void releaseBuffer(Buffer* buffer) {
            if (buffer->frameCount > mUnrel) {
                fprintf(stderr, "ERROR releaseBuffer() released %u frames but only %u available "
                        "to release\n", buffer->frameCount, mUnrel);
                mNextFrame += mUnrel;
                mUnrel = 0;
            } else {
                if (gVerbose) {
                    printf("releaseBuffer() released %u frames out of %u frames available "
                            "to release\n", buffer->frameCount, mUnrel);
                }
                mNextFrame += buffer->frameCount;
                mUnrel -= buffer->frameCount;
            }
            buffer->frameCount = 0;
            buffer->i16 = NULL;
        }
        void reset() {
            mNextFrame = 0;
        }
    } provider(input_vaddr, input_size, channels);

    size_t input_frames = input_size / (channels * sizeof(int16_t));
    if (gVerbose) {
        printf("%u input frames\n", input_frames);
    }
    size_t output_size = 2 * 4 * ((int64_t) input_frames * output_freq) / input_freq;
    output_size &= ~7; // always stereo, 32-bits

    void* output_vaddr = malloc(output_size);
    AudioResampler* resampler = AudioResampler::create(16, channels,
            output_freq, quality);
    size_t out_frames = output_size/8;
    resampler->setSampleRate(input_freq);
    resampler->setVolume(0x1000, 0x1000);

    if (profiling) {
        const int looplimit = 100;
        timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < looplimit; ++i) {
            resampler->resample((int*) output_vaddr, out_frames, &provider);
            provider.reset(); //  reset only provider as benchmarking
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        int64_t start_ns = start.tv_sec * 1000000000LL + start.tv_nsec;
        int64_t end_ns = end.tv_sec * 1000000000LL + end.tv_nsec;
        int64_t time = end_ns - start_ns;
        printf("time(ns):%lld  channels:%d  quality:%d\n", time, channels, quality);
        printf("%f Mspl/s\n", out_frames * looplimit / (time / 1e9) / 1e6);
        resampler->reset();
    }

    memset(output_vaddr, 0, output_size);
    if (gVerbose) {
        printf("resample() %u output frames\n", out_frames);
    }
    resampler->resample((int*) output_vaddr, out_frames, &provider);
    if (gVerbose) {
        printf("resample() complete\n");
    }
    resampler->reset();
    if (gVerbose) {
        printf("reset() complete\n");
    }

    // mono takes left channel only
    // stereo right channel is half amplitude of stereo left channel (due to input creation)
    int32_t* out = (int32_t*) output_vaddr;
    int16_t* convert = (int16_t*) malloc(out_frames * channels * sizeof(int16_t));

    for (size_t i = 0; i < out_frames; i++) {
        for (int j = 0; j < channels; j++) {
            int32_t s = out[i * 2 + j] >> 12;
            if (s > 32767)
                s = 32767;
            else if (s < -32768)
                s = -32768;
            convert[i * channels + j] = int16_t(s);
        }
    }

    // write output to disk
    if (writeHeader) {
        SF_INFO info;
        info.frames = 0;
        info.samplerate = output_freq;
        info.channels = channels;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        SNDFILE *sf = sf_open(file_out, SFM_WRITE, &info);
        if (sf == NULL) {
            perror(file_out);
            return EXIT_FAILURE;
        }
        (void) sf_writef_short(sf, convert, out_frames);
        sf_close(sf);
    } else {
        int output_fd = open(file_out, O_WRONLY | O_CREAT | O_TRUNC,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (output_fd < 0) {
            perror(file_out);
            return EXIT_FAILURE;
        }
        write(output_fd, convert, out_frames * channels * sizeof(int16_t));
        close(output_fd);
    }

    return EXIT_SUCCESS;
}
