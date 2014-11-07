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

#ifndef ANDROID_AUDIO_FAST_THREAD_STATE_H
#define ANDROID_AUDIO_FAST_THREAD_STATE_H

#include "Configuration.h"
#include <stdint.h>
#include <media/nbaio/NBLog.h>

namespace android {

struct FastThreadDumpState;

// Represents a single state of a FastThread
struct FastThreadState {
                FastThreadState();
    /*virtual*/ ~FastThreadState();

    typedef uint32_t Command;
    static const Command
        INITIAL = 0,            // used only for the initial state
        HOT_IDLE = 1,           // do nothing
        COLD_IDLE = 2,          // wait for the futex
        IDLE = 3,               // either HOT_IDLE or COLD_IDLE
        EXIT = 4;               // exit from thread
        // additional values defined per subclass
    Command     mCommand;       // current command
    int32_t*    mColdFutexAddr; // for COLD_IDLE only, pointer to the associated futex
    unsigned    mColdGen;       // increment when COLD_IDLE is requested so it's only performed once

    // This might be a one-time configuration rather than per-state
    FastThreadDumpState* mDumpState; // if non-NULL, then update dump state periodically
    NBLog::Writer* mNBLogWriter; // non-blocking logger

};  // struct FastThreadState


// FIXME extract common part of comment at FastMixerDumpState
struct FastThreadDumpState {
    FastThreadDumpState();
    /*virtual*/ ~FastThreadDumpState();

    FastThreadState::Command mCommand;   // current command
    uint32_t mUnderruns;        // total number of underruns
    uint32_t mOverruns;         // total number of overruns
    struct timespec mMeasuredWarmupTs;  // measured warmup time
    uint32_t mWarmupCycles;     // number of loop cycles required to warmup

#ifdef FAST_MIXER_STATISTICS
    // Recently collected samples of per-cycle monotonic time, thread CPU time, and CPU frequency.
    // kSamplingN is max size of sampling frame (statistics), and must be a power of 2 <= 0x8000.
    // The sample arrays are virtually allocated based on this compile-time constant,
    // but are only initialized and used based on the runtime parameter mSamplingN.
    static const uint32_t kSamplingN = 0x8000;
    // Corresponding runtime maximum size of sample arrays, must be a power of 2 <= kSamplingN.
    uint32_t mSamplingN;
    // The bounds define the interval of valid samples, and are represented as follows:
    //      newest open (excluded) endpoint   = lower 16 bits of bounds, modulo N
    //      oldest closed (included) endpoint = upper 16 bits of bounds, modulo N
    // Number of valid samples is newest - oldest.
    uint32_t mBounds;                   // bounds for mMonotonicNs, mThreadCpuNs, and mCpukHz
    // The elements in the *Ns arrays are in units of nanoseconds <= 3999999999.
    uint32_t mMonotonicNs[kSamplingN];  // delta monotonic (wall clock) time
    uint32_t mLoadNs[kSamplingN];       // delta CPU load in time
#ifdef CPU_FREQUENCY_STATISTICS
    uint32_t mCpukHz[kSamplingN];       // absolute CPU clock frequency in kHz, bits 0-3 are CPU#
#endif
#endif

};  // struct FastThreadDumpState

}   // android

#endif  // ANDROID_AUDIO_FAST_THREAD_STATE_H
