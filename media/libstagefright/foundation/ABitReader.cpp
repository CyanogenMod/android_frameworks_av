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

#include "ABitReader.h"

#include <media/stagefright/foundation/ADebug.h>

namespace android {

ABitReader::ABitReader(const uint8_t *data, size_t size)
    : mData(data),
      mSize(size),
      mReservoir(0),
      mNumBitsLeft(0) {
}

ABitReader::~ABitReader() {
}

void ABitReader::fillReservoir() {
    CHECK_GT(mSize, 0u);

    mReservoir = 0;
    size_t i;
    for (i = 0; mSize > 0 && i < 4; ++i) {
        mReservoir = (mReservoir << 8) | *mData;

        ++mData;
        --mSize;
    }

    mNumBitsLeft = 8 * i;
    mReservoir <<= 32 - mNumBitsLeft;
}

uint32_t ABitReader::getBits(size_t n) {
    CHECK_LE(n, 32u);

    uint32_t result = 0;
    while (n > 0) {
        if (mNumBitsLeft == 0) {
            fillReservoir();
        }

        size_t m = n;
        if (m > mNumBitsLeft) {
            m = mNumBitsLeft;
        }

        result = (result << m) | (mReservoir >> (32 - m));
        mReservoir <<= m;
        mNumBitsLeft -= m;

        n -= m;
    }

    return result;
}

void ABitReader::skipBits(size_t n) {
    while (n > 32) {
        getBits(32);
        n -= 32;
    }

    if (n > 0) {
        getBits(n);
    }
}

void ABitReader::putBits(uint32_t x, size_t n) {
    CHECK_LE(n, 32u);

    while (mNumBitsLeft + n > 32) {
        mNumBitsLeft -= 8;
        --mData;
        ++mSize;
    }

    mReservoir = (mReservoir >> n) | (x << (32 - n));
    mNumBitsLeft += n;
}

size_t ABitReader::numBitsLeft() const {
    return mSize * 8 + mNumBitsLeft;
}

const uint8_t *ABitReader::data() const {
    return mData - (mNumBitsLeft + 7) / 8;
}

NALBitReader::NALBitReader(const uint8_t *data, size_t size)
    : ABitReader(data, size),
      mNumZeros(0) {
}

bool NALBitReader::atLeastNumBitsLeft(size_t n) const {
    // check against raw size and reservoir bits first
    size_t numBits = numBitsLeft();
    if (n > numBits) {
        return false;
    }

    ssize_t numBitsRemaining = n - mNumBitsLeft;

    size_t size = mSize;
    const uint8_t *data = mData;
    int32_t numZeros = mNumZeros;
    while (size > 0 && numBitsRemaining > 0) {
        bool isEmulationPreventionByte = (numZeros >= 2 && *data == 3);

        if (*data == 0) {
            ++numZeros;
        } else {
            numZeros = 0;
        }

        if (!isEmulationPreventionByte) {
            numBitsRemaining -= 8;
        }

        ++data;
        --size;
    }

    return (numBitsRemaining <= 0);
}

void NALBitReader::fillReservoir() {
    CHECK_GT(mSize, 0u);

    mReservoir = 0;
    size_t i = 0;
    while (mSize > 0 && i < 4) {
        bool isEmulationPreventionByte = (mNumZeros >= 2 && *mData == 3);

        if (*mData == 0) {
            ++mNumZeros;
        } else {
            mNumZeros = 0;
        }

        // skip emulation_prevention_three_byte
        if (!isEmulationPreventionByte) {
            mReservoir = (mReservoir << 8) | *mData;
            ++i;
        }

        ++mData;
        --mSize;
    }

    mNumBitsLeft = 8 * i;
    mReservoir <<= 32 - mNumBitsLeft;
}

}  // namespace android
