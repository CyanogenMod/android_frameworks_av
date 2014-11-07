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

#ifndef ANDROID_AUDIO_FAST_CAPTURE_H
#define ANDROID_AUDIO_FAST_CAPTURE_H

#include "FastThread.h"
#include "StateQueue.h"
#include "FastCaptureState.h"

namespace android {

typedef StateQueue<FastCaptureState> FastCaptureStateQueue;

struct FastCaptureDumpState : FastThreadDumpState {
    FastCaptureDumpState();
    /*virtual*/ ~FastCaptureDumpState();

    // FIXME by renaming, could pull up many of these to FastThreadDumpState
    uint32_t mReadSequence;     // incremented before and after each read()
    uint32_t mFramesRead;       // total number of frames read successfully
    uint32_t mReadErrors;       // total number of read() errors
    uint32_t mSampleRate;
    size_t   mFrameCount;
};

class FastCapture : public FastThread {

public:
            FastCapture();
    virtual ~FastCapture();

            FastCaptureStateQueue*  sq();

private:
            FastCaptureStateQueue   mSQ;

    // callouts
    virtual const FastThreadState *poll();
    virtual void setLog(NBLog::Writer *logWriter);
    virtual void onIdle();
    virtual void onExit();
    virtual bool isSubClassCommand(FastThreadState::Command command);
    virtual void onStateChange();
    virtual void onWork();

    static const FastCaptureState initial;
    FastCaptureState preIdle; // copy of state before we went into idle
    // FIXME by renaming, could pull up many of these to FastThread
    NBAIO_Source *inputSource;
    int inputSourceGen;
    NBAIO_Sink *pipeSink;
    int pipeSinkGen;
    short *readBuffer;
    ssize_t readBufferState;    // number of initialized frames in readBuffer, or -1 to clear
    NBAIO_Format format;
    unsigned sampleRate;
    FastCaptureDumpState dummyDumpState;
    uint32_t totalNativeFramesRead; // copied to dumpState->mFramesRead

};  // class FastCapture

}   // namespace android

#endif  // ANDROID_AUDIO_FAST_CAPTURE_H
