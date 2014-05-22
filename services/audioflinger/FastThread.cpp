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

#define LOG_TAG "FastThread"
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_AUDIO

#include "Configuration.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include "FastThread.h"

#define FAST_DEFAULT_NS    999999999L   // ~1 sec: default time to sleep
#define FAST_HOT_IDLE_NS     1000000L   // 1 ms: time to sleep while hot idling
#define MIN_WARMUP_CYCLES          2    // minimum number of loop cycles to wait for warmup
#define MAX_WARMUP_CYCLES         10    // maximum number of loop cycles to wait for warmup

namespace android {

FastThread::FastThread() : Thread(false /*canCallJava*/),
    // re-initialized to &initial by subclass constructor
     previous(NULL), current(NULL),
    /* oldTs({0, 0}), */
    oldTsValid(false),
    sleepNs(-1),
    periodNs(0),
    underrunNs(0),
    overrunNs(0),
    forceNs(0),
    warmupNs(0),
    // re-initialized to &dummyDumpState by subclass constructor
    mDummyDumpState(NULL),
    dumpState(NULL),
    ignoreNextOverrun(true),
#ifdef FAST_MIXER_STATISTICS
    // oldLoad
    oldLoadValid(false),
    bounds(0),
    full(false),
    // tcu
#endif
    coldGen(0),
    isWarm(false),
    /* measuredWarmupTs({0, 0}), */
    warmupCycles(0),
    // dummyLogWriter
    logWriter(&dummyLogWriter),
    timestampStatus(INVALID_OPERATION),

    command(FastThreadState::INITIAL),
#if 0
    frameCount(0),
#endif
    attemptedWrite(false)
{
    oldTs.tv_sec = 0;
    oldTs.tv_nsec = 0;
    measuredWarmupTs.tv_sec = 0;
    measuredWarmupTs.tv_nsec = 0;
}

FastThread::~FastThread()
{
}

bool FastThread::threadLoop()
{
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
        const FastThreadState *next = poll();
        if (next == NULL) {
            // continue to use the default initial state until a real state is available
            // FIXME &initial not available, should save address earlier
            //ALOG_ASSERT(current == &initial && previous == &initial);
            next = current;
        }

        command = next->mCommand;
        if (next != current) {

            // As soon as possible of learning of a new dump area, start using it
            dumpState = next->mDumpState != NULL ? next->mDumpState : mDummyDumpState;
            logWriter = next->mNBLogWriter != NULL ? next->mNBLogWriter : &dummyLogWriter;
            setLog(logWriter);

            // We want to always have a valid reference to the previous (non-idle) state.
            // However, the state queue only guarantees access to current and previous states.
            // So when there is a transition from a non-idle state into an idle state, we make a
            // copy of the last known non-idle state so it is still available on return from idle.
            // The possible transitions are:
            //  non-idle -> non-idle    update previous from current in-place
            //  non-idle -> idle        update previous from copy of current
            //  idle     -> idle        don't update previous
            //  idle     -> non-idle    don't update previous
            if (!(current->mCommand & FastThreadState::IDLE)) {
                if (command & FastThreadState::IDLE) {
                    onIdle();
                    oldTsValid = false;
#ifdef FAST_MIXER_STATISTICS
                    oldLoadValid = false;
#endif
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

        // << current, previous, command, dumpState >>

        switch (command) {
        case FastThreadState::INITIAL:
        case FastThreadState::HOT_IDLE:
            sleepNs = FAST_HOT_IDLE_NS;
            continue;
        case FastThreadState::COLD_IDLE:
            // only perform a cold idle command once
            // FIXME consider checking previous state and only perform if previous != COLD_IDLE
            if (current->mColdGen != coldGen) {
                int32_t *coldFutexAddr = current->mColdFutexAddr;
                ALOG_ASSERT(coldFutexAddr != NULL);
                int32_t old = android_atomic_dec(coldFutexAddr);
                if (old <= 0) {
                    syscall(__NR_futex, coldFutexAddr, FUTEX_WAIT_PRIVATE, old - 1, NULL);
                }
                int policy = sched_getscheduler(0);
                if (!(policy == SCHED_FIFO || policy == SCHED_RR)) {
                    ALOGE("did not receive expected priority boost");
                }
                // This may be overly conservative; there could be times that the normal mixer
                // requests such a brief cold idle that it doesn't require resetting this flag.
                isWarm = false;
                measuredWarmupTs.tv_sec = 0;
                measuredWarmupTs.tv_nsec = 0;
                warmupCycles = 0;
                sleepNs = -1;
                coldGen = current->mColdGen;
#ifdef FAST_MIXER_STATISTICS
                bounds = 0;
                full = false;
#endif
                oldTsValid = !clock_gettime(CLOCK_MONOTONIC, &oldTs);
                timestampStatus = INVALID_OPERATION;
            } else {
                sleepNs = FAST_HOT_IDLE_NS;
            }
            continue;
        case FastThreadState::EXIT:
            onExit();
            return false;
        default:
            LOG_ALWAYS_FATAL_IF(!isSubClassCommand(command));
            break;
        }

        // there is a non-idle state available to us; did the state change?
        if (current != previous) {
            onStateChange();
#if 1   // FIXME shouldn't need this
            // only process state change once
            previous = current;
#endif
        }

        // do work using current state here
        attemptedWrite = false;
        onWork();

        // To be exactly periodic, compute the next sleep time based on current time.
        // This code doesn't have long-term stability when the sink is non-blocking.
        // FIXME To avoid drift, use the local audio clock or watch the sink's fill status.
        struct timespec newTs;
        int rc = clock_gettime(CLOCK_MONOTONIC, &newTs);
        if (rc == 0) {
            //logWriter->logTimestamp(newTs);
            if (oldTsValid) {
                time_t sec = newTs.tv_sec - oldTs.tv_sec;
                long nsec = newTs.tv_nsec - oldTs.tv_nsec;
                ALOGE_IF(sec < 0 || (sec == 0 && nsec < 0),
                        "clock_gettime(CLOCK_MONOTONIC) failed: was %ld.%09ld but now %ld.%09ld",
                        oldTs.tv_sec, oldTs.tv_nsec, newTs.tv_sec, newTs.tv_nsec);
                if (nsec < 0) {
                    --sec;
                    nsec += 1000000000;
                }
                // To avoid an initial underrun on fast tracks after exiting standby,
                // do not start pulling data from tracks and mixing until warmup is complete.
                // Warmup is considered complete after the earlier of:
                //      MIN_WARMUP_CYCLES write() attempts and last one blocks for at least warmupNs
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
                    if ((nsec > warmupNs && warmupCycles >= MIN_WARMUP_CYCLES) ||
                            (warmupCycles >= MAX_WARMUP_CYCLES)) {
                        isWarm = true;
                        dumpState->mMeasuredWarmupTs = measuredWarmupTs;
                        dumpState->mWarmupCycles = warmupCycles;
                    }
                }
                sleepNs = -1;
                if (isWarm) {
                    if (sec > 0 || nsec > underrunNs) {
                        ATRACE_NAME("underrun");
                        // FIXME only log occasionally
                        ALOGV("underrun: time since last cycle %d.%03ld sec",
                                (int) sec, nsec / 1000000L);
                        dumpState->mUnderruns++;
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
                        // This forces a minimum cycle time. It:
                        //  - compensates for an audio HAL with jitter due to sample rate conversion
                        //  - works with a variable buffer depth audio HAL that never pulls at a
                        //    rate < than overrunNs per buffer.
                        //  - recovers from overrun immediately after underrun
                        // It doesn't work with a non-blocking audio HAL.
                        sleepNs = forceNs - nsec;
                    } else {
                        ignoreNextOverrun = false;
                    }
                }
#ifdef FAST_MIXER_STATISTICS
                if (isWarm) {
                    // advance the FIFO queue bounds
                    size_t i = bounds & (dumpState->mSamplingN - 1);
                    bounds = (bounds & 0xFFFF0000) | ((bounds + 1) & 0xFFFF);
                    if (full) {
                        bounds += 0x10000;
                    } else if (!(bounds & (dumpState->mSamplingN - 1))) {
                        full = true;
                    }
                    // compute the delta value of clock_gettime(CLOCK_MONOTONIC)
                    uint32_t monotonicNs = nsec;
                    if (sec > 0 && sec < 4) {
                        monotonicNs += sec * 1000000000;
                    }
                    // compute raw CPU load = delta value of clock_gettime(CLOCK_THREAD_CPUTIME_ID)
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
#ifdef CPU_FREQUENCY_STATISTICS
                    // get the absolute value of CPU clock frequency in kHz
                    int cpuNum = sched_getcpu();
                    uint32_t kHz = tcu.getCpukHz(cpuNum);
                    kHz = (kHz << 4) | (cpuNum & 0xF);
#endif
                    // save values in FIFO queues for dumpsys
                    // these stores #1, #2, #3 are not atomic with respect to each other,
                    // or with respect to store #4 below
                    dumpState->mMonotonicNs[i] = monotonicNs;
                    dumpState->mLoadNs[i] = loadNs;
#ifdef CPU_FREQUENCY_STATISTICS
                    dumpState->mCpukHz[i] = kHz;
#endif
                    // this store #4 is not atomic with respect to stores #1, #2, #3 above, but
                    // the newest open & oldest closed halves are atomic with respect to each other
                    dumpState->mBounds = bounds;
                    ATRACE_INT("cycle_ms", monotonicNs / 1000000);
                    ATRACE_INT("load_us", loadNs / 1000);
                }
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

}   // namespace android
