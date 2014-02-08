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

// #define LOG_NDEBUG 0
#define LOG_TAG "WebmWriter"

#include "EbmlUtil.h"
#include "WebmWriter.h"

#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ADebug.h>

#include <utils/Errors.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <inttypes.h>

using namespace webm;

namespace {
size_t XiphLaceCodeLen(size_t size) {
    return size / 0xff + 1;
}

size_t XiphLaceEnc(uint8_t *buf, size_t size) {
    size_t i;
    for (i = 0; size >= 0xff; ++i, size -= 0xff) {
        buf[i] = 0xff;
    }
    buf[i++] = size;
    return i;
}
}

namespace android {

static const int64_t kMinStreamableFileSizeInBytes = 5 * 1024 * 1024;

WebmWriter::WebmWriter(int fd)
    : mFd(dup(fd)),
      mInitCheck(mFd < 0 ? NO_INIT : OK),
      mTimeCodeScale(1000000),
      mStartTimestampUs(0),
      mStartTimeOffsetMs(0),
      mSegmentOffset(0),
      mSegmentDataStart(0),
      mInfoOffset(0),
      mInfoSize(0),
      mTracksOffset(0),
      mCuesOffset(0),
      mPaused(false),
      mStarted(false),
      mIsFileSizeLimitExplicitlyRequested(false),
      mIsRealTimeRecording(false),
      mStreamableFile(true),
      mEstimatedCuesSize(0) {
    mStreams[kAudioIndex] = WebmStream(kAudioType, "Audio", &WebmWriter::audioTrack);
    mStreams[kVideoIndex] = WebmStream(kVideoType, "Video", &WebmWriter::videoTrack);
    mSinkThread = new WebmFrameSinkThread(
            mFd,
            mSegmentDataStart,
            mStreams[kVideoIndex].mSink,
            mStreams[kAudioIndex].mSink,
            mCuePoints);
}

WebmWriter::WebmWriter(const char *filename)
    : mInitCheck(NO_INIT),
      mTimeCodeScale(1000000),
      mStartTimestampUs(0),
      mStartTimeOffsetMs(0),
      mSegmentOffset(0),
      mSegmentDataStart(0),
      mInfoOffset(0),
      mInfoSize(0),
      mTracksOffset(0),
      mCuesOffset(0),
      mPaused(false),
      mStarted(false),
      mIsFileSizeLimitExplicitlyRequested(false),
      mIsRealTimeRecording(false),
      mStreamableFile(true),
      mEstimatedCuesSize(0) {
    mFd = open(filename, O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
    if (mFd >= 0) {
        ALOGV("fd %d; flags: %o", mFd, fcntl(mFd, F_GETFL, 0));
        mInitCheck = OK;
    }
    mStreams[kAudioIndex] = WebmStream(kAudioType, "Audio", &WebmWriter::audioTrack);
    mStreams[kVideoIndex] = WebmStream(kVideoType, "Video", &WebmWriter::videoTrack);
    mSinkThread = new WebmFrameSinkThread(
            mFd,
            mSegmentDataStart,
            mStreams[kVideoIndex].mSink,
            mStreams[kAudioIndex].mSink,
            mCuePoints);
}

// static
sp<WebmElement> WebmWriter::videoTrack(const sp<MetaData>& md) {
    int32_t width, height;
    CHECK(md->findInt32(kKeyWidth, &width));
    CHECK(md->findInt32(kKeyHeight, &height));
    return WebmElement::VideoTrackEntry(width, height);
}

// static
sp<WebmElement> WebmWriter::audioTrack(const sp<MetaData>& md) {
    int32_t nChannels, samplerate;
    uint32_t type;
    const void *headerData1;
    const char headerData2[] = { 3, 'v', 'o', 'r', 'b', 'i', 's', 7, 0, 0, 0,
            'a', 'n', 'd', 'r', 'o', 'i', 'd', 0, 0, 0, 0, 1 };
    const void *headerData3;
    size_t headerSize1, headerSize2 = sizeof(headerData2), headerSize3;

    CHECK(md->findInt32(kKeyChannelCount, &nChannels));
    CHECK(md->findInt32(kKeySampleRate, &samplerate));
    CHECK(md->findData(kKeyVorbisInfo, &type, &headerData1, &headerSize1));
    CHECK(md->findData(kKeyVorbisBooks, &type, &headerData3, &headerSize3));

    size_t codecPrivateSize = 1;
    codecPrivateSize += XiphLaceCodeLen(headerSize1);
    codecPrivateSize += XiphLaceCodeLen(headerSize2);
    codecPrivateSize += headerSize1 + headerSize2 + headerSize3;

    off_t off = 0;
    sp<ABuffer> codecPrivateBuf = new ABuffer(codecPrivateSize);
    uint8_t *codecPrivateData = codecPrivateBuf->data();
    codecPrivateData[off++] = 2;

    off += XiphLaceEnc(codecPrivateData + off, headerSize1);
    off += XiphLaceEnc(codecPrivateData + off, headerSize2);

    memcpy(codecPrivateData + off, headerData1, headerSize1);
    off += headerSize1;
    memcpy(codecPrivateData + off, headerData2, headerSize2);
    off += headerSize2;
    memcpy(codecPrivateData + off, headerData3, headerSize3);

    sp<WebmElement> entry = WebmElement::AudioTrackEntry(
            nChannels,
            samplerate,
            codecPrivateBuf);
    return entry;
}

size_t WebmWriter::numTracks() {
    Mutex::Autolock autolock(mLock);

    size_t numTracks = 0;
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (mStreams[i].mTrackEntry != NULL) {
            numTracks++;
        }
    }

    return numTracks;
}

uint64_t WebmWriter::estimateCuesSize(int32_t bitRate) {
    // This implementation is based on estimateMoovBoxSize in MPEG4Writer.
    //
    // Statistical analysis shows that metadata usually accounts
    // for a small portion of the total file size, usually < 0.6%.

    // The default MIN_MOOV_BOX_SIZE is set to 0.6% x 1MB / 2,
    // where 1MB is the common file size limit for MMS application.
    // The default MAX _MOOV_BOX_SIZE value is based on about 3
    // minute video recording with a bit rate about 3 Mbps, because
    // statistics also show that most of the video captured are going
    // to be less than 3 minutes.

    // If the estimation is wrong, we will pay the price of wasting
    // some reserved space. This should not happen so often statistically.
    static const int32_t factor = 2;
    static const int64_t MIN_CUES_SIZE = 3 * 1024;  // 3 KB
    static const int64_t MAX_CUES_SIZE = (180 * 3000000 * 6LL / 8000);
    int64_t size = MIN_CUES_SIZE;

    // Max file size limit is set
    if (mMaxFileSizeLimitBytes != 0 && mIsFileSizeLimitExplicitlyRequested) {
        size = mMaxFileSizeLimitBytes * 6 / 1000;
    }

    // Max file duration limit is set
    if (mMaxFileDurationLimitUs != 0) {
        if (bitRate > 0) {
            int64_t size2 = ((mMaxFileDurationLimitUs * bitRate * 6) / 1000 / 8000000);
            if (mMaxFileSizeLimitBytes != 0 && mIsFileSizeLimitExplicitlyRequested) {
                // When both file size and duration limits are set,
                // we use the smaller limit of the two.
                if (size > size2) {
                    size = size2;
                }
            } else {
                // Only max file duration limit is set
                size = size2;
            }
        }
    }

    if (size < MIN_CUES_SIZE) {
        size = MIN_CUES_SIZE;
    }

    // Any long duration recording will be probably end up with
    // non-streamable webm file.
    if (size > MAX_CUES_SIZE) {
        size = MAX_CUES_SIZE;
    }

    ALOGV("limits: %" PRId64 "/%" PRId64 " bytes/us,"
            " bit rate: %d bps and the estimated cues size %" PRId64 " bytes",
            mMaxFileSizeLimitBytes, mMaxFileDurationLimitUs, bitRate, size);
    return factor * size;
}

void WebmWriter::initStream(size_t idx) {
    if (mStreams[idx].mThread != NULL) {
        return;
    }
    if (mStreams[idx].mSource == NULL) {
        ALOGV("adding dummy source ... ");
        mStreams[idx].mThread = new WebmFrameEmptySourceThread(
                mStreams[idx].mType, mStreams[idx].mSink);
    } else {
        ALOGV("adding source %p", mStreams[idx].mSource.get());
        mStreams[idx].mThread = new WebmFrameMediaSourceThread(
                mStreams[idx].mSource,
                mStreams[idx].mType,
                mStreams[idx].mSink,
                mTimeCodeScale,
                mStartTimestampUs,
                mStartTimeOffsetMs,
                numTracks(),
                mIsRealTimeRecording);
    }
}

void WebmWriter::release() {
    close(mFd);
    mFd = -1;
    mInitCheck = NO_INIT;
    mStarted = false;
}

status_t WebmWriter::reset() {
    if (mInitCheck != OK) {
        return OK;
    } else {
        if (!mStarted) {
            release();
            return OK;
        }
    }

    status_t err = OK;
    int64_t maxDurationUs = 0;
    int64_t minDurationUs = 0x7fffffffffffffffLL;
    for (int i = 0; i < kMaxStreams; ++i) {
        if (mStreams[i].mThread == NULL) {
            continue;
        }

        status_t status = mStreams[i].mThread->stop();
        if (err == OK && status != OK) {
            err = status;
        }

        int64_t durationUs = mStreams[i].mThread->getDurationUs();
        if (durationUs > maxDurationUs) {
            maxDurationUs = durationUs;
        }
        if (durationUs < minDurationUs) {
            minDurationUs = durationUs;
        }
    }

    if (numTracks() > 1) {
        ALOGD("Duration from tracks range is [%" PRId64 ", %" PRId64 "] us", minDurationUs, maxDurationUs);
    }

    mSinkThread->stop();

    // Do not write out movie header on error.
    if (err != OK) {
        release();
        return err;
    }

    sp<WebmElement> cues = new WebmMaster(kMkvCues, mCuePoints);
    uint64_t cuesSize = cues->totalSize();
    // TRICKY Even when the cues do fit in the space we reserved, if they do not fit
    // perfectly, we still need to check if there is enough "extra space" to write an
    // EBML void element.
    if (cuesSize != mEstimatedCuesSize && cuesSize > mEstimatedCuesSize - kMinEbmlVoidSize) {
        mCuesOffset = ::lseek(mFd, 0, SEEK_CUR);
        cues->write(mFd, cuesSize);
    } else {
        uint64_t spaceSize;
        ::lseek(mFd, mCuesOffset, SEEK_SET);
        cues->write(mFd, cuesSize);
        sp<WebmElement> space = new EbmlVoid(mEstimatedCuesSize - cuesSize);
        space->write(mFd, spaceSize);
    }

    mCuePoints.clear();
    mStreams[kVideoIndex].mSink.clear();
    mStreams[kAudioIndex].mSink.clear();

    uint8_t bary[sizeof(uint64_t)];
    uint64_t totalSize = ::lseek(mFd, 0, SEEK_END);
    uint64_t segmentSize = totalSize - mSegmentDataStart;
    ::lseek(mFd, mSegmentOffset + sizeOf(kMkvSegment), SEEK_SET);
    uint64_t segmentSizeCoded = encodeUnsigned(segmentSize, sizeOf(kMkvUnknownLength));
    serializeCodedUnsigned(segmentSizeCoded, bary);
    ::write(mFd, bary, sizeOf(kMkvUnknownLength));

    uint64_t size;
    uint64_t durationOffset = mInfoOffset + sizeOf(kMkvInfo) + sizeOf(mInfoSize)
        + sizeOf(kMkvSegmentDuration) + sizeOf(sizeof(double));
    sp<WebmElement> duration = new WebmFloat(
            kMkvSegmentDuration,
            (double) (maxDurationUs * 1000 / mTimeCodeScale));
    duration->serializePayload(bary);
    ::lseek(mFd, durationOffset, SEEK_SET);
    ::write(mFd, bary, sizeof(double));

    List<sp<WebmElement> > seekEntries;
    seekEntries.push_back(WebmElement::SeekEntry(kMkvInfo, mInfoOffset - mSegmentDataStart));
    seekEntries.push_back(WebmElement::SeekEntry(kMkvTracks, mTracksOffset - mSegmentDataStart));
    seekEntries.push_back(WebmElement::SeekEntry(kMkvCues, mCuesOffset - mSegmentDataStart));
    sp<WebmElement> seekHead = new WebmMaster(kMkvSeekHead, seekEntries);

    uint64_t metaSeekSize;
    ::lseek(mFd, mSegmentDataStart, SEEK_SET);
    seekHead->write(mFd, metaSeekSize);

    uint64_t spaceSize;
    sp<WebmElement> space = new EbmlVoid(kMaxMetaSeekSize - metaSeekSize);
    space->write(mFd, spaceSize);

    release();
    return err;
}

status_t WebmWriter::addSource(const sp<MediaSource> &source) {
    Mutex::Autolock l(mLock);
    if (mStarted) {
        ALOGE("Attempt to add source AFTER recording is started");
        return UNKNOWN_ERROR;
    }

    // At most 2 tracks can be supported.
    if (mStreams[kVideoIndex].mTrackEntry != NULL
            && mStreams[kAudioIndex].mTrackEntry != NULL) {
        ALOGE("Too many tracks (2) to add");
        return ERROR_UNSUPPORTED;
    }

    CHECK(source != NULL);

    // A track of type other than video or audio is not supported.
    const char *mime;
    source->getFormat()->findCString(kKeyMIMEType, &mime);
    const char *vp8 = MEDIA_MIMETYPE_VIDEO_VP8;
    const char *vorbis = MEDIA_MIMETYPE_AUDIO_VORBIS;

    size_t streamIndex;
    if (!strncasecmp(mime, vp8, strlen(vp8))) {
        streamIndex = kVideoIndex;
    } else if (!strncasecmp(mime, vorbis, strlen(vorbis))) {
        streamIndex = kAudioIndex;
    } else {
        ALOGE("Track (%s) other than %s or %s is not supported", mime, vp8, vorbis);
        return ERROR_UNSUPPORTED;
    }

    // No more than one video or one audio track is supported.
    if (mStreams[streamIndex].mTrackEntry != NULL) {
        ALOGE("%s track already exists", mStreams[streamIndex].mName);
        return ERROR_UNSUPPORTED;
    }

    // This is the first track of either audio or video.
    // Go ahead to add the track.
    mStreams[streamIndex].mSource = source;
    mStreams[streamIndex].mTrackEntry = mStreams[streamIndex].mMakeTrack(source->getFormat());

    return OK;
}

status_t WebmWriter::start(MetaData *params) {
    if (mInitCheck != OK) {
        return UNKNOWN_ERROR;
    }

    if (mStreams[kVideoIndex].mTrackEntry == NULL
            && mStreams[kAudioIndex].mTrackEntry == NULL) {
        ALOGE("No source added");
        return INVALID_OPERATION;
    }

    if (mMaxFileSizeLimitBytes != 0) {
        mIsFileSizeLimitExplicitlyRequested = true;
    }

    if (params) {
        int32_t isRealTimeRecording;
        params->findInt32(kKeyRealTimeRecording, &isRealTimeRecording);
        mIsRealTimeRecording = isRealTimeRecording;
    }

    if (mStarted) {
        if (mPaused) {
            mPaused = false;
            mStreams[kAudioIndex].mThread->resume();
            mStreams[kVideoIndex].mThread->resume();
        }
        return OK;
    }

    if (params) {
        int32_t tcsl;
        if (params->findInt32(kKeyTimeScale, &tcsl)) {
            mTimeCodeScale = tcsl;
        }
    }
    CHECK_GT(mTimeCodeScale, 0);
    ALOGV("movie time scale: %" PRIu64, mTimeCodeScale);

    /*
     * When the requested file size limit is small, the priority
     * is to meet the file size limit requirement, rather than
     * to make the file streamable. mStreamableFile does not tell
     * whether the actual recorded file is streamable or not.
     */
    mStreamableFile = (!mMaxFileSizeLimitBytes)
        || (mMaxFileSizeLimitBytes >= kMinStreamableFileSizeInBytes);

    /*
     * Write various metadata.
     */
    sp<WebmElement> ebml, segment, info, seekHead, tracks, cues;
    ebml = WebmElement::EbmlHeader();
    segment = new WebmMaster(kMkvSegment);
    seekHead = new EbmlVoid(kMaxMetaSeekSize);
    info = WebmElement::SegmentInfo(mTimeCodeScale, 0);

    List<sp<WebmElement> > children;
    for (size_t i = 0; i < kMaxStreams; ++i) {
        if (mStreams[i].mTrackEntry != NULL) {
            children.push_back(mStreams[i].mTrackEntry);
        }
    }
    tracks = new WebmMaster(kMkvTracks, children);

    if (!mStreamableFile) {
        cues = NULL;
    } else {
        int32_t bitRate = -1;
        if (params) {
            params->findInt32(kKeyBitRate, &bitRate);
        }
        mEstimatedCuesSize = estimateCuesSize(bitRate);
        CHECK_GE(mEstimatedCuesSize, 8);
        cues = new EbmlVoid(mEstimatedCuesSize);
    }

    sp<WebmElement> elems[] = { ebml, segment, seekHead, info, tracks, cues };
    size_t nElems = sizeof(elems) / sizeof(elems[0]);
    uint64_t offsets[nElems];
    uint64_t sizes[nElems];
    for (uint32_t i = 0; i < nElems; i++) {
        WebmElement *e = elems[i].get();
        if (!e) {
            continue;
        }

        uint64_t size;
        offsets[i] = ::lseek(mFd, 0, SEEK_CUR);
        sizes[i] = e->mSize;
        e->write(mFd, size);
    }

    mSegmentOffset = offsets[1];
    mSegmentDataStart = offsets[2];
    mInfoOffset = offsets[3];
    mInfoSize = sizes[3];
    mTracksOffset = offsets[4];
    mCuesOffset = offsets[5];

    // start threads
    if (params) {
        params->findInt64(kKeyTime, &mStartTimestampUs);
    }

    initStream(kAudioIndex);
    initStream(kVideoIndex);

    mStreams[kAudioIndex].mThread->start();
    mStreams[kVideoIndex].mThread->start();
    mSinkThread->start();

    mStarted = true;
    return OK;
}

status_t WebmWriter::pause() {
    if (mInitCheck != OK) {
        return OK;
    }
    mPaused = true;
    status_t err = OK;
    for (int i = 0; i < kMaxStreams; ++i) {
        if (mStreams[i].mThread == NULL) {
            continue;
        }
        status_t status = mStreams[i].mThread->pause();
        if (status != OK) {
            err = status;
        }
    }
    return err;
}

status_t WebmWriter::stop() {
    return reset();
}

bool WebmWriter::reachedEOS() {
    return !mSinkThread->running();
}
} /* namespace android */
