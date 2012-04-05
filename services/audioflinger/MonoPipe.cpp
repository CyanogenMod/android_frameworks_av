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
    if (writeCanBlock) {
        // compute sleep time to be about 2/3 of a full pipe;
        // this gives a balance between risk of underrun vs. too-frequent wakeups
        mSleep.tv_sec = 0;
        uint64_t ns = mMaxFrames * (666666667 / Format_sampleRate(format));
        if (ns > 999999999) {
            ns = 999999999;
        }
        mSleep.tv_nsec = ns;
    }
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
    // count == 0 is unlikely and not worth checking for explicitly; will be handled automatically
    if (CC_UNLIKELY(!mNegotiated)) {
        return NEGOTIATE;
    }
    size_t totalFramesWritten = 0;
    for (;;) {
        size_t written = availableToWrite();
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
        if ((count -= written) == 0 || !mWriteCanBlock) {
            break;
        }
        buffer = (char *) buffer + (written << mBitShift);
        // simulate blocking I/O by sleeping
        nanosleep(&mSleep, NULL);
    }
    mFramesWritten += totalFramesWritten;
    return totalFramesWritten;
}

}   // namespace android
