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

#define LOG_TAG "MonoPipe"
//#define LOG_NDEBUG 0

#include <cutils/atomic.h>
#include <cutils/compiler.h>
#include <utils/Log.h>
#include "MonoPipe.h"
#include "roundup.h"

namespace android {

MonoPipe::MonoPipe(size_t maxFrames, NBAIO_Format format, bool writeCanBlock) :
        NBAIO_Sink(format),
        mMaxFrames(roundup(maxFrames)),
        mBuffer(malloc(mMaxFrames * Format_frameSize(format))),
        mFront(0),
        mRear(0),
        mWriteCanBlock(writeCanBlock)
{
}

MonoPipe::~MonoPipe()
{
    free(mBuffer);
}

ssize_t MonoPipe::availableToWrite() const
{
    if (CC_UNLIKELY(!mNegotiated)) {
        return NEGOTIATE;
    }
    ssize_t ret = mMaxFrames - (mRear - android_atomic_acquire_load(&mFront));
    ALOG_ASSERT((0 <= ret) && (ret <= mMaxFrames));
    return ret;
}

ssize_t MonoPipe::write(const void *buffer, size_t count)
{
    if (CC_UNLIKELY(!mNegotiated)) {
        return NEGOTIATE;
    }
    size_t totalFramesWritten = 0;
    while (count > 0) {
        size_t avail = availableToWrite();
        size_t written = avail;
        if (CC_LIKELY(written > count)) {
            written = count;
        }
        size_t rear = mRear & (mMaxFrames - 1);
        size_t part1 = mMaxFrames - rear;
        if (part1 > written) {
            part1 = written;
        }
        if (CC_LIKELY(part1 > 0)) {
            memcpy((char *) mBuffer + (rear << mBitShift), buffer, part1 << mBitShift);
            if (CC_UNLIKELY(rear + part1 == mMaxFrames)) {
                size_t part2 = written - part1;
                if (CC_LIKELY(part2 > 0)) {
                    memcpy(mBuffer, (char *) buffer + (part1 << mBitShift), part2 << mBitShift);
                }
            }
            android_atomic_release_store(written + mRear, &mRear);
            totalFramesWritten += written;
        }
        if (!mWriteCanBlock) {
            break;
        }
        count -= written;
        buffer = (char *) buffer + (written << mBitShift);
        // Simulate blocking I/O by sleeping at different rates, depending on a throttle.
        // The throttle tries to keep the pipe about 5/8 full on average, with a slight jitter.
        uint64_t ns;
        enum {
            THROTTLE_VERY_FAST, // pipe is (nearly) empty, fill quickly
            THROTTLE_FAST,      // pipe is normal, fill at slightly faster rate
            THROTTLE_NOMINAL,   // pipe is normal, fill at nominal rate
            THROTTLE_SLOW,      // pipe is normal, fill at slightly slower rate
            THROTTLE_VERY_SLOW, // pipe is (nearly) full, fill slowly
        } throttle;
        avail -= written;
        // FIXME cache these values to avoid re-computation
        if (avail >= (mMaxFrames * 3) / 4) {
            throttle = THROTTLE_VERY_FAST;
        } else if (avail >= mMaxFrames / 2) {
            throttle = THROTTLE_FAST;
        } else if (avail >= (mMaxFrames * 3) / 8) {
            throttle = THROTTLE_NOMINAL;
        } else if (avail >= mMaxFrames / 4) {
            throttle = THROTTLE_SLOW;
        } else {
            throttle = THROTTLE_VERY_SLOW;
        }
        if (written > 0) {
            // FIXME cache these values also
            switch (throttle) {
            case THROTTLE_VERY_FAST:
            default:
                ns = written * ( 500000000 / Format_sampleRate(mFormat));
                break;
            case THROTTLE_FAST:
                ns = written * ( 750000000 / Format_sampleRate(mFormat));
                break;
            case THROTTLE_NOMINAL:
                ns = written * (1000000000 / Format_sampleRate(mFormat));
                break;
            case THROTTLE_SLOW:
                ns = written * (1100000000 / Format_sampleRate(mFormat));
                break;
            case THROTTLE_VERY_SLOW:
                ns = written * (1250000000 / Format_sampleRate(mFormat));
                break;
            }
        } else {
            ns = mMaxFrames * (250000000 / Format_sampleRate(mFormat));
        }
        if (ns > 999999999) {
            ns = 999999999;
        }
        struct timespec sleep;
        sleep.tv_sec = 0;
        sleep.tv_nsec = ns;
        nanosleep(&sleep, NULL);
    }
    mFramesWritten += totalFramesWritten;
    return totalFramesWritten;
}

}   // namespace android
