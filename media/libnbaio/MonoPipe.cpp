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

#include <inttypes.h>

#define LOG_TAG "MonoPipe"
//#define LOG_NDEBUG 0

#include <cutils/atomic.h>
#include <cutils/compiler.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <media/AudioBufferProvider.h>
#include <media/nbaio/MonoPipe.h>
#include <audio_utils/roundup.h>


namespace android {

MonoPipe::MonoPipe(size_t reqFrames, const NBAIO_Format& format, bool writeCanBlock) :
        NBAIO_Sink(format),
        mReqFrames(reqFrames),
        mMaxFrames(roundup(reqFrames)),
        mBuffer(malloc(mMaxFrames * Format_frameSize(format))),
        mFront(0),
        mRear(0),
        mWriteTsValid(false),
        // mWriteTs
        mSetpoint((reqFrames * 11) / 16),
        mWriteCanBlock(writeCanBlock),
        mIsShutdown(false),
        // mTimestampShared
        mTimestampMutator(&mTimestampShared),
        mTimestampObserver(&mTimestampShared)
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
            memcpy((char *) mBuffer + (rear * mFrameSize), buffer, part1 * mFrameSize);
            if (CC_UNLIKELY(rear + part1 == mMaxFrames)) {
                size_t part2 = written - part1;
                if (CC_LIKELY(part2 > 0)) {
                    memcpy(mBuffer, (char *) buffer + (part1 * mFrameSize), part2 * mFrameSize);
                }
            }
            android_atomic_release_store(written + mRear, &mRear);
            totalFramesWritten += written;
        }
        if (!mWriteCanBlock || mIsShutdown) {
            break;
        }
        count -= written;
        buffer = (char *) buffer + (written * mFrameSize);
        // Simulate blocking I/O by sleeping at different rates, depending on a throttle.
        // The throttle tries to keep the mean pipe depth near the setpoint, with a slight jitter.
        uint32_t ns;
        if (written > 0) {
            size_t filled = (mMaxFrames - avail) + written;
            // FIXME cache these values to avoid re-computation
            if (filled <= mSetpoint / 2) {
                // pipe is (nearly) empty, fill quickly
                ns = written * ( 500000000 / Format_sampleRate(mFormat));
            } else if (filled <= (mSetpoint * 3) / 4) {
                // pipe is below setpoint, fill at slightly faster rate
                ns = written * ( 750000000 / Format_sampleRate(mFormat));
            } else if (filled <= (mSetpoint * 5) / 4) {
                // pipe is at setpoint, fill at nominal rate
                ns = written * (1000000000 / Format_sampleRate(mFormat));
            } else if (filled <= (mSetpoint * 3) / 2) {
                // pipe is above setpoint, fill at slightly slower rate
                ns = written * (1150000000 / Format_sampleRate(mFormat));
            } else if (filled <= (mSetpoint * 7) / 4) {
                // pipe is overflowing, fill slowly
                ns = written * (1350000000 / Format_sampleRate(mFormat));
            } else {
                // pipe is severely overflowing
                ns = written * (1750000000 / Format_sampleRate(mFormat));
            }
        } else {
            ns = count * (1350000000 / Format_sampleRate(mFormat));
        }
        if (ns > 999999999) {
            ns = 999999999;
        }
        struct timespec nowTs;
        bool nowTsValid = !clock_gettime(CLOCK_MONOTONIC, &nowTs);
        // deduct the elapsed time since previous write() completed
        if (nowTsValid && mWriteTsValid) {
            time_t sec = nowTs.tv_sec - mWriteTs.tv_sec;
            long nsec = nowTs.tv_nsec - mWriteTs.tv_nsec;
            ALOGE_IF(sec < 0 || (sec == 0 && nsec < 0),
                    "clock_gettime(CLOCK_MONOTONIC) failed: was %ld.%09ld but now %ld.%09ld",
                    mWriteTs.tv_sec, mWriteTs.tv_nsec, nowTs.tv_sec, nowTs.tv_nsec);
            if (nsec < 0) {
                --sec;
                nsec += 1000000000;
            }
            if (sec == 0) {
                if ((long) ns > nsec) {
                    ns -= nsec;
                } else {
                    ns = 0;
                }
            }
        }
        if (ns > 0) {
            const struct timespec req = {0, static_cast<long>(ns)};
            nanosleep(&req, NULL);
        }
        // record the time that this write() completed
        if (nowTsValid) {
            mWriteTs = nowTs;
            if ((mWriteTs.tv_nsec += ns) >= 1000000000) {
                mWriteTs.tv_nsec -= 1000000000;
                ++mWriteTs.tv_sec;
            }
        }
        mWriteTsValid = nowTsValid;
    }
    mFramesWritten += totalFramesWritten;
    return totalFramesWritten;
}

void MonoPipe::setAvgFrames(size_t setpoint)
{
    mSetpoint = setpoint;
}

void MonoPipe::shutdown(bool newState)
{
    mIsShutdown = newState;
}

bool MonoPipe::isShutdown()
{
    return mIsShutdown;
}

status_t MonoPipe::getTimestamp(AudioTimestamp& timestamp)
{
    if (mTimestampObserver.poll(timestamp)) {
        return OK;
    }
    return INVALID_OPERATION;
}

}   // namespace android
