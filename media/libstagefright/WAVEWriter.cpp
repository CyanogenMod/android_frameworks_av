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

//#define LOG_NDEBUG 0
#define LOG_TAG "WAVEWriter"
#include <utils/Log.h>

#include <media/stagefright/WAVEWriter.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/mediarecorder.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace android {

static struct wav_header hdr;


WAVEWriter::WAVEWriter(const char *filename)
    : mFd(-1),
      mInitCheck(NO_INIT),
      mStarted(false),
      mPaused(false),
      mResumed(false) {

    mFd = open(filename, O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
    if (mFd >= 0) {
        mInitCheck = OK;
    }
}

WAVEWriter::WAVEWriter(int fd)
    : mFd(dup(fd)),
      mInitCheck(mFd < 0? NO_INIT: OK),
      mStarted(false),
      mPaused(false),
      mResumed(false) {
}

WAVEWriter::~WAVEWriter() {
    if (mStarted) {
        stop();
    }

    if (mFd != -1) {
        close(mFd);
        mFd = -1;
    }
}

status_t WAVEWriter::initCheck() const {
    return mInitCheck;
}

status_t WAVEWriter::addSource(const sp<MediaSource> &source) {
    uint32_t count;
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

    int32_t channelCount;
    int32_t sampleRate;
    CHECK(meta->findInt32(kKeyChannelCount, &channelCount));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

    memset(&hdr, 0, sizeof(struct wav_header));
    hdr.riff_id = ID_RIFF;
    hdr.riff_fmt = ID_WAVE;
    hdr.fmt_id = ID_FMT;
    hdr.fmt_sz = 16;
    hdr.audio_format = FORMAT_PCM;
    hdr.num_channels = channelCount;
    hdr.sample_rate = sampleRate;
    hdr.bits_per_sample = 16;
    hdr.byte_rate = (sampleRate * channelCount * hdr.bits_per_sample) / 8;
    hdr.block_align = ( hdr.bits_per_sample * channelCount ) / 8;
    hdr.data_id = ID_DATA;
    hdr.data_sz = 0;
    hdr.riff_sz = hdr.data_sz + 44 - 8;

    if (write(mFd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        ALOGE("Write header error, return ERROR_IO");
        return -ERROR_IO;
    }

    mSource = source;

    return OK;
}

status_t WAVEWriter::start(MetaData *params) {
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

status_t WAVEWriter::pause() {
    if (!mStarted) {
        return OK;
    }
    mPaused = true;
    return OK;
}

status_t WAVEWriter::stop() {
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

bool WAVEWriter::exceedsFileSizeLimit() {
    if (mMaxFileSizeLimitBytes == 0) {
        return false;
    }
    return mEstimatedSizeBytes >= mMaxFileSizeLimitBytes;
}

bool WAVEWriter::exceedsFileDurationLimit() {
    if (mMaxFileDurationLimitUs == 0) {
        return false;
    }
    return mEstimatedDurationUs >= mMaxFileDurationLimitUs;
}

// static
void *WAVEWriter::ThreadWrapper(void *me) {
    return (void *) (uintptr_t)static_cast<WAVEWriter *>(me)->threadFunc();
}

status_t WAVEWriter::threadFunc() {
    mEstimatedDurationUs = 0;
    mEstimatedSizeBytes = 0;
    bool stoppedPrematurely = true;
    int64_t previousPausedDurationUs = 0;
    int64_t maxTimestampUs = 0;
    status_t err = OK;

    prctl(PR_SET_NAME, (unsigned long)"WAVEWriter", 0, 0, 0);
    hdr.data_sz = 0;
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
        ssize_t n = write(mFd,
                        (const uint8_t *)buffer->data() + buffer->range_offset(),
                        buffer->range_length());

        hdr.data_sz += (ssize_t)buffer->range_length();
        hdr.riff_sz = hdr.data_sz + 44 - 8;

        if (n < (ssize_t)buffer->range_length()) {
            buffer->release();
            buffer = NULL;

            break;
        }

        if (stoppedPrematurely) {
            stoppedPrematurely = false;
        }

        buffer->release();
        buffer = NULL;
    }

    if (stoppedPrematurely) {
        notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_TRACK_INFO_COMPLETION_STATUS, UNKNOWN_ERROR);
    }

    lseek(mFd, 0, SEEK_SET);
    write(mFd, &hdr, sizeof(hdr));
    lseek(mFd, 0, SEEK_END);

    close(mFd);
    mFd = -1;
    mReachedEOS = true;
    if (err == ERROR_END_OF_STREAM) {
        return OK;
    }
    return err;
}

bool WAVEWriter::reachedEOS() {
    return mReachedEOS;
}

}  // namespace android
