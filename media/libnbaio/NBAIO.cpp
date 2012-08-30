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

#define LOG_TAG "NBAIO"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <media/nbaio/NBAIO.h>

namespace android {

size_t Format_frameSize(NBAIO_Format format)
{
    switch (format) {
    case Format_SR44_1_C2_I16:
    case Format_SR48_C2_I16:
        return 2 * sizeof(short);
    case Format_SR44_1_C1_I16:
    case Format_SR48_C1_I16:
        return 1 * sizeof(short);
    case Format_Invalid:
    default:
        return 0;
    }
}

size_t Format_frameBitShift(NBAIO_Format format)
{
    switch (format) {
    case Format_SR44_1_C2_I16:
    case Format_SR48_C2_I16:
        return 2;   // 1 << 2 == 2 * sizeof(short)
    case Format_SR44_1_C1_I16:
    case Format_SR48_C1_I16:
        return 1;   // 1 << 1 == 1 * sizeof(short)
    case Format_Invalid:
    default:
        return 0;
    }
}

unsigned Format_sampleRate(NBAIO_Format format)
{
    switch (format) {
    case Format_SR44_1_C1_I16:
    case Format_SR44_1_C2_I16:
        return 44100;
    case Format_SR48_C1_I16:
    case Format_SR48_C2_I16:
        return 48000;
    case Format_Invalid:
    default:
        return 0;
    }
}

unsigned Format_channelCount(NBAIO_Format format)
{
    switch (format) {
    case Format_SR44_1_C1_I16:
    case Format_SR48_C1_I16:
        return 1;
    case Format_SR44_1_C2_I16:
    case Format_SR48_C2_I16:
        return 2;
    case Format_Invalid:
    default:
        return 0;
    }
}

NBAIO_Format Format_from_SR_C(unsigned sampleRate, unsigned channelCount)
{
    if (sampleRate == 44100 && channelCount == 2) return Format_SR44_1_C2_I16;
    if (sampleRate == 48000 && channelCount == 2) return Format_SR48_C2_I16;
    if (sampleRate == 44100 && channelCount == 1) return Format_SR44_1_C1_I16;
    if (sampleRate == 48000 && channelCount == 1) return Format_SR48_C1_I16;
    return Format_Invalid;
}

// This is a default implementation; it is expected that subclasses will optimize this.
ssize_t NBAIO_Sink::writeVia(writeVia_t via, size_t total, void *user, size_t block)
{
    if (!mNegotiated) {
        return (ssize_t) NEGOTIATE;
    }
    static const size_t maxBlock = 32;
    size_t frameSize = Format_frameSize(mFormat);
    ALOG_ASSERT(frameSize > 0 && frameSize <= 8);
    // double guarantees alignment for stack similar to what malloc() gives for heap
    if (block == 0 || block > maxBlock) {
        block = maxBlock;
    }
    double buffer[((frameSize * block) + sizeof(double) - 1) / sizeof(double)];
    size_t accumulator = 0;
    while (accumulator < total) {
        size_t count = total - accumulator;
        if (count > block) {
            count = block;
        }
        ssize_t ret = via(user, buffer, count);
        if (ret > 0) {
            ALOG_ASSERT((size_t) ret <= count);
            size_t maxRet = ret;
            ret = write(buffer, maxRet);
            if (ret > 0) {
                ALOG_ASSERT((size_t) ret <= maxRet);
                accumulator += ret;
                continue;
            }
        }
        return accumulator > 0 ? accumulator : ret;
    }
    return accumulator;
}

// This is a default implementation; it is expected that subclasses will optimize this.
ssize_t NBAIO_Source::readVia(readVia_t via, size_t total, void *user,
                              int64_t readPTS, size_t block)
{
    if (!mNegotiated) {
        return (ssize_t) NEGOTIATE;
    }
    static const size_t maxBlock = 32;
    size_t frameSize = Format_frameSize(mFormat);
    ALOG_ASSERT(frameSize > 0 && frameSize <= 8);
    // double guarantees alignment for stack similar to what malloc() gives for heap
    if (block == 0 || block > maxBlock) {
        block = maxBlock;
    }
    double buffer[((frameSize * block) + sizeof(double) - 1) / sizeof(double)];
    size_t accumulator = 0;
    while (accumulator < total) {
        size_t count = total - accumulator;
        if (count > block) {
            count = block;
        }
        ssize_t ret = read(buffer, count, readPTS);
        if (ret > 0) {
            ALOG_ASSERT((size_t) ret <= count);
            size_t maxRet = ret;
            ret = via(user, buffer, maxRet, readPTS);
            if (ret > 0) {
                ALOG_ASSERT((size_t) ret <= maxRet);
                accumulator += ret;
                continue;
            }
        }
        return accumulator > 0 ? accumulator : ret;
    }
    return accumulator;
}

// Default implementation that only accepts my mFormat
ssize_t NBAIO_Port::negotiate(const NBAIO_Format offers[], size_t numOffers,
                                  NBAIO_Format counterOffers[], size_t& numCounterOffers)
{
    ALOGV("negotiate offers=%p numOffers=%u countersOffers=%p numCounterOffers=%u",
            offers, numOffers, counterOffers, numCounterOffers);
    if (mFormat != Format_Invalid) {
        for (size_t i = 0; i < numOffers; ++i) {
            if (offers[i] == mFormat) {
                mNegotiated = true;
                return i;
            }
        }
        if (numCounterOffers > 0) {
            counterOffers[0] = mFormat;
        }
        numCounterOffers = 1;
    } else {
        numCounterOffers = 0;
    }
    return (ssize_t) NEGOTIATE;
}

}   // namespace android
