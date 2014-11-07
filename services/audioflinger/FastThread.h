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

#ifndef ANDROID_AUDIO_FAST_THREAD_H
#define ANDROID_AUDIO_FAST_THREAD_H

#include "Configuration.h"
#ifdef CPU_FREQUENCY_STATISTICS
#include <cpustats/ThreadCpuUsage.h>
#endif
#include <utils/Thread.h>
#include "FastThreadState.h"

namespace android {

// FastThread is the common abstract base class of FastMixer and FastCapture
class FastThread : public Thread {

public:
            FastThread();
    virtual ~FastThread();

private:
    // implement Thread::threadLoop()
    virtual bool threadLoop();

protected:
    // callouts to subclass in same lexical order as they were in original FastMixer.cpp
    // FIXME need comments
    virtual const FastThreadState *poll() = 0;
    virtual void setLog(NBLog::Writer *logWriter __unused) { }
    virtual void onIdle() = 0;
    virtual void onExit() = 0;
    virtual bool isSubClassCommand(FastThreadState::Command command) = 0;
    virtual void onStateChange() = 0;
    virtual void onWork() = 0;

    // FIXME these former local variables need comments and to be renamed to have an "m" prefix
    const FastThreadState *previous;
    const FastThreadState *current;
    struct timespec oldTs;
    bool oldTsValid;
    long sleepNs;   // -1: busy wait, 0: sched_yield, > 0: nanosleep
    long periodNs;      // expected period; the time required to render one mix buffer
    long underrunNs;    // underrun likely when write cycle is greater than this value
    long overrunNs;     // overrun likely when write cycle is less than this value
    long forceNs;       // if overrun detected, force the write cycle to take this much time
    long warmupNs;      // warmup complete when write cycle is greater than to this value
    FastThreadDumpState *mDummyDumpState;
    FastThreadDumpState *dumpState;
    bool ignoreNextOverrun;  // used to ignore initial overrun and first after an underrun
#ifdef FAST_MIXER_STATISTICS
    struct timespec oldLoad;    // previous value of clock_gettime(CLOCK_THREAD_CPUTIME_ID)
    bool oldLoadValid;  // whether oldLoad is valid
    uint32_t bounds;
    bool full;          // whether we have collected at least mSamplingN samples
#ifdef CPU_FREQUENCY_STATISTICS
    ThreadCpuUsage tcu;     // for reading the current CPU clock frequency in kHz
#endif
#endif
    unsigned coldGen;   // last observed mColdGen
    bool isWarm;        // true means ready to mix, false means wait for warmup before mixing
    struct timespec measuredWarmupTs;  // how long did it take for warmup to complete
    uint32_t warmupCycles;  // counter of number of loop cycles required to warmup
    NBLog::Writer dummyLogWriter;
    NBLog::Writer *logWriter;
    status_t timestampStatus;

    FastThreadState::Command command;
#if 0
    size_t frameCount;
#endif
    bool attemptedWrite;

};  // class FastThread

}   // android

#endif  // ANDROID_AUDIO_FAST_THREAD_H
