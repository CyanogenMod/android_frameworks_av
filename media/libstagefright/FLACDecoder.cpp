/*Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "FLACDecoder"
#include <utils/Log.h>

#include <dlfcn.h>  // for dlopen/dlclose

#ifdef ENABLE_AV_ENHANCEMENTS
#include <QCMetaData.h>
#endif

#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaBufferGroup.h>
#include "include/FLACDecoder.h"

namespace android {

typedef void* (*DecoderInit) (CFlacDecState* pFlacDecState, int* nRes);

typedef int* (*DecoderLib_Process) (CFlacDecState* pFlacDecState, uint8* pInBitStream,
                                    uint32 nActualDataLen, void *pOutSamples,
                                    uint32* uFlacOutputBufSize, uint32* usedBitstream,
                                    uint32* blockSize, uint32* bytesInInternalBuffer);

typedef void* (*SetMetaData) (CFlacDecState* pFlacDecState,
                      FLACDec_ParserInfo* parserInfoToPass);

static const char* FLAC_DECODER_LIB = "libFlacSwDec.so";

FLACDecoder::FLACDecoder(const sp<MediaSource> &source)
    : mSource(source), mInputBuffer(NULL),
      mStarted(false), mBufferGroup(NULL),
      mNumFramesOutput(0), mAnchorTimeUs(0) {
    init();
}

FLACDecoder::~FLACDecoder() {
    if (mStarted) {
        stop();
    }
}

void FLACDecoder::init() {
    ALOGV("FLACDecoder::init");
    int nRes;
    memset(&pFlacDecState,0,sizeof(CFlacDecState));
    decoderInit(&pFlacDecState, &nRes);

    sp<MetaData> srcFormat = mSource->getFormat();

    mMeta = new MetaData;

    int32_t sampleBits, minBlkSize, maxBlkSize, minFrmSize, maxFrmSize;
    CHECK(srcFormat->findInt32(kKeyChannelCount, &mNumChannels));
    CHECK(srcFormat->findInt32(kKeySampleRate, &mSampleRate));
    CHECK(srcFormat->findInt32(kKeySampleBits, &sampleBits));
    CHECK(srcFormat->findInt32(kKeyMinBlkSize, &minBlkSize));
    CHECK(srcFormat->findInt32(kKeyMaxBlkSize, &maxBlkSize));
    CHECK(srcFormat->findInt32(kKeyMinFrmSize, &minFrmSize));
    CHECK(srcFormat->findInt32(kKeyMaxFrmSize, &maxFrmSize));

    parserInfoToPass.i32NumChannels = mNumChannels;
    parserInfoToPass.i32SampleRate = mSampleRate;
    parserInfoToPass.i32BitsPerSample = sampleBits;
    parserInfoToPass.i32MinBlkSize = minBlkSize;
    parserInfoToPass.i32MaxBlkSize = maxBlkSize;
    parserInfoToPass.i32MinFrmSize = minFrmSize;
    parserInfoToPass.i32MaxFrmSize = maxFrmSize;

    ALOGV("i32NumChannels = %d", parserInfoToPass.i32NumChannels);
    ALOGV("i32SampleRate = %d", parserInfoToPass.i32SampleRate);
    ALOGV("i32BitsPerSample = %d", parserInfoToPass.i32BitsPerSample);
    ALOGV("i32MinBlkSize = %d", parserInfoToPass.i32MinBlkSize);
    ALOGV("i32MaxBlkSize = %d", parserInfoToPass.i32MaxBlkSize);
    ALOGV("i32MinFrmSize = %d", parserInfoToPass.i32MinFrmSize);
    ALOGV("i32MaxFrmSize = %d", parserInfoToPass.i32MaxFrmSize);

    setMetaData(&pFlacDecState, &parserInfoToPass);

    mMeta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

    int64_t durationUs;
    if (srcFormat->findInt64(kKeyDuration, &durationUs)) {
        mMeta->setInt64(kKeyDuration, durationUs);
    }

    ALOGV("durationUs = %lld", durationUs);
    mMeta->setCString(kKeyDecoderComponent, "FLACDecoder");
    mMeta->setInt32(kKeySampleRate, mSampleRate);
    mMeta->setInt32(kKeyChannelCount, mNumChannels);
}

status_t FLACDecoder::start(MetaData *params) {
    ALOGV("FLACDecoder::start");

    CHECK(!mStarted);

    mBufferGroup = new MediaBufferGroup;
    mBufferGroup->add_buffer(new MediaBuffer(MAXINPBUFFER));

    mSource->start();
    mAnchorTimeUs = 0;
    mNumFramesOutput = 0;
    mStarted = true;

    return OK;
}

status_t FLACDecoder::stop() {
    ALOGV("FLACDecoder::stop");

    CHECK(mStarted);
    if (mInputBuffer) {
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    delete mBufferGroup;
    mBufferGroup = NULL;

    mSource->stop();
    mStarted = false;

    return OK;
}

sp<MetaData> FLACDecoder::getFormat() {
    ALOGV("FLACDecoder::getFormat");
    return mMeta;
}


static void* loadFlacDecoderLib() {
    static void* flacDecoderLib = NULL;
    static bool alreadyTriedToLoadLib = false;

    if (!alreadyTriedToLoadLib) {
        alreadyTriedToLoadLib = true;
        ALOGV("Opening the FLAC Decoder Library");
        flacDecoderLib = ::dlopen(FLAC_DECODER_LIB, RTLD_LAZY);

        if (flacDecoderLib == NULL) {
            ALOGE("Failed to load %s, dlerror = %s",
                FLAC_DECODER_LIB, dlerror());
        }
    }

    return flacDecoderLib;
}

void FLACDecoder::decoderInit(CFlacDecState* pFlacDecState, int* nRes) {
    static DecoderInit decoder_init = NULL;
    void *flacDecoderLib = loadFlacDecoderLib();

    if (flacDecoderLib != NULL) {
        *(void **)(&decoder_init) = dlsym(flacDecoderLib, "CFlacDecoderLib_Meminit");

        decoder_init(pFlacDecState, nRes);

        if ((uint32) *nRes != (uint32) DEC_SUCCESS) {
            ALOGE("Decoder init failed! nRes %d", *nRes);
        }
    }
}

void FLACDecoder::setMetaData(CFlacDecState* pFlacDecState,
                      FLACDec_ParserInfo* parserInfoToPass) {
    static SetMetaData set_meta_data = NULL;
    void *flacDecoderLib = loadFlacDecoderLib();

    if (flacDecoderLib != NULL) {
        *(void **)(&set_meta_data) = dlsym(flacDecoderLib, "CFlacDecoderLib_SetMetaData");
        set_meta_data(pFlacDecState, parserInfoToPass);
    }
}

int* FLACDecoder::decoderLib_Process(CFlacDecState* pFlacDecState, uint8* pInBitStream,
                             uint32 nActualDataLen, void *pOutSamples,
                             uint32* uFlacOutputBufSize, uint32* usedBitstream,
                             uint32* blockSize, uint32* bytesInInternalBuffer) {
    static DecoderLib_Process decoderlib_process = NULL;
    int* status = 0;
    void *flacDecoderLib = loadFlacDecoderLib();

    if (flacDecoderLib != NULL) {
        *(void **)(&decoderlib_process) = dlsym(flacDecoderLib, "CFlacDecoderLib_Process");

        status = decoderlib_process(pFlacDecState, pInBitStream, nActualDataLen,
                                    pOutSamples, uFlacOutputBufSize, usedBitstream,
                                    blockSize, bytesInInternalBuffer);
    }
    return status;
}

status_t FLACDecoder::read(MediaBuffer **out, const ReadOptions* options) {
    int err = 0;
    *out = NULL;
    uint32 blockSize, usedBitstream, availLength = 0;
    uint32 flacOutputBufSize = FLAC_OUTPUT_BUFFER_SIZE;
    uint32 instSize = FLAC_INSTANCE_SIZE;
    int *status = 0;

    bool seekSource = false, eos = false;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        ALOGV("Seek to %lld", seekTimeUs);
        CHECK(seekTimeUs >= 0);
        mNumFramesOutput = 0;
        seekSource = true;

        if (mInputBuffer) {
            mInputBuffer->release();
            mInputBuffer = NULL;
        }
    }
    else {
        seekTimeUs = -1;
    }

    if (mInputBuffer) {
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    if (!eos) {
        err = mSource->read(&mInputBuffer, options);
        if (err != OK) {
            ALOGV("Parser returned %d", err);
            eos = true;
            return err;
        }
    }

    int64_t timeUs;
    if (mInputBuffer->meta_data()->findInt64(kKeyTime, &timeUs)) {
        mAnchorTimeUs = timeUs;
        mNumFramesOutput = 0;
        ALOGV("mAnchorTimeUs %lld", mAnchorTimeUs);
    }
    else {
        CHECK(seekTimeUs < 0);
    }

    void *pOutBuffer = NULL;
    pOutBuffer = (int8*) malloc (instSize);

    if (!eos) {
        if (mInputBuffer) {
            ALOGV("Parser filled %d bytes", mInputBuffer->range_length());
            availLength = mInputBuffer->range_length();
            status = decoderLib_Process(&pFlacDecState,
                                        (uint8*)mInputBuffer->data(),
                                        availLength,
                                        pOutBuffer,
                                        &flacOutputBufSize,
                                        &usedBitstream,
                                        &blockSize,
                                        &(pFlacDecState.bytesInInternalBuffer));
        }

        ALOGV("decoderlib_process returned %d, availLength %d, usedBitstream %d,\
               blockSize %d, bytesInInternalBuffer %d", (int)status, availLength,
               usedBitstream, blockSize, pFlacDecState.bytesInInternalBuffer);

        MediaBuffer *buffer = new MediaBuffer(blockSize*mNumChannels*2);

        uint16_t *tmpbuf = (uint16_t *) malloc (blockSize*mNumChannels*2);

        uint16_t *ptr = (uint16_t *) pOutBuffer;

        //Interleave the output from decoder for stereo clips.
        if (mNumChannels > 1) {
            for (uint16_t i = 0, j = 0; i < blockSize*2; i += 2, j++) {
                tmpbuf[i] = ptr[j];
                tmpbuf[i+1] = ptr[j+(blockSize)];
            }
            memcpy((uint16_t *)buffer->data(), tmpbuf, blockSize*mNumChannels*2);
        }
        else {
            memcpy((uint16_t *)buffer->data(), pOutBuffer, blockSize*mNumChannels*2);
        }

        int64_t time = 0;
        time = mAnchorTimeUs + (mNumFramesOutput*1000000)/mSampleRate;
        buffer->meta_data()->setInt64(kKeyTime, time);
        mNumFramesOutput += blockSize;
        ALOGV("time = %lld", time);

        *out = buffer;
        ALOGV("out->range = %d", buffer->range_length());
        free(tmpbuf);
        free(pOutBuffer);

    }

    return OK;
}

}
