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
#ifdef FAST_MIXER_STATISTICS
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

/*static*/ const FastMixerState FastMixer::initial;

FastMixer::FastMixer() : FastThread(),
    slopNs(0),
    // fastTrackNames
    // generations
    outputSink(NULL),
    outputSinkGen(0),
    mixer(NULL),
    mSinkBuffer(NULL),
    mSinkBufferSize(0),
    mSinkChannelCount(FCC_2),
    mMixerBuffer(NULL),
    mMixerBufferSize(0),
    mMixerBufferFormat(AUDIO_FORMAT_PCM_16_BIT),
    mMixerBufferState(UNDEFINED),
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
    // TODO: Add channel mask to NBAIO_Format.
    // We assume that the channel mask must be a valid positional channel mask.
    mSinkChannelMask = audio_channel_out_mask_from_count(mSinkChannelCount);

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
            mSinkChannelCount = 0;
            mSinkChannelMask = AUDIO_CHANNEL_NONE;
        } else {
            format = outputSink->format();
            sampleRate = Format_sampleRate(format);
            mSinkChannelCount = Format_channelCount(format);
            LOG_ALWAYS_FATAL_IF(mSinkChannelCount > AudioMixer::MAX_NUM_CHANNELS);

            // TODO: Add channel mask to NBAIO_Format
            // We assume that the channel mask must be a valid positional channel mask.
            mSinkChannelMask = audio_channel_out_mask_from_count(mSinkChannelCount);
        }
        dumpState->mSampleRate = sampleRate;
    }

    if ((!Format_isEqual(format, previousFormat)) || (frameCount != previous->mFrameCount)) {
        // FIXME to avoid priority inversion, don't delete here
        delete mixer;
        mixer = NULL;
        free(mMixerBuffer);
        mMixerBuffer = NULL;
        free(mSinkBuffer);
        mSinkBuffer = NULL;
        if (frameCount > 0 && sampleRate > 0) {
            // FIXME new may block for unbounded time at internal mutex of the heap
            //       implementation; it would be better to have normal mixer allocate for us
            //       to avoid blocking here and to prevent possible priority inversion
            mixer = new AudioMixer(frameCount, sampleRate, FastMixerState::kMaxFastTracks);
            const size_t mixerFrameSize = mSinkChannelCount
                    * audio_bytes_per_sample(mMixerBufferFormat);
            mMixerBufferSize = mixerFrameSize * frameCount;
            (void)posix_memalign(&mMixerBuffer, 32, mMixerBufferSize);
            const size_t sinkFrameSize = mSinkChannelCount
                    * audio_bytes_per_sample(format.mFormat);
            if (sinkFrameSize > mixerFrameSize) { // need a sink buffer
                mSinkBufferSize = sinkFrameSize * frameCount;
                (void)posix_memalign(&mSinkBuffer, 32, mSinkBufferSize);
            }
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
        mMixerBufferState = UNDEFINED;
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
        ALOG_ASSERT(mMixerBuffer != NULL);
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
                name = mixer->getTrackName(fastTrack->mChannelMask,
                        fastTrack->mFormat, AUDIO_SESSION_OUTPUT_MIX);
                ALOG_ASSERT(name >= 0);
                fastTrackNames[i] = name;
                mixer->setBufferProvider(name, bufferProvider);
                mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MAIN_BUFFER,
                        (void *)mMixerBuffer);
                // newly allocated track names default to full scale volume
                mixer->setParameter(
                        name,
                        AudioMixer::TRACK,
                        AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::FORMAT,
                        (void *)(uintptr_t)fastTrack->mFormat);
                mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                        (void *)(uintptr_t)fastTrack->mChannelMask);
                mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MIXER_CHANNEL_MASK,
                        (void *)(uintptr_t)mSinkChannelMask);
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
                        float f = AudioMixer::UNITY_GAIN_FLOAT;
                        mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0, &f);
                        mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1, &f);
                    }
                    mixer->setParameter(name, AudioMixer::RESAMPLE,
                            AudioMixer::REMOVE, NULL);
                    mixer->setParameter(
                            name,
                            AudioMixer::TRACK,
                            AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                    mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::FORMAT,
                            (void *)(uintptr_t)fastTrack->mFormat);
                    mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::CHANNEL_MASK,
                            (void *)(uintptr_t)fastTrack->mChannelMask);
                    mixer->setParameter(name, AudioMixer::TRACK, AudioMixer::MIXER_CHANNEL_MASK,
                            (void *)(uintptr_t)mSinkChannelMask);
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
        ALOG_ASSERT(mMixerBuffer != NULL);
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
                float vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                float vrf = float_from_gain(gain_minifloat_unpack_right(vlr));

                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME0, &vlf);
                mixer->setParameter(name, AudioMixer::VOLUME, AudioMixer::VOLUME1, &vrf);
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
        mMixerBufferState = MIXED;
    } else if (mMixerBufferState == MIXED) {
        mMixerBufferState = UNDEFINED;
    }
    //bool didFullWrite = false;    // dumpsys could display a count of partial writes
    if ((command & FastMixerState::WRITE) && (outputSink != NULL) && (mMixerBuffer != NULL)) {
        if (mMixerBufferState == UNDEFINED) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
            mMixerBufferState = ZEROED;
        }
        void *buffer = mSinkBuffer != NULL ? mSinkBuffer : mMixerBuffer;
        if (format.mFormat != mMixerBufferFormat) { // sink format not the same as mixer format
            memcpy_by_audio_format(buffer, format.mFormat, mMixerBuffer, mMixerBufferFormat,
                    frameCount * Format_channelCount(format));
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
        ssize_t framesWritten = outputSink->write(buffer, frameCount);
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

}   // namespace android
