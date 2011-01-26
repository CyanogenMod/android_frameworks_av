/*
 * Copyright (C) 2011 NXP Software
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_NDEBUG 1
#include <utils/Log.h>
#include "AudioMixer.h"
#include "VideoEditorResampler.h"

namespace android {

struct VideoEditorResampler : public AudioBufferProvider {

    public:

        virtual status_t getNextBuffer(Buffer* buffer);
        virtual void releaseBuffer(Buffer* buffer);

    enum { //Sampling freq
     kFreq8000Hz = 8000,
     kFreq11025Hz = 11025,
     kFreq12000Hz = 12000,
     kFreq16000Hz = 16000,
     kFreq22050Hz = 22050,
     kFreq240000Hz = 24000,
     kFreq32000Hz = 32000,
     kFreq44100 = 44100,
     kFreq48000 = 48000,
    };

    AudioResampler *mResampler;
    int16_t* mInput;
    int nbChannels;
    int nbSamples;

};


status_t VideoEditorResampler::getNextBuffer(AudioBufferProvider::Buffer *pBuffer) {

    pBuffer->raw = (void*)(this->mInput);
    return OK;
}

void VideoEditorResampler::releaseBuffer(AudioBufferProvider::Buffer *pBuffer) {

    if(pBuffer->raw != NULL) {
        pBuffer->raw = NULL;
    }
    pBuffer->frameCount = 0;
}

extern "C" {

M4OSA_Int32 LVAudioResamplerCreate(M4OSA_Int32 bitDepth, M4OSA_Int32 inChannelCount,
                                     M4OSA_Int32 sampleRate, M4OSA_Int32 quality) {

    VideoEditorResampler *context = new VideoEditorResampler();
    context->mResampler = AudioResampler::create(
        bitDepth, inChannelCount, sampleRate, AudioResampler::DEFAULT);
    if (context->mResampler == NULL) {
        return NO_MEMORY;
    }
    context->mResampler->setSampleRate(32000);
    context->mResampler->setVolume(0x1000, 0x1000);
    context->nbChannels = inChannelCount;

    return ((M4OSA_Int32)context);
}


void LVAudiosetSampleRate(M4OSA_Int32 resamplerContext, M4OSA_Int32 inSampleRate) {

    VideoEditorResampler *context =
      (VideoEditorResampler *)resamplerContext;
    context->mResampler->setSampleRate(inSampleRate);
    /*
     * nbSamples is calculated for 40ms worth of data;hence sample rate
     * is used to calculate the nbSamples
     */
    context->nbSamples = inSampleRate / 25;
    context->mInput = (int16_t*)malloc(context->nbSamples *
                                   context->nbChannels * sizeof(int16_t));
}

void LVAudiosetVolume(M4OSA_Int32 resamplerContext, M4OSA_Int16 left, M4OSA_Int16 right) {

    VideoEditorResampler *context =
       (VideoEditorResampler *)resamplerContext;
    context->mResampler->setVolume(left,right);
}


void LVAudioresample_LowQuality(M4OSA_Int16* out, M4OSA_Int16* input,
                                     M4OSA_Int32 outFrameCount, M4OSA_Int32 resamplerContext) {

    VideoEditorResampler *context =
      (VideoEditorResampler *)resamplerContext;
    int32_t *pTmpBuffer = NULL;
    memcpy(context->mInput,input,(context->nbSamples * context->nbChannels * sizeof(int16_t)));
    /*
     SRC module always gives stereo output, hence 2 for stereo audio
    */
    pTmpBuffer = (int32_t*)malloc(outFrameCount * 2 * sizeof(int32_t));
    memset(pTmpBuffer, 0x00, outFrameCount * 2 * sizeof(int32_t));

    context->mResampler->resample((int32_t *)pTmpBuffer,
       (size_t)outFrameCount, (VideoEditorResampler *)resamplerContext);
    // Convert back to 16 bits
    AudioMixer::ditherAndClamp((int32_t*)out, pTmpBuffer, outFrameCount);
    free(pTmpBuffer);
    pTmpBuffer = NULL;
}

}

} //namespace android
