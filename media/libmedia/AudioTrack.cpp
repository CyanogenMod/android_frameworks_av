/*
** Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
** Not a Contribution.
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
#include <media/AudioParameter.h>
#include <media/AudioSystem.h>
#include <media/AudioTrack.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <media/IAudioFlinger.h>
#include <media/AudioPolicyHelper.h>
#include <media/AudioResamplerPublic.h>
#include <cutils/properties.h>
#include <system/audio.h>

#define WAIT_PERIOD_MS                  10
#define WAIT_STREAM_END_TIMEOUT_SEC     120


namespace android {
// ---------------------------------------------------------------------------

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

// static
status_t AudioTrack::getMinFrameCount(
        size_t* frameCount,
        audio_stream_type_t streamType,
        uint32_t sampleRate)
{
    if (frameCount == NULL) {
        return BAD_VALUE;
    }

    // FIXME merge with similar code in createTrack_l(), except we're missing
    //       some information here that is available in createTrack_l():
    //          audio_io_handle_t output
    //          audio_format_t format
    //          audio_channel_mask_t channelMask
    //          audio_output_flags_t flags
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

    // Ensure that buffer depth covers at least audio hardware latency
    uint32_t minBufCount = afLatency / ((1000 * afFrameCount) / afSampleRate);
    if (minBufCount < 2) {
        minBufCount = 2;
    }

    *frameCount = (sampleRate == 0) ? afFrameCount * minBufCount :
            afFrameCount * minBufCount * uint64_t(sampleRate) / afSampleRate;
    // The formula above should always produce a non-zero value, but return an error
    // in the unlikely event that it does not, as that's part of the API contract.
    if (*frameCount == 0) {
        ALOGE("AudioTrack::getMinFrameCount failed for streamType %d, sampleRate %d",
                streamType, sampleRate);
        return BAD_VALUE;
    }
    ALOGV("getMinFrameCount=%zu: afFrameCount=%zu, minBufCount=%d, afSampleRate=%d, afLatency=%d",
            *frameCount, afFrameCount, minBufCount, afSampleRate, afLatency);
    return NO_ERROR;
}

// ---------------------------------------------------------------------------

AudioTrack::AudioTrack()
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mUseSmallBuf(false),
      mPausedPosition(0)
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
        const audio_attributes_t* pAttributes)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mUseSmallBuf(false),
#ifdef QCOM_DIRECTTRACK
      mPausedPosition(0),
      mAudioFlinger(NULL),
      mObserver(NULL)
#else
      mPausedPosition(0)
#endif

{
#ifdef QCOM_DIRECTTRACK
    mAttributes.content_type = AUDIO_CONTENT_TYPE_UNKNOWN;
    mAttributes.usage = AUDIO_USAGE_UNKNOWN;
    mAttributes.flags = 0x0;
    strcpy(mAttributes.tags, "");
#endif
    mStatus = set(streamType, sampleRate, format, channelMask,
            frameCount, flags, cbf, user, notificationFrames,
            0 /*sharedBuffer*/, false /*threadCanCallJava*/, sessionId, transferType,
            offloadInfo, uid, pid, pAttributes);
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
        const audio_attributes_t* pAttributes)
    : mStatus(NO_INIT),
      mIsTimed(false),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mUseSmallBuf(false),
#ifdef QCOM_DIRECTTRACK
      mPausedPosition(0),
      mAudioFlinger(NULL),
      mObserver(NULL)
#else
      mPausedPosition(0)
#endif
{
#ifdef QCOM_DIRECTTRACK
    mAttributes.content_type = AUDIO_CONTENT_TYPE_UNKNOWN;
    mAttributes.usage = AUDIO_USAGE_UNKNOWN;
    mAttributes.flags = 0x0;
    strcpy(mAttributes.tags, "");
#endif
    mStatus = set(streamType, sampleRate, format, channelMask,
            0 /*frameCount*/, flags, cbf, user, notificationFrames,
            sharedBuffer, false /*threadCanCallJava*/, sessionId, transferType, offloadInfo,
            uid, pid, pAttributes);
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
#ifdef QCOM_DIRECTTRACK
        if (mDirectTrack != 0) {
            mDirectTrack.clear();
        } else if (mAudioTrack != 0) {
#endif
            mAudioTrack->asBinder()->unlinkToDeath(mDeathNotifier, this);
            mAudioTrack.clear();
            mCblkMemory.clear();
            mSharedBuffer.clear();
            IPCThreadState::self()->flushCommands();
            ALOGV("~AudioTrack, releasing session id from %d on behalf of %d",
                IPCThreadState::self()->getCallingPid(), mClientPid);
            AudioSystem::releaseAudioSessionId(mSessionId, mClientPid);
#ifdef QCOM_DIRECTTRACK
        }
#endif
    }
}

bool AudioTrack::canOffloadTrack(
        audio_stream_type_t streamType,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        audio_output_flags_t flags,
        transfer_type transferType,
        audio_attributes_t *attributes,
        const audio_offload_info_t *offloadInfo)
{
        char propValue[PROPERTY_VALUE_MAX] = {0};
        property_get("audio.offload.track.enabled", propValue, "0");
        bool track_offload = atoi(propValue) || !strncmp("true", propValue, sizeof("true"));
        bool decision = false;

        if (track_offload == false) {
             ALOGV("TrackOffload: AudioTrack Offload disabled by property, returning false");
             return false;
        }


       // Track offload only if the following criterion
       // 1. Track offload info structure should NOT have been provided
       // 2. Format is 16 bit
       // 3. Track is NOT fast track (to prevent tones, and low latency from
       //     being offloaded
       // 4. Client uses write interface to provide data


       // Track offload does not base any decision on mAttributes, or channelMask for now

        if (!offloadInfo &&
             (format == AUDIO_FORMAT_PCM_16_BIT) &&
             (streamType == AUDIO_STREAM_MUSIC) &&
             (!(flags & AUDIO_OUTPUT_FLAG_FAST)) &&
             (transferType != TRANSFER_CALLBACK))
        {

            //initialize format to 16_bit_ofload if content is 16 bit
            mPcmTrackOffloadInfo.format = AUDIO_FORMAT_PCM_16_BIT_OFFLOAD;
            mPcmTrackOffloadInfo.sample_rate  = mSampleRate;
            mPcmTrackOffloadInfo.channel_mask = channelMask;
            mPcmTrackOffloadInfo.stream_type = streamType;
            mPcmTrackOffloadInfo.duration_us = 0xFFFFFFFF;
            //TODO: Use mAttributes and pass the value of content in this field
            mPcmTrackOffloadInfo.has_video = false;
            mPcmTrackOffloadInfo.is_streaming = false;
            mPcmTrackOffloadInfo.use_small_bufs = true;

            decision = AudioSystem::isOffloadSupported(mPcmTrackOffloadInfo);
            ALOGV("TrackOffload: Pcm Track offloaded decided %s", decision?"true":"false");
            ALOGV("TrackOffload: offloading audio output for stream type"
              "%d, usage %d, sample rate %u, format %#x,"
              " channel mask %#x, flags %#x, transferType %d, offloadInfo %p",
              streamType, attributes->usage, mSampleRate, format, channelMask, flags,
              transferType, offloadInfo);
            return decision;
        }

        ALOGV("TrackOffload: Not track offloading offloading audio output for stream type"
              "%d, usage %d, sample rate %u, format %#x,"
              " channel mask %#x, flags %#x, transferType %d, offloadInfo %p",
              streamType, attributes->usage, mSampleRate, format, channelMask, flags,
              transferType, offloadInfo);
        return false;
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
        const audio_attributes_t* pAttributes)
{
    ALOGV("set(): streamType %d, sampleRate %u, format %#x, channelMask %#x, frameCount %zu, "
          "flags #%x, notificationFrames %u, sessionId %d, transferType %d",
          streamType, sampleRate, format, channelMask, frameCount, flags, notificationFrames,
          sessionId, transferType);

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

    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(),
            sharedBuffer->size());

    ALOGV("set() streamType %d frameCount %zu flags %04x", streamType, frameCount, flags);

    AutoMutex lock(mLock);

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
        mStreamType = audio_attributes_to_stream_type(&mAttributes);
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

    // AudioFlinger does not currently support 8-bit data in shared memory
    if (format == AUDIO_FORMAT_PCM_8_BIT && sharedBuffer != 0) {
        ALOGE("8-bit data in shared memory is not supported");
        return BAD_VALUE;
    }

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

    // only allow deep buffering for music stream type
    if (mStreamType != AUDIO_STREAM_MUSIC) {
        flags = (audio_output_flags_t)(flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
        if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
            ALOGE("Offloading only allowed with music stream");
            return BAD_VALUE; // To trigger fallback or let the client handle
        }
    }

    audio_stream_type_t attr_streamType = (mStreamType == AUDIO_STREAM_DEFAULT) ?
                                           audio_attributes_to_stream_type(&mAttributes):
                                           mStreamType;

#ifdef QCOM_HARDWARE
    if ((attr_streamType == AUDIO_STREAM_VOICE_CALL) &&
        (mChannelCount == 1) &&
        (sampleRate == 8000 || sampleRate == 16000)) {
        // Allow Voip direct output only if:
        // audio mode is MODE_IN_COMMUNCATION; AND
        // voip output is not opened already; AND
        // requested sample rate matches with that of voip input stream (if opened already)
        int value = 0;
        uint32_t mode = 0, voipOutCount = 1, voipSampleRate = 1;
        String8 valueStr = AudioSystem::getParameters((audio_io_handle_t)0, String8("audio_mode"));
        AudioParameter result = AudioParameter(valueStr);
        if (result.getInt(String8("audio_mode"), value) == NO_ERROR) {
            mode = value;
        }

        valueStr = AudioSystem::getParameters((audio_io_handle_t)0,
                                              String8("voip_out_stream_count"));
        result = AudioParameter(valueStr);
        if (result.getInt(String8("voip_out_stream_count"), value) == NO_ERROR) {
            voipOutCount = value;
        }

        valueStr = AudioSystem::getParameters((audio_io_handle_t)0,
                                              String8("voip_sample_rate"));
        result = AudioParameter(valueStr);
        if (result.getInt(String8("voip_sample_rate"), value) == NO_ERROR) {
            voipSampleRate = value;
        }

        if ((mode == AUDIO_MODE_IN_COMMUNICATION) && (voipOutCount == 0) &&
            ((voipSampleRate == 0) || (voipSampleRate == sampleRate))) {
            if (audio_is_linear_pcm(format)) {
                char propValue[PROPERTY_VALUE_MAX] = {0};
                property_get("use.voice.path.for.pcm.voip", propValue, "0");
                bool voipPcmSysPropEnabled = !strncmp("true", propValue, sizeof("true"));
                if (voipPcmSysPropEnabled) {
                    flags = (audio_output_flags_t)((flags &~AUDIO_OUTPUT_FLAG_FAST) |
                                AUDIO_OUTPUT_FLAG_VOIP_RX | AUDIO_OUTPUT_FLAG_DIRECT);
                    ALOGD("Set VoIP and Direct output flags for PCM format");
                }
            } else {
                flags = (audio_output_flags_t)((flags &~AUDIO_OUTPUT_FLAG_FAST) |
                            AUDIO_OUTPUT_FLAG_VOIP_RX | AUDIO_OUTPUT_FLAG_DIRECT);
                ALOGD("Set VoIP and Direct output flags for Non-PCM formats");
            }
        }
    }
#endif


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
        mFrameSizeAF = mFrameSize;
    } else {
        ALOG_ASSERT(audio_is_linear_pcm(format));
        mFrameSize = channelCount * audio_bytes_per_sample(format);
        mFrameSizeAF = channelCount * audio_bytes_per_sample(
                format == AUDIO_FORMAT_PCM_8_BIT ? AUDIO_FORMAT_PCM_16_BIT : format);
        // createTrack will return an error if PCM format is not supported by server,
        // so no need to check for specific PCM formats here
    }

    // sampling rate must be specified for direct outputs
    if (sampleRate == 0 && (flags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) {
        return BAD_VALUE;
    }
    mSampleRate = sampleRate;

    // Make copy of input parameter offloadInfo so that in the future:
    //  (a) createTrack_l doesn't need it as an input parameter
    //  (b) we can support re-creation of offloaded tracks
    if (offloadInfo != NULL) {
        mOffloadInfoCopy = *offloadInfo;
        mOffloadInfo = &mOffloadInfoCopy;
    } else {
        mOffloadInfo = NULL;
    }

    if (audio_is_offload_pcm(mFormat) &&
         offloadInfo && offloadInfo->use_small_bufs) {
        mUseSmallBuf = true;
        ALOGI("Using small buffers for PCM offload");
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
#ifdef QCOM_DIRECTTRACK
    status_t status;
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
   

    if (flags & AUDIO_OUTPUT_FLAG_LPA || flags & AUDIO_OUTPUT_FLAG_TUNNEL) {
         AudioSystem::getOutputForAttr(&mAttributes, &output,
                                            (audio_session_t)mSessionId, &streamType, 
                                             mSampleRate, mFormat,mChannelMask, 
                                             mFlags, mOffloadInfo); 

    if (status != NO_ERROR && output == AUDIO_IO_HANDLE_NONE) {
        ALOGE("Could not get audio output for stream type %d, usage %d, sample rate %u, format %#x,"
              " channel mask %#x, flags %#x",
              mStreamType, mAttributes.usage, mSampleRate, mFormat, mChannelMask, mFlags);
        return BAD_VALUE;
    }
//    if (mFlags & AUDIO_OUTPUT_FLAG_LPA || mFlags & AUDIO_OUTPUT_FLAG_TUNNEL) {
        mAudioDirectOutput = output;
  //  }

        ALOGV("Creating Direct Track");
        const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
        if (audioFlinger == 0) {
            ALOGE("Could not get audioflinger");
            return NO_INIT;
        }
        mAudioFlinger = audioFlinger;
        status_t status = NO_ERROR;
        mDirectClient = new DirectClient(this);
        mDirectTrack = audioFlinger->createDirectTrack( getpid(),
                                                        sampleRate,
                                                        channelMask,
                                                        mAudioDirectOutput,
                                                        &mSessionId,
                                                        mDirectClient.get(),
                                                        streamType,
                                                        &status);
        if(status != NO_ERROR) {
            ALOGE("createDirectTrack returned with status %d", status);
            return status;
        }
        mAudioTrack = NULL;
        mSharedBuffer = NULL;
    }
    else {
#endif
        if (cbf != NULL) {
            mAudioTrackThread = new AudioTrackThread(*this, threadCanCallJava);
            mAudioTrackThread->run("AudioTrack", ANDROID_PRIORITY_AUDIO, 0 /*stack*/);
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

#ifdef QCOM_DIRECTTRACK
    AudioSystem::acquireAudioSessionId(mSessionId, mClientPid);
    mAudioDirectOutput = -1;
    mDirectTrack = NULL;
 }
#endif
    mStatus = NO_ERROR;
    mState = STATE_STOPPED;
    mUserData = user;
    mLoopPeriod = 0;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    mServer = 0;
    mPosition = 0;
    mReleased = 0;
    mStartUs = 0;
    AudioSystem::acquireAudioSessionId(mSessionId, mClientPid);
    mSequence = 1;
    mObservedSequence = mSequence;
    mInUnderrun = false;
    ALOGE("AudioTrack::set : Exit");
    return NO_ERROR;
}
// -------------------------------------------------------------------------

status_t AudioTrack::start()
{
    status_t status = NO_ERROR;
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
#ifdef QCOM_DIRECTTRACK
    if (mDirectTrack != NULL) {
        mDirectTrack->start();
        return status;
    }
#endif
    (void) updateAndGetPosition_l();
    if (previousState == STATE_STOPPED || previousState == STATE_FLUSHED) {
        // reset current position as seen by client to 0
        mPosition = 0;
        // For offloaded tracks, we don't know if the hardware counters are really zero here,
        // since the flush is asynchronous and stop may not fully drain.
        // We save the time when the track is started to later verify whether
        // the counters are realistic (i.e. start from zero after this time).
        mStartUs = getNowUs();

        // force refresh of remaining frames by processAudioBuffer() as last
        // write before stop could be partial.
        mRefreshRemaining = true;
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

    if (!(flags & (CBLK_INVALID | CBLK_STREAM_FATAL_ERROR))) {
        status = mAudioTrack->start();
        if (status == DEAD_OBJECT) {
            flags |= CBLK_INVALID;
        }
    }
    if (flags & (CBLK_INVALID | CBLK_STREAM_FATAL_ERROR)) {
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
#ifdef QCOM_DIRECTTRACK
    if(mDirectTrack != NULL) {
        mDirectTrack->stop();
    } else if (mAudioTrack != NULL) {
#endif
        mProxy->interrupt();
        mAudioTrack->stop();
    // the playback head position will reset to 0, so if a marker is set, we need
    // to activate it again
        mMarkerReached = false;
#if 0
    // Force flush if a shared buffer is used otherwise audioflinger
    // will not stop before end of buffer is reached.
    // It may be needed to make sure that we stop playback, likely in case looping is on.
    if (mSharedBuffer != 0) {
        flush_l();
    }
#endif

        sp<AudioTrackThread> t = mAudioTrackThread;
        if (t != 0) {
            if (!isOffloaded_l()) {
                t->pause();
            }
        } else {
            setpriority(PRIO_PROCESS, 0, mPreviousPriority);
            set_sched_policy(0, mPreviousSchedulingGroup);
        }
#ifdef QCOM_DIRECTTRACK
    }
#endif
}

bool AudioTrack::stopped() const
{
    AutoMutex lock(mLock);
    return mState != STATE_ACTIVE;
}

void AudioTrack::flush()
{
#ifdef QCOM_DIRECTTRACK
    if(mDirectTrack != NULL) {
        ALOGE("mdirect track flush");
        mDirectTrack->flush();
        return ;
    }
#else
    if (mSharedBuffer != 0) {
        return;
    }
    AutoMutex lock(mLock);
#endif
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
#ifdef QCOM_DIRECTTRACK
    if(mDirectTrack != NULL) {
       ALOGV("mDirectTrack pause");
       mDirectTrack->pause();
    } else {
#endif
       mProxy->interrupt();
       mAudioTrack->pause();
#ifdef QCOM_DIRECTTRACK
    }
#endif
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
#ifdef QCOM_DIRECTTRACK
    if(mDirectTrack != NULL) {
        ALOGV("mDirectTrack->setVolume(left = %f , right = %f)", left,right);
        mDirectTrack->setVolume(left, right);
    } else
#endif
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
#ifdef QCOM_DIRECTTRACK
    if (mDirectTrack != NULL) {
        return NO_ERROR;
    }
#endif
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
    if (mIsTimed || isOffloadedOrDirect()) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);
    if (mOutput == AUDIO_IO_HANDLE_NONE) {
        return NO_INIT;
    }
    uint32_t afSamplingRate;
    if (AudioSystem::getSamplingRate(mOutput, &afSamplingRate) != NO_ERROR) {
        return NO_INIT;
    }
    if (rate == 0 || rate > afSamplingRate * AUDIO_RESAMPLER_DOWN_RATIO_MAX) {
        return BAD_VALUE;
    }

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
#ifdef QCOM_DIRECTTRACK
    if(mAudioDirectOutput != -1) {
        return mAudioFlinger->sampleRate(mAudioDirectOutput);
    }
#endif
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
    // Setting the loop will reset next notification update period (like setPosition).
    mNewPosition = updateAndGetPosition_l() + mUpdatePeriod;
    mLoopPeriod = loopCount != 0 ? loopEnd - loopStart : 0;
    mStaticProxy->setLoop(loopStart, loopEnd, loopCount);
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
    mNewPosition = updateAndGetPosition_l() + mUpdatePeriod;
    mLoopPeriod = 0;
    // FIXME Check whether loops and setting position are incompatible in old code.
    // If we use setLoop for both purposes we lose the capability to set the position while looping.
    mStaticProxy->setLoop(position, mFrameCount, 0);

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
        status_t status;

        if (isOffloaded_l() && ((mState == STATE_PAUSED) || (mState == STATE_PAUSED_STOPPING))) {
            ALOGV("getPosition called in paused state, return cached position %u", mPausedPosition);
            *position = mPausedPosition;
            return NO_ERROR;
        }
        if (mUseSmallBuf) {
            uint32_t tempPos = 0;
            tempPos = (mState == STATE_STOPPED || mState == STATE_FLUSHED) ?
                0 : updateAndGetPosition_l();
            *position = (tempPos / (mChannelCount * audio_bytes_per_sample(mFormat)));
            return NO_ERROR;
        }

        if (mOutput != AUDIO_IO_HANDLE_NONE) {
            uint32_t halFrames;
            status = AudioSystem::getRenderPosition(mOutput, &halFrames, &dspFrames);
            if (status != NO_ERROR) {
                ALOGW("failed to getRenderPosition for offload session");
                return INVALID_OPERATION;
            }
        }
        // FIXME: dspFrames may not be zero in (mState == STATE_STOPPED || mState == STATE_FLUSHED)
        // due to hardware latency. We leave this behavior for now.
        *position = dspFrames;
    } else {
        if (mCblk->mFlags & CBLK_INVALID) {
            restoreTrack_l("getPosition");
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
    mLoopPeriod = 0;
    // FIXME The new code cannot reload while keeping a loop specified.
    // Need to check how the old code handled this, and whether it's a significant change.
    mStaticProxy->setLoop(0, mFrameCount, 0);
    return NO_ERROR;
}

audio_io_handle_t AudioTrack::getOutput() const
{
    AutoMutex lock(mLock);
    return mOutput;
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

    status_t status;
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    audio_stream_type_t streamType = mStreamType;
    audio_attributes_t *attr = (mStreamType == AUDIO_STREAM_DEFAULT) ? &mAttributes : NULL;
    mCanOffloadPcmTrack = false;
    mIsPcmTrackOffloaded = false;
    mPcmTrackOffloadInfo = AUDIO_INFO_INITIALIZER;

    // Check if the track can be offloaded. Store the decision in mCanOffloadPcmTrack
    mCanOffloadPcmTrack = canOffloadTrack(mStreamType, mFormat, mChannelMask, mFlags,
                              mTransfer, &mAttributes, mOffloadInfo);


    if(mCanOffloadPcmTrack) {
        ALOGV("TrackOffload: Tying to create PCM Offload track");
        audio_output_flags_t flags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
        flags = (audio_output_flags_t) (flags | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
        status = AudioSystem::getOutputForAttr(attr, &output,
                                                (audio_session_t) mSessionId, &streamType,
                                                mSampleRate, mPcmTrackOffloadInfo.format,
                                                mChannelMask, (audio_output_flags_t) (flags),
                                                &mPcmTrackOffloadInfo);
        if (status == NO_ERROR && output != AUDIO_IO_HANDLE_NONE) {
            //got output
            ALOGV("TrackOffload: Going through PCM Offload Track");
            mIsPcmTrackOffloaded = true;
            mFlags = flags;
            mFrameSize = sizeof(uint8_t);
            mFrameSizeAF = sizeof(uint8_t);
            mUseSmallBuf = true;
        }
    }

    //retry retrieving output
    if (status != NO_ERROR || output == AUDIO_IO_HANDLE_NONE) {
        status = AudioSystem::getOutputForAttr(attr, &output,
                                                        (audio_session_t)mSessionId, &streamType,
                                                        mSampleRate, mFormat, mChannelMask,
                                                        mFlags, mOffloadInfo);

    }

    if (status != NO_ERROR || output == AUDIO_IO_HANDLE_NONE) {
            ALOGE("Could not get audio output for stream type %d, usage %d, sample rate %u, format %#x,"
                  " channel mask %#x, flags %#x",
                  streamType, mAttributes.usage, mSampleRate, mFormat, mChannelMask, mFlags);
            return BAD_VALUE;
    }

    {
    // Now that we have a reference to an I/O handle and have not yet handed it off to AudioFlinger,
    // we must release it ourselves if anything goes wrong.

    // Not all of these values are needed under all conditions, but it is easier to get them all

    uint32_t afLatency;
    status = AudioSystem::getLatency(output, &afLatency);
    if (status != NO_ERROR) {
        ALOGE("getLatency(%d) failed status %d", output, status);
        goto release;
    }

    size_t afFrameCount;
    status = AudioSystem::getFrameCount(output, &afFrameCount);
    if (status != NO_ERROR) {
        ALOGE("getFrameCount(output=%d) status %d", output, status);
        goto release;
    }

    uint32_t afSampleRate;
    status = AudioSystem::getSamplingRate(output, &afSampleRate);
    if (status != NO_ERROR) {
        ALOGE("getSamplingRate(output=%d) status %d", output, status);
        goto release;
    }
    if (mSampleRate == 0) {
        mSampleRate = afSampleRate;
    }
    // Client decides whether the track is TIMED (see below), but can only express a preference
    // for FAST.  Server will perform additional tests.
    if ((mFlags & AUDIO_OUTPUT_FLAG_FAST) && !((
            // either of these use cases:
            // use case 1: shared buffer
            (mSharedBuffer != 0) ||
            // use case 2: callback transfer mode
            (mTransfer == TRANSFER_CALLBACK)) &&
            // matching sample rate
            (mSampleRate == afSampleRate))) {
        ALOGW("AUDIO_OUTPUT_FLAG_FAST denied by client");
        // once denied, do not request again if IAudioTrack is re-created
        mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_FAST);
    }
    ALOGV("createTrack_l() output %d afLatency %d", output, afLatency);

    // The client's AudioTrack buffer is divided into n parts for purpose of wakeup by server, where
    //  n = 1   fast track with single buffering; nBuffering is ignored
    //  n = 2   fast track with double buffering
    //  n = 2   normal track, no sample rate conversion
    //  n = 3   normal track, with sample rate conversion
    //          (pessimistic; some non-1:1 conversion ratios don't actually need triple-buffering)
    //  n > 3   very high latency or very small notification interval; nBuffering is ignored
    const uint32_t nBuffering = (mSampleRate == afSampleRate) ? 2 : 3;

    mNotificationFramesAct = mNotificationFramesReq;

    size_t frameCount = mReqFrameCount;
    if(mIsPcmTrackOffloaded)
        frameCount = 0;

    if (!audio_is_linear_pcm(mFormat)) {

        if (mSharedBuffer != 0) {
            // Same comment as below about ignoring frameCount parameter for set()
            frameCount = mSharedBuffer->size();
        } else if (frameCount == 0) {
            frameCount = afFrameCount * 2;
            ALOGV("Offload: new frameCount = %d", frameCount);
        }
        if (mNotificationFramesAct != frameCount) {
            mNotificationFramesAct = frameCount;
        }
    } else if (mSharedBuffer != 0) {

        // Ensure that buffer alignment matches channel count
        // 8-bit data in shared memory is not currently supported by AudioFlinger
        size_t alignment = audio_bytes_per_sample(
                mFormat == AUDIO_FORMAT_PCM_8_BIT ? AUDIO_FORMAT_PCM_16_BIT : mFormat);
        if (alignment & 1) {
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
        frameCount = mSharedBuffer->size() / mFrameSizeAF;

    } else if (!(mFlags & AUDIO_OUTPUT_FLAG_FAST) || mIsPcmTrackOffloaded) {

        // FIXME move these calculations and associated checks to server

        // Ensure that buffer depth covers at least audio hardware latency
        uint32_t minBufCount = afLatency / ((1000 * afFrameCount)/afSampleRate);
        ALOGV("afFrameCount=%zu, minBufCount=%d, afSampleRate=%u, afLatency=%d",
                afFrameCount, minBufCount, afSampleRate, afLatency);
        if (minBufCount <= nBuffering) {
            minBufCount = nBuffering;
        }

        size_t minFrameCount = afFrameCount * minBufCount * uint64_t(mSampleRate) / afSampleRate;
        ALOGV("minFrameCount: %zu, afFrameCount=%zu, minBufCount=%d, sampleRate=%u, afSampleRate=%u"
                ", afLatency=%d",
                minFrameCount, afFrameCount, minBufCount, mSampleRate, afSampleRate, afLatency);

        if (frameCount == 0) {
            frameCount = minFrameCount;
        } else if (frameCount < minFrameCount) {
            // not ALOGW because it happens all the time when playing key clicks over A2DP
            ALOGV("Minimum buffer size corrected from %zu to %zu",
                     frameCount, minFrameCount);
            frameCount = minFrameCount;
        }
        // Make sure that application is notified with sufficient margin before underrun
        if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount/nBuffering) {
            mNotificationFramesAct = frameCount/nBuffering;
        }

    } else {
        // For fast tracks, the frame count calculations and checks are done by server
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
        ALOGV("TrackOffload: mFlags has offload");
        trackFlags |= IAudioFlinger::TRACK_OFFLOAD;
    }

    if (mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
        ALOGV("TrackOffload: mFlags has direct");
        trackFlags |= IAudioFlinger::TRACK_DIRECT;
    }

    size_t temp = frameCount;   // temp may be replaced by a revised value of frameCount,
                                // but we will still need the original value also
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;
    if (mIsPcmTrackOffloaded) {
        format = AUDIO_FORMAT_PCM_16_BIT_OFFLOAD;
        ALOGV("Create offloaded PCM 0x%x Track", format);
        trackFlags |= IAudioFlinger::TRACK_OFFLOAD;
    } else {
        // AudioFlinger only sees 16-bit PCM
        format = (mFormat == AUDIO_FORMAT_PCM_8_BIT &&
                   !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) ?
                    AUDIO_FORMAT_PCM_16_BIT : mFormat;
        ALOGV("Create normal PCM 0x%x Track", format);
    }
    sp<IAudioTrack> track = audioFlinger->createTrack(streamType,
                                                      mSampleRate,
                                                      format,
                                                      mChannelMask,
                                                      &temp,
                                                      &trackFlags,
                                                      mSharedBuffer,
                                                      output,
                                                      tid,
                                                      &mSessionId,
                                                      mClientUid,
                                                      &status);

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
        mAudioTrack->asBinder()->unlinkToDeath(mDeathNotifier, this);
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
    ALOGV("Flags here  0x%x ", mFlags);
    if (mFlags & AUDIO_OUTPUT_FLAG_FAST) {
        if (trackFlags & IAudioFlinger::TRACK_FAST) {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST successful; frameCount %zu", frameCount);
            mAwaitBoost = true;
            if (mSharedBuffer == 0) {
                // Theoretically double-buffering is not required for fast tracks,
                // due to tighter scheduling.  But in practice, to accommodate kernels with
                // scheduling jitter, and apps with computation jitter, we use double-buffering.
                if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount/nBuffering) {
                    mNotificationFramesAct = frameCount/nBuffering;
                }
            }
        } else {
            ALOGV("AUDIO_OUTPUT_FLAG_FAST denied by server; frameCount %zu", frameCount);
            // once denied, do not request again if IAudioTrack is re-created
            mFlags = (audio_output_flags_t) (mFlags & ~AUDIO_OUTPUT_FLAG_FAST);
            if (mSharedBuffer == 0) {
                if (mNotificationFramesAct == 0 || mNotificationFramesAct > frameCount/nBuffering) {
                    mNotificationFramesAct = frameCount/nBuffering;
                }
            }
        }
    }
    if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD || mIsPcmTrackOffloaded) {
        if (trackFlags & IAudioFlinger::TRACK_OFFLOAD) {
            ALOGV("TrackOffload: AUDIO_OUTPUT_FLAG_OFFLOAD successful");
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

    // We retain a copy of the I/O handle, but don't own the reference
    mOutput = output;
    mRefreshRemaining = true;

    // Starting address of buffers in shared memory.  If there is a shared buffer, buffers
    // is the value of pointer() for the shared buffer, otherwise buffers points
    // immediately after the control block.  This address is for the mapping within client
    // address space.  AudioFlinger::TrackBase::mBuffer is for the server address space.
    void* buffers;
    if (mSharedBuffer == 0) {
        buffers = (char*)cblk + sizeof(audio_track_cblk_t);
    } else {
        buffers = mSharedBuffer->pointer();
    }

    mAudioTrack->attachAuxEffect(mAuxEffectId);

    if (mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
        // Use latency given by HAL in offload mode
        mLatency = afLatency;
    } else {
        // FIXME don't believe this lie
        mLatency = afLatency + (1000*frameCount) / mSampleRate;
    }
    mFrameCount = frameCount;
    // If IAudioTrack is re-created, don't let the requested frameCount
    // decrease.  This can confuse clients that cache frameCount().
    if (frameCount > mReqFrameCount) {
        mReqFrameCount = frameCount;
    }

    // update proxy
    if (mSharedBuffer == 0) {
        mStaticProxy.clear();
        mProxy = new AudioTrackClientProxy(cblk, buffers, frameCount, mFrameSizeAF);
    } else {
        mStaticProxy = new StaticAudioTrackClientProxy(cblk, buffers, frameCount, mFrameSizeAF);
        mProxy = mStaticProxy;
    }

    mProxy->setVolumeLR(gain_minifloat_pack(
            gain_from_float(mVolume[AUDIO_INTERLEAVE_LEFT]),
            gain_from_float(mVolume[AUDIO_INTERLEAVE_RIGHT])));

    mProxy->setSendLevel(mSendLevel);
    mProxy->setSampleRate(mSampleRate);
    mProxy->setMinimum(mNotificationFramesAct);

    mDeathNotifier = new DeathNotifier(this);
    mAudioTrack->asBinder()->linkToDeath(mDeathNotifier, this);

    return NO_ERROR;
    }

release:
    AudioSystem::releaseOutput(output, streamType, (audio_session_t)mSessionId);
    if (status == NO_ERROR) {
        status = NO_INIT;
    }
    return status;
}

status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, int32_t waitCount)
{
    if (audioBuffer == NULL) {
        return BAD_VALUE;
    }
    if (mTransfer != TRANSFER_OBTAIN) {
        audioBuffer->frameCount = 0;
        audioBuffer->size = 0;
        audioBuffer->raw = NULL;
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
    return obtainBuffer(audioBuffer, requested);
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
    audioBuffer->size = buffer.mFrameCount * mFrameSizeAF;
    audioBuffer->raw = buffer.mRaw;
    if (nonContig != NULL) {
        *nonContig = buffer.mNonContig;
    }
    return status;
}

void AudioTrack::releaseBuffer(Buffer* audioBuffer)
{
    if (mTransfer == TRANSFER_SHARED) {
        return;
    }

    size_t stepCount = audioBuffer->size / mFrameSizeAF;
    if (stepCount == 0) {
        return;
    }

    if (stepCount > audioBuffer->frameCount) {
        ALOGI("TrackOffload: stepCount:framecount %d:%d", stepCount, audioBuffer->frameCount);
        stepCount = audioBuffer->frameCount;
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
#ifdef QCOM_DIRECTTRACK
    if (mDirectTrack != NULL) {
        ssize_t written = 0;
        written = mDirectTrack->write(buffer,userSize);
        return written;
    }
#endif

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

        size_t toWrite;
        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            // Divide capacity by 2 to take expansion into account
            toWrite = audioBuffer.size >> 1;
            memcpy_to_i16_from_u8(audioBuffer.i16, (const uint8_t *) buffer, toWrite);
        } else {
            toWrite = audioBuffer.size;
            if(toWrite > userSize) {
                ALOGI("TrackOffload: temp toWrite>userSize %d:%d", toWrite, userSize);
                toWrite = userSize;
            }
            memcpy(audioBuffer.i8, buffer, toWrite);
        }
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
    if (mCblk == NULL) {
        ALOGE("mCblk is NULL");
        return NS_NEVER;
    }

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

    if (flags & CBLK_STREAM_FATAL_ERROR) {
        ALOGE("clbk sees STREAM_FATAL_ERROR.. close session");
        mLock.unlock();
        mCbf(EVENT_STREAM_END, mUserData, NULL);
        return NS_INACTIVE;
    }

    // Check for track invalidation
    if (flags & CBLK_INVALID) {
        // for offloaded tracks restoreTrack_l() will just update the sequence and clear
        // AudioSystem cache. We should not exit here but after calling the callback so
        // that the upper layers can recreate the track
        if (!isOffloadedOrDirect_l() || (mSequence == mObservedSequence)) {
            status_t status = restoreTrack_l("processAudioBuffer");
            mLock.unlock();
            // Run again immediately, but with a new IAudioTrack
            return 0;
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
    uint32_t loopPeriod = mLoopPeriod;
    uint32_t sampleRate = mSampleRate;
    uint32_t notificationFrames = mNotificationFramesAct;
    if (mRefreshRemaining) {
        mRefreshRemaining = false;
        mRemainingFrames = notificationFrames;
        mRetryOnPartialBuffer = false;
    }
    size_t misalignment = mProxy->getMisalignment();
    uint32_t sequence = mSequence;
    sp<AudioTrackClientProxy> proxy = mProxy;

    // These fields don't need to be cached, because they are assigned only by set():
    //     mTransfer, mCbf, mUserData, mFormat, mFrameSize, mFrameSizeAF, mFlags
    // mFlags is also assigned by createTrack_l(), but not the bit we care about.

    mLock.unlock();

    // perform callbacks while unlocked
    if (newUnderrun) {
        mCbf(EVENT_UNDERRUN, mUserData, NULL);
    }
    // FIXME we will miss loops if loop cycle was signaled several times since last call
    //       to processAudioBuffer()
    if (flags & (CBLK_LOOP_CYCLE | CBLK_LOOP_FINAL)) {
        mCbf(EVENT_LOOP_END, mUserData, NULL);
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
        AutoMutex lock(mLock);

        sp<AudioTrackClientProxy> proxy = mProxy;
        sp<IMemory> iMem = mCblkMemory;

        struct timespec timeout;
        timeout.tv_sec = WAIT_STREAM_END_TIMEOUT_SEC;
        timeout.tv_nsec = 0;

        mLock.unlock();
        status_t status = mProxy->waitStreamEndDone(&timeout);
        mLock.lock();
        switch (status) {
        case NO_ERROR:
        case DEAD_OBJECT:
        case TIMED_OUT:
            if (isOffloaded_l()) {
                if (mCblk->mFlags & (CBLK_INVALID | CBLK_STREAM_FATAL_ERROR)) {
                    // will trigger EVENT_NEW_IAUDIOTRACK/STREAM_END in next iteration
                    return 0;
                }
            }

            mLock.unlock();
            mCbf(EVENT_STREAM_END, mUserData, NULL);
            mLock.lock();
            if (mState == STATE_STOPPING) {
                mState = STATE_STOPPED;
                if (status != DEAD_OBJECT) {
                   return NS_INACTIVE;
                }
            }
            return 0;
        default:
            return 0;
        }
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
        minFrames = loopPeriod;
    }
    if (updatePeriod > 0 && updatePeriod < minFrames) {
        minFrames = updatePeriod;
    }

    // If > 0, poll periodically to recover from a stuck server.  A good value is 2.
    static const uint32_t kPoll = 0;
    if (kPoll > 0 && mTransfer == TRANSFER_CALLBACK && kPoll * notificationFrames < minFrames) {
        minFrames = kPoll * notificationFrames;
    }

    // Convert frame units to time units
    nsecs_t ns = NS_WHENEVER;
    if (minFrames != (uint32_t) ~0) {
        // This "fudge factor" avoids soaking CPU, and compensates for late progress by server
        static const nsecs_t kFudgeNs = 10000000LL; // 10 ms
        ns = ((minFrames * 1000000000LL) / sampleRate) + kFudgeNs;
    }

    // If not supplying data by EVENT_MORE_DATA, then we're done
    if (mTransfer != TRANSFER_CALLBACK) {
        return ns;
    }

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
                return 0;
            }
            ALOGE("Error %d obtaining an audio buffer, giving up.", err);
            return NS_NEVER;
        }

        if (mRetryOnPartialBuffer && !isOffloaded()) {
            mRetryOnPartialBuffer = false;
            if (avail < mRemainingFrames) {
                int64_t myns = ((mRemainingFrames - avail) * 1100000000LL) / sampleRate;
                if (ns < 0 || myns < ns) {
                    ns = myns;
                }
                return ns;
            }
        }

        // Divide buffer size by 2 to take into account the expansion
        // due to 8 to 16 bit conversion: the callback must fill only half
        // of the destination buffer
        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            audioBuffer.size >>= 1;
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
            return WAIT_PERIOD_MS * 1000000LL;
        }

        if (mFormat == AUDIO_FORMAT_PCM_8_BIT && !(mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
            // 8 to 16 bit conversion, note that source and destination are the same address
            memcpy_to_i16_from_u8(audioBuffer.i16, (const uint8_t *) audioBuffer.i8, writtenSize);
            audioBuffer.size <<= 1;
        }

        size_t releasedFrames = audioBuffer.size / mFrameSizeAF;
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
            return (mRemainingFrames * 1100000000LL) / sampleRate;
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
    status_t result;

    // refresh the audio configuration cache in this process to make sure we get new
    // output parameters and new IAudioFlinger in createTrack_l()
    AudioSystem::clearAudioConfigCache();

    if (isOffloadedOrDirect_l()) {
        // FIXME re-creation of offloaded tracks is not yet implemented
        return DEAD_OBJECT;
    }

    // save the old static buffer position
    size_t bufferPosition = mStaticProxy != NULL ? mStaticProxy->getBufferPosition() : 0;

    // If a new IAudioTrack is successfully created, createTrack_l() will modify the
    // following member variables: mAudioTrack, mCblkMemory and mCblk.
    // It will also delete the strong references on previous IAudioTrack and IMemory.
    // If a new IAudioTrack cannot be created, the previous (dead) instance will be left intact.
    result = createTrack_l();

    // take the frames that will be lost by track recreation into account in saved position
    (void) updateAndGetPosition_l();
    mPosition = mReleased;

    if (result == NO_ERROR) {
        // continue playback from last known position, but
        // don't attempt to restore loop after invalidation; it's difficult and not worthwhile
        if (mStaticProxy != NULL) {
            mLoopPeriod = 0;
            mStaticProxy->setLoop(bufferPosition, mFrameCount, 0);
        }
        // FIXME How do we simulate the fact that all frames present in the buffer at the time of
        //       track destruction have been played? This is critical for SoundPool implementation
        //       This must be broken, and needs to be tested/debugged.
#if 0
        // restore write index and set other indexes to reflect empty buffer status
        if (!strcmp(from, "start")) {
            // Make sure that a client relying on callback events indicating underrun or
            // the actual amount of audio frames played (e.g SoundPool) receives them.
            if (mSharedBuffer == 0) {
                // restart playback even if buffer is not completely filled.
                android_atomic_or(CBLK_FORCEREADY, &mCblk->mFlags);
            }
        }
#endif
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
    int32_t delta = newServer - mServer;
    mServer = newServer;
    // TODO There is controversy about whether there can be "negative jitter" in server position.
    //      This should be investigated further, and if possible, it should be addressed.
    //      A more definite failure mode is infrequent polling by client.
    //      One could call (void)getPosition_l() in releaseBuffer(),
    //      so mReleased and mPosition are always lock-step as best possible.
    //      That should ensure delta never goes negative for infrequent polling
    //      unless the server has more than 2^31 frames in its buffer,
    //      in which case the use of uint32_t for these counters has bigger issues.
    if (delta < 0) {
        ALOGE("detected illegal retrograde motion by the server: mServer advanced by %d", delta);
        delta = 0;
    }
    return mPosition += (uint32_t) delta;
}

status_t AudioTrack::setParameters(const String8& keyValuePairs)
{
    AutoMutex lock(mLock);
    return mAudioTrack->setParameters(keyValuePairs);
}

status_t AudioTrack::getTimestamp(AudioTimestamp& timestamp)
{
    AutoMutex lock(mLock);
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
        restoreTrack_l("getTimestamp");
    }

    // The presented frame count must always lag behind the consumed frame count.
    // To avoid a race, read the presented frames first.  This ensures that presented <= consumed.
    status_t status = NO_ERROR;
    if (!mUseSmallBuf) {
        status = mAudioTrack->getTimestamp(timestamp);
        if (status != NO_ERROR) {
            ALOGV_IF(status != WOULD_BLOCK, "getTimestamp error:%#x", status);
            return status;
        }
    }

    if (isOffloadedOrDirect_l() && !mUseSmallBuf) {
        if (isOffloaded_l() && (mState == STATE_PAUSED || mState == STATE_PAUSED_STOPPING)) {
            // use cached paused position in case another offloaded track is running.
            timestamp.mPosition = mPausedPosition;
            clock_gettime(CLOCK_MONOTONIC, &timestamp.mTime);
            return NO_ERROR;
        }

        // Check whether a pending flush or stop has completed, as those commands may
        // be asynchronous or return near finish.
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
                const int64_t deltaPositionByUs = timestamp.mPosition * 1000000LL / mSampleRate;

                if (deltaPositionByUs > deltaTimeUs + kTimeJitterUs) {
                    // Verify that the counter can't count faster than the sample rate
                    // since the start time.  If greater, then that means we have failed
                    // to completely flush or stop the previous playing track.
                    ALOGW("incomplete flush or stop:"
                            " deltaTimeUs(%lld) deltaPositionUs(%lld) tsmPosition(%u)",
                            (long long)deltaTimeUs, (long long)deltaPositionByUs,
                            timestamp.mPosition);
                    return WOULD_BLOCK;
                }
            }
            mStartUs = 0; // no need to check again, start timestamp has either expired or unneeded.
        }
    } else {
        // Update the mapping between local consumed (mPosition) and server consumed (mServer)
        if (mUseSmallBuf) {
            uint32_t tempPos = 0;
            tempPos = (mState == STATE_STOPPED || mState == STATE_FLUSHED) ?
                0 : updateAndGetPosition_l();
            timestamp.mPosition = (tempPos / (mChannelCount * audio_bytes_per_sample(mFormat)));
            clock_gettime(CLOCK_MONOTONIC, &timestamp.mTime);
            return NO_ERROR;
        } else {
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
            // If (mPosition - mServer) can be negative then should use:
            //   (int32_t)(mPosition - mServer)
            timestamp.mPosition += mPosition - mServer;
           // Immediately after a call to getPosition_l(), mPosition and
           // mServer both represent the same frame position.  mPosition is
           // in client's point of view, and mServer is in server's point of
           // view.  So the difference between them is the "fudge factor"
           // between client and server views due to stop() and/or new
           // IAudioTrack.  And timestamp.mPosition is initially in server's
           // point of view, so we need to apply the same fudge factor to it.
        }
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
    snprintf(buffer, 255, "  sample rate(%u), status(%d)\n", mSampleRate, mStatus);
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

// =========================================================================

void AudioTrack::DeathNotifier::binderDied(const wp<IBinder>& who __unused)
{
    sp<AudioTrack> audioTrack = mAudioTrack.promote();
    if (audioTrack != 0) {
        AutoMutex lock(audioTrack->mLock);
        audioTrack->mProxy->binderDied();
    }
}

#ifdef QCOM_DIRECTTRACK
void AudioTrack::notify(int msg) {
    if (msg == EVENT_UNDERRUN) {
        ALOGV("Posting event underrun to Audio Sink.");
        mCbf(EVENT_UNDERRUN, mUserData, 0);
    }
    if (msg == EVENT_HW_FAIL) {
        ALOGV("Posting event HW fail to Audio Sink.");
        mCbf(EVENT_HW_FAIL, mUserData, 0);
    }
}

status_t AudioTrack::getTimeStamp(uint64_t *tstamp) {
    if (mDirectTrack != NULL) {
        *tstamp = mDirectTrack->getTimeStamp();
        ALOGE("Timestamp %lld ", *tstamp);
    }
    return NO_ERROR;
}

void AudioTrack::DirectClient::notify(int msg) {
    sp<AudioTrack> track = mAudioTrack.promote();
    if (track == 0) {
        ALOGE("AudioTrack dead?");
        return;
    }

    return track->notify(msg);
}
#endif

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
        // FIXME increase poll interval, or make event-driven
        ns = 1000000000LL;
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

void AudioTrack::AudioTrackThread::pauseInternal(nsecs_t ns)
{
    AutoMutex _l(mMyLock);
    mPausedInt = true;
    mPausedNs = ns;
}

}; // namespace android
