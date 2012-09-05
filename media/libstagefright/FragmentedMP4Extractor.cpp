/*
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
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "FragmentedMP4Extractor"
#include <utils/Log.h>

#include "include/FragmentedMP4Extractor.h"
#include "include/SampleTable.h"
#include "include/ESDS.h"

#include <arpa/inet.h>

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cutils/properties.h> // for property_get

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>

namespace android {

class FragmentedMPEG4Source : public MediaSource {
public:
    // Caller retains ownership of the Parser
    FragmentedMPEG4Source(bool audio,
                const sp<MetaData> &format,
                const sp<FragmentedMP4Parser> &parser,
                const sp<FragmentedMP4Extractor> &extractor);

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();

    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~FragmentedMPEG4Source();

private:
    Mutex mLock;

    sp<MetaData> mFormat;
    sp<FragmentedMP4Parser> mParser;
    sp<FragmentedMP4Extractor> mExtractor;
    bool mIsAudioTrack;
    uint32_t mCurrentSampleIndex;

    bool mIsAVC;
    size_t mNALLengthSize;

    bool mStarted;

    MediaBufferGroup *mGroup;

    bool mWantsNALFragments;

    uint8_t *mSrcBuffer;

    FragmentedMPEG4Source(const FragmentedMPEG4Source &);
    FragmentedMPEG4Source &operator=(const FragmentedMPEG4Source &);
};


FragmentedMP4Extractor::FragmentedMP4Extractor(const sp<DataSource> &source)
    : mLooper(new ALooper),
      mParser(new FragmentedMP4Parser()),
      mDataSource(source),
      mInitCheck(NO_INIT),
      mFileMetaData(new MetaData) {
    ALOGV("FragmentedMP4Extractor");
    mLooper->registerHandler(mParser);
    mLooper->start(false /* runOnCallingThread */);
    mParser->start(mDataSource);

    bool hasVideo = mParser->getFormat(false /* audio */, true /* synchronous */) != NULL;
    bool hasAudio = mParser->getFormat(true /* audio */, true /* synchronous */) != NULL;

    ALOGV("number of tracks: %d", countTracks());

    if (hasVideo) {
        mFileMetaData->setCString(
                kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_MPEG4);
    } else if (hasAudio) {
        mFileMetaData->setCString(kKeyMIMEType, "audio/mp4");
    } else {
        ALOGE("no audio and no video, no idea what file type this is");
    }
    // tracks are numbered such that video track is first, audio track is second
    if (hasAudio && hasVideo) {
        mTrackCount = 2;
        mAudioTrackIndex = 1;
    } else if (hasAudio) {
        mTrackCount = 1;
        mAudioTrackIndex = 0;
    } else if (hasVideo) {
        mTrackCount = 1;
        mAudioTrackIndex = -1;
    } else {
        mTrackCount = 0;
        mAudioTrackIndex = -1;
    }
}

FragmentedMP4Extractor::~FragmentedMP4Extractor() {
    ALOGV("~FragmentedMP4Extractor");
    mLooper->stop();
}

uint32_t FragmentedMP4Extractor::flags() const {
    return CAN_PAUSE |
            (mParser->isSeekable() ? (CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK) : 0);
}

sp<MetaData> FragmentedMP4Extractor::getMetaData() {
    return mFileMetaData;
}

size_t FragmentedMP4Extractor::countTracks() {
    return mTrackCount;
}


sp<MetaData> FragmentedMP4Extractor::getTrackMetaData(
        size_t index, uint32_t flags) {
    if (index >= countTracks()) {
        return NULL;
    }

    sp<AMessage> msg = mParser->getFormat(index == mAudioTrackIndex, true /* synchronous */);

    if (msg == NULL) {
        ALOGV("got null format for track %d", index);
        return NULL;
    }

    sp<MetaData> meta = new MetaData();
    convertMessageToMetaData(msg, meta);
    return meta;
}

static void MakeFourCCString(uint32_t x, char *s) {
    s[0] = x >> 24;
    s[1] = (x >> 16) & 0xff;
    s[2] = (x >> 8) & 0xff;
    s[3] = x & 0xff;
    s[4] = '\0';
}

sp<MediaSource> FragmentedMP4Extractor::getTrack(size_t index) {
    if (index >= countTracks()) {
        return NULL;
    }
    return new FragmentedMPEG4Source(index == mAudioTrackIndex, getTrackMetaData(index, 0), mParser, this);
}


////////////////////////////////////////////////////////////////////////////////

FragmentedMPEG4Source::FragmentedMPEG4Source(
        bool audio,
        const sp<MetaData> &format,
        const sp<FragmentedMP4Parser> &parser,
        const sp<FragmentedMP4Extractor> &extractor)
    : mFormat(format),
      mParser(parser),
      mExtractor(extractor),
      mIsAudioTrack(audio),
      mStarted(false),
      mGroup(NULL),
      mWantsNALFragments(false),
      mSrcBuffer(NULL) {
}

FragmentedMPEG4Source::~FragmentedMPEG4Source() {
    if (mStarted) {
        stop();
    }
}

status_t FragmentedMPEG4Source::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);

    CHECK(!mStarted);

    int32_t val;
    if (params && params->findInt32(kKeyWantsNALFragments, &val)
        && val != 0) {
        mWantsNALFragments = true;
    } else {
        mWantsNALFragments = false;
    }
    ALOGV("caller wants NAL fragments: %s", mWantsNALFragments ? "yes" : "no");

    mGroup = new MediaBufferGroup;

    int32_t max_size = 65536;
    // XXX CHECK(mFormat->findInt32(kKeyMaxInputSize, &max_size));

    mGroup->add_buffer(new MediaBuffer(max_size));

    mSrcBuffer = new uint8_t[max_size];

    mStarted = true;

    return OK;
}

status_t FragmentedMPEG4Source::stop() {
    Mutex::Autolock autoLock(mLock);

    CHECK(mStarted);

    delete[] mSrcBuffer;
    mSrcBuffer = NULL;

    delete mGroup;
    mGroup = NULL;

    mStarted = false;
    mCurrentSampleIndex = 0;

    return OK;
}

sp<MetaData> FragmentedMPEG4Source::getFormat() {
    Mutex::Autolock autoLock(mLock);

    return mFormat;
}


status_t FragmentedMPEG4Source::read(
        MediaBuffer **out, const ReadOptions *options) {
    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        mParser->seekTo(mIsAudioTrack, seekTimeUs);
    }
    MediaBuffer *buffer = NULL;
    mGroup->acquire_buffer(&buffer);
    sp<ABuffer> parseBuffer;

    status_t ret = mParser->dequeueAccessUnit(mIsAudioTrack, &parseBuffer, true /* synchronous */);
    if (ret != OK) {
        buffer->release();
        ALOGV("returning %d", ret);
        return ret;
    }
    sp<AMessage> meta = parseBuffer->meta();
    int64_t timeUs;
    CHECK(meta->findInt64("timeUs", &timeUs));
    buffer->meta_data()->setInt64(kKeyTime, timeUs);
    buffer->set_range(0, parseBuffer->size());
    memcpy(buffer->data(), parseBuffer->data(), parseBuffer->size());
    *out = buffer;
    return OK;
}


static bool isCompatibleBrand(uint32_t fourcc) {
    static const uint32_t kCompatibleBrands[] = {
        FOURCC('i', 's', 'o', 'm'),
        FOURCC('i', 's', 'o', '2'),
        FOURCC('a', 'v', 'c', '1'),
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
static bool Sniff(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    // We scan up to 128k bytes to identify this file as an MP4.
    static const off64_t kMaxScanOffset = 128ll * 1024ll;

    off64_t offset = 0ll;
    bool foundGoodFileType = false;
    bool isFragmented = false;
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

        off64_t chunkDataSize = offset + chunkSize - chunkDataOffset;

        char chunkstring[5];
        MakeFourCCString(chunkType, chunkstring);
        ALOGV("saw chunk type %s, size %lld @ %lld", chunkstring, chunkSize, offset);
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
                    char brandstring[5];
                    MakeFourCCString(brand, brandstring);
                    ALOGV("Brand: %s", brandstring);

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
                break;
            }

            case FOURCC('m', 'o', 'o', 'f'):
            {
                // this is kind of broken, since we might not actually find a
                // moof box in the first 128k.
                isFragmented = true;
                done = true;
                break;
            }

            default:
                break;
        }

        offset += chunkSize;
    }

    if (!foundGoodFileType || !isFragmented) {
        return false;
    }

    *mimeType = MEDIA_MIMETYPE_CONTAINER_MPEG4;
    *confidence = 0.5f; // slightly more than MPEG4Extractor

    if (moovAtomEndOffset >= 0) {
        *meta = new AMessage;
        (*meta)->setInt64("meta-data-size", moovAtomEndOffset);
        (*meta)->setInt32("fragmented", 1); // tell MediaExtractor what to instantiate

        ALOGV("found metadata size: %lld", moovAtomEndOffset);
    }

    return true;
}

// used by DataSource::RegisterDefaultSniffers
bool SniffFragmentedMP4(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    ALOGV("SniffFragmentedMP4");
    char prop[PROPERTY_VALUE_MAX];
    if (property_get("media.stagefright.use-fragmp4", prop, NULL)
            && (!strcmp(prop, "1") || !strcasecmp(prop, "true"))) {
        return Sniff(source, mimeType, confidence, meta);
    }

    return false;
}

}  // namespace android
