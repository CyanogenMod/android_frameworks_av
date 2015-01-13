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

#ifndef MIDI_EXTRACTOR_H_
#define MIDI_EXTRACTOR_H_

#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/MidiIoWrapper.h>
#include <utils/String8.h>
#include <libsonivox/eas.h>

namespace android {

class MidiEngine : public RefBase {
public:
    MidiEngine(const sp<DataSource> &dataSource,
            const sp<MetaData> &fileMetadata,
            const sp<MetaData> &trackMetadata);
    ~MidiEngine();

    status_t initCheck();

    status_t allocateBuffers();
    status_t releaseBuffers();
    status_t seekTo(int64_t positionUs);
    MediaBuffer* readBuffer();
private:
    sp<MidiIoWrapper> mIoWrapper;
    MediaBufferGroup *mGroup;
    EAS_DATA_HANDLE mEasData;
    EAS_HANDLE mEasHandle;
    const S_EAS_LIB_CONFIG* mEasConfig;
    bool mIsInitialized;
};

class MidiExtractor : public MediaExtractor {

public:
    // Extractor assumes ownership of source
    MidiExtractor(const sp<DataSource> &source);

    virtual size_t countTracks();
    virtual sp<MediaSource> getTrack(size_t index);
    virtual sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);

    virtual sp<MetaData> getMetaData();

protected:
    virtual ~MidiExtractor();

private:
    sp<DataSource> mDataSource;
    status_t mInitCheck;
    sp<MetaData> mFileMetadata;

    // There is only one track
    sp<MetaData> mTrackMetadata;

    sp<MidiEngine> mEngine;

    EAS_DATA_HANDLE     mEasData;
    EAS_HANDLE          mEasHandle;
    EAS_PCM*            mAudioBuffer;
    EAS_I32             mPlayTime;
    EAS_I32             mDuration;
    EAS_STATE           mState;
    EAS_FILE            mFileLocator;

    MidiExtractor(const MidiExtractor &);
    MidiExtractor &operator=(const MidiExtractor &);

};

bool SniffMidi(const sp<DataSource> &source, String8 *mimeType,
        float *confidence, sp<AMessage> *);

}  // namespace android

#endif  // MIDI_EXTRACTOR_H_
