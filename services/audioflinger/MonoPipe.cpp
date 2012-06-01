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

MonoPipe::MonoPipe(size_t reqFrames, NBAIO_Format format, bool writeCanBlock) :
        NBAIO_Sink(format),
        mReqFrames(reqFrames),
        mMaxFrames(roundup(reqFrames)),
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
    // uses mMaxFrames not mReqFrames, so allows "over-filling" the pipe beyond requested limit
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
        // can't return a negative value, as we already checked for !mNegotiated
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
        // The throttle tries to keep the pipe about 11/16 full on average, with a slight jitter.
        uint32_t ns;
        if (written > 0) {
            size_t filled = (mMaxFrames - avail) + written;
            // FIXME cache these values to avoid re-computation
            if (filled <= mReqFrames / 4) {
                // pipe is (nearly) empty, fill quickly
                ns = written * ( 500000000 / Format_sampleRate(mFormat));
            } else if (filled <= mReqFrames / 2) {
                // pipe is normal, fill at slightly faster rate
                ns = written * ( 750000000 / Format_sampleRate(mFormat));
            } else if (filled <= (mReqFrames * 5) / 8) {
                // pipe is normal, fill at nominal rate
                ns = written * (1000000000 / Format_sampleRate(mFormat));
            } else if (filled <= (mReqFrames * 3) / 4) {
                // pipe is normal, fill at slightly slower rate
                ns = written * (1100000000 / Format_sampleRate(mFormat));
            } else {
                // pipe is (nearly) full, fill slowly
                ns = written * (1250000000 / Format_sampleRate(mFormat));
            }
        } else {
            ns = mReqFrames * (250000000 / Format_sampleRate(mFormat));
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
