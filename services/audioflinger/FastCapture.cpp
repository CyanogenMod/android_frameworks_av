/*
 * Copyright (C) 2014 The Android Open Source Project
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

#define LOG_TAG "FastCapture"
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_AUDIO

#include "Configuration.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <media/AudioBufferProvider.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include "FastCapture.h"

namespace android {

/*static*/ const FastCaptureState FastCapture::initial;

FastCapture::FastCapture() : FastThread(),
    inputSource(NULL), inputSourceGen(0), pipeSink(NULL), pipeSinkGen(0),
    readBuffer(NULL), readBufferState(-1), format(Format_Invalid), sampleRate(0),
    // dummyDumpState
    totalNativeFramesRead(0)
{
    previous = &initial;
    current = &initial;

    mDummyDumpState = &dummyDumpState;
}

FastCapture::~FastCapture()
{
}

FastCaptureStateQueue* FastCapture::sq()
{
    return &mSQ;
}

const FastThreadState *FastCapture::poll()
{
    return mSQ.poll();
}

void FastCapture::setLog(NBLog::Writer *logWriter __unused)
{
}

void FastCapture::onIdle()
{
    preIdle = *(const FastCaptureState *)current;
    current = &preIdle;
}

void FastCapture::onExit()
{
    delete[] readBuffer;
}

bool FastCapture::isSubClassCommand(FastThreadState::Command command)
{
    switch ((FastCaptureState::Command) command) {
    case FastCaptureState::READ:
    case FastCaptureState::WRITE:
    case FastCaptureState::READ_WRITE:
        return true;
    default:
        return false;
    }
}

void FastCapture::onStateChange()
{
    const FastCaptureState * const current = (const FastCaptureState *) this->current;
    const FastCaptureState * const previous = (const FastCaptureState *) this->previous;
    FastCaptureDumpState * const dumpState = (FastCaptureDumpState *) this->dumpState;
    const size_t frameCount = current->mFrameCount;

    bool eitherChanged = false;

    // check for change in input HAL configuration
    NBAIO_Format previousFormat = format;
    if (current->mInputSourceGen != inputSourceGen) {
        inputSource = current->mInputSource;
        inputSourceGen = current->mInputSourceGen;
        if (inputSource == NULL) {
            format = Format_Invalid;
            sampleRate = 0;
        } else {
            format = inputSource->format();
            sampleRate = Format_sampleRate(format);
            unsigned channelCount = Format_channelCount(format);
            ALOG_ASSERT(channelCount == 1 || channelCount == 2);
        }
        dumpState->mSampleRate = sampleRate;
        eitherChanged = true;
    }

    // check for change in pipe
    if (current->mPipeSinkGen != pipeSinkGen) {
        pipeSink = current->mPipeSink;
        pipeSinkGen = current->mPipeSinkGen;
        eitherChanged = true;
    }

    // input source and pipe sink must be compatible
    if (eitherChanged && inputSource != NULL && pipeSink != NULL) {
        ALOG_ASSERT(Format_isEqual(format, pipeSink->format()));
    }

    if ((!Format_isEqual(format, previousFormat)) || (frameCount != previous->mFrameCount)) {
        // FIXME to avoid priority inversion, don't delete here
        delete[] readBuffer;
        readBuffer = NULL;
        if (frameCount > 0 && sampleRate > 0) {
            // FIXME new may block for unbounded time at internal mutex of the heap
            //       implementation; it would be better to have normal capture thread allocate for
            //       us to avoid blocking here and to prevent possible priority inversion
            unsigned channelCount = Format_channelCount(format);
            // FIXME frameSize
            readBuffer = new short[frameCount * channelCount];
            memset(readBuffer, 0, frameCount * channelCount * sizeof(readBuffer[0]));
            periodNs = (frameCount * 1000000000LL) / sampleRate;    // 1.00
            underrunNs = (frameCount * 1750000000LL) / sampleRate;  // 1.75
            overrunNs = (frameCount * 500000000LL) / sampleRate;    // 0.50
            forceNs = (frameCount * 950000000LL) / sampleRate;      // 0.95
            warmupNs = (frameCount * 500000000LL) / sampleRate;     // 0.50
        } else {
            periodNs = 0;
            underrunNs = 0;
            overrunNs = 0;
            forceNs = 0;
            warmupNs = 0;
        }
        readBufferState = -1;
        dumpState->mFrameCount = frameCount;
    }

}

void FastCapture::onWork()
{
    const FastCaptureState * const current = (const FastCaptureState *) this->current;
    FastCaptureDumpState * const dumpState = (FastCaptureDumpState *) this->dumpState;
    const FastCaptureState::Command command = this->command;
    const size_t frameCount = current->mFrameCount;

    if ((command & FastCaptureState::READ) /*&& isWarm*/) {
        ALOG_ASSERT(inputSource != NULL);
        ALOG_ASSERT(readBuffer != NULL);
        dumpState->mReadSequence++;
        ATRACE_BEGIN("read");
        ssize_t framesRead = inputSource->read(readBuffer, frameCount,
                AudioBufferProvider::kInvalidPTS);
        ATRACE_END();
        dumpState->mReadSequence++;
        if (framesRead >= 0) {
            LOG_ALWAYS_FATAL_IF((size_t) framesRead > frameCount);
            totalNativeFramesRead += framesRead;
            dumpState->mFramesRead = totalNativeFramesRead;
            readBufferState = framesRead;
        } else {
            dumpState->mReadErrors++;
            readBufferState = 0;
        }
        // FIXME rename to attemptedIO
        attemptedWrite = true;
    }

    if (command & FastCaptureState::WRITE) {
        ALOG_ASSERT(pipeSink != NULL);
        ALOG_ASSERT(readBuffer != NULL);
        if (readBufferState < 0) {
            unsigned channelCount = Format_channelCount(format);
            // FIXME frameSize
            memset(readBuffer, 0, frameCount * channelCount * sizeof(short));
            readBufferState = frameCount;
        }
        if (readBufferState > 0) {
            ssize_t framesWritten = pipeSink->write(readBuffer, readBufferState);
            // FIXME This supports at most one fast capture client.
            //       To handle multiple clients this could be converted to an array,
            //       or with a lot more work the control block could be shared by all clients.
            audio_track_cblk_t* cblk = current->mCblk;
            if (cblk != NULL && framesWritten > 0) {
                int32_t rear = cblk->u.mStreaming.mRear;
                android_atomic_release_store(framesWritten + rear, &cblk->u.mStreaming.mRear);
                cblk->mServer += framesWritten;
                int32_t old = android_atomic_or(CBLK_FUTEX_WAKE, &cblk->mFutex);
                if (!(old & CBLK_FUTEX_WAKE)) {
                    // client is never in server process, so don't use FUTEX_WAKE_PRIVATE
                    (void) syscall(__NR_futex, &cblk->mFutex, FUTEX_WAKE, 1);
                }
            }
        }
    }
}

FastCaptureDumpState::FastCaptureDumpState() : FastThreadDumpState(),
    mReadSequence(0), mFramesRead(0), mReadErrors(0), mSampleRate(0), mFrameCount(0)
{
}

FastCaptureDumpState::~FastCaptureDumpState()
{
}

}   // namespace android
