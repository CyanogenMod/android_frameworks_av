/*
 * Copyright (C) ST-Ericsson SA 2012
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
 *
 * Author: Stefan Ekenberg (stefan.ekenberg@stericsson.com) for ST-Ericsson
 */

#ifndef FMRADIO_SOURCE_H_

#define FMRADIO_SOURCE_H_

#include <media/AudioRecord.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ABase.h>
#include <system/audio.h>

namespace android {

class FMRadioSource : public DataSource {
public:
    FMRadioSource();

    virtual status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual status_t getSize(off64_t *size);

protected:
    virtual ~FMRadioSource();

private:
    struct Buffer {
        size_t  frameCount;
        size_t  size;
        int8_t* data;
    };

    status_t openRecord(int frameCount, audio_io_handle_t input);
    status_t obtainBuffer(Buffer* audioBuffer);

    status_t mInitCheck;
    bool mStarted;
    int mSessionId;
    sp<IAudioRecord> mAudioRecord;
    sp<IMemory> mCblkMemory;
    audio_track_cblk_t* mCblk;

    DISALLOW_EVIL_CONSTRUCTORS(FMRadioSource);
};

}  // namespace android

#endif  // FMRADIO_SOURCE_H_
