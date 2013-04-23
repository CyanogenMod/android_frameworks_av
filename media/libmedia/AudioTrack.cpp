/*
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/


//#define LOG_NDEBUG 0
#define LOG_TAG "AudioTrack"

#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#include <sched.h>
#include <sys/resource.h>

#include <private/media/AudioTrackShared.h>

#include <media/AudioSystem.h>
#include <media/AudioTrack.h>

#include <utils/Log.h>
#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <utils/Timers.h>
#include <utils/Atomic.h>

#include <cutils/bitops.h>
#include <cutils/compiler.h>

#include <system/audio.h>
#include <system/audio_policy.h>

#include <audio_utils/primitives.h>

namespace android {
// ---------------------------------------------------------------------------

// static
status_t AudioTrack::getMinFrameCount(
        size_t* frameCount,
        audio_stream_type_t streamType,
        uint32_t sampleRate)
{
    if (frameCount == NULL) {
        return BAD_VALUE;
    }

    // default to 0 in case of error
    *frameCount = 0;

    // FIXME merge with similar code in createTrack_l(), except we're missing
    //       some information here that is available in createTrack_l():
    //          audio_io_handle_t output
    //          audio_format_t format
    //          audio_channel_mask_t channelMask
    //          audio_output_flags_t flags
    uint32_t afSampleRate;
    if (AudioSystem::getOutputSamplingRate(&afSampleRate, streamType) != NO_ERROR) {
        return NO_INIT;
    }
    size_t afFrameCount;
    if (AudioSystem::getOutputFrameCount(&afFrameCount, streamType) != NO_ERROR) {
        return NO_INIT;
    }
    uint32_t afLatency;
    if (AudioSystem::getOutputLatency(&afLatency, streamType) != NO_ERROR) {
        return NO_INIT;
    }

    // Ensure that buffer depth covers at least audio hardware latency
    uint32_t minBufCount = afLatency / ((1000 * afFrameCount) / afSampleRate);
    if (minBufCount < 2) minBufCount = 2;

    *frameCount = (sampleRate == 0) ? afFrameCount * minBufCount :
            afFrameCount * minBufCount * sampleRate / afSampleRate;
    ALOGV("getMinFrameCount=%d: afFrameCount=%d, minBufCount=%d, afSampleRate=%d, afLatency=%d",
            *frameCount, afFrameCount, minBufCount, afSampleRate, afLatency);
    return NO_ERROR;
}

// ---------------------------------------------------------------------------

AudioTrack::AudioTrack()
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL)
{
}

AudioTrack::AudioTrack(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCount,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL)
{
    mStatus = set(streamType, sampleRate, format, channelMask,
            frameCount, flags, cbf, user, notificationFrames,
            0 /*sharedBuffer*/, false /*threadCanCallJava*/, sessionId);
}

AudioTrack::AudioTrack(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        const sp<IMemory>& sharedBuffer,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL)
{
    if (sharedBuffer == 0) {
        ALOGE("sharedBuffer must be non-0");
        mStatus = BAD_VALUE;
        return;
    }
    mStatus = set(streamType, sampleRate, format, channelMask,
            0 /*frameCount*/, flags, cbf, user, notificationFrames,
            sharedBuffer, false /*threadCanCallJava*/, sessionId);
}

AudioTrack::~AudioTrack()
{
    ALOGV_IF(mSharedBuffer != 0, "Destructor sharedBuffer: %p", mSharedBuffer->pointer());

    if (mStatus == NO_ERROR) {
        // Make sure that callback function exits in the case where
        // it is looping on buffer full condition in obtainBuffer().
        // Otherwise the callback thread will never exit.
        stop();
        if (mAudioTrackThread != 0) {
            mAudioTrackThread->requestExit();   // see comment in AudioTrack.h
            mAudioTrackThread->requestExitAndWait();
            mAudioTrackThread.clear();
        }
        mAudioTrack.clear();
        IPCThreadState::self()->flushCommands();
        AudioSystem::releaseAudioSessionId(mSessionId);
    }
    delete mProxy;
}

status_t AudioTrack::set(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCountInt,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        const sp<IMemory>& sharedBuffer,
        bool threadCanCallJava,
        int sessionId)
{
    // FIXME "int" here is legacy and will be replaced by size_t later
    if (frameCountInt < 0) {
        ALOGE("Invalid frame count %d", frameCountInt);
        return BAD_VALUE;
    }
    size_t frameCount = frameCountInt;

    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(),
            sharedBuffer->size());

    ALOGV("set() streamType %d frameCount %u flags %04x", streamType, frameCount, flags);

    AutoMutex lock(mLock);
    if (mAudioTrack != 0) {
        ALOGE("Track already in use");
        return INVALID_OPERATION;
    }

    // handle default values first.
    if (streamType == AUDIO_STREAM_DEFAULT) {
        streamType = AUDIO_STREAM_MUSIC;
    }

    if (sampleRate == 0) {
        uint32_t afSampleRate;
        if (AudioSystem::getOutputSamplingRate(&afSampleRate, streamType) != NO_ERROR) {
            return NO_INIT;
        }
        sampleRate = afSampleRate;
    }
    mSampleRate = sampleRate;

    // these below should probably come from the audioFlinger too...
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }
    if (channelMask == 0) {
        channelMask = AUDIO_CHANNEL_OUT_STEREO;
    }

    // validate parameters
    if (!audio_is_valid_format(format)) {
        ALOGE("Invalid format");
        return BAD_VALUE;
    }

    // AudioFlinger does not currently support 8-bit data in shared memory
    if (format == AUDIO_FORMAT_PCM_8_BIT && sharedBuffer != 0) {
        ALOGE("8-bit data in shared memory is not supported");
        return BAD_VALUE;
    }

    // force direct flag if format is not linear PCM
    if (!audio_is_linear_pcm(format)) {
        flags = (audio_output_flags_t)
                // FIXME why can't we allow direct AND fast?
                ((flags | AUDIO_OUTPUT_FLAG_DIRECT) & ~AUDIO_OUTPUT_FLAG_FAST);
    }
    // only allow deep buffering for music stream type
    if (streamType != AUDIO_STREAM_MUSIC) {
        flags = (audio_output_flags_t)(flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
    }

    if (!audio_is_output_channel(channelMask)) {
        ALOGE("Invalid channel mask %#x", channelMask);
        return BAD_VALUE;
    }
    mChannelMask = channelMask;
    uint32_t channelCount = popcount(channelMask);
    mChannelCount = channelCount;

    if (audio_is_linear_pcm(format)) {
        mFrameSize = channelCount * audio_bytes_per_sample(format);
        mFrameSizeAF = channelCount * sizeof(int16_t);
    } else {
        mFrameSize = sizeof(uint8_t);
        mFrameSizeAF = sizeof(uint8_t);
    }

    audio_io_handle_t output = AudioSystem::getOutput(
                                    streamType,
                                    sampleRate, format, channelMask,
                                    flags);

    if (output == 0) {
        ALOGE("Could not get audio output for stream type %d", streamType);
        return BAD_VALUE;
    }

    mVolume[LEFT] = 1.0f;
    mVolume[RIGHT] = 1.0f;
    mSendLevel = 0.0f;
    mFrameCount = frameCount;
    mReqFrameCount = frameCount;
    mNotificationFramesReq = notificationFrames;
    mSessionId = sessionId;
    mAuxEffectId = 0;
    mFlags = flags;
    mCbf = cbf;

    if (cbf != NULL) {
        mAudioTrackThread = new AudioTrackThread(*this, threadCanCallJava);
        mAudioTrackThread->run("AudioTrack", ANDROID_PRIORITY_AUDIO, 0 /*stack*/);
    }

    // create the IAudioTrack
    status_t status = createTrack_l(streamType,
                                  sampleRate,
                                  format,
                                  frameCount,
                                  flags,
                                  sharedBuffer,
                                  output);

    if (status != NO_ERROR) {
        if (mAudioTrackThread != 0) {
            mAudioTrackThread->requestExit();
            mAudioTrackThread.clear();
        }
        return status;
    }

    mStatus = NO_ERROR;

    mStreamType = streamType;
    mFormat = format;

    mSharedBuffer = sharedBuffer;
    mActive = false;
    mUserData = user;
    mLoopCount = 0;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    mFlushed = false;
    AudioSystem::acquireAudioSessionId(mSessionId);
    return NO_ERROR;
}

// -------------------------------------------------------------------------

void AudioTrack::start()
{
    sp<AudioTrackThread> t = mAudioTrackThread;

    ALOGV("start %p", this);

    AutoMutex lock(mLock);
    // acquire a strong reference on the IMemory and IAudioTrack so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioTrack> audioTrack = mAudioTrack;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;

    if (!mActive) {
        mFlushed = false;
        mActive = true;
        mNewPosition = cblk->server + mUpdatePeriod;
        cblk->lock.lock();
        cblk->bufferTimeoutMs = MAX_STARTUP_TIMEOUT_MS;
        cblk->waitTimeMs = 0;
        android_atomic_and(~CBLK_DISABLED, &cblk->flags);
        if (t != 0) {
            t->resume();
        } else {
            mPreviousPriority = getpriority(PRIO_PROCESS, 0);
            get_sched_policy(0, &mPreviousSchedulingGroup);
            androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
        }

        ALOGV("start %p before lock cblk %p", this, cblk);
        status_t status = NO_ERROR;
        if (!(cblk->flags & CBLK_INVALID)) {
            cblk->lock.unlock();
            ALOGV("mAudioTrack->start()");
            status = mAudioTrack->start();
            cblk->lock.lock();
            if (status == DEAD_OBJECT) {
                android_atomic_or(CBLK_INVALID, &cblk->flags);
            }
        }
        if (cblk->flags & CBLK_INVALID) {
            audio_track_cblk_t* temp = cblk;
            status = restoreTrack_l(temp, true /*fromStart*/);
            cblk = temp;
        }
        cblk->lock.unlock();
        if (status != NO_ERROR) {
            ALOGV("start() failed");
            mActive = false;
            if (t != 0) {
                t->pause();
            } else {
                setpriority(PRIO_PROCESS, 0, mPreviousPriority);
                set_sched_policy(0, mPreviousSchedulingGroup);
            }
        }
    }

}

void AudioTrack::stop()
{
    sp<AudioTrackThread> t = mAudioTrackThread;

    ALOGV("stop %p", this);

    AutoMutex lock(mLock);
    if (mActive) {
        mActive = false;
        mCblk->cv.signal();
        mAudioTrack->stop();
        // Cancel loops (If we are in the middle of a loop, playback
        // would not stop until loopCount reaches 0).
        setLoop_l(0, 0, 0);
        // the playback head position will reset to 0, so if a marker is set, we need
        // to activate it again
        mMarkerReached = false;
        // Force flush if a shared buffer is used otherwise audioflinger
        // will not stop before end of buffer is reached.
        // It may be needed to make sure that we stop playback, likely in case looping is on.
        if (mSharedBuffer != 0) {
            flush_l();
        }
        if (t != 0) {
            t->pause();
        } else {
            setpriority(PRIO_PROCESS, 0, mPreviousPriority);
            set_sched_policy(0, mPreviousSchedulingGroup);
        }
    }

}

bool AudioTrack::stopped() const
{
    AutoMutex lock(mLock);
    return stopped_l();
}

void AudioTrack::flush()
{
    AutoMutex lock(mLock);
    if (!mActive && mSharedBuffer == 0) {
        flush_l();
    }
}

void AudioTrack::flush_l()
{
    ALOGV("flush");
    ALOG_ASSERT(!mActive);

    // clear playback marker and periodic update counter
    mMarkerPosition = 0;
    mMarkerReached = false;
    mUpdatePeriod = 0;

    mFlushed = true;
    mAudioTrack->flush();
    // Release AudioTrack callback thread in case it was waiting for new buffers
    // in AudioTrack::obtainBuffer()
    mCblk->cv.signal();
}

void AudioTrack::pause()
{
    ALOGV("pause");
    AutoMutex lock(mLock);
    if (mActive) {
        mActive = false;
        mCblk->cv.signal();
        mAudioTrack->pause();
    }
}

status_t AudioTrack::setVolume(float left, float right)
{
    if (mStatus != NO_ERROR) {
        return mStatus;
    }
    ALOG_ASSERT(mProxy != NULL);

    if (left < 0.0f || left > 1.0f || right < 0.0f || right > 1.0f) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mVolume[LEFT] = left;
    mVolume[RIGHT] = right;

    mProxy->setVolumeLR((uint32_t(uint16_t(right * 0x1000)) << 16) | uint16_t(left * 0x1000));

    return NO_ERROR;
}

status_t AudioTrack::setVolume(float volume)
{
    return setVolume(volume, volume);
}

status_t AudioTrack::setAuxEffectSendLevel(float level)
{
    ALOGV("setAuxEffectSendLevel(%f)", level);

    if (mStatus != NO_ERROR) {
        return mStatus;
    }
    ALOG_ASSERT(mProxy != NULL);

    if (level < 0.0f || level > 1.0f) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);

    mSendLevel = level;
    mProxy->setSendLevel(level);

    return NO_ERROR;
}

void AudioTrack::getAuxEffectSendLevel(float* level) const
{
    if (level != NULL) {
        *level  = mSendLevel;
    }
}

status_t AudioTrack::setSampleRate(uint32_t rate)
{
    uint32_t afSamplingRate;

    if (mIsTimed) {
        return INVALID_OPERATION;
    }

    if (AudioSystem::getOutputSamplingRate(&afSamplingRate, mStreamType) != NO_ERROR) {
        return NO_INIT;
    }
    // Resampler implementation limits input sampling rate to 2 x output sampling rate.
    if (rate == 0 || rate > afSamplingRate*2 ) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mSampleRate = rate;
    mProxy->setSampleRate(rate);

    return NO_ERROR;
}

uint32_t AudioTrack::getSampleRate() const
{
    if (mIsTimed) {
        return 0;
    }

    AutoMutex lock(mLock);
    return mSampleRate;
}

status_t AudioTrack::setLoop(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    AutoMutex lock(mLock);
    return setLoop_l(loopStart, loopEnd, loopCount);
}

// must be called with mLock held
status_t AudioTrack::setLoop_l(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    if (mSharedBuffer == 0 || mIsTimed) {
        return INVALID_OPERATION;
    }

    audio_track_cblk_t* cblk = mCblk;

    Mutex::Autolock _l(cblk->lock);

    if (loopCount == 0) {
        cblk->loopStart = UINT_MAX;
        cblk->loopEnd = UINT_MAX;
        cblk->loopCount = 0;
        mLoopCount = 0;
        return NO_ERROR;
    }

    if (loopStart >= loopEnd ||
        loopEnd - loopStart > mFrameCount ||
        cblk->server > loopStart) {
        ALOGE("setLoop invalid value: loopStart %d, loopEnd %d, loopCount %d, framecount %d, "
              "user %d", loopStart, loopEnd, loopCount, mFrameCount, cblk->user);
        return BAD_VALUE;
    }

    if ((mSharedBuffer != 0) && (loopEnd > mFrameCount)) {
        ALOGE("setLoop invalid value: loop markers beyond data: loopStart %d, loopEnd %d, "
            "framecount %d",
            loopStart, loopEnd, mFrameCount);
        return BAD_VALUE;
    }

    cblk->loopStart = loopStart;
    cblk->loopEnd = loopEnd;
    cblk->loopCount = loopCount;
    mLoopCount = loopCount;

    return NO_ERROR;
}

status_t AudioTrack::setMarkerPosition(uint32_t marker)
{
    if (mCbf == NULL) {
        return INVALID_OPERATION;
    }

    mMarkerPosition = marker;
    mMarkerReached = false;

    return NO_ERROR;
}

status_t AudioTrack::getMarkerPosition(uint32_t *marker) const
{
    if (marker == NULL) {
        return BAD_VALUE;
    }

    *marker = mMarkerPosition;

    return NO_ERROR;
}

status_t AudioTrack::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    if (mCbf == NULL) {
        return INVALID_OPERATION;
    }

    uint32_t curPosition;
    getPosition(&curPosition);
    mNewPosition = curPosition + updatePeriod;
    mUpdatePeriod = updatePeriod;

    return NO_ERROR;
}

status_t AudioTrack::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (updatePeriod == NULL) {
        return BAD_VALUE;
    }

    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioTrack::setPosition(uint32_t position)
{
    if (mSharedBuffer == 0 || mIsTimed) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);

    if (!stopped_l()) {
        return INVALID_OPERATION;
    }

    audio_track_cblk_t* cblk = mCblk;
    Mutex::Autolock _l(cblk->lock);

    if (position > cblk->user) {
        return BAD_VALUE;
    }

    cblk->server = position;
    android_atomic_or(CBLK_FORCEREADY, &cblk->flags);

    return NO_ERROR;
}

status_t AudioTrack::getPosition(uint32_t *position)
{
    if (position == NULL) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    *position = mFlushed ? 0 : mCblk->server;

    return NO_ERROR;
}

status_t AudioTrack::reload()
{
    if (mStatus != NO_ERROR) {
        return mStatus;
    }
    ALOG_ASSERT(mProxy != NULL);

    if (mSharedBuffer == 0 || mIsTimed) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);

    if (!stopped_l()) {
        return INVALID_OPERATION;
    }

    flush_l();

    (void) mProxy->stepUser(mFrameCount);

    return NO_ERROR;
}

audio_io_handle_t AudioTrack::getOutput()
{
    AutoMutex lock(mLock);
    return getOutput_l();
}

// must be called with mLock held
audio_io_handle_t AudioTrack::getOutput_l()
{
    return AudioSystem::getOutput(mStreamType,
            mSampleRate, mFormat, mChannelMask, mFlags);
}

status_t AudioTrack::attachAuxEffect(int effectId)
{
    ALOGV("attachAuxEffect(%d)", effectId);
    status_t status = mAudioTrack->attachAuxEffect(effectId);
    if (status == NO_ERROR) {
        mAuxEffectId = effectId;
    }
    return status;
}

// -------------------------------------------------------------------------

// must be called with mLock held
status_t AudioTrack::createTrack_l(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        size_t frameCount,
        audio_output_flags_t flags,
        const sp<IMemory>& sharedBuffer,
        audio_io_handle_t output)
{
    status_t status;
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        ALOGE("Could not get audioflinger");
        return NO_INIT;
    }

    uint32_t afLatency;
    if (AudioSystem::getLatency(output, streamType, &afLatency) != NO_ERROR) {
        return NO_INIT;
    }

    // Client decides whether the track is TIMED (see below), but can only express a preference
    // for FAST.  Server will perform additional tests.
    if ((flags & AUDIO_OUTPUT_FLAG_FAST) && !(
            // either of these use cases:
            // use case 1: shared buffer
            (sharedBuffer != 0) ||
            // use case 2: callback handler
            (mCbf != NULL))) {
        ALOGW("AUDIO_OUTPUT_FLAG_FAST denied by client");
        // once denied, do not request again if IAudioTrack is re-created
        flags = (audio_output_flags_t) (flags & ~AUDIO_OUTPUT_FLAG_FAST);
        mFlags = flags;
    }
    ALOGV("createTrack_l() output %d afLatency %d", output, afLatency);

    mNotificationFramesAct = mNotificationFramesReq;

    if (!audio_is_linear_pcm(format)) {

        if (sharedBuffer != 0) {
            // Same comment as below about ignoring frameCount parameter for set()
            frameCount = sharedBuffer->size();
        } else if (frameCount == 0) {
            size_t afFrameCount;
            if (AudioSystem::getFrameCount(output, streamType, &afFrameCount) != NO_ERROR) {
                return NO_INIT;
            }
            frameCount = afFrameCount;
        }

    } else if (sharedBuffer != 0) {

        // Ensure that buffer alignment matches channel count
        // 8-bit data in shared memory is not currently supported by AudioFlinger
        size_t alignment = /* format == AUDIO_FORMAT_PCM_8_BIT ? 1 : */ 2;
        if (mChannelCount > 1) {
            // More than 2 channels does not require stronger alignment than stereo
            alignment <<= 1;
        }
        if (((size_t)sharedBuffer->pointer() & (alignment - 1)) != 0) {
            ALOGE("Invalid buffer alignment: address %p, channel count %u",
                    sharedBuffer->pointer(), mChannelCount);
            return BAD_VALUE;
        }

        // When initializing a shared buffer AudioTrack via constructors,
        // there's no frameCount parameter.
        // But when initializing a shared buffer AudioTrack via set(),
        // there _is_ a frameCount parameter.  We silently ignore it.
        frameCount = sharedBuffer->size()/mChannelCount/sizeof(int16_t);

    } else if (!(flags & AUDIO_OUTPUT_FLAG_FAST)) {

        // FIXME move these calculations and associated checks to server
        uint32_t afSampleRate;
        if (AudioSystem::getSamplingRate(output, streamType, &afSampleRate) != NO_ERROR) {
            return NO_INIT;
        }
        size_t afFrameCount;
        if (AudioSystem::getFrameCount(output, streamType, &afFrameCount) != NO_ERROR) {
            return NO_INIT;
        }

        // Ensure that buffer depth covers at least audio hardware latency
        uint32_t minBufCount = afLatency / ((1000 * afFrameCount)/afSampleRate);
        if (minBufCount < 2) minBufCount = 2;

        size_t minFrameCount = (afFrameCount*sampleRate*minBufCount)/afSampleRate;
        ALOGV("minFrameCount: %u, afFrameCount=%d, minBufCount=%d, sampleRate=%u, afSampleRate=%u"
                ", afLatency=%d",
                minFrameCount, afFrameCount, minBufCount, sampleRate, afSampleRate, afLatency);

        if (frameCount == 0) {
            frameCount = minFrameCount;
        }
        if (mNotificationFramesAct == 0) {
            mNotificationFramesAct = frameCount/2;
        }
        // Make sure that application is notified with sufficient margin
        // before underrun
        if (mNotificationFramesAct > frameCount/2) {
            mNotificationFramesAct = frameCount/2;
        }
        if (frameCount < minFrameCount) {
            // not ALOGW because it happens all the time when playing key clicks over A2DP
            ALOGV("Minimum buffer size corrected from %d to %d",
                     frameCount, minFrameCount);
            frameCount = minFrameCount;
        }

    } else {
        // For fast tracks, the frame count calculations and checks are done by server
    }

    IAudioFlinger::track_flags_t trackFlags = IAudioFlinger::TRACK_DEFAULT;
    if (mIsTimed) {
        trackFlags |= IAudioFlinger::TRACK_TIMED;
    }

    pid_t tid = -1;
    if (flags & AUDIO_OUTPUT_FLAG_FAST) {
        trackFlags |= IAudioFlinger::TRACK_FAST;
        if (mAudioTrackThread != 0) {
            tid = mAudioTrackThread->getTid();
        }
    }

    sp<IAudioTrack> track = audioFlinger->createTrack(streamType,
                                                      sampleRate,
                                                      // AudioFlinger only sees 16-bit PCM
                                                      format == AUDIO_FORMAT_PCM_8_BIT ?
                                                              AUDIO_FORMAT_PCM_16_BIT : format,
                                                      mChannelMask,
                                                      frameCount,
                                                      &trackFlags,
                                                      sharedBuffer,
                                                      output,
                                                      tid,
                                                      &mSessionId,
                                                      &status);

    if (track == 0) {
        ALOGE("AudioFlinger could not create track, status: %d", status);
        return status;
    }
    sp<IMemory> iMem = track->getCblk();
    if (iMem == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    mAudioTrack = track;
    mCblkMemory = iMem;
    audio_track_cblk_t* cblk = static_cast<audio_track_cblk_t*>(iMem->pointer());
    mCblk = cblk;
    size_t temp = cblk->frameCount_;
    if (temp < frameCount || (frameCount == 0 && temp == 0)) {
        // In current design, AudioTrack client checks and ensures frame count validity before
        // passing it to AudioFlinger so AudioFlinger should not return a different value except
        // for fast track as it uses a special method of assigning frame count.
        ALOGW("Requested frameCount %u but received frameCount %u", frameCount, temp);
    }
    frameCount = temp;
    mAwaitBoost = false;
    if (flags & AUDIO_OUTPUT_FLAG_FAST) {
        if (trackFlags & IAudioFlinger::TRACK_FAST) {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST successful; frameCount %u", frameCount);
            mAwaitBoost = true;
        } else {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST denied by server; frameCount %u", frameCount);
            // once denied, do not request again if IAudioTrack is re-created
            flags = (audio_output_flags_t) (flags & ~AUDIO_OUTPUT_FLAG_FAST);
            mFlags = flags;
        }
        if (sharedBuffer == 0) {
            mNotificationFramesAct = frameCount/2;
        }
    }
    if (sharedBuffer == 0) {
        mBuffers = (char*)cblk + sizeof(audio_track_cblk_t);
    } else {
        mBuffers = sharedBuffer->pointer();
    }

    mAudioTrack->attachAuxEffect(mAuxEffectId);
    cblk->bufferTimeoutMs = MAX_STARTUP_TIMEOUT_MS;
    cblk->waitTimeMs = 0;
    mRemainingFrames = mNotificationFramesAct;
    // FIXME don't believe this lie
    mLatency = afLatency + (1000*frameCount) / sampleRate;
    mFrameCount = frameCount;
    // If IAudioTrack is re-created, don't let the requested frameCount
    // decrease.  This can confuse clients that cache frameCount().
    if (frameCount > mReqFrameCount) {
        mReqFrameCount = frameCount;
    }

    // update proxy
    delete mProxy;
    mProxy = new AudioTrackClientProxy(cblk, mBuffers, frameCount, mFrameSizeAF);
    mProxy->setVolumeLR((uint32_t(uint16_t(mVolume[RIGHT] * 0x1000)) << 16) |
            uint16_t(mVolume[LEFT] * 0x1000));
    mProxy->setSendLevel(mSendLevel);
    mProxy->setSampleRate(mSampleRate);
    if (sharedBuffer != 0) {
        // Force buffer full condition as data is already present in shared memory
        mProxy->stepUser(frameCount);
    }

    return NO_ERROR;
}

status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, int32_t waitCount)
{
    ALOG_ASSERT(mStatus == NO_ERROR && mProxy != NULL);

    AutoMutex lock(mLock);
    bool active;
    status_t result = NO_ERROR;
    audio_track_cblk_t* cblk = mCblk;
    uint32_t framesReq = audioBuffer->frameCount;
    uint32_t waitTimeMs = (waitCount < 0) ? cblk->bufferTimeoutMs : WAIT_PERIOD_MS;

    audioBuffer->frameCount  = 0;
    audioBuffer->size = 0;

    size_t framesAvail = mProxy->framesAvailable();

    cblk->lock.lock();
    if (cblk->flags & CBLK_INVALID) {
        goto create_new_track;
    }
    cblk->lock.unlock();

    if (framesAvail == 0) {
        cblk->lock.lock();
        goto start_loop_here;
        while (framesAvail == 0) {
            active = mActive;
            if (CC_UNLIKELY(!active)) {
                ALOGV("Not active and NO_MORE_BUFFERS");
                cblk->lock.unlock();
                return NO_MORE_BUFFERS;
            }
            if (CC_UNLIKELY(!waitCount)) {
                cblk->lock.unlock();
                return WOULD_BLOCK;
            }
            if (!(cblk->flags & CBLK_INVALID)) {
                mLock.unlock();
                // this condition is in shared memory, so if IAudioTrack and control block
                // are replaced due to mediaserver death or IAudioTrack invalidation then
                // cv won't be signalled, but fortunately the timeout will limit the wait
                result = cblk->cv.waitRelative(cblk->lock, milliseconds(waitTimeMs));
                cblk->lock.unlock();
                mLock.lock();
                if (!mActive) {
                    return status_t(STOPPED);
                }
                // IAudioTrack may have been re-created while mLock was unlocked
                cblk = mCblk;
                cblk->lock.lock();
            }

            if (cblk->flags & CBLK_INVALID) {
                goto create_new_track;
            }
            if (CC_UNLIKELY(result != NO_ERROR)) {
                cblk->waitTimeMs += waitTimeMs;
                if (cblk->waitTimeMs >= cblk->bufferTimeoutMs) {
                    // timing out when a loop has been set and we have already written upto loop end
                    // is a normal condition: no need to wake AudioFlinger up.
                    if (cblk->user < cblk->loopEnd) {
                        ALOGW("obtainBuffer timed out (is the CPU pegged?) %p name=%#x user=%08x, "
                              "server=%08x", this, cblk->mName, cblk->user, cblk->server);
                        //unlock cblk mutex before calling mAudioTrack->start() (see issue #1617140)
                        cblk->lock.unlock();
                        result = mAudioTrack->start();
                        cblk->lock.lock();
                        if (result == DEAD_OBJECT) {
                            android_atomic_or(CBLK_INVALID, &cblk->flags);
create_new_track:
                            audio_track_cblk_t* temp = cblk;
                            result = restoreTrack_l(temp, false /*fromStart*/);
                            cblk = temp;
                        }
                        if (result != NO_ERROR) {
                            ALOGW("obtainBuffer create Track error %d", result);
                            cblk->lock.unlock();
                            return result;
                        }
                    }
                    cblk->waitTimeMs = 0;
                }

                if (--waitCount == 0) {
                    cblk->lock.unlock();
                    return TIMED_OUT;
                }
            }
            // read the server count again
        start_loop_here:
            framesAvail = mProxy->framesAvailable_l();
        }
        cblk->lock.unlock();
    }

    cblk->waitTimeMs = 0;

    if (framesReq > framesAvail) {
        framesReq = framesAvail;
    }

    uint32_t u = cblk->user;
    uint32_t bufferEnd = cblk->userBase + mFrameCount;

    if (framesReq > bufferEnd - u) {
        framesReq = bufferEnd - u;
    }

    audioBuffer->frameCount = framesReq;
    audioBuffer->size = framesReq * mFrameSizeAF;
    audioBuffer->raw = mProxy->buffer(u);
    active = mActive;
    return active ? status_t(NO_ERROR) : status_t(STOPPED);
}

void AudioTrack::releaseBuffer(Buffer* audioBuffer)
{
    ALOG_ASSERT(mStatus == NO_ERROR && mProxy != NULL);

    AutoMutex lock(mLock);
    audio_track_cblk_t* cblk = mCblk;
    (void) mProxy->stepUser(audioBuffer->frameCount);
    if (audioBuffer->frameCount > 0) {
        // restart track if it was disabled by audioflinger due to previous underrun
        if (mActive && (cblk->flags & CBLK_DISABLED)) {
            android_atomic_and(~CBLK_DISABLED, &cblk->flags);
            ALOGW("releaseBuffer() track %p name=%#x disabled, restarting", this, cblk->mName);
            mAudioTrack->start();
        }
    }
}

// -------------------------------------------------------------------------

ssize_t AudioTrack::write(const void* buffer, size_t userSize)
{

    if (mSharedBuffer != 0 || mIsTimed) {
        return INVALID_OPERATION;
    }

    if (ssize_t(userSize) < 0) {
        // Sanity-check: user is most-likely passing an error code, and it would
        // make the return value ambiguous (actualSize vs error).
        ALOGE("AudioTrack::write(buffer=%p, size=%u (%d)",
                buffer, userSize, userSize);
        return BAD_VALUE;
    }

    ALOGV("write %p: %d bytes, mActive=%d", this, userSize, mActive);

    if (userSize == 0) {
        return 0;
    }

    // acquire a strong reference on the IMemory and IAudioTrack so that they cannot be destroyed
    // while we are accessing the cblk
    mLock.lock();
    sp<IAudioTrack> audioTrack = mAudioTrack;
    sp<IMemory> iMem = mCblkMemory;
    mLock.unlock();

    // since mLock is unlocked the IAudioTrack and shared memory may be re-created,
    // so all cblk references might still refer to old shared memory, but that should be benign

    ssize_t written = 0;
    const int8_t *src = (const int8_t *)buffer;
    Buffer audioBuffer;
    size_t frameSz = frameSize();

    do {
        audioBuffer.frameCount = userSize/frameSz;

        status_t err = obtainBuffer(&audioBuffer, -1);
        if (err < 0) {
            // out of buffers, return #bytes written
            if (err == status_t(NO_MORE_BUFFERS)) {
                break;
            }
            return ssize_t(err);
        }

        size_t toWrite;

        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            // Divide capacity by 2 to take expansion into account
            toWrite = audioBuffer.size>>1;
            memcpy_to_i16_from_u8(audioBuffer.i16, (const uint8_t *) src, toWrite);
        } else {
            toWrite = audioBuffer.size;
            memcpy(audioBuffer.i8, src, toWrite);
        }
        src += toWrite;
        userSize -= toWrite;
        written += toWrite;

        releaseBuffer(&audioBuffer);
    } while (userSize >= frameSz);

    return written;
}

// -------------------------------------------------------------------------

TimedAudioTrack::TimedAudioTrack() {
    mIsTimed = true;
}

status_t TimedAudioTrack::allocateTimedBuffer(size_t size, sp<IMemory>* buffer)
{
    AutoMutex lock(mLock);
    status_t result = UNKNOWN_ERROR;

    // acquire a strong reference on the IMemory and IAudioTrack so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioTrack> audioTrack = mAudioTrack;
    sp<IMemory> iMem = mCblkMemory;

    // If the track is not invalid already, try to allocate a buffer.  alloc
    // fails indicating that the server is dead, flag the track as invalid so
    // we can attempt to restore in just a bit.
    audio_track_cblk_t* cblk = mCblk;
    if (!(cblk->flags & CBLK_INVALID)) {
        result = mAudioTrack->allocateTimedBuffer(size, buffer);
        if (result == DEAD_OBJECT) {
            android_atomic_or(CBLK_INVALID, &cblk->flags);
        }
    }

    // If the track is invalid at this point, attempt to restore it. and try the
    // allocation one more time.
    if (cblk->flags & CBLK_INVALID) {
        cblk->lock.lock();
        audio_track_cblk_t* temp = cblk;
        result = restoreTrack_l(temp, false /*fromStart*/);
        cblk = temp;
        cblk->lock.unlock();

        if (result == OK) {
            result = mAudioTrack->allocateTimedBuffer(size, buffer);
        }
    }

    return result;
}

status_t TimedAudioTrack::queueTimedBuffer(const sp<IMemory>& buffer,
                                           int64_t pts)
{
    status_t status = mAudioTrack->queueTimedBuffer(buffer, pts);
    {
        AutoMutex lock(mLock);
        audio_track_cblk_t* cblk = mCblk;
        // restart track if it was disabled by audioflinger due to previous underrun
        if (buffer->size() != 0 && status == NO_ERROR &&
                mActive && (cblk->flags & CBLK_DISABLED)) {
            android_atomic_and(~CBLK_DISABLED, &cblk->flags);
            ALOGW("queueTimedBuffer() track %p disabled, restarting", this);
            mAudioTrack->start();
        }
    }
    return status;
}

status_t TimedAudioTrack::setMediaTimeTransform(const LinearTransform& xform,
                                                TargetTimeline target)
{
    return mAudioTrack->setMediaTimeTransform(xform, target);
}

// -------------------------------------------------------------------------

bool AudioTrack::processAudioBuffer(const sp<AudioTrackThread>& thread)
{
    Buffer audioBuffer;
    uint32_t frames;
    size_t writtenSize;

    mLock.lock();
    if (mAwaitBoost) {
        mAwaitBoost = false;
        mLock.unlock();
        static const int32_t kMaxTries = 5;
        int32_t tryCounter = kMaxTries;
        uint32_t pollUs = 10000;
        do {
            int policy = sched_getscheduler(0);
            if (policy == SCHED_FIFO || policy == SCHED_RR) {
                break;
            }
            usleep(pollUs);
            pollUs <<= 1;
        } while (tryCounter-- > 0);
        if (tryCounter < 0) {
            ALOGE("did not receive expected priority boost on time");
        }
        return true;
    }
    // acquire a strong reference on the IMemory and IAudioTrack so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioTrack> audioTrack = mAudioTrack;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;
    bool active = mActive;
    mLock.unlock();

    // since mLock is unlocked the IAudioTrack and shared memory may be re-created,
    // so all cblk references might still refer to old shared memory, but that should be benign

    // Manage underrun callback
    if (active && (mProxy->framesAvailable() == mFrameCount)) {
        ALOGV("Underrun user: %x, server: %x, flags %04x", cblk->user, cblk->server, cblk->flags);
        if (!(android_atomic_or(CBLK_UNDERRUN, &cblk->flags) & CBLK_UNDERRUN)) {
            mCbf(EVENT_UNDERRUN, mUserData, 0);
            if (cblk->server == mFrameCount) {
                mCbf(EVENT_BUFFER_END, mUserData, 0);
            }
            if (mSharedBuffer != 0) {
                return false;
            }
        }
    }

    // Manage loop end callback
    while (mLoopCount > cblk->loopCount) {
        int loopCount = -1;
        mLoopCount--;
        if (mLoopCount >= 0) loopCount = mLoopCount;

        mCbf(EVENT_LOOP_END, mUserData, (void *)&loopCount);
    }

    // Manage marker callback
    if (!mMarkerReached && (mMarkerPosition > 0)) {
        if (cblk->server >= mMarkerPosition) {
            mCbf(EVENT_MARKER, mUserData, (void *)&mMarkerPosition);
            mMarkerReached = true;
        }
    }

    // Manage new position callback
    if (mUpdatePeriod > 0) {
        while (cblk->server >= mNewPosition) {
            mCbf(EVENT_NEW_POS, mUserData, (void *)&mNewPosition);
            mNewPosition += mUpdatePeriod;
        }
    }

    // If Shared buffer is used, no data is requested from client.
    if (mSharedBuffer != 0) {
        frames = 0;
    } else {
        frames = mRemainingFrames;
    }

    // See description of waitCount parameter at declaration of obtainBuffer().
    // The logic below prevents us from being stuck below at obtainBuffer()
    // not being able to handle timed events (position, markers, loops).
    int32_t waitCount = -1;
    if (mUpdatePeriod || (!mMarkerReached && mMarkerPosition) || mLoopCount) {
        waitCount = 1;
    }

    do {

        audioBuffer.frameCount = frames;

        status_t err = obtainBuffer(&audioBuffer, waitCount);
        if (err < NO_ERROR) {
            if (err != TIMED_OUT) {
                ALOGE_IF(err != status_t(NO_MORE_BUFFERS),
                        "Error obtaining an audio buffer, giving up.");
                return false;
            }
            break;
        }
        if (err == status_t(STOPPED)) {
            return false;
        }

        // Divide buffer size by 2 to take into account the expansion
        // due to 8 to 16 bit conversion: the callback must fill only half
        // of the destination buffer
        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            audioBuffer.size >>= 1;
        }

        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        writtenSize = audioBuffer.size;

        // Sanity check on returned size
        if (ssize_t(writtenSize) <= 0) {
            // The callback is done filling buffers
            // Keep this thread going to handle timed events and
            // still try to get more data in intervals of WAIT_PERIOD_MS
            // but don't just loop and block the CPU, so wait
            usleep(WAIT_PERIOD_MS*1000);
            break;
        }

        if (writtenSize > reqSize) {
            writtenSize = reqSize;
        }

        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            // 8 to 16 bit conversion, note that source and destination are the same address
            memcpy_to_i16_from_u8(audioBuffer.i16, (const uint8_t *) audioBuffer.i8, writtenSize);
            writtenSize <<= 1;
        }

        audioBuffer.size = writtenSize;
        // NOTE: cblk->frameSize is not equal to AudioTrack::frameSize() for
        // 8 bit PCM data: in this case,  cblk->frameSize is based on a sample size of
        // 16 bit.
        audioBuffer.frameCount = writtenSize / mFrameSizeAF;

        frames -= audioBuffer.frameCount;

        releaseBuffer(&audioBuffer);
    }
    while (frames);

    if (frames == 0) {
        mRemainingFrames = mNotificationFramesAct;
    } else {
        mRemainingFrames = frames;
    }
    return true;
}

// must be called with mLock and refCblk.lock held. Callers must also hold strong references on
// the IAudioTrack and IMemory in case they are recreated here.
// If the IAudioTrack is successfully restored, the refCblk pointer is updated
// FIXME Don't depend on caller to hold strong references.
status_t AudioTrack::restoreTrack_l(audio_track_cblk_t*& refCblk, bool fromStart)
{
    status_t result;

    audio_track_cblk_t* cblk = refCblk;
    audio_track_cblk_t* newCblk = cblk;
    ALOGW("dead IAudioTrack, creating a new one from %s",
        fromStart ? "start()" : "obtainBuffer()");

    // signal old cblk condition so that other threads waiting for available buffers stop
    // waiting now
    cblk->cv.broadcast();
    cblk->lock.unlock();

    // refresh the audio configuration cache in this process to make sure we get new
    // output parameters in getOutput_l() and createTrack_l()
    AudioSystem::clearAudioConfigCache();

    // if the new IAudioTrack is created, createTrack_l() will modify the
    // following member variables: mAudioTrack, mCblkMemory and mCblk.
    // It will also delete the strong references on previous IAudioTrack and IMemory
    result = createTrack_l(mStreamType,
                           mSampleRate,
                           mFormat,
                           mReqFrameCount,  // so that frame count never goes down
                           mFlags,
                           mSharedBuffer,
                           getOutput_l());

    if (result == NO_ERROR) {
        uint32_t user = cblk->user;
        uint32_t server = cblk->server;
        // restore write index and set other indexes to reflect empty buffer status
        newCblk = mCblk;
        newCblk->user = user;
        newCblk->server = user;
        newCblk->userBase = user;
        newCblk->serverBase = user;
        // restore loop: this is not guaranteed to succeed if new frame count is not
        // compatible with loop length
        setLoop_l(cblk->loopStart, cblk->loopEnd, cblk->loopCount);
        size_t frames = 0;
        if (!fromStart) {
            newCblk->bufferTimeoutMs = MAX_RUN_TIMEOUT_MS;
            // Make sure that a client relying on callback events indicating underrun or
            // the actual amount of audio frames played (e.g SoundPool) receives them.
            if (mSharedBuffer == 0) {
                if (user > server) {
                    frames = ((user - server) > mFrameCount) ?
                            mFrameCount : (user - server);
                    memset(mBuffers, 0, frames * mFrameSizeAF);
                }
                // restart playback even if buffer is not completely filled.
                android_atomic_or(CBLK_FORCEREADY, &newCblk->flags);
            }
        }
        if (mSharedBuffer != 0) {
            frames = mFrameCount;
        }
        if (frames > 0) {
            // stepUser() clears CBLK_UNDERRUN flag enabling underrun callbacks to
            // the client
            mProxy->stepUser(frames);
        }
        if (mActive) {
            result = mAudioTrack->start();
            ALOGW_IF(result != NO_ERROR, "restoreTrack_l() start() failed status %d", result);
        }
        if (fromStart && result == NO_ERROR) {
            mNewPosition = newCblk->server + mUpdatePeriod;
        }
    }
    ALOGW_IF(result != NO_ERROR, "restoreTrack_l() failed status %d", result);
    ALOGV("restoreTrack_l() status %d mActive %d cblk %p, old cblk %p flags %08x old flags %08x",
        result, mActive, newCblk, cblk, newCblk->flags, cblk->flags);

    if (result == NO_ERROR) {
        // from now on we switch to the newly created cblk
        refCblk = newCblk;
    }
    newCblk->lock.lock();

    ALOGW_IF(result != NO_ERROR, "restoreTrack_l() error %d", result);

    return result;
}

status_t AudioTrack::dump(int fd, const Vector<String16>& args) const
{

    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.append(" AudioTrack::dump\n");
    snprintf(buffer, 255, "  stream type(%d), left - right volume(%f, %f)\n", mStreamType,
            mVolume[0], mVolume[1]);
    result.append(buffer);
    snprintf(buffer, 255, "  format(%d), channel count(%d), frame count(%d)\n", mFormat,
            mChannelCount, mFrameCount);
    result.append(buffer);
    snprintf(buffer, 255, "  sample rate(%u), status(%d)\n", mSampleRate, mStatus);
    result.append(buffer);
    snprintf(buffer, 255, "  active(%d), latency (%d)\n", mActive, mLatency);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

// =========================================================================

AudioTrack::AudioTrackThread::AudioTrackThread(AudioTrack& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver), mPaused(true)
{
}

AudioTrack::AudioTrackThread::~AudioTrackThread()
{
}

bool AudioTrack::AudioTrackThread::threadLoop()
{
    {
        AutoMutex _l(mMyLock);
        if (mPaused) {
            mMyCond.wait(mMyLock);
            // caller will check for exitPending()
            return true;
        }
    }
    if (!mReceiver.processAudioBuffer(this)) {
        pause();
    }
    return true;
}

void AudioTrack::AudioTrackThread::requestExit()
{
    // must be in this order to avoid a race condition
    Thread::requestExit();
    resume();
}

void AudioTrack::AudioTrackThread::pause()
{
    AutoMutex _l(mMyLock);
    mPaused = true;
}

void AudioTrack::AudioTrackThread::resume()
{
    AutoMutex _l(mMyLock);
    if (mPaused) {
        mPaused = false;
        mMyCond.signal();
    }
}

}; // namespace android
