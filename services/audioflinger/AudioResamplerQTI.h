/*
 * Copyright (C) 2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 * Copyright (C) 2007 The Android Open Source Project
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

#include <stdint.h>
#include <sys/types.h>
#include <cutils/log.h>

#include "AudioResampler.h"

namespace android {
// ----------------------------------------------------------------------------

class AudioResamplerQTI : public AudioResampler {
public:
    AudioResamplerQTI(int format, int inChannelCount, int32_t sampleRate);
    ~AudioResamplerQTI();
    size_t resample(int32_t* out, size_t outFrameCount,
                  AudioBufferProvider* provider);
    void setSampleRate(int32_t inSampleRate);
    size_t getNumInSample(size_t outFrameCount);

    int16_t *mState;
    int32_t *mTmpBuf;
    int32_t *mResamplerOutBuf;
    size_t mFrameIndex;
    size_t stateSize;
    size_t mOutFrameCount;

    static const int kNumTmpBufSize = 1024;

    void init();
    void reset();
};

// ----------------------------------------------------------------------------
}; // namespace android

