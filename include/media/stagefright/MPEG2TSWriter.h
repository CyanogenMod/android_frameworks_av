/*
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

#ifndef MPEG2TS_WRITER_H_

#define MPEG2TS_WRITER_H_

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AHandlerReflector.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/MediaWriter.h>

namespace android {

struct ABuffer;

struct MPEG2TSWriter : public MediaWriter {
    MPEG2TSWriter(int fd);
    MPEG2TSWriter(const char *filename);

    MPEG2TSWriter(
            void *cookie,
            ssize_t (*write)(void *cookie, const void *data, size_t size));

    virtual status_t addSource(const sp<MediaSource> &source);
    virtual status_t start(MetaData *param = NULL);
    virtual status_t stop() { return reset(); }
    virtual status_t pause();
    virtual bool reachedEOS();
    virtual status_t dump(int fd, const Vector<String16>& args);

    void onMessageReceived(const sp<AMessage> &msg);

protected:
    virtual ~MPEG2TSWriter();

private:
    enum {
        kWhatSourceNotify = 'noti'
    };

    struct SourceInfo;

    FILE *mFile;

    void *mWriteCookie;
    ssize_t (*mWriteFunc)(void *cookie, const void *data, size_t size);

    sp<ALooper> mLooper;
    sp<AHandlerReflector<MPEG2TSWriter> > mReflector;

    bool mStarted;

    Vector<sp<SourceInfo> > mSources;
    size_t mNumSourcesDone;

    int64_t mNumTSPacketsWritten;
    int64_t mNumTSPacketsBeforeMeta;
    int mPATContinuityCounter;
    int mPMTContinuityCounter;
    uint32_t mCrcTable[256];

    void init();

    void writeTS();
    void writeProgramAssociationTable();
    void writeProgramMap();
    void writeAccessUnit(int32_t sourceIndex, const sp<ABuffer> &buffer);
    void initCrcTable();
    uint32_t crc32(const uint8_t *start, size_t length);

    ssize_t internalWrite(const void *data, size_t size);
    status_t reset();

    DISALLOW_EVIL_CONSTRUCTORS(MPEG2TSWriter);
};

}  // namespace android

#endif  // MPEG2TS_WRITER_H_
