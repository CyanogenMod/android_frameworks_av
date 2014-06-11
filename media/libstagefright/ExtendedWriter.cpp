/*
* Copyright (c) 2014, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
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
*
*/

#include <media/stagefright/ExtendedWriter.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/mediarecorder.h>
#include <system/audio.h>

#include <sys/prctl.h>
#include <sys/resource.h>

#include <arpa/inet.h>
#include <QCMediaDefs.h>

#undef LOG_TAG
#define LOG_TAG "ExtendedWriter"

namespace android {

ExtendedWriter::ExtendedWriter(const char *filename)
    : mFile(fopen(filename, "wb")),
      mInitCheck(mFile != NULL ? OK : NO_INIT),
      mStarted(false),
      mPaused(false),
      mResumed(false),
      mOffset(0) {
}

ExtendedWriter::ExtendedWriter(int fd)
    : mFile(fdopen(fd, "wb")),
      mInitCheck(mFile != NULL ? OK : NO_INIT),
      mStarted(false),
      mPaused(false),
      mResumed(false),
      mOffset(0) {
}

ExtendedWriter::~ExtendedWriter() {
    if (mStarted) {
        stop();
    }

    if (mFile != NULL) {
        fclose(mFile);
        mFile = NULL;
    }
}

status_t ExtendedWriter::initCheck() const {
    return mInitCheck;
}

status_t ExtendedWriter::addSource(const sp<MediaSource> &source) {
    if (mInitCheck != OK) {
        ALOGE("Init Check not OK, return");
        return mInitCheck;
    }

    if (mSource != NULL) {
        ALOGE("A source already exists, return");
        return UNKNOWN_ERROR;
    }

    sp<MetaData> meta = source->getFormat();

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if ( !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_QCELP)) {
        mFormat = AUDIO_FORMAT_QCELP;
    } else if ( !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_EVRC)) {
        mFormat = AUDIO_FORMAT_EVRC;
    }
    else {
        return UNKNOWN_ERROR;
    }

    int32_t channelCount;
    int32_t sampleRate;
    CHECK(meta->findInt32(kKeyChannelCount, &channelCount));
    CHECK_EQ(channelCount, 1);
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    CHECK_EQ(sampleRate, 8000);

    mSource = source;

    return OK;
}

status_t ExtendedWriter::start(MetaData *params) {
    if (mInitCheck != OK) {
        ALOGE("Init Check not OK, return");
        return mInitCheck;
    }

    if (mSource == NULL) {
        ALOGE("NULL Source");
        return UNKNOWN_ERROR;
    }

    if (mStarted && mPaused) {
        mPaused = false;
        mResumed = true;
        return OK;
    } else if (mStarted) {
        ALOGE("Already startd, return");
        return OK;
    }

    //space for header;
    size_t headerSize = sizeof( struct QCPEVRCHeader );
    uint8_t * header = (uint8_t *)malloc(headerSize);
    memset( header, '?', headerSize);
    fwrite( header, 1, headerSize, mFile );
    mOffset += headerSize;
    delete header;

    status_t err = mSource->start();

    if (err != OK) {
        return err;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    mReachedEOS = false;
    mDone = false;

    pthread_create(&mThread, &attr, ThreadWrapper, this);
    pthread_attr_destroy(&attr);

    mStarted = true;

    return OK;
}

status_t ExtendedWriter::pause() {
    if (!mStarted) {
        return OK;
    }
    mPaused = true;
    return OK;
}

status_t ExtendedWriter::stop() {
    if (!mStarted) {
        return OK;
    }

    mDone = true;

    void *dummy;
    pthread_join(mThread, &dummy);

    status_t err = static_cast<status_t>(reinterpret_cast<uintptr_t>(dummy));
    {
        status_t status = mSource->stop();
        if (err == OK &&
            (status != OK && status != ERROR_END_OF_STREAM)) {
            err = status;
        }
    }

    mStarted = false;
    return err;
}

bool ExtendedWriter::exceedsFileSizeLimit() {
    if (mMaxFileSizeLimitBytes == 0) {
        return false;
    }
    return mEstimatedSizeBytes >= mMaxFileSizeLimitBytes;
}

bool ExtendedWriter::exceedsFileDurationLimit() {
    if (mMaxFileDurationLimitUs == 0) {
        return false;
    }
    return mEstimatedDurationUs >= mMaxFileDurationLimitUs;
}

// static
void *ExtendedWriter::ThreadWrapper(void *me) {
    return (void *) (uintptr_t)static_cast<ExtendedWriter *>(me)->threadFunc();
}

status_t ExtendedWriter::threadFunc() {
    mEstimatedDurationUs = 0;
    mEstimatedSizeBytes = 0;
    bool stoppedPrematurely = true;
    int64_t previousPausedDurationUs = 0;
    int64_t maxTimestampUs = 0;
    status_t err = OK;

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"ExtendedWriter", 0, 0, 0);
    while (!mDone) {
        MediaBuffer *buffer;
        err = mSource->read(&buffer);

        if (err != OK) {
            break;
        }

        if (mPaused) {
            buffer->release();
            buffer = NULL;
            continue;
        }

        mEstimatedSizeBytes += buffer->range_length();
        if (exceedsFileSizeLimit()) {
            buffer->release();
            buffer = NULL;
            notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED, 0);
            break;
        }

        int64_t timestampUs;
        CHECK(buffer->meta_data()->findInt64(kKeyTime, &timestampUs));
        if (timestampUs > mEstimatedDurationUs) {
            mEstimatedDurationUs = timestampUs;
        }
        if (mResumed) {
            previousPausedDurationUs += (timestampUs - maxTimestampUs - 20000);
            mResumed = false;
        }
        timestampUs -= previousPausedDurationUs;
        ALOGV("time stamp: %lld, previous paused duration: %lld",
                timestampUs, previousPausedDurationUs);
        if (timestampUs > maxTimestampUs) {
            maxTimestampUs = timestampUs;
        }

        if (exceedsFileDurationLimit()) {
            buffer->release();
            buffer = NULL;
            notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_INFO_MAX_DURATION_REACHED, 0);
            break;
        }
        ssize_t n = fwrite(
                (const uint8_t *)buffer->data() + buffer->range_offset(),
                1,
                buffer->range_length(),
                mFile);
        mOffset += n;

        if (n < (ssize_t)buffer->range_length()) {
            buffer->release();
            buffer = NULL;

            break;
        }

        // XXX: How to tell it is stopped prematurely?
        if (stoppedPrematurely) {
            stoppedPrematurely = false;
        }

        buffer->release();
        buffer = NULL;
    }

    if (stoppedPrematurely) {
        notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_TRACK_INFO_COMPLETION_STATUS, UNKNOWN_ERROR);
    }

    if ( mFormat == AUDIO_FORMAT_QCELP ) {
        writeQCPHeader( );
    }
    else if ( mFormat == AUDIO_FORMAT_EVRC ) {
        writeEVRCHeader( );
    }

    fflush(mFile);
    fclose(mFile);
    mFile = NULL;
    mReachedEOS = true;
    if (err == ERROR_END_OF_STREAM || (err == -ETIMEDOUT)) {
        return OK;
    }
    return err;
}

bool ExtendedWriter::reachedEOS() {
    return mReachedEOS;
}

status_t ExtendedWriter::writeQCPHeader() {
    /* Common part */
    struct QCPEVRCHeader header = {
        {'R', 'I', 'F', 'F'}, 0, {'Q', 'L', 'C', 'M'}, /* Riff */
        {'f', 'm', 't', ' '}, 150, 1, 0, 0, 0, 0,{0}, 0, {0},0,0,160,8000,16,0,{0},{0},{0}, /* Fmt */
        {'v','r','a','t'}, 0, 0, 0, /* Vrat */
        {'d','a','t','a'},0 /* Data */
    };

    fseeko(mFile, 0, SEEK_SET);
    header.s_riff = (mOffset - 8);
    header.data1 = (0x5E7F6D41);
    header.data2 = (0xB115);
    header.data3 = (0x11D0);
    header.data4[0] = 0xBA;
    header.data4[1] = 0x91;
    header.data4[2] = 0x00;
    header.data4[3] = 0x80;
    header.data4[4] = 0x5F;
    header.data4[5] = 0xB4;
    header.data4[6] = 0xB9;
    header.data4[7] = 0x7E;
    header.ver = (0x0002);
    memcpy(header.name, "Qcelp 13K", 9);
    header.abps = (13000);
    header.bytes_per_pkt = (35);
    header.vr_num_of_rates = 5;
    header.vr_bytes_per_pkt[0] = (0x0422);
    header.vr_bytes_per_pkt[1] = (0x0310);
    header.vr_bytes_per_pkt[2] = (0x0207);
    header.vr_bytes_per_pkt[3] = (0x0103);
    header.s_vrat = (0x00000008);
    header.v_rate = (0x00000001);
    header.size_in_pkts = (mOffset - sizeof( struct QCPEVRCHeader ))/ header.bytes_per_pkt;
    header.s_data = mOffset - sizeof( struct QCPEVRCHeader );
    fwrite( &header, 1, sizeof( struct QCPEVRCHeader ), mFile );
    return OK;
}

status_t ExtendedWriter::writeEVRCHeader() {
    /* Common part */
    struct QCPEVRCHeader header = {
        {'R', 'I', 'F', 'F'}, 0, {'Q', 'L', 'C', 'M'}, /* Riff */
        {'f', 'm', 't', ' '}, 150, 1, 0, 0, 0, 0,{0}, 0, {0},0,0,160,8000,16,0,{0},{0},{0}, /* Fmt */
        {'v','r','a','t'}, 0, 0, 0, /* Vrat */
        {'d','a','t','a'},0 /* Data */
    };

    fseeko(mFile, 0, SEEK_SET);
    header.s_riff = (mOffset - 8);
    header.data1 = (0xe689d48d);
    header.data2 = (0x9076);
    header.data3 = (0x46b5);
    header.data4[0] = 0x91;
    header.data4[1] = 0xef;
    header.data4[2] = 0x73;
    header.data4[3] = 0x6a;
    header.data4[4] = 0x51;
    header.data4[5] = 0x00;
    header.data4[6] = 0xce;
    header.data4[7] = 0xb4;
    header.ver = (0x0001);
    memcpy(header.name, "TIA IS-127 Enhanced Variable Rate Codec, Speech Service Option 3", 64);
    header.abps = (9600);
    header.bytes_per_pkt = (23);
    header.vr_num_of_rates = 4;
    header.vr_bytes_per_pkt[0] = (0x0416);
    header.vr_bytes_per_pkt[1] = (0x030a);
    header.vr_bytes_per_pkt[2] = (0x0200);
    header.vr_bytes_per_pkt[3] = (0x0102);
    header.s_vrat = (0x00000008);
    header.v_rate = (0x00000001);
    header.size_in_pkts = (mOffset - sizeof( struct QCPEVRCHeader )) / header.bytes_per_pkt;
    header.s_data = mOffset - sizeof( struct QCPEVRCHeader );
    fwrite( &header, 1, sizeof( struct QCPEVRCHeader ), mFile );
    return OK;
}


}  // namespace android
