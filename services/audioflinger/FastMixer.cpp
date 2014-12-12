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
#include <utils/Debug.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <system/audio.h>
#ifdef FAST_THREAD_STATISTICS
#include <cpustats/CentralTendencyStatistics.h>
#ifdef CPU_FREQUENCY_STATISTICS
#include <cpustats/ThreadCpuUsage.h>
#endif
#endif
#include <audio_utils/format.h>
#include "AudioMixer.h"
#include "FastMixer.h"

#define FCC_2                       2   // fixed channel count assumption

namespace android {

/*static*/ const FastMixerState FastMixer::sInitial;

FastMixer::FastMixer() : FastThread(),
    mSlopNs(0),
    // mFastTrackNames
    // mGenerations
    mOutputSink(NULL),
    mOutputSinkGen(0),
    mMixer(NULL),
    mSinkBuffer(NULL),
    mSinkBufferSize(0),
    mSinkChannelCount(FCC_2),
    mMixerBuffer(NULL),
    mMixerBufferSize(0),
    mMixerBufferFormat(AUDIO_FORMAT_PCM_16_BIT),
    mMixerBufferState(UNDEFINED),
    mFormat(Format_Invalid),
    mSampleRate(0),
    mFastTracksGen(0),
    mTotalNativeFramesWritten(0),
    // timestamp
    mNativeFramesWrittenButNotPresented(0)   // the = 0 is to silence the compiler
{
    // FIXME pass sInitial as parameter to base class constructor, and make it static local
    mPrevious = &sInitial;
    mCurrent = &sInitial;

    mDummyDumpState = &mDummyFastMixerDumpState;
    // TODO: Add channel mask to NBAIO_Format.
    // We assume that the channel mask must be a valid positional channel mask.
    mSinkChannelMask = audio_channel_out_mask_from_count(mSinkChannelCount);

    unsigned i;
    for (i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
        mFastTrackNames[i] = -1;
        mGenerations[i] = 0;
    }
#ifdef FAST_THREAD_STATISTICS
    mOldLoad.tv_sec = 0;
    mOldLoad.tv_nsec = 0;
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
    if (mMixer != NULL) {
        mMixer->setLog(logWriter);
    }
}

void FastMixer::onIdle()
{
    mPreIdle = *(const FastMixerState *)mCurrent;
    mCurrent = &mPreIdle;
}

void FastMixer::onExit()
{
    delete mMixer;
    free(mMixerBuffer);
    free(mSinkBuffer);
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
    const FastMixerState * const current = (const FastMixerState *) mCurrent;
    const FastMixerState * const previous = (const FastMixerState *) mPrevious;
    FastMixerDumpState * const dumpState = (FastMixerDumpState *) mDumpState;
    const size_t frameCount = current->mFrameCount;

    // handle state change here, but since we want to diff the state,
    // we're prepared for previous == &sInitial the first time through
    unsigned previousTrackMask;

    // check for change in output HAL configuration
    NBAIO_Format previousFormat = mFormat;
    if (current->mOutputSinkGen != mOutputSinkGen) {
        mOutputSink = current->mOutputSink;
        mOutputSinkGen = current->mOutputSinkGen;
        if (mOutputSink == NULL) {
            mFormat = Format_Invalid;
            mSampleRate = 0;
            mSinkChannelCount = 0;
            mSinkChannelMask = AUDIO_CHANNEL_NONE;
        } else {
            mFormat = mOutputSink->format();
            mSampleRate = Format_sampleRate(mFormat);
            mSinkChannelCount = Format_channelCount(mFormat);
            LOG_ALWAYS_FATAL_IF(mSinkChannelCount > AudioMixer::MAX_NUM_CHANNELS);

            // TODO: Add channel mask to NBAIO_Format
            // We assume that the channel mask must be a valid positional channel mask.
            mSinkChannelMask = audio_channel_out_mask_from_count(mSinkChannelCount);
        }
        dumpState->mSampleRate = mSampleRate;
    }

    if ((!Format_isEqual(mFormat, previousFormat)) || (frameCount != previous->mFrameCount)) {
        // FIXME to avoid priority inversion, don't delete here
        delete mMixer;
        mMixer = NULL;
        free(mMixerBuffer);
        mMixerBuffer = NULL;
        free(mSinkBuffer);
        mSinkBuffer = NULL;
        if (frameCount > 0 && mSampleRate > 0) {
            // The mixer produces either 16 bit PCM or float output, select
            // float output if the HAL supports higher than 16 bit precision.
            mMixerBufferFormat = mFormat.mFormat == AUDIO_FORMAT_PCM_16_BIT ?
                    AUDIO_FORMAT_PCM_16_BIT : AUDIO_FORMAT_PCM_FLOAT;
            // FIXME new may block for unbounded time at internal mutex of the heap
            //       implementation; it would be better to have normal mixer allocate for us
            //       to avoid blocking here and to prevent possible priority inversion
            mMixer = new AudioMixer(frameCount, mSampleRate, FastMixerState::kMaxFastTracks);
            const size_t mixerFrameSize = mSinkChannelCount
                    * audio_bytes_per_sample(mMixerBufferFormat);
            mMixerBufferSize = mixerFrameSize * frameCount;
            (void)posix_memalign(&mMixerBuffer, 32, mMixerBufferSize);
            const size_t sinkFrameSize = mSinkChannelCount
                    * audio_bytes_per_sample(mFormat.mFormat);
            if (sinkFrameSize > mixerFrameSize) { // need a sink buffer
                mSinkBufferSize = sinkFrameSize * frameCount;
                (void)posix_memalign(&mSinkBuffer, 32, mSinkBufferSize);
            }
            mPeriodNs = (frameCount * 1000000000LL) / mSampleRate;    // 1.00
            mUnderrunNs = (frameCount * 1750000000LL) / mSampleRate;  // 1.75
            mOverrunNs = (frameCount * 500000000LL) / mSampleRate;    // 0.50
            mForceNs = (frameCount * 950000000LL) / mSampleRate;      // 0.95
            mWarmupNsMin = (frameCount * 750000000LL) / mSampleRate;  // 0.75
            mWarmupNsMax = (frameCount * 1250000000LL) / mSampleRate; // 1.25
        } else {
            mPeriodNs = 0;
            mUnderrunNs = 0;
            mOverrunNs = 0;
            mForceNs = 0;
            mWarmupNsMin = 0;
            mWarmupNsMax = LONG_MAX;
        }
        mMixerBufferState = UNDEFINED;
#if !LOG_NDEBUG
        for (unsigned i = 0; i < FastMixerState::kMaxFastTracks; ++i) {
            mFastTrackNames[i] = -1;
        }
#endif
        // we need to reconfigure all active tracks
        previousTrackMask = 0;
        mFastTracksGen = current->mFastTracksGen - 1;
        dumpState->mFrameCount = frameCount;
    } else {
        previousTrackMask = previous->mTrackMask;
    }

    // check for change in active track set
    const unsigned currentTrackMask = current->mTrackMask;
    dumpState->mTrackMask = currentTrackMask;
    if (current->mFastTracksGen != mFastTracksGen) {
        ALOG_ASSERT(mMixerBuffer != NULL);
        int name;

        // process removed tracks first to avoid running out of track names
        unsigned removedTracks = previousTrackMask & ~currentTrackMask;
        while (removedTracks != 0) {
            int i = __builtin_ctz(removedTracks);
            removedTracks &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];
            ALOG_ASSERT(fastTrack->mBufferProvider == NULL);
            if (mMixer != NULL) {
                name = mFastTrackNames[i];
                ALOG_ASSERT(name >= 0);
                mMixer->deleteTrackName(name);
            }
#if !LOG_NDEBUG
            mFastTrackNames[i] = -1;
#endif
            // don't reset track dump state, since other side is ignoring it
            mGenerations[i] = fastTrack->mGeneration;
        }

        // now process added tracks
        unsigned addedTracks = currentTrackMask & ~previousTrackMask;
        while (addedTracks != 0) {
            int i = __builtin_ctz(addedTracks);
            addedTracks &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];
            AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
            ALOG_ASSERT(bufferProvider != NULL && mFastTrackNames[i] == -1);
            if (mMixer != NULL) {
                name = mMixer->getTrackName(fastTrack->mChannelMask,
                        fastTrack->mFormat, AUDIO_SESSION_OUTPUT_MIX);
                ALOG_ASSERT(name >= 0);
                mFastTrackNames[i] = name;
                mMixer->setBufferProvider(name, bufferProvider);
                mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MAIN_BUFFER,
                        (void *)mMixerBuffer);
                // newly allocated track names default to full scale volume
                mMixer->setParameter(
                        name,
                        AudioMixer::TRACK,
                        AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::FORMAT,
                        (void *)(uintptr_t)fastTrack->mFormat);
                mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                        (void *)(uintptr_t)fastTrack->mChannelMask);
                mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MIXER_CHANNEL_MASK,
                        (void *)(uintptr_t)mSinkChannelMask);
                mMixer->enable(name);
            }
            mGenerations[i] = fastTrack->mGeneration;
        }

        // finally process (potentially) modified tracks; these use the same slot
        // but may have a different buffer provider or volume provider
        unsigned modifiedTracks = currentTrackMask & previousTrackMask;
        while (modifiedTracks != 0) {
            int i = __builtin_ctz(modifiedTracks);
            modifiedTracks &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];
            if (fastTrack->mGeneration != mGenerations[i]) {
                // this track was actually modified
                AudioBufferProvider *bufferProvider = fastTrack->mBufferProvider;
                ALOG_ASSERT(bufferProvider != NULL);
                if (mMixer != NULL) {
                    name = mFastTrackNames[i];
                    ALOG_ASSERT(name >= 0);
                    mMixer->setBufferProvider(name, bufferProvider);
                    if (fastTrack->mVolumeProvider == NULL) {
                        float f = AudioMixer::UNITY_GAIN_FLOAT;
                        mMixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0, &f);
                        mMixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1, &f);
                    }
                    mMixer->setParameter(name, AudioMixer::RESAMPLE,
                            AudioMixer::REMOVE, NULL);
                    mMixer->setParameter(
                            name,
                            AudioMixer::TRACK,
                            AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                    mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::FORMAT,
                            (void *)(uintptr_t)fastTrack->mFormat);
                    mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                            (void *)(uintptr_t)fastTrack->mChannelMask);
                    mMixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MIXER_CHANNEL_MASK,
                            (void *)(uintptr_t)mSinkChannelMask);
                    // already enabled
                }
                mGenerations[i] = fastTrack->mGeneration;
            }
        }

        mFastTracksGen = current->mFastTracksGen;

        dumpState->mNumTracks = popcount(currentTrackMask);
    }
}

void FastMixer::onWork()
{
    const FastMixerState * const current = (const FastMixerState *) mCurrent;
    FastMixerDumpState * const dumpState = (FastMixerDumpState *) mDumpState;
    const FastMixerState::Command command = mCommand;
    const size_t frameCount = current->mFrameCount;

    if ((command & FastMixerState::MIX) && (mMixer != NULL) && mIsWarm) {
        ALOG_ASSERT(mMixerBuffer != NULL);

        // AudioMixer::mState.enabledTracks is undefined if mState.hook == process__validate,
        // so we keep a side copy of enabledTracks
        bool anyEnabledTracks = false;

        // for each track, update volume and check for underrun
        unsigned currentTrackMask = current->mTrackMask;
        while (currentTrackMask != 0) {
            int i = __builtin_ctz(currentTrackMask);
            currentTrackMask &= ~(1 << i);
            const FastTrack* fastTrack = &current->mFastTracks[i];

            // Refresh the per-track timestamp
            if (mTimestampStatus == NO_ERROR) {
                uint32_t trackFramesWrittenButNotPresented =
                    mNativeFramesWrittenButNotPresented;
                uint32_t trackFramesWritten = fastTrack->mBufferProvider->framesReleased();
                // Can't provide an AudioTimestamp before first frame presented,
                // or during the brief 32-bit wraparound window
                if (trackFramesWritten >= trackFramesWrittenButNotPresented) {
                    AudioTimestamp perTrackTimestamp;
                    perTrackTimestamp.mPosition =
                            trackFramesWritten - trackFramesWrittenButNotPresented;
                    perTrackTimestamp.mTime = mTimestamp.mTime;
                    fastTrack->mBufferProvider->onTimestamp(perTrackTimestamp);
                }
            }

            int name = mFastTrackNames[i];
            ALOG_ASSERT(name >= 0);
            if (fastTrack->mVolumeProvider != NULL) {
                gain_minifloat_packed_t vlr = fastTrack->mVolumeProvider->getVolumeLR();
                float vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                float vrf = float_from_gain(gain_minifloat_unpack_right(vlr));

                mMixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0, &vlf);
                mMixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1, &vrf);
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
                    mMixer->disable(name);
                } else {
                    // allow mixing partial buffer
                    underruns.mBitFields.mPartial++;
                    underruns.mBitFields.mMostRecent = UNDERRUN_PARTIAL;
                    mMixer->enable(name);
                    anyEnabledTracks = true;
                }
            } else {
                underruns.mBitFields.mFull++;
                underruns.mBitFields.mMostRecent = UNDERRUN_FULL;
                mMixer->enable(name);
                anyEnabledTracks = true;
            }
            ftDump->mUnderruns = underruns;
            ftDump->mFramesReady = framesReady;
        }

        int64_t pts;
        if (mOutputSink == NULL || (OK != mOutputSink->getNextWriteTimestamp(&pts))) {
            pts = AudioBufferProvider::kInvalidPTS;
        }

        if (anyEnabledTracks) {
            // process() is CPU-bound
            mMixer->process(pts);
            mMixerBufferState = MIXED;
        } else if (mMixerBufferState != ZEROED) {
            mMixerBufferState = UNDEFINED;
        }

    } else if (mMixerBufferState == MIXED) {
        mMixerBufferState = UNDEFINED;
    }
    //bool didFullWrite = false;    // dumpsys could display a count of partial writes
    if ((command & FastMixerState::WRITE) && (mOutputSink != NULL) && (mMixerBuffer != NULL)) {
        if (mMixerBufferState == UNDEFINED) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
            mMixerBufferState = ZEROED;
        }
        // prepare the buffer used to write to sink
        void *buffer = mSinkBuffer != NULL ? mSinkBuffer : mMixerBuffer;
        if (mFormat.mFormat != mMixerBufferFormat) { // sink format not the same as mixer format
            memcpy_by_audio_format(buffer, mFormat.mFormat, mMixerBuffer, mMixerBufferFormat,
                    frameCount * Format_channelCount(mFormat));
        }
        // if non-NULL, then duplicate write() to this non-blocking sink
        NBAIO_Sink* teeSink;
        if ((teeSink = current->mTeeSink) != NULL) {
            (void) teeSink->write(buffer, frameCount);
        }
        // FIXME write() is non-blocking and lock-free for a properly implemented NBAIO sink,
        //       but this code should be modified to handle both non-blocking and blocking sinks
        dumpState->mWriteSequence++;
        ATRACE_BEGIN("write");
        ssize_t framesWritten = mOutputSink->write(buffer, frameCount);
        ATRACE_END();
        dumpState->mWriteSequence++;
        if (framesWritten >= 0) {
            ALOG_ASSERT((size_t) framesWritten <= frameCount);
            mTotalNativeFramesWritten += framesWritten;
            dumpState->mFramesWritten = mTotalNativeFramesWritten;
            //if ((size_t) framesWritten == frameCount) {
            //    didFullWrite = true;
            //}
        } else {
            dumpState->mWriteErrors++;
        }
        mAttemptedWrite = true;
        // FIXME count # of writes blocked excessively, CPU usage, etc. for dump

        mTimestampStatus = mOutputSink->getTimestamp(mTimestamp);
        if (mTimestampStatus == NO_ERROR) {
            uint32_t totalNativeFramesPresented = mTimestamp.mPosition;
            if (totalNativeFramesPresented <= mTotalNativeFramesWritten) {
                mNativeFramesWrittenButNotPresented =
                    mTotalNativeFramesWritten - totalNativeFramesPresented;
            } else {
                // HAL reported that more frames were presented than were written
                mTimestampStatus = INVALID_OPERATION;
            }
        }
    }
}

}   // namespace android
