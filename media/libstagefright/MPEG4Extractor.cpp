/*
 * Copyright (C) 2009 The Android Open Source Project
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
#define LOG_TAG "MPEG4Extractor"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <utils/Log.h>

#include "include/MPEG4Extractor.h"
#include "include/SampleTable.h"
#include "include/ESDS.h"

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <utils/String8.h>

#include <byteswap.h>
#include "include/ID3.h"
#include "include/avc_utils.h"

#ifndef UINT32_MAX
#define UINT32_MAX       (4294967295U)
#endif

namespace android {

enum {
    // max track header chunk to return
    kMaxTrackHeaderSize = 32,

    // maximum size of an atom. Some atoms can be bigger according to the spec,
    // but we only allow up to this size.
    kMaxAtomSize = 64 * 1024 * 1024,
};

class MPEG4Source : public MediaSource {
public:
    // Caller retains ownership of both "dataSource" and "sampleTable".
    MPEG4Source(const sp<MPEG4Extractor> &owner,
                const sp<MetaData> &format,
                const sp<DataSource> &dataSource,
                int32_t timeScale,
                const sp<SampleTable> &sampleTable,
                Vector<SidxEntry> &sidx,
                const Trex *trex,
                off64_t firstMoofOffset);

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();

    virtual sp<MetaData> getFormat();

    virtual status_t read(MediaBuffer **buffer, const ReadOptions *options = NULL);
    virtual bool supportNonblockingRead() { return true; }
    virtual status_t fragmentedRead(MediaBuffer **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~MPEG4Source();

private:
    Mutex mLock;

    // keep the MPEG4Extractor around, since we're referencing its data
    sp<MPEG4Extractor> mOwner;
    sp<MetaData> mFormat;
    sp<DataSource> mDataSource;
    int32_t mTimescale;
    sp<SampleTable> mSampleTable;
    uint32_t mCurrentSampleIndex;
    uint32_t mCurrentFragmentIndex;
    Vector<SidxEntry> &mSegments;
    const Trex *mTrex;
    off64_t mFirstMoofOffset;
    off64_t mCurrentMoofOffset;
    off64_t mNextMoofOffset;
    uint32_t mCurrentTime;
    int32_t mLastParsedTrackId;
    int32_t mTrackId;

    int32_t mCryptoMode;    // passed in from extractor
    int32_t mDefaultIVSize; // passed in from extractor
    uint8_t mCryptoKey[16]; // passed in from extractor
    uint32_t mCurrentAuxInfoType;
    uint32_t mCurrentAuxInfoTypeParameter;
    int32_t mCurrentDefaultSampleInfoSize;
    uint32_t mCurrentSampleInfoCount;
    uint32_t mCurrentSampleInfoAllocSize;
    uint8_t* mCurrentSampleInfoSizes;
    uint32_t mCurrentSampleInfoOffsetCount;
    uint32_t mCurrentSampleInfoOffsetsAllocSize;
    uint64_t* mCurrentSampleInfoOffsets;

    bool mIsAVC;
    bool mIsHEVC;
    size_t mNALLengthSize;

    bool mStarted;

    MediaBufferGroup *mGroup;

    MediaBuffer *mBuffer;

    bool mWantsNALFragments;

    uint8_t *mSrcBuffer;

    size_t parseNALSize(const uint8_t *data) const;
    status_t parseChunk(off64_t *offset);
    status_t parseTrackFragmentHeader(off64_t offset, off64_t size);
    status_t parseTrackFragmentRun(off64_t offset, off64_t size);
    status_t parseSampleAuxiliaryInformationSizes(off64_t offset, off64_t size);
    status_t parseSampleAuxiliaryInformationOffsets(off64_t offset, off64_t size);

    struct TrackFragmentHeaderInfo {
        enum Flags {
            kBaseDataOffsetPresent         = 0x01,
            kSampleDescriptionIndexPresent = 0x02,
            kDefaultSampleDurationPresent  = 0x08,
            kDefaultSampleSizePresent      = 0x10,
            kDefaultSampleFlagsPresent     = 0x20,
            kDurationIsEmpty               = 0x10000,
        };

        uint32_t mTrackID;
        uint32_t mFlags;
        uint64_t mBaseDataOffset;
        uint32_t mSampleDescriptionIndex;
        uint32_t mDefaultSampleDuration;
        uint32_t mDefaultSampleSize;
        uint32_t mDefaultSampleFlags;

        uint64_t mDataOffset;
    };
    TrackFragmentHeaderInfo mTrackFragmentHeaderInfo;

    struct Sample {
        off64_t offset;
        size_t size;
        uint32_t duration;
        int32_t compositionOffset;
        uint8_t iv[16];
        Vector<size_t> clearsizes;
        Vector<size_t> encryptedsizes;
    };
    Vector<Sample> mCurrentSamples;

    MPEG4Source(const MPEG4Source &);
    MPEG4Source &operator=(const MPEG4Source &);
};

// This custom data source wraps an existing one and satisfies requests
// falling entirely within a cached range from the cache while forwarding
// all remaining requests to the wrapped datasource.
// This is used to cache the full sampletable metadata for a single track,
// possibly wrapping multiple times to cover all tracks, i.e.
// Each MPEG4DataSource caches the sampletable metadata for a single track.

struct MPEG4DataSource : public DataSource {
    MPEG4DataSource(const sp<DataSource> &source);

    virtual status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual status_t getSize(off64_t *size);
    virtual uint32_t flags();

    status_t setCachedRange(off64_t offset, size_t size);

protected:
    virtual ~MPEG4DataSource();

private:
    Mutex mLock;

    sp<DataSource> mSource;
    off64_t mCachedOffset;
    size_t mCachedSize;
    uint8_t *mCache;

    void clearCache();

    MPEG4DataSource(const MPEG4DataSource &);
    MPEG4DataSource &operator=(const MPEG4DataSource &);
};

MPEG4DataSource::MPEG4DataSource(const sp<DataSource> &source)
    : mSource(source),
      mCachedOffset(0),
      mCachedSize(0),
      mCache(NULL) {
}

MPEG4DataSource::~MPEG4DataSource() {
    clearCache();
}

void MPEG4DataSource::clearCache() {
    if (mCache) {
        free(mCache);
        mCache = NULL;
    }

    mCachedOffset = 0;
    mCachedSize = 0;
}

status_t MPEG4DataSource::initCheck() const {
    return mSource->initCheck();
}

ssize_t MPEG4DataSource::readAt(off64_t offset, void *data, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (isInRange(mCachedOffset, mCachedSize, offset, size)) {
        memcpy(data, &mCache[offset - mCachedOffset], size);
        return size;
    }

    return mSource->readAt(offset, data, size);
}

status_t MPEG4DataSource::getSize(off64_t *size) {
    return mSource->getSize(size);
}

uint32_t MPEG4DataSource::flags() {
    return mSource->flags();
}

status_t MPEG4DataSource::setCachedRange(off64_t offset, size_t size) {
    Mutex::Autolock autoLock(mLock);

    clearCache();

    mCache = (uint8_t *)malloc(size);

    if (mCache == NULL) {
        return -ENOMEM;
    }

    mCachedOffset = offset;
    mCachedSize = size;

    ssize_t err = mSource->readAt(mCachedOffset, mCache, mCachedSize);

    if (err < (ssize_t)size) {
        clearCache();

        return ERROR_IO;
    }

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

static const bool kUseHexDump = false;

static void hexdump(const void *_data, size_t size) {
    const uint8_t *data = (const uint8_t *)_data;
    size_t offset = 0;
    while (offset < size) {
        printf("0x%04zx  ", offset);

        size_t n = size - offset;
        if (n > 16) {
            n = 16;
        }

        for (size_t i = 0; i < 16; ++i) {
            if (i == 8) {
                printf(" ");
            }

            if (offset + i < size) {
                printf("%02x ", data[offset + i]);
            } else {
                printf("   ");
            }
        }

        printf(" ");

        for (size_t i = 0; i < n; ++i) {
            if (isprint(data[offset + i])) {
                printf("%c", data[offset + i]);
            } else {
                printf(".");
            }
        }

        printf("\n");

        offset += 16;
    }
}

static const char *FourCC2MIME(uint32_t fourcc) {
    switch (fourcc) {
        case FOURCC('m', 'p', '4', 'a'):
            return MEDIA_MIMETYPE_AUDIO_AAC;

        case FOURCC('.', 'm', 'p', '3'):
            return MEDIA_MIMETYPE_AUDIO_MPEG;

        case FOURCC('s', 'a', 'm', 'r'):
            return MEDIA_MIMETYPE_AUDIO_AMR_NB;

        case FOURCC('s', 'a', 'w', 'b'):
            return MEDIA_MIMETYPE_AUDIO_AMR_WB;

        case FOURCC('m', 'p', '4', 'v'):
            return MEDIA_MIMETYPE_VIDEO_MPEG4;

        case FOURCC('s', '2', '6', '3'):
        case FOURCC('h', '2', '6', '3'):
        case FOURCC('H', '2', '6', '3'):
            return MEDIA_MIMETYPE_VIDEO_H263;

        case FOURCC('a', 'v', 'c', '1'):
            return MEDIA_MIMETYPE_VIDEO_AVC;

        case FOURCC('h', 'v', 'c', '1'):
        case FOURCC('h', 'e', 'v', '1'):
            return MEDIA_MIMETYPE_VIDEO_HEVC;
        default:
            CHECK(!"should not be here.");
            return NULL;
    }
}

static bool AdjustChannelsAndRate(uint32_t fourcc, uint32_t *channels, uint32_t *rate) {
    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, FourCC2MIME(fourcc))) {
        // AMR NB audio is always mono, 8kHz
        *channels = 1;
        *rate = 8000;
        return true;
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, FourCC2MIME(fourcc))) {
        // AMR WB audio is always mono, 16kHz
        *channels = 1;
        *rate = 16000;
        return true;
    }
    return false;
}

MPEG4Extractor::MPEG4Extractor(const sp<DataSource> &source)
    : mMoofOffset(0),
      mMoofFound(false),
      mMdatFound(false),
      mDataSource(source),
      mInitCheck(NO_INIT),
      mHasVideo(false),
      mHeaderTimescale(0),
      mIsQT(false),
      mFirstTrack(NULL),
      mLastTrack(NULL),
      mFileMetaData(new MetaData),
      mFirstSINF(NULL),
      mIsDrm(false) {
}

MPEG4Extractor::~MPEG4Extractor() {
    Track *track = mFirstTrack;
    while (track) {
        Track *next = track->next;

        delete track;
        track = next;
    }
    mFirstTrack = mLastTrack = NULL;

    SINF *sinf = mFirstSINF;
    while (sinf) {
        SINF *next = sinf->next;
        delete[] sinf->IPMPData;
        delete sinf;
        sinf = next;
    }
    mFirstSINF = NULL;

    for (size_t i = 0; i < mPssh.size(); i++) {
        delete [] mPssh[i].data;
    }
}

uint32_t MPEG4Extractor::flags() const {
    return CAN_PAUSE |
            ((mMoofOffset == 0 || mSidxEntries.size() != 0) ?
                    (CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK) : 0);
}

sp<MetaData> MPEG4Extractor::getMetaData() {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return new MetaData;
    }

    return mFileMetaData;
}

size_t MPEG4Extractor::countTracks() {
    status_t err;
    if ((err = readMetaData()) != OK) {
        ALOGV("MPEG4Extractor::countTracks: no tracks");
        return 0;
    }

    size_t n = 0;
    Track *track = mFirstTrack;
    while (track) {
        ++n;
        track = track->next;
    }

    ALOGV("MPEG4Extractor::countTracks: %zu tracks", n);
    return n;
}

sp<MetaData> MPEG4Extractor::getTrackMetaData(
        size_t index, uint32_t flags) {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return NULL;
    }

    Track *track = mFirstTrack;
    while (index > 0) {
        if (track == NULL) {
            return NULL;
        }

        track = track->next;
        --index;
    }

    if (track == NULL) {
        return NULL;
    }

    if ((flags & kIncludeExtensiveMetaData)
            && !track->includes_expensive_metadata) {
        track->includes_expensive_metadata = true;

        const char *mime;
        CHECK(track->meta->findCString(kKeyMIMEType, &mime));
        if (!strncasecmp("video/", mime, 6)) {
            if (mMoofOffset > 0) {
                int64_t duration;
                if (track->meta->findInt64(kKeyDuration, &duration)) {
                    // nothing fancy, just pick a frame near 1/4th of the duration
                    track->meta->setInt64(
                            kKeyThumbnailTime, duration / 4);
                }
            } else {
                uint32_t sampleIndex;
                uint32_t sampleTime;
                if (track->sampleTable->findThumbnailSample(&sampleIndex) == OK
                        && track->sampleTable->getMetaDataForSample(
                            sampleIndex, NULL /* offset */, NULL /* size */,
                            &sampleTime) == OK) {
                    track->meta->setInt64(
                            kKeyThumbnailTime,
                            ((int64_t)sampleTime * 1000000) / track->timescale);
                }
            }

            // MPEG2 tracks do not provide CSD, so read the stream header
            if (!strcmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG2)) {
                off64_t offset;
                size_t size;
                if (track->sampleTable->getMetaDataForSample(
                            0 /* sampleIndex */, &offset, &size, NULL /* sampleTime */) == OK) {
                    if (size > kMaxTrackHeaderSize) {
                        size = kMaxTrackHeaderSize;
                    }
                    uint8_t header[kMaxTrackHeaderSize];
                    if (mDataSource->readAt(offset, &header, size) == (ssize_t)size) {
                        track->meta->setData(kKeyStreamHeader, 'mdat', header, size);
                    }
                }
            }
        }
    }

    return track->meta;
}

static void MakeFourCCString(uint32_t x, char *s) {
    s[0] = x >> 24;
    s[1] = (x >> 16) & 0xff;
    s[2] = (x >> 8) & 0xff;
    s[3] = x & 0xff;
    s[4] = '\0';
}

status_t MPEG4Extractor::readMetaData() {
    if (mInitCheck != NO_INIT) {
        return mInitCheck;
    }

    off64_t offset = 0;
    status_t err;
    bool sawMoovOrSidx = false;

    while (!(sawMoovOrSidx && (mMdatFound || mMoofFound))) {
        off64_t orig_offset = offset;
        err = parseChunk(&offset, 0);

        if (err != OK && err != UNKNOWN_ERROR) {
            break;
        } else if (offset <= orig_offset) {
            // only continue parsing if the offset was advanced,
            // otherwise we might end up in an infinite loop
            ALOGE("did not advance: %lld->%lld", (long long)orig_offset, (long long)offset);
            err = ERROR_MALFORMED;
            break;
        } else if (err == UNKNOWN_ERROR) {
            sawMoovOrSidx = true;
        }
    }

    if (mInitCheck == OK) {
        if (mHasVideo) {
            mFileMetaData->setCString(
                    kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_MPEG4);
        } else {
            mFileMetaData->setCString(kKeyMIMEType, "audio/mp4");
        }
    } else {
        mInitCheck = err;
    }

    CHECK_NE(err, (status_t)NO_INIT);

    // copy pssh data into file metadata
    uint64_t psshsize = 0;
    for (size_t i = 0; i < mPssh.size(); i++) {
        psshsize += 20 + mPssh[i].datalen;
    }
    if (psshsize > 0 && psshsize <= UINT32_MAX) {
        char *buf = (char*)malloc(psshsize);
        if (!buf) {
            ALOGE("b/28471206");
            return NO_MEMORY;
        }
        char *ptr = buf;
        for (size_t i = 0; i < mPssh.size(); i++) {
            memcpy(ptr, mPssh[i].uuid, 20); // uuid + length
            memcpy(ptr + 20, mPssh[i].data, mPssh[i].datalen);
            ptr += (20 + mPssh[i].datalen);
        }
        mFileMetaData->setData(kKeyPssh, 'pssh', buf, psshsize);
        free(buf);
    }
    return mInitCheck;
}

char* MPEG4Extractor::getDrmTrackInfo(size_t trackID, int *len) {
    if (mFirstSINF == NULL) {
        return NULL;
    }

    SINF *sinf = mFirstSINF;
    while (sinf && (trackID != sinf->trackID)) {
        sinf = sinf->next;
    }

    if (sinf == NULL) {
        return NULL;
    }

    *len = sinf->len;
    return sinf->IPMPData;
}

// Reads an encoded integer 7 bits at a time until it encounters the high bit clear.
static int32_t readSize(off64_t offset,
        const sp<DataSource> DataSource, uint8_t *numOfBytes) {
    uint32_t size = 0;
    uint8_t data;
    bool moreData = true;
    *numOfBytes = 0;

    while (moreData) {
        if (DataSource->readAt(offset, &data, 1) < 1) {
            return -1;
        }
        offset ++;
        moreData = (data >= 128) ? true : false;
        size = (size << 7) | (data & 0x7f); // Take last 7 bits
        (*numOfBytes) ++;
    }

    return size;
}

status_t MPEG4Extractor::parseDrmSINF(
        off64_t * /* offset */, off64_t data_offset) {
    uint8_t updateIdTag;
    if (mDataSource->readAt(data_offset, &updateIdTag, 1) < 1) {
        return ERROR_IO;
    }
    data_offset ++;

    if (0x01/*OBJECT_DESCRIPTOR_UPDATE_ID_TAG*/ != updateIdTag) {
        return ERROR_MALFORMED;
    }

    uint8_t numOfBytes;
    int32_t size = readSize(data_offset, mDataSource, &numOfBytes);
    if (size < 0) {
        return ERROR_IO;
    }
    data_offset += numOfBytes;

    while(size >= 11 ) {
        uint8_t descriptorTag;
        if (mDataSource->readAt(data_offset, &descriptorTag, 1) < 1) {
            return ERROR_IO;
        }
        data_offset ++;

        if (0x11/*OBJECT_DESCRIPTOR_ID_TAG*/ != descriptorTag) {
            return ERROR_MALFORMED;
        }

        uint8_t buffer[8];
        //ObjectDescriptorID and ObjectDescriptor url flag
        if (mDataSource->readAt(data_offset, buffer, 2) < 2) {
            return ERROR_IO;
        }
        data_offset += 2;

        if ((buffer[1] >> 5) & 0x0001) { //url flag is set
            return ERROR_MALFORMED;
        }

        if (mDataSource->readAt(data_offset, buffer, 8) < 8) {
            return ERROR_IO;
        }
        data_offset += 8;

        if ((0x0F/*ES_ID_REF_TAG*/ != buffer[1])
                || ( 0x0A/*IPMP_DESCRIPTOR_POINTER_ID_TAG*/ != buffer[5])) {
            return ERROR_MALFORMED;
        }

        SINF *sinf = new SINF;
        sinf->trackID = U16_AT(&buffer[3]);
        sinf->IPMPDescriptorID = buffer[7];
        sinf->next = mFirstSINF;
        mFirstSINF = sinf;

        size -= (8 + 2 + 1);
    }

    if (size != 0) {
        return ERROR_MALFORMED;
    }

    if (mDataSource->readAt(data_offset, &updateIdTag, 1) < 1) {
        return ERROR_IO;
    }
    data_offset ++;

    if(0x05/*IPMP_DESCRIPTOR_UPDATE_ID_TAG*/ != updateIdTag) {
        return ERROR_MALFORMED;
    }

    size = readSize(data_offset, mDataSource, &numOfBytes);
    if (size < 0) {
        return ERROR_IO;
    }
    data_offset += numOfBytes;

    while (size > 0) {
        uint8_t tag;
        int32_t dataLen;
        if (mDataSource->readAt(data_offset, &tag, 1) < 1) {
            return ERROR_IO;
        }
        data_offset ++;

        if (0x0B/*IPMP_DESCRIPTOR_ID_TAG*/ == tag) {
            uint8_t id;
            dataLen = readSize(data_offset, mDataSource, &numOfBytes);
            if (dataLen < 0) {
                return ERROR_IO;
            } else if (dataLen < 4) {
                return ERROR_MALFORMED;
            }
            data_offset += numOfBytes;

            if (mDataSource->readAt(data_offset, &id, 1) < 1) {
                return ERROR_IO;
            }
            data_offset ++;

            SINF *sinf = mFirstSINF;
            while (sinf && (sinf->IPMPDescriptorID != id)) {
                sinf = sinf->next;
            }
            if (sinf == NULL) {
                return ERROR_MALFORMED;
            }
            sinf->len = dataLen - 3;
            sinf->IPMPData = new (std::nothrow) char[sinf->len];
            if (sinf->IPMPData == NULL) {
                return ERROR_MALFORMED;
            }
            data_offset += 2;

            if (mDataSource->readAt(data_offset, sinf->IPMPData, sinf->len) < sinf->len) {
                return ERROR_IO;
            }
            data_offset += sinf->len;

            size -= (dataLen + numOfBytes + 1);
        }
    }

    if (size != 0) {
        return ERROR_MALFORMED;
    }

    return UNKNOWN_ERROR;  // Return a dummy error.
}

struct PathAdder {
    PathAdder(Vector<uint32_t> *path, uint32_t chunkType)
        : mPath(path) {
        mPath->push(chunkType);
    }

    ~PathAdder() {
        mPath->pop();
    }

private:
    Vector<uint32_t> *mPath;

    PathAdder(const PathAdder &);
    PathAdder &operator=(const PathAdder &);
};

static bool underMetaDataPath(const Vector<uint32_t> &path) {
    return path.size() >= 5
        && path[0] == FOURCC('m', 'o', 'o', 'v')
        && path[1] == FOURCC('u', 'd', 't', 'a')
        && path[2] == FOURCC('m', 'e', 't', 'a')
        && path[3] == FOURCC('i', 'l', 's', 't');
}

static bool underQTMetaPath(const Vector<uint32_t> &path, int32_t depth) {
    return path.size() >= 2
            && path[0] == FOURCC('m', 'o', 'o', 'v')
            && path[1] == FOURCC('m', 'e', 't', 'a')
            && (depth == 2
            || (depth == 3
                    && (path[2] == FOURCC('h', 'd', 'l', 'r')
                    ||  path[2] == FOURCC('i', 'l', 's', 't')
                    ||  path[2] == FOURCC('k', 'e', 'y', 's'))));
}

// Given a time in seconds since Jan 1 1904, produce a human-readable string.
static bool convertTimeToDate(int64_t time_1904, String8 *s) {
    // delta between mpeg4 time and unix epoch time
    static const int64_t delta = (((66 * 365 + 17) * 24) * 3600);
    if (time_1904 < INT64_MIN + delta) {
        return false;
    }
    time_t time_1970 = time_1904 - delta;

    char tmp[32];
    struct tm* tm = gmtime(&time_1970);
    if (tm != NULL &&
            strftime(tmp, sizeof(tmp), "%Y%m%dT%H%M%S.000Z", tm) > 0) {
        s->setTo(tmp);
        return true;
    }
    return false;
}

status_t MPEG4Extractor::parseChunk(off64_t *offset, int depth) {
    ALOGV("entering parseChunk %lld/%d", (long long)*offset, depth);

    if (*offset < 0) {
        ALOGE("b/23540914");
        return ERROR_MALFORMED;
    }
    uint32_t hdr[2];
    if (mDataSource->readAt(*offset, hdr, 8) < 8) {
        return ERROR_IO;
    }
    uint64_t chunk_size = ntohl(hdr[0]);
    int32_t chunk_type = ntohl(hdr[1]);
    off64_t data_offset = *offset + 8;

    if (chunk_size == 1) {
        if (mDataSource->readAt(*offset + 8, &chunk_size, 8) < 8) {
            return ERROR_IO;
        }
        chunk_size = ntoh64(chunk_size);
        data_offset += 8;

        if (chunk_size < 16) {
            // The smallest valid chunk is 16 bytes long in this case.
            return ERROR_MALFORMED;
        }
    } else if (chunk_size == 0) {
        if (depth == 0) {
            // atom extends to end of file
            off64_t sourceSize;
            if (mDataSource->getSize(&sourceSize) == OK) {
                chunk_size = (sourceSize - *offset);
            } else {
                // XXX could we just pick a "sufficiently large" value here?
                ALOGE("atom size is 0, and data source has no size");
                return ERROR_MALFORMED;
            }
        } else {
            // not allowed for non-toplevel atoms, skip it
            *offset += 4;
            return OK;
        }
    } else if (chunk_size < 8) {
        // The smallest valid chunk is 8 bytes long.
        ALOGE("invalid chunk size: %" PRIu64, chunk_size);
        return ERROR_MALFORMED;
    }

    char chunk[5];
    MakeFourCCString(chunk_type, chunk);
    ALOGV("chunk: %s @ %lld, %d", chunk, (long long)*offset, depth);

    if (kUseHexDump) {
        static const char kWhitespace[] = "                                        ";
        const char *indent = &kWhitespace[sizeof(kWhitespace) - 1 - 2 * depth];
        printf("%sfound chunk '%s' of size %" PRIu64 "\n", indent, chunk, chunk_size);

        char buffer[256];
        size_t n = chunk_size;
        if (n > sizeof(buffer)) {
            n = sizeof(buffer);
        }
        if (mDataSource->readAt(*offset, buffer, n)
                < (ssize_t)n) {
            return ERROR_IO;
        }

        hexdump(buffer, n);
    }

    PathAdder autoAdder(&mPath, chunk_type);

    // (data_offset - *offset) is either 8 or 16
    off64_t chunk_data_size = chunk_size - (data_offset - *offset);
    if (chunk_data_size < 0) {
        ALOGE("b/23540914");
        return ERROR_MALFORMED;
    }
    if (chunk_type != FOURCC('m', 'd', 'a', 't') && chunk_data_size > kMaxAtomSize) {
        char errMsg[100];
        sprintf(errMsg, "%s atom has size %" PRId64, chunk, chunk_data_size);
        ALOGE("%s (b/28615448)", errMsg);
        android_errorWriteWithInfoLog(0x534e4554, "28615448", -1, errMsg, strlen(errMsg));
        return ERROR_MALFORMED;
    }

    if (chunk_type != FOURCC('c', 'p', 'r', 't')
            && chunk_type != FOURCC('c', 'o', 'v', 'r')
            && mPath.size() == 5 && underMetaDataPath(mPath)) {
        off64_t stop_offset = *offset + chunk_size;
        *offset = data_offset;
        while (*offset < stop_offset) {
            status_t err = parseChunk(offset, depth + 1);
            if (err != OK) {
                return err;
            }
        }

        if (*offset != stop_offset) {
            return ERROR_MALFORMED;
        }

        return OK;
    }

    switch(chunk_type) {
        case FOURCC('m', 'o', 'o', 'v'):
        case FOURCC('t', 'r', 'a', 'k'):
        case FOURCC('m', 'd', 'i', 'a'):
        case FOURCC('m', 'i', 'n', 'f'):
        case FOURCC('d', 'i', 'n', 'f'):
        case FOURCC('s', 't', 'b', 'l'):
        case FOURCC('m', 'v', 'e', 'x'):
        case FOURCC('m', 'o', 'o', 'f'):
        case FOURCC('t', 'r', 'a', 'f'):
        case FOURCC('m', 'f', 'r', 'a'):
        case FOURCC('u', 'd', 't', 'a'):
        case FOURCC('i', 'l', 's', 't'):
        case FOURCC('s', 'i', 'n', 'f'):
        case FOURCC('s', 'c', 'h', 'i'):
        case FOURCC('e', 'd', 't', 's'):
        case FOURCC('w', 'a', 'v', 'e'):
        {
            if (chunk_type == FOURCC('m', 'o', 'o', 'f') && !mMoofFound) {
                // store the offset of the first segment
                mMoofFound = true;
                mMoofOffset = *offset;
            }

            if (chunk_type == FOURCC('s', 't', 'b', 'l')) {
                ALOGV("sampleTable chunk is %" PRIu64 " bytes long.", chunk_size);

                if (mDataSource->flags()
                        & (DataSource::kWantsPrefetching
                            | DataSource::kIsCachingDataSource)) {
                    sp<MPEG4DataSource> cachedSource =
                        new MPEG4DataSource(mDataSource);

                    if (cachedSource->setCachedRange(*offset, chunk_size) == OK) {
                        mDataSource = cachedSource;
                    }
                }

                if (mLastTrack == NULL)
                    return ERROR_MALFORMED;

                mLastTrack->sampleTable = new SampleTable(mDataSource);
            }

            bool isTrack = false;
            if (chunk_type == FOURCC('t', 'r', 'a', 'k')) {
                isTrack = true;

                Track *track = new Track;
                track->next = NULL;
                if (mLastTrack) {
                    mLastTrack->next = track;
                } else {
                    mFirstTrack = track;
                }
                mLastTrack = track;

                track->meta = new MetaData;
                track->includes_expensive_metadata = false;
                track->skipTrack = false;
                track->timescale = 0;
                track->meta->setCString(kKeyMIMEType, "application/octet-stream");
            }

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset;
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }

            if (isTrack) {
                int32_t trackId;
                // There must be exact one track header per track.
                if (!mLastTrack->meta->findInt32(kKeyTrackID, &trackId)) {
                    mLastTrack->skipTrack = true;
                }
                if (mLastTrack->skipTrack) {
                    Track *cur = mFirstTrack;

                    if (cur == mLastTrack) {
                        delete cur;
                        mFirstTrack = mLastTrack = NULL;
                    } else {
                        while (cur && cur->next != mLastTrack) {
                            cur = cur->next;
                        }
                        cur->next = NULL;
                        delete mLastTrack;
                        mLastTrack = cur;
                    }

                    return OK;
                }

                status_t err = verifyTrack(mLastTrack);

                if (err != OK) {
                    return err;
                }
            } else if (chunk_type == FOURCC('m', 'o', 'o', 'v')) {
                mInitCheck = OK;

                if (!mIsDrm) {
                    return UNKNOWN_ERROR;  // Return a dummy error.
                } else {
                    return OK;
                }
            }
            break;
        }

        case FOURCC('e', 'l', 's', 't'):
        {
            *offset += chunk_size;

            // See 14496-12 8.6.6
            uint8_t version;
            if (mDataSource->readAt(data_offset, &version, 1) < 1) {
                return ERROR_IO;
            }

            uint32_t entry_count;
            if (!mDataSource->getUInt32(data_offset + 4, &entry_count)) {
                return ERROR_IO;
            }

            if (entry_count != 1) {
                // we only support a single entry at the moment, for gapless playback
                ALOGW("ignoring edit list with %d entries", entry_count);
            } else if (mHeaderTimescale == 0) {
                ALOGW("ignoring edit list because timescale is 0");
            } else {
                off64_t entriesoffset = data_offset + 8;
                uint64_t segment_duration;
                int64_t media_time;

                if (version == 1) {
                    if (!mDataSource->getUInt64(entriesoffset, &segment_duration) ||
                            !mDataSource->getUInt64(entriesoffset + 8, (uint64_t*)&media_time)) {
                        return ERROR_IO;
                    }
                } else if (version == 0) {
                    uint32_t sd;
                    int32_t mt;
                    if (!mDataSource->getUInt32(entriesoffset, &sd) ||
                            !mDataSource->getUInt32(entriesoffset + 4, (uint32_t*)&mt)) {
                        return ERROR_IO;
                    }
                    segment_duration = sd;
                    media_time = mt;
                } else {
                    return ERROR_IO;
                }

                uint64_t halfscale = mHeaderTimescale / 2;
                segment_duration = (segment_duration * 1000000 + halfscale)/ mHeaderTimescale;
                media_time = (media_time * 1000000 + halfscale) / mHeaderTimescale;

                int64_t duration;
                int32_t samplerate;
                if (!mLastTrack) {
                    return ERROR_MALFORMED;
                }
                if (mLastTrack->meta->findInt64(kKeyDuration, &duration) &&
                        mLastTrack->meta->findInt32(kKeySampleRate, &samplerate)) {

                    int64_t delay = (media_time  * samplerate + 500000) / 1000000;
                    mLastTrack->meta->setInt32(kKeyEncoderDelay, delay);

                    int64_t paddingus = duration - (int64_t)(segment_duration + media_time);
                    if (paddingus < 0) {
                        // track duration from media header (which is what kKeyDuration is) might
                        // be slightly shorter than the segment duration, which would make the
                        // padding negative. Clamp to zero.
                        paddingus = 0;
                    }
                    int64_t paddingsamples = (paddingus * samplerate + 500000) / 1000000;
                    mLastTrack->meta->setInt32(kKeyEncoderPadding, paddingsamples);
                }
            }
            break;
        }

        case FOURCC('f', 'r', 'm', 'a'):
        {
            *offset += chunk_size;

            uint32_t original_fourcc;
            if (mDataSource->readAt(data_offset, &original_fourcc, 4) < 4) {
                return ERROR_IO;
            }
            original_fourcc = ntohl(original_fourcc);
            ALOGV("read original format: %d", original_fourcc);

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            mLastTrack->meta->setCString(kKeyMIMEType, FourCC2MIME(original_fourcc));
            uint32_t num_channels = 0;
            uint32_t sample_rate = 0;
            if (AdjustChannelsAndRate(original_fourcc, &num_channels, &sample_rate)) {
                mLastTrack->meta->setInt32(kKeyChannelCount, num_channels);
                mLastTrack->meta->setInt32(kKeySampleRate, sample_rate);
            }
            break;
        }

        case FOURCC('t', 'e', 'n', 'c'):
        {
            *offset += chunk_size;

            if (chunk_size < 32) {
                return ERROR_MALFORMED;
            }

            // tenc box contains 1 byte version, 3 byte flags, 3 byte default algorithm id, one byte
            // default IV size, 16 bytes default KeyID
            // (ISO 23001-7)
            char buf[4];
            memset(buf, 0, 4);
            if (mDataSource->readAt(data_offset + 4, buf + 1, 3) < 3) {
                return ERROR_IO;
            }
            uint32_t defaultAlgorithmId = ntohl(*((int32_t*)buf));
            if (defaultAlgorithmId > 1) {
                // only 0 (clear) and 1 (AES-128) are valid
                return ERROR_MALFORMED;
            }

            memset(buf, 0, 4);
            if (mDataSource->readAt(data_offset + 7, buf + 3, 1) < 1) {
                return ERROR_IO;
            }
            uint32_t defaultIVSize = ntohl(*((int32_t*)buf));

            if ((defaultAlgorithmId == 0 && defaultIVSize != 0) ||
                    (defaultAlgorithmId != 0 && defaultIVSize == 0)) {
                // only unencrypted data must have 0 IV size
                return ERROR_MALFORMED;
            } else if (defaultIVSize != 0 &&
                    defaultIVSize != 8 &&
                    defaultIVSize != 16) {
                // only supported sizes are 0, 8 and 16
                return ERROR_MALFORMED;
            }

            uint8_t defaultKeyId[16];

            if (mDataSource->readAt(data_offset + 8, &defaultKeyId, 16) < 16) {
                return ERROR_IO;
            }

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            mLastTrack->meta->setInt32(kKeyCryptoMode, defaultAlgorithmId);
            mLastTrack->meta->setInt32(kKeyCryptoDefaultIVSize, defaultIVSize);
            mLastTrack->meta->setData(kKeyCryptoKey, 'tenc', defaultKeyId, 16);
            break;
        }

        case FOURCC('t', 'k', 'h', 'd'):
        {
            *offset += chunk_size;

            status_t err;
            if ((err = parseTrackHeader(data_offset, chunk_data_size)) != OK) {
                return err;
            }

            break;
        }

        case FOURCC('p', 's', 's', 'h'):
        {
            *offset += chunk_size;

            PsshInfo pssh;

            if (mDataSource->readAt(data_offset + 4, &pssh.uuid, 16) < 16) {
                return ERROR_IO;
            }

            uint32_t psshdatalen = 0;
            if (mDataSource->readAt(data_offset + 20, &psshdatalen, 4) < 4) {
                return ERROR_IO;
            }
            pssh.datalen = ntohl(psshdatalen);
            ALOGV("pssh data size: %d", pssh.datalen);
            if (chunk_size < 20 || pssh.datalen > chunk_size - 20) {
                // pssh data length exceeds size of containing box
                return ERROR_MALFORMED;
            }

            pssh.data = new (std::nothrow) uint8_t[pssh.datalen];
            if (pssh.data == NULL) {
                return ERROR_MALFORMED;
            }
            ALOGV("allocated pssh @ %p", pssh.data);
            ssize_t requested = (ssize_t) pssh.datalen;
            if (mDataSource->readAt(data_offset + 24, pssh.data, requested) < requested) {
                return ERROR_IO;
            }
            mPssh.push_back(pssh);

            break;
        }

        case FOURCC('m', 'd', 'h', 'd'):
        {
            *offset += chunk_size;

            if (chunk_data_size < 4 || mLastTrack == NULL) {
                return ERROR_MALFORMED;
            }

            uint8_t version;
            if (mDataSource->readAt(
                        data_offset, &version, sizeof(version))
                    < (ssize_t)sizeof(version)) {
                return ERROR_IO;
            }

            off64_t timescale_offset;

            if (version == 1) {
                timescale_offset = data_offset + 4 + 16;
            } else if (version == 0) {
                timescale_offset = data_offset + 4 + 8;
            } else {
                return ERROR_IO;
            }

            uint32_t timescale;
            if (mDataSource->readAt(
                        timescale_offset, &timescale, sizeof(timescale))
                    < (ssize_t)sizeof(timescale)) {
                return ERROR_IO;
            }

            if (!timescale) {
                ALOGE("timescale should not be ZERO.");
                return ERROR_MALFORMED;
            }

            mLastTrack->timescale = ntohl(timescale);

            // 14496-12 says all ones means indeterminate, but some files seem to use
            // 0 instead. We treat both the same.
            int64_t duration = 0;
            if (version == 1) {
                if (mDataSource->readAt(
                            timescale_offset + 4, &duration, sizeof(duration))
                        < (ssize_t)sizeof(duration)) {
                    return ERROR_IO;
                }
                if (duration != -1) {
                    duration = ntoh64(duration);
                }
            } else {
                uint32_t duration32;
                if (mDataSource->readAt(
                            timescale_offset + 4, &duration32, sizeof(duration32))
                        < (ssize_t)sizeof(duration32)) {
                    return ERROR_IO;
                }
                if (duration32 != 0xffffffff) {
                    duration = ntohl(duration32);
                }
            }
            if (duration != 0 && mLastTrack->timescale != 0) {
                mLastTrack->meta->setInt64(
                        kKeyDuration, (duration * 1000000) / mLastTrack->timescale);
            }

            uint8_t lang[2];
            off64_t lang_offset;
            if (version == 1) {
                lang_offset = timescale_offset + 4 + 8;
            } else if (version == 0) {
                lang_offset = timescale_offset + 4 + 4;
            } else {
                return ERROR_IO;
            }

            if (mDataSource->readAt(lang_offset, &lang, sizeof(lang))
                    < (ssize_t)sizeof(lang)) {
                return ERROR_IO;
            }

            // To get the ISO-639-2/T three character language code
            // 1 bit pad followed by 3 5-bits characters. Each character
            // is packed as the difference between its ASCII value and 0x60.
            char lang_code[4];
            lang_code[0] = ((lang[0] >> 2) & 0x1f) + 0x60;
            lang_code[1] = ((lang[0] & 0x3) << 3 | (lang[1] >> 5)) + 0x60;
            lang_code[2] = (lang[1] & 0x1f) + 0x60;
            lang_code[3] = '\0';

            mLastTrack->meta->setCString(
                    kKeyMediaLanguage, lang_code);

            break;
        }

        case FOURCC('s', 't', 's', 'd'):
        {
            if (chunk_data_size < 8) {
                return ERROR_MALFORMED;
            }

            uint8_t buffer[8];
            if (chunk_data_size < (off64_t)sizeof(buffer)) {
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, 8) < 8) {
                return ERROR_IO;
            }

            if (U32_AT(buffer) != 0) {
                // Should be version 0, flags 0.
                return ERROR_MALFORMED;
            }

            uint32_t entry_count = U32_AT(&buffer[4]);

            if (entry_count > 1) {
                // For 3GPP timed text, there could be multiple tx3g boxes contain
                // multiple text display formats. These formats will be used to
                // display the timed text.
                // For encrypted files, there may also be more than one entry.
                const char *mime;

                if (mLastTrack == NULL)
                    return ERROR_MALFORMED;

                CHECK(mLastTrack->meta->findCString(kKeyMIMEType, &mime));
                if (strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP) &&
                        strcasecmp(mime, "application/octet-stream")) {
                    // For now we only support a single type of media per track.
                    mLastTrack->skipTrack = true;
                    *offset += chunk_size;
                    break;
                }
            }
            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset + 8;
            for (uint32_t i = 0; i < entry_count; ++i) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }

        case FOURCC('m', 'p', '4', 'a'):
        case FOURCC('e', 'n', 'c', 'a'):
        case FOURCC('s', 'a', 'm', 'r'):
        case FOURCC('s', 'a', 'w', 'b'):
        {
            if (mIsQT && chunk_type == FOURCC('m', 'p', '4', 'a')
                    && depth >= 1 && mPath[depth - 1] == FOURCC('w', 'a', 'v', 'e')) {
                // Ignore mp4a embedded in QT wave atom
                *offset += chunk_size;
                break;
            }

            uint8_t buffer[8 + 20];
            if (chunk_data_size < (ssize_t)sizeof(buffer)) {
                // Basic AudioSampleEntry size.
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer)) {
                return ERROR_IO;
            }

            uint16_t data_ref_index __unused = U16_AT(&buffer[6]);
            uint16_t version = U16_AT(&buffer[8]);
            uint32_t num_channels = U16_AT(&buffer[16]);

            uint16_t sample_size = U16_AT(&buffer[18]);
            uint32_t sample_rate = U32_AT(&buffer[24]) >> 16;

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset + sizeof(buffer);

            if (mIsQT && chunk_type == FOURCC('m', 'p', '4', 'a')) {
                if (version == 1) {
                    if (mDataSource->readAt(*offset, buffer, 16) < 16) {
                        return ERROR_IO;
                    }

#if 0
                    U32_AT(buffer);  // samples per packet
                    U32_AT(&buffer[4]);  // bytes per packet
                    U32_AT(&buffer[8]);  // bytes per frame
                    U32_AT(&buffer[12]);  // bytes per sample
#endif
                    *offset += 16;
                } else if (version == 2) {
                    uint8_t v2buffer[36];
                    if (mDataSource->readAt(*offset, v2buffer, 36) < 36) {
                        return ERROR_IO;
                    }

#if 0
                    U32_AT(v2buffer);  // size of struct only
                    sample_rate = (uint32_t)U64_AT(&v2buffer[4]);  // audio sample rate
                    num_channels = U32_AT(&v2buffer[12]);  // num audio channels
                    U32_AT(&v2buffer[16]);  // always 0x7f000000
                    sample_size = (uint16_t)U32_AT(&v2buffer[20]);  // const bits per channel
                    U32_AT(&v2buffer[24]);  // format specifc flags
                    U32_AT(&v2buffer[28]);  // const bytes per audio packet
                    U32_AT(&v2buffer[32]);  // const LPCM frames per audio packet
#endif
                    *offset += 36;
                }
            }

            if (chunk_type != FOURCC('e', 'n', 'c', 'a')) {
                // if the chunk type is enca, we'll get the type from the sinf/frma box later
                mLastTrack->meta->setCString(kKeyMIMEType, FourCC2MIME(chunk_type));
                AdjustChannelsAndRate(chunk_type, &num_channels, &sample_rate);
            }
            ALOGV("*** coding='%s' %d channels, size %d, rate %d\n",
                   chunk, num_channels, sample_size, sample_rate);
            mLastTrack->meta->setInt32(kKeyChannelCount, num_channels);
            mLastTrack->meta->setInt32(kKeySampleRate, sample_rate);

            if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG, FourCC2MIME(chunk_type)) ||
                !strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, FourCC2MIME(chunk_type))) {
                // ESD is not required in mp3
                // amr wb with damr atom corrupted can cause the clip to not play
               *offset = stop_offset;
            } else {
               *offset = data_offset + sizeof(buffer);
            }
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }

        case FOURCC('m', 'p', '4', 'v'):
        case FOURCC('e', 'n', 'c', 'v'):
        case FOURCC('s', '2', '6', '3'):
        case FOURCC('H', '2', '6', '3'):
        case FOURCC('h', '2', '6', '3'):
        case FOURCC('a', 'v', 'c', '1'):
        case FOURCC('h', 'v', 'c', '1'):
        case FOURCC('h', 'e', 'v', '1'):
        {
            mHasVideo = true;

            uint8_t buffer[78];
            if (chunk_data_size < (ssize_t)sizeof(buffer)) {
                // Basic VideoSampleEntry size.
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer)) {
                return ERROR_IO;
            }

            uint16_t data_ref_index __unused = U16_AT(&buffer[6]);
            uint16_t width = U16_AT(&buffer[6 + 18]);
            uint16_t height = U16_AT(&buffer[6 + 20]);

            // The video sample is not standard-compliant if it has invalid dimension.
            // Use some default width and height value, and
            // let the decoder figure out the actual width and height (and thus
            // be prepared for INFO_FOMRAT_CHANGED event).
            if (width == 0)  width  = 352;
            if (height == 0) height = 288;

            // printf("*** coding='%s' width=%d height=%d\n",
            //        chunk, width, height);

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            if (chunk_type != FOURCC('e', 'n', 'c', 'v')) {
                // if the chunk type is encv, we'll get the type from the sinf/frma box later
                mLastTrack->meta->setCString(kKeyMIMEType, FourCC2MIME(chunk_type));
            }
            mLastTrack->meta->setInt32(kKeyWidth, width);
            mLastTrack->meta->setInt32(kKeyHeight, height);

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset + sizeof(buffer);
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }

        case FOURCC('s', 't', 'c', 'o'):
        case FOURCC('c', 'o', '6', '4'):
        {
            if ((mLastTrack == NULL) || (mLastTrack->sampleTable == NULL))
                return ERROR_MALFORMED;

            status_t err =
                mLastTrack->sampleTable->setChunkOffsetParams(
                        chunk_type, data_offset, chunk_data_size);

            *offset += chunk_size;

            if (err != OK) {
                return err;
            }

            break;
        }

        case FOURCC('s', 't', 's', 'c'):
        {
            if ((mLastTrack == NULL) || (mLastTrack->sampleTable == NULL))
                return ERROR_MALFORMED;

            status_t err =
                mLastTrack->sampleTable->setSampleToChunkParams(
                        data_offset, chunk_data_size);

            *offset += chunk_size;

            if (err != OK) {
                return err;
            }

            break;
        }

        case FOURCC('s', 't', 's', 'z'):
        case FOURCC('s', 't', 'z', '2'):
        {
            if ((mLastTrack == NULL) || (mLastTrack->sampleTable == NULL))
                return ERROR_MALFORMED;

            status_t err =
                mLastTrack->sampleTable->setSampleSizeParams(
                        chunk_type, data_offset, chunk_data_size);

            *offset += chunk_size;

            if (err != OK) {
                return err;
            }

            size_t max_size;
            err = mLastTrack->sampleTable->getMaxSampleSize(&max_size);

            if (err != OK) {
                return err;
            }

            if (max_size != 0) {
                // Assume that a given buffer only contains at most 10 chunks,
                // each chunk originally prefixed with a 2 byte length will
                // have a 4 byte header (0x00 0x00 0x00 0x01) after conversion,
                // and thus will grow by 2 bytes per chunk.
                if (max_size > SIZE_MAX - 10 * 2) {
                    ALOGE("max sample size too big: %zu", max_size);
                    return ERROR_MALFORMED;
                }
                mLastTrack->meta->setInt32(kKeyMaxInputSize, max_size + 10 * 2);
            } else {
                // No size was specified. Pick a conservatively large size.
                uint32_t width, height;
                if (!mLastTrack->meta->findInt32(kKeyWidth, (int32_t*)&width) ||
                    !mLastTrack->meta->findInt32(kKeyHeight,(int32_t*) &height)) {
                    ALOGE("No width or height, assuming worst case 1080p");
                    width = 1920;
                    height = 1080;
                } else {
                    // A resolution was specified, check that it's not too big. The values below
                    // were chosen so that the calculations below don't cause overflows, they're
                    // not indicating that resolutions up to 32kx32k are actually supported.
                    if (width > 32768 || height > 32768) {
                        ALOGE("can't support %u x %u video", width, height);
                        return ERROR_MALFORMED;
                    }
                }

                const char *mime;
                CHECK(mLastTrack->meta->findCString(kKeyMIMEType, &mime));
                if (!strcmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)
                        || !strcmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC)) {
                    // AVC & HEVC requires compression ratio of at least 2, and uses
                    // macroblocks
                    max_size = ((width + 15) / 16) * ((height + 15) / 16) * 192;
                } else {
                    // For all other formats there is no minimum compression
                    // ratio. Use compression ratio of 1.
                    max_size = width * height * 3 / 2;
                }
                mLastTrack->meta->setInt32(kKeyMaxInputSize, max_size);
            }

            // NOTE: setting another piece of metadata invalidates any pointers (such as the
            // mimetype) previously obtained, so don't cache them.
            const char *mime;
            CHECK(mLastTrack->meta->findCString(kKeyMIMEType, &mime));
            // Calculate average frame rate.
            if (!strncasecmp("video/", mime, 6)) {
                size_t nSamples = mLastTrack->sampleTable->countSamples();
                if (nSamples == 0) {
                    int32_t trackId;
                    if (mLastTrack->meta->findInt32(kKeyTrackID, &trackId)) {
                        for (size_t i = 0; i < mTrex.size(); i++) {
                            Trex *t = &mTrex.editItemAt(i);
                            if (t->track_ID == (uint32_t) trackId) {
                                if (t->default_sample_duration > 0) {
                                    int32_t frameRate =
                                            mLastTrack->timescale / t->default_sample_duration;
                                    mLastTrack->meta->setInt32(kKeyFrameRate, frameRate);
                                }
                                break;
                            }
                        }
                    }
                } else {
                    int64_t durationUs;
                    if (mLastTrack->meta->findInt64(kKeyDuration, &durationUs)) {
                        if (durationUs > 0) {
                            int32_t frameRate = (nSamples * 1000000LL +
                                        (durationUs >> 1)) / durationUs;
                            mLastTrack->meta->setInt32(kKeyFrameRate, frameRate);
                        }
                    }
                }
            }

            break;
        }

        case FOURCC('s', 't', 't', 's'):
        {
            if ((mLastTrack == NULL) || (mLastTrack->sampleTable == NULL))
                return ERROR_MALFORMED;

            *offset += chunk_size;

            status_t err =
                mLastTrack->sampleTable->setTimeToSampleParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            break;
        }

        case FOURCC('c', 't', 't', 's'):
        {
            if ((mLastTrack == NULL) || (mLastTrack->sampleTable == NULL))
                return ERROR_MALFORMED;

            *offset += chunk_size;

            status_t err =
                mLastTrack->sampleTable->setCompositionTimeToSampleParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            break;
        }

        case FOURCC('s', 't', 's', 's'):
        {
            if ((mLastTrack == NULL) || (mLastTrack->sampleTable == NULL))
                return ERROR_MALFORMED;

            *offset += chunk_size;

            status_t err =
                mLastTrack->sampleTable->setSyncSampleParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            break;
        }

        // \xA9xyz
        case FOURCC(0xA9, 'x', 'y', 'z'):
        {
            *offset += chunk_size;

            // Best case the total data length inside "\xA9xyz" box
            // would be 8, for instance "\xA9xyz" + "\x00\x04\x15\xc7" + "0+0/",
            // where "\x00\x04" is the text string length with value = 4,
            // "\0x15\xc7" is the language code = en, and "0+0" is a
            // location (string) value with longitude = 0 and latitude = 0.
            if (chunk_data_size < 8) {
                return ERROR_MALFORMED;
            }

            // Worst case the location string length would be 18,
            // for instance +90.0000-180.0000, without the trailing "/" and
            // the string length + language code, and some devices include
            // an additional 8 bytes of altitude, e.g. +007.186
            char buffer[18 + 8];

            // Substracting 5 from the data size is because the text string length +
            // language code takes 4 bytes, and the trailing slash "/" takes 1 byte.
            off64_t location_length = chunk_data_size - 5;
            if (location_length >= (off64_t) sizeof(buffer)) {
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset + 4, buffer, location_length) < location_length) {
                return ERROR_IO;
            }

            buffer[location_length] = '\0';
            mFileMetaData->setCString(kKeyLocation, buffer);
            break;
        }

        case FOURCC('e', 's', 'd', 's'):
        {
            *offset += chunk_size;

            if (chunk_data_size < 4) {
                return ERROR_MALFORMED;
            }

            uint8_t buffer[256];
            if (chunk_data_size > (off64_t)sizeof(buffer)) {
                return ERROR_BUFFER_TOO_SMALL;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            if (U32_AT(buffer) != 0) {
                // Should be version 0, flags 0.
                return ERROR_MALFORMED;
            }

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            mLastTrack->meta->setData(
                    kKeyESDS, kTypeESDS, &buffer[4], chunk_data_size - 4);

            if (mPath.size() >= 2
                    && mPath[mPath.size() - 2] == FOURCC('m', 'p', '4', 'a')) {
                // Information from the ESDS must be relied on for proper
                // setup of sample rate and channel count for MPEG4 Audio.
                // The generic header appears to only contain generic
                // information...

                status_t err = updateAudioTrackInfoFromESDS_MPEG4Audio(
                        &buffer[4], chunk_data_size - 4);

                if (err != OK) {
                    return err;
                }
            }
            if (mPath.size() >= 2
                    && mPath[mPath.size() - 2] == FOURCC('m', 'p', '4', 'v')) {
                // Check if the video is MPEG2
                ESDS esds(&buffer[4], chunk_data_size - 4);

                uint8_t objectTypeIndication;
                if (esds.getObjectTypeIndication(&objectTypeIndication) == OK) {
                    if (objectTypeIndication >= 0x60 && objectTypeIndication <= 0x65) {
                        mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);
                    }
                }
            }
            break;
        }

        case FOURCC('b', 't', 'r', 't'):
        {
            *offset += chunk_size;

            uint8_t buffer[12];
            if (chunk_data_size != sizeof(buffer)) {
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                    data_offset, buffer, chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            uint32_t maxBitrate = U32_AT(&buffer[4]);
            uint32_t avgBitrate = U32_AT(&buffer[8]);
            if (maxBitrate > 0 && maxBitrate < INT32_MAX) {
                mLastTrack->meta->setInt32(kKeyMaxBitRate, (int32_t)maxBitrate);
            }
            if (avgBitrate > 0 && avgBitrate < INT32_MAX) {
                mLastTrack->meta->setInt32(kKeyBitRate, (int32_t)avgBitrate);
            }
            break;
        }

        case FOURCC('a', 'v', 'c', 'C'):
        {
            *offset += chunk_size;

            sp<ABuffer> buffer = new ABuffer(chunk_data_size);

            if (buffer->data() == NULL) {
                ALOGE("b/28471206");
                return NO_MEMORY;
            }

            if (mDataSource->readAt(
                        data_offset, buffer->data(), chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            mLastTrack->meta->setData(
                    kKeyAVCC, kTypeAVCC, buffer->data(), chunk_data_size);

            break;
        }
        case FOURCC('h', 'v', 'c', 'C'):
        {
            sp<ABuffer> buffer = new ABuffer(chunk_data_size);

            if (buffer->data() == NULL) {
                ALOGE("b/28471206");
                return NO_MEMORY;
            }

            if (mDataSource->readAt(
                        data_offset, buffer->data(), chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            mLastTrack->meta->setData(
                    kKeyHVCC, kTypeHVCC, buffer->data(), chunk_data_size);

            *offset += chunk_size;
            break;
        }

        case FOURCC('d', '2', '6', '3'):
        {
            *offset += chunk_size;
            /*
             * d263 contains a fixed 7 bytes part:
             *   vendor - 4 bytes
             *   version - 1 byte
             *   level - 1 byte
             *   profile - 1 byte
             * optionally, "d263" box itself may contain a 16-byte
             * bit rate box (bitr)
             *   average bit rate - 4 bytes
             *   max bit rate - 4 bytes
             */
            char buffer[23];
            if (chunk_data_size != 7 &&
                chunk_data_size != 23) {
                ALOGE("Incorrect D263 box size %lld", (long long)chunk_data_size);
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                    data_offset, buffer, chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            mLastTrack->meta->setData(kKeyD263, kTypeD263, buffer, chunk_data_size);

            break;
        }

        case FOURCC('m', 'e', 't', 'a'):
        {
            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset;
            bool isParsingMetaKeys = underQTMetaPath(mPath, 2);
            if (!isParsingMetaKeys) {
                uint8_t buffer[4];
                if (chunk_data_size < (off64_t)sizeof(buffer)) {
                    *offset = stop_offset;
                    return ERROR_MALFORMED;
                }

                if (mDataSource->readAt(
                            data_offset, buffer, 4) < 4) {
                    *offset = stop_offset;
                    return ERROR_IO;
                }

                if (U32_AT(buffer) != 0) {
                    // Should be version 0, flags 0.

                    // If it's not, let's assume this is one of those
                    // apparently malformed chunks that don't have flags
                    // and completely different semantics than what's
                    // in the MPEG4 specs and skip it.
                    *offset = stop_offset;
                    return OK;
                }
                *offset +=  sizeof(buffer);
            }

            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }

        case FOURCC('m', 'e', 'a', 'n'):
        case FOURCC('n', 'a', 'm', 'e'):
        case FOURCC('d', 'a', 't', 'a'):
        {
            *offset += chunk_size;

            if (mPath.size() == 6 && underMetaDataPath(mPath)) {
                status_t err = parseITunesMetaData(data_offset, chunk_data_size);

                if (err != OK) {
                    return err;
                }
            }

            break;
        }

        case FOURCC('m', 'v', 'h', 'd'):
        {
            *offset += chunk_size;

            if (chunk_data_size < 32) {
                return ERROR_MALFORMED;
            }

            uint8_t header[32];
            if (mDataSource->readAt(
                        data_offset, header, sizeof(header))
                    < (ssize_t)sizeof(header)) {
                return ERROR_IO;
            }

            uint64_t creationTime;
            uint64_t duration = 0;
            if (header[0] == 1) {
                creationTime = U64_AT(&header[4]);
                mHeaderTimescale = U32_AT(&header[20]);
                duration = U64_AT(&header[24]);
                if (duration == 0xffffffffffffffff) {
                    duration = 0;
                }
            } else if (header[0] != 0) {
                return ERROR_MALFORMED;
            } else {
                creationTime = U32_AT(&header[4]);
                mHeaderTimescale = U32_AT(&header[12]);
                uint32_t d32 = U32_AT(&header[16]);
                if (d32 == 0xffffffff) {
                    d32 = 0;
                }
                duration = d32;
            }
            if (duration != 0 && mHeaderTimescale != 0 && duration < UINT64_MAX / 1000000) {
                mFileMetaData->setInt64(kKeyDuration, duration * 1000000 / mHeaderTimescale);
            }

            String8 s;
            if (convertTimeToDate(creationTime, &s)) {
                mFileMetaData->setCString(kKeyDate, s.string());
            }


            break;
        }

        case FOURCC('m', 'e', 'h', 'd'):
        {
            *offset += chunk_size;

            if (chunk_data_size < 8) {
                return ERROR_MALFORMED;
            }

            uint8_t flags[4];
            if (mDataSource->readAt(
                        data_offset, flags, sizeof(flags))
                    < (ssize_t)sizeof(flags)) {
                return ERROR_IO;
            }

            uint64_t duration = 0;
            if (flags[0] == 1) {
                // 64 bit
                if (chunk_data_size < 12) {
                    return ERROR_MALFORMED;
                }
                mDataSource->getUInt64(data_offset + 4, &duration);
                if (duration == 0xffffffffffffffff) {
                    duration = 0;
                }
            } else if (flags[0] == 0) {
                // 32 bit
                uint32_t d32;
                mDataSource->getUInt32(data_offset + 4, &d32);
                if (d32 == 0xffffffff) {
                    d32 = 0;
                }
                duration = d32;
            } else {
                return ERROR_MALFORMED;
            }

            if (duration != 0 && mHeaderTimescale != 0) {
                mFileMetaData->setInt64(kKeyDuration, duration * 1000000 / mHeaderTimescale);
            }

            break;
        }

        case FOURCC('m', 'd', 'a', 't'):
        {
            ALOGV("mdat chunk, drm: %d", mIsDrm);

            mMdatFound = true;

            if (!mIsDrm) {
                *offset += chunk_size;
                break;
            }

            if (chunk_size < 8) {
                return ERROR_MALFORMED;
            }

            return parseDrmSINF(offset, data_offset);
        }

        case FOURCC('h', 'd', 'l', 'r'):
        {
            *offset += chunk_size;

            if (underQTMetaPath(mPath, 3)) {
                break;
            }

            uint32_t buffer;
            if (mDataSource->readAt(
                        data_offset + 8, &buffer, 4) < 4) {
                return ERROR_IO;
            }

            uint32_t type = ntohl(buffer);
            // For the 3GPP file format, the handler-type within the 'hdlr' box
            // shall be 'text'. We also want to support 'sbtl' handler type
            // for a practical reason as various MPEG4 containers use it.
            if (type == FOURCC('t', 'e', 'x', 't') || type == FOURCC('s', 'b', 't', 'l')) {
                if (mLastTrack != NULL) {
                    mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_TEXT_3GPP);
                }
            }

            break;
        }

        case FOURCC('k', 'e', 'y', 's'):
        {
            *offset += chunk_size;

            if (underQTMetaPath(mPath, 3)) {
                parseQTMetaKey(data_offset, chunk_data_size);
            }
            break;
        }

        case FOURCC('t', 'r', 'e', 'x'):
        {
            *offset += chunk_size;

            if (chunk_data_size < 24) {
                return ERROR_IO;
            }
            Trex trex;
            if (!mDataSource->getUInt32(data_offset + 4, &trex.track_ID) ||
                !mDataSource->getUInt32(data_offset + 8, &trex.default_sample_description_index) ||
                !mDataSource->getUInt32(data_offset + 12, &trex.default_sample_duration) ||
                !mDataSource->getUInt32(data_offset + 16, &trex.default_sample_size) ||
                !mDataSource->getUInt32(data_offset + 20, &trex.default_sample_flags)) {
                return ERROR_IO;
            }
            mTrex.add(trex);
            break;
        }

        case FOURCC('t', 'x', '3', 'g'):
        {
            if (mLastTrack == NULL)
                return ERROR_MALFORMED;

            uint32_t type;
            const void *data;
            size_t size = 0;
            if (!mLastTrack->meta->findData(
                    kKeyTextFormatData, &type, &data, &size)) {
                size = 0;
            }

            if ((chunk_size > SIZE_MAX) || (SIZE_MAX - chunk_size <= size)) {
                return ERROR_MALFORMED;
            }

            uint8_t *buffer = new (std::nothrow) uint8_t[size + chunk_size];
            if (buffer == NULL) {
                return ERROR_MALFORMED;
            }

            if (size > 0) {
                memcpy(buffer, data, size);
            }

            if ((size_t)(mDataSource->readAt(*offset, buffer + size, chunk_size))
                    < chunk_size) {
                delete[] buffer;
                buffer = NULL;

                // advance read pointer so we don't end up reading this again
                *offset += chunk_size;
                return ERROR_IO;
            }

            mLastTrack->meta->setData(
                    kKeyTextFormatData, 0, buffer, size + chunk_size);

            delete[] buffer;

            *offset += chunk_size;
            break;
        }

        case FOURCC('c', 'o', 'v', 'r'):
        {
            *offset += chunk_size;

            if (mFileMetaData != NULL) {
                ALOGV("chunk_data_size = %" PRId64 " and data_offset = %" PRId64,
                      chunk_data_size, data_offset);

                if (chunk_data_size < 0 || static_cast<uint64_t>(chunk_data_size) >= SIZE_MAX - 1) {
                    return ERROR_MALFORMED;
                }
                sp<ABuffer> buffer = new ABuffer(chunk_data_size + 1);
                if (buffer->data() == NULL) {
                    ALOGE("b/28471206");
                    return NO_MEMORY;
                }
                if (mDataSource->readAt(
                    data_offset, buffer->data(), chunk_data_size) != (ssize_t)chunk_data_size) {
                    return ERROR_IO;
                }
                const int kSkipBytesOfDataBox = 16;
                if (chunk_data_size <= kSkipBytesOfDataBox) {
                    return ERROR_MALFORMED;
                }

                mFileMetaData->setData(
                    kKeyAlbumArt, MetaData::TYPE_NONE,
                    buffer->data() + kSkipBytesOfDataBox, chunk_data_size - kSkipBytesOfDataBox);
            }

            break;
        }

        case FOURCC('c', 'o', 'l', 'r'):
        {
            *offset += chunk_size;
            // this must be in a VisualSampleEntry box under the Sample Description Box ('stsd')
            // ignore otherwise
            if (depth >= 2 && mPath[depth - 2] == FOURCC('s', 't', 's', 'd')) {
                status_t err = parseColorInfo(data_offset, chunk_data_size);
                if (err != OK) {
                    return err;
                }
            }

            break;
        }

        case FOURCC('t', 'i', 't', 'l'):
        case FOURCC('p', 'e', 'r', 'f'):
        case FOURCC('a', 'u', 't', 'h'):
        case FOURCC('g', 'n', 'r', 'e'):
        case FOURCC('a', 'l', 'b', 'm'):
        case FOURCC('y', 'r', 'r', 'c'):
        {
            *offset += chunk_size;

            status_t err = parse3GPPMetaData(data_offset, chunk_data_size, depth);

            if (err != OK) {
                return err;
            }

            break;
        }

        case FOURCC('I', 'D', '3', '2'):
        {
            *offset += chunk_size;

            if (chunk_data_size < 6) {
                return ERROR_MALFORMED;
            }

            parseID3v2MetaData(data_offset + 6);

            break;
        }

        case FOURCC('-', '-', '-', '-'):
        {
            mLastCommentMean.clear();
            mLastCommentName.clear();
            mLastCommentData.clear();
            *offset += chunk_size;
            break;
        }

        case FOURCC('s', 'i', 'd', 'x'):
        {
            parseSegmentIndex(data_offset, chunk_data_size);
            *offset += chunk_size;
            return UNKNOWN_ERROR; // stop parsing after sidx
        }

        case FOURCC('f', 't', 'y', 'p'):
        {
            if (chunk_data_size < 8 || depth != 0) {
                return ERROR_MALFORMED;
            }

            off64_t stop_offset = *offset + chunk_size;
            uint32_t numCompatibleBrands = (chunk_data_size - 8) / 4;
            for (size_t i = 0; i < numCompatibleBrands + 2; ++i) {
                if (i == 1) {
                    // Skip this index, it refers to the minorVersion,
                    // not a brand.
                    continue;
                }

                uint32_t brand;
                if (mDataSource->readAt(data_offset + 4 * i, &brand, 4) < 4) {
                    return ERROR_MALFORMED;
                }

                brand = ntohl(brand);
                if (brand == FOURCC('q', 't', ' ', ' ')) {
                    mIsQT = true;
                    break;
                }
            }

            *offset = stop_offset;

            break;
        }

        default:
        {
            // check if we're parsing 'ilst' for meta keys
            // if so, treat type as a number (key-id).
            if (underQTMetaPath(mPath, 3)) {
                parseQTMetaVal(chunk_type, data_offset, chunk_data_size);
            }

            *offset += chunk_size;
            break;
        }
    }

    return OK;
}

status_t MPEG4Extractor::parseSegmentIndex(off64_t offset, size_t size) {
  ALOGV("MPEG4Extractor::parseSegmentIndex");

    if (size < 12) {
      return -EINVAL;
    }

    uint32_t flags;
    if (!mDataSource->getUInt32(offset, &flags)) {
        return ERROR_MALFORMED;
    }

    uint32_t version = flags >> 24;
    flags &= 0xffffff;

    ALOGV("sidx version %d", version);

    uint32_t referenceId;
    if (!mDataSource->getUInt32(offset + 4, &referenceId)) {
        return ERROR_MALFORMED;
    }

    uint32_t timeScale;
    if (!mDataSource->getUInt32(offset + 8, &timeScale)) {
        return ERROR_MALFORMED;
    }
    ALOGV("sidx refid/timescale: %d/%d", referenceId, timeScale);
    if (timeScale == 0)
        return ERROR_MALFORMED;

    uint64_t earliestPresentationTime;
    uint64_t firstOffset;

    offset += 12;
    size -= 12;

    if (version == 0) {
        if (size < 8) {
            return -EINVAL;
        }
        uint32_t tmp;
        if (!mDataSource->getUInt32(offset, &tmp)) {
            return ERROR_MALFORMED;
        }
        earliestPresentationTime = tmp;
        if (!mDataSource->getUInt32(offset + 4, &tmp)) {
            return ERROR_MALFORMED;
        }
        firstOffset = tmp;
        offset += 8;
        size -= 8;
    } else {
        if (size < 16) {
            return -EINVAL;
        }
        if (!mDataSource->getUInt64(offset, &earliestPresentationTime)) {
            return ERROR_MALFORMED;
        }
        if (!mDataSource->getUInt64(offset + 8, &firstOffset)) {
            return ERROR_MALFORMED;
        }
        offset += 16;
        size -= 16;
    }
    ALOGV("sidx pres/off: %" PRIu64 "/%" PRIu64, earliestPresentationTime, firstOffset);

    if (size < 4) {
        return -EINVAL;
    }

    uint16_t referenceCount;
    if (!mDataSource->getUInt16(offset + 2, &referenceCount)) {
        return ERROR_MALFORMED;
    }
    offset += 4;
    size -= 4;
    ALOGV("refcount: %d", referenceCount);

    if (size < referenceCount * 12) {
        return -EINVAL;
    }

    uint64_t total_duration = 0;
    for (unsigned int i = 0; i < referenceCount; i++) {
        uint32_t d1, d2, d3;

        if (!mDataSource->getUInt32(offset, &d1) ||     // size
            !mDataSource->getUInt32(offset + 4, &d2) || // duration
            !mDataSource->getUInt32(offset + 8, &d3)) { // flags
            return ERROR_MALFORMED;
        }

        if (d1 & 0x80000000) {
            ALOGW("sub-sidx boxes not supported yet");
        }
        bool sap = d3 & 0x80000000;
        uint32_t saptype = (d3 >> 28) & 7;
        if (!sap || (saptype != 1 && saptype != 2)) {
            // type 1 and 2 are sync samples
            ALOGW("not a stream access point, or unsupported type: %08x", d3);
        }
        total_duration += d2;
        offset += 12;
        ALOGV(" item %d, %08x %08x %08x", i, d1, d2, d3);
        SidxEntry se;
        se.mSize = d1 & 0x7fffffff;
        se.mDurationUs = 1000000LL * d2 / timeScale;
        mSidxEntries.add(se);
    }

    uint64_t sidxDuration = total_duration * 1000000 / timeScale;

    if (mLastTrack == NULL)
        return ERROR_MALFORMED;

    int64_t metaDuration;
    if (!mLastTrack->meta->findInt64(kKeyDuration, &metaDuration) || metaDuration == 0) {
        mLastTrack->meta->setInt64(kKeyDuration, sidxDuration);
    }
    return OK;
}

status_t MPEG4Extractor::parseQTMetaKey(off64_t offset, size_t size) {
    if (size < 8) {
        return ERROR_MALFORMED;
    }

    uint32_t count;
    if (!mDataSource->getUInt32(offset + 4, &count)) {
        return ERROR_MALFORMED;
    }

    if (mMetaKeyMap.size() > 0) {
        ALOGW("'keys' atom seen again, discarding existing entries");
        mMetaKeyMap.clear();
    }

    off64_t keyOffset = offset + 8;
    off64_t stopOffset = offset + size;
    for (size_t i = 1; i <= count; i++) {
        if (keyOffset + 8 > stopOffset) {
            return ERROR_MALFORMED;
        }

        uint32_t keySize;
        if (!mDataSource->getUInt32(keyOffset, &keySize)
                || keySize < 8
                || keyOffset + keySize > stopOffset) {
            return ERROR_MALFORMED;
        }

        uint32_t type;
        if (!mDataSource->getUInt32(keyOffset + 4, &type)
                || type != FOURCC('m', 'd', 't', 'a')) {
            return ERROR_MALFORMED;
        }

        keySize -= 8;
        keyOffset += 8;

        sp<ABuffer> keyData = new ABuffer(keySize);
        if (keyData->data() == NULL) {
            return ERROR_MALFORMED;
        }
        if (mDataSource->readAt(
                keyOffset, keyData->data(), keySize) < (ssize_t) keySize) {
            return ERROR_MALFORMED;
        }

        AString key((const char *)keyData->data(), keySize);
        mMetaKeyMap.add(i, key);

        keyOffset += keySize;
    }
    return OK;
}

status_t MPEG4Extractor::parseQTMetaVal(
        int32_t keyId, off64_t offset, size_t size) {
    ssize_t index = mMetaKeyMap.indexOfKey(keyId);
    if (index < 0) {
        // corresponding key is not present, ignore
        return ERROR_MALFORMED;
    }

    if (size <= 16) {
        return ERROR_MALFORMED;
    }
    uint32_t dataSize;
    if (!mDataSource->getUInt32(offset, &dataSize)
            || dataSize > size || dataSize <= 16) {
        return ERROR_MALFORMED;
    }
    uint32_t atomFourCC;
    if (!mDataSource->getUInt32(offset + 4, &atomFourCC)
            || atomFourCC != FOURCC('d', 'a', 't', 'a')) {
        return ERROR_MALFORMED;
    }
    uint32_t dataType;
    if (!mDataSource->getUInt32(offset + 8, &dataType)
            || ((dataType & 0xff000000) != 0)) {
        // not well-known type
        return ERROR_MALFORMED;
    }

    dataSize -= 16;
    offset += 16;

    if (dataType == 23 && dataSize >= 4) {
        // BE Float32
        uint32_t val;
        if (!mDataSource->getUInt32(offset, &val)) {
            return ERROR_MALFORMED;
        }
        if (!strcasecmp(mMetaKeyMap[index].c_str(), "com.android.capture.fps")) {
            mFileMetaData->setFloat(kKeyCaptureFramerate, *(float *)&val);
        }
    } else if (dataType == 67 && dataSize >= 4) {
        // BE signed int32
        uint32_t val;
        if (!mDataSource->getUInt32(offset, &val)) {
            return ERROR_MALFORMED;
        }
        if (!strcasecmp(mMetaKeyMap[index].c_str(), "com.android.video.temporal_layers_count")) {
            mFileMetaData->setInt32(kKeyTemporalLayerCount, val);
        }
    } else {
        // add more keys if needed
        ALOGV("ignoring key: type %d, size %d", dataType, dataSize);
    }

    return OK;
}

status_t MPEG4Extractor::parseTrackHeader(
        off64_t data_offset, off64_t data_size) {
    if (data_size < 4) {
        return ERROR_MALFORMED;
    }

    uint8_t version;
    if (mDataSource->readAt(data_offset, &version, 1) < 1) {
        return ERROR_IO;
    }

    size_t dynSize = (version == 1) ? 36 : 24;

    uint8_t buffer[36 + 60];

    if (data_size != (off64_t)dynSize + 60) {
        return ERROR_MALFORMED;
    }

    if (mDataSource->readAt(
                data_offset, buffer, data_size) < (ssize_t)data_size) {
        return ERROR_IO;
    }

    uint64_t ctime __unused, mtime __unused, duration __unused;
    int32_t id;

    if (version == 1) {
        ctime = U64_AT(&buffer[4]);
        mtime = U64_AT(&buffer[12]);
        id = U32_AT(&buffer[20]);
        duration = U64_AT(&buffer[28]);
    } else if (version == 0) {
        ctime = U32_AT(&buffer[4]);
        mtime = U32_AT(&buffer[8]);
        id = U32_AT(&buffer[12]);
        duration = U32_AT(&buffer[20]);
    } else {
        return ERROR_UNSUPPORTED;
    }

    if (mLastTrack == NULL)
        return ERROR_MALFORMED;

    mLastTrack->meta->setInt32(kKeyTrackID, id);

    size_t matrixOffset = dynSize + 16;
    int32_t a00 = U32_AT(&buffer[matrixOffset]);
    int32_t a01 = U32_AT(&buffer[matrixOffset + 4]);
    int32_t a10 = U32_AT(&buffer[matrixOffset + 12]);
    int32_t a11 = U32_AT(&buffer[matrixOffset + 16]);

#if 0
    int32_t dx = U32_AT(&buffer[matrixOffset + 8]);
    int32_t dy = U32_AT(&buffer[matrixOffset + 20]);

    ALOGI("x' = %.2f * x + %.2f * y + %.2f",
         a00 / 65536.0f, a01 / 65536.0f, dx / 65536.0f);
    ALOGI("y' = %.2f * x + %.2f * y + %.2f",
         a10 / 65536.0f, a11 / 65536.0f, dy / 65536.0f);
#endif

    uint32_t rotationDegrees;

    static const int32_t kFixedOne = 0x10000;
    if (a00 == kFixedOne && a01 == 0 && a10 == 0 && a11 == kFixedOne) {
        // Identity, no rotation
        rotationDegrees = 0;
    } else if (a00 == 0 && a01 == kFixedOne && a10 == -kFixedOne && a11 == 0) {
        rotationDegrees = 90;
    } else if (a00 == 0 && a01 == -kFixedOne && a10 == kFixedOne && a11 == 0) {
        rotationDegrees = 270;
    } else if (a00 == -kFixedOne && a01 == 0 && a10 == 0 && a11 == -kFixedOne) {
        rotationDegrees = 180;
    } else {
        ALOGW("We only support 0,90,180,270 degree rotation matrices");
        rotationDegrees = 0;
    }

    if (rotationDegrees != 0) {
        mLastTrack->meta->setInt32(kKeyRotation, rotationDegrees);
    }

    // Handle presentation display size, which could be different
    // from the image size indicated by kKeyWidth and kKeyHeight.
    uint32_t width = U32_AT(&buffer[dynSize + 52]);
    uint32_t height = U32_AT(&buffer[dynSize + 56]);
    mLastTrack->meta->setInt32(kKeyDisplayWidth, width >> 16);
    mLastTrack->meta->setInt32(kKeyDisplayHeight, height >> 16);

    return OK;
}

status_t MPEG4Extractor::parseITunesMetaData(off64_t offset, size_t size) {
    if (size < 4 || size == SIZE_MAX) {
        return ERROR_MALFORMED;
    }

    uint8_t *buffer = new (std::nothrow) uint8_t[size + 1];
    if (buffer == NULL) {
        return ERROR_MALFORMED;
    }
    if (mDataSource->readAt(
                offset, buffer, size) != (ssize_t)size) {
        delete[] buffer;
        buffer = NULL;

        return ERROR_IO;
    }

    uint32_t flags = U32_AT(buffer);

    uint32_t metadataKey = 0;
    char chunk[5];
    MakeFourCCString(mPath[4], chunk);
    ALOGV("meta: %s @ %lld", chunk, (long long)offset);
    switch ((int32_t)mPath[4]) {
        case FOURCC(0xa9, 'a', 'l', 'b'):
        {
            metadataKey = kKeyAlbum;
            break;
        }
        case FOURCC(0xa9, 'A', 'R', 'T'):
        {
            metadataKey = kKeyArtist;
            break;
        }
        case FOURCC('a', 'A', 'R', 'T'):
        {
            metadataKey = kKeyAlbumArtist;
            break;
        }
        case FOURCC(0xa9, 'd', 'a', 'y'):
        {
            metadataKey = kKeyYear;
            break;
        }
        case FOURCC(0xa9, 'n', 'a', 'm'):
        {
            metadataKey = kKeyTitle;
            break;
        }
        case FOURCC(0xa9, 'w', 'r', 't'):
        {
            metadataKey = kKeyWriter;
            break;
        }
        case FOURCC('c', 'o', 'v', 'r'):
        {
            metadataKey = kKeyAlbumArt;
            break;
        }
        case FOURCC('g', 'n', 'r', 'e'):
        {
            metadataKey = kKeyGenre;
            break;
        }
        case FOURCC(0xa9, 'g', 'e', 'n'):
        {
            metadataKey = kKeyGenre;
            break;
        }
        case FOURCC('c', 'p', 'i', 'l'):
        {
            if (size == 9 && flags == 21) {
                char tmp[16];
                sprintf(tmp, "%d",
                        (int)buffer[size - 1]);

                mFileMetaData->setCString(kKeyCompilation, tmp);
            }
            break;
        }
        case FOURCC('t', 'r', 'k', 'n'):
        {
            if (size == 16 && flags == 0) {
                char tmp[16];
                uint16_t* pTrack = (uint16_t*)&buffer[10];
                uint16_t* pTotalTracks = (uint16_t*)&buffer[12];
                sprintf(tmp, "%d/%d", ntohs(*pTrack), ntohs(*pTotalTracks));

                mFileMetaData->setCString(kKeyCDTrackNumber, tmp);
            }
            break;
        }
        case FOURCC('d', 'i', 's', 'k'):
        {
            if ((size == 14 || size == 16) && flags == 0) {
                char tmp[16];
                uint16_t* pDisc = (uint16_t*)&buffer[10];
                uint16_t* pTotalDiscs = (uint16_t*)&buffer[12];
                sprintf(tmp, "%d/%d", ntohs(*pDisc), ntohs(*pTotalDiscs));

                mFileMetaData->setCString(kKeyDiscNumber, tmp);
            }
            break;
        }
        case FOURCC('-', '-', '-', '-'):
        {
            buffer[size] = '\0';
            switch (mPath[5]) {
                case FOURCC('m', 'e', 'a', 'n'):
                    mLastCommentMean.setTo((const char *)buffer + 4);
                    break;
                case FOURCC('n', 'a', 'm', 'e'):
                    mLastCommentName.setTo((const char *)buffer + 4);
                    break;
                case FOURCC('d', 'a', 't', 'a'):
                    if (size < 8) {
                        delete[] buffer;
                        buffer = NULL;
                        ALOGE("b/24346430");
                        return ERROR_MALFORMED;
                    }
                    mLastCommentData.setTo((const char *)buffer + 8);
                    break;
            }

            // Once we have a set of mean/name/data info, go ahead and process
            // it to see if its something we are interested in.  Whether or not
            // were are interested in the specific tag, make sure to clear out
            // the set so we can be ready to process another tuple should one
            // show up later in the file.
            if ((mLastCommentMean.length() != 0) &&
                (mLastCommentName.length() != 0) &&
                (mLastCommentData.length() != 0)) {

                if (mLastCommentMean == "com.apple.iTunes"
                        && mLastCommentName == "iTunSMPB") {
                    int32_t delay, padding;
                    if (sscanf(mLastCommentData,
                               " %*x %x %x %*x", &delay, &padding) == 2) {
                        if (mLastTrack == NULL)
                            return ERROR_MALFORMED;

                        mLastTrack->meta->setInt32(kKeyEncoderDelay, delay);
                        mLastTrack->meta->setInt32(kKeyEncoderPadding, padding);
                    }
                }

                mLastCommentMean.clear();
                mLastCommentName.clear();
                mLastCommentData.clear();
            }
            break;
        }

        default:
            break;
    }

    if (size >= 8 && metadataKey && !mFileMetaData->hasData(metadataKey)) {
        if (metadataKey == kKeyAlbumArt) {
            mFileMetaData->setData(
                    kKeyAlbumArt, MetaData::TYPE_NONE,
                    buffer + 8, size - 8);
        } else if (metadataKey == kKeyGenre) {
            if (flags == 0) {
                // uint8_t genre code, iTunes genre codes are
                // the standard id3 codes, except they start
                // at 1 instead of 0 (e.g. Pop is 14, not 13)
                // We use standard id3 numbering, so subtract 1.
                int genrecode = (int)buffer[size - 1];
                genrecode--;
                if (genrecode < 0) {
                    genrecode = 255; // reserved for 'unknown genre'
                }
                char genre[10];
                sprintf(genre, "%d", genrecode);

                mFileMetaData->setCString(metadataKey, genre);
            } else if (flags == 1) {
                // custom genre string
                buffer[size] = '\0';

                mFileMetaData->setCString(
                        metadataKey, (const char *)buffer + 8);
            }
        } else {
            buffer[size] = '\0';

            mFileMetaData->setCString(
                    metadataKey, (const char *)buffer + 8);
        }
    }

    delete[] buffer;
    buffer = NULL;

    return OK;
}

status_t MPEG4Extractor::parseColorInfo(off64_t offset, size_t size) {
    if (size < 4 || size == SIZE_MAX || mLastTrack == NULL) {
        return ERROR_MALFORMED;
    }

    uint8_t *buffer = new (std::nothrow) uint8_t[size + 1];
    if (buffer == NULL) {
        return ERROR_MALFORMED;
    }
    if (mDataSource->readAt(offset, buffer, size) != (ssize_t)size) {
        delete[] buffer;
        buffer = NULL;

        return ERROR_IO;
    }

    int32_t type = U32_AT(&buffer[0]);
    if ((type == FOURCC('n', 'c', 'l', 'x') && size >= 11)
            || (type == FOURCC('n', 'c', 'l', 'c' && size >= 10))) {
        int32_t primaries = U16_AT(&buffer[4]);
        int32_t transfer = U16_AT(&buffer[6]);
        int32_t coeffs = U16_AT(&buffer[8]);
        bool fullRange = (type == FOURCC('n', 'c', 'l', 'x')) && (buffer[10] & 128);

        ColorAspects aspects;
        ColorUtils::convertIsoColorAspectsToCodecAspects(
                primaries, transfer, coeffs, fullRange, aspects);

        // only store the first color specification
        if (!mLastTrack->meta->hasData(kKeyColorPrimaries)) {
            mLastTrack->meta->setInt32(kKeyColorPrimaries, aspects.mPrimaries);
            mLastTrack->meta->setInt32(kKeyTransferFunction, aspects.mTransfer);
            mLastTrack->meta->setInt32(kKeyColorMatrix, aspects.mMatrixCoeffs);
            mLastTrack->meta->setInt32(kKeyColorRange, aspects.mRange);
        }
    }

    delete[] buffer;
    buffer = NULL;

    return OK;
}

status_t MPEG4Extractor::parse3GPPMetaData(off64_t offset, size_t size, int depth) {
    if (size < 4 || size == SIZE_MAX) {
        return ERROR_MALFORMED;
    }

    uint8_t *buffer = new (std::nothrow) uint8_t[size + 1];
    if (buffer == NULL) {
        return ERROR_MALFORMED;
    }
    if (mDataSource->readAt(
                offset, buffer, size) != (ssize_t)size) {
        delete[] buffer;
        buffer = NULL;

        return ERROR_IO;
    }

    uint32_t metadataKey = 0;
    switch (mPath[depth]) {
        case FOURCC('t', 'i', 't', 'l'):
        {
            metadataKey = kKeyTitle;
            break;
        }
        case FOURCC('p', 'e', 'r', 'f'):
        {
            metadataKey = kKeyArtist;
            break;
        }
        case FOURCC('a', 'u', 't', 'h'):
        {
            metadataKey = kKeyWriter;
            break;
        }
        case FOURCC('g', 'n', 'r', 'e'):
        {
            metadataKey = kKeyGenre;
            break;
        }
        case FOURCC('a', 'l', 'b', 'm'):
        {
            if (buffer[size - 1] != '\0') {
              char tmp[4];
              sprintf(tmp, "%u", buffer[size - 1]);

              mFileMetaData->setCString(kKeyCDTrackNumber, tmp);
            }

            metadataKey = kKeyAlbum;
            break;
        }
        case FOURCC('y', 'r', 'r', 'c'):
        {
            char tmp[5];
            uint16_t year = U16_AT(&buffer[4]);

            if (year < 10000) {
                sprintf(tmp, "%u", year);

                mFileMetaData->setCString(kKeyYear, tmp);
            }
            break;
        }

        default:
            break;
    }

    if (metadataKey > 0) {
        bool isUTF8 = true; // Common case
        char16_t *framedata = NULL;
        int len16 = 0; // Number of UTF-16 characters

        // smallest possible valid UTF-16 string w BOM: 0xfe 0xff 0x00 0x00
        if (size < 6) {
            return ERROR_MALFORMED;
        }

        if (size - 6 >= 4) {
            len16 = ((size - 6) / 2) - 1; // don't include 0x0000 terminator
            framedata = (char16_t *)(buffer + 6);
            if (0xfffe == *framedata) {
                // endianness marker (BOM) doesn't match host endianness
                for (int i = 0; i < len16; i++) {
                    framedata[i] = bswap_16(framedata[i]);
                }
                // BOM is now swapped to 0xfeff, we will execute next block too
            }

            if (0xfeff == *framedata) {
                // Remove the BOM
                framedata++;
                len16--;
                isUTF8 = false;
            }
            // else normal non-zero-length UTF-8 string
            // we can't handle UTF-16 without BOM as there is no other
            // indication of encoding.
        }

        if (isUTF8) {
            buffer[size] = 0;
            mFileMetaData->setCString(metadataKey, (const char *)buffer + 6);
        } else {
            // Convert from UTF-16 string to UTF-8 string.
            String8 tmpUTF8str(framedata, len16);
            mFileMetaData->setCString(metadataKey, tmpUTF8str.string());
        }
    }

    delete[] buffer;
    buffer = NULL;

    return OK;
}

void MPEG4Extractor::parseID3v2MetaData(off64_t offset) {
    ID3 id3(mDataSource, true /* ignorev1 */, offset);

    if (id3.isValid()) {
        struct Map {
            int key;
            const char *tag1;
            const char *tag2;
        };
        static const Map kMap[] = {
            { kKeyAlbum, "TALB", "TAL" },
            { kKeyArtist, "TPE1", "TP1" },
            { kKeyAlbumArtist, "TPE2", "TP2" },
            { kKeyComposer, "TCOM", "TCM" },
            { kKeyGenre, "TCON", "TCO" },
            { kKeyTitle, "TIT2", "TT2" },
            { kKeyYear, "TYE", "TYER" },
            { kKeyAuthor, "TXT", "TEXT" },
            { kKeyCDTrackNumber, "TRK", "TRCK" },
            { kKeyDiscNumber, "TPA", "TPOS" },
            { kKeyCompilation, "TCP", "TCMP" },
        };
        static const size_t kNumMapEntries = sizeof(kMap) / sizeof(kMap[0]);

        for (size_t i = 0; i < kNumMapEntries; ++i) {
            if (!mFileMetaData->hasData(kMap[i].key)) {
                ID3::Iterator *it = new ID3::Iterator(id3, kMap[i].tag1);
                if (it->done()) {
                    delete it;
                    it = new ID3::Iterator(id3, kMap[i].tag2);
                }

                if (it->done()) {
                    delete it;
                    continue;
                }

                String8 s;
                it->getString(&s);
                delete it;

                mFileMetaData->setCString(kMap[i].key, s);
            }
        }

        size_t dataSize;
        String8 mime;
        const void *data = id3.getAlbumArt(&dataSize, &mime);

        if (data) {
            mFileMetaData->setData(kKeyAlbumArt, MetaData::TYPE_NONE, data, dataSize);
            mFileMetaData->setCString(kKeyAlbumArtMIME, mime.string());
        }
    }
}

sp<IMediaSource> MPEG4Extractor::getTrack(size_t index) {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return NULL;
    }

    Track *track = mFirstTrack;
    while (index > 0) {
        if (track == NULL) {
            return NULL;
        }

        track = track->next;
        --index;
    }

    if (track == NULL) {
        return NULL;
    }


    Trex *trex = NULL;
    int32_t trackId;
    if (track->meta->findInt32(kKeyTrackID, &trackId)) {
        for (size_t i = 0; i < mTrex.size(); i++) {
            Trex *t = &mTrex.editItemAt(i);
            if (t->track_ID == (uint32_t) trackId) {
                trex = t;
                break;
            }
        }
    } else {
        ALOGE("b/21657957");
        return NULL;
    }

    ALOGV("getTrack called, pssh: %zu", mPssh.size());

    const char *mime;
    if (!track->meta->findCString(kKeyMIMEType, &mime)) {
        return NULL;
    }

    if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
        uint32_t type;
        const void *data;
        size_t size;
        if (!track->meta->findData(kKeyAVCC, &type, &data, &size)) {
            return NULL;
        }

        const uint8_t *ptr = (const uint8_t *)data;

        if (size < 7 || ptr[0] != 1) {  // configurationVersion == 1
            return NULL;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC)) {
        uint32_t type;
        const void *data;
        size_t size;
        if (!track->meta->findData(kKeyHVCC, &type, &data, &size)) {
            return NULL;
        }

        const uint8_t *ptr = (const uint8_t *)data;

        if (size < 22 || ptr[0] != 1) {  // configurationVersion == 1
            return NULL;
        }
    }

    return new MPEG4Source(this,
            track->meta, mDataSource, track->timescale, track->sampleTable,
            mSidxEntries, trex, mMoofOffset);
}

// static
status_t MPEG4Extractor::verifyTrack(Track *track) {
    const char *mime;
    CHECK(track->meta->findCString(kKeyMIMEType, &mime));

    uint32_t type;
    const void *data;
    size_t size;
    if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
        if (!track->meta->findData(kKeyAVCC, &type, &data, &size)
                || type != kTypeAVCC) {
            return ERROR_MALFORMED;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC)) {
        if (!track->meta->findData(kKeyHVCC, &type, &data, &size)
                    || type != kTypeHVCC) {
            return ERROR_MALFORMED;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4)
            || !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG2)
            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
        if (!track->meta->findData(kKeyESDS, &type, &data, &size)
                || type != kTypeESDS) {
            return ERROR_MALFORMED;
        }
    }

    if (track->sampleTable == NULL || !track->sampleTable->isValid()) {
        // Make sure we have all the metadata we need.
        ALOGE("stbl atom missing/invalid.");
        return ERROR_MALFORMED;
    }

    if (track->timescale == 0) {
        ALOGE("timescale invalid.");
        return ERROR_MALFORMED;
    }

    return OK;
}

typedef enum {
    //AOT_NONE             = -1,
    //AOT_NULL_OBJECT      = 0,
    //AOT_AAC_MAIN         = 1, /**< Main profile                              */
    AOT_AAC_LC           = 2,   /**< Low Complexity object                     */
    //AOT_AAC_SSR          = 3,
    //AOT_AAC_LTP          = 4,
    AOT_SBR              = 5,
    //AOT_AAC_SCAL         = 6,
    //AOT_TWIN_VQ          = 7,
    //AOT_CELP             = 8,
    //AOT_HVXC             = 9,
    //AOT_RSVD_10          = 10, /**< (reserved)                                */
    //AOT_RSVD_11          = 11, /**< (reserved)                                */
    //AOT_TTSI             = 12, /**< TTSI Object                               */
    //AOT_MAIN_SYNTH       = 13, /**< Main Synthetic object                     */
    //AOT_WAV_TAB_SYNTH    = 14, /**< Wavetable Synthesis object                */
    //AOT_GEN_MIDI         = 15, /**< General MIDI object                       */
    //AOT_ALG_SYNTH_AUD_FX = 16, /**< Algorithmic Synthesis and Audio FX object */
    AOT_ER_AAC_LC        = 17,   /**< Error Resilient(ER) AAC Low Complexity    */
    //AOT_RSVD_18          = 18, /**< (reserved)                                */
    //AOT_ER_AAC_LTP       = 19, /**< Error Resilient(ER) AAC LTP object        */
    AOT_ER_AAC_SCAL      = 20,   /**< Error Resilient(ER) AAC Scalable object   */
    //AOT_ER_TWIN_VQ       = 21, /**< Error Resilient(ER) TwinVQ object         */
    AOT_ER_BSAC          = 22,   /**< Error Resilient(ER) BSAC object           */
    AOT_ER_AAC_LD        = 23,   /**< Error Resilient(ER) AAC LowDelay object   */
    //AOT_ER_CELP          = 24, /**< Error Resilient(ER) CELP object           */
    //AOT_ER_HVXC          = 25, /**< Error Resilient(ER) HVXC object           */
    //AOT_ER_HILN          = 26, /**< Error Resilient(ER) HILN object           */
    //AOT_ER_PARA          = 27, /**< Error Resilient(ER) Parametric object     */
    //AOT_RSVD_28          = 28, /**< might become SSC                          */
    AOT_PS               = 29,   /**< PS, Parametric Stereo (includes SBR)      */
    //AOT_MPEGS            = 30, /**< MPEG Surround                             */

    AOT_ESCAPE           = 31,   /**< Signal AOT uses more than 5 bits          */

    //AOT_MP3ONMP4_L1      = 32, /**< MPEG-Layer1 in mp4                        */
    //AOT_MP3ONMP4_L2      = 33, /**< MPEG-Layer2 in mp4                        */
    //AOT_MP3ONMP4_L3      = 34, /**< MPEG-Layer3 in mp4                        */
    //AOT_RSVD_35          = 35, /**< might become DST                          */
    //AOT_RSVD_36          = 36, /**< might become ALS                          */
    //AOT_AAC_SLS          = 37, /**< AAC + SLS                                 */
    //AOT_SLS              = 38, /**< SLS                                       */
    //AOT_ER_AAC_ELD       = 39, /**< AAC Enhanced Low Delay                    */

    //AOT_USAC             = 42, /**< USAC                                      */
    //AOT_SAOC             = 43, /**< SAOC                                      */
    //AOT_LD_MPEGS         = 44, /**< Low Delay MPEG Surround                   */

    //AOT_RSVD50           = 50,  /**< Interim AOT for Rsvd50                   */
} AUDIO_OBJECT_TYPE;

status_t MPEG4Extractor::updateAudioTrackInfoFromESDS_MPEG4Audio(
        const void *esds_data, size_t esds_size) {
    ESDS esds(esds_data, esds_size);

    uint8_t objectTypeIndication;
    if (esds.getObjectTypeIndication(&objectTypeIndication) != OK) {
        return ERROR_MALFORMED;
    }

    if (objectTypeIndication == 0xe1) {
        // This isn't MPEG4 audio at all, it's QCELP 14k...
        if (mLastTrack == NULL)
            return ERROR_MALFORMED;

        mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_QCELP);
        return OK;
    }

    if (objectTypeIndication  == 0x6b
         || objectTypeIndication  == 0x69) {
         // This is mpeg1/2 audio content, set mimetype to mpeg
         mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
         ALOGD("objectTypeIndication:0x%x, set mimetype to mpeg ",objectTypeIndication);
         return OK;
    }

    const uint8_t *csd;
    size_t csd_size;
    if (esds.getCodecSpecificInfo(
                (const void **)&csd, &csd_size) != OK) {
        return ERROR_MALFORMED;
    }

    if (kUseHexDump) {
        printf("ESD of size %zu\n", csd_size);
        hexdump(csd, csd_size);
    }

    if (csd_size == 0) {
        // There's no further information, i.e. no codec specific data
        // Let's assume that the information provided in the mpeg4 headers
        // is accurate and hope for the best.

        return OK;
    }

    if (csd_size < 2) {
        return ERROR_MALFORMED;
    }

    static uint32_t kSamplingRate[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350
    };

    ABitReader br(csd, csd_size);
    uint32_t objectType = br.getBits(5);

    if (objectType == 31) {  // AAC-ELD => additional 6 bits
        objectType = 32 + br.getBits(6);
    }

    if (mLastTrack == NULL)
        return ERROR_MALFORMED;

    //keep AOT type
    mLastTrack->meta->setInt32(kKeyAACAOT, objectType);

    uint32_t freqIndex = br.getBits(4);

    int32_t sampleRate = 0;
    int32_t numChannels = 0;
    if (freqIndex == 15) {
        if (br.numBitsLeft() < 28) return ERROR_MALFORMED;
        sampleRate = br.getBits(24);
        numChannels = br.getBits(4);
    } else {
        if (br.numBitsLeft() < 4) return ERROR_MALFORMED;
        numChannels = br.getBits(4);

        if (freqIndex == 13 || freqIndex == 14) {
            return ERROR_MALFORMED;
        }

        sampleRate = kSamplingRate[freqIndex];
    }

    if (objectType == AOT_SBR || objectType == AOT_PS) {//SBR specific config per 14496-3 table 1.13
        if (br.numBitsLeft() < 4) return ERROR_MALFORMED;
        uint32_t extFreqIndex = br.getBits(4);
        int32_t extSampleRate __unused;
        if (extFreqIndex == 15) {
            if (csd_size < 8) {
                return ERROR_MALFORMED;
            }
            if (br.numBitsLeft() < 24) return ERROR_MALFORMED;
            extSampleRate = br.getBits(24);
        } else {
            if (extFreqIndex == 13 || extFreqIndex == 14) {
                return ERROR_MALFORMED;
            }
            extSampleRate = kSamplingRate[extFreqIndex];
        }
        //TODO: save the extension sampling rate value in meta data =>
        //      mLastTrack->meta->setInt32(kKeyExtSampleRate, extSampleRate);
    }

    switch (numChannels) {
        // values defined in 14496-3_2009 amendment-4 Table 1.19 - Channel Configuration
        case 0:
        case 1:// FC
        case 2:// FL FR
        case 3:// FC, FL FR
        case 4:// FC, FL FR, RC
        case 5:// FC, FL FR, SL SR
        case 6:// FC, FL FR, SL SR, LFE
            //numChannels already contains the right value
            break;
        case 11:// FC, FL FR, SL SR, RC, LFE
            numChannels = 7;
            break;
        case 7: // FC, FCL FCR, FL FR, SL SR, LFE
        case 12:// FC, FL  FR,  SL SR, RL RR, LFE
        case 14:// FC, FL  FR,  SL SR, LFE, FHL FHR
            numChannels = 8;
            break;
        default:
            return ERROR_UNSUPPORTED;
    }

    {
        if (objectType == AOT_SBR || objectType == AOT_PS) {
            if (br.numBitsLeft() < 5) return ERROR_MALFORMED;
            objectType = br.getBits(5);

            if (objectType == AOT_ESCAPE) {
                if (br.numBitsLeft() < 6) return ERROR_MALFORMED;
                objectType = 32 + br.getBits(6);
            }
        }
        if (objectType == AOT_AAC_LC || objectType == AOT_ER_AAC_LC ||
                objectType == AOT_ER_AAC_LD || objectType == AOT_ER_AAC_SCAL ||
                objectType == AOT_ER_BSAC) {
            if (br.numBitsLeft() < 2) return ERROR_MALFORMED;
            const int32_t frameLengthFlag __unused = br.getBits(1);

            const int32_t dependsOnCoreCoder = br.getBits(1);

            if (dependsOnCoreCoder ) {
                if (br.numBitsLeft() < 14) return ERROR_MALFORMED;
                const int32_t coreCoderDelay __unused = br.getBits(14);
            }

            int32_t extensionFlag = -1;
            if (br.numBitsLeft() > 0) {
                extensionFlag = br.getBits(1);
            } else {
                switch (objectType) {
                // 14496-3 4.5.1.1 extensionFlag
                case AOT_AAC_LC:
                    extensionFlag = 0;
                    break;
                case AOT_ER_AAC_LC:
                case AOT_ER_AAC_SCAL:
                case AOT_ER_BSAC:
                case AOT_ER_AAC_LD:
                    extensionFlag = 1;
                    break;
                default:
                    return ERROR_MALFORMED;
                    break;
                }
                ALOGW("csd missing extension flag; assuming %d for object type %u.",
                        extensionFlag, objectType);
            }

            if (numChannels == 0) {
                int32_t channelsEffectiveNum = 0;
                int32_t channelsNum = 0;
                if (br.numBitsLeft() < 32) {
                    return ERROR_MALFORMED;
                }
                const int32_t ElementInstanceTag __unused = br.getBits(4);
                const int32_t Profile __unused = br.getBits(2);
                const int32_t SamplingFrequencyIndex __unused = br.getBits(4);
                const int32_t NumFrontChannelElements = br.getBits(4);
                const int32_t NumSideChannelElements = br.getBits(4);
                const int32_t NumBackChannelElements = br.getBits(4);
                const int32_t NumLfeChannelElements = br.getBits(2);
                const int32_t NumAssocDataElements __unused = br.getBits(3);
                const int32_t NumValidCcElements __unused = br.getBits(4);

                const int32_t MonoMixdownPresent = br.getBits(1);

                if (MonoMixdownPresent != 0) {
                    if (br.numBitsLeft() < 4) return ERROR_MALFORMED;
                    const int32_t MonoMixdownElementNumber __unused = br.getBits(4);
                }

                if (br.numBitsLeft() < 1) return ERROR_MALFORMED;
                const int32_t StereoMixdownPresent = br.getBits(1);
                if (StereoMixdownPresent != 0) {
                    if (br.numBitsLeft() < 4) return ERROR_MALFORMED;
                    const int32_t StereoMixdownElementNumber __unused = br.getBits(4);
                }

                if (br.numBitsLeft() < 1) return ERROR_MALFORMED;
                const int32_t MatrixMixdownIndexPresent = br.getBits(1);
                if (MatrixMixdownIndexPresent != 0) {
                    if (br.numBitsLeft() < 3) return ERROR_MALFORMED;
                    const int32_t MatrixMixdownIndex __unused = br.getBits(2);
                    const int32_t PseudoSurroundEnable __unused = br.getBits(1);
                }

                int i;
                for (i=0; i < NumFrontChannelElements; i++) {
                    if (br.numBitsLeft() < 5) return ERROR_MALFORMED;
                    const int32_t FrontElementIsCpe = br.getBits(1);
                    const int32_t FrontElementTagSelect __unused = br.getBits(4);
                    channelsNum += FrontElementIsCpe ? 2 : 1;
                }

                for (i=0; i < NumSideChannelElements; i++) {
                    if (br.numBitsLeft() < 5) return ERROR_MALFORMED;
                    const int32_t SideElementIsCpe = br.getBits(1);
                    const int32_t SideElementTagSelect __unused = br.getBits(4);
                    channelsNum += SideElementIsCpe ? 2 : 1;
                }

                for (i=0; i < NumBackChannelElements; i++) {
                    if (br.numBitsLeft() < 5) return ERROR_MALFORMED;
                    const int32_t BackElementIsCpe = br.getBits(1);
                    const int32_t BackElementTagSelect __unused = br.getBits(4);
                    channelsNum += BackElementIsCpe ? 2 : 1;
                }
                channelsEffectiveNum = channelsNum;

                for (i=0; i < NumLfeChannelElements; i++) {
                    if (br.numBitsLeft() < 4) return ERROR_MALFORMED;
                    const int32_t LfeElementTagSelect __unused = br.getBits(4);
                    channelsNum += 1;
                }
                ALOGV("mpeg4 audio channelsNum = %d", channelsNum);
                ALOGV("mpeg4 audio channelsEffectiveNum = %d", channelsEffectiveNum);
                numChannels = channelsNum;
            }
        }
    }

    if (numChannels == 0) {
        return ERROR_UNSUPPORTED;
    }

    if (mLastTrack == NULL)
        return ERROR_MALFORMED;

    int32_t prevSampleRate;
    CHECK(mLastTrack->meta->findInt32(kKeySampleRate, &prevSampleRate));

    if (prevSampleRate != sampleRate) {
        ALOGV("mpeg4 audio sample rate different from previous setting. "
             "was: %d, now: %d", prevSampleRate, sampleRate);
    }

    mLastTrack->meta->setInt32(kKeySampleRate, sampleRate);

    int32_t prevChannelCount;
    CHECK(mLastTrack->meta->findInt32(kKeyChannelCount, &prevChannelCount));

    if (prevChannelCount != numChannels) {
        ALOGV("mpeg4 audio channel count different from previous setting. "
             "was: %d, now: %d", prevChannelCount, numChannels);
    }

    mLastTrack->meta->setInt32(kKeyChannelCount, numChannels);

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

MPEG4Source::MPEG4Source(
        const sp<MPEG4Extractor> &owner,
        const sp<MetaData> &format,
        const sp<DataSource> &dataSource,
        int32_t timeScale,
        const sp<SampleTable> &sampleTable,
        Vector<SidxEntry> &sidx,
        const Trex *trex,
        off64_t firstMoofOffset)
    : mOwner(owner),
      mFormat(format),
      mDataSource(dataSource),
      mTimescale(timeScale),
      mSampleTable(sampleTable),
      mCurrentSampleIndex(0),
      mCurrentFragmentIndex(0),
      mSegments(sidx),
      mTrex(trex),
      mFirstMoofOffset(firstMoofOffset),
      mCurrentMoofOffset(firstMoofOffset),
      mCurrentTime(0),
      mCurrentSampleInfoAllocSize(0),
      mCurrentSampleInfoSizes(NULL),
      mCurrentSampleInfoOffsetsAllocSize(0),
      mCurrentSampleInfoOffsets(NULL),
      mIsAVC(false),
      mIsHEVC(false),
      mNALLengthSize(0),
      mStarted(false),
      mGroup(NULL),
      mBuffer(NULL),
      mWantsNALFragments(false),
      mSrcBuffer(NULL) {

    memset(&mTrackFragmentHeaderInfo, 0, sizeof(mTrackFragmentHeaderInfo));

    mFormat->findInt32(kKeyCryptoMode, &mCryptoMode);
    mDefaultIVSize = 0;
    mFormat->findInt32(kKeyCryptoDefaultIVSize, &mDefaultIVSize);
    uint32_t keytype;
    const void *key;
    size_t keysize;
    if (mFormat->findData(kKeyCryptoKey, &keytype, &key, &keysize)) {
        CHECK(keysize <= 16);
        memset(mCryptoKey, 0, 16);
        memcpy(mCryptoKey, key, keysize);
    }

    const char *mime;
    bool success = mFormat->findCString(kKeyMIMEType, &mime);
    CHECK(success);

    mIsAVC = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC);
    mIsHEVC = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC);

    if (mIsAVC) {
        uint32_t type;
        const void *data;
        size_t size;
        CHECK(format->findData(kKeyAVCC, &type, &data, &size));

        const uint8_t *ptr = (const uint8_t *)data;

        CHECK(size >= 7);
        CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1

        // The number of bytes used to encode the length of a NAL unit.
        mNALLengthSize = 1 + (ptr[4] & 3);
    } else if (mIsHEVC) {
        uint32_t type;
        const void *data;
        size_t size;
        CHECK(format->findData(kKeyHVCC, &type, &data, &size));

        const uint8_t *ptr = (const uint8_t *)data;

        CHECK(size >= 22);
        CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1

        mNALLengthSize = 1 + (ptr[14 + 7] & 3);
    }

    CHECK(format->findInt32(kKeyTrackID, &mTrackId));

    if (mFirstMoofOffset != 0) {
        off64_t offset = mFirstMoofOffset;
        parseChunk(&offset);
    }
}

MPEG4Source::~MPEG4Source() {
    if (mStarted) {
        stop();
    }
    free(mCurrentSampleInfoSizes);
    free(mCurrentSampleInfoOffsets);
}

status_t MPEG4Source::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);

    CHECK(!mStarted);

    int32_t val;
    if (params && params->findInt32(kKeyWantsNALFragments, &val)
        && val != 0) {
        mWantsNALFragments = true;
    } else {
        mWantsNALFragments = false;
    }

    int32_t tmp;
    CHECK(mFormat->findInt32(kKeyMaxInputSize, &tmp));
    size_t max_size = tmp;

    // A somewhat arbitrary limit that should be sufficient for 8k video frames
    // If you see the message below for a valid input stream: increase the limit
    const size_t kMaxBufferSize = 64 * 1024 * 1024;
    if (max_size > kMaxBufferSize) {
        ALOGE("bogus max input size: %zu > %zu", max_size, kMaxBufferSize);
        return ERROR_MALFORMED;
    }
    if (max_size == 0) {
        ALOGE("zero max input size");
        return ERROR_MALFORMED;
    }

    // Allow up to kMaxBuffers, but not if the total exceeds kMaxBufferSize.
    const size_t kMaxBuffers = 8;
    const size_t buffers = min(kMaxBufferSize / max_size, kMaxBuffers);
    mGroup = new MediaBufferGroup(buffers, max_size);
    mSrcBuffer = new (std::nothrow) uint8_t[max_size];
    if (mSrcBuffer == NULL) {
        // file probably specified a bad max size
        delete mGroup;
        mGroup = NULL;
        return ERROR_MALFORMED;
    }

    mStarted = true;

    return OK;
}

status_t MPEG4Source::stop() {
    Mutex::Autolock autoLock(mLock);

    CHECK(mStarted);

    if (mBuffer != NULL) {
        mBuffer->release();
        mBuffer = NULL;
    }

    delete[] mSrcBuffer;
    mSrcBuffer = NULL;

    delete mGroup;
    mGroup = NULL;

    mStarted = false;
    mCurrentSampleIndex = 0;

    return OK;
}

status_t MPEG4Source::parseChunk(off64_t *offset) {
    uint32_t hdr[2];
    if (mDataSource->readAt(*offset, hdr, 8) < 8) {
        return ERROR_IO;
    }
    uint64_t chunk_size = ntohl(hdr[0]);
    uint32_t chunk_type = ntohl(hdr[1]);
    off64_t data_offset = *offset + 8;

    if (chunk_size == 1) {
        if (mDataSource->readAt(*offset + 8, &chunk_size, 8) < 8) {
            return ERROR_IO;
        }
        chunk_size = ntoh64(chunk_size);
        data_offset += 8;

        if (chunk_size < 16) {
            // The smallest valid chunk is 16 bytes long in this case.
            return ERROR_MALFORMED;
        }
    } else if (chunk_size < 8) {
        // The smallest valid chunk is 8 bytes long.
        return ERROR_MALFORMED;
    }

    char chunk[5];
    MakeFourCCString(chunk_type, chunk);
    ALOGV("MPEG4Source chunk %s @ %#llx", chunk, (long long)*offset);

    off64_t chunk_data_size = *offset + chunk_size - data_offset;

    switch(chunk_type) {

        case FOURCC('t', 'r', 'a', 'f'):
        case FOURCC('m', 'o', 'o', 'f'): {
            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset;
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset);
                if (err != OK) {
                    return err;
                }
            }
            if (chunk_type == FOURCC('m', 'o', 'o', 'f')) {
                // *offset points to the box following this moof. Find the next moof from there.

                while (true) {
                    if (mDataSource->readAt(*offset, hdr, 8) < 8) {
                        return ERROR_END_OF_STREAM;
                    }
                    chunk_size = ntohl(hdr[0]);
                    chunk_type = ntohl(hdr[1]);
                    if (chunk_type == FOURCC('m', 'o', 'o', 'f')) {
                        mNextMoofOffset = *offset;
                        break;
                    }
                    *offset += chunk_size;
                }
            }
            break;
        }

        case FOURCC('t', 'f', 'h', 'd'): {
                status_t err;
                if ((err = parseTrackFragmentHeader(data_offset, chunk_data_size)) != OK) {
                    return err;
                }
                *offset += chunk_size;
                break;
        }

        case FOURCC('t', 'r', 'u', 'n'): {
                status_t err;
                if (mLastParsedTrackId == mTrackId) {
                    if ((err = parseTrackFragmentRun(data_offset, chunk_data_size)) != OK) {
                        return err;
                    }
                }

                *offset += chunk_size;
                break;
        }

        case FOURCC('s', 'a', 'i', 'z'): {
            status_t err;
            if ((err = parseSampleAuxiliaryInformationSizes(data_offset, chunk_data_size)) != OK) {
                return err;
            }
            *offset += chunk_size;
            break;
        }
        case FOURCC('s', 'a', 'i', 'o'): {
            status_t err;
            if ((err = parseSampleAuxiliaryInformationOffsets(data_offset, chunk_data_size)) != OK) {
                return err;
            }
            *offset += chunk_size;
            break;
        }

        case FOURCC('m', 'd', 'a', 't'): {
            // parse DRM info if present
            ALOGV("MPEG4Source::parseChunk mdat");
            // if saiz/saoi was previously observed, do something with the sampleinfos
            *offset += chunk_size;
            break;
        }

        default: {
            *offset += chunk_size;
            break;
        }
    }
    return OK;
}

status_t MPEG4Source::parseSampleAuxiliaryInformationSizes(
        off64_t offset, off64_t /* size */) {
    ALOGV("parseSampleAuxiliaryInformationSizes");
    // 14496-12 8.7.12
    uint8_t version;
    if (mDataSource->readAt(
            offset, &version, sizeof(version))
            < (ssize_t)sizeof(version)) {
        return ERROR_IO;
    }

    if (version != 0) {
        return ERROR_UNSUPPORTED;
    }
    offset++;

    uint32_t flags;
    if (!mDataSource->getUInt24(offset, &flags)) {
        return ERROR_IO;
    }
    offset += 3;

    if (flags & 1) {
        uint32_t tmp;
        if (!mDataSource->getUInt32(offset, &tmp)) {
            return ERROR_MALFORMED;
        }
        mCurrentAuxInfoType = tmp;
        offset += 4;
        if (!mDataSource->getUInt32(offset, &tmp)) {
            return ERROR_MALFORMED;
        }
        mCurrentAuxInfoTypeParameter = tmp;
        offset += 4;
    }

    uint8_t defsize;
    if (mDataSource->readAt(offset, &defsize, 1) != 1) {
        return ERROR_MALFORMED;
    }
    mCurrentDefaultSampleInfoSize = defsize;
    offset++;

    uint32_t smplcnt;
    if (!mDataSource->getUInt32(offset, &smplcnt)) {
        return ERROR_MALFORMED;
    }
    mCurrentSampleInfoCount = smplcnt;
    offset += 4;

    if (mCurrentDefaultSampleInfoSize != 0) {
        ALOGV("@@@@ using default sample info size of %d", mCurrentDefaultSampleInfoSize);
        return OK;
    }
    if (smplcnt > mCurrentSampleInfoAllocSize) {
        mCurrentSampleInfoSizes = (uint8_t*) realloc(mCurrentSampleInfoSizes, smplcnt);
        mCurrentSampleInfoAllocSize = smplcnt;
    }

    mDataSource->readAt(offset, mCurrentSampleInfoSizes, smplcnt);
    return OK;
}

status_t MPEG4Source::parseSampleAuxiliaryInformationOffsets(
        off64_t offset, off64_t /* size */) {
    ALOGV("parseSampleAuxiliaryInformationOffsets");
    // 14496-12 8.7.13
    uint8_t version;
    if (mDataSource->readAt(offset, &version, sizeof(version)) != 1) {
        return ERROR_IO;
    }
    offset++;

    uint32_t flags;
    if (!mDataSource->getUInt24(offset, &flags)) {
        return ERROR_IO;
    }
    offset += 3;

    uint32_t entrycount;
    if (!mDataSource->getUInt32(offset, &entrycount)) {
        return ERROR_IO;
    }
    offset += 4;
    if (entrycount == 0) {
        return OK;
    }
    if (entrycount > UINT32_MAX / 8) {
        return ERROR_MALFORMED;
    }

    if (entrycount > mCurrentSampleInfoOffsetsAllocSize) {
        uint64_t *newPtr = (uint64_t *)realloc(mCurrentSampleInfoOffsets, entrycount * 8);
        if (newPtr == NULL) {
            return NO_MEMORY;
        }
        mCurrentSampleInfoOffsets = newPtr;
        mCurrentSampleInfoOffsetsAllocSize = entrycount;
    }
    mCurrentSampleInfoOffsetCount = entrycount;

    if (mCurrentSampleInfoOffsets == NULL) {
        return OK;
    }

    for (size_t i = 0; i < entrycount; i++) {
        if (version == 0) {
            uint32_t tmp;
            if (!mDataSource->getUInt32(offset, &tmp)) {
                return ERROR_IO;
            }
            mCurrentSampleInfoOffsets[i] = tmp;
            offset += 4;
        } else {
            uint64_t tmp;
            if (!mDataSource->getUInt64(offset, &tmp)) {
                return ERROR_IO;
            }
            mCurrentSampleInfoOffsets[i] = tmp;
            offset += 8;
        }
    }

    // parse clear/encrypted data

    off64_t drmoffset = mCurrentSampleInfoOffsets[0]; // from moof

    drmoffset += mCurrentMoofOffset;
    int ivlength;
    CHECK(mFormat->findInt32(kKeyCryptoDefaultIVSize, &ivlength));

    // only 0, 8 and 16 byte initialization vectors are supported
    if (ivlength != 0 && ivlength != 8 && ivlength != 16) {
        ALOGW("unsupported IV length: %d", ivlength);
        return ERROR_MALFORMED;
    }
    // read CencSampleAuxiliaryDataFormats
    for (size_t i = 0; i < mCurrentSampleInfoCount; i++) {
        if (i >= mCurrentSamples.size()) {
            ALOGW("too few samples");
            break;
        }
        Sample *smpl = &mCurrentSamples.editItemAt(i);

        memset(smpl->iv, 0, 16);
        if (mDataSource->readAt(drmoffset, smpl->iv, ivlength) != ivlength) {
            return ERROR_IO;
        }

        drmoffset += ivlength;

        int32_t smplinfosize = mCurrentDefaultSampleInfoSize;
        if (smplinfosize == 0) {
            smplinfosize = mCurrentSampleInfoSizes[i];
        }
        if (smplinfosize > ivlength) {
            uint16_t numsubsamples;
            if (!mDataSource->getUInt16(drmoffset, &numsubsamples)) {
                return ERROR_IO;
            }
            drmoffset += 2;
            for (size_t j = 0; j < numsubsamples; j++) {
                uint16_t numclear;
                uint32_t numencrypted;
                if (!mDataSource->getUInt16(drmoffset, &numclear)) {
                    return ERROR_IO;
                }
                drmoffset += 2;
                if (!mDataSource->getUInt32(drmoffset, &numencrypted)) {
                    return ERROR_IO;
                }
                drmoffset += 4;
                smpl->clearsizes.add(numclear);
                smpl->encryptedsizes.add(numencrypted);
            }
        } else {
            smpl->clearsizes.add(0);
            smpl->encryptedsizes.add(smpl->size);
        }
    }


    return OK;
}

status_t MPEG4Source::parseTrackFragmentHeader(off64_t offset, off64_t size) {

    if (size < 8) {
        return -EINVAL;
    }

    uint32_t flags;
    if (!mDataSource->getUInt32(offset, &flags)) { // actually version + flags
        return ERROR_MALFORMED;
    }

    if (flags & 0xff000000) {
        return -EINVAL;
    }

    if (!mDataSource->getUInt32(offset + 4, (uint32_t*)&mLastParsedTrackId)) {
        return ERROR_MALFORMED;
    }

    if (mLastParsedTrackId != mTrackId) {
        // this is not the right track, skip it
        return OK;
    }

    mTrackFragmentHeaderInfo.mFlags = flags;
    mTrackFragmentHeaderInfo.mTrackID = mLastParsedTrackId;
    offset += 8;
    size -= 8;

    ALOGV("fragment header: %08x %08x", flags, mTrackFragmentHeaderInfo.mTrackID);

    if (flags & TrackFragmentHeaderInfo::kBaseDataOffsetPresent) {
        if (size < 8) {
            return -EINVAL;
        }

        if (!mDataSource->getUInt64(offset, &mTrackFragmentHeaderInfo.mBaseDataOffset)) {
            return ERROR_MALFORMED;
        }
        offset += 8;
        size -= 8;
    }

    if (flags & TrackFragmentHeaderInfo::kSampleDescriptionIndexPresent) {
        if (size < 4) {
            return -EINVAL;
        }

        if (!mDataSource->getUInt32(offset, &mTrackFragmentHeaderInfo.mSampleDescriptionIndex)) {
            return ERROR_MALFORMED;
        }
        offset += 4;
        size -= 4;
    }

    if (flags & TrackFragmentHeaderInfo::kDefaultSampleDurationPresent) {
        if (size < 4) {
            return -EINVAL;
        }

        if (!mDataSource->getUInt32(offset, &mTrackFragmentHeaderInfo.mDefaultSampleDuration)) {
            return ERROR_MALFORMED;
        }
        offset += 4;
        size -= 4;
    }

    if (flags & TrackFragmentHeaderInfo::kDefaultSampleSizePresent) {
        if (size < 4) {
            return -EINVAL;
        }

        if (!mDataSource->getUInt32(offset, &mTrackFragmentHeaderInfo.mDefaultSampleSize)) {
            return ERROR_MALFORMED;
        }
        offset += 4;
        size -= 4;
    }

    if (flags & TrackFragmentHeaderInfo::kDefaultSampleFlagsPresent) {
        if (size < 4) {
            return -EINVAL;
        }

        if (!mDataSource->getUInt32(offset, &mTrackFragmentHeaderInfo.mDefaultSampleFlags)) {
            return ERROR_MALFORMED;
        }
        offset += 4;
        size -= 4;
    }

    if (!(flags & TrackFragmentHeaderInfo::kBaseDataOffsetPresent)) {
        mTrackFragmentHeaderInfo.mBaseDataOffset = mCurrentMoofOffset;
    }

    mTrackFragmentHeaderInfo.mDataOffset = 0;
    return OK;
}

status_t MPEG4Source::parseTrackFragmentRun(off64_t offset, off64_t size) {

    ALOGV("MPEG4Extractor::parseTrackFragmentRun");
    if (size < 8) {
        return -EINVAL;
    }

    enum {
        kDataOffsetPresent                  = 0x01,
        kFirstSampleFlagsPresent            = 0x04,
        kSampleDurationPresent              = 0x100,
        kSampleSizePresent                  = 0x200,
        kSampleFlagsPresent                 = 0x400,
        kSampleCompositionTimeOffsetPresent = 0x800,
    };

    uint32_t flags;
    if (!mDataSource->getUInt32(offset, &flags)) {
        return ERROR_MALFORMED;
    }
    ALOGV("fragment run flags: %08x", flags);

    if (flags & 0xff000000) {
        return -EINVAL;
    }

    if ((flags & kFirstSampleFlagsPresent) && (flags & kSampleFlagsPresent)) {
        // These two shall not be used together.
        return -EINVAL;
    }

    uint32_t sampleCount;
    if (!mDataSource->getUInt32(offset + 4, &sampleCount)) {
        return ERROR_MALFORMED;
    }
    offset += 8;
    size -= 8;

    uint64_t dataOffset = mTrackFragmentHeaderInfo.mDataOffset;

    uint32_t firstSampleFlags = 0;

    if (flags & kDataOffsetPresent) {
        if (size < 4) {
            return -EINVAL;
        }

        int32_t dataOffsetDelta;
        if (!mDataSource->getUInt32(offset, (uint32_t*)&dataOffsetDelta)) {
            return ERROR_MALFORMED;
        }

        dataOffset = mTrackFragmentHeaderInfo.mBaseDataOffset + dataOffsetDelta;

        offset += 4;
        size -= 4;
    }

    if (flags & kFirstSampleFlagsPresent) {
        if (size < 4) {
            return -EINVAL;
        }

        if (!mDataSource->getUInt32(offset, &firstSampleFlags)) {
            return ERROR_MALFORMED;
        }
        offset += 4;
        size -= 4;
    }

    uint32_t sampleDuration = 0, sampleSize = 0, sampleFlags = 0,
             sampleCtsOffset = 0;

    size_t bytesPerSample = 0;
    if (flags & kSampleDurationPresent) {
        bytesPerSample += 4;
    } else if (mTrackFragmentHeaderInfo.mFlags
            & TrackFragmentHeaderInfo::kDefaultSampleDurationPresent) {
        sampleDuration = mTrackFragmentHeaderInfo.mDefaultSampleDuration;
    } else if (mTrex) {
        sampleDuration = mTrex->default_sample_duration;
    }

    if (flags & kSampleSizePresent) {
        bytesPerSample += 4;
    } else if (mTrackFragmentHeaderInfo.mFlags
            & TrackFragmentHeaderInfo::kDefaultSampleSizePresent) {
        sampleSize = mTrackFragmentHeaderInfo.mDefaultSampleSize;
    } else {
        sampleSize = mTrackFragmentHeaderInfo.mDefaultSampleSize;
    }

    if (flags & kSampleFlagsPresent) {
        bytesPerSample += 4;
    } else if (mTrackFragmentHeaderInfo.mFlags
            & TrackFragmentHeaderInfo::kDefaultSampleFlagsPresent) {
        sampleFlags = mTrackFragmentHeaderInfo.mDefaultSampleFlags;
    } else {
        sampleFlags = mTrackFragmentHeaderInfo.mDefaultSampleFlags;
    }

    if (flags & kSampleCompositionTimeOffsetPresent) {
        bytesPerSample += 4;
    } else {
        sampleCtsOffset = 0;
    }

    if (size < (off64_t)(sampleCount * bytesPerSample)) {
        return -EINVAL;
    }

    Sample tmp;
    for (uint32_t i = 0; i < sampleCount; ++i) {
        if (flags & kSampleDurationPresent) {
            if (!mDataSource->getUInt32(offset, &sampleDuration)) {
                return ERROR_MALFORMED;
            }
            offset += 4;
        }

        if (flags & kSampleSizePresent) {
            if (!mDataSource->getUInt32(offset, &sampleSize)) {
                return ERROR_MALFORMED;
            }
            offset += 4;
        }

        if (flags & kSampleFlagsPresent) {
            if (!mDataSource->getUInt32(offset, &sampleFlags)) {
                return ERROR_MALFORMED;
            }
            offset += 4;
        }

        if (flags & kSampleCompositionTimeOffsetPresent) {
            if (!mDataSource->getUInt32(offset, &sampleCtsOffset)) {
                return ERROR_MALFORMED;
            }
            offset += 4;
        }

        ALOGV("adding sample %d at offset 0x%08" PRIx64 ", size %u, duration %u, "
              " flags 0x%08x", i + 1,
                dataOffset, sampleSize, sampleDuration,
                (flags & kFirstSampleFlagsPresent) && i == 0
                    ? firstSampleFlags : sampleFlags);
        tmp.offset = dataOffset;
        tmp.size = sampleSize;
        tmp.duration = sampleDuration;
        tmp.compositionOffset = sampleCtsOffset;
        mCurrentSamples.add(tmp);

        dataOffset += sampleSize;
    }

    mTrackFragmentHeaderInfo.mDataOffset = dataOffset;

    return OK;
}

sp<MetaData> MPEG4Source::getFormat() {
    Mutex::Autolock autoLock(mLock);

    return mFormat;
}

size_t MPEG4Source::parseNALSize(const uint8_t *data) const {
    switch (mNALLengthSize) {
        case 1:
            return *data;
        case 2:
            return U16_AT(data);
        case 3:
            return ((size_t)data[0] << 16) | U16_AT(&data[1]);
        case 4:
            return U32_AT(data);
    }

    // This cannot happen, mNALLengthSize springs to life by adding 1 to
    // a 2-bit integer.
    CHECK(!"Should not be here.");

    return 0;
}

status_t MPEG4Source::read(
        MediaBuffer **out, const ReadOptions *options) {
    Mutex::Autolock autoLock(mLock);

    CHECK(mStarted);

    if (options != nullptr && options->getNonBlocking() && !mGroup->has_buffers()) {
        *out = nullptr;
        return WOULD_BLOCK;
    }

    if (mFirstMoofOffset > 0) {
        return fragmentedRead(out, options);
    }

    *out = NULL;

    int64_t targetSampleTimeUs = -1;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        uint32_t findFlags = 0;
        switch (mode) {
            case ReadOptions::SEEK_PREVIOUS_SYNC:
                findFlags = SampleTable::kFlagBefore;
                break;
            case ReadOptions::SEEK_NEXT_SYNC:
                findFlags = SampleTable::kFlagAfter;
                break;
            case ReadOptions::SEEK_CLOSEST_SYNC:
            case ReadOptions::SEEK_CLOSEST:
                findFlags = SampleTable::kFlagClosest;
                break;
            default:
                CHECK(!"Should not be here.");
                break;
        }

        uint32_t sampleIndex;
        status_t err = mSampleTable->findSampleAtTime(
                seekTimeUs, 1000000, mTimescale,
                &sampleIndex, findFlags);

        if (mode == ReadOptions::SEEK_CLOSEST) {
            // We found the closest sample already, now we want the sync
            // sample preceding it (or the sample itself of course), even
            // if the subsequent sync sample is closer.
            findFlags = SampleTable::kFlagBefore;
        }

        uint32_t syncSampleIndex;
        if (err == OK) {
            err = mSampleTable->findSyncSampleNear(
                    sampleIndex, &syncSampleIndex, findFlags);
        }

        uint32_t sampleTime;
        if (err == OK) {
            err = mSampleTable->getMetaDataForSample(
                    sampleIndex, NULL, NULL, &sampleTime);
        }

        if (err != OK) {
            if (err == ERROR_OUT_OF_RANGE) {
                // An attempt to seek past the end of the stream would
                // normally cause this ERROR_OUT_OF_RANGE error. Propagating
                // this all the way to the MediaPlayer would cause abnormal
                // termination. Legacy behaviour appears to be to behave as if
                // we had seeked to the end of stream, ending normally.
                err = ERROR_END_OF_STREAM;
            }
            ALOGV("end of stream");
            return err;
        }

        if (mode == ReadOptions::SEEK_CLOSEST) {
            targetSampleTimeUs = (sampleTime * 1000000ll) / mTimescale;
        }

#if 0
        uint32_t syncSampleTime;
        CHECK_EQ(OK, mSampleTable->getMetaDataForSample(
                    syncSampleIndex, NULL, NULL, &syncSampleTime));

        ALOGI("seek to time %lld us => sample at time %lld us, "
             "sync sample at time %lld us",
             seekTimeUs,
             sampleTime * 1000000ll / mTimescale,
             syncSampleTime * 1000000ll / mTimescale);
#endif

        mCurrentSampleIndex = syncSampleIndex;
        if (mBuffer != NULL) {
            mBuffer->release();
            mBuffer = NULL;
        }

        // fall through
    }

    off64_t offset;
    size_t size;
    uint32_t cts, stts;
    bool isSyncSample;
    bool newBuffer = false;
    if (mBuffer == NULL) {
        newBuffer = true;

        status_t err =
            mSampleTable->getMetaDataForSample(
                    mCurrentSampleIndex, &offset, &size, &cts, &isSyncSample, &stts);

        if (err != OK) {
            return err;
        }

        err = mGroup->acquire_buffer(&mBuffer);

        if (err != OK) {
            CHECK(mBuffer == NULL);
            return err;
        }
        if (size > mBuffer->size()) {
            ALOGE("buffer too small: %zu > %zu", size, mBuffer->size());
            return ERROR_BUFFER_TOO_SMALL;
        }
    }

    if ((!mIsAVC && !mIsHEVC) || mWantsNALFragments) {
        if (newBuffer) {
            ssize_t num_bytes_read =
                mDataSource->readAt(offset, (uint8_t *)mBuffer->data(), size);

            if (num_bytes_read < (ssize_t)size) {
                mBuffer->release();
                mBuffer = NULL;

                return ERROR_IO;
            }

            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, size);
            mBuffer->meta_data()->clear();
            mBuffer->meta_data()->setInt64(
                    kKeyTime, ((int64_t)cts * 1000000) / mTimescale);
            mBuffer->meta_data()->setInt64(
                    kKeyDuration, ((int64_t)stts * 1000000) / mTimescale);

            if (targetSampleTimeUs >= 0) {
                mBuffer->meta_data()->setInt64(
                        kKeyTargetTime, targetSampleTimeUs);
            }

            if (isSyncSample) {
                mBuffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
            }

            ++mCurrentSampleIndex;
        }

        if (!mIsAVC && !mIsHEVC) {
            *out = mBuffer;
            mBuffer = NULL;

            return OK;
        }

        // Each NAL unit is split up into its constituent fragments and
        // each one of them returned in its own buffer.

        CHECK(mBuffer->range_length() >= mNALLengthSize);

        const uint8_t *src =
            (const uint8_t *)mBuffer->data() + mBuffer->range_offset();

        size_t nal_size = parseNALSize(src);
        if (mNALLengthSize > SIZE_MAX - nal_size) {
            ALOGE("b/24441553, b/24445122");
        }
        if (mBuffer->range_length() - mNALLengthSize < nal_size) {
            ALOGE("incomplete NAL unit.");

            mBuffer->release();
            mBuffer = NULL;

            return ERROR_MALFORMED;
        }

        MediaBuffer *clone = mBuffer->clone();
        CHECK(clone != NULL);
        clone->set_range(mBuffer->range_offset() + mNALLengthSize, nal_size);

        CHECK(mBuffer != NULL);
        mBuffer->set_range(
                mBuffer->range_offset() + mNALLengthSize + nal_size,
                mBuffer->range_length() - mNALLengthSize - nal_size);

        if (mBuffer->range_length() == 0) {
            mBuffer->release();
            mBuffer = NULL;
        }

        *out = clone;

        return OK;
    } else {
        // Whole NAL units are returned but each fragment is prefixed by
        // the start code (0x00 00 00 01).
        ssize_t num_bytes_read = 0;
        int32_t drm = 0;
        bool usesDRM = (mFormat->findInt32(kKeyIsDRM, &drm) && drm != 0);
        if (usesDRM) {
            num_bytes_read =
                mDataSource->readAt(offset, (uint8_t*)mBuffer->data(), size);
        } else {
            num_bytes_read = mDataSource->readAt(offset, mSrcBuffer, size);
        }

        if (num_bytes_read < (ssize_t)size) {
            mBuffer->release();
            mBuffer = NULL;

            return ERROR_IO;
        }

        if (usesDRM) {
            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, size);

        } else {
            uint8_t *dstData = (uint8_t *)mBuffer->data();
            size_t srcOffset = 0;
            size_t dstOffset = 0;

            while (srcOffset < size) {
                bool isMalFormed = !isInRange((size_t)0u, size, srcOffset, mNALLengthSize);
                size_t nalLength = 0;
                if (!isMalFormed) {
                    nalLength = parseNALSize(&mSrcBuffer[srcOffset]);
                    srcOffset += mNALLengthSize;
                    isMalFormed = !isInRange((size_t)0u, size, srcOffset, nalLength);
                }

                if (isMalFormed) {
                    ALOGE("Video is malformed");
                    mBuffer->release();
                    mBuffer = NULL;
                    return ERROR_MALFORMED;
                }

                if (nalLength == 0) {
                    continue;
                }

                if (dstOffset > SIZE_MAX - 4 ||
                        dstOffset + 4 > SIZE_MAX - nalLength ||
                        dstOffset + 4 + nalLength > mBuffer->size()) {
                    ALOGE("b/27208621 : %zu %zu", dstOffset, mBuffer->size());
                    android_errorWriteLog(0x534e4554, "27208621");
                    mBuffer->release();
                    mBuffer = NULL;
                    return ERROR_MALFORMED;
                }

                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 1;
                memcpy(&dstData[dstOffset], &mSrcBuffer[srcOffset], nalLength);
                srcOffset += nalLength;
                dstOffset += nalLength;
            }
            CHECK_EQ(srcOffset, size);
            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, dstOffset);
        }

        mBuffer->meta_data()->clear();
        mBuffer->meta_data()->setInt64(
                kKeyTime, ((int64_t)cts * 1000000) / mTimescale);
        mBuffer->meta_data()->setInt64(
                kKeyDuration, ((int64_t)stts * 1000000) / mTimescale);

        if (targetSampleTimeUs >= 0) {
            mBuffer->meta_data()->setInt64(
                    kKeyTargetTime, targetSampleTimeUs);
        }

        if (mIsAVC) {
            uint32_t layerId = FindAVCLayerId(
                    (const uint8_t *)mBuffer->data(), mBuffer->range_length());
            mBuffer->meta_data()->setInt32(kKeyTemporalLayerId, layerId);
        }

        if (isSyncSample) {
            mBuffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
        }

        ++mCurrentSampleIndex;

        *out = mBuffer;
        mBuffer = NULL;

        return OK;
    }
}

status_t MPEG4Source::fragmentedRead(
        MediaBuffer **out, const ReadOptions *options) {

    ALOGV("MPEG4Source::fragmentedRead");

    CHECK(mStarted);

    *out = NULL;

    int64_t targetSampleTimeUs = -1;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {

        int numSidxEntries = mSegments.size();
        if (numSidxEntries != 0) {
            int64_t totalTime = 0;
            off64_t totalOffset = mFirstMoofOffset;
            for (int i = 0; i < numSidxEntries; i++) {
                const SidxEntry *se = &mSegments[i];
                if (totalTime + se->mDurationUs > seekTimeUs) {
                    // The requested time is somewhere in this segment
                    if ((mode == ReadOptions::SEEK_NEXT_SYNC && seekTimeUs > totalTime) ||
                        (mode == ReadOptions::SEEK_CLOSEST_SYNC &&
                        (seekTimeUs - totalTime) > (totalTime + se->mDurationUs - seekTimeUs))) {
                        // requested next sync, or closest sync and it was closer to the end of
                        // this segment
                        totalTime += se->mDurationUs;
                        totalOffset += se->mSize;
                    }
                    break;
                }
                totalTime += se->mDurationUs;
                totalOffset += se->mSize;
            }
            mCurrentMoofOffset = totalOffset;
            mCurrentSamples.clear();
            mCurrentSampleIndex = 0;
            parseChunk(&totalOffset);
            mCurrentTime = totalTime * mTimescale / 1000000ll;
        } else {
            // without sidx boxes, we can only seek to 0
            mCurrentMoofOffset = mFirstMoofOffset;
            mCurrentSamples.clear();
            mCurrentSampleIndex = 0;
            off64_t tmp = mCurrentMoofOffset;
            parseChunk(&tmp);
            mCurrentTime = 0;
        }

        if (mBuffer != NULL) {
            mBuffer->release();
            mBuffer = NULL;
        }

        // fall through
    }

    off64_t offset = 0;
    size_t size = 0;
    uint32_t cts = 0;
    bool isSyncSample = false;
    bool newBuffer = false;
    if (mBuffer == NULL) {
        newBuffer = true;

        if (mCurrentSampleIndex >= mCurrentSamples.size()) {
            // move to next fragment if there is one
            if (mNextMoofOffset <= mCurrentMoofOffset) {
                return ERROR_END_OF_STREAM;
            }
            off64_t nextMoof = mNextMoofOffset;
            mCurrentMoofOffset = nextMoof;
            mCurrentSamples.clear();
            mCurrentSampleIndex = 0;
            parseChunk(&nextMoof);
            if (mCurrentSampleIndex >= mCurrentSamples.size()) {
                return ERROR_END_OF_STREAM;
            }
        }

        const Sample *smpl = &mCurrentSamples[mCurrentSampleIndex];
        offset = smpl->offset;
        size = smpl->size;
        cts = mCurrentTime + smpl->compositionOffset;
        mCurrentTime += smpl->duration;
        isSyncSample = (mCurrentSampleIndex == 0); // XXX

        status_t err = mGroup->acquire_buffer(&mBuffer);

        if (err != OK) {
            CHECK(mBuffer == NULL);
            ALOGV("acquire_buffer returned %d", err);
            return err;
        }
        if (size > mBuffer->size()) {
            ALOGE("buffer too small: %zu > %zu", size, mBuffer->size());
            return ERROR_BUFFER_TOO_SMALL;
        }
    }

    const Sample *smpl = &mCurrentSamples[mCurrentSampleIndex];
    const sp<MetaData> bufmeta = mBuffer->meta_data();
    bufmeta->clear();
    if (smpl->encryptedsizes.size()) {
        // store clear/encrypted lengths in metadata
        bufmeta->setData(kKeyPlainSizes, 0,
                smpl->clearsizes.array(), smpl->clearsizes.size() * 4);
        bufmeta->setData(kKeyEncryptedSizes, 0,
                smpl->encryptedsizes.array(), smpl->encryptedsizes.size() * 4);
        bufmeta->setData(kKeyCryptoIV, 0, smpl->iv, 16); // use 16 or the actual size?
        bufmeta->setInt32(kKeyCryptoDefaultIVSize, mDefaultIVSize);
        bufmeta->setInt32(kKeyCryptoMode, mCryptoMode);
        bufmeta->setData(kKeyCryptoKey, 0, mCryptoKey, 16);
    }

    if ((!mIsAVC && !mIsHEVC)|| mWantsNALFragments) {
        if (newBuffer) {
            if (!isInRange((size_t)0u, mBuffer->size(), size)) {
                mBuffer->release();
                mBuffer = NULL;

                ALOGE("fragmentedRead ERROR_MALFORMED size %zu", size);
                return ERROR_MALFORMED;
            }

            ssize_t num_bytes_read =
                mDataSource->readAt(offset, (uint8_t *)mBuffer->data(), size);

            if (num_bytes_read < (ssize_t)size) {
                mBuffer->release();
                mBuffer = NULL;

                ALOGE("i/o error");
                return ERROR_IO;
            }

            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, size);
            mBuffer->meta_data()->setInt64(
                    kKeyTime, ((int64_t)cts * 1000000) / mTimescale);
            mBuffer->meta_data()->setInt64(
                    kKeyDuration, ((int64_t)smpl->duration * 1000000) / mTimescale);

            if (targetSampleTimeUs >= 0) {
                mBuffer->meta_data()->setInt64(
                        kKeyTargetTime, targetSampleTimeUs);
            }

            if (mIsAVC) {
                uint32_t layerId = FindAVCLayerId(
                        (const uint8_t *)mBuffer->data(), mBuffer->range_length());
                mBuffer->meta_data()->setInt32(kKeyTemporalLayerId, layerId);
            }

            if (isSyncSample) {
                mBuffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
            }

            ++mCurrentSampleIndex;
        }

        if (!mIsAVC && !mIsHEVC) {
            *out = mBuffer;
            mBuffer = NULL;

            return OK;
        }

        // Each NAL unit is split up into its constituent fragments and
        // each one of them returned in its own buffer.

        CHECK(mBuffer->range_length() >= mNALLengthSize);

        const uint8_t *src =
            (const uint8_t *)mBuffer->data() + mBuffer->range_offset();

        size_t nal_size = parseNALSize(src);
        if (mNALLengthSize > SIZE_MAX - nal_size) {
            ALOGE("b/24441553, b/24445122");
        }

        if (mBuffer->range_length() - mNALLengthSize < nal_size) {
            ALOGE("incomplete NAL unit.");

            mBuffer->release();
            mBuffer = NULL;

            return ERROR_MALFORMED;
        }

        MediaBuffer *clone = mBuffer->clone();
        CHECK(clone != NULL);
        clone->set_range(mBuffer->range_offset() + mNALLengthSize, nal_size);

        CHECK(mBuffer != NULL);
        mBuffer->set_range(
                mBuffer->range_offset() + mNALLengthSize + nal_size,
                mBuffer->range_length() - mNALLengthSize - nal_size);

        if (mBuffer->range_length() == 0) {
            mBuffer->release();
            mBuffer = NULL;
        }

        *out = clone;

        return OK;
    } else {
        ALOGV("whole NAL");
        // Whole NAL units are returned but each fragment is prefixed by
        // the start code (0x00 00 00 01).
        ssize_t num_bytes_read = 0;
        int32_t drm = 0;
        bool usesDRM = (mFormat->findInt32(kKeyIsDRM, &drm) && drm != 0);
        void *data = NULL;
        bool isMalFormed = false;
        if (usesDRM) {
            if (mBuffer == NULL || !isInRange((size_t)0u, mBuffer->size(), size)) {
                isMalFormed = true;
            } else {
                data = mBuffer->data();
            }
        } else {
            int32_t max_size;
            if (mFormat == NULL
                    || !mFormat->findInt32(kKeyMaxInputSize, &max_size)
                    || !isInRange((size_t)0u, (size_t)max_size, size)) {
                isMalFormed = true;
            } else {
                data = mSrcBuffer;
            }
        }

        if (isMalFormed || data == NULL) {
            ALOGE("isMalFormed size %zu", size);
            if (mBuffer != NULL) {
                mBuffer->release();
                mBuffer = NULL;
            }
            return ERROR_MALFORMED;
        }
        num_bytes_read = mDataSource->readAt(offset, data, size);

        if (num_bytes_read < (ssize_t)size) {
            mBuffer->release();
            mBuffer = NULL;

            ALOGE("i/o error");
            return ERROR_IO;
        }

        if (usesDRM) {
            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, size);

        } else {
            uint8_t *dstData = (uint8_t *)mBuffer->data();
            size_t srcOffset = 0;
            size_t dstOffset = 0;

            while (srcOffset < size) {
                isMalFormed = !isInRange((size_t)0u, size, srcOffset, mNALLengthSize);
                size_t nalLength = 0;
                if (!isMalFormed) {
                    nalLength = parseNALSize(&mSrcBuffer[srcOffset]);
                    srcOffset += mNALLengthSize;
                    isMalFormed = !isInRange((size_t)0u, size, srcOffset, nalLength)
                            || !isInRange((size_t)0u, mBuffer->size(), dstOffset, (size_t)4u)
                            || !isInRange((size_t)0u, mBuffer->size(), dstOffset + 4, nalLength);
                }

                if (isMalFormed) {
                    ALOGE("Video is malformed; nalLength %zu", nalLength);
                    mBuffer->release();
                    mBuffer = NULL;
                    return ERROR_MALFORMED;
                }

                if (nalLength == 0) {
                    continue;
                }

                if (dstOffset > SIZE_MAX - 4 ||
                        dstOffset + 4 > SIZE_MAX - nalLength ||
                        dstOffset + 4 + nalLength > mBuffer->size()) {
                    ALOGE("b/26365349 : %zu %zu", dstOffset, mBuffer->size());
                    android_errorWriteLog(0x534e4554, "26365349");
                    mBuffer->release();
                    mBuffer = NULL;
                    return ERROR_MALFORMED;
                }

                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 1;
                memcpy(&dstData[dstOffset], &mSrcBuffer[srcOffset], nalLength);
                srcOffset += nalLength;
                dstOffset += nalLength;
            }
            CHECK_EQ(srcOffset, size);
            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, dstOffset);
        }

        mBuffer->meta_data()->setInt64(
                kKeyTime, ((int64_t)cts * 1000000) / mTimescale);
        mBuffer->meta_data()->setInt64(
                kKeyDuration, ((int64_t)smpl->duration * 1000000) / mTimescale);

        if (targetSampleTimeUs >= 0) {
            mBuffer->meta_data()->setInt64(
                    kKeyTargetTime, targetSampleTimeUs);
        }

        if (isSyncSample) {
            mBuffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
        }

        ++mCurrentSampleIndex;

        *out = mBuffer;
        mBuffer = NULL;

        return OK;
    }
}

MPEG4Extractor::Track *MPEG4Extractor::findTrackByMimePrefix(
        const char *mimePrefix) {
    for (Track *track = mFirstTrack; track != NULL; track = track->next) {
        const char *mime;
        if (track->meta != NULL
                && track->meta->findCString(kKeyMIMEType, &mime)
                && !strncasecmp(mime, mimePrefix, strlen(mimePrefix))) {
            return track;
        }
    }

    return NULL;
}

static bool LegacySniffMPEG4(
        const sp<DataSource> &source, String8 *mimeType, float *confidence) {
    uint8_t header[8];

    ssize_t n = source->readAt(4, header, sizeof(header));
    if (n < (ssize_t)sizeof(header)) {
        return false;
    }

    if (!memcmp(header, "ftyp3g2a", 8) || !memcmp(header, "ftyp3g2b", 8)
        || !memcmp(header, "ftyp3g2c", 8)
        || !memcmp(header, "ftyp3gp", 7) || !memcmp(header, "ftypmp42", 8)
        || !memcmp(header, "ftyp3gr6", 8) || !memcmp(header, "ftyp3gs6", 8)
        || !memcmp(header, "ftyp3ge6", 8) || !memcmp(header, "ftyp3gg6", 8)
        || !memcmp(header, "ftypisom", 8) || !memcmp(header, "ftypM4V ", 8)
        || !memcmp(header, "ftypM4A ", 8) || !memcmp(header, "ftypf4v ", 8)
        || !memcmp(header, "ftypkddi", 8) || !memcmp(header, "ftypM4VP", 8)) {
        *mimeType = MEDIA_MIMETYPE_CONTAINER_MPEG4;
        *confidence = 0.4;

        return true;
    }

    return false;
}

static bool isCompatibleBrand(uint32_t fourcc) {
    static const uint32_t kCompatibleBrands[] = {
        FOURCC('i', 's', 'o', 'm'),
        FOURCC('i', 's', 'o', '2'),
        FOURCC('a', 'v', 'c', '1'),
        FOURCC('h', 'v', 'c', '1'),
        FOURCC('h', 'e', 'v', '1'),
        FOURCC('3', 'g', 'p', '4'),
        FOURCC('m', 'p', '4', '1'),
        FOURCC('m', 'p', '4', '2'),

        // Won't promise that the following file types can be played.
        // Just give these file types a chance.
        FOURCC('q', 't', ' ', ' '),  // Apple's QuickTime
        FOURCC('M', 'S', 'N', 'V'),  // Sony's PSP

        FOURCC('3', 'g', '2', 'a'),  // 3GPP2
        FOURCC('3', 'g', '2', 'b'),
    };

    for (size_t i = 0;
         i < sizeof(kCompatibleBrands) / sizeof(kCompatibleBrands[0]);
         ++i) {
        if (kCompatibleBrands[i] == fourcc) {
            return true;
        }
    }

    return false;
}

// Attempt to actually parse the 'ftyp' atom and determine if a suitable
// compatible brand is present.
// Also try to identify where this file's metadata ends
// (end of the 'moov' atom) and report it to the caller as part of
// the metadata.
static bool BetterSniffMPEG4(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    // We scan up to 128 bytes to identify this file as an MP4.
    static const off64_t kMaxScanOffset = 128ll;

    off64_t offset = 0ll;
    bool foundGoodFileType = false;
    off64_t moovAtomEndOffset = -1ll;
    bool done = false;

    while (!done && offset < kMaxScanOffset) {
        uint32_t hdr[2];
        if (source->readAt(offset, hdr, 8) < 8) {
            return false;
        }

        uint64_t chunkSize = ntohl(hdr[0]);
        uint32_t chunkType = ntohl(hdr[1]);
        off64_t chunkDataOffset = offset + 8;

        if (chunkSize == 1) {
            if (source->readAt(offset + 8, &chunkSize, 8) < 8) {
                return false;
            }

            chunkSize = ntoh64(chunkSize);
            chunkDataOffset += 8;

            if (chunkSize < 16) {
                // The smallest valid chunk is 16 bytes long in this case.
                return false;
            }

        } else if (chunkSize < 8) {
            // The smallest valid chunk is 8 bytes long.
            return false;
        }

        // (data_offset - offset) is either 8 or 16
        off64_t chunkDataSize = chunkSize - (chunkDataOffset - offset);
        if (chunkDataSize < 0) {
            ALOGE("b/23540914");
            return ERROR_MALFORMED;
        }

        char chunkstring[5];
        MakeFourCCString(chunkType, chunkstring);
        ALOGV("saw chunk type %s, size %" PRIu64 " @ %lld", chunkstring, chunkSize, (long long)offset);
        switch (chunkType) {
            case FOURCC('f', 't', 'y', 'p'):
            {
                if (chunkDataSize < 8) {
                    return false;
                }

                uint32_t numCompatibleBrands = (chunkDataSize - 8) / 4;
                for (size_t i = 0; i < numCompatibleBrands + 2; ++i) {
                    if (i == 1) {
                        // Skip this index, it refers to the minorVersion,
                        // not a brand.
                        continue;
                    }

                    uint32_t brand;
                    if (source->readAt(
                                chunkDataOffset + 4 * i, &brand, 4) < 4) {
                        return false;
                    }

                    brand = ntohl(brand);

                    if (isCompatibleBrand(brand)) {
                        foundGoodFileType = true;
                        break;
                    }
                }

                if (!foundGoodFileType) {
                    return false;
                }

                break;
            }

            case FOURCC('m', 'o', 'o', 'v'):
            {
                moovAtomEndOffset = offset + chunkSize;

                done = true;
                break;
            }

            default:
                break;
        }

        offset += chunkSize;
    }

    if (!foundGoodFileType) {
        return false;
    }

    *mimeType = MEDIA_MIMETYPE_CONTAINER_MPEG4;
    *confidence = 0.4f;

    if (moovAtomEndOffset >= 0) {
        *meta = new AMessage;
        (*meta)->setInt64("meta-data-size", moovAtomEndOffset);

        ALOGV("found metadata size: %lld", (long long)moovAtomEndOffset);
    }

    return true;
}

bool SniffMPEG4(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    if (BetterSniffMPEG4(source, mimeType, confidence, meta)) {
        return true;
    }

    if (LegacySniffMPEG4(source, mimeType, confidence)) {
        ALOGW("Identified supported mpeg4 through LegacySniffMPEG4.");
        return true;
    }

    return false;
}

}  // namespace android
