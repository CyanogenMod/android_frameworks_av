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

#include <inttypes.h>
#include <math.h>
#include <sys/resource.h>

#include <audio_utils/primitives.h>
#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <media/IAudioFlinger.h>
#include <media/AudioPolicyHelper.h>
#include <media/AudioResamplerPublic.h>
#include "media/AVMediaExtensions.h"
#include <cutils/properties.h>

#define WAIT_PERIOD_MS                  10
#define WAIT_STREAM_END_TIMEOUT_SEC     120
static const int kMaxLoopCountNotifications = 32;

namespace android {
// ---------------------------------------------------------------------------

// TODO: Move to a separate .h

template <typename T>
static inline const T &min(const T &x, const T &y) {
    return x < y ? x : y;
}

template <typename T>
static inline const T &max(const T &x, const T &y) {
    return x > y ? x : y;
}

static inline nsecs_t framesToNanoseconds(ssize_t frames, uint32_t sampleRate, float speed)
{
    return ((double)frames * 1000000000) / ((double)sampleRate * speed);
}

static int64_t convertTimespecToUs(const struct timespec &tv)
{
    return tv.tv_sec * 1000000ll + tv.tv_nsec / 1000;
}

// current monotonic time in microseconds.
static int64_t getNowUs()
{
    struct timespec tv;
    (void) clock_gettime(CLOCK_MONOTONIC, &tv);
    return convertTimespecToUs(tv);
}

// FIXME: we don't use the pitch setting in the time stretcher (not working);
// instead we emulate it using our sample rate converter.
static const bool kFixPitch = true; // enable pitch fix
static inline uint32_t adjustSampleRate(uint32_t sampleRate, float pitch)
{
    return kFixPitch ? (sampleRate * pitch + 0.5) : sampleRate;
}

static inline float adjustSpeed(float speed, float pitch)
{
    return kFixPitch ? speed / max(pitch, AUDIO_TIMESTRETCH_PITCH_MIN_DELTA) : speed;
}

static inline float adjustPitch(float pitch)
{
    return kFixPitch ? AUDIO_TIMESTRETCH_PITCH_NORMAL : pitch;
}

// Must match similar computation in createTrack_l in Threads.cpp.
// TODO: Move to a common library
static size_t calculateMinFrameCount(
        uint32_t afLatencyMs, uint32_t afFrameCount, uint32_t afSampleRate,
        uint32_t sampleRate, float speed)
{
    // Ensure that buffer depth covers at least audio hardware latency
    uint32_t minBufCount = afLatencyMs / ((1000 * afFrameCount) / afSampleRate);
    if (minBufCount < 2) {
        minBufCount = 2;
    }
    ALOGV("calculateMinFrameCount afLatency %u  afFrameCount %u  afSampleRate %u  "
            "sampleRate %u  speed %f  minBufCount: %u",
            afLatencyMs, afFrameCount, afSampleRate, sampleRate, speed, minBufCount);
    return minBufCount * sourceFramesNeededWithTimestretch(
            sampleRate, afFrameCount, afSampleRate, speed);
}

// static
status_t AudioTrack::getMinFrameCount(
        size_t* frameCount,
        audio_stream_type_t streamType,
        uint32_t sampleRate)
{
    if (frameCount == NULL) {
        return BAD_VALUE;
    }

    // FIXME handle in server, like createTrack_l(), possible missing info:
    //          audio_io_handle_t output
    //          audio_format_t format
    //          audio_channel_mask_t channelMask
    //          audio_output_flags_t flags (FAST)
    uint32_t afSampleRate;
    status_t status;
    status = AudioSystem::getOutputSamplingRate(&afSampleRate, streamType);
    if (status != NO_ERROR) {
        ALOGE("Unable to query output sample rate for stream type %d; status %d",
                streamType, status);
        return status;
    }
    size_t afFrameCount;
    status = AudioSystem::getOutputFrameCount(&afFrameCount, streamType);
    if (status != NO_ERROR) {
        ALOGE("Unable to query output frame count for stream type %d; status %d",
                streamType, status);
        return status;
    }
    uint32_t afLatency;
    status = AudioSystem::getOutputLatency(&afLatency, streamType);
    if (status != NO_ERROR) {
        ALOGE("Unable to query output latency for stream type %d; status %d",
                streamType, status);
        return status;
    }

    // When called from createTrack, speed is 1.0f (normal speed).
    // This is rechecked again on setting playback rate (TODO: on setting sample rate, too).
    *frameCount = calculateMinFrameCount(afLatency, afFrameCount, afSampleRate, sampleRate, 1.0f);

    // The formula above should always produce a non-zero value under normal circumstances:
    // AudioTrack.SAMPLE_RATE_HZ_MIN <= sampleRate <= AudioTrack.SAMPLE_RATE_HZ_MAX.
    // Return error in the unlikely event that it does not, as that's part of the API contract.
    if (*frameCount == 0) {
        ALOGE("AudioTrack::getMinFrameCount failed for streamType %d, sampleRate %u",
                streamType, sampleRate);
        return BAD_VALUE;
    }
    ALOGV("getMinFrameCount=%zu: afFrameCount=%zu, afSampleRate=%u, afLatency=%u",
            *frameCount, afFrameCount, afSampleRate, afLatency);
    return NO_ERROR;
}

// ---------------------------------------------------------------------------

AudioTrack::AudioTrack()
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mPausedPosition(0),
      mSelectedDeviceId(AUDIO_PORT_HANDLE_NONE),
      mPlaybackRateSet(false)
{
    mAttributes.content_type = AUDIO_CONTENT_TYPE_UNKNOWN;
    mAttributes.usage = AUDIO_USAGE_UNKNOWN;
    mAttributes.flags = 0x0;
    strcpy(mAttributes.tags, "");
}

AudioTrack::AudioTrack(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        uint32_t notificationFrames,
        int sessionId,
        transfer_type transferType,
        const audio_offload_info_t *offloadInfo,
        int uid,
        pid_t pid,
        const audio_attributes_t* pAttributes,
        bool doNotReconnect)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mPausedPosition(0),
      mSelectedDeviceId(AUDIO_PORT_HANDLE_NONE),
      mPlaybackRateSet(false)
{
    mStatus = set(streamType, sampleRate, format, channelMask,
            frameCount, flags, cbf, user, notificationFrames,
            0 /*sharedBuffer*/, false /*threadCanCallJava*/, sessionId, transferType,
            offloadInfo, uid, pid, pAttributes, doNotReconnect);
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
        uint32_t notificationFrames,
        int sessionId,
        transfer_type transferType,
        const audio_offload_info_t *offloadInfo,
        int uid,
        pid_t pid,
        const audio_attributes_t* pAttributes,
        bool doNotReconnect)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mPausedPosition(0),
      mSelectedDeviceId(AUDIO_PORT_HANDLE_NONE),
      mPlaybackRateSet(false)
{
    mStatus = set(streamType, sampleRate, format, channelMask,
            0 /*frameCount*/, flags, cbf, user, notificationFrames,
            sharedBuffer, false /*threadCanCallJava*/, sessionId, transferType, offloadInfo,
            uid, pid, pAttributes, doNotReconnect);
}

AudioTrack::~AudioTrack()
{
    if (mStatus == NO_ERROR) {
        // Make sure that callback function exits in the case where
        // it is looping on buffer full condition in obtainBuffer().
        // Otherwise the callback thread will never exit.
        stop();
        if (mAudioTrackThread != 0) {
            mProxy->interrupt();
            mAudioTrackThread->requestExit();   // see comment in AudioTrack.h
            mAudioTrackThread->requestExitAndWait();
            mAudioTrackThread.clear();
        }
        // No lock here: worst case we remove a NULL callback which will be a nop
        if (mDeviceCallback != 0 && mOutput != AUDIO_IO_HANDLE_NONE) {
            AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mOutput);
        }
        IInterface::asBinder(mAudioTrack)->unlinkToDeath(mDeathNotifier, this);
        mAudioTrack.clear();
        mCblkMemory.clear();
        mSharedBuffer.clear();
        IPCThreadState::self()->flushCommands();
        ALOGV("~AudioTrack, releasing session id %d from %d on behalf of %d",
                mSessionId, IPCThreadState::self()->getCallingPid(), mClientPid);
        AudioSystem::releaseAudioSessionId(mSessionId, mClientPid);
    }
}

status_t AudioTrack::set(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        audio_output_flags_t flags,
        callback_t cbf,
        void* user,
        uint32_t notificationFrames,
        const sp<IMemory>& sharedBuffer,
        bool threadCanCallJava,
        int sessionId,
        transfer_type transferType,
        const audio_offload_info_t *offloadInfo,
        int uid,
        pid_t pid,
        const audio_attributes_t* pAttributes,
        bool doNotReconnect)
{
    ALOGV("set(): streamType %d, sampleRate %u, format %#x, channelMask %#x, frameCount %zu, "
          "flags #%x, notificationFrames %u, sessionId %d, transferType %d, uid %d, pid %d",
          streamType, sampleRate, format, channelMask, frameCount, flags, notificationFrames,
          sessionId, transferType, uid, pid);

    switch (transferType) {
    case TRANSFER_DEFAULT:
        if (sharedBuffer != 0) {
            transferType = TRANSFER_SHARED;
        } else if (cbf == NULL || threadCanCallJava) {
            transferType = TRANSFER_SYNC;
        } else {
            transferType = TRANSFER_CALLBACK;
        }
        break;
    case TRANSFER_CALLBACK:
        if (cbf == NULL || sharedBuffer != 0) {
            ALOGE("Transfer type TRANSFER_CALLBACK but cbf == NULL || sharedBuffer != 0");
            return BAD_VALUE;
        }
        break;
    case TRANSFER_OBTAIN:
    case TRANSFER_SYNC:
        if (sharedBuffer != 0) {
            ALOGE("Transfer type TRANSFER_OBTAIN but sharedBuffer != 0");
            return BAD_VALUE;
        }
        break;
    case TRANSFER_SHARED:
        if (sharedBuffer == 0) {
            ALOGE("Transfer type TRANSFER_SHARED but sharedBuffer == 0");
            return BAD_VALUE;
        }
        break;
    default:
        ALOGE("Invalid transfer type %d", transferType);
        return BAD_VALUE;
    }
    mSharedBuffer = sharedBuffer;
    mTransfer = transferType;
    mDoNotReconnect = doNotReconnect;

    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %zu", sharedBuffer->pointer(),
            sharedBuffer->size());

    ALOGV("set() streamType %d frameCount %zu flags %04x", streamType, frameCount, flags);

    // invariant that mAudioTrack != 0 is true only after set() returns successfully
    if (mAudioTrack != 0) {
        ALOGE("Track already in use");
        return INVALID_OPERATION;
    }

    // handle default values first.
    if (streamType == AUDIO_STREAM_DEFAULT) {
        streamType = AUDIO_STREAM_MUSIC;
    }
    if (pAttributes == NULL) {
        if (uint32_t(streamType) >= AUDIO_STREAM_PUBLIC_CNT) {
            ALOGE("Invalid stream type %d", streamType);
            return BAD_VALUE;
        }
        mStreamType = streamType;

    } else {
        // stream type shouldn't be looked at, this track has audio attributes
        memcpy(&mAttributes, pAttributes, sizeof(audio_attributes_t));
        ALOGV("Building AudioTrack with attributes: usage=%d content=%d flags=0x%x tags=[%s]",
                mAttributes.usage, mAttributes.content_type, mAttributes.flags, mAttributes.tags);
        mStreamType = AUDIO_STREAM_DEFAULT;
        if ((mAttributes.flags & AUDIO_FLAG_HW_AV_SYNC) != 0) {
            flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
        }
    }

    // these below should probably come from the audioFlinger too...
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }

    // validate parameters
    if (!audio_is_valid_format(format)) {
        ALOGE("Invalid format %#x", format);
        return BAD_VALUE;
    }
    mFormat = format;

    if (!audio_is_output_channel(channelMask)) {
        ALOGE("Invalid channel mask %#x", channelMask);
        return BAD_VALUE;
    }
    mChannelMask = channelMask;
    uint32_t channelCount = audio_channel_count_from_out_mask(channelMask);
    mChannelCount = channelCount;

    // force direct flag if format is not linear PCM
    // or offload was requested
    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
            || !audio_is_linear_pcm(format)) {
        ALOGV( (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)
                    ? "Offload request, forcing to Direct Output"
                    : "Not linear PCM, forcing to Direct Output");
        flags = (audio_output_flags_t)
                // FIXME why can't we allow direct AND fast?
                ((flags | AUDIO_OUTPUT_FLAG_DIRECT) & ~AUDIO_OUTPUT_FLAG_FAST);
    }

    // force direct flag if HW A/V sync requested
    if ((flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }

    if (flags & AUDIO_OUTPUT_FLAG_DIRECT) {
        if (audio_is_linear_pcm(format)) {
            mFrameSize = channelCount * audio_bytes_per_sample(format);
        } else {
            mFrameSize = sizeof(uint8_t);
        }
    } else {
        ALOG_ASSERT(audio_is_linear_pcm(format));
        mFrameSize = channelCount * audio_bytes_per_sample(format);
        // createTrack will return an error if PCM format is not supported by server,
        // so no need to check for specific PCM formats here
    }

    // sampling rate must be specified for direct outputs
    if (sampleRate == 0 && (flags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) {
        return BAD_VALUE;
    }
    mSampleRate = sampleRate;
    mOriginalSampleRate = sampleRate;
    mPlaybackRate = AUDIO_PLAYBACK_RATE_DEFAULT;

    // Make copy of input parameter offloadInfo so that in the future:
    //  (a) createTrack_l doesn't need it as an input parameter
    //  (b) we can support re-creation of offloaded tracks
    if (offloadInfo != NULL) {
        mOffloadInfoCopy = *offloadInfo;
        mOffloadInfo = &mOffloadInfoCopy;
    } else {
        mOffloadInfo = NULL;
    }

    mVolume[AUDIO_INTERLEAVE_LEFT] = 1.0f;
    mVolume[AUDIO_INTERLEAVE_RIGHT] = 1.0f;
    mSendLevel = 0.0f;
    // mFrameCount is initialized in createTrack_l
    mReqFrameCount = frameCount;
    mNotificationFramesReq = notificationFrames;
    mNotificationFramesAct = 0;
    if (sessionId == AUDIO_SESSION_ALLOCATE) {
        mSessionId = AudioSystem::newAudioUniqueId();
    } else {
        mSessionId = sessionId;
    }
    int callingpid = IPCThreadState::self()->getCallingPid();
    int mypid = getpid();
    if (uid == -1 || (callingpid != mypid)) {
        mClientUid = IPCThreadState::self()->getCallingUid();
    } else {
        mClientUid = uid;
    }
    if (pid == -1 || (callingpid != mypid)) {
        mClientPid = callingpid;
    } else {
        mClientPid = pid;
    }
    mAuxEffectId = 0;
    mFlags = flags;
    mCbf = cbf;

    if (cbf != NULL) {
        mAudioTrackThread = new AudioTrackThread(*this, threadCanCallJava);
        mAudioTrackThread->run("AudioTrack", ANDROID_PRIORITY_AUDIO, 0 /*stack*/);
        // thread begins in paused state, and will not reference us until start()
    }

    // create the IAudioTrack
    status_t status = createTrack_l();

    if (status != NO_ERROR) {
        if (mAudioTrackThread != 0) {
            mAudioTrackThread->requestExit();   // see comment in AudioTrack.h
            mAudioTrackThread->requestExitAndWait();
            mAudioTrackThread.clear();
        }
        return status;
    }

    mStatus = NO_ERROR;
    mState = STATE_STOPPED;
    mUserData = user;
    mLoopCount = 0;
    mLoopStart = 0;
    mLoopEnd = 0;
    mLoopCountNotified = 0;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    mPosition = 0;
    mReleased = 0;
    mStartUs = 0;
    AudioSystem::acquireAudioSessionId(mSessionId, mClientPid);
    mSequence = 1;
    mObservedSequence = mSequence;
    mInUnderrun = false;
    mPreviousTimestampValid = false;
    mTimestampStartupGlitchReported = false;
    mRetrogradeMotionReported = false;

    return NO_ERROR;
}

// -------------------------------------------------------------------------

status_t AudioTrack::start()
{
    AutoMutex lock(mLock);

    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }

    mInUnderrun = true;

    State previousState = mState;
    if (previousState == STATE_PAUSED_STOPPING) {
        mState = STATE_STOPPING;
    } else {
        mState = STATE_ACTIVE;
    }
    (void) updateAndGetPosition_l();
    if (previousState == STATE_STOPPED || previousState == STATE_FLUSHED) {
        // reset current position as seen by client to 0
        mPosition = 0;
        mPreviousTimestampValid = false;
        mTimestampStartupGlitchReported = false;
        mRetrogradeMotionReported = false;

        // If previousState == STATE_STOPPED, we reactivate markers (mMarkerPosition != 0)
        // as the position is reset to 0. This is legacy behavior. This is not done
        // in stop() to avoid a race condition where the last marker event is issued twice.
        // Note: the if is technically unnecessary because previousState == STATE_FLUSHED
        // is only for streaming tracks, and mMarkerReached is already set to false.
        if (previousState == STATE_STOPPED) {
            mMarkerReached = false;
        }

        // For offloaded tracks, we don't know if the hardware counters are really zero here,
        // since the flush is asynchronous and stop may not fully drain.
        // We save the time when the track is started to later verify whether
        // the counters are realistic (i.e. start from zero after this time).
        mStartUs = getNowUs();

        // force refresh of remaining frames by processAudioBuffer() as last
        // write before stop could be partial.
        mRefreshRemaining = true;

       // for static track, clear the old flags when start from stopped state
       if (mSharedBuffer != 0)
           android_atomic_and(
           ~(CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL | CBLK_BUFFER_END),
           &mCblk->mFlags);
    }
    mNewPosition = mPosition + mUpdatePeriod;
    int32_t flags = android_atomic_and(~CBLK_DISABLED, &mCblk->mFlags);

    sp<AudioTrackThread> t = mAudioTrackThread;
    if (t != 0) {
        if (previousState == STATE_STOPPING) {
            mProxy->interrupt();
        } else {
            t->resume();
        }
    } else {
        mPreviousPriority = getpriority(PRIO_PROCESS, 0);
        get_sched_policy(0, &mPreviousSchedulingGroup);
        androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
    }

    status_t status = NO_ERROR;
    if (!(flags & CBLK_INVALID)) {
        status = mAudioTrack->start();
        if (status == DEAD_OBJECT) {
            flags |= CBLK_INVALID;
        }
    }
    if (flags & CBLK_INVALID) {
        status = restoreTrack_l("start");
    }

    if (status != NO_ERROR) {
        ALOGE("start() status %d", status);
        mState = previousState;
        if (t != 0) {
            if (previousState != STATE_STOPPING) {
                t->pause();
            }
        } else {
            setpriority(PRIO_PROCESS, 0, mPreviousPriority);
            set_sched_policy(0, mPreviousSchedulingGroup);
        }
    }

    return status;
}

void AudioTrack::stop()
{
    AutoMutex lock(mLock);
    if (mState != STATE_ACTIVE && mState != STATE_PAUSED) {
        return;
    }

    if (isOffloaded_l()) {
        mState = STATE_STOPPING;
    } else {
        mState = STATE_STOPPED;
        mReleased = 0;
    }

    mProxy->interrupt();
    mAudioTrack->stop();

    // Note: legacy handling - stop does not clear playback marker
    // and periodic update counter, but flush does for streaming tracks.

    if (mSharedBuffer != 0) {
        // clear buffer position and loop count.
        mStaticProxy->setBufferPositionAndLoop(0 /* position */,
                0 /* loopStart */, 0 /* loopEnd */, 0 /* loopCount */);
    }

    sp<AudioTrackThread> t = mAudioTrackThread;
    if (t != 0) {
        if (!isOffloaded_l()) {
            t->pause();
        }
    } else {
        setpriority(PRIO_PROCESS, 0, mPreviousPriority);
        set_sched_policy(0, mPreviousSchedulingGroup);
    }
}

bool AudioTrack::stopped() const
{
    AutoMutex lock(mLock);
    return mState != STATE_ACTIVE;
}

void AudioTrack::flush()
{
    if (mSharedBuffer != 0) {
        return;
    }
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE || mState == STATE_FLUSHED) {
        return;
    }
    flush_l();
}

void AudioTrack::flush_l()
{
    ALOG_ASSERT(mState != STATE_ACTIVE);

    // clear playback marker and periodic update counter
    mMarkerPosition = 0;
    mMarkerReached = false;
    mUpdatePeriod = 0;
    mRefreshRemaining = true;

    mState = STATE_FLUSHED;
    mReleased = 0;
    if (isOffloaded_l()) {
        mProxy->interrupt();
    }
    mProxy->flush();
    mAudioTrack->flush();
}

void AudioTrack::pause()
{
    AutoMutex lock(mLock);
    if (mState == STATE_ACTIVE) {
        mState = STATE_PAUSED;
    } else if (mState == STATE_STOPPING) {
        mState = STATE_PAUSED_STOPPING;
    } else {
        return;
    }
    mProxy->interrupt();
    mAudioTrack->pause();

    if (isOffloaded_l()) {
        if (mOutput != AUDIO_IO_HANDLE_NONE) {
            // An offload output can be re-used between two audio tracks having
            // the same configuration. A timestamp query for a paused track
            // while the other is running would return an incorrect time.
            // To fix this, cache the playback position on a pause() and return
            // this time when requested until the track is resumed.

            // OffloadThread sends HAL pause in its threadLoop. Time saved
            // here can be slightly off.

            // TODO: check return code for getRenderPosition.

            uint32_t halFrames;
            AudioSystem::getRenderPosition(mOutput, &halFrames, &mPausedPosition);
            ALOGV("AudioTrack::pause for offload, cache current position %u", mPausedPosition);
        }
    }
}

status_t AudioTrack::setVolume(float left, float right)
{
    // This duplicates a test by AudioTrack JNI, but that is not the only caller
    if (isnanf(left) || left < GAIN_FLOAT_ZERO || left > GAIN_FLOAT_UNITY ||
            isnanf(right) || right < GAIN_FLOAT_ZERO || right > GAIN_FLOAT_UNITY) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mVolume[AUDIO_INTERLEAVE_LEFT] = left;
    mVolume[AUDIO_INTERLEAVE_RIGHT] = right;

    mProxy->setVolumeLR(gain_minifloat_pack(gain_from_float(left), gain_from_float(right)));

    if (isOffloaded_l() && mAudioTrack != NULL) {
        mAudioTrack->signal();
    }
    return NO_ERROR;
}

status_t AudioTrack::setVolume(float volume)
{
    return setVolume(volume, volume);
}

status_t AudioTrack::setAuxEffectSendLevel(float level)
{
    // This duplicates a test by AudioTrack JNI, but that is not the only caller
    if (isnanf(level) || level < GAIN_FLOAT_ZERO || level > GAIN_FLOAT_UNITY) {
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
        *level = mSendLevel;
    }
}

status_t AudioTrack::setSampleRate(uint32_t rate)
{
    AutoMutex lock(mLock);
    if (rate == mSampleRate) {
        return NO_ERROR;
    }
    if (mIsTimed || isOffloadedOrDirect_l() || (mFlags & AUDIO_OUTPUT_FLAG_FAST)) {
        return INVALID_OPERATION;
    }
    if (mOutput == AUDIO_IO_HANDLE_NONE) {
        return NO_INIT;
    }
    // NOTE: it is theoretically possible, but highly unlikely, that a device change
    // could mean a previously allowed sampling rate is no longer allowed.
    uint32_t afSamplingRate;
    if (AudioSystem::getSamplingRate(mOutput, &afSamplingRate) != NO_ERROR) {
        return NO_INIT;
    }
    // pitch is emulated by adjusting speed and sampleRate
    const uint32_t effectiveSampleRate = adjustSampleRate(rate, mPlaybackRate.mPitch);
    if (rate == 0 || effectiveSampleRate > afSamplingRate * AUDIO_RESAMPLER_DOWN_RATIO_MAX) {
        return BAD_VALUE;
    }
    // TODO: Should we also check if the buffer size is compatible?

    mSampleRate = rate;
    mProxy->setSampleRate(effectiveSampleRate);

    return NO_ERROR;
}

uint32_t AudioTrack::getSampleRate() const
{
    if (mIsTimed) {
        return 0;
    }

    AutoMutex lock(mLock);

    // sample rate can be updated during playback by the offloaded decoder so we need to
    // query the HAL and update if needed.
// FIXME use Proxy return channel to update the rate from server and avoid polling here
    if (isOffloadedOrDirect_l()) {
        if (mOutput != AUDIO_IO_HANDLE_NONE) {
            uint32_t sampleRate = 0;
            status_t status = AudioSystem::getSamplingRate(mOutput, &sampleRate);
            if (status == NO_ERROR) {
                mSampleRate = sampleRate;
            }
        }
    }
    return mSampleRate;
}

uint32_t AudioTrack::getOriginalSampleRate() const
{
    if (mIsTimed) {
        return 0;
    }

    return mOriginalSampleRate;
}

status_t AudioTrack::setPlaybackRate(const AudioPlaybackRate &playbackRate)
{
    AutoMutex lock(mLock);
    if (isAudioPlaybackRateEqual(playbackRate, mPlaybackRate)) {
        return NO_ERROR;
    }
    if (mIsTimed || isOffloadedOrDirect_l()) {
        return INVALID_OPERATION;
    }
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        return INVALID_OPERATION;
    }
    // pitch is emulated by adjusting speed and sampleRate
    const uint32_t effectiveRate = adjustSampleRate(mSampleRate, playbackRate.mPitch);
    const float effectiveSpeed = adjustSpeed(playbackRate.mSpeed, playbackRate.mPitch);
    const float effectivePitch = adjustPitch(playbackRate.mPitch);
    AudioPlaybackRate playbackRateTemp = playbackRate;
    playbackRateTemp.mSpeed = effectiveSpeed;
    playbackRateTemp.mPitch = effectivePitch;

    if (!isAudioPlaybackRateValid(playbackRateTemp)) {
        return BAD_VALUE;
    }
    // Check if the buffer size is compatible.
    if (!isSampleRateSpeedAllowed_l(effectiveRate, effectiveSpeed)) {
        ALOGV("setPlaybackRate(%f, %f) failed", playbackRate.mSpeed, playbackRate.mPitch);
        return BAD_VALUE;
    }

    // Check resampler ratios are within bounds
    if ((uint64_t)effectiveRate > (uint64_t)mSampleRate * (uint64_t)AUDIO_RESAMPLER_DOWN_RATIO_MAX) {
        ALOGV("setPlaybackRate(%f, %f) failed. Resample rate exceeds max accepted value",
                playbackRate.mSpeed, playbackRate.mPitch);
        return BAD_VALUE;
    }

    if ((uint64_t)effectiveRate * (uint64_t)AUDIO_RESAMPLER_UP_RATIO_MAX < (uint64_t)mSampleRate) {
        ALOGV("setPlaybackRate(%f, %f) failed. Resample rate below min accepted value",
                        playbackRate.mSpeed, playbackRate.mPitch);
        return BAD_VALUE;
    }
    mPlaybackRate = playbackRate;
    //set effective rates
    mProxy->setPlaybackRate(playbackRateTemp);
    mProxy->setSampleRate(effectiveRate); // FIXME: not quite "atomic" with setPlaybackRate

    // fallback out of Direct PCM if setPlaybackRate is called on a track offloaded
    // session. Do this by setting mPlaybackRateSet to true
    if (mTrackOffloaded) {
        mPlaybackRateSet = true;
        android_atomic_or(CBLK_INVALID, &mCblk->mFlags);
    }

    return NO_ERROR;
}

const AudioPlaybackRate& AudioTrack::getPlaybackRate() const
{
    AutoMutex lock(mLock);
    return mPlaybackRate;
}

status_t AudioTrack::setLoop(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    if (mSharedBuffer == 0 || mIsTimed || isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }

    if (loopCount == 0) {
        ;
    } else if (loopCount >= -1 && loopStart < loopEnd && loopEnd <= mFrameCount &&
            loopEnd - loopStart >= MIN_LOOP) {
        ;
    } else {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    // See setPosition() regarding setting parameters such as loop points or position while active
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    setLoop_l(loopStart, loopEnd, loopCount);
    return NO_ERROR;
}

void AudioTrack::setLoop_l(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    // We do not update the periodic notification point.
    // mNewPosition = updateAndGetPosition_l() + mUpdatePeriod;
    mLoopCount = loopCount;
    mLoopEnd = loopEnd;
    mLoopStart = loopStart;
    mLoopCountNotified = loopCount;
    mStaticProxy->setLoop(loopStart, loopEnd, loopCount);

    // Waking the AudioTrackThread is not needed as this cannot be called when active.
}

status_t AudioTrack::setMarkerPosition(uint32_t marker)
{
    // The only purpose of setting marker position is to get a callback
    if (mCbf == NULL || isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);
    mMarkerPosition = marker;
    mMarkerReached = false;

    sp<AudioTrackThread> t = mAudioTrackThread;
    if (t != 0) {
        t->wake();
    }
    return NO_ERROR;
}

status_t AudioTrack::getMarkerPosition(uint32_t *marker) const
{
    if (isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }
    if (marker == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    *marker = mMarkerPosition;

    return NO_ERROR;
}

status_t AudioTrack::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    // The only purpose of setting position update period is to get a callback
    if (mCbf == NULL || isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);
    mNewPosition = updateAndGetPosition_l() + updatePeriod;
    mUpdatePeriod = updatePeriod;

    sp<AudioTrackThread> t = mAudioTrackThread;
    if (t != 0) {
        t->wake();
    }
    return NO_ERROR;
}

status_t AudioTrack::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }
    if (updatePeriod == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioTrack::setPosition(uint32_t position)
{
    if (mSharedBuffer == 0 || mIsTimed || isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }
    if (position > mFrameCount) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    // Currently we require that the player is inactive before setting parameters such as position
    // or loop points.  Otherwise, there could be a race condition: the application could read the
    // current position, compute a new position or loop parameters, and then set that position or
    // loop parameters but it would do the "wrong" thing since the position has continued to advance
    // in the mean time.  If we ever provide a sequencer in server, we could allow a way for the app
    // to specify how it wants to handle such scenarios.
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    // After setting the position, use full update period before notification.
    mNewPosition = updateAndGetPosition_l() + mUpdatePeriod;
    mStaticProxy->setBufferPosition(position);

    // Waking the AudioTrackThread is not needed as this cannot be called when active.
    return NO_ERROR;
}

status_t AudioTrack::getPosition(uint32_t *position)
{
    if (position == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    if (isOffloadedOrDirect_l()) {
        uint32_t dspFrames = 0;

        if (isOffloaded_l() && ((mState == STATE_PAUSED) || (mState == STATE_PAUSED_STOPPING))) {
            ALOGV("getPosition called in paused state, return cached position %u", mPausedPosition);
            *position = mPausedPosition;
            return NO_ERROR;
        }

        if (AVMediaUtils::get()->AudioTrackIsPcmOffloaded(mFormat) &&
                AVMediaUtils::get()->AudioTrackGetPosition(this, position) == NO_ERROR) {
            return NO_ERROR;
        }

        if (mOutput != AUDIO_IO_HANDLE_NONE) {
            uint32_t halFrames; // actually unused
            status_t status = AudioSystem::getRenderPosition(mOutput, &halFrames, &dspFrames);
            if (status != NO_ERROR) {
                ALOGW("failed to getRenderPosition for offload session status %d", status);
                return INVALID_OPERATION;
            }
        }
        // FIXME: dspFrames may not be zero in (mState == STATE_STOPPED || mState == STATE_FLUSHED)
        // due to hardware latency. We leave this behavior for now.
        *position = dspFrames;
    } else {
        if (mCblk->mFlags & CBLK_INVALID) {
            (void) restoreTrack_l("getPosition");
            // FIXME: for compatibility with the Java API we ignore the restoreTrack_l()
            // error here (e.g. DEAD_OBJECT) and return OK with the last recorded server position.
        }

        // IAudioTrack::stop() isn't synchronous; we don't know when presentation completes
        *position = (mState == STATE_STOPPED || mState == STATE_FLUSHED) ?
                0 : updateAndGetPosition_l();
    }
    return NO_ERROR;
}

status_t AudioTrack::getBufferPosition(uint32_t *position)
{
    if (mSharedBuffer == 0 || mIsTimed) {
        return INVALID_OPERATION;
    }
    if (position == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    *position = mStaticProxy->getBufferPosition();
    return NO_ERROR;
}

status_t AudioTrack::reload()
{
    if (mSharedBuffer == 0 || mIsTimed || isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);
    // See setPosition() regarding setting parameters such as loop points or position while active
    if (mState == STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    mNewPosition = mUpdatePeriod;
    (void) updateAndGetPosition_l();
    mPosition = 0;
    mPreviousTimestampValid = false;
#if 0
    // The documentation is not clear on the behavior of reload() and the restoration
    // of loop count. Historically we have not restored loop count, start, end,
    // but it makes sense if one desires to repeat playing a particular sound.
    if (mLoopCount != 0) {
        mLoopCountNotified = mLoopCount;
        mStaticProxy->setLoop(mLoopStart, mLoopEnd, mLoopCount);
    }
#endif
    mStaticProxy->setBufferPosition(0);
    return NO_ERROR;
}

audio_io_handle_t AudioTrack::getOutput() const
{
    AutoMutex lock(mLock);
    return mOutput;
}

status_t AudioTrack::setOutputDevice(audio_port_handle_t deviceId) {
    AutoMutex lock(mLock);
    if (mSelectedDeviceId != deviceId) {
        mSelectedDeviceId = deviceId;
        android_atomic_or(CBLK_INVALID, &mCblk->mFlags);
    }
    return NO_ERROR;
}

audio_port_handle_t AudioTrack::getOutputDevice() {
    AutoMutex lock(mLock);
    return mSelectedDeviceId;
}

audio_port_handle_t AudioTrack::getRoutedDeviceId() {
    AutoMutex lock(mLock);
    if (mOutput == AUDIO_IO_HANDLE_NONE) {
        return AUDIO_PORT_HANDLE_NONE;
    }
    return AudioSystem::getDeviceIdForIo(mOutput);
}

status_t AudioTrack::attachAuxEffect(int effectId)
{
    AutoMutex lock(mLock);
    status_t status = mAudioTrack->attachAuxEffect(effectId);
    if (status == NO_ERROR) {
        mAuxEffectId = effectId;
    }
    return status;
}

audio_stream_type_t AudioTrack::streamType() const
{
    if (mStreamType == AUDIO_STREAM_DEFAULT) {
        return audio_attributes_to_stream_type(&mAttributes);
    }
    return mStreamType;
}

// -------------------------------------------------------------------------

// must be called with mLock held
status_t AudioTrack::createTrack_l()
{
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        ALOGE("Could not get audioflinger");
        return NO_INIT;
    }

    if (mDeviceCallback != 0 && mOutput != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mOutput);
    }
    audio_io_handle_t output;
    audio_stream_type_t streamType = mStreamType;
    audio_attributes_t *attr = (mStreamType == AUDIO_STREAM_DEFAULT) ? &mAttributes : NULL;

    audio_offload_info_t tOffloadInfo = AUDIO_INFO_INITIALIZER;
    if (mPlaybackRateSet == true && mOffloadInfo == NULL && mFormat == AUDIO_FORMAT_PCM_16_BIT) {
        mOffloadInfo = &tOffloadInfo;
    }
    status_t status = AudioSystem::getOutputForAttr(attr, &output,
                                           (audio_session_t)mSessionId, &streamType, mClientUid,
                                           mSampleRate, mFormat, mChannelMask,
                                           mFlags, mSelectedDeviceId, mOffloadInfo);
    //reset offload info if forced
    mOffloadInfo = (mOffloadInfo == &tOffloadInfo) ? NULL : mOffloadInfo;

    if (status != NO_ERROR || output == AUDIO_IO_HANDLE_NONE) {
        ALOGE("Could not get audio output for session %d, stream type %d, usage %d, sample rate %u, format %#x,"
              " channel mask %#x, flags %#x",
              mSessionId, streamType, mAttributes.usage, mSampleRate, mFormat, mChannelMask, mFlags);
        return BAD_VALUE;
    }
    mTrackOffloaded = AVMediaUtils::get()->AudioTrackIsTrackOffloaded(output);
    {
    // Now that we have a reference to an I/O handle and have not yet handed it off to AudioFlinger,
    // we must release it ourselves if anything goes wrong.

    // Not all of these values are needed under all conditions, but it is easier to get them all
    status = AudioSystem::getLatency(output, &mAfLatency);
    if (status != NO_ERROR) {
        ALOGE("getLatency(%d) failed status %d", output, status);
        goto release;
    }
    ALOGV("createTrack_l() output %d afLatency %u", output, mAfLatency);

    status = AudioSystem::getFrameCount(output, &mAfFrameCount);
    if (status != NO_ERROR) {
        ALOGE("getFrameCount(output=%d) status %d", output, status);
        goto release;
    }

    status = AudioSystem::getSamplingRate(output, &mAfSampleRate);
    if (status != NO_ERROR) {
        ALOGE("getSamplingRate(output=%d) status %d", output, status);
        goto release;
    }
    if (mSampleRate == 0) {
        mSampleRate = mAfSampleRate;
        mOriginalSampleRate = mAfSampleRate;
    }
    // Client decides whether the track is TIMED (see below), but can only express a preference
    // for FAST.  Server will perform additional tests.
    if ((mFlags & AUDIO_OUTPUT_FLAG_FAST) && !((
            // either of these use cases:
            // use case 1: shared buffer
            (mSharedBuffer != 0) ||
            // use case 2: callback transfer mode
            (mTransfer == TRANSFER_CALLBACK) ||
            // use case 3: obtain/release mode
            (mTransfer == TRANSFER_OBTAIN)) &&
            // matching sample rate
            (mSampleRate == mAfSampleRate))) {
        ALOGW("AUDIO_OUTPUT_FLAG_FAST denied by client; transfer %d, track %u Hz, output %u Hz",
                mTransfer, mSampleRate, mAfSampleRate);
        // once denied, do not request again if IAudioTrack is re-created
        mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_FAST);
    }

    // The client's AudioTrack buffer is divided into n parts for purpose of wakeup by server, where
    //  n = 1   fast track with single buffering; nBuffering is ignored
    //  n = 2   fast track with double buffering
    //  n = 2   normal track, (including those with sample rate conversion)
    //  n >= 3  very high latency or very small notification interval (unused).
    const uint32_t nBuffering = 2;

    mNotificationFramesAct = mNotificationFramesReq;

    size_t frameCount = mReqFrameCount;
    if (!audio_is_linear_pcm(mFormat)) {

        if (mSharedBuffer != 0) {
            // Same comment as below about ignoring frameCount parameter for set()
            frameCount = mSharedBuffer->size();
        } else if (frameCount == 0) {
            frameCount = mAfFrameCount;
            frameCount = AVMediaUtils::get()->AudioTrackGetOffloadFrameCount(frameCount);
        }
        if (mNotificationFramesAct != frameCount) {
            mNotificationFramesAct = frameCount;
        }
    } else if (mSharedBuffer != 0) {
        // FIXME: Ensure client side memory buffers need
        // not have additional alignment beyond sample
        // (e.g. 16 bit stereo accessed as 32 bit frame).
        size_t alignment = audio_bytes_per_sample(mFormat);
        if (alignment & 1) {
            // for AUDIO_FORMAT_PCM_24_BIT_PACKED (not exposed through Java).
            alignment = 1;
        }
        if (mChannelCount > 1) {
            // More than 2 channels does not require stronger alignment than stereo
            alignment <<= 1;
        }
        if (((uintptr_t)mSharedBuffer->pointer() & (alignment - 1)) != 0) {
            ALOGE("Invalid buffer alignment: address %p, channel count %u",
                    mSharedBuffer->pointer(), mChannelCount);
            status = BAD_VALUE;
            goto release;
        }

        // When initializing a shared buffer AudioTrack via constructors,
        // there's no frameCount parameter.
        // But when initializing a shared buffer AudioTrack via set(),
        // there _is_ a frameCount parameter.  We silently ignore it.
        frameCount = mSharedBuffer->size() / mFrameSize;
    } else {
        // For fast tracks the frame count calculations and checks are done by server

        if ((mFlags & AUDIO_OUTPUT_FLAG_FAST) == 0) {
            // for normal tracks precompute the frame count based on speed.
            const size_t minFrameCount = calculateMinFrameCount(
                    mAfLatency, mAfFrameCount, mAfSampleRate, mSampleRate,
                    mPlaybackRate.mSpeed);
            if (frameCount < minFrameCount) {
                frameCount = minFrameCount;
            }
        }
    }

    IAudioFlinger::track_flags_t trackFlags = IAudioFlinger::TRACK_DEFAULT;
    if (mIsTimed) {
        trackFlags |= IAudioFlinger::TRACK_TIMED;
    }

    pid_t tid = -1;
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        trackFlags |= IAudioFlinger::TRACK_FAST;
        if (mAudioTrackThread != 0) {
            tid = mAudioTrackThread->getTid();
        }
    }

    if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        trackFlags |= IAudioFlinger::TRACK_OFFLOAD;
    }

    if ((mFlags & AUDIO_OUTPUT_FLAG_DIRECT) || mTrackOffloaded) {
        trackFlags |= IAudioFlinger::TRACK_DIRECT;
    }

    size_t temp = frameCount;   // temp may be replaced by a revised value of frameCount,
                                // but we will still need the original value also
    int originalSessionId = mSessionId;
    sp<IAudioTrack> track = audioFlinger->createTrack(streamType,
                                                      mSampleRate,
                                                      mFormat,
                                                      mChannelMask,
                                                      &temp,
                                                      &trackFlags,
                                                      mSharedBuffer,
                                                      output,
                                                      tid,
                                                      &mSessionId,
                                                      mClientUid,
                                                      &status);
    ALOGE_IF(originalSessionId != AUDIO_SESSION_ALLOCATE && mSessionId != originalSessionId,
            "session ID changed from %d to %d", originalSessionId, mSessionId);

    if (status != NO_ERROR) {
        ALOGE("AudioFlinger could not create track, status: %d", status);
        goto release;
    }
    ALOG_ASSERT(track != 0);

    // AudioFlinger now owns the reference to the I/O handle,
    // so we are no longer responsible for releasing it.

    sp<IMemory> iMem = track->getCblk();
    if (iMem == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    void *iMemPointer = iMem->pointer();
    if (iMemPointer == NULL) {
        ALOGE("Could not get control block pointer");
        return NO_INIT;
    }
    // invariant that mAudioTrack != 0 is true only after set() returns successfully
    if (mAudioTrack != 0) {
        IInterface::asBinder(mAudioTrack)->unlinkToDeath(mDeathNotifier, this);
        mDeathNotifier.clear();
    }
    mAudioTrack = track;
    mCblkMemory = iMem;
    IPCThreadState::self()->flushCommands();

    audio_track_cblk_t* cblk = static_cast<audio_track_cblk_t*>(iMemPointer);
    mCblk = cblk;
    // note that temp is the (possibly revised) value of frameCount
    if (temp < frameCount || (frameCount == 0 && temp == 0)) {
        // In current design, AudioTrack client checks and ensures frame count validity before
        // passing it to AudioFlinger so AudioFlinger should not return a different value except
        // for fast track as it uses a special method of assigning frame count.
        ALOGW("Requested frameCount %zu but received frameCount %zu", frameCount, temp);
    }
    frameCount = temp;

    mAwaitBoost = false;
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        if (trackFlags & IAudioFlinger::TRACK_FAST) {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST successful; frameCount %zu", frameCount);
            mAwaitBoost = true;
        } else {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST denied by server; frameCount %zu", frameCount);
            // once denied, do not request again if IAudioTrack is re-created
            mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_FAST);
        }
    }
    if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        if (trackFlags & IAudioFlinger::TRACK_OFFLOAD) {
            ALOGV("AUDIO_OUTPUT_FLAG_OFFLOAD successful");
        } else {
            ALOGW("AUDIO_OUTPUT_FLAG_OFFLOAD denied by server");
            mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
            // FIXME This is a warning, not an error, so don't return error status
            //return NO_INIT;
        }
    }
    if (mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
        if (trackFlags & IAudioFlinger::TRACK_DIRECT) {
            ALOGV("AUDIO_OUTPUT_FLAG_DIRECT successful");
        } else {
            ALOGW("AUDIO_OUTPUT_FLAG_DIRECT denied by server");
            mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_DIRECT);
            // FIXME This is a warning, not an error, so don't return error status
            //return NO_INIT;
        }
    }
    // Make sure that application is notified with sufficient margin before underrun
    if (mSharedBuffer == 0 && audio_is_linear_pcm(mFormat)) {
        // Theoretically double-buffering is not required for fast tracks,
        // due to tighter scheduling.  But in practice, to accommodate kernels with
        // scheduling jitter, and apps with computation jitter, we use double-buffering
        // for fast tracks just like normal streaming tracks.
        if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount / nBuffering) {
            mNotificationFramesAct = frameCount / nBuffering;
        }
    }

    // We retain a copy of the I/O handle, but don't own the reference
    mOutput = output;
    mRefreshRemaining = true;

    // Starting address of buffers in shared memory.  If there is a shared buffer, buffers
    // is the value of pointer() for the shared buffer, otherwise buffers points
    // immediately after the control block.  This address is for the mapping within client
    // address space.  AudioFlinger::TrackBase::mBuffer is for the server address space.
    void* buffers;
    if (mSharedBuffer == 0) {
        buffers = cblk + 1;
    } else {
        buffers = mSharedBuffer->pointer();
        if (buffers == NULL) {
            ALOGE("Could not get buffer pointer");
            return NO_INIT;
        }
    }

    mAudioTrack->attachAuxEffect(mAuxEffectId);
    // FIXME doesn't take into account speed or future sample rate changes (until restoreTrack)
    // FIXME don't believe this lie
    mLatency = mAfLatency + (1000*frameCount) / mSampleRate;

    mFrameCount = frameCount;
    // If IAudioTrack is re-created, don't let the requested frameCount
    // decrease.  This can confuse clients that cache frameCount().
    if (frameCount > mReqFrameCount) {
        mReqFrameCount = frameCount;
    }

    // reset server position to 0 as we have new cblk.
    mServer = 0;

    // update proxy
    if (mSharedBuffer == 0) {
        mStaticProxy.clear();
        mProxy = new AudioTrackClientProxy(cblk, buffers, frameCount, mFrameSize);
    } else {
        mStaticProxy = new StaticAudioTrackClientProxy(cblk, buffers, frameCount, mFrameSize);
        mProxy = mStaticProxy;
    }

    mProxy->setVolumeLR(gain_minifloat_pack(
            gain_from_float(mVolume[AUDIO_INTERLEAVE_LEFT]),
            gain_from_float(mVolume[AUDIO_INTERLEAVE_RIGHT])));

    mProxy->setSendLevel(mSendLevel);
    const uint32_t effectiveSampleRate = adjustSampleRate(mSampleRate, mPlaybackRate.mPitch);
    const float effectiveSpeed = adjustSpeed(mPlaybackRate.mSpeed, mPlaybackRate.mPitch);
    const float effectivePitch = adjustPitch(mPlaybackRate.mPitch);
    mProxy->setSampleRate(effectiveSampleRate);

    AudioPlaybackRate playbackRateTemp = mPlaybackRate;
    playbackRateTemp.mSpeed = effectiveSpeed;
    playbackRateTemp.mPitch = effectivePitch;
    mProxy->setPlaybackRate(playbackRateTemp);
    mProxy->setMinimum(mNotificationFramesAct);

    mDeathNotifier = new DeathNotifier(this);
    IInterface::asBinder(mAudioTrack)->linkToDeath(mDeathNotifier, this);

    if (mDeviceCallback != 0) {
        AudioSystem::addAudioDeviceCallback(mDeviceCallback, mOutput);
    }

    return NO_ERROR;
    }

release:
    AudioSystem::releaseOutput(output, streamType, (audio_session_t)mSessionId);
    if (status == NO_ERROR) {
        status = NO_INIT;
    }
    return status;
}

status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, int32_t waitCount, size_t *nonContig)
{
    if (audioBuffer == NULL) {
        if (nonContig != NULL) {
            *nonContig = 0;
        }
        return BAD_VALUE;
    }
    if (mTransfer != TRANSFER_OBTAIN) {
        audioBuffer->frameCount = 0;
        audioBuffer->size = 0;
        audioBuffer->raw = NULL;
        if (nonContig != NULL) {
            *nonContig = 0;
        }
        return INVALID_OPERATION;
    }

    const struct timespec *requested;
    struct timespec timeout;
    if (waitCount == -1) {
        requested = &ClientProxy::kForever;
    } else if (waitCount == 0) {
        requested = &ClientProxy::kNonBlocking;
    } else if (waitCount > 0) {
        long long ms = WAIT_PERIOD_MS * (long long) waitCount;
        timeout.tv_sec = ms / 1000;
        timeout.tv_nsec = (int) (ms % 1000) * 1000000;
        requested = &timeout;
    } else {
        ALOGE("%s invalid waitCount %d", __func__, waitCount);
        requested = NULL;
    }
    return obtainBuffer(audioBuffer, requested, NULL /*elapsed*/, nonContig);
}

status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, const struct timespec *requested,
        struct timespec *elapsed, size_t *nonContig)
{
    // previous and new IAudioTrack sequence numbers are used to detect track re-creation
    uint32_t oldSequence = 0;
    uint32_t newSequence;

    Proxy::Buffer buffer;
    status_t status = NO_ERROR;

    static const int32_t kMaxTries = 5;
    int32_t tryCounter = kMaxTries;

    do {
        // obtainBuffer() is called with mutex unlocked, so keep extra references to these fields to
        // keep them from going away if another thread re-creates the track during obtainBuffer()
        sp<AudioTrackClientProxy> proxy;
        sp<IMemory> iMem;

        {   // start of lock scope
            AutoMutex lock(mLock);

            newSequence = mSequence;
            // did previous obtainBuffer() fail due to media server death or voluntary invalidation?
            if (status == DEAD_OBJECT) {
                // re-create track, unless someone else has already done so
                if (newSequence == oldSequence) {
                    status = restoreTrack_l("obtainBuffer");
                    if (status != NO_ERROR) {
                        buffer.mFrameCount = 0;
                        buffer.mRaw = NULL;
                        buffer.mNonContig = 0;
                        break;
                    }
                }
            }
            oldSequence = newSequence;

            // Keep the extra references
            proxy = mProxy;
            iMem = mCblkMemory;

            if (mState == STATE_STOPPING) {
                status = -EINTR;
                buffer.mFrameCount = 0;
                buffer.mRaw = NULL;
                buffer.mNonContig = 0;
                break;
            }

            // Non-blocking if track is stopped or paused
            if (mState != STATE_ACTIVE) {
                requested = &ClientProxy::kNonBlocking;
            }

        }   // end of lock scope

        buffer.mFrameCount = audioBuffer->frameCount;
        // FIXME starts the requested timeout and elapsed over from scratch
        status = proxy->obtainBuffer(&buffer, requested, elapsed);

    } while ((status == DEAD_OBJECT) && (tryCounter-- > 0));

    audioBuffer->frameCount = buffer.mFrameCount;
    audioBuffer->size = buffer.mFrameCount * mFrameSize;
    audioBuffer->raw = buffer.mRaw;
    if (nonContig != NULL) {
        *nonContig = buffer.mNonContig;
    }
    return status;
}

void AudioTrack::releaseBuffer(const Buffer* audioBuffer)
{
    // FIXME add error checking on mode, by adding an internal version
    if (mTransfer == TRANSFER_SHARED) {
        return;
    }

    size_t stepCount = audioBuffer->size / mFrameSize;
    if (stepCount == 0) {
        return;
    }

    Proxy::Buffer buffer;
    buffer.mFrameCount = stepCount;
    buffer.mRaw = audioBuffer->raw;

    AutoMutex lock(mLock);
    mReleased += stepCount;
    mInUnderrun = false;
    mProxy->releaseBuffer(&buffer);

    // restart track if it was disabled by audioflinger due to previous underrun
    if (mState == STATE_ACTIVE) {
        audio_track_cblk_t* cblk = mCblk;
        if (android_atomic_and(~CBLK_DISABLED, &cblk->mFlags) & CBLK_DISABLED) {
            ALOGW("releaseBuffer() track %p disabled due to previous underrun, restarting", this);
            // FIXME ignoring status
            mAudioTrack->start();
        }
    }
}

// -------------------------------------------------------------------------

ssize_t AudioTrack::write(const void* buffer, size_t userSize, bool blocking)
{
    if (mTransfer != TRANSFER_SYNC || mIsTimed) {
        return INVALID_OPERATION;
    }

    if (isDirect()) {
        AutoMutex lock(mLock);
        int32_t flags = android_atomic_and(
                            ~(CBLK_UNDERRUN | CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL | CBLK_BUFFER_END),
                            &mCblk->mFlags);
        if (flags & CBLK_INVALID) {
            return DEAD_OBJECT;
        }
    }

    if (ssize_t(userSize) < 0 || (buffer == NULL && userSize != 0)) {
        // Sanity-check: user is most-likely passing an error code, and it would
        // make the return value ambiguous (actualSize vs error).
        ALOGE("AudioTrack::write(buffer=%p, size=%zu (%zd)", buffer, userSize, userSize);
        return BAD_VALUE;
    }

    size_t written = 0;
    Buffer audioBuffer;

    while (userSize >= mFrameSize) {
        audioBuffer.frameCount = userSize / mFrameSize;

        status_t err = obtainBuffer(&audioBuffer,
                blocking ? &ClientProxy::kForever : &ClientProxy::kNonBlocking);
        if (err < 0) {
            if (written > 0) {
                break;
            }
            return ssize_t(err);
        }

        size_t toWrite = audioBuffer.size;
        memcpy(audioBuffer.i8, buffer, toWrite);
        buffer = ((const char *) buffer) + toWrite;
        userSize -= toWrite;
        written += toWrite;

        releaseBuffer(&audioBuffer);
    }

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

#if 1
    // acquire a strong reference on the IMemory and IAudioTrack so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioTrack> audioTrack = mAudioTrack;
    sp<IMemory> iMem = mCblkMemory;
#endif

    // If the track is not invalid already, try to allocate a buffer.  alloc
    // fails indicating that the server is dead, flag the track as invalid so
    // we can attempt to restore in just a bit.
    audio_track_cblk_t* cblk = mCblk;
    if (!(cblk->mFlags & CBLK_INVALID)) {
        result = mAudioTrack->allocateTimedBuffer(size, buffer);
        if (result == DEAD_OBJECT) {
            android_atomic_or(CBLK_INVALID, &cblk->mFlags);
        }
    }

    // If the track is invalid at this point, attempt to restore it. and try the
    // allocation one more time.
    if (cblk->mFlags & CBLK_INVALID) {
        result = restoreTrack_l("allocateTimedBuffer");

        if (result == NO_ERROR) {
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
                (mState == STATE_ACTIVE) && (cblk->mFlags & CBLK_DISABLED)) {
            android_atomic_and(~CBLK_DISABLED, &cblk->mFlags);
            ALOGW("queueTimedBuffer() track %p disabled, restarting", this);
            // FIXME ignoring status
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

nsecs_t AudioTrack::processAudioBuffer()
{
    // Currently the AudioTrack thread is not created if there are no callbacks.
    // Would it ever make sense to run the thread, even without callbacks?
    // If so, then replace this by checks at each use for mCbf != NULL.
    LOG_ALWAYS_FATAL_IF(mCblk == NULL);

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
        // Run again immediately
        return 0;
    }

    // Can only reference mCblk while locked
    int32_t flags = android_atomic_and(
        ~(CBLK_UNDERRUN | CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL | CBLK_BUFFER_END), &mCblk->mFlags);

    // Check for track invalidation
    if (flags & CBLK_INVALID) {
        // for offloaded tracks restoreTrack_l() will just update the sequence and clear
        // AudioSystem cache. We should not exit here but after calling the callback so
        // that the upper layers can recreate the track
        if (!isOffloadedOrDirect_l() || (mSequence == mObservedSequence)) {
            status_t status __unused = restoreTrack_l("processAudioBuffer");
            // FIXME unused status
            // after restoration, continue below to make sure that the loop and buffer events
            // are notified because they have been cleared from mCblk->mFlags above.
        }
    }

    bool waitStreamEnd = mState == STATE_STOPPING;
    bool active = mState == STATE_ACTIVE;

    // Manage underrun callback, must be done under lock to avoid race with releaseBuffer()
    bool newUnderrun = false;
    if (flags & CBLK_UNDERRUN) {
#if 0
        // Currently in shared buffer mode, when the server reaches the end of buffer,
        // the track stays active in continuous underrun state.  It's up to the application
        // to pause or stop the track, or set the position to a new offset within buffer.
        // This was some experimental code to auto-pause on underrun.   Keeping it here
        // in "if 0" so we can re-visit this if we add a real sequencer for shared memory content.
        if (mTransfer == TRANSFER_SHARED) {
            mState = STATE_PAUSED;
            active = false;
        }
#endif
        if (!mInUnderrun) {
            mInUnderrun = true;
            newUnderrun = true;
        }
    }

    // Get current position of server
    size_t position = updateAndGetPosition_l();

    // Manage marker callback
    bool markerReached = false;
    size_t markerPosition = mMarkerPosition;
    // FIXME fails for wraparound, need 64 bits
    if (!mMarkerReached && (markerPosition > 0) && (position >= markerPosition)) {
        mMarkerReached = markerReached = true;
    }

    // Determine number of new position callback(s) that will be needed, while locked
    size_t newPosCount = 0;
    size_t newPosition = mNewPosition;
    size_t updatePeriod = mUpdatePeriod;
    // FIXME fails for wraparound, need 64 bits
    if (updatePeriod > 0 && position >= newPosition) {
        newPosCount = ((position - newPosition) / updatePeriod) + 1;
        mNewPosition += updatePeriod * newPosCount;
    }

    // Cache other fields that will be needed soon
    uint32_t sampleRate = mSampleRate;
    float speed = mPlaybackRate.mSpeed;
    const uint32_t notificationFrames = mNotificationFramesAct;
    if (mRefreshRemaining) {
        mRefreshRemaining = false;
        mRemainingFrames = notificationFrames;
        mRetryOnPartialBuffer = false;
    }
    size_t misalignment = mProxy->getMisalignment();
    uint32_t sequence = mSequence;
    sp<AudioTrackClientProxy> proxy = mProxy;

    // Determine the number of new loop callback(s) that will be needed, while locked.
    int loopCountNotifications = 0;
    uint32_t loopPeriod = 0; // time in frames for next EVENT_LOOP_END or EVENT_BUFFER_END

    if (mLoopCount > 0) {
        int loopCount;
        size_t bufferPosition;
        mStaticProxy->getBufferPositionAndLoopCount(&bufferPosition, &loopCount);
        loopPeriod = ((loopCount > 0) ? mLoopEnd : mFrameCount) - bufferPosition;
        loopCountNotifications = min(mLoopCountNotified - loopCount, kMaxLoopCountNotifications);
        mLoopCountNotified = loopCount; // discard any excess notifications
    } else if (mLoopCount < 0) {
        // FIXME: We're not accurate with notification count and position with infinite looping
        // since loopCount from server side will always return -1 (we could decrement it).
        size_t bufferPosition = mStaticProxy->getBufferPosition();
        loopCountNotifications = int((flags & (CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL)) != 0);
        loopPeriod = mLoopEnd - bufferPosition;
    } else if (/* mLoopCount == 0 && */ mSharedBuffer != 0) {
        size_t bufferPosition = mStaticProxy->getBufferPosition();
        loopPeriod = mFrameCount - bufferPosition;
    }

    // These fields don't need to be cached, because they are assigned only by set():
    //     mTransfer, mCbf, mUserData, mFormat, mFrameSize, mFlags
    // mFlags is also assigned by createTrack_l(), but not the bit we care about.

    mLock.unlock();

    // get anchor time to account for callbacks.
    const nsecs_t timeBeforeCallbacks = systemTime();

    // perform callbacks while unlocked
    if (newUnderrun) {
        mCbf(EVENT_UNDERRUN, mUserData, NULL);
    }
    while (loopCountNotifications > 0) {
        mCbf(EVENT_LOOP_END, mUserData, NULL);
        --loopCountNotifications;
    }
    if (flags & CBLK_BUFFER_END) {
        mCbf(EVENT_BUFFER_END, mUserData, NULL);
    }
    if (markerReached) {
        mCbf(EVENT_MARKER, mUserData, &markerPosition);
    }
    while (newPosCount > 0) {
        size_t temp = newPosition;
        mCbf(EVENT_NEW_POS, mUserData, &temp);
        newPosition += updatePeriod;
        newPosCount--;
    }

    if (mObservedSequence != sequence) {
        mObservedSequence = sequence;
        mCbf(EVENT_NEW_IAUDIOTRACK, mUserData, NULL);
        // for offloaded tracks, just wait for the upper layers to recreate the track
        if (isOffloadedOrDirect()) {
            return NS_INACTIVE;
        }
    }

    if (waitStreamEnd) {
        // FIXME:  Instead of blocking in proxy->waitStreamEndDone(), Callback thread
        // should wait on proxy futex and handle CBLK_STREAM_END_DONE within this function
        // (and make sure we don't callback for more data while we're stopping).
        // This helps with position, marker notifications, and track invalidation.
        struct timespec timeout;
        timeout.tv_sec = WAIT_STREAM_END_TIMEOUT_SEC;
        timeout.tv_nsec = 0;

        status_t status = proxy->waitStreamEndDone(&timeout);
        switch (status) {
        case NO_ERROR:
        case DEAD_OBJECT:
        case TIMED_OUT:
            if (isOffloaded_l()) {
                if (mCblk->mFlags & (CBLK_INVALID)){
                    // will trigger EVENT_STREAM_END in next iteration
                    return 0;
                }
            }
            if (status != DEAD_OBJECT) {
                // for DEAD_OBJECT, we do not send a EVENT_STREAM_END after stop();
                // instead, the application should handle the EVENT_NEW_IAUDIOTRACK.
                mCbf(EVENT_STREAM_END, mUserData, NULL);
            }
            {
                AutoMutex lock(mLock);
                // The previously assigned value of waitStreamEnd is no longer valid,
                // since the mutex has been unlocked and either the callback handler
                // or another thread could have re-started the AudioTrack during that time.
                waitStreamEnd = mState == STATE_STOPPING;
                if (waitStreamEnd) {
                    mState = STATE_STOPPED;
                    mReleased = 0;
                }
            }
            if (waitStreamEnd && status != DEAD_OBJECT) {
               return NS_INACTIVE;
            }
            break;
        }
        return 0;
    }

    // if inactive, then don't run me again until re-started
    if (!active) {
        return NS_INACTIVE;
    }

    // Compute the estimated time until the next timed event (position, markers, loops)
    // FIXME only for non-compressed audio
    uint32_t minFrames = ~0;
    if (!markerReached && position < markerPosition) {
        minFrames = markerPosition - position;
    }
    if (loopPeriod > 0 && loopPeriod < minFrames) {
        // loopPeriod is already adjusted for actual position.
        minFrames = loopPeriod;
    }
    if (updatePeriod > 0) {
        minFrames = min(minFrames, uint32_t(newPosition - position));
    }

    // If > 0, poll periodically to recover from a stuck server.  A good value is 2.
    static const uint32_t kPoll = 0;
    if (kPoll > 0 && mTransfer == TRANSFER_CALLBACK && kPoll * notificationFrames < minFrames) {
        minFrames = kPoll * notificationFrames;
    }

    // This "fudge factor" avoids soaking CPU, and compensates for late progress by server
    static const nsecs_t kWaitPeriodNs = WAIT_PERIOD_MS * 1000000LL;
    const nsecs_t timeAfterCallbacks = systemTime();

    // Convert frame units to time units
    nsecs_t ns = NS_WHENEVER;
    if (minFrames != (uint32_t) ~0) {
        ns = framesToNanoseconds(minFrames, sampleRate, speed) + kWaitPeriodNs;
        ns -= (timeAfterCallbacks - timeBeforeCallbacks);  // account for callback time
        // TODO: Should we warn if the callback time is too long?
        if (ns < 0) ns = 0;
    }

    // If not supplying data by EVENT_MORE_DATA, then we're done
    if (mTransfer != TRANSFER_CALLBACK) {
        return ns;
    }

    // EVENT_MORE_DATA callback handling.
    // Timing for linear pcm audio data formats can be derived directly from the
    // buffer fill level.
    // Timing for compressed data is not directly available from the buffer fill level,
    // rather indirectly from waiting for blocking mode callbacks or waiting for obtain()
    // to return a certain fill level.

    struct timespec timeout;
    const struct timespec *requested = &ClientProxy::kForever;
    if (ns != NS_WHENEVER) {
        timeout.tv_sec = ns / 1000000000LL;
        timeout.tv_nsec = ns % 1000000000LL;
        ALOGV("timeout %ld.%03d", timeout.tv_sec, (int) timeout.tv_nsec / 1000000);
        requested = &timeout;
    }

    while (mRemainingFrames > 0) {

        Buffer audioBuffer;
        audioBuffer.frameCount = mRemainingFrames;
        size_t nonContig;
        status_t err = obtainBuffer(&audioBuffer, requested, NULL, &nonContig);
        LOG_ALWAYS_FATAL_IF((err != NO_ERROR) != (audioBuffer.frameCount == 0),
                "obtainBuffer() err=%d frameCount=%zu", err, audioBuffer.frameCount);
        requested = &ClientProxy::kNonBlocking;
        size_t avail = audioBuffer.frameCount + nonContig;
        ALOGV("obtainBuffer(%u) returned %zu = %zu + %zu err %d",
                mRemainingFrames, avail, audioBuffer.frameCount, nonContig, err);
        if (err != NO_ERROR) {
            if (err == TIMED_OUT || err == WOULD_BLOCK || err == -EINTR ||
                    (isOffloaded() && (err == DEAD_OBJECT))) {
                // FIXME bug 25195759
                return 1000000;
            }
            ALOGE("Error %d obtaining an audio buffer, giving up.", err);
            return NS_NEVER;
        }

        if (mRetryOnPartialBuffer && audio_is_linear_pcm(mFormat)) {
            mRetryOnPartialBuffer = false;
            if (avail < mRemainingFrames) {
                if (ns > 0) { // account for obtain time
                    const nsecs_t timeNow = systemTime();
                    ns = max((nsecs_t)0, ns - (timeNow - timeAfterCallbacks));
                }
                nsecs_t myns = framesToNanoseconds(mRemainingFrames - avail, sampleRate, speed);
                if (ns < 0 /* NS_WHENEVER */ || myns < ns) {
                    ns = myns;
                }
                return ns;
            }
        }

        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        size_t writtenSize = audioBuffer.size;

        // Sanity check on returned size
        if (ssize_t(writtenSize) < 0 || writtenSize > reqSize) {
            ALOGE("EVENT_MORE_DATA requested %zu bytes but callback returned %zd bytes",
                    reqSize, ssize_t(writtenSize));
            return NS_NEVER;
        }

        if (writtenSize == 0) {
            // The callback is done filling buffers
            // Keep this thread going to handle timed events and
            // still try to get more data in intervals of WAIT_PERIOD_MS
            // but don't just loop and block the CPU, so wait

            // mCbf(EVENT_MORE_DATA, ...) might either
            // (1) Block until it can fill the buffer, returning 0 size on EOS.
            // (2) Block until it can fill the buffer, returning 0 data (silence) on EOS.
            // (3) Return 0 size when no data is available, does not wait for more data.
            //
            // (1) and (2) occurs with AudioPlayer/AwesomePlayer; (3) occurs with NuPlayer.
            // We try to compute the wait time to avoid a tight sleep-wait cycle,
            // especially for case (3).
            //
            // The decision to support (1) and (2) affect the sizing of mRemainingFrames
            // and this loop; whereas for case (3) we could simply check once with the full
            // buffer size and skip the loop entirely.

            nsecs_t myns;
            if (audio_is_linear_pcm(mFormat)) {
                // time to wait based on buffer occupancy
                const nsecs_t datans = mRemainingFrames <= avail ? 0 :
                        framesToNanoseconds(mRemainingFrames - avail, sampleRate, speed);
                // audio flinger thread buffer size (TODO: adjust for fast tracks)
                const nsecs_t afns = framesToNanoseconds(mAfFrameCount, mAfSampleRate, speed);
                // add a half the AudioFlinger buffer time to avoid soaking CPU if datans is 0.
                myns = datans + (afns / 2);
            } else {
                // FIXME: This could ping quite a bit if the buffer isn't full.
                // Note that when mState is stopping we waitStreamEnd, so it never gets here.
                myns = kWaitPeriodNs;
            }
            if (ns > 0) { // account for obtain and callback time
                const nsecs_t timeNow = systemTime();
                ns = max((nsecs_t)0, ns - (timeNow - timeAfterCallbacks));
            }
            if (ns < 0 /* NS_WHENEVER */ || myns < ns) {
                ns = myns;
            }
            return ns;
        }

        size_t releasedFrames = writtenSize / mFrameSize;
        audioBuffer.frameCount = releasedFrames;
        mRemainingFrames -= releasedFrames;
        if (misalignment >= releasedFrames) {
            misalignment -= releasedFrames;
        } else {
            misalignment = 0;
        }

        releaseBuffer(&audioBuffer);

        // FIXME here is where we would repeat EVENT_MORE_DATA again on same advanced buffer
        // if callback doesn't like to accept the full chunk
        if (writtenSize < reqSize) {
            continue;
        }

        // There could be enough non-contiguous frames available to satisfy the remaining request
        if (mRemainingFrames <= nonContig) {
            continue;
        }

#if 0
        // This heuristic tries to collapse a series of EVENT_MORE_DATA that would total to a
        // sum <= notificationFrames.  It replaces that series by at most two EVENT_MORE_DATA
        // that total to a sum == notificationFrames.
        if (0 < misalignment && misalignment <= mRemainingFrames) {
            mRemainingFrames = misalignment;
            return ((double)mRemainingFrames * 1100000000) / ((double)sampleRate * speed);
        }
#endif

    }
    mRemainingFrames = notificationFrames;
    mRetryOnPartialBuffer = true;

    // A lot has transpired since ns was calculated, so run again immediately and re-calculate
    return 0;
}

status_t AudioTrack::restoreTrack_l(const char *from)
{
    ALOGW("dead IAudioTrack, %s, creating a new one from %s()",
          isOffloadedOrDirect_l() ? "Offloaded or Direct" : "PCM", from);
    ++mSequence;

    // refresh the audio configuration cache in this process to make sure we get new
    // output parameters and new IAudioFlinger in createTrack_l()
    AudioSystem::clearAudioConfigCache();

    if (isOffloadedOrDirect_l() || mDoNotReconnect) {
        // FIXME re-creation of offloaded and direct tracks is not yet implemented;
        // reconsider enabling for linear PCM encodings when position can be preserved.
        return DEAD_OBJECT;
    }

    // save the old static buffer position
    size_t bufferPosition = 0;
    int loopCount = 0;
    if (mStaticProxy != 0) {
        mStaticProxy->getBufferPositionAndLoopCount(&bufferPosition, &loopCount);
    }

    // If a new IAudioTrack is successfully created, createTrack_l() will modify the
    // following member variables: mAudioTrack, mCblkMemory and mCblk.
    // It will also delete the strong references on previous IAudioTrack and IMemory.
    // If a new IAudioTrack cannot be created, the previous (dead) instance will be left intact.
    status_t result = createTrack_l();

    if (result == NO_ERROR) {
        // take the frames that will be lost by track recreation into account in saved position
        // For streaming tracks, this is the amount we obtained from the user/client
        // (not the number actually consumed at the server - those are already lost).
        if (mStaticProxy == 0) {
            mPosition = mReleased;
        }
        // Continue playback from last known position and restore loop.
        if (mStaticProxy != 0) {
            if (loopCount != 0) {
                mStaticProxy->setBufferPositionAndLoop(bufferPosition,
                        mLoopStart, mLoopEnd, loopCount);
            } else {
                mStaticProxy->setBufferPosition(bufferPosition);
                if (bufferPosition == mFrameCount) {
                    ALOGD("restoring track at end of static buffer");
                }
            }
        }
        if (mState == STATE_ACTIVE) {
            result = mAudioTrack->start();
        }
    }
    if (result != NO_ERROR) {
        ALOGW("restoreTrack_l() failed status %d", result);
        mState = STATE_STOPPED;
        mReleased = 0;
    }

    return result;
}

uint32_t AudioTrack::updateAndGetPosition_l()
{
    // This is the sole place to read server consumed frames
    uint32_t newServer = mProxy->getPosition();
    uint32_t delta = newServer > mServer ? newServer - mServer : 0;
    // TODO There is controversy about whether there can be "negative jitter" in server position.
    //      This should be investigated further, and if possible, it should be addressed.
    //      A more definite failure mode is infrequent polling by client.
    //      One could call (void)getPosition_l() in releaseBuffer(),
    //      so mReleased and mPosition are always lock-step as best possible.
    //      That should ensure delta never goes negative for infrequent polling
    //      unless the server has more than 2^31 frames in its buffer,
    //      in which case the use of uint32_t for these counters has bigger issues.
    if (newServer < mServer) {
        ALOGE("detected illegal retrograde motion by the server: mServer advanced by %d",
              (int32_t) newServer - mServer);
    }
    mServer = newServer;
    return mPosition += delta;
}

bool AudioTrack::isSampleRateSpeedAllowed_l(uint32_t sampleRate, float speed) const
{
    // applicable for mixing tracks only (not offloaded or direct)
    if (mStaticProxy != 0) {
        return true; // static tracks do not have issues with buffer sizing.
    }
    const size_t minFrameCount =
            calculateMinFrameCount(mAfLatency, mAfFrameCount, mAfSampleRate, sampleRate, speed);
    ALOGV("isSampleRateSpeedAllowed_l mFrameCount %zu  minFrameCount %zu",
            mFrameCount, minFrameCount);
    return mFrameCount >= minFrameCount;
}

status_t AudioTrack::setParameters(const String8& keyValuePairs)
{
    AutoMutex lock(mLock);
    return mAudioTrack->setParameters(keyValuePairs);
}

status_t AudioTrack::getTimestamp(AudioTimestamp& timestamp)
{
    AutoMutex lock(mLock);

    bool previousTimestampValid = mPreviousTimestampValid;
    // Set false here to cover all the error return cases.
    mPreviousTimestampValid = false;

    // FIXME not implemented for fast tracks; should use proxy and SSQ
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        return INVALID_OPERATION;
    }

    switch (mState) {
    case STATE_ACTIVE:
    case STATE_PAUSED:
        break; // handle below
    case STATE_FLUSHED:
    case STATE_STOPPED:
        return WOULD_BLOCK;
    case STATE_STOPPING:
    case STATE_PAUSED_STOPPING:
        if (!isOffloaded_l()) {
            return INVALID_OPERATION;
        }
        break; // offloaded tracks handled below
    default:
        LOG_ALWAYS_FATAL("Invalid mState in getTimestamp(): %d", mState);
        break;
    }

    if (mCblk->mFlags & CBLK_INVALID) {
        const status_t status = restoreTrack_l("getTimestamp");
        if (status != OK) {
            // per getTimestamp() API doc in header, we return DEAD_OBJECT here,
            // recommending that the track be recreated.
            return DEAD_OBJECT;
        }
    }

    status_t status = UNKNOWN_ERROR;
    //call Timestamp only if its NOT PCM offloaded and NOT Track Offloaded
    if (!AVMediaUtils::get()->AudioTrackIsPcmOffloaded(mFormat) && !mTrackOffloaded) {
        // The presented frame count must always lag behind the consumed frame count.
        // To avoid a race, read the presented frames first.  This ensures that presented <= consumed.

        status = mAudioTrack->getTimestamp(timestamp);
        if (status != NO_ERROR) {
            ALOGV_IF(status != WOULD_BLOCK, "getTimestamp error:%#x", status);
            return status;
        }

    }

    if (isOffloadedOrDirect_l() && !AVMediaUtils::get()->AudioTrackIsPcmOffloaded(mFormat)
        && !mTrackOffloaded) {
        if (isOffloaded_l() && (mState == STATE_PAUSED || mState == STATE_PAUSED_STOPPING)) {
            // use cached paused position in case another offloaded track is running.
            timestamp.mPosition = mPausedPosition;
            clock_gettime(CLOCK_MONOTONIC, &timestamp.mTime);
            return NO_ERROR;
        }

        // Check whether a pending flush or stop has completed, as those commands may
        // be asynchronous or return near finish or exhibit glitchy behavior.
        //
        // Originally this showed up as the first timestamp being a continuation of
        // the previous song under gapless playback.
        // However, we sometimes see zero timestamps, then a glitch of
        // the previous song's position, and then correct timestamps afterwards.
        if (mStartUs != 0 && mSampleRate != 0) {
            static const int kTimeJitterUs = 100000; // 100 ms
            static const int k1SecUs = 1000000;

            const int64_t timeNow = getNowUs();

            if (timeNow < mStartUs + k1SecUs) { // within first second of starting
                const int64_t timestampTimeUs = convertTimespecToUs(timestamp.mTime);
                if (timestampTimeUs < mStartUs) {
                    return WOULD_BLOCK;  // stale timestamp time, occurs before start.
                }
                const int64_t deltaTimeUs = timestampTimeUs - mStartUs;
                const int64_t deltaPositionByUs = (double)timestamp.mPosition * 1000000
                        / ((double)mSampleRate * mPlaybackRate.mSpeed);

                if (deltaPositionByUs > deltaTimeUs + kTimeJitterUs) {
                    // Verify that the counter can't count faster than the sample rate
                    // since the start time.  If greater, then that means we may have failed
                    // to completely flush or stop the previous playing track.
                    ALOGW_IF(!mTimestampStartupGlitchReported,
                            "getTimestamp startup glitch detected"
                            " deltaTimeUs(%lld) deltaPositionUs(%lld) tsmPosition(%u)",
                            (long long)deltaTimeUs, (long long)deltaPositionByUs,
                            timestamp.mPosition);
                    mTimestampStartupGlitchReported = true;
                    if (previousTimestampValid
                            && mPreviousTimestamp.mPosition == 0 /* should be true if valid */) {
                        timestamp = mPreviousTimestamp;
                        mPreviousTimestampValid = true;
                        return NO_ERROR;
                    }
                    return WOULD_BLOCK;
                }
                if (deltaPositionByUs != 0) {
                    mStartUs = 0; // don't check again, we got valid nonzero position.
                }
            } else {
                mStartUs = 0; // don't check again, start time expired.
            }
            mTimestampStartupGlitchReported = false;
        }
    } else {
        // Update the mapping between local consumed (mPosition) and server consumed (mServer)

        if (AVMediaUtils::get()->AudioTrackGetTimestamp(this, &timestamp) == NO_ERROR) {
            return NO_ERROR;
        }

        (void) updateAndGetPosition_l();
        // Server consumed (mServer) and presented both use the same server time base,
        // and server consumed is always >= presented.
        // The delta between these represents the number of frames in the buffer pipeline.
        // If this delta between these is greater than the client position, it means that
        // actually presented is still stuck at the starting line (figuratively speaking),
        // waiting for the first frame to go by.  So we can't report a valid timestamp yet.
        if ((uint32_t) (mServer - timestamp.mPosition) > mPosition) {
            return INVALID_OPERATION;
        }
        // Convert timestamp position from server time base to client time base.
        // TODO The following code should work OK now because timestamp.mPosition is 32-bit.
        // But if we change it to 64-bit then this could fail.
        // Split this out instead of using += to prevent unsigned overflow
        // checks in the outer sum.
        timestamp.mPosition = timestamp.mPosition + static_cast<int32_t>(mPosition) - mServer;
        // Immediately after a call to getPosition_l(), mPosition and
        // mServer both represent the same frame position.  mPosition is
        // in client's point of view, and mServer is in server's point of
        // view.  So the difference between them is the "fudge factor"
        // between client and server views due to stop() and/or new
        // IAudioTrack.  And timestamp.mPosition is initially in server's
        // point of view, so we need to apply the same fudge factor to it.
    }

    // Prevent retrograde motion in timestamp.
    // This is sometimes caused by erratic reports of the available space in the ALSA drivers.
    if (status == NO_ERROR) {
        if (previousTimestampValid) {
#define TIME_TO_NANOS(time) ((uint64_t)time.tv_sec * 1000000000 + time.tv_nsec)
            const uint64_t previousTimeNanos = TIME_TO_NANOS(mPreviousTimestamp.mTime);
            const uint64_t currentTimeNanos = TIME_TO_NANOS(timestamp.mTime);
#undef TIME_TO_NANOS
            if (currentTimeNanos < previousTimeNanos) {
                ALOGW("retrograde timestamp time");
                // FIXME Consider blocking this from propagating upwards.
            }

            // Looking at signed delta will work even when the timestamps
            // are wrapping around.
            int32_t deltaPosition = static_cast<int32_t>(timestamp.mPosition
                    - mPreviousTimestamp.mPosition);
            // position can bobble slightly as an artifact; this hides the bobble
            static const int32_t MINIMUM_POSITION_DELTA = 8;
            if (deltaPosition < 0) {
                // Only report once per position instead of spamming the log.
                if (!mRetrogradeMotionReported) {
                    ALOGW("retrograde timestamp position corrected, %d = %u - %u",
                            deltaPosition,
                            timestamp.mPosition,
                            mPreviousTimestamp.mPosition);
                    mRetrogradeMotionReported = true;
                }
            } else {
                mRetrogradeMotionReported = false;
            }
            if (deltaPosition < MINIMUM_POSITION_DELTA) {
                timestamp = mPreviousTimestamp;  // Use last valid timestamp.
            }
        }
        mPreviousTimestamp = timestamp;
        mPreviousTimestampValid = true;
    }

    return status;
}

String8 AudioTrack::getParameters(const String8& keys)
{
    audio_io_handle_t output = getOutput();
    if (output != AUDIO_IO_HANDLE_NONE) {
        return AudioSystem::getParameters(output, keys);
    } else {
        return String8::empty();
    }
}

bool AudioTrack::isOffloaded() const
{
    AutoMutex lock(mLock);
    return isOffloaded_l();
}

bool AudioTrack::isDirect() const
{
    AutoMutex lock(mLock);
    return isDirect_l();
}

bool AudioTrack::isOffloadedOrDirect() const
{
    AutoMutex lock(mLock);
    return isOffloadedOrDirect_l();
}


status_t AudioTrack::dump(int fd, const Vector<String16>& args __unused) const
{

    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.append(" AudioTrack::dump\n");
    snprintf(buffer, 255, "  stream type(%d), left - right volume(%f, %f)\n", mStreamType,
            mVolume[AUDIO_INTERLEAVE_LEFT], mVolume[AUDIO_INTERLEAVE_RIGHT]);
    result.append(buffer);
    snprintf(buffer, 255, "  format(%d), channel count(%d), frame count(%zu)\n", mFormat,
            mChannelCount, mFrameCount);
    result.append(buffer);
    snprintf(buffer, 255, "  sample rate(%u), speed(%f), status(%d)\n",
            mSampleRate, mPlaybackRate.mSpeed, mStatus);
    result.append(buffer);
    snprintf(buffer, 255, "  state(%d), latency (%d)\n", mState, mLatency);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

uint32_t AudioTrack::getUnderrunFrames() const
{
    AutoMutex lock(mLock);
    return mProxy->getUnderrunFrames();
}

status_t AudioTrack::addAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback)
{
    if (callback == 0) {
        ALOGW("%s adding NULL callback!", __FUNCTION__);
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (mDeviceCallback == callback) {
        ALOGW("%s adding same callback!", __FUNCTION__);
        return INVALID_OPERATION;
    }
    status_t status = NO_ERROR;
    if (mOutput != AUDIO_IO_HANDLE_NONE) {
        if (mDeviceCallback != 0) {
            ALOGW("%s callback already present!", __FUNCTION__);
            AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mOutput);
        }
        status = AudioSystem::addAudioDeviceCallback(callback, mOutput);
    }
    mDeviceCallback = callback;
    return status;
}

status_t AudioTrack::removeAudioDeviceCallback(
        const sp<AudioSystem::AudioDeviceCallback>& callback)
{
    if (callback == 0) {
        ALOGW("%s removing NULL callback!", __FUNCTION__);
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (mDeviceCallback != callback) {
        ALOGW("%s removing different callback!", __FUNCTION__);
        return INVALID_OPERATION;
    }
    if (mOutput != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mOutput);
    }
    mDeviceCallback = 0;
    return NO_ERROR;
}

// =========================================================================

void AudioTrack::DeathNotifier::binderDied(const wp<IBinder>& who __unused)
{
    sp<AudioTrack> audioTrack = mAudioTrack.promote();
    if (audioTrack != 0) {
        AutoMutex lock(audioTrack->mLock);
        audioTrack->mProxy->binderDied();
    }
}

// =========================================================================

AudioTrack::AudioTrackThread::AudioTrackThread(AudioTrack& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver), mPaused(true), mPausedInt(false), mPausedNs(0LL),
      mIgnoreNextPausedInt(false)
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
        if (mIgnoreNextPausedInt) {
            mIgnoreNextPausedInt = false;
            mPausedInt = false;
        }
        if (mPausedInt) {
            if (mPausedNs > 0) {
                (void) mMyCond.waitRelative(mMyLock, mPausedNs);
            } else {
                mMyCond.wait(mMyLock);
            }
            mPausedInt = false;
            return true;
        }
    }
    if (exitPending()) {
        return false;
    }
    nsecs_t ns = mReceiver.processAudioBuffer();
    switch (ns) {
    case 0:
        return true;
    case NS_INACTIVE:
        pauseInternal();
        return true;
    case NS_NEVER:
        return false;
    case NS_WHENEVER:
        // Event driven: call wake() when callback notifications conditions change.
        ns = INT64_MAX;
        // fall through
    default:
        LOG_ALWAYS_FATAL_IF(ns < 0, "processAudioBuffer() returned %" PRId64, ns);
        pauseInternal(ns);
        return true;
    }
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
    mIgnoreNextPausedInt = true;
    if (mPaused || mPausedInt) {
        mPaused = false;
        mPausedInt = false;
        mMyCond.signal();
    }
}

void AudioTrack::AudioTrackThread::wake()
{
    AutoMutex _l(mMyLock);
    if (!mPaused) {
        // wake() might be called while servicing a callback - ignore the next
        // pause time and call processAudioBuffer.
        mIgnoreNextPausedInt = true;
        if (mPausedInt && mPausedNs > 0) {
            // audio track is active and internally paused with timeout.
            mPausedInt = false;
            mMyCond.signal();
        }
    }
}

void AudioTrack::AudioTrackThread::pauseInternal(nsecs_t ns)
{
    AutoMutex _l(mMyLock);
    mPausedInt = true;
    mPausedNs = ns;
}

} // namespace android
