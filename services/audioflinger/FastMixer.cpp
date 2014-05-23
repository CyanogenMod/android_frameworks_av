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

// <IMPORTANT_WARNING>
// Design rules for threadLoop() are given in the comments at section "Fast mixer thread" of
// StateQueue.h.  In particular, avoid library and system calls except at well-known points.
// The design rules are only for threadLoop(), and don't apply to FastMixerDumpState methods.
// </IMPORTANT_WARNING>

#define LOG_TAG "FastMixer"
//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_AUDIO

#include "Configuration.h"
#include <time.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <system/audio.h>
#ifdef FAST_MIXER_STATISTICS
#include <cpustats/CentralTendencyStatistics.h>
#ifdef CPU_FREQUENCY_STATISTICS
#include <cpustats/ThreadCpuUsage.h>
#endif
#endif
#include "AudioMixer.h"
#include "FastMixer.h"

#define FCC_2                       2   // fixed channel count assumption

namespace android {

/*static*/ const FastMixerState FastMixer::initial;

FastMixer::FastMixer() : FastThread(),
    slopNs(0),
    // fastTrackNames
    // generations
    outputSink(NULL),
    outputSinkGen(0),
    mixer(NULL),
    mixBuffer(NULL),
    mixBufferState(UNDEFINED),
    format(Format_Invalid),
    sampleRate(0),
    fastTracksGen(0),
    totalNativeFramesWritten(0),
    // timestamp
    nativeFramesWrittenButNotPresented(0)   // the = 0 is to silence the compiler
{
    // FIXME pass initial as parameter to base class constructor, and make it static local
    previous = &initial;
    current = &initial;

    mDummyDumpState = &dummyDumpState;

    unsigned i;
    for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
        fastTrackNames[i] = -1;
        generations[i] = 0;
    }
#ifdef FAST_MIXER_STATISTICS
    oldLoad.tv_sec = 0;
    oldLoad.tv_nsec = 0;
#endif
}

FastMixer::~FastMixer()
{
}

FastMixerStateQueue* FastMixer::sq()
{
    return &mSQ;
}

const FastThreadState *FastMixer::poll()
{
    return mSQ.poll();
}

void FastMixer::setLog(NBLog::Writer *logWriter)
{
    if (mixer != NULL) {
        mixer->setLog(logWriter);
    }
}

void FastMixer::onIdle()
{
    preIdle = *(const FastMixerState *)current;
    current = &preIdle;
}

void FastMixer::onExit()
{
    delete mixer;
    delete[] mixBuffer;
}

bool FastMixer::isSubClassCommand(FastThreadState::Command command)
{
    switch ((FastMixerState::Command) command) {
    case FastMixerState::MIX:
    case FastMixerState::WRITE:
    case FastMixerState::MIX_WRITE:
        return true;
    default:
        return false;
    }
}

void FastMixer::onStateChange()
{
    const FastMixerState * const current = (const FastMixerState *) this->current;
    const FastMixerState * const previous = (const FastMixerState *) this->previous;
    FastMixerDumpState * const dumpState = (FastMixerDumpState *) this->dumpState;
    const size_t frameCount = current->mFrameCount;

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
            ALOG_ASSERT(Format_channelCount(format) == FCC_2);
        }
        dumpState->mSampleRate = sampleRate;
    }

    if ((!Format_isEqual(format, previousFormat)) || (frameCount != previous->mFrameCount)) {
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
            mixBuffer = new short[frameCount * FCC_2];
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
        mixBufferState = UNDEFINED;
#if !LOG_NDEBUG
        for (unsigned i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
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
    const unsigned currentTrackMask = current->mTrackMask;
    dumpState->mTrackMask = currentTrackMask;
    if (current->mFastTracksGen != fastTracksGen) {
        ALOG_ASSERT(mixBuffer != NULL);
        int name;

        // process removed tracks first to avoid running out of track names
        unsigned removedTracks = previousTrackMask & ~currentTrackMask;
        while (removedTracks != 0) {
            int i = __builtin_ctz(removedTracks);
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
            int i = __builtin_ctz(addedTracks);
            addedTracks &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];
            AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
            ALOG_ASSERT(bufferProvider != NULL && fastTrackNames[i] == -1);
            if (mixer != NULL) {
                name = mixer->getTrackName(fastTrack->mChannelMask, AUDIO_SESSION_OUTPUT_MIX);
                ALOG_ASSERT(name >= 0);
                fastTrackNames[i] = name;
                mixer->setBufferProvider(name, bufferProvider);
                mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MAIN_BUFFER,
                        (void *) mixBuffer);
                // newly allocated track names default to full scale volume
                mixer->enable(name);
            }
            generations[i] = fastTrack->mGeneration;
        }

        // finally process (potentially) modified tracks; these use the same slot
        // but may have a different buffer provider or volume provider
        unsigned modifiedTracks = currentTrackMask & previousTrackMask;
        while (modifiedTracks != 0) {
            int i = __builtin_ctz(modifiedTracks);
            modifiedTracks &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];
            if (fastTrack->mGeneration != generations[i]) {
                // this track was actually modified
                AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                ALOG_ASSERT(bufferProvider != NULL);
                if (mixer != NULL) {
                    name = fastTrackNames[i];
                    ALOG_ASSERT(name >= 0);
                    mixer->setBufferProvider(name, bufferProvider);
                    if (fastTrack->mVolumeProvider == NULL) {
                        mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                                (void *) MAX_GAIN_INT);
                        mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                                (void *) MAX_GAIN_INT);
                    }
                    mixer->setParameter(name, AudioMixer::RESAMPLE,
                            AudioMixer::REMOVE, NULL);
                    mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                            (void *)(uintptr_t) fastTrack->mChannelMask);
                    // already enabled
                }
                generations[i] = fastTrack->mGeneration;
            }
        }

        fastTracksGen = current->mFastTracksGen;

        dumpState->mNumTracks = popcount(currentTrackMask);
    }
}

void FastMixer::onWork()
{
    const FastMixerState * const current = (const FastMixerState *) this->current;
    FastMixerDumpState * const dumpState = (FastMixerDumpState *) this->dumpState;
    const FastMixerState::Command command = this->command;
    const size_t frameCount = current->mFrameCount;

    if ((command & FastMixerState::MIX) && (mixer != NULL) && isWarm) {
        ALOG_ASSERT(mixBuffer != NULL);
        // for each track, update volume and check for underrun
        unsigned currentTrackMask = current->mTrackMask;
        while (currentTrackMask != 0) {
            int i = __builtin_ctz(currentTrackMask);
            currentTrackMask &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];

            // Refresh the per-track timestamp
            if (timestampStatus == NO_ERROR) {
                uint32_t trackFramesWrittenButNotPresented =
                    nativeFramesWrittenButNotPresented;
                uint32_t trackFramesWritten = fastTrack->mBufferProvider->framesReleased();
                // Can't provide an AudioTimestamp before first frame presented,
                // or during the brief 32-bit wraparound window
                if (trackFramesWritten >= trackFramesWrittenButNotPresented) {
                    AudioTimestamp perTrackTimestamp;
                    perTrackTimestamp.mPosition =
                            trackFramesWritten - trackFramesWrittenButNotPresented;
                    perTrackTimestamp.mTime = timestamp.mTime;
                    fastTrack->mBufferProvider->onTimestamp(perTrackTimestamp);
                }
            }

            int name = fastTrackNames[i];
            ALOG_ASSERT(name >= 0);
            if (fastTrack->mVolumeProvider != NULL) {
                gain_minifloat_packed_t vlr = fastTrack->mVolumeProvider->getVolumeLR();
                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0,
                        (void *) (uintptr_t)
                            (float_from_gain(gain_minifloat_unpack_left(vlr)) * MAX_GAIN_INT));
                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1,
                        (void *) (uintptr_t)
                            (float_from_gain(gain_minifloat_unpack_right(vlr)) * MAX_GAIN_INT));
            }
            // FIXME The current implementation of framesReady() for fast tracks
            // takes a tryLock, which can block
            // up to 1 ms.  If enough active tracks all blocked in sequence, this would result
            // in the overall fast mix cycle being delayed.  Should use a non-blocking FIFO.
            size_t framesReady = fastTrack->mBufferProvider->framesReady();
            if (ATRACE_ENABLED()) {
                // I wish we had formatted trace names
                char traceName[16];
                strcpy(traceName, "fRdy");
                traceName[4] = i + (i < 10 ? '0' : 'A' - 10);
                traceName[5] = '\0';
                ATRACE_INT(traceName, framesReady);
            }
            FastTrackDump *ftDump = &dumpState->mTracks[i];
            FastTrackUnderruns underruns = ftDump->mUnderruns;
            if (framesReady < frameCount) {
                if (framesReady == 0) {
                    underruns.mBitFields.mEmpty++;
                    underruns.mBitFields.mMostRecent = UNDERRUN_EMPTY;
                    mixer->disable(name);
                } else {
                    // allow mixing partial buffer
                    underruns.mBitFields.mPartial++;
                    underruns.mBitFields.mMostRecent = UNDERRUN_PARTIAL;
                    mixer->enable(name);
                }
            } else {
                underruns.mBitFields.mFull++;
                underruns.mBitFields.mMostRecent = UNDERRUN_FULL;
                mixer->enable(name);
            }
            ftDump->mUnderruns = underruns;
            ftDump->mFramesReady = framesReady;
        }

        int64_t pts;
        if (outputSink == NULL || (OK != outputSink->getNextWriteTimestamp(&pts))) {
            pts = AudioBufferProvider::kInvalidPTS;
        }

        // process() is CPU-bound
        mixer->process(pts);
        mixBufferState = MIXED;
    } else if (mixBufferState == MIXED) {
        mixBufferState = UNDEFINED;
    }
    //bool didFullWrite = false;    // dumpsys could display a count of partial writes
    if ((command & FastMixerState::WRITE) && (outputSink != NULL) && (mixBuffer != NULL)) {
        if (mixBufferState == UNDEFINED) {
            memset(mixBuffer, 0, frameCount * FCC_2 * sizeof(short));
            mixBufferState = ZEROED;
        }
        // if non-NULL, then duplicate write() to this non-blocking sink
        NBAIO_Sink* teeSink;
        if ((teeSink = current->mTeeSink) != NULL) {
            (void) teeSink->write(mixBuffer, frameCount);
        }
        // FIXME write() is non-blocking and lock-free for a properly implemented NBAIO sink,
        //       but this code should be modified to handle both non-blocking and blocking sinks
        dumpState->mWriteSequence++;
        ATRACE_BEGIN("write");
        ssize_t framesWritten = outputSink->write(mixBuffer, frameCount);
        ATRACE_END();
        dumpState->mWriteSequence++;
        if (framesWritten >= 0) {
            ALOG_ASSERT((size_t) framesWritten <= frameCount);
            totalNativeFramesWritten += framesWritten;
            dumpState->mFramesWritten = totalNativeFramesWritten;
            //if ((size_t) framesWritten == frameCount) {
            //    didFullWrite = true;
            //}
        } else {
            dumpState->mWriteErrors++;
        }
        attemptedWrite = true;
        // FIXME count # of writes blocked excessively, CPU usage, etc. for dump

        timestampStatus = outputSink->getTimestamp(timestamp);
        if (timestampStatus == NO_ERROR) {
            uint32_t totalNativeFramesPresented = timestamp.mPosition;
            if (totalNativeFramesPresented <= totalNativeFramesWritten) {
                nativeFramesWrittenButNotPresented =
                    totalNativeFramesWritten - totalNativeFramesPresented;
            } else {
                // HAL reported that more frames were presented than were written
                timestampStatus = INVALID_OPERATION;
            }
        }
    }
}

FastMixerDumpState::FastMixerDumpState(
#ifdef FAST_MIXER_STATISTICS
        uint32_t samplingN
#endif
        ) : FastThreadDumpState(),
    mWriteSequence(0), mFramesWritten(0),
    mNumTracks(0), mWriteErrors(0),
    mSampleRate(0), mFrameCount(0),
    mTrackMask(0)
{
#ifdef FAST_MIXER_STATISTICS
    increaseSamplingN(samplingN);
#endif
}

#ifdef FAST_MIXER_STATISTICS
void FastMixerDumpState::increaseSamplingN(uint32_t samplingN)
{
    if (samplingN <= mSamplingN || samplingN > kSamplingN || roundup(samplingN) != samplingN) {
        return;
    }
    uint32_t additional = samplingN - mSamplingN;
    // sample arrays aren't accessed atomically with respect to the bounds,
    // so clearing reduces chance for dumpsys to read random uninitialized samples
    memset(&mMonotonicNs[mSamplingN], 0, sizeof(mMonotonicNs[0]) * additional);
    memset(&mLoadNs[mSamplingN], 0, sizeof(mLoadNs[0]) * additional);
#ifdef CPU_FREQUENCY_STATISTICS
    memset(&mCpukHz[mSamplingN], 0, sizeof(mCpukHz[0]) * additional);
#endif
    mSamplingN = samplingN;
}
#endif

FastMixerDumpState::~FastMixerDumpState()
{
}

// helper function called by qsort()
static int compare_uint32_t(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    } else {
        return 0;
    }
}

void FastMixerDumpState::dump(int fd) const
{
    if (mCommand == FastMixerState::INITIAL) {
        dprintf(fd, "  FastMixer not initialized\n");
        return;
    }
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
    dprintf(fd, "  FastMixer command=%s writeSequence=%u framesWritten=%u\n"
                "            numTracks=%u writeErrors=%u underruns=%u overruns=%u\n"
                "            sampleRate=%u frameCount=%zu measuredWarmup=%.3g ms, warmupCycles=%u\n"
                "            mixPeriod=%.2f ms\n",
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
    if (n > mSamplingN) {
        ALOGE("too many samples %u", n);
        n = mSamplingN;
    }
    // statistics for monotonic (wall clock) time, thread raw CPU load in time, CPU clock frequency,
    // and adjusted CPU load in MHz normalized for CPU clock frequency
    CentralTendencyStatistics wall, loadNs;
#ifdef CPU_FREQUENCY_STATISTICS
    CentralTendencyStatistics kHz, loadMHz;
    uint32_t previousCpukHz = 0;
#endif
    // Assuming a normal distribution for cycle times, three standard deviations on either side of
    // the mean account for 99.73% of the population.  So if we take each tail to be 1/1000 of the
    // sample set, we get 99.8% combined, or close to three standard deviations.
    static const uint32_t kTailDenominator = 1000;
    uint32_t *tail = n >= kTailDenominator ? new uint32_t[n] : NULL;
    // loop over all the samples
    for (uint32_t j = 0; j < n; ++j) {
        size_t i = oldestClosed++ & (mSamplingN - 1);
        uint32_t wallNs = mMonotonicNs[i];
        if (tail != NULL) {
            tail[j] = wallNs;
        }
        wall.sample(wallNs);
        uint32_t sampleLoadNs = mLoadNs[i];
        loadNs.sample(sampleLoadNs);
#ifdef CPU_FREQUENCY_STATISTICS
        uint32_t sampleCpukHz = mCpukHz[i];
        // skip bad kHz samples
        if ((sampleCpukHz & ~0xF) != 0) {
            kHz.sample(sampleCpukHz >> 4);
            if (sampleCpukHz == previousCpukHz) {
                double megacycles = (double) sampleLoadNs * (double) (sampleCpukHz >> 4) * 1e-12;
                double adjMHz = megacycles / mixPeriodSec;  // _not_ wallNs * 1e9
                loadMHz.sample(adjMHz);
            }
        }
        previousCpukHz = sampleCpukHz;
#endif
    }
    if (n) {
        dprintf(fd, "  Simple moving statistics over last %.1f seconds:\n",
                    wall.n() * mixPeriodSec);
        dprintf(fd, "    wall clock time in ms per mix cycle:\n"
                    "      mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                    wall.mean()*1e-6, wall.minimum()*1e-6, wall.maximum()*1e-6,
                    wall.stddev()*1e-6);
        dprintf(fd, "    raw CPU load in us per mix cycle:\n"
                    "      mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                    loadNs.mean()*1e-3, loadNs.minimum()*1e-3, loadNs.maximum()*1e-3,
                    loadNs.stddev()*1e-3);
    } else {
        dprintf(fd, "  No FastMixer statistics available currently\n");
    }
#ifdef CPU_FREQUENCY_STATISTICS
    dprintf(fd, "  CPU clock frequency in MHz:\n"
                "    mean=%.0f min=%.0f max=%.0f stddev=%.0f\n",
                kHz.mean()*1e-3, kHz.minimum()*1e-3, kHz.maximum()*1e-3, kHz.stddev()*1e-3);
    dprintf(fd, "  adjusted CPU load in MHz (i.e. normalized for CPU clock frequency):\n"
                "    mean=%.1f min=%.1f max=%.1f stddev=%.1f\n",
                loadMHz.mean(), loadMHz.minimum(), loadMHz.maximum(), loadMHz.stddev());
#endif
    if (tail != NULL) {
        qsort(tail, n, sizeof(uint32_t), compare_uint32_t);
        // assume same number of tail samples on each side, left and right
        uint32_t count = n / kTailDenominator;
        CentralTendencyStatistics left, right;
        for (uint32_t i = 0; i < count; ++i) {
            left.sample(tail[i]);
            right.sample(tail[n - (i + 1)]);
        }
        dprintf(fd, "  Distribution of mix cycle times in ms for the tails (> ~3 stddev outliers):\n"
                    "    left tail: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n"
                    "    right tail: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
                    left.mean()*1e-6, left.minimum()*1e-6, left.maximum()*1e-6, left.stddev()*1e-6,
                    right.mean()*1e-6, right.minimum()*1e-6, right.maximum()*1e-6,
                    right.stddev()*1e-6);
        delete[] tail;
    }
#endif
    // The active track mask and track states are updated non-atomically.
    // So if we relied on isActive to decide whether to display,
    // then we might display an obsolete track or omit an active track.
    // Instead we always display all tracks, with an indication
    // of whether we think the track is active.
    uint32_t trackMask = mTrackMask;
    dprintf(fd, "  Fast tracks: kMaxFastTracks=%u activeMask=%#x\n",
            FastMixerState::kMaxFastTracks, trackMask);
    dprintf(fd, "  Index Active Full Partial Empty  Recent Ready\n");
    for (uint32_t i = 0; i < FastMixerState::kMaxFastTracks; ++i, trackMask >>= 1) {
        bool isActive = trackMask & 1;
        const FastTrackDump *ftDump = &mTracks[i];
        const FastTrackUnderruns& underruns = ftDump->mUnderruns;
        const char *mostRecent;
        switch (underruns.mBitFields.mMostRecent) {
        case UNDERRUN_FULL:
            mostRecent = "full";
            break;
        case UNDERRUN_PARTIAL:
            mostRecent = "partial";
            break;
        case UNDERRUN_EMPTY:
            mostRecent = "empty";
            break;
        default:
            mostRecent = "?";
            break;
        }
        dprintf(fd, "  %5u %6s %4u %7u %5u %7s %5zu\n", i, isActive ? "yes" : "no",
                (underruns.mBitFields.mFull) & UNDERRUN_MASK,
                (underruns.mBitFields.mPartial) & UNDERRUN_MASK,
                (underruns.mBitFields.mEmpty) & UNDERRUN_MASK,
                mostRecent, ftDump->mFramesReady);
    }
}

}   // namespace android
