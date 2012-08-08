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
#define LOG_TAG "TrackFragment"
#include <utils/Log.h>

#include "TrackFragment.h"

#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>

namespace android {

Parser::DynamicTrackFragment::DynamicTrackFragment()
    : mComplete(false),
      mSampleIndex(0) {
}

Parser::DynamicTrackFragment::~DynamicTrackFragment() {
}

status_t Parser::DynamicTrackFragment::getSample(SampleInfo *info) {
    if (mSampleIndex >= mSamples.size()) {
        return mComplete ? ERROR_END_OF_STREAM : -EWOULDBLOCK;
    }

    *info = mSamples.itemAt(mSampleIndex);

    return OK;
}

void Parser::DynamicTrackFragment::advance() {
    ++mSampleIndex;
}

void Parser::DynamicTrackFragment::addSample(
        off64_t dataOffset, size_t sampleSize,
        uint32_t presentationTime,
        size_t sampleDescIndex,
        uint32_t flags) {
    mSamples.push();
    SampleInfo *sampleInfo = &mSamples.editItemAt(mSamples.size() - 1);

    sampleInfo->mOffset = dataOffset;
    sampleInfo->mSize = sampleSize;
    sampleInfo->mPresentationTime = presentationTime;
    sampleInfo->mSampleDescIndex = sampleDescIndex;
    sampleInfo->mFlags = flags;
}

status_t Parser::DynamicTrackFragment::signalCompletion() {
    mComplete = true;

    return OK;
}

bool Parser::DynamicTrackFragment::complete() const {
    return mComplete;
}

////////////////////////////////////////////////////////////////////////////////

Parser::StaticTrackFragment::StaticTrackFragment()
    : mSampleIndex(0),
      mSampleCount(0),
      mSampleToChunkIndex(-1),
      mSampleToChunkRemaining(0),
      mPrevChunkIndex(0xffffffff),
      mNextSampleOffset(0) {
}

Parser::StaticTrackFragment::~StaticTrackFragment() {
}

status_t Parser::StaticTrackFragment::getSample(SampleInfo *info) {
    if (mSampleIndex >= mSampleCount) {
        return ERROR_END_OF_STREAM;
    }

    *info = mSampleInfo;

    ALOGV("returning sample %d at [0x%08llx, 0x%08llx)",
          mSampleIndex,
          info->mOffset, info->mOffset + info->mSize);

    return OK;
}

void Parser::StaticTrackFragment::updateSampleInfo() {
    if (mSampleIndex >= mSampleCount) {
        return;
    }

    if (mSampleSizes != NULL) {
        uint32_t defaultSampleSize = U32_AT(mSampleSizes->data() + 4);
        if (defaultSampleSize > 0) {
            mSampleInfo.mSize = defaultSampleSize;
        } else {
            mSampleInfo.mSize= U32_AT(mSampleSizes->data() + 12 + 4 * mSampleIndex);
        }
    } else {
        CHECK(mCompactSampleSizes != NULL);

        uint32_t fieldSize = U32_AT(mCompactSampleSizes->data() + 4);

        switch (fieldSize) {
            case 4:
            {
                unsigned byte = mCompactSampleSizes->data()[12 + mSampleIndex / 2];
                mSampleInfo.mSize = (mSampleIndex & 1) ? byte & 0x0f : byte >> 4;
                break;
            }

            case 8:
            {
                mSampleInfo.mSize = mCompactSampleSizes->data()[12 + mSampleIndex];
                break;
            }

            default:
            {
                CHECK_EQ(fieldSize, 16);
                mSampleInfo.mSize =
                    U16_AT(mCompactSampleSizes->data() + 12 + mSampleIndex * 2);
                break;
            }
        }
    }

    CHECK_GT(mSampleToChunkRemaining, 0);

    // The sample desc index is 1-based... XXX
    mSampleInfo.mSampleDescIndex =
        U32_AT(mSampleToChunk->data() + 8 + 12 * mSampleToChunkIndex + 8);

    uint32_t chunkIndex =
        U32_AT(mSampleToChunk->data() + 8 + 12 * mSampleToChunkIndex);

    CHECK_GE(chunkIndex, 1);
    --chunkIndex;

    if (chunkIndex != mPrevChunkIndex) {
        mPrevChunkIndex = chunkIndex;

        if (mChunkOffsets != NULL) {
            uint32_t entryCount = U32_AT(mChunkOffsets->data() + 4);

            if (chunkIndex >= entryCount) {
                mSampleIndex = mSampleCount;
                return;
            }

            mNextSampleOffset =
                U32_AT(mChunkOffsets->data() + 8 + 4 * chunkIndex);
        } else {
            CHECK(mChunkOffsets64 != NULL);

            uint32_t entryCount = U32_AT(mChunkOffsets64->data() + 4);

            if (chunkIndex >= entryCount) {
                mSampleIndex = mSampleCount;
                return;
            }

            mNextSampleOffset =
                U64_AT(mChunkOffsets64->data() + 8 + 8 * chunkIndex);
        }
    }

    mSampleInfo.mOffset = mNextSampleOffset;

    mSampleInfo.mPresentationTime = 0;
    mSampleInfo.mFlags = 0;
}

void Parser::StaticTrackFragment::advance() {
    mNextSampleOffset += mSampleInfo.mSize;

    ++mSampleIndex;
    if (--mSampleToChunkRemaining == 0) {
        uint32_t entryCount = U32_AT(mSampleToChunk->data() + 4);

        if ((uint32_t)(mSampleToChunkIndex + 1) == entryCount) {
            mSampleIndex = mSampleCount;  // EOS.
            return;
        }

        ++mSampleToChunkIndex;
        mSampleToChunkRemaining =
            U32_AT(mSampleToChunk->data() + 8 + 12 * mSampleToChunkIndex + 4);
    }

    updateSampleInfo();
}

static void setU32At(uint8_t *ptr, uint32_t x) {
    ptr[0] = x >> 24;
    ptr[1] = (x >> 16) & 0xff;
    ptr[2] = (x >> 8) & 0xff;
    ptr[3] = x & 0xff;
}

void Parser::StaticTrackFragment::fixSampleToChunkTableIfNecessary() {
    if (mSampleToChunk == NULL) {
        return;
    }
    uint32_t entryCount = U32_AT(mSampleToChunk->data() + 4);
    uint32_t totalSamples = 0;
    for (uint32_t i = 0; i < entryCount; ++i) {
        totalSamples += U32_AT(mSampleToChunk->data() + 8 + 12 * i + 4);
    }

    if (totalSamples < mSampleCount) {
        // Some samples are not accounted for in the sample-to-chunk
        // data. Fabricate an extra chunk adjacent to the last one
        // in the table with the same sample desription index.

        ALOGW("Faking an extra sample-to-chunk entry for %d samples.",
              mSampleCount - totalSamples);

        uint32_t lastChunkIndex =
            U32_AT(mSampleToChunk->data() + 8 + 12 * (entryCount - 1));

        uint32_t lastSampleDescriptionIndex =
            U32_AT(mSampleToChunk->data() + 8 + 12 * (entryCount - 1) + 8);

        uint8_t *ptr = mSampleToChunk->data() + 8 + 12 * entryCount;

        setU32At(ptr, lastChunkIndex + 1);
        setU32At(ptr + 4, mSampleCount - totalSamples);
        setU32At(ptr + 8, lastSampleDescriptionIndex);
        setU32At(mSampleToChunk->data() + 4, entryCount + 1);
    }
}

status_t Parser::StaticTrackFragment::signalCompletion() {
    fixSampleToChunkTableIfNecessary();

    mSampleToChunkIndex = 0;

    mSampleToChunkRemaining = (mSampleToChunk == NULL) ? 0 :
        U32_AT(mSampleToChunk->data() + 8 + 12 * mSampleToChunkIndex + 4);

    updateSampleInfo();

    return OK;
}

bool Parser::StaticTrackFragment::complete() const {
    return true;
}

status_t Parser::StaticTrackFragment::parseSampleSizes(
        Parser *parser, uint32_t type, size_t offset, uint64_t size) {
    if (offset + 12 > size) {
        return ERROR_MALFORMED;
    }

    if (parser->readU32(offset) != 0) {
        return ERROR_MALFORMED;
    }

    uint32_t sampleSize = parser->readU32(offset + 4);
    uint32_t sampleCount = parser->readU32(offset + 8);

    if (sampleSize == 0 && offset + 12 + sampleCount * 4 != size) {
        return ERROR_MALFORMED;
    }

    parser->copyBuffer(&mSampleSizes, offset, size);

    mSampleCount = sampleCount;

    return OK;
}

status_t Parser::StaticTrackFragment::parseCompactSampleSizes(
        Parser *parser, uint32_t type, size_t offset, uint64_t size) {
    if (offset + 12 > size) {
        return ERROR_MALFORMED;
    }

    if (parser->readU32(offset) != 0) {
        return ERROR_MALFORMED;
    }

    uint32_t fieldSize = parser->readU32(offset + 4);

    if (fieldSize != 4 && fieldSize != 8 && fieldSize != 16) {
        return ERROR_MALFORMED;
    }

    uint32_t sampleCount = parser->readU32(offset + 8);

    if (offset + 12 + (sampleCount * fieldSize + 4) / 8 != size) {
        return ERROR_MALFORMED;
    }

    parser->copyBuffer(&mCompactSampleSizes, offset, size);

    mSampleCount = sampleCount;

    return OK;
}

status_t Parser::StaticTrackFragment::parseSampleToChunk(
        Parser *parser, uint32_t type, size_t offset, uint64_t size) {
    if (offset + 8 > size) {
        return ERROR_MALFORMED;
    }

    if (parser->readU32(offset) != 0) {
        return ERROR_MALFORMED;
    }

    uint32_t entryCount = parser->readU32(offset + 4);

    if (entryCount == 0) {
        return OK;
    }

    if (offset + 8 + entryCount * 12 != size) {
        return ERROR_MALFORMED;
    }

    parser->copyBuffer(&mSampleToChunk, offset, size, 12 /* extra */);

    return OK;
}

status_t Parser::StaticTrackFragment::parseChunkOffsets(
        Parser *parser, uint32_t type, size_t offset, uint64_t size) {
    if (offset + 8 > size) {
        return ERROR_MALFORMED;
    }

    if (parser->readU32(offset) != 0) {
        return ERROR_MALFORMED;
    }

    uint32_t entryCount = parser->readU32(offset + 4);

    if (offset + 8 + entryCount * 4 != size) {
        return ERROR_MALFORMED;
    }

    parser->copyBuffer(&mChunkOffsets, offset, size);

    return OK;
}

status_t Parser::StaticTrackFragment::parseChunkOffsets64(
        Parser *parser, uint32_t type, size_t offset, uint64_t size) {
    if (offset + 8 > size) {
        return ERROR_MALFORMED;
    }

    if (parser->readU32(offset) != 0) {
        return ERROR_MALFORMED;
    }

    uint32_t entryCount = parser->readU32(offset + 4);

    if (offset + 8 + entryCount * 8 != size) {
        return ERROR_MALFORMED;
    }

    parser->copyBuffer(&mChunkOffsets64, offset, size);

    return OK;
}

}  // namespace android

