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

#define LOG_TAG "MonoPipeReader"
//#define LOG_NDEBUG 0

#include <cutils/compiler.h>
#include <utils/Log.h>
#include <media/nbaio/MonoPipeReader.h>

namespace android {

MonoPipeReader::MonoPipeReader(MonoPipe* pipe) :
        NBAIO_Source(pipe->mFormat),
        mPipe(pipe)
{
}

MonoPipeReader::~MonoPipeReader()
{
}

ssize_t MonoPipeReader::availableToRead()
{
    if (CC_UNLIKELY(!mNegotiated)) {
        return NEGOTIATE;
    }
    ssize_t ret = android_atomic_acquire_load(&mPipe->mRear) - mPipe->mFront;
    ALOG_ASSERT((0 <= ret) && ((size_t) ret <= mPipe->mMaxFrames));
    return ret;
}

ssize_t MonoPipeReader::read(void *buffer, size_t count)
{
    // count == 0 is unlikely and not worth checking for explicitly; will be handled automatically
    ssize_t red = availableToRead();
    if (CC_UNLIKELY(red <= 0)) {
        return red;
    }
    if (CC_LIKELY((size_t) red > count)) {
        red = count;
    }
    size_t front = mPipe->mFront & (mPipe->mMaxFrames - 1);
    size_t part1 = mPipe->mMaxFrames - front;
    if (part1 > (size_t) red) {
        part1 = red;
    }
    if (CC_LIKELY(part1 > 0)) {
        memcpy(buffer, (char *) mPipe->mBuffer + (front * mFrameSize), part1 * mFrameSize);
        if (CC_UNLIKELY(front + part1 == mPipe->mMaxFrames)) {
            size_t part2 = red - part1;
            if (CC_LIKELY(part2 > 0)) {
                memcpy((char *) buffer + (part1 * mFrameSize), mPipe->mBuffer, part2 * mFrameSize);
            }
        }
        android_atomic_release_store(red + mPipe->mFront, &mPipe->mFront);
        mFramesRead += red;
    }
    return red;
}

void MonoPipeReader::onTimestamp(const AudioTimestamp& timestamp)
{
    mPipe->mTimestampMutator.push(timestamp);
}

}   // namespace android
