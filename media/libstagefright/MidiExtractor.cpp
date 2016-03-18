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
#define LOG_TAG "MidiExtractor"
#include <utils/Log.h>

#include "include/MidiExtractor.h"

#include <media/MidiIoWrapper.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaSource.h>
#include <libsonivox/eas_reverb.h>

namespace android {

// how many Sonivox output buffers to aggregate into one MediaBuffer
static const int NUM_COMBINE_BUFFERS = 4;

class MidiSource : public MediaSource {

public:
    MidiSource(
            const sp<MidiEngine> &engine,
            const sp<MetaData> &trackMetadata);

    virtual status_t start(MetaData *params);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~MidiSource();

private:
    sp<MidiEngine> mEngine;
    sp<MetaData> mTrackMetadata;
    bool mInitCheck;
    bool mStarted;

    status_t init();

    // no copy constructor or assignment
    MidiSource(const MidiSource &);
    MidiSource &operator=(const MidiSource &);

};


// Midisource

MidiSource::MidiSource(
        const sp<MidiEngine> &engine,
        const sp<MetaData> &trackMetadata)
    : mEngine(engine),
      mTrackMetadata(trackMetadata),
      mInitCheck(false),
      mStarted(false)
{
    ALOGV("MidiSource ctor");
    mInitCheck = init();
}

MidiSource::~MidiSource()
{
    ALOGV("MidiSource dtor");
    if (mStarted) {
        stop();
    }
}

status_t MidiSource::start(MetaData * /* params */)
{
    ALOGV("MidiSource::start");

    CHECK(!mStarted);
    mStarted = true;
    mEngine->allocateBuffers();
    return OK;
}

status_t MidiSource::stop()
{
    ALOGV("MidiSource::stop");

    CHECK(mStarted);
    mStarted = false;
    mEngine->releaseBuffers();

    return OK;
}

sp<MetaData> MidiSource::getFormat()
{
    return mTrackMetadata;
}

status_t MidiSource::read(
        MediaBuffer **outBuffer, const ReadOptions *options)
{
    ALOGV("MidiSource::read");
    MediaBuffer *buffer;
    // process an optional seek request
    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if ((NULL != options) && options->getSeekTo(&seekTimeUs, &mode)) {
        if (seekTimeUs <= 0LL) {
            seekTimeUs = 0LL;
        }
        mEngine->seekTo(seekTimeUs);
    }
    buffer = mEngine->readBuffer();
    *outBuffer = buffer;
    ALOGV("MidiSource::read %p done", this);
    return buffer != NULL ? (status_t) OK : (status_t) ERROR_END_OF_STREAM;
}

status_t MidiSource::init()
{
    ALOGV("MidiSource::init");
    return OK;
}

// MidiEngine

MidiEngine::MidiEngine(const sp<DataSource> &dataSource,
        const sp<MetaData> &fileMetadata,
        const sp<MetaData> &trackMetadata) :
            mGroup(NULL),
            mEasData(NULL),
            mEasHandle(NULL),
            mEasConfig(NULL),
            mIsInitialized(false) {
    mIoWrapper = new MidiIoWrapper(dataSource);
    // spin up a new EAS engine
    EAS_I32 temp;
    EAS_RESULT result = EAS_Init(&mEasData);

    if (result == EAS_SUCCESS) {
        result = EAS_OpenFile(mEasData, mIoWrapper->getLocator(), &mEasHandle);
    }
    if (result == EAS_SUCCESS) {
        result = EAS_Prepare(mEasData, mEasHandle);
    }
    if (result == EAS_SUCCESS) {
        result = EAS_ParseMetaData(mEasData, mEasHandle, &temp);
    }

    if (result != EAS_SUCCESS) {
        return;
    }

    if (fileMetadata != NULL) {
        fileMetadata->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MIDI);
    }

    if (trackMetadata != NULL) {
        trackMetadata->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
        trackMetadata->setInt64(kKeyDuration, 1000ll * temp); // milli->micro
        mEasConfig = EAS_Config();
        trackMetadata->setInt32(kKeySampleRate, mEasConfig->sampleRate);
        trackMetadata->setInt32(kKeyChannelCount, mEasConfig->numChannels);
        trackMetadata->setInt32(kKeyPcmEncoding, kAudioEncodingPcm16bit);
    }
    mIsInitialized = true;
}

MidiEngine::~MidiEngine() {
    if (mEasHandle) {
        EAS_CloseFile(mEasData, mEasHandle);
    }
    if (mEasData) {
        EAS_Shutdown(mEasData);
    }
    delete mGroup;

}

status_t MidiEngine::initCheck() {
    return mIsInitialized ? OK : UNKNOWN_ERROR;
}

status_t MidiEngine::allocateBuffers() {
    // select reverb preset and enable
    EAS_SetParameter(mEasData, EAS_MODULE_REVERB, EAS_PARAM_REVERB_PRESET, EAS_PARAM_REVERB_CHAMBER);
    EAS_SetParameter(mEasData, EAS_MODULE_REVERB, EAS_PARAM_REVERB_BYPASS, EAS_FALSE);

    mGroup = new MediaBufferGroup;
    int bufsize = sizeof(EAS_PCM)
            * mEasConfig->mixBufferSize * mEasConfig->numChannels * NUM_COMBINE_BUFFERS;
    ALOGV("using %d byte buffer", bufsize);
    mGroup->add_buffer(new MediaBuffer(bufsize));
    return OK;
}

status_t MidiEngine::releaseBuffers() {
    delete mGroup;
    mGroup = NULL;
    return OK;
}

status_t MidiEngine::seekTo(int64_t positionUs) {
    ALOGV("seekTo %lld", (long long)positionUs);
    EAS_RESULT result = EAS_Locate(mEasData, mEasHandle, positionUs / 1000, false);
    return result == EAS_SUCCESS ? OK : UNKNOWN_ERROR;
}

MediaBuffer* MidiEngine::readBuffer() {
    EAS_STATE state;
    EAS_State(mEasData, mEasHandle, &state);
    if ((state == EAS_STATE_STOPPED) || (state == EAS_STATE_ERROR)) {
        return NULL;
    }
    MediaBuffer *buffer;
    status_t err = mGroup->acquire_buffer(&buffer);
    if (err != OK) {
        ALOGE("readBuffer: no buffer");
        return NULL;
    }
    EAS_I32 timeMs;
    EAS_GetLocation(mEasData, mEasHandle, &timeMs);
    int64_t timeUs = 1000ll * timeMs;
    buffer->meta_data()->setInt64(kKeyTime, timeUs);

    EAS_PCM* p = (EAS_PCM*) buffer->data();
    int numBytesOutput = 0;
    for (int i = 0; i < NUM_COMBINE_BUFFERS; i++) {
        EAS_I32 numRendered;
        EAS_RESULT result = EAS_Render(mEasData, p, mEasConfig->mixBufferSize, &numRendered);
        if (result != EAS_SUCCESS) {
            ALOGE("EAS_Render returned %ld", result);
            break;
        }
        p += numRendered * mEasConfig->numChannels;
        numBytesOutput += numRendered * mEasConfig->numChannels * sizeof(EAS_PCM);
    }
    buffer->set_range(0, numBytesOutput);
    ALOGV("readBuffer: returning %zd in buffer %p", buffer->range_length(), buffer);
    return buffer;
}


// MidiExtractor

MidiExtractor::MidiExtractor(
        const sp<DataSource> &dataSource)
    : mDataSource(dataSource),
      mInitCheck(false)
{
    ALOGV("MidiExtractor ctor");
    mFileMetadata = new MetaData;
    mTrackMetadata = new MetaData;
    mEngine = new MidiEngine(mDataSource, mFileMetadata, mTrackMetadata);
    mInitCheck = mEngine->initCheck();
}

MidiExtractor::~MidiExtractor()
{
    ALOGV("MidiExtractor dtor");
}

size_t MidiExtractor::countTracks()
{
    return mInitCheck == OK ? 1 : 0;
}

sp<IMediaSource> MidiExtractor::getTrack(size_t index)
{
    if (mInitCheck != OK || index > 0) {
        return NULL;
    }
    return new MidiSource(mEngine, mTrackMetadata);
}

sp<MetaData> MidiExtractor::getTrackMetaData(
        size_t index, uint32_t /* flags */) {
    ALOGV("MidiExtractor::getTrackMetaData");
    if (mInitCheck != OK || index > 0) {
        return NULL;
    }
    return mTrackMetadata;
}

sp<MetaData> MidiExtractor::getMetaData()
{
    ALOGV("MidiExtractor::getMetaData");
    return mFileMetadata;
}

// Sniffer

bool SniffMidi(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *)
{
    sp<MidiEngine> p = new MidiEngine(source, NULL, NULL);
    if (p->initCheck() == OK) {
        *mimeType = MEDIA_MIMETYPE_AUDIO_MIDI;
        *confidence = 0.8;
        ALOGV("SniffMidi: yes");
        return true;
    }
    ALOGV("SniffMidi: no");
    return false;

}

}  // namespace android
