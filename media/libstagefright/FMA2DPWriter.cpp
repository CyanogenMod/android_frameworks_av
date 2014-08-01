/*
 * Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "FMA2DPWriter"
#include <utils/Log.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/FMA2DPWriter.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/mediarecorder.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#include <media/AudioRecord.h>
#include <media/AudioTrack.h>

namespace android {

#define BUFFER_POOL_SIZE 5
#define MAX_BUFFER_SIZE 2048

FMA2DPWriter::FMA2DPWriter()
    :mStarted(false),
    mAudioChannels(0),
    mSampleRate(0),
    mAudioFormat(AUDIO_FORMAT_PCM_16_BIT),
    mAudioSource(AUDIO_SOURCE_FM_RX_A2DP),
    mBufferSize(0) {
}

FMA2DPWriter::~FMA2DPWriter() {
    if (mStarted) {
        stop();
    }
}

status_t FMA2DPWriter::initCheck() const {
    // API not need for FMA2DPWriter
    return OK;
}

status_t FMA2DPWriter::addSource(const sp<MediaSource> &source) {
   // API not need for FMA2DPWriter
   return OK;
}

status_t FMA2DPWriter::allocateBufferPool() {
    Mutex::Autolock lock(mLock);

    for (int i = 0; i < BUFFER_POOL_SIZE; ++i) {
        int *buffer = new int[mBufferSize];
        if (buffer) {
            FMData *fmBuffer = new FMData(buffer, mBufferSize);
            mFMDataPool.add(fmBuffer);
            mFreeList.push_back(i);
        } else {
            ALOGE("%s, fatal: failed to alloate buffer pool. Deleting partially created mFMDataPool", __func__);
            for ( Vector<FMData *>::iterator it = mFMDataPool.begin();
                  it != mFMDataPool.end();) {
                int *tempBuffer = (int *)((*it)->audioBuffer);
                it = mFMDataPool.erase(it);
                delete tempBuffer;
            }
            mFreeList.clear();
            return  NO_INIT;
        }
    }
    return OK;
}

status_t FMA2DPWriter::start(MetaData *params) {
    ALOGV("%s Entered", __func__);
    if (mStarted) {
        // Already started, does nothing
        return OK;
    }

    if(!mStarted) {
        if(params) {
            params->findInt32(kKeyChannelCount, &mAudioChannels);
            params->findInt32(kKeySampleRate, &mSampleRate);
        }
        if (0 == mAudioChannels) {
             ALOGD("%s set default channel count:%", __func__, mAudioChannels);
             mAudioChannels = AUDIO_CHANNELS;
        }
        if (0 == mSampleRate) {
             ALOGD("%s set default sample rate:%", __func__, SAMPLING_RATE);
             mSampleRate = SAMPLING_RATE;
        }

        if ( NO_ERROR != AudioSystem::getInputBufferSize(
                    mSampleRate, mAudioFormat, mAudioChannels, &mBufferSize) ){
            mBufferSize = MAX_BUFFER_SIZE;
        }
        ALOGV("%s mBufferSize = %d", __func__, mBufferSize);
    }

    status_t err = allocateBufferPool();

    if(err != OK)
        return err;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    mDone = false;

    pthread_create(&mReaderThread, &attr, ReaderThreadWrapper, this);
    pthread_create(&mWriterThread, &attr, WriterThreadWrapper, this);

    pthread_attr_destroy(&attr);

    mStarted = true;
    ALOGV("%s Exit", __func__);
    return OK;
}

status_t FMA2DPWriter::pause() {
    // API not need for FMA2DPWriter
    return OK;
}

status_t FMA2DPWriter::stop() {
    ALOGV("%s Enter", __func__);
    if (!mStarted) {
        return OK;
    }

    {
        Mutex::Autolock _l(mLock);
        ALOGV("Exiting");
        mDone = true;
        mCondVar.signal();
    }

    pthread_join(mReaderThread, NULL);
    pthread_join(mWriterThread, NULL);
    for (Vector<FMData *>::iterator it = mFMDataPool.begin();
                                            it != mFMDataPool.end();) {
        int *tempBuffer = (int *)((*it)->audioBuffer);
        it = mFMDataPool.erase(it);
        delete tempBuffer;
    }
    mFreeList.clear();
    mDataList.clear();
    mStarted = false;
    ALOGV("%s Exit", __func__);
    return OK;
}

void *FMA2DPWriter::ReaderThreadWrapper(void *me) {
    return (void *) (uintptr_t)static_cast<FMA2DPWriter *>(me)->readerthread();
}

void *FMA2DPWriter::WriterThreadWrapper(void *me) {
    return (void *) (uintptr_t)static_cast<FMA2DPWriter *>(me)->writerthread();
}

status_t FMA2DPWriter::readerthread() {
    status_t err = OK;
    int framecount = ((4*mBufferSize)/mAudioChannels)/sizeof(int16_t);
    //sizeof(int16_t) is frame size for PCM stream
    int inChannel =
        (mAudioChannels == 2) ? AUDIO_CHANNEL_IN_STEREO :
        AUDIO_CHANNEL_IN_MONO;

    prctl(PR_SET_NAME, (unsigned long)"FMA2DPReaderThread", 0, 0, 0);

    sp<AudioRecord> record;
    record = new AudioRecord(
                     mAudioSource,
                     mSampleRate,
                     mAudioFormat,
                     inChannel,
                     framecount,
                     0);

    if(NULL == record.get()){
        ALOGE("fatal:Not able to open audiorecord");
        return UNKNOWN_ERROR;
    }

    status_t res = record->initCheck();
    if (res == NO_ERROR)
        res = record->start();
    else{
        ALOGE("fatal:record init check failure");
        return UNKNOWN_ERROR;
    }

    while (true) {
        {
            Mutex::Autolock _l (mLock);
            if(mDone)
                break;

            if(mFreeList.empty()){
                mCondVar.signal();
                continue;
            }

            int32_t index = *(mFreeList.begin());
            mFreeList.erase(mFreeList.begin());

            int len = record->read(mFMDataPool.editItemAt(index)->audioBuffer, mBufferSize);
            ALOGV("%s read %d bytes", __func__, len);
            if (len <= 0){
                ALOGE("%s error in reading from audiorecord..bailing out.", __func__);
                this ->notify(MEDIA_RECORDER_EVENT_ERROR, MEDIA_RECORDER_ERROR_UNKNOWN,
                              ERROR_MALFORMED);
                err = INVALID_OPERATION;
                break;
            }

            mDataList.push_back(index);
            mCondVar.signal();
        }
    }
    record->stop();
    return err;
}

status_t FMA2DPWriter::writerthread(){
    status_t err = OK;
    int framecount = (16*mBufferSize)/sizeof(int16_t);
    //sizeof(int16_t) is frame size for PCM stream
    int outChannel = (mAudioChannels== 2) ? AUDIO_CHANNEL_OUT_STEREO :
        AUDIO_CHANNEL_OUT_MONO ;

    prctl(PR_SET_NAME, (unsigned long)"FMA2DPWriterThread", 0, 0, 0);

    sp<AudioTrack> audioTrack;
    audioTrack = new AudioTrack(AUDIO_STREAM_MUSIC,
                                mSampleRate,
                                mAudioFormat,
                                outChannel,
                                framecount);

    if(audioTrack.get() == NULL) {
        ALOGE("fatal:Not able to open audiotrack");
        return UNKNOWN_ERROR;
    }

    status_t res = audioTrack->initCheck();
    if (res == NO_ERROR) {
        audioTrack->setVolume(1, 1);
        audioTrack->start();
    } else {
        ALOGE("fatal:audiotrack init check failure");
        return UNKNOWN_ERROR;
    }

    while (true) {
        {
            Mutex::Autolock _l (mLock);
            if (mDone)
                break;

            if(mDataList.empty()){
                mCondVar.wait(mLock);
                continue;
            }

            int32_t index = *(mDataList.begin());
            mDataList.erase(mDataList.begin());

            size_t ret = audioTrack->write(mFMDataPool.editItemAt(index)->audioBuffer,
                                           mFMDataPool.editItemAt(index)->bufferLen);
            if (!ret) {
                ALOGE("%s audio track write failure.. bailing out", __func__);
                this->notify(MEDIA_RECORDER_EVENT_ERROR, MEDIA_RECORDER_ERROR_UNKNOWN,
                              ERROR_MALFORMED);
                err = INVALID_OPERATION;
                break;
            }
            ALOGV("%s wrote %d bytes", __func__, ret);

            mFreeList.push_back(index);
            mCondVar.signal();
        }
    }
    audioTrack->stop();
    return err;
}

bool FMA2DPWriter::reachedEOS() {
    //API not need for FMA2DPWriter
    return OK;
}

}  // namespace android
