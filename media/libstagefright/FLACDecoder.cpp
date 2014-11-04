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

#define LOG_TAG "FLACDecoder"
//#define LOG_NDEBUG 0
//#define VERY_VERY_VERBOSE_LOGGING
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while (0)
#endif

#include <utils/Log.h>
#include <dlfcn.h>

#ifdef ENABLE_AV_ENHANCEMENTS
#include <QCMetaData.h>
#endif

#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaBufferGroup.h>
#include "include/FLACDecoder.h"

namespace android {

static const char* FLAC_DECODER_LIB = "libFlacSwDec.so";

FLACDecoder::FLACDecoder(const sp<MediaSource> &source)
    : mSource(source),
      mInputBuffer(NULL),
      mStarted(false),
      mInitStatus(false),
      mBufferGroup(NULL),
      mNumFramesOutput(0),
      mAnchorTimeUs(0),
      mLibHandle(dlopen(FLAC_DECODER_LIB, RTLD_LAZY)),
      mOutBuffer(NULL),
      mDecoderInit(NULL),
      mProcessData(NULL) {
    ALOGD("qti_flac: Instantiate FLACDecoder");
    if (mLibHandle != NULL) {
        mDecoderInit = (DecoderInit) dlsym (mLibHandle, "CFlacDecoderLib_Meminit");
        mProcessData = (DecoderLib_Process) dlsym (mLibHandle, "CFlacDecoderLib_Process");
        init();
    }
}

FLACDecoder::~FLACDecoder() {
    ALOGD("qti_flac: Destroy FLACDecoder");
    if (mStarted) {
        stop();
    }
    if (mLibHandle != NULL) {
        dlclose(mLibHandle);
    }
    mLibHandle = NULL;
}

void FLACDecoder::init() {
    ALOGV("qti_flac: FLACDecoder::init");
    int result, bitWidth = 16; //currently, only 16 bit is supported
    memset(&pFlacDecState,0,sizeof(CFlacDecState));
    (*mDecoderInit)(&pFlacDecState, &result, bitWidth);

    if (result != DEC_SUCCESS) {
        ALOGE("qti_flac: CSIM decoder init failed! Result %d", result);
        return;
    }
    else {
        mInitStatus = true;
    }

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

    ALOGV("qti_flac: i32NumChannels = %d", parserInfoToPass.i32NumChannels);
    ALOGV("qti_flac: i32SampleRate = %d", parserInfoToPass.i32SampleRate);
    ALOGV("qti_flac: i32BitsPerSample = %d", parserInfoToPass.i32BitsPerSample);
    ALOGV("qti_flac: i32MinBlkSize = %d", parserInfoToPass.i32MinBlkSize);
    ALOGV("qti_flac: i32MaxBlkSize = %d", parserInfoToPass.i32MaxBlkSize);
    ALOGV("qti_flac: i32MinFrmSize = %d", parserInfoToPass.i32MinFrmSize);
    ALOGV("qti_flac: i32MaxFrmSize = %d", parserInfoToPass.i32MaxFrmSize);

    setMetaData(&pFlacDecState, &parserInfoToPass);

    mMeta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

    int64_t durationUs;
    if (srcFormat->findInt64(kKeyDuration, &durationUs)) {
        mMeta->setInt64(kKeyDuration, durationUs);
    }

    ALOGV("qti_flac: durationUs = %lld", durationUs);
    mMeta->setCString(kKeyDecoderComponent, "FLACDecoder");
    mMeta->setInt32(kKeySampleRate, mSampleRate);
    mMeta->setInt32(kKeyChannelCount, mNumChannels);

    ALOGV("qti_flac: FLACDecoder::init done");
}

void FLACDecoder::setMetaData(CFlacDecState* pFlacDecState,
                              FLACDec_ParserInfo* parserInfoToPass) {
    ALOGV("qti_flac: FLACDecoder::setMetadata");
    stFLACDec* pstFLACDec=(stFLACDec*)(pFlacDecState->m_pFlacDecoder);
    memcpy(&pstFLACDec->MetaDataBlocks.MetaDataStrmInfo,parserInfoToPass,sizeof(FLACDec_ParserInfo));
    pFlacDecState->m_bIsStreamInfoPresent=1;

    pFlacDecState->ui32MaxBlockSize=pstFLACDec->MetaDataBlocks.MetaDataStrmInfo.i32MaxBlkSize;

    memcpy(pFlacDecState->pFlacDecMetaDataStrmInfo,parserInfoToPass,sizeof(FLACDec_ParserInfo));
    ALOGV("qti_flac: FLACDecoder::setMetadata done");

}

status_t FLACDecoder::start(MetaData *params) {
    ALOGV("qti_flac: FLACDecoder::start");

    CHECK(!mStarted);
    CHECK(mInitStatus);

    mBufferGroup = new MediaBufferGroup;
    mBufferGroup->add_buffer(new MediaBuffer(FLAC_INSTANCE_SIZE));

    mOutBuffer = (uint16_t *) malloc (FLAC_INSTANCE_SIZE);
    mTmpBuf = (uint16_t *) malloc (FLAC_INSTANCE_SIZE);

    mSource->start();
    mAnchorTimeUs = 0;
    mNumFramesOutput = 0;
    mStarted = true;

    ALOGV("qti_flac: FLACDecoder::start done");
    return OK;
}

status_t FLACDecoder::stop() {
    ALOGV("qti_flac: FLACDecoder::stop");

    CHECK(mStarted);
    CHECK(mInitStatus);

    if (mInputBuffer) {
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    delete mBufferGroup;
    mBufferGroup = NULL;

    mSource->stop();
    mStarted = false;

    if (mOutBuffer) {
        free(mOutBuffer);
        mOutBuffer = NULL;
    }
    if (mTmpBuf) {
        free(mTmpBuf);
        mTmpBuf = NULL;
    }

    ALOGV("qti_flac: FLACDecoder::stop done");
    return OK;
}

sp<MetaData> FLACDecoder::getFormat() {
    ALOGV("qti_flac: FLACDecoder::getFormat");
    CHECK(mInitStatus);
    ALOGV("qti_flac: FLACDecoder::getFormat done");
    return mMeta;
}

status_t FLACDecoder::read(MediaBuffer **out, const ReadOptions* options) {
    int err = 0;
    *out = NULL;
    uint32 blockSize, usedBitstream, availLength = 0;
    uint32 flacOutputBufSize = FLAC_OUTPUT_BUFFER_SIZE;
    int *status = 0;

    bool seekSource = false, eos = false;

    CHECK(mStarted);
    if (!mInitStatus) {
        return NO_INIT;
    }

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        ALOGD("qti_flac: Seek to %lld", seekTimeUs);
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
            ALOGE("qti_flac: Parser returned %d", err);
            eos = true;
            return err;
        }
    }

    int64_t timeUs;
    if (mInputBuffer->meta_data()->findInt64(kKeyTime, &timeUs)) {
        mAnchorTimeUs = timeUs;
        mNumFramesOutput = 0;
        ALOGVV("qti_flac: mAnchorTimeUs %lld", mAnchorTimeUs);
    }
    else {
        CHECK(seekTimeUs < 0);
    }

    if (!eos) {
        if (mInputBuffer) {
            ALOGVV("qti_flac: Parser filled %d bytes", mInputBuffer->range_length());
            availLength = mInputBuffer->range_length();
            status = (*mProcessData)(&pFlacDecState,
                                     (uint8*)mInputBuffer->data(),
                                     availLength,
                                     mOutBuffer,
                                     &flacOutputBufSize,
                                     &usedBitstream,
                                     &blockSize);
        }

        ALOGVV("qti_flac: status %d, availLength %d, usedBitstream %d, blockSize %d",
                (int)status, availLength, usedBitstream, blockSize);

        MediaBuffer *buffer;
        CHECK_EQ(mBufferGroup->acquire_buffer(&buffer), (status_t)OK);

        buffer->set_range(0, blockSize*mNumChannels*2);

        uint16_t *ptr = (uint16_t *) mOutBuffer;

        //Interleave the output from decoder for multichannel clips.
        if (mNumChannels > 1) {
            for (uint32_t k = 0; k < blockSize; k++) {
                for (uint32_t i = k, j = mNumChannels*k; i < blockSize*mNumChannels; i += blockSize, j++) {
                    mTmpBuf[j] = ptr[i];
                }
            }
            memcpy((uint16_t *)buffer->data(), mTmpBuf, blockSize*mNumChannels*2);
        }
        else {
            memcpy((uint16_t *)buffer->data(), mOutBuffer, blockSize*mNumChannels*2);
        }

        int64_t time = 0;
        time = mAnchorTimeUs + (mNumFramesOutput*1000000)/mSampleRate;
        buffer->meta_data()->setInt64(kKeyTime, time);
        mNumFramesOutput += blockSize;
        ALOGVV("qti_flac: time = %lld", time);

        *out = buffer;
    }

    return OK;
}

}
