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

#define LOG_TAG "FastMixer"
//#define LOG_NDEBUG 0

//#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <sys/atomics.h>
#include <time.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <system/audio.h>
#ifdef FAST_MIXER_STATISTICS
#include <cpustats/CentralTendencyStatistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif
#include "AudioMixer.h"
#include "FastMixer.h"

#define FAST_HOT_IDLE_NS     1000000L   // 1 ms: time to sleep while hot idling
#define FAST_DEFAULT_NS    999999999L   // ~1 sec: default time to sleep
#define MAX_WARMUP_CYCLES         10    // maximum number of loop cycles to wait for warmup

namespace android {

// Fast mixer thread
bool FastMixer::threadLoop()
{
    static const FastMixerState initial;
    const FastMixerState *previous = &initial, *current = &initial;
    FastMixerState preIdle; // copy of state before we went into idle
    struct timespec oldTs = {0, 0};
    bool oldTsValid = false;
    long slopNs = 0;    // accumulated time we've woken up too early (> 0) or too late (< 0)
    long sleepNs = -1;  // -1: busy wait, 0: sched_yield, > 0: nanosleep
    int fastTrackNames[FastMixerState::kMaxFastTracks]; // handles used by mixer to identify tracks
    int generations[FastMixerState::kMaxFastTracks];    // last observed mFastTracks[i].mGeneration
    unsigned i;
    for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
        fastTrackNames[i] = -1;
        generations[i] = 0;
    }
    NBAIO_Sink *outputSink = NULL;
    int outputSinkGen = 0;
    AudioMixer* mixer = NULL;
    short *mixBuffer = NULL;
    enum {UNDEFINED, MIXED, ZEROED} mixBufferState = UNDEFINED;
    NBAIO_Format format = Format_Invalid;
    unsigned sampleRate = 0;
    int fastTracksGen = 0;
    long periodNs = 0;      // expected period; the time required to render one mix buffer
    long underrunNs = 0;    // underrun likely when write cycle is greater than this value
    long overrunNs = 0;     // overrun likely when write cycle is less than this value
    long warmupNs = 0;      // warmup complete when write cycle is greater than to this value
    FastMixerDumpState dummyDumpState, *dumpState = &dummyDumpState;
    bool ignoreNextOverrun = true;  // used to ignore initial overrun and first after an underrun
#ifdef FAST_MIXER_STATISTICS
    struct timespec oldLoad = {0, 0};    // previous value of clock_gettime(CLOCK_THREAD_CPUTIME_ID)
    bool oldLoadValid = false;  // whether oldLoad is valid
    uint32_t bounds = 0;
    bool full = false;      // whether we have collected at least kSamplingN samples
    ThreadCpuUsage tcu;     // for reading the current CPU clock frequency in kHz
#endif
    unsigned coldGen = 0;   // last observed mColdGen
    bool isWarm = false;    // true means ready to mix, false means wait for warmup before mixing
    struct timespec measuredWarmupTs = {0, 0};  // how long did it take for warmup to complete
    uint32_t warmupCycles = 0;  // counter of number of loop cycles required to warmup

    for (;;) {

        // either nanosleep, sched_yield, or busy wait
        if (sleepNs >= 0) {
            if (sleepNs > 0) {
                ALOG_ASSERT(sleepNs < 1000000000);
                const struct timespec req = {0, sleepNs};
                nanosleep(&req, NULL);
            } else {
                sched_yield();
            }
        }
        // default to long sleep for next cycle
        sleepNs = FAST_DEFAULT_NS;

        // poll for state change
        const FastMixerState *next = mSQ.poll();
        if (next == NULL) {
            // continue to use the default initial state until a real state is available
            ALOG_ASSERT(current == &initial && previous == &initial);
            next = current;
        }

        FastMixerState::Command command = next->mCommand;
        if (next != current) {

            // As soon as possible of learning of a new dump area, start using it
            dumpState = next->mDumpState != NULL ? next->mDumpState : &dummyDumpState;

            // We want to always have a valid reference to the previous (non-idle) state.
            // However, the state queue only guarantees access to current and previous states.
            // So when there is a transition from a non-idle state into an idle state, we make a
            // copy of the last known non-idle state so it is still available on return from idle.
            // The possible transitions are:
            //  non-idle -> non-idle    update previous from current in-place
            //  non-idle -> idle        update previous from copy of current
            //  idle     -> idle        don't update previous
            //  idle     -> non-idle    don't update previous
            if (!(current->mCommand & FastMixerState::IDLE)) {
                if (command & FastMixerState::IDLE) {
                    preIdle = *current;
                    current = &preIdle;
                    oldTsValid = false;
                    oldLoadValid = false;
                    ignoreNextOverrun = true;
                }
                previous = current;
            }
            current = next;
        }
#if !LOG_NDEBUG
        next = NULL;    // not referenced again
#endif

        dumpState->mCommand = command;

        switch (command) {
        case FastMixerState::INITIAL:
        case FastMixerState::HOT_IDLE:
            sleepNs = FAST_HOT_IDLE_NS;
            continue;
        case FastMixerState::COLD_IDLE:
            // only perform a cold idle command once
            // FIXME consider checking previous state and only perform if previous != COLD_IDLE
            if (current->mColdGen != coldGen) {
                int32_t *coldFutexAddr = current->mColdFutexAddr;
                ALOG_ASSERT(coldFutexAddr != NULL);
                int32_t old = android_atomic_dec(coldFutexAddr);
                if (old <= 0) {
                    __futex_syscall4(coldFutexAddr, FUTEX_WAIT_PRIVATE, old - 1, NULL);
                }
                // This may be overly conservative; there could be times that the normal mixer
                // requests such a brief cold idle that it doesn't require resetting this flag.
                isWarm = false;
                measuredWarmupTs.tv_sec = 0;
                measuredWarmupTs.tv_nsec = 0;
                warmupCycles = 0;
                sleepNs = -1;
                coldGen = current->mColdGen;
                bounds = 0;
                full = false;
            } else {
                sleepNs = FAST_HOT_IDLE_NS;
            }
            continue;
        case FastMixerState::EXIT:
            delete mixer;
            delete[] mixBuffer;
            return false;
        case FastMixerState::MIX:
        case FastMixerState::WRITE:
        case FastMixerState::MIX_WRITE:
            break;
        default:
            LOG_FATAL("bad command %d", command);
        }

        // there is a non-idle state available to us; did the state change?
        size_t frameCount = current->mFrameCount;
        if (current != previous) {

            // handle state change here, but since we want to diff the state,
            // we're prepared for previous == &initial the first time through
            unsigned previousTrackMask;

            // check for change in output HAL configuration
            NBAIO_Format previousFormat = format;
            if (current->mOutputSinkGen != outputSinkGen) {
                outputSink = current->mOutputSink;
                outputSinkGen = current->mOutputSinkGen;
                if (outputSink == NULL) {
                    format = Format_Invalid;
                    sampleRate = 0;
                } else {
                    format = outputSink->format();
                    sampleRate = Format_sampleRate(format);
                    ALOG_ASSERT(Format_channelCount(format) == 2);
                }
                dumpState->mSampleRate = sampleRate;
            }

            if ((format != previousFormat) || (frameCount != previous->mFrameCount)) {
                // FIXME to avoid priority inversion, don't delete here
                delete mixer;
                mixer = NULL;
                delete[] mixBuffer;
                mixBuffer = NULL;
                if (frameCount > 0 && sampleRate > 0) {
                    // FIXME new may block for unbounded time at internal mutex of the heap
                    //       implementation; it would be better to have normal mixer allocate for us
                    //       to avoid blocking here and to prevent possible priority inversion
                    mixer = new AudioMixer(frameCount, sampleRate, FastMixerState::kMaxFastTracks);
                    mixBuffer = new short[frameCount * 2];
                    periodNs = (frameCount * 1000000000LL) / sampleRate;    // 1.00
                    underrunNs = (frameCount * 1750000000LL) / sampleRate;  // 1.75
                    overrunNs = (frameCount * 250000000LL) / sampleRate;    // 0.25
                    warmupNs = (frameCount * 500000000LL) / sampleRate;     // 0.50
                } else {
                    periodNs = 0;
                    underrunNs = 0;
                    overrunNs = 0;
                }
                mixBufferState = UNDEFINED;
#if !LOG_NDEBUG
                for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
                    fastTrackNames[i] = -1;
                }
#endif
                // we need to reconfigure all active tracks
                previousTrackMask = 0;
                fastTracksGen = current->mFastTracksGen - 1;
                dumpState->mFrameCount = frameCount;
            } else {
                previousTrackMask = previous->mTrackMask;
            }

            // check for change in active track set
            unsigned currentTrackMask = current->mTrackMask;
            if (current->mFastTracksGen != fastTracksGen) {
                ALOG_ASSERT(mixBuffer != NULL);
                int name;

                // process removed tracks first to avoid running out of track names
                unsigned removedTracks = previousTrackMask & ~currentTrackMask;
                while (removedTracks != 0) {
                    i = __builtin_ctz(removedTracks);
                    removedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    ALOG_ASSERT(fastTrack->mBufferProvider == NULL);
                    if (mixer != NULL) {
                        name = fastTrackNames[i];
                        ALOG_ASSERT(name >= 0);
                        mixer->deleteTrackName(name);
                    }
#if !LOG_NDEBUG
                    fastTrackNames[i] = -1;
#endif
                    // don't reset track dump state, since other side is ignoring it
                    generations[i] = fastTrack->mGeneration;
                }

                // now process added tracks
                unsigned addedTracks = currentTrackMask & ~previousTrackMask;
                while (addedTracks != 0) {
                    i = __builtin_ctz(addedTracks);
                    addedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                    ALOG_ASSERT(bufferProvider != NULL && fastTrackNames[i] == -1);
                    if (mixer != NULL) {
                        // calling getTrackName with default channel mask
                        name = mixer->getTrackName(AUDIO_CHANNEL_OUT_STEREO);
                        ALOG_ASSERT(name >= 0);
                        fastTrackNames[i] = name;
                        mixer->setBufferProvider(name, bufferProvider);
                        mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MAIN_BUFFER,
                                (void *) mixBuffer);
                        // newly allocated track names default to full scale volume
                        if (fastTrack->mSampleRate != 0 && fastTrack->mSampleRate != sampleRate) {
                            mixer->setParameter(name, AudioMixer::RESAMPLE,
                                    AudioMixer::SAMPLE_RATE, (void*) fastTrack->mSampleRate);
                        }
                        mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                                (void *) fastTrack->mChannelMask);
                        mixer->enable(name);
                    }
                    generations[i] = fastTrack->mGeneration;
                }

                // finally process modified tracks; these use the same slot
                // but may have a different buffer provider or volume provider
                unsigned modifiedTracks = currentTrackMask & previousTrackMask;
                while (modifiedTracks != 0) {
                    i = __builtin_ctz(modifiedTracks);
                    modifiedTracks &= ~(1 << i);
                    const FastTrack* fastTrack = &current->mFastTracks[i];
                    if (fastTrack->mGeneration != generations[i]) {
                        AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                        ALOG_ASSERT(bufferProvider != NULL);
                        if (mixer != NULL) {
                            name = fastTrackNames[i];
                            ALOG_ASSERT(name >= 0);
                            mixer->setBufferProvider(name, bufferProvider);
                            if (fastTrack->mVolumeProvider == NULL) {
                                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                                        (void *)0x1000);
                                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                                        (void *)0x1000);
                            }
                            if (fastTrack->mSampleRate != 0 &&
                                    fastTrack->mSampleRate != sampleRate) {
                                mixer->setParameter(name, AudioMixer::RESAMPLE,
                                        AudioMixer::SAMPLE_RATE, (void*) fastTrack->mSampleRate);
                            } else {
                                mixer->setParameter(name, AudioMixer::RESAMPLE,
                                        AudioMixer::REMOVE, NULL);
                            }
                            mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                                    (void *) fastTrack->mChannelMask);
                            // already enabled
                        }
                        generations[i] = fastTrack->mGeneration;
                    }
                }

                fastTracksGen = current->mFastTracksGen;

                dumpState->mNumTracks = popcount(currentTrackMask);
            }

#if 1   // FIXME shouldn't need this
            // only process state change once
            previous = current;
#endif
        }

        // do work using current state here
        if ((command & FastMixerState::MIX) && (mixer != NULL) && isWarm) {
            ALOG_ASSERT(mixBuffer != NULL);
            // for each track, update volume and check for underrun
            unsigned currentTrackMask = current->mTrackMask;
            while (currentTrackMask != 0) {
                i = __builtin_ctz(currentTrackMask);
                currentTrackMask &= ~(1 << i);
                const FastTrack* fastTrack = &current->mFastTracks[i];
                int name = fastTrackNames[i];
                ALOG_ASSERT(name >= 0);
                if (fastTrack->mVolumeProvider != NULL) {
                    uint32_t vlr = fastTrack->mVolumeProvider->getVolumeLR();
                    mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                            (void *)(vlr & 0xFFFF));
                    mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                            (void *)(vlr >> 16));
                }
                // FIXME The current implementation of framesReady() for fast tracks
                // takes a tryLock, which can block
                // up to 1 ms.  If enough active tracks all blocked in sequence, this would result
                // in the overall fast mix cycle being delayed.  Should use a non-blocking FIFO.
                size_t framesReady = fastTrack->mBufferProvider->framesReady();
                FastTrackDump *ftDump = &dumpState->mTracks[i];
                uint32_t underruns = ftDump->mUnderruns;
                if (framesReady < frameCount) {
                    ATRACE_INT("underrun", i);
                    ftDump->mUnderruns = (underruns + 2) | 1;
                    if (framesReady == 0) {
                        mixer->disable(name);
                    } else {
                        // allow mixing partial buffer
                        mixer->enable(name);
                    }
                } else if (underruns & 1) {
                    ftDump->mUnderruns = underruns & ~1;
                    mixer->enable(name);
                }
            }
            // process() is CPU-bound
            mixer->process(AudioBufferProvider::kInvalidPTS);
            mixBufferState = MIXED;
        } else if (mixBufferState == MIXED) {
            mixBufferState = UNDEFINED;
        }
        bool attemptedWrite = false;
        //bool didFullWrite = false;    // dumpsys could display a count of partial writes
        if ((command & FastMixerState::WRITE) && (outputSink != NULL) && (mixBuffer != NULL)) {
            if (mixBufferState == UNDEFINED) {
                memset(mixBuffer, 0, frameCount * 2 * sizeof(short));
                mixBufferState = ZEROED;
            }
            // FIXME write() is non-blocking and lock-free for a properly implemented NBAIO sink,
            //       but this code should be modified to handle both non-blocking and blocking sinks
            dumpState->mWriteSequence++;
            Tracer::traceBegin(ATRACE_TAG, "write");
            ssize_t framesWritten = outputSink->write(mixBuffer, frameCount);
            Tracer::traceEnd(ATRACE_TAG);
            dumpState->mWriteSequence++;
            if (framesWritten >= 0) {
                ALOG_ASSERT(framesWritten <= frameCount);
                dumpState->mFramesWritten += framesWritten;
                //if ((size_t) framesWritten == frameCount) {
                //    didFullWrite = true;
                //}
            } else {
                dumpState->mWriteErrors++;
            }
            attemptedWrite = true;
            // FIXME count # of writes blocked excessively, CPU usage, etc. for dump
        }

        // To be exactly periodic, compute the next sleep time based on current time.
        // This code doesn't have long-term stability when the sink is non-blocking.
        // FIXME To avoid drift, use the local audio clock or watch the sink's fill status.
        struct timespec newTs;
        int rc = clock_gettime(CLOCK_MONOTONIC, &newTs);
        if (rc == 0) {
            if (oldTsValid) {
                time_t sec = newTs.tv_sec - oldTs.tv_sec;
                long nsec = newTs.tv_nsec - oldTs.tv_nsec;
                if (nsec < 0) {
                    --sec;
                    nsec += 1000000000;
                }
                // To avoid an initial underrun on fast tracks after exiting standby,
                // do not start pulling data from tracks and mixing until warmup is complete.
                // Warmup is considered complete after the earlier of:
                //      first successful single write() that blocks for more than warmupNs
                //      MAX_WARMUP_CYCLES write() attempts.
                // This is overly conservative, but to get better accuracy requires a new HAL API.
                if (!isWarm && attemptedWrite) {
                    measuredWarmupTs.tv_sec += sec;
                    measuredWarmupTs.tv_nsec += nsec;
                    if (measuredWarmupTs.tv_nsec >= 1000000000) {
                        measuredWarmupTs.tv_sec++;
                        measuredWarmupTs.tv_nsec -= 1000000000;
                    }
                    ++warmupCycles;
                    if ((attemptedWrite && nsec > warmupNs) ||
                            (warmupCycles >= MAX_WARMUP_CYCLES)) {
                        isWarm = true;
                        dumpState->mMeasuredWarmupTs = measuredWarmupTs;
                        dumpState->mWarmupCycles = warmupCycles;
                    }
                }
                if (sec > 0 || nsec > underrunNs) {
                    ScopedTrace st(ATRACE_TAG, "underrun");
                    // FIXME only log occasionally
                    ALOGV("underrun: time since last cycle %d.%03ld sec",
                            (int) sec, nsec / 1000000L);
                    dumpState->mUnderruns++;
                    sleepNs = -1;
                    ignoreNextOverrun = true;
                } else if (nsec < overrunNs) {
                    if (ignoreNextOverrun) {
                        ignoreNextOverrun = false;
                    } else {
                        // FIXME only log occasionally
                        ALOGV("overrun: time since last cycle %d.%03ld sec",
                                (int) sec, nsec / 1000000L);
                        dumpState->mOverruns++;
                    }
                    sleepNs = periodNs - overrunNs;
                } else {
                    sleepNs = -1;
                    ignoreNextOverrun = false;
                }
#ifdef FAST_MIXER_STATISTICS
                // advance the FIFO queue bounds
                size_t i = bounds & (FastMixerDumpState::kSamplingN - 1);
                bounds = (bounds & 0xFFFF0000) | ((bounds + 1) & 0xFFFF);
                if (full) {
                    bounds += 0x10000;
                } else if (!(bounds & (FastMixerDumpState::kSamplingN - 1))) {
                    full = true;
                }
                // compute the delta value of clock_gettime(CLOCK_MONOTONIC)
                uint32_t monotonicNs = nsec;
                if (sec > 0 && sec < 4) {
                    monotonicNs += sec * 1000000000;
                }
                // compute the raw CPU load = delta value of clock_gettime(CLOCK_THREAD_CPUTIME_ID)
                uint32_t loadNs = 0;
                struct timespec newLoad;
                rc = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &newLoad);
                if (rc == 0) {
                    if (oldLoadValid) {
                        sec = newLoad.tv_sec - oldLoad.tv_sec;
                        nsec = newLoad.tv_nsec - oldLoad.tv_nsec;
                        if (nsec < 0) {
                            --sec;
                            nsec += 1000000000;
                        }
                        loadNs = nsec;
                        if (sec > 0 && sec < 4) {
                            loadNs += sec * 1000000000;
                        }
                    } else {
                        // first time through the loop
                        oldLoadValid = true;
                    }
                    oldLoad = newLoad;
                }
                // get the absolute value of CPU clock frequency in kHz
                int cpuNum = sched_getcpu();
                uint32_t kHz = tcu.getCpukHz(cpuNum);
                kHz = (kHz & ~0xF) | (cpuNum & 0xF);
                // save values in FIFO queues for dumpsys
                // these stores #1, #2, #3 are not atomic with respect to each other,
                // or with respect to store #4 below
                dumpState->mMonotonicNs[i] = monotonicNs;
                dumpState->mLoadNs[i] = loadNs;
                dumpState->mCpukHz[i] = kHz;
                // this store #4 is not atomic with respect to stores #1, #2, #3 above, but
                // the newest open and oldest closed halves are atomic with respect to each other
                dumpState->mBounds = bounds;
#endif
            } else {
                // first time through the loop
                oldTsValid = true;
                sleepNs = periodNs;
                ignoreNextOverrun = true;
            }
            oldTs = newTs;
        } else {
            // monotonic clock is broken
            oldTsValid = false;
            sleepNs = periodNs;
        }


    }   // for (;;)

    // never return 'true'; Thread::_threadLoop() locks mutex which can result in priority inversion
}

FastMixerDumpState::FastMixerDumpState() :
    mCommand(FastMixerState::INITIAL), mWriteSequence(0), mFramesWritten(0),
    mNumTracks(0), mWriteErrors(0), mUnderruns(0), mOverruns(0),
    mSampleRate(0), mFrameCount(0), /* mMeasuredWarmupTs({0, 0}), */ mWarmupCycles(0)
#ifdef FAST_MIXER_STATISTICS
    , mBounds(0)
#endif
{
    mMeasuredWarmupTs.tv_sec = 0;
    mMeasuredWarmupTs.tv_nsec = 0;
    // sample arrays aren't accessed atomically with respect to the bounds,
    // so clearing reduces chance for dumpsys to read random uninitialized samples
    memset(&mMonotonicNs, 0, sizeof(mMonotonicNs));
    memset(&mLoadNs, 0, sizeof(mLoadNs));
    memset(&mCpukHz, 0, sizeof(mCpukHz));
}

FastMixerDumpState::~FastMixerDumpState()
{
}

void FastMixerDumpState::dump(int fd)
{
#define COMMAND_MAX 32
    char string[COMMAND_MAX];
    switch (mCommand) {
    case FastMixerState::INITIAL:
        strcpy(string, "INITIAL");
        break;
    case FastMixerState::HOT_IDLE:
        strcpy(string, "HOT_IDLE");
        break;
    case FastMixerState::COLD_IDLE:
        strcpy(string, "COLD_IDLE");
        break;
    case FastMixerState::EXIT:
        strcpy(string, "EXIT");
        break;
    case FastMixerState::MIX:
        strcpy(string, "MIX");
        break;
    case FastMixerState::WRITE:
        strcpy(string, "WRITE");
        break;
    case FastMixerState::MIX_WRITE:
        strcpy(string, "MIX_WRITE");
        break;
    default:
        snprintf(string, COMMAND_MAX, "%d", mCommand);
        break;
    }
    double measuredWarmupMs = (mMeasuredWarmupTs.tv_sec * 1000.0) +
            (mMeasuredWarmupTs.tv_nsec / 1000000.0);
    double mixPeriodSec = (double) mFrameCount / (double) mSampleRate;
    fdprintf(fd, "FastMixer command=%s writeSequence=%u framesWritten=%u\n"
                 "          numTracks=%u writeErrors=%u underruns=%u overruns=%u\n"
                 "          sampleRate=%u frameCount=%u measuredWarmup=%.3g ms, warmupCycles=%u\n"
                 "          mixPeriod=%.2f ms\n",
                 string, mWriteSequence, mFramesWritten,
                 mNumTracks, mWriteErrors, mUnderruns, mOverruns,
                 mSampleRate, mFrameCount, measuredWarmupMs, mWarmupCycles,
                 mixPeriodSec * 1e3);
#ifdef FAST_MIXER_STATISTICS
    // find the interval of valid samples
    uint32_t bounds = mBounds;
    uint32_t newestOpen = bounds & 0xFFFF;
    uint32_t oldestClosed = bounds >> 16;
    uint32_t n = (newestOpen - oldestClosed) & 0xFFFF;
    if (n > kSamplingN) {
        ALOGE("too many samples %u", n);
        n = kSamplingN;
    }
    // statistics for monotonic (wall clock) time, thread raw CPU load in time, CPU clock frequency,
    // and adjusted CPU load in MHz normalized for CPU clock frequency
    CentralTendencyStatistics wall, loadNs, kHz, loadMHz;
    // only compute adjusted CPU load in Hz if current CPU number and CPU clock frequency are stable
    bool valid = false;
    uint32_t previousCpukHz = 0;
    // loop over all the samples
    for (; n > 0; --n) {
        size_t i = oldestClosed++ & (kSamplingN - 1);
        uint32_t wallNs = mMonotonicNs[i];
        wall.sample(wallNs);
        uint32_t sampleLoadNs = mLoadNs[i];
        uint32_t sampleCpukHz = mCpukHz[i];
        loadNs.sample(sampleLoadNs);
        kHz.sample(sampleCpukHz & ~0xF);
        if (sampleCpukHz == previousCpukHz) {
            double megacycles = (double) sampleLoadNs * (double) sampleCpukHz * 1e-12;
            double adjMHz = megacycles / mixPeriodSec;  // _not_ wallNs * 1e9
            loadMHz.sample(adjMHz);
        }
        previousCpukHz = sampleCpukHz;
    }
    fdprintf(fd, "Simple moving statistics over last %.1f seconds:\n", wall.n() * mixPeriodSec);
    fdprintf(fd, "  wall clock time in ms per mix cycle:\n"
                 "    mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                 wall.mean()*1e-6, wall.minimum()*1e-6, wall.maximum()*1e-6, wall.stddev()*1e-6);
    fdprintf(fd, "  raw CPU load in us per mix cycle:\n"
                 "    mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                 loadNs.mean()*1e-3, loadNs.minimum()*1e-3, loadNs.maximum()*1e-3,
                 loadNs.stddev()*1e-3);
    fdprintf(fd, "  CPU clock frequency in MHz:\n"
                 "    mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                 kHz.mean()*1e-3, kHz.minimum()*1e-3, kHz.maximum()*1e-3, kHz.stddev()*1e-3);
    fdprintf(fd, "  adjusted CPU load in MHz (i.e. normalized for CPU clock frequency):\n"
                 "    mean=%.1f min=%.1f max=%.1f stddev=%.1f\n",
                 loadMHz.mean(), loadMHz.minimum(), loadMHz.maximum(), loadMHz.stddev());
#endif
}

}   // namespace android
