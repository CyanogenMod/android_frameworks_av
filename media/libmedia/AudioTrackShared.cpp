/*
 * Copyright (C) 2007 The Android Open Source Project
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

#define LOG_TAG "AudioTrackShared"
//#define LOG_NDEBUG 0

#include <private/media/AudioTrackShared.h>
#include <utils/Log.h>

namespace android {

audio_track_cblk_t::audio_track_cblk_t()
    : lock(Mutex::SHARED), cv(Condition::SHARED), user(0), server(0),
    userBase(0), serverBase(0), frameCount_(0),
    loopStart(UINT_MAX), loopEnd(UINT_MAX), loopCount(0), mVolumeLR(0x10001000),
    mSampleRate(0), mSendLevel(0), flags(0)
{
}

uint32_t audio_track_cblk_t::stepUser(size_t stepCount, size_t frameCount, bool isOut)
{
    ALOGV("stepuser %08x %08x %d", user, server, stepCount);

    uint32_t u = user;
    u += stepCount;
    // Ensure that user is never ahead of server for AudioRecord
    if (isOut) {
        // If stepServer() has been called once, switch to normal obtainBuffer() timeout period
        if (bufferTimeoutMs == MAX_STARTUP_TIMEOUT_MS-1) {
            bufferTimeoutMs = MAX_RUN_TIMEOUT_MS;
        }
    } else if (u > server) {
        ALOGW("stepUser occurred after track reset");
        u = server;
    }

    if (u >= frameCount) {
        // common case, user didn't just wrap
        if (u - frameCount >= userBase ) {
            userBase += frameCount;
        }
    } else if (u >= userBase + frameCount) {
        // user just wrapped
        userBase += frameCount;
    }

    user = u;

    // Clear flow control error condition as new data has been written/read to/from buffer.
    if (flags & CBLK_UNDERRUN) {
        android_atomic_and(~CBLK_UNDERRUN, &flags);
    }

    return u;
}

bool audio_track_cblk_t::stepServer(size_t stepCount, size_t frameCount, bool isOut)
{
    ALOGV("stepserver %08x %08x %d", user, server, stepCount);

    if (!tryLock()) {
        ALOGW("stepServer() could not lock cblk");
        return false;
    }

    uint32_t s = server;
    bool flushed = (s == user);

    s += stepCount;
    if (isOut) {
        // Mark that we have read the first buffer so that next time stepUser() is called
        // we switch to normal obtainBuffer() timeout period
        if (bufferTimeoutMs == MAX_STARTUP_TIMEOUT_MS) {
            bufferTimeoutMs = MAX_STARTUP_TIMEOUT_MS - 1;
        }
        // It is possible that we receive a flush()
        // while the mixer is processing a block: in this case,
        // stepServer() is called After the flush() has reset u & s and
        // we have s > u
        if (flushed) {
            ALOGW("stepServer occurred after track reset");
            s = user;
        }
    }

    if (s >= loopEnd) {
        ALOGW_IF(s > loopEnd, "stepServer: s %u > loopEnd %u", s, loopEnd);
        s = loopStart;
        if (--loopCount == 0) {
            loopEnd = UINT_MAX;
            loopStart = UINT_MAX;
        }
    }

    if (s >= frameCount) {
        // common case, server didn't just wrap
        if (s - frameCount >= serverBase ) {
            serverBase += frameCount;
        }
    } else if (s >= serverBase + frameCount) {
        // server just wrapped
        serverBase += frameCount;
    }

    server = s;

    if (!(flags & CBLK_INVALID)) {
        cv.signal();
    }
    lock.unlock();
    return true;
}

void* audio_track_cblk_t::buffer(void *buffers, size_t frameSize, uint32_t offset) const
{
    return (int8_t *)buffers + (offset - userBase) * frameSize;
}

uint32_t audio_track_cblk_t::framesAvailable(size_t frameCount, bool isOut)
{
    Mutex::Autolock _l(lock);
    return framesAvailable_l(frameCount, isOut);
}

uint32_t audio_track_cblk_t::framesAvailable_l(size_t frameCount, bool isOut)
{
    uint32_t u = user;
    uint32_t s = server;

    if (isOut) {
        uint32_t limit = (s < loopStart) ? s : loopStart;
        return limit + frameCount - u;
    } else {
        return frameCount + u - s;
    }
}

uint32_t audio_track_cblk_t::framesReady(bool isOut)
{
    uint32_t u = user;
    uint32_t s = server;

    if (isOut) {
        if (u < loopEnd) {
            return u - s;
        } else {
            // do not block on mutex shared with client on AudioFlinger side
            if (!tryLock()) {
                ALOGW("framesReady() could not lock cblk");
                return 0;
            }
            uint32_t frames = UINT_MAX;
            if (loopCount >= 0) {
                frames = (loopEnd - loopStart)*loopCount + u - s;
            }
            lock.unlock();
            return frames;
        }
    } else {
        return s - u;
    }
}

bool audio_track_cblk_t::tryLock()
{
    // the code below simulates lock-with-timeout
    // we MUST do this to protect the AudioFlinger server
    // as this lock is shared with the client.
    status_t err;

    err = lock.tryLock();
    if (err == -EBUSY) { // just wait a bit
        usleep(1000);
        err = lock.tryLock();
    }
    if (err != NO_ERROR) {
        // probably, the client just died.
        return false;
    }
    return true;
}

}   // namespace android
