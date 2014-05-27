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

#ifndef ANDROID_AUDIO_FAST_MIXER_H
#define ANDROID_AUDIO_FAST_MIXER_H

#include <linux/futex.h>
#include <sys/syscall.h>
#include <utils/Debug.h>
#include "FastThread.h"
#include <utils/Thread.h>
#include "StateQueue.h"
#include "FastMixerState.h"
#include "FastMixerDumpState.h"

namespace android {

class AudioMixer;

typedef StateQueue<FastMixerState> FastMixerStateQueue;

class FastMixer : public FastThread {

public:
            FastMixer();
    virtual ~FastMixer();

            FastMixerStateQueue* sq();

private:
            FastMixerStateQueue mSQ;

    // callouts
    virtual const FastThreadState *poll();
    virtual void setLog(NBLog::Writer *logWriter);
    virtual void onIdle();
    virtual void onExit();
    virtual bool isSubClassCommand(FastThreadState::Command command);
    virtual void onStateChange();
    virtual void onWork();

    // FIXME these former local variables need comments and to be renamed to have "m" prefix
    static const FastMixerState initial;
    FastMixerState preIdle; // copy of state before we went into idle
    long slopNs;        // accumulated time we've woken up too early (> 0) or too late (< 0)
    int fastTrackNames[FastMixerState::kMaxFastTracks]; // handles used by mixer to identify tracks
    int generations[FastMixerState::kMaxFastTracks];    // last observed mFastTracks[i].mGeneration
    NBAIO_Sink *outputSink;
    int outputSinkGen;
    AudioMixer* mixer;
    short *mMixerBuffer;
    enum {UNDEFINED, MIXED, ZEROED} mMixerBufferState;
    NBAIO_Format format;
    unsigned sampleRate;
    int fastTracksGen;
    FastMixerDumpState dummyDumpState;
    uint32_t totalNativeFramesWritten;  // copied to dumpState->mFramesWritten

    // next 2 fields are valid only when timestampStatus == NO_ERROR
    AudioTimestamp timestamp;
    uint32_t nativeFramesWrittenButNotPresented;

};  // class FastMixer

}   // namespace android

#endif  // ANDROID_AUDIO_FAST_MIXER_H
