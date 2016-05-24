/*
**
** Copyright 2008, The Android Open Source Project
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
#define LOG_TAG "AudioRecord"

#include <inttypes.h>
#include <sys/resource.h>

#include <binder/IPCThreadState.h>
#include <media/AudioRecord.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <media/IAudioFlinger.h>

#define WAIT_PERIOD_MS          10

namespace android {
// ---------------------------------------------------------------------------

// static
status_t AudioRecord::getMinFrameCount(
        size_t* frameCount,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask)
{
    if (frameCount == NULL) {
        return BAD_VALUE;
    }

    size_t size;
    status_t status = AudioSystem::getInputBufferSize(sampleRate, format, channelMask, &size);
    if (status != NO_ERROR) {
        ALOGE("AudioSystem could not query the input buffer size for sampleRate %u, format %#x, "
              "channelMask %#x; status %d", sampleRate, format, channelMask, status);
        return status;
    }

    // We double the size of input buffer for ping pong use of record buffer.
    // Assumes audio_is_linear_pcm(format)
    if ((*frameCount = (size * 2) / (audio_channel_count_from_in_mask(channelMask) *
            audio_bytes_per_sample(format))) == 0) {
        ALOGE("Unsupported configuration: sampleRate %u, format %#x, channelMask %#x",
            sampleRate, format, channelMask);
        return BAD_VALUE;
    }

    return NO_ERROR;
}

// ---------------------------------------------------------------------------

AudioRecord::AudioRecord(const String16 &opPackageName)
    : mActive(false), mStatus(NO_INIT), mOpPackageName(opPackageName), mSessionId(AUDIO_SESSION_ALLOCATE),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL), mPreviousSchedulingGroup(SP_DEFAULT),
      mSelectedDeviceId(AUDIO_PORT_HANDLE_NONE)
{
}

AudioRecord::AudioRecord(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        const String16& opPackageName,
        size_t frameCount,
        callback_t cbf,
        void* user,
        uint32_t notificationFrames,
        audio_session_t sessionId,
        transfer_type transferType,
        audio_input_flags_t flags,
        int uid,
        pid_t pid,
        const audio_attributes_t* pAttributes)
    : mActive(false),
      mStatus(NO_INIT),
      mOpPackageName(opPackageName),
      mSessionId(AUDIO_SESSION_ALLOCATE),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL),
      mSelectedDeviceId(AUDIO_PORT_HANDLE_NONE)
{
    mStatus = set(inputSource, sampleRate, format, channelMask, frameCount, cbf, user,
            notificationFrames, false /*threadCanCallJava*/, sessionId, transferType, flags,
            uid, pid, pAttributes);
}

AudioRecord::~AudioRecord()
{
    if (mStatus == NO_ERROR) {
        // Make sure that callback function exits in the case where
        // it is looping on buffer empty condition in obtainBuffer().
        // Otherwise the callback thread will never exit.
        stop();
        if (mAudioRecordThread != 0) {
            mProxy->interrupt();
            mAudioRecordThread->requestExit();  // see comment in AudioRecord.h
            mAudioRecordThread->requestExitAndWait();
            mAudioRecordThread.clear();
        }
        // No lock here: worst case we remove a NULL callback which will be a nop
        if (mDeviceCallback != 0 && mInput != AUDIO_IO_HANDLE_NONE) {
            AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mInput);
        }
        IInterface::asBinder(mAudioRecord)->unlinkToDeath(mDeathNotifier, this);
        mAudioRecord.clear();
        mCblkMemory.clear();
        mBufferMemory.clear();
        IPCThreadState::self()->flushCommands();
        ALOGV("~AudioRecord, releasing session id %d",
                mSessionId);
        AudioSystem::releaseAudioSessionId(mSessionId, -1 /*pid*/);
    }
}

status_t AudioRecord::set(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        callback_t cbf,
        void* user,
        uint32_t notificationFrames,
        bool threadCanCallJava,
        audio_session_t sessionId,
        transfer_type transferType,
        audio_input_flags_t flags,
        int uid,
        pid_t pid,
        const audio_attributes_t* pAttributes)
{
    ALOGV("set(): inputSource %d, sampleRate %u, format %#x, channelMask %#x, frameCount %zu, "
          "notificationFrames %u, sessionId %d, transferType %d, flags %#x, opPackageName %s "
          "uid %d, pid %d",
          inputSource, sampleRate, format, channelMask, frameCount, notificationFrames,
          sessionId, transferType, flags, String8(mOpPackageName).string(), uid, pid);

    switch (transferType) {
    case TRANSFER_DEFAULT:
        if (cbf == NULL || threadCanCallJava) {
            transferType = TRANSFER_SYNC;
        } else {
            transferType = TRANSFER_CALLBACK;
        }
        break;
    case TRANSFER_CALLBACK:
        if (cbf == NULL) {
            ALOGE("Transfer type TRANSFER_CALLBACK but cbf == NULL");
            return BAD_VALUE;
        }
        break;
    case TRANSFER_OBTAIN:
    case TRANSFER_SYNC:
        break;
    default:
        ALOGE("Invalid transfer type %d", transferType);
        return BAD_VALUE;
    }
    mTransfer = transferType;

    // invariant that mAudioRecord != 0 is true only after set() returns successfully
    if (mAudioRecord != 0) {
        ALOGE("Track already in use");
        return INVALID_OPERATION;
    }

    if (pAttributes == NULL) {
        memset(&mAttributes, 0, sizeof(audio_attributes_t));
        mAttributes.source = inputSource;
    } else {
        // stream type shouldn't be looked at, this track has audio attributes
        memcpy(&mAttributes, pAttributes, sizeof(audio_attributes_t));
        ALOGV("Building AudioRecord with attributes: source=%d flags=0x%x tags=[%s]",
              mAttributes.source, mAttributes.flags, mAttributes.tags);
    }

    mSampleRate = sampleRate;

    // these below should probably come from the audioFlinger too...
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }

    // validate parameters
    // AudioFlinger capture only supports linear PCM
    if (!audio_is_valid_format(format) || !audio_is_linear_pcm(format)) {
        ALOGE("Format %#x is not linear pcm", format);
        return BAD_VALUE;
    }
    mFormat = format;

    if (!audio_is_input_channel(channelMask)) {
        ALOGE("Invalid channel mask %#x", channelMask);
        return BAD_VALUE;
    }
    mChannelMask = channelMask;
    uint32_t channelCount = audio_channel_count_from_in_mask(channelMask);
    mChannelCount = channelCount;

    if (audio_is_linear_pcm(format)) {
        mFrameSize = channelCount * audio_bytes_per_sample(format);
    } else {
        mFrameSize = sizeof(uint8_t);
    }

    // mFrameCount is initialized in openRecord_l
    mReqFrameCount = frameCount;

    mNotificationFramesReq = notificationFrames;
    // mNotificationFramesAct is initialized in openRecord_l

    if (sessionId == AUDIO_SESSION_ALLOCATE) {
        mSessionId = (audio_session_t) AudioSystem::newAudioUniqueId(AUDIO_UNIQUE_ID_USE_SESSION);
    } else {
        mSessionId = sessionId;
    }
    ALOGV("set(): mSessionId %d", mSessionId);

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

    mOrigFlags = mFlags = flags;
    mCbf = cbf;

    if (cbf != NULL) {
        mAudioRecordThread = new AudioRecordThread(*this, threadCanCallJava);
        mAudioRecordThread->run("AudioRecord", ANDROID_PRIORITY_AUDIO);
        // thread begins in paused state, and will not reference us until start()
    }

    // create the IAudioRecord
    status_t status = openRecord_l(0 /*epoch*/, mOpPackageName);

    if (status != NO_ERROR) {
        if (mAudioRecordThread != 0) {
            mAudioRecordThread->requestExit();   // see comment in AudioRecord.h
            mAudioRecordThread->requestExitAndWait();
            mAudioRecordThread.clear();
        }
        return status;
    }

    mStatus = NO_ERROR;
    mUserData = user;
    // TODO: add audio hardware input latency here
    mLatency = (1000 * mFrameCount) / mSampleRate;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    AudioSystem::acquireAudioSessionId(mSessionId, -1);
    mSequence = 1;
    mObservedSequence = mSequence;
    mInOverrun = false;
    mFramesRead = 0;
    mFramesReadServerOffset = 0;

    return NO_ERROR;
}

// -------------------------------------------------------------------------

status_t AudioRecord::start(AudioSystem::sync_event_t event, audio_session_t triggerSession)
{
    ALOGV("start, sync event %d trigger session %d", event, triggerSession);

    AutoMutex lock(mLock);
    if (mActive) {
        return NO_ERROR;
    }

    // discard data in buffer
    const uint32_t framesFlushed = mProxy->flush();
    mFramesReadServerOffset -= mFramesRead + framesFlushed;
    mFramesRead = 0;
    mProxy->clearTimestamp();  // timestamp is invalid until next server push

    // reset current position as seen by client to 0
    mProxy->setEpoch(mProxy->getEpoch() - mProxy->getPosition());
    // force refresh of remaining frames by processAudioBuffer() as last
    // read before stop could be partial.
    mRefreshRemaining = true;

    mNewPosition = mProxy->getPosition() + mUpdatePeriod;
    int32_t flags = android_atomic_acquire_load(&mCblk->mFlags);

    // we reactivate markers (mMarkerPosition != 0) as the position is reset to 0.
    // This is legacy behavior.  This is not done in stop() to avoid a race condition
    // where the last marker event is issued twice.
    mMarkerReached = false;
    mActive = true;

    status_t status = NO_ERROR;
    if (!(flags & CBLK_INVALID)) {
        status = mAudioRecord->start(event, triggerSession);
        if (status == DEAD_OBJECT) {
            flags |= CBLK_INVALID;
        }
    }
    if (flags & CBLK_INVALID) {
        status = restoreRecord_l("start");
    }

    if (status != NO_ERROR) {
        mActive = false;
        ALOGE("start() status %d", status);
    } else {
        sp<AudioRecordThread> t = mAudioRecordThread;
        if (t != 0) {
            t->resume();
        } else {
            mPreviousPriority = getpriority(PRIO_PROCESS, 0);
            get_sched_policy(0, &mPreviousSchedulingGroup);
            androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
        }
    }

    return status;
}

void AudioRecord::stop()
{
    AutoMutex lock(mLock);
    if (!mActive) {
        return;
    }

    mActive = false;
    mProxy->interrupt();
    mAudioRecord->stop();

    // Note: legacy handling - stop does not clear record marker and
    // periodic update position; we update those on start().

    sp<AudioRecordThread> t = mAudioRecordThread;
    if (t != 0) {
        t->pause();
    } else {
        setpriority(PRIO_PROCESS, 0, mPreviousPriority);
        set_sched_policy(0, mPreviousSchedulingGroup);
    }
}

bool AudioRecord::stopped() const
{
    AutoMutex lock(mLock);
    return !mActive;
}

status_t AudioRecord::setMarkerPosition(uint32_t marker)
{
    // The only purpose of setting marker position is to get a callback
    if (mCbf == NULL) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);
    mMarkerPosition = marker;
    mMarkerReached = false;

    sp<AudioRecordThread> t = mAudioRecordThread;
    if (t != 0) {
        t->wake();
    }
    return NO_ERROR;
}

status_t AudioRecord::getMarkerPosition(uint32_t *marker) const
{
    if (marker == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mMarkerPosition.getValue(marker);

    return NO_ERROR;
}

status_t AudioRecord::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    // The only purpose of setting position update period is to get a callback
    if (mCbf == NULL) {
        return INVALID_OPERATION;
    }

    AutoMutex lock(mLock);
    mNewPosition = mProxy->getPosition() + updatePeriod;
    mUpdatePeriod = updatePeriod;

    sp<AudioRecordThread> t = mAudioRecordThread;
    if (t != 0) {
        t->wake();
    }
    return NO_ERROR;
}

status_t AudioRecord::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (updatePeriod == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioRecord::getPosition(uint32_t *position) const
{
    if (position == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mProxy->getPosition().getValue(position);

    return NO_ERROR;
}

uint32_t AudioRecord::getInputFramesLost() const
{
    // no need to check mActive, because if inactive this will return 0, which is what we want
    return AudioSystem::getInputFramesLost(getInputPrivate());
}

status_t AudioRecord::getTimestamp(ExtendedTimestamp *timestamp)
{
    if (timestamp == nullptr) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    status_t status = mProxy->getTimestamp(timestamp);
    if (status == OK) {
        timestamp->mPosition[ExtendedTimestamp::LOCATION_CLIENT] = mFramesRead;
        timestamp->mTimeNs[ExtendedTimestamp::LOCATION_CLIENT] = 0;
        // server side frame offset in case AudioRecord has been restored.
        for (int i = ExtendedTimestamp::LOCATION_SERVER;
                i < ExtendedTimestamp::LOCATION_MAX; ++i) {
            if (timestamp->mTimeNs[i] >= 0) {
                timestamp->mPosition[i] += mFramesReadServerOffset;
            }
        }
    }
    return status;
}

// ---- Explicit Routing ---------------------------------------------------
status_t AudioRecord::setInputDevice(audio_port_handle_t deviceId) {
    AutoMutex lock(mLock);
    if (mSelectedDeviceId != deviceId) {
        mSelectedDeviceId = deviceId;
        // stop capture so that audio policy manager does not reject the new instance start request
        // as only one capture can be active at a time.
        if (mAudioRecord != 0 && mActive) {
            mAudioRecord->stop();
        }
        android_atomic_or(CBLK_INVALID, &mCblk->mFlags);
    }
    return NO_ERROR;
}

audio_port_handle_t AudioRecord::getInputDevice() {
    AutoMutex lock(mLock);
    return mSelectedDeviceId;
}

audio_port_handle_t AudioRecord::getRoutedDeviceId() {
    AutoMutex lock(mLock);
    if (mInput == AUDIO_IO_HANDLE_NONE) {
        return AUDIO_PORT_HANDLE_NONE;
    }
    return AudioSystem::getDeviceIdForIo(mInput);
}

// -------------------------------------------------------------------------

// must be called with mLock held
status_t AudioRecord::openRecord_l(const Modulo<uint32_t> &epoch, const String16& opPackageName)
{
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        ALOGE("Could not get audioflinger");
        return NO_INIT;
    }

    if (mDeviceCallback != 0 && mInput != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mInput);
    }
    audio_io_handle_t input;

    // mFlags (not mOrigFlags) is modified depending on whether fast request is accepted.
    // After fast request is denied, we will request again if IAudioRecord is re-created.

    status_t status;

    // Not a conventional loop, but a retry loop for at most two iterations total.
    // Try first maybe with FAST flag then try again without FAST flag if that fails.
    // Exits loop normally via a return at the bottom, or with error via a break.
    // The sp<> references will be dropped when re-entering scope.
    // The lack of indentation is deliberate, to reduce code churn and ease merges.
    for (;;) {

    status = AudioSystem::getInputForAttr(&mAttributes, &input,
                                        mSessionId,
                                        // FIXME compare to AudioTrack
                                        mClientPid,
                                        mClientUid,
                                        mSampleRate, mFormat, mChannelMask,
                                        mFlags, mSelectedDeviceId);

    if (status != NO_ERROR || input == AUDIO_IO_HANDLE_NONE) {
        ALOGE("Could not get audio input for session %d, record source %d, sample rate %u, "
              "format %#x, channel mask %#x, flags %#x",
              mSessionId, mAttributes.source, mSampleRate, mFormat, mChannelMask, mFlags);
        return BAD_VALUE;
    }

    // Now that we have a reference to an I/O handle and have not yet handed it off to AudioFlinger,
    // we must release it ourselves if anything goes wrong.

#if 0
    size_t afFrameCount;
    status = AudioSystem::getFrameCount(input, &afFrameCount);
    if (status != NO_ERROR) {
        ALOGE("getFrameCount(input=%d) status %d", input, status);
        break;
    }
#endif

    uint32_t afSampleRate;
    status = AudioSystem::getSamplingRate(input, &afSampleRate);
    if (status != NO_ERROR) {
        ALOGE("getSamplingRate(input=%d) status %d", input, status);
        break;
    }
    if (mSampleRate == 0) {
        mSampleRate = afSampleRate;
    }

    // Client can only express a preference for FAST.  Server will perform additional tests.
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        bool useCaseAllowed =
            // either of these use cases:
            // use case 1: callback transfer mode
            (mTransfer == TRANSFER_CALLBACK) ||
            // use case 2: obtain/release mode
            (mTransfer == TRANSFER_OBTAIN);
        // sample rates must also match
        bool fastAllowed = useCaseAllowed && (mSampleRate == afSampleRate);
        if (!fastAllowed) {
            ALOGW("AUDIO_INPUT_FLAG_FAST denied by client; transfer %d, "
                "track %u Hz, input %u Hz",
                mTransfer, mSampleRate, afSampleRate);
            mFlags = (audio_input_flags_t) (mFlags & ~(AUDIO_INPUT_FLAG_FAST |
                    AUDIO_INPUT_FLAG_RAW));
            AudioSystem::releaseInput(input, mSessionId);
            continue;   // retry
        }
    }

    // The notification frame count is the period between callbacks, as suggested by the client
    // but moderated by the server.  For record, the calculations are done entirely on server side.
    size_t notificationFrames = mNotificationFramesReq;
    size_t frameCount = mReqFrameCount;

    IAudioFlinger::track_flags_t trackFlags = IAudioFlinger::TRACK_DEFAULT;

    pid_t tid = -1;
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        trackFlags |= IAudioFlinger::TRACK_FAST;
        if (mAudioRecordThread != 0) {
            tid = mAudioRecordThread->getTid();
        }
    }

    size_t temp = frameCount;   // temp may be replaced by a revised value of frameCount,
                                // but we will still need the original value also
    audio_session_t originalSessionId = mSessionId;

    sp<IMemory> iMem;           // for cblk
    sp<IMemory> bufferMem;
    sp<IAudioRecord> record = audioFlinger->openRecord(input,
                                                       mSampleRate,
                                                       mFormat,
                                                       mChannelMask,
                                                       opPackageName,
                                                       &temp,
                                                       &trackFlags,
                                                       mClientPid,
                                                       tid,
                                                       mClientUid,
                                                       &mSessionId,
                                                       &notificationFrames,
                                                       iMem,
                                                       bufferMem,
                                                       &status);
    ALOGE_IF(originalSessionId != AUDIO_SESSION_ALLOCATE && mSessionId != originalSessionId,
            "session ID changed from %d to %d", originalSessionId, mSessionId);

    if (status != NO_ERROR) {
        ALOGE("AudioFlinger could not create record track, status: %d", status);
        break;
    }
    ALOG_ASSERT(record != 0);

    // AudioFlinger now owns the reference to the I/O handle,
    // so we are no longer responsible for releasing it.

    mAwaitBoost = false;
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        if (trackFlags & IAudioFlinger::TRACK_FAST) {
            ALOGI("AUDIO_INPUT_FLAG_FAST successful; frameCount %zu", frameCount);
            mAwaitBoost = true;
        } else {
            ALOGW("AUDIO_INPUT_FLAG_FAST denied by server; frameCount %zu", frameCount);
            mFlags = (audio_input_flags_t) (mFlags & ~(AUDIO_INPUT_FLAG_FAST |
                    AUDIO_INPUT_FLAG_RAW));
            continue;   // retry
        }
    }

    if (iMem == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    void *iMemPointer = iMem->pointer();
    if (iMemPointer == NULL) {
        ALOGE("Could not get control block pointer");
        return NO_INIT;
    }
    audio_track_cblk_t* cblk = static_cast<audio_track_cblk_t*>(iMemPointer);

    // Starting address of buffers in shared memory.
    // The buffers are either immediately after the control block,
    // or in a separate area at discretion of server.
    void *buffers;
    if (bufferMem == 0) {
        buffers = cblk + 1;
    } else {
        buffers = bufferMem->pointer();
        if (buffers == NULL) {
            ALOGE("Could not get buffer pointer");
            return NO_INIT;
        }
    }

    // invariant that mAudioRecord != 0 is true only after set() returns successfully
    if (mAudioRecord != 0) {
        IInterface::asBinder(mAudioRecord)->unlinkToDeath(mDeathNotifier, this);
        mDeathNotifier.clear();
    }
    mAudioRecord = record;
    mCblkMemory = iMem;
    mBufferMemory = bufferMem;
    IPCThreadState::self()->flushCommands();

    mCblk = cblk;
    // note that temp is the (possibly revised) value of frameCount
    if (temp < frameCount || (frameCount == 0 && temp == 0)) {
        ALOGW("Requested frameCount %zu but received frameCount %zu", frameCount, temp);
    }
    frameCount = temp;

    // Make sure that application is notified with sufficient margin before overrun.
    // The computation is done on server side.
    if (mNotificationFramesReq > 0 && notificationFrames != mNotificationFramesReq) {
        ALOGW("Server adjusted notificationFrames from %u to %zu for frameCount %zu",
                mNotificationFramesReq, notificationFrames, frameCount);
    }
    mNotificationFramesAct = (uint32_t) notificationFrames;

    // We retain a copy of the I/O handle, but don't own the reference
    mInput = input;
    mRefreshRemaining = true;

    mFrameCount = frameCount;
    // If IAudioRecord is re-created, don't let the requested frameCount
    // decrease.  This can confuse clients that cache frameCount().
    if (frameCount > mReqFrameCount) {
        mReqFrameCount = frameCount;
    }

    // update proxy
    mProxy = new AudioRecordClientProxy(cblk, buffers, mFrameCount, mFrameSize);
    mProxy->setEpoch(epoch);
    mProxy->setMinimum(mNotificationFramesAct);

    mDeathNotifier = new DeathNotifier(this);
    IInterface::asBinder(mAudioRecord)->linkToDeath(mDeathNotifier, this);

    if (mDeviceCallback != 0) {
        AudioSystem::addAudioDeviceCallback(mDeviceCallback, mInput);
    }

    return NO_ERROR;

    // End of retry loop.
    // The lack of indentation is deliberate, to reduce code churn and ease merges.
    }

// Arrive here on error, via a break
    AudioSystem::releaseInput(input, mSessionId);
    if (status == NO_ERROR) {
        status = NO_INIT;
    }
    return status;
}

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, int32_t waitCount, size_t *nonContig)
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

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, const struct timespec *requested,
        struct timespec *elapsed, size_t *nonContig)
{
    // previous and new IAudioRecord sequence numbers are used to detect track re-creation
    uint32_t oldSequence = 0;
    uint32_t newSequence;

    Proxy::Buffer buffer;
    status_t status = NO_ERROR;

    static const int32_t kMaxTries = 5;
    int32_t tryCounter = kMaxTries;

    do {
        // obtainBuffer() is called with mutex unlocked, so keep extra references to these fields to
        // keep them from going away if another thread re-creates the track during obtainBuffer()
        sp<AudioRecordClientProxy> proxy;
        sp<IMemory> iMem;
        sp<IMemory> bufferMem;
        {
            // start of lock scope
            AutoMutex lock(mLock);

            newSequence = mSequence;
            // did previous obtainBuffer() fail due to media server death or voluntary invalidation?
            if (status == DEAD_OBJECT) {
                // re-create track, unless someone else has already done so
                if (newSequence == oldSequence) {
                    status = restoreRecord_l("obtainBuffer");
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
            bufferMem = mBufferMemory;

            // Non-blocking if track is stopped
            if (!mActive) {
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

void AudioRecord::releaseBuffer(const Buffer* audioBuffer)
{
    // FIXME add error checking on mode, by adding an internal version

    size_t stepCount = audioBuffer->size / mFrameSize;
    if (stepCount == 0) {
        return;
    }

    Proxy::Buffer buffer;
    buffer.mFrameCount = stepCount;
    buffer.mRaw = audioBuffer->raw;

    AutoMutex lock(mLock);
    mInOverrun = false;
    mProxy->releaseBuffer(&buffer);

    // the server does not automatically disable recorder on overrun, so no need to restart
}

audio_io_handle_t AudioRecord::getInputPrivate() const
{
    AutoMutex lock(mLock);
    return mInput;
}

// -------------------------------------------------------------------------

ssize_t AudioRecord::read(void* buffer, size_t userSize, bool blocking)
{
    if (mTransfer != TRANSFER_SYNC) {
        return INVALID_OPERATION;
    }

    if (ssize_t(userSize) < 0 || (buffer == NULL && userSize != 0)) {
        // sanity-check. user is most-likely passing an error code, and it would
        // make the return value ambiguous (actualSize vs error).
        ALOGE("AudioRecord::read(buffer=%p, size=%zu (%zu)", buffer, userSize, userSize);
        return BAD_VALUE;
    }

    ssize_t read = 0;
    Buffer audioBuffer;

    while (userSize >= mFrameSize) {
        audioBuffer.frameCount = userSize / mFrameSize;

        status_t err = obtainBuffer(&audioBuffer,
                blocking ? &ClientProxy::kForever : &ClientProxy::kNonBlocking);
        if (err < 0) {
            if (read > 0) {
                break;
            }
            return ssize_t(err);
        }

        size_t bytesRead = audioBuffer.size;
        memcpy(buffer, audioBuffer.i8, bytesRead);
        buffer = ((char *) buffer) + bytesRead;
        userSize -= bytesRead;
        read += bytesRead;

        releaseBuffer(&audioBuffer);
    }
    if (read > 0) {
        mFramesRead += read / mFrameSize;
        // mFramesReadTime = systemTime(SYSTEM_TIME_MONOTONIC); // not provided at this time.
    }
    return read;
}

// -------------------------------------------------------------------------

nsecs_t AudioRecord::processAudioBuffer()
{
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
    int32_t flags = android_atomic_and(~CBLK_OVERRUN, &mCblk->mFlags);

    // Check for track invalidation
    if (flags & CBLK_INVALID) {
        (void) restoreRecord_l("processAudioBuffer");
        mLock.unlock();
        // Run again immediately, but with a new IAudioRecord
        return 0;
    }

    bool active = mActive;

    // Manage overrun callback, must be done under lock to avoid race with releaseBuffer()
    bool newOverrun = false;
    if (flags & CBLK_OVERRUN) {
        if (!mInOverrun) {
            mInOverrun = true;
            newOverrun = true;
        }
    }

    // Get current position of server
    Modulo<uint32_t> position(mProxy->getPosition());

    // Manage marker callback
    bool markerReached = false;
    Modulo<uint32_t> markerPosition(mMarkerPosition);
    // FIXME fails for wraparound, need 64 bits
    if (!mMarkerReached && markerPosition.value() > 0 && position >= markerPosition) {
        mMarkerReached = markerReached = true;
    }

    // Determine the number of new position callback(s) that will be needed, while locked
    size_t newPosCount = 0;
    Modulo<uint32_t> newPosition(mNewPosition);
    uint32_t updatePeriod = mUpdatePeriod;
    // FIXME fails for wraparound, need 64 bits
    if (updatePeriod > 0 && position >= newPosition) {
        newPosCount = ((position - newPosition).value() / updatePeriod) + 1;
        mNewPosition += updatePeriod * newPosCount;
    }

    // Cache other fields that will be needed soon
    uint32_t notificationFrames = mNotificationFramesAct;
    if (mRefreshRemaining) {
        mRefreshRemaining = false;
        mRemainingFrames = notificationFrames;
        mRetryOnPartialBuffer = false;
    }
    size_t misalignment = mProxy->getMisalignment();
    uint32_t sequence = mSequence;

    // These fields don't need to be cached, because they are assigned only by set():
    //      mTransfer, mCbf, mUserData, mSampleRate, mFrameSize

    mLock.unlock();

    // perform callbacks while unlocked
    if (newOverrun) {
        mCbf(EVENT_OVERRUN, mUserData, NULL);
    }
    if (markerReached) {
        mCbf(EVENT_MARKER, mUserData, &markerPosition);
    }
    while (newPosCount > 0) {
        size_t temp = newPosition.value(); // FIXME size_t != uint32_t
        mCbf(EVENT_NEW_POS, mUserData, &temp);
        newPosition += updatePeriod;
        newPosCount--;
    }
    if (mObservedSequence != sequence) {
        mObservedSequence = sequence;
        mCbf(EVENT_NEW_IAUDIORECORD, mUserData, NULL);
    }

    // if inactive, then don't run me again until re-started
    if (!active) {
        return NS_INACTIVE;
    }

    // Compute the estimated time until the next timed event (position, markers)
    uint32_t minFrames = ~0;
    if (!markerReached && position < markerPosition) {
        minFrames = (markerPosition - position).value();
    }
    if (updatePeriod > 0) {
        uint32_t remaining = (newPosition - position).value();
        if (remaining < minFrames) {
            minFrames = remaining;
        }
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
        ns = ((minFrames * 1000000000LL) / mSampleRate) + kFudgeNs;
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

    size_t readFrames = 0;
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
            if (err == TIMED_OUT || err == WOULD_BLOCK || err == -EINTR) {
                break;
            }
            ALOGE("Error %d obtaining an audio buffer, giving up.", err);
            return NS_NEVER;
        }

        if (mRetryOnPartialBuffer) {
            mRetryOnPartialBuffer = false;
            if (avail < mRemainingFrames) {
                int64_t myns = ((mRemainingFrames - avail) *
                        1100000000LL) / mSampleRate;
                if (ns < 0 || myns < ns) {
                    ns = myns;
                }
                return ns;
            }
        }

        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        size_t readSize = audioBuffer.size;

        // Sanity check on returned size
        if (ssize_t(readSize) < 0 || readSize > reqSize) {
            ALOGE("EVENT_MORE_DATA requested %zu bytes but callback returned %zd bytes",
                    reqSize, ssize_t(readSize));
            return NS_NEVER;
        }

        if (readSize == 0) {
            // The callback is done consuming buffers
            // Keep this thread going to handle timed events and
            // still try to provide more data in intervals of WAIT_PERIOD_MS
            // but don't just loop and block the CPU, so wait
            return WAIT_PERIOD_MS * 1000000LL;
        }

        size_t releasedFrames = readSize / mFrameSize;
        audioBuffer.frameCount = releasedFrames;
        mRemainingFrames -= releasedFrames;
        if (misalignment >= releasedFrames) {
            misalignment -= releasedFrames;
        } else {
            misalignment = 0;
        }

        releaseBuffer(&audioBuffer);
        readFrames += releasedFrames;

        // FIXME here is where we would repeat EVENT_MORE_DATA again on same advanced buffer
        // if callback doesn't like to accept the full chunk
        if (readSize < reqSize) {
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
            return (mRemainingFrames * 1100000000LL) / mSampleRate;
        }
#endif

    }
    if (readFrames > 0) {
        AutoMutex lock(mLock);
        mFramesRead += readFrames;
        // mFramesReadTime = systemTime(SYSTEM_TIME_MONOTONIC); // not provided at this time.
    }
    mRemainingFrames = notificationFrames;
    mRetryOnPartialBuffer = true;

    // A lot has transpired since ns was calculated, so run again immediately and re-calculate
    return 0;
}

status_t AudioRecord::restoreRecord_l(const char *from)
{
    ALOGW("dead IAudioRecord, creating a new one from %s()", from);
    ++mSequence;

    mFlags = mOrigFlags;

    // if the new IAudioRecord is created, openRecord_l() will modify the
    // following member variables: mAudioRecord, mCblkMemory, mCblk, mBufferMemory.
    // It will also delete the strong references on previous IAudioRecord and IMemory
    Modulo<uint32_t> position(mProxy->getPosition());
    mNewPosition = position + mUpdatePeriod;
    status_t result = openRecord_l(position, mOpPackageName);
    if (result == NO_ERROR) {
        if (mActive) {
            // callback thread or sync event hasn't changed
            // FIXME this fails if we have a new AudioFlinger instance
            result = mAudioRecord->start(AudioSystem::SYNC_EVENT_SAME, AUDIO_SESSION_NONE);
        }
        mFramesReadServerOffset = mFramesRead; // server resets to zero so we need an offset.
    }
    if (result != NO_ERROR) {
        ALOGW("restoreRecord_l() failed status %d", result);
        mActive = false;
    }

    return result;
}

status_t AudioRecord::addAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback)
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
    if (mInput != AUDIO_IO_HANDLE_NONE) {
        if (mDeviceCallback != 0) {
            ALOGW("%s callback already present!", __FUNCTION__);
            AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mInput);
        }
        status = AudioSystem::addAudioDeviceCallback(callback, mInput);
    }
    mDeviceCallback = callback;
    return status;
}

status_t AudioRecord::removeAudioDeviceCallback(
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
    if (mInput != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::removeAudioDeviceCallback(mDeviceCallback, mInput);
    }
    mDeviceCallback = 0;
    return NO_ERROR;
}

// =========================================================================

void AudioRecord::DeathNotifier::binderDied(const wp<IBinder>& who __unused)
{
    sp<AudioRecord> audioRecord = mAudioRecord.promote();
    if (audioRecord != 0) {
        AutoMutex lock(audioRecord->mLock);
        audioRecord->mProxy->binderDied();
    }
}

// =========================================================================

AudioRecord::AudioRecordThread::AudioRecordThread(AudioRecord& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver), mPaused(true), mPausedInt(false), mPausedNs(0LL),
      mIgnoreNextPausedInt(false)
{
}

AudioRecord::AudioRecordThread::~AudioRecordThread()
{
}

bool AudioRecord::AudioRecordThread::threadLoop()
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
    nsecs_t ns =  mReceiver.processAudioBuffer();
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

void AudioRecord::AudioRecordThread::requestExit()
{
    // must be in this order to avoid a race condition
    Thread::requestExit();
    resume();
}

void AudioRecord::AudioRecordThread::pause()
{
    AutoMutex _l(mMyLock);
    mPaused = true;
}

void AudioRecord::AudioRecordThread::resume()
{
    AutoMutex _l(mMyLock);
    mIgnoreNextPausedInt = true;
    if (mPaused || mPausedInt) {
        mPaused = false;
        mPausedInt = false;
        mMyCond.signal();
    }
}

void AudioRecord::AudioRecordThread::wake()
{
    AutoMutex _l(mMyLock);
    if (!mPaused) {
        // wake() might be called while servicing a callback - ignore the next
        // pause time and call processAudioBuffer.
        mIgnoreNextPausedInt = true;
        if (mPausedInt && mPausedNs > 0) {
            // audio record is active and internally paused with timeout.
            mPausedInt = false;
            mMyCond.signal();
        }
    }
}

void AudioRecord::AudioRecordThread::pauseInternal(nsecs_t ns)
{
    AutoMutex _l(mMyLock);
    mPausedInt = true;
    mPausedNs = ns;
}

// -------------------------------------------------------------------------

} // namespace android
