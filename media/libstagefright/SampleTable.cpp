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

#define LOG_TAG "SampleTable"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include "include/SampleTable.h"
#include "include/SampleIterator.h"

#include <arpa/inet.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/Utils.h>

/* TODO: remove after being merged into other branches */
#ifndef UINT32_MAX
#define UINT32_MAX       (4294967295U)
#endif

namespace android {

// static
const uint32_t SampleTable::kChunkOffsetType32 = FOURCC('s', 't', 'c', 'o');
// static
const uint32_t SampleTable::kChunkOffsetType64 = FOURCC('c', 'o', '6', '4');
// static
const uint32_t SampleTable::kSampleSizeType32 = FOURCC('s', 't', 's', 'z');
// static
const uint32_t SampleTable::kSampleSizeTypeCompact = FOURCC('s', 't', 'z', '2');

////////////////////////////////////////////////////////////////////////////////

struct SampleTable::CompositionDeltaLookup {
    CompositionDeltaLookup();

    void setEntries(
            const uint32_t *deltaEntries, size_t numDeltaEntries);

    uint32_t getCompositionTimeOffset(uint32_t sampleIndex);

private:
    Mutex mLock;

    const uint32_t *mDeltaEntries;
    size_t mNumDeltaEntries;

    size_t mCurrentDeltaEntry;
    size_t mCurrentEntrySampleIndex;

    DISALLOW_EVIL_CONSTRUCTORS(CompositionDeltaLookup);
};

SampleTable::CompositionDeltaLookup::CompositionDeltaLookup()
    : mDeltaEntries(NULL),
      mNumDeltaEntries(0),
      mCurrentDeltaEntry(0),
      mCurrentEntrySampleIndex(0) {
}

void SampleTable::CompositionDeltaLookup::setEntries(
        const uint32_t *deltaEntries, size_t numDeltaEntries) {
    Mutex::Autolock autolock(mLock);

    mDeltaEntries = deltaEntries;
    mNumDeltaEntries = numDeltaEntries;
    mCurrentDeltaEntry = 0;
    mCurrentEntrySampleIndex = 0;
}

uint32_t SampleTable::CompositionDeltaLookup::getCompositionTimeOffset(
        uint32_t sampleIndex) {
    Mutex::Autolock autolock(mLock);

    if (mDeltaEntries == NULL) {
        return 0;
    }

    if (sampleIndex < mCurrentEntrySampleIndex) {
        mCurrentDeltaEntry = 0;
        mCurrentEntrySampleIndex = 0;
    }

    while (mCurrentDeltaEntry < mNumDeltaEntries) {
        uint32_t sampleCount = mDeltaEntries[2 * mCurrentDeltaEntry];
        if (sampleIndex < mCurrentEntrySampleIndex + sampleCount) {
            return mDeltaEntries[2 * mCurrentDeltaEntry + 1];
        }

        mCurrentEntrySampleIndex += sampleCount;
        ++mCurrentDeltaEntry;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////

SampleTable::SampleTable(const sp<DataSource> &source)
    : mDataSource(source),
      mChunkOffsetOffset(-1),
      mChunkOffsetType(0),
      mNumChunkOffsets(0),
      mSampleToChunkOffset(-1),
      mNumSampleToChunkOffsets(0),
      mSampleSizeOffset(-1),
      mSampleSizeFieldSize(0),
      mDefaultSampleSize(0),
      mNumSampleSizes(0),
      mHasTimeToSample(false),
      mTimeToSampleCount(0),
      mTimeToSample(),
      mSampleTimeEntries(NULL),
      mCompositionTimeDeltaEntries(NULL),
      mNumCompositionTimeDeltaEntries(0),
      mCompositionDeltaLookup(new CompositionDeltaLookup),
      mSyncSampleOffset(-1),
      mNumSyncSamples(0),
      mSyncSamples(NULL),
      mLastSyncSampleIndex(0),
      mSampleToChunkEntries(NULL) {
    mSampleIterator = new SampleIterator(this);
}

SampleTable::~SampleTable() {
    delete[] mSampleToChunkEntries;
    mSampleToChunkEntries = NULL;

    delete[] mSyncSamples;
    mSyncSamples = NULL;

    delete mCompositionDeltaLookup;
    mCompositionDeltaLookup = NULL;

    delete[] mCompositionTimeDeltaEntries;
    mCompositionTimeDeltaEntries = NULL;

    delete[] mSampleTimeEntries;
    mSampleTimeEntries = NULL;

    delete mSampleIterator;
    mSampleIterator = NULL;
}

bool SampleTable::isValid() const {
    return mChunkOffsetOffset >= 0
        && mSampleToChunkOffset >= 0
        && mSampleSizeOffset >= 0
        && mHasTimeToSample;
}

status_t SampleTable::setChunkOffsetParams(
        uint32_t type, off64_t data_offset, size_t data_size) {
    if (mChunkOffsetOffset >= 0) {
        return ERROR_MALFORMED;
    }

    CHECK(type == kChunkOffsetType32 || type == kChunkOffsetType64);

    mChunkOffsetOffset = data_offset;
    mChunkOffsetType = type;

    if (data_size < 8) {
        return ERROR_MALFORMED;
    }

    uint8_t header[8];
    if (mDataSource->readAt(
                data_offset, header, sizeof(header)) < (ssize_t)sizeof(header)) {
        return ERROR_IO;
    }

    if (U32_AT(header) != 0) {
        // Expected version = 0, flags = 0.
        return ERROR_MALFORMED;
    }

    mNumChunkOffsets = U32_AT(&header[4]);

    if (mChunkOffsetType == kChunkOffsetType32) {
        if (data_size < 8 + mNumChunkOffsets * 4) {
            return ERROR_MALFORMED;
        }
    } else {
        if (data_size < 8 + mNumChunkOffsets * 8) {
            return ERROR_MALFORMED;
        }
    }

    return OK;
}

status_t SampleTable::setSampleToChunkParams(
        off64_t data_offset, size_t data_size) {
    if (mSampleToChunkOffset >= 0) {
        return ERROR_MALFORMED;
    }

    mSampleToChunkOffset = data_offset;

    if (data_size < 8) {
        return ERROR_MALFORMED;
    }

    uint8_t header[8];
    if (mDataSource->readAt(
                data_offset, header, sizeof(header)) < (ssize_t)sizeof(header)) {
        return ERROR_IO;
    }

    if (U32_AT(header) != 0) {
        // Expected version = 0, flags = 0.
        return ERROR_MALFORMED;
    }

    mNumSampleToChunkOffsets = U32_AT(&header[4]);

    if (data_size < 8 + mNumSampleToChunkOffsets * 12) {
        return ERROR_MALFORMED;
    }

    if (SIZE_MAX / sizeof(SampleToChunkEntry) <= (size_t)mNumSampleToChunkOffsets)
        return ERROR_OUT_OF_RANGE;

    mSampleToChunkEntries =
        new (std::nothrow) SampleToChunkEntry[mNumSampleToChunkOffsets];
    if (!mSampleToChunkEntries)
        return ERROR_OUT_OF_RANGE;

    for (uint32_t i = 0; i < mNumSampleToChunkOffsets; ++i) {
        uint8_t buffer[12];
        if (mDataSource->readAt(
                    mSampleToChunkOffset + 8 + i * 12, buffer, sizeof(buffer))
                != (ssize_t)sizeof(buffer)) {
            return ERROR_IO;
        }

        CHECK(U32_AT(buffer) >= 1);  // chunk index is 1 based in the spec.

        // We want the chunk index to be 0-based.
        mSampleToChunkEntries[i].startChunk = U32_AT(buffer) - 1;
        mSampleToChunkEntries[i].samplesPerChunk = U32_AT(&buffer[4]);
        mSampleToChunkEntries[i].chunkDesc = U32_AT(&buffer[8]);
    }

    return OK;
}

status_t SampleTable::setSampleSizeParams(
        uint32_t type, off64_t data_offset, size_t data_size) {
    if (mSampleSizeOffset >= 0) {
        return ERROR_MALFORMED;
    }

    CHECK(type == kSampleSizeType32 || type == kSampleSizeTypeCompact);

    mSampleSizeOffset = data_offset;

    if (data_size < 12) {
        return ERROR_MALFORMED;
    }

    uint8_t header[12];
    if (mDataSource->readAt(
                data_offset, header, sizeof(header)) < (ssize_t)sizeof(header)) {
        return ERROR_IO;
    }

    if (U32_AT(header) != 0) {
        // Expected version = 0, flags = 0.
        return ERROR_MALFORMED;
    }

    mDefaultSampleSize = U32_AT(&header[4]);
    mNumSampleSizes = U32_AT(&header[8]);
    if (mNumSampleSizes > (UINT32_MAX - 12) / 16) {
        return ERROR_MALFORMED;
    }

    if (type == kSampleSizeType32) {
        mSampleSizeFieldSize = 32;

        if (mDefaultSampleSize != 0) {
            return OK;
        }

        if (data_size < 12 + mNumSampleSizes * 4) {
            return ERROR_MALFORMED;
        }
    } else {
        if ((mDefaultSampleSize & 0xffffff00) != 0) {
            // The high 24 bits are reserved and must be 0.
            return ERROR_MALFORMED;
        }

        mSampleSizeFieldSize = mDefaultSampleSize & 0xff;
        mDefaultSampleSize = 0;

        if (mSampleSizeFieldSize != 4 && mSampleSizeFieldSize != 8
            && mSampleSizeFieldSize != 16) {
            return ERROR_MALFORMED;
        }

        if (data_size < 12 + (mNumSampleSizes * mSampleSizeFieldSize + 4) / 8) {
            return ERROR_MALFORMED;
        }
    }

    return OK;
}

status_t SampleTable::setTimeToSampleParams(
        off64_t data_offset, size_t data_size) {
    if (mHasTimeToSample || data_size < 8) {
        return ERROR_MALFORMED;
    }

    uint8_t header[8];
    if (mDataSource->readAt(
                data_offset, header, sizeof(header)) < (ssize_t)sizeof(header)) {
        return ERROR_IO;
    }

    if (U32_AT(header) != 0) {
        // Expected version = 0, flags = 0.
        return ERROR_MALFORMED;
    }

    mTimeToSampleCount = U32_AT(&header[4]);
    if ((uint64_t)mTimeToSampleCount >
        (uint64_t)UINT32_MAX / (2 * sizeof(uint32_t))) {
        // Choose this bound because
        // 1) 2 * sizeof(uint32_t) is the amount of memory needed for one
        //    time-to-sample entry in the time-to-sample table.
        // 2) mTimeToSampleCount is the number of entries of the time-to-sample
        //    table.
        // 3) We hope that the table size does not exceed UINT32_MAX.
        ALOGE("  Error: Time-to-sample table size too large.");

        return ERROR_OUT_OF_RANGE;
    }

    // Note: At this point, we know that mTimeToSampleCount * 2 will not
    // overflow because of the above condition.
    if (!mDataSource->getVector(data_offset + 8, &mTimeToSample,
                                mTimeToSampleCount * 2)) {
        ALOGE("  Error: Incomplete data read for time-to-sample table.");
        return ERROR_IO;
    }

    for (size_t i = 0; i < mTimeToSample.size(); ++i) {
        mTimeToSample.editItemAt(i) = ntohl(mTimeToSample[i]);
    }

    mHasTimeToSample = true;
    return OK;
}

status_t SampleTable::setCompositionTimeToSampleParams(
        off64_t data_offset, size_t data_size) {
    ALOGI("There are reordered frames present.");

    if (mCompositionTimeDeltaEntries != NULL || data_size < 8) {
        return ERROR_MALFORMED;
    }

    uint8_t header[8];
    if (mDataSource->readAt(
                data_offset, header, sizeof(header))
            < (ssize_t)sizeof(header)) {
        return ERROR_IO;
    }

    if (U32_AT(header) != 0) {
        // Expected version = 0, flags = 0.
        return ERROR_MALFORMED;
    }

    size_t numEntries = U32_AT(&header[4]);

    if (data_size != (numEntries + 1) * 8) {
        return ERROR_MALFORMED;
    }

    mNumCompositionTimeDeltaEntries = numEntries;
    uint64_t allocSize = (uint64_t)numEntries * 2 * sizeof(uint32_t);
    if (allocSize > UINT32_MAX) {
        return ERROR_OUT_OF_RANGE;
    }

    mCompositionTimeDeltaEntries = new (std::nothrow) uint32_t[2 * numEntries];
    if (!mCompositionTimeDeltaEntries)
        return ERROR_OUT_OF_RANGE;

    if (mDataSource->readAt(
                data_offset + 8, mCompositionTimeDeltaEntries, numEntries * 8)
            < (ssize_t)numEntries * 8) {
        delete[] mCompositionTimeDeltaEntries;
        mCompositionTimeDeltaEntries = NULL;

        return ERROR_IO;
    }

    for (size_t i = 0; i < 2 * numEntries; ++i) {
        mCompositionTimeDeltaEntries[i] = ntohl(mCompositionTimeDeltaEntries[i]);
    }

    mCompositionDeltaLookup->setEntries(
            mCompositionTimeDeltaEntries, mNumCompositionTimeDeltaEntries);

    return OK;
}

status_t SampleTable::setSyncSampleParams(off64_t data_offset, size_t data_size) {
    if (mSyncSampleOffset >= 0 || data_size < 8) {
        return ERROR_MALFORMED;
    }

    mSyncSampleOffset = data_offset;

    uint8_t header[8];
    if (mDataSource->readAt(
                data_offset, header, sizeof(header)) < (ssize_t)sizeof(header)) {
        return ERROR_IO;
    }

    if (U32_AT(header) != 0) {
        // Expected version = 0, flags = 0.
        return ERROR_MALFORMED;
    }

    mNumSyncSamples = U32_AT(&header[4]);

    if (mNumSyncSamples < 2) {
        ALOGV("Table of sync samples is empty or has only a single entry!");
    }

    uint64_t allocSize = mNumSyncSamples * (uint64_t)sizeof(uint32_t);
    if (allocSize > SIZE_MAX) {
        return ERROR_OUT_OF_RANGE;
    }

    mSyncSamples = new (std::nothrow) uint32_t[mNumSyncSamples];
    if (!mSyncSamples)
        return ERROR_OUT_OF_RANGE;

    size_t size = mNumSyncSamples * sizeof(uint32_t);
    if (mDataSource->readAt(mSyncSampleOffset + 8, mSyncSamples, size)
            != (ssize_t)size) {
        return ERROR_IO;
    }

    for (size_t i = 0; i < mNumSyncSamples; ++i) {
        mSyncSamples[i] = ntohl(mSyncSamples[i]) - 1;
    }

    return OK;
}

uint32_t SampleTable::countChunkOffsets() const {
    return mNumChunkOffsets;
}

uint32_t SampleTable::countSamples() const {
    return mNumSampleSizes;
}

status_t SampleTable::getMaxSampleSize(size_t *max_size) {
    Mutex::Autolock autoLock(mLock);

    *max_size = 0;

    for (uint32_t i = 0; i < mNumSampleSizes; ++i) {
        size_t sample_size;
        status_t err = getSampleSize_l(i, &sample_size);

        if (err != OK) {
            return err;
        }

        if (sample_size > *max_size) {
            *max_size = sample_size;
        }
    }

    return OK;
}

uint32_t abs_difference(uint32_t time1, uint32_t time2) {
    return time1 > time2 ? time1 - time2 : time2 - time1;
}

// static
int SampleTable::CompareIncreasingTime(const void *_a, const void *_b) {
    const SampleTimeEntry *a = (const SampleTimeEntry *)_a;
    const SampleTimeEntry *b = (const SampleTimeEntry *)_b;

    if (a->mCompositionTime < b->mCompositionTime) {
        return -1;
    } else if (a->mCompositionTime > b->mCompositionTime) {
        return 1;
    }

    return 0;
}

void SampleTable::buildSampleEntriesTable() {
    Mutex::Autolock autoLock(mLock);

    if (mSampleTimeEntries != NULL || mNumSampleSizes == 0) {
        return;
    }

    mSampleTimeEntries = new (std::nothrow) SampleTimeEntry[mNumSampleSizes];
    if (!mSampleTimeEntries)
        return;

    uint32_t sampleIndex = 0;
    uint32_t sampleTime = 0;

    for (uint32_t i = 0; i < mTimeToSampleCount; ++i) {
        uint32_t n = mTimeToSample[2 * i];
        uint32_t delta = mTimeToSample[2 * i + 1];

        for (uint32_t j = 0; j < n; ++j) {
            if (sampleIndex < mNumSampleSizes) {
                // Technically this should always be the case if the file
                // is well-formed, but you know... there's (gasp) malformed
                // content out there.

                mSampleTimeEntries[sampleIndex].mSampleIndex = sampleIndex;

                uint32_t compTimeDelta =
                    mCompositionDeltaLookup->getCompositionTimeOffset(
                            sampleIndex);

                mSampleTimeEntries[sampleIndex].mCompositionTime =
                    sampleTime + compTimeDelta;
            }

            ++sampleIndex;
            sampleTime += delta;
        }
    }

    qsort(mSampleTimeEntries, mNumSampleSizes, sizeof(SampleTimeEntry),
          CompareIncreasingTime);
}

status_t SampleTable::findSampleAtTime(
        uint64_t req_time, uint64_t scale_num, uint64_t scale_den,
        uint32_t *sample_index, uint32_t flags) {
    buildSampleEntriesTable();

    if (mSampleTimeEntries == NULL) {
        return ERROR_OUT_OF_RANGE;
    }

    uint32_t left = 0;
    uint32_t right_plus_one = mNumSampleSizes;
    while (left < right_plus_one) {
        uint32_t center = left + (right_plus_one - left) / 2;
        uint64_t centerTime =
            getSampleTime(center, scale_num, scale_den);

        if (req_time < centerTime) {
            right_plus_one = center;
        } else if (req_time > centerTime) {
            left = center + 1;
        } else {
            *sample_index = mSampleTimeEntries[center].mSampleIndex;
            return OK;
        }
    }

    uint32_t closestIndex = left;

    if (closestIndex == mNumSampleSizes) {
        if (flags == kFlagAfter) {
            return ERROR_OUT_OF_RANGE;
        }
        flags = kFlagBefore;
    } else if (closestIndex == 0) {
        if (flags == kFlagBefore) {
            // normally we should return out of range, but that is
            // treated as end-of-stream.  instead return first sample
            //
            // return ERROR_OUT_OF_RANGE;
        }
        flags = kFlagAfter;
    }

    switch (flags) {
        case kFlagBefore:
        {
            --closestIndex;
            break;
        }

        case kFlagAfter:
        {
            // nothing to do
            break;
        }

        default:
        {
            CHECK(flags == kFlagClosest);
            // pick closest based on timestamp. use abs_difference for safety
            if (abs_difference(
                    getSampleTime(closestIndex, scale_num, scale_den), req_time) >
                abs_difference(
                    req_time, getSampleTime(closestIndex - 1, scale_num, scale_den))) {
                --closestIndex;
            }
            break;
        }
    }

    *sample_index = mSampleTimeEntries[closestIndex].mSampleIndex;
    return OK;
}

status_t SampleTable::findSyncSampleNear(
        uint32_t start_sample_index, uint32_t *sample_index, uint32_t flags) {
    Mutex::Autolock autoLock(mLock);

    *sample_index = 0;

    if (mSyncSampleOffset < 0) {
        // All samples are sync-samples.
        *sample_index = start_sample_index;
        return OK;
    }

    if (mNumSyncSamples == 0) {
        *sample_index = 0;
        return OK;
    }

    uint32_t left = 0;
    uint32_t right_plus_one = mNumSyncSamples;
    while (left < right_plus_one) {
        uint32_t center = left + (right_plus_one - left) / 2;
        uint32_t x = mSyncSamples[center];

        if (start_sample_index < x) {
            right_plus_one = center;
        } else if (start_sample_index > x) {
            left = center + 1;
        } else {
            *sample_index = x;
            return OK;
        }
    }

    if (left == mNumSyncSamples) {
        if (flags == kFlagAfter) {
            ALOGE("tried to find a sync frame after the last one: %d", left);
            return ERROR_OUT_OF_RANGE;
        }
        flags = kFlagBefore;
    }
    else if (left == 0) {
        if (flags == kFlagBefore) {
            ALOGE("tried to find a sync frame before the first one: %d", left);

            // normally we should return out of range, but that is
            // treated as end-of-stream.  instead seek to first sync
            //
            // return ERROR_OUT_OF_RANGE;
        }
        flags = kFlagAfter;
    }

    // Now ssi[left - 1] <(=) start_sample_index <= ssi[left]
    switch (flags) {
        case kFlagBefore:
        {
            --left;
            break;
        }
        case kFlagAfter:
        {
            // nothing to do
            break;
        }
        default:
        {
            // this route is not used, but implement it nonetheless
            CHECK(flags == kFlagClosest);

            status_t err = mSampleIterator->seekTo(start_sample_index);
            if (err != OK) {
                return err;
            }
            uint32_t sample_time = mSampleIterator->getSampleTime();

            err = mSampleIterator->seekTo(mSyncSamples[left]);
            if (err != OK) {
                return err;
            }
            uint32_t upper_time = mSampleIterator->getSampleTime();

            err = mSampleIterator->seekTo(mSyncSamples[left - 1]);
            if (err != OK) {
                return err;
            }
            uint32_t lower_time = mSampleIterator->getSampleTime();

            // use abs_difference for safety
            if (abs_difference(upper_time, sample_time) >
                abs_difference(sample_time, lower_time)) {
                --left;
            }
            break;
        }
    }

    *sample_index = mSyncSamples[left];
    return OK;
}

status_t SampleTable::findThumbnailSample(uint32_t *sample_index) {
    Mutex::Autolock autoLock(mLock);

    if (mSyncSampleOffset < 0) {
        // All samples are sync-samples.
        *sample_index = 0;
        return OK;
    }

    uint32_t bestSampleIndex = 0;
    size_t maxSampleSize = 0;

    static const size_t kMaxNumSyncSamplesToScan = 20;

    // Consider the first kMaxNumSyncSamplesToScan sync samples and
    // pick the one with the largest (compressed) size as the thumbnail.

    size_t numSamplesToScan = mNumSyncSamples;
    if (numSamplesToScan > kMaxNumSyncSamplesToScan) {
        numSamplesToScan = kMaxNumSyncSamplesToScan;
    }

    for (size_t i = 0; i < numSamplesToScan; ++i) {
        uint32_t x = mSyncSamples[i];

        // Now x is a sample index.
        size_t sampleSize;
        status_t err = getSampleSize_l(x, &sampleSize);
        if (err != OK) {
            return err;
        }

        if (i == 0 || sampleSize > maxSampleSize) {
            bestSampleIndex = x;
            maxSampleSize = sampleSize;
        }
    }

    *sample_index = bestSampleIndex;

    return OK;
}

status_t SampleTable::getSampleSize_l(
        uint32_t sampleIndex, size_t *sampleSize) {
    return mSampleIterator->getSampleSizeDirect(
            sampleIndex, sampleSize);
}

status_t SampleTable::getMetaDataForSample(
        uint32_t sampleIndex,
        off64_t *offset,
        size_t *size,
        uint32_t *compositionTime,
        bool *isSyncSample,
        uint32_t *sampleDuration) {
    Mutex::Autolock autoLock(mLock);

    status_t err;
    if ((err = mSampleIterator->seekTo(sampleIndex)) != OK) {
        return err;
    }

    if (offset) {
        *offset = mSampleIterator->getSampleOffset();
    }

    if (size) {
        *size = mSampleIterator->getSampleSize();
    }

    if (compositionTime) {
        *compositionTime = mSampleIterator->getSampleTime();
    }

    if (isSyncSample) {
        *isSyncSample = false;
        if (mSyncSampleOffset < 0) {
            // Every sample is a sync sample.
            *isSyncSample = true;
        } else {
            size_t i = (mLastSyncSampleIndex < mNumSyncSamples)
                    && (mSyncSamples[mLastSyncSampleIndex] <= sampleIndex)
                ? mLastSyncSampleIndex : 0;

            while (i < mNumSyncSamples && mSyncSamples[i] < sampleIndex) {
                ++i;
            }

            if (i < mNumSyncSamples && mSyncSamples[i] == sampleIndex) {
                *isSyncSample = true;
            }

            mLastSyncSampleIndex = i;
        }
    }

    if (sampleDuration) {
        *sampleDuration = mSampleIterator->getSampleDuration();
    }

    return OK;
}

uint32_t SampleTable::getCompositionTimeOffset(uint32_t sampleIndex) {
    return mCompositionDeltaLookup->getCompositionTimeOffset(sampleIndex);
}

}  // namespace android

