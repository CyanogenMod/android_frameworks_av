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

#ifndef ANDROID_AUDIO_FAST_MIXER_DUMP_STATE_H
#define ANDROID_AUDIO_FAST_MIXER_DUMP_STATE_H

#include "Configuration.h"

namespace android {

// Describes the underrun status for a single "pull" attempt
enum FastTrackUnderrunStatus {
    UNDERRUN_FULL,      // framesReady() is full frame count, no underrun
    UNDERRUN_PARTIAL,   // framesReady() is non-zero but < full frame count, partial underrun
    UNDERRUN_EMPTY,     // framesReady() is zero, total underrun
};

// Underrun counters are not reset to zero for new tracks or if track generation changes.
// This packed representation is used to keep the information atomic.
union FastTrackUnderruns {
    FastTrackUnderruns() { mAtomic = 0;
            COMPILE_TIME_ASSERT_FUNCTION_SCOPE(sizeof(FastTrackUnderruns) == sizeof(uint32_t)); }
    FastTrackUnderruns(const FastTrackUnderruns& copyFrom) : mAtomic(copyFrom.mAtomic) { }
    FastTrackUnderruns& operator=(const FastTrackUnderruns& rhs)
            { if (this != &rhs) mAtomic = rhs.mAtomic; return *this; }
    struct {
#define UNDERRUN_BITS 10
#define UNDERRUN_MASK ((1 << UNDERRUN_BITS) - 1)
        uint32_t mFull    : UNDERRUN_BITS; // framesReady() is full frame count
        uint32_t mPartial : UNDERRUN_BITS; // framesReady() is non-zero but < full frame count
        uint32_t mEmpty   : UNDERRUN_BITS; // framesReady() is zero
        FastTrackUnderrunStatus mMostRecent : 2;    // status of most recent framesReady()
    }        mBitFields;
private:
    uint32_t mAtomic;
};

// Represents the dump state of a fast track
struct FastTrackDump {
    FastTrackDump() : mFramesReady(0) { }
    /*virtual*/ ~FastTrackDump() { }
    FastTrackUnderruns mUnderruns;
    size_t mFramesReady;        // most recent value only; no long-term statistics kept
};

// The FastMixerDumpState keeps a cache of FastMixer statistics that can be logged by dumpsys.
// Each individual native word-sized field is accessed atomically.  But the
// overall structure is non-atomic, that is there may be an inconsistency between fields.
// No barriers or locks are used for either writing or reading.
// Only POD types are permitted, and the contents shouldn't be trusted (i.e. do range checks).
// It has a different lifetime than the FastMixer, and so it can't be a member of FastMixer.
struct FastMixerDumpState : FastThreadDumpState {
    FastMixerDumpState(
#ifdef FAST_MIXER_STATISTICS
            uint32_t samplingN = kSamplingNforLowRamDevice
#endif
            );
    /*virtual*/ ~FastMixerDumpState();

    void dump(int fd) const;    // should only be called on a stable copy, not the original

    uint32_t mWriteSequence;    // incremented before and after each write()
    uint32_t mFramesWritten;    // total number of frames written successfully
    uint32_t mNumTracks;        // total number of active fast tracks
    uint32_t mWriteErrors;      // total number of write() errors
    uint32_t mSampleRate;
    size_t   mFrameCount;
    uint32_t mTrackMask;        // mask of active tracks
    FastTrackDump   mTracks[FastMixerState::kMaxFastTracks];

#ifdef FAST_MIXER_STATISTICS
    // Compile-time constant for a "low RAM device", must be a power of 2 <= kSamplingN.
    // This value was chosen such that each array uses 1 small page (4 Kbytes).
    static const uint32_t kSamplingNforLowRamDevice = 0x400;
    // Increase sampling window after construction, must be a power of 2 <= kSamplingN
    void    increaseSamplingN(uint32_t samplingN);
#endif
};

}   // android

#endif  // ANDROID_AUDIO_FAST_MIXER_DUMP_STATE_H
