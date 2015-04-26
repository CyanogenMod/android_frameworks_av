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

    // handle non-linear-pcm formats and update frameCount
    if (!audio_is_linear_pcm(format)) {
        *frameCount = (size * 2) / sizeof(uint8_t);
        return NO_ERROR;
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

AudioRecord::AudioRecord()
    : mStatus(NO_INIT), mSessionId(AUDIO_SESSION_ALLOCATE),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL), mPreviousSchedulingGroup(SP_DEFAULT)
{
}

AudioRecord::AudioRecord(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        callback_t cbf,
        void* user,
        uint32_t notificationFrames,
        int sessionId,
        transfer_type transferType,
        audio_input_flags_t flags,
        const audio_attributes_t* pAttributes)
    : mStatus(NO_INIT), mSessionId(AUDIO_SESSION_ALLOCATE),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL)
{
    mStatus = set(inputSource, sampleRate, format, channelMask, frameCount, cbf, user,
            notificationFrames, false /*threadCanCallJava*/, sessionId, transferType, flags,
            pAttributes);
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
        mAudioRecord->asBinder()->unlinkToDeath(mDeathNotifier, this);
        mAudioRecord.clear();
        mCblkMemory.clear();
        mBufferMemory.clear();
        IPCThreadState::self()->flushCommands();
        AudioSystem::releaseAudioSessionId(mSessionId, -1);
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
        int sessionId,
        transfer_type transferType,
        audio_input_flags_t flags,
        const audio_attributes_t* pAttributes)
{
    ALOGV("set(): inputSource %d, sampleRate %u, format %#x, channelMask %#x, frameCount %zu, "
          "notificationFrames %u, sessionId %d, transferType %d, flags %#x",
          inputSource, sampleRate, format, channelMask, frameCount, notificationFrames,
          sessionId, transferType, flags);

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

    AutoMutex lock(mLock);

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

    if (sampleRate == 0) {
        ALOGE("Invalid sample rate %u", sampleRate);
        return BAD_VALUE;
    }
    mSampleRate = sampleRate;

    // these below should probably come from the audioFlinger too...
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }

    // validate parameters
    if (!audio_is_valid_format(format)) {
        ALOGE("Invalid format %#x", format);
        return BAD_VALUE;
    }
    // Temporary restriction: AudioFlinger currently supports 16-bit PCM and compress formats only
    if (format != AUDIO_FORMAT_PCM_16_BIT &&
           !audio_is_compress_voip_format(format) &&
           !audio_is_compress_capture_format(format)) {
        ALOGE("Format %#x is not supported", format);
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
        mSessionId = AudioSystem::newAudioUniqueId();
    } else {
        mSessionId = sessionId;
    }
    ALOGV("set(): mSessionId %d", mSessionId);

    mFlags = flags;
    mCbf = cbf;

    if (cbf != NULL) {
        mAudioRecordThread = new AudioRecordThread(*this, threadCanCallJava);
        mAudioRecordThread->run("AudioRecord", ANDROID_PRIORITY_AUDIO);
    }

    // create the IAudioRecord
    status_t status = openRecord_l(0 /*epoch*/);

    if (status != NO_ERROR) {
        if (mAudioRecordThread != 0) {
            mAudioRecordThread->requestExit();   // see comment in AudioRecord.h
            mAudioRecordThread->requestExitAndWait();
            mAudioRecordThread.clear();
        }
        return status;
    }

    mStatus = NO_ERROR;
    mActive = false;
    mUserData = user;
    // TODO: add audio hardware input latency here
    mLatency = (1000*mFrameCount) / sampleRate;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    AudioSystem::acquireAudioSessionId(mSessionId, -1);
    mSequence = 1;
    mObservedSequence = mSequence;
    mInOverrun = false;

    return NO_ERROR;
}

// -------------------------------------------------------------------------

status_t AudioRecord::start(AudioSystem::sync_event_t event, int triggerSession)
{
    ALOGV("start, sync event %d trigger session %d", event, triggerSession);

    AutoMutex lock(mLock);
    if (mActive) {
        return NO_ERROR;
    }

    // reset current position as seen by client to 0
    mProxy->setEpoch(mProxy->getEpoch() - mProxy->getPosition());
    // force refresh of remaining frames by processAudioBuffer() as last
    // read before stop could be partial.
    mRefreshRemaining = true;

    mNewPosition = mProxy->getPosition() + mUpdatePeriod;
    int32_t flags = android_atomic_acquire_load(&mCblk->mFlags);

    status_t status = NO_ERROR;
    if (!(flags & CBLK_INVALID)) {
        ALOGV("mAudioRecord->start()");
        status = mAudioRecord->start(event, triggerSession);
        if (status == DEAD_OBJECT) {
            flags |= CBLK_INVALID;
        }
    }
    if (flags & CBLK_INVALID) {
        status = restoreRecord_l("start");
    }

    if (status != NO_ERROR) {
        ALOGE("start() status %d", status);
    } else {
        mActive = true;
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
    // the record head position will reset to 0, so if a marker is set, we need
    // to activate it again
    mMarkerReached = false;
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

    return NO_ERROR;
}

status_t AudioRecord::getMarkerPosition(uint32_t *marker) const
{
    if (marker == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    *marker = mMarkerPosition;

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
    *position = mProxy->getPosition();

    return NO_ERROR;
}

uint32_t AudioRecord::getInputFramesLost() const
{
    // no need to check mActive, because if inactive this will return 0, which is what we want
    return AudioSystem::getInputFramesLost(getInput());
}

// -------------------------------------------------------------------------

// must be called with mLock held
status_t AudioRecord::openRecord_l(size_t epoch)
{
    status_t status;
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        ALOGE("Could not get audioflinger");
        return NO_INIT;
    }

    // Fast tracks must be at the primary _output_ [sic] sampling rate,
    // because there is currently no concept of a primary input sampling rate
    uint32_t afSampleRate = AudioSystem::getPrimaryOutputSamplingRate();
    if (afSampleRate == 0) {
        ALOGW("getPrimaryOutputSamplingRate failed");
    }

    // Client can only express a preference for FAST.  Server will perform additional tests.
    if ((mFlags & AUDIO_INPUT_FLAG_FAST) && !(
            // use case: callback transfer mode
            (mTransfer == TRANSFER_CALLBACK) &&
            // matching sample rate
            (mSampleRate == afSampleRate))) {
        ALOGW("AUDIO_INPUT_FLAG_FAST denied by client");
        // once denied, do not request again if IAudioRecord is re-created
        mFlags = (audio_input_flags_t) (mFlags & ~AUDIO_INPUT_FLAG_FAST);
    }

    IAudioFlinger::track_flags_t trackFlags = IAudioFlinger::TRACK_DEFAULT;

    pid_t tid = -1;
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        trackFlags |= IAudioFlinger::TRACK_FAST;
        if (mAudioRecordThread != 0) {
            tid = mAudioRecordThread->getTid();
        }
    }

    audio_io_handle_t input;
    status = AudioSystem::getInputForAttr(&mAttributes, &input, (audio_session_t)mSessionId,
                                        mSampleRate, mFormat, mChannelMask, mFlags);

    if (status != NO_ERROR) {
        ALOGE("Could not get audio input for record source %d, sample rate %u, format %#x, "
              "channel mask %#x, session %d, flags %#x",
              mAttributes.source, mSampleRate, mFormat, mChannelMask, mSessionId, mFlags);
        return BAD_VALUE;
    }
    {
    // Now that we have a reference to an I/O handle and have not yet handed it off to AudioFlinger,
    // we must release it ourselves if anything goes wrong.

    size_t frameCount = mReqFrameCount;
    size_t temp = frameCount;   // temp may be replaced by a revised value of frameCount,
                                // but we will still need the original value also
    int originalSessionId = mSessionId;

    // The notification frame count is the period between callbacks, as suggested by the server.
    size_t notificationFrames = mNotificationFramesReq;

    sp<IMemory> iMem;           // for cblk
    sp<IMemory> bufferMem;

    if (audioFlinger == NULL) {
        ALOGE("AudioFlinger was NULL!");
        return NO_INIT;
    }

    sp<IAudioRecord> record = audioFlinger->openRecord(input,
                                                       mSampleRate, mFormat,
                                                       mChannelMask,
                                                       &temp,
                                                       &trackFlags,
                                                       tid,
                                                       &mSessionId,
                                                       &notificationFrames,
                                                       iMem,
                                                       bufferMem,
                                                       &status);
    ALOGE_IF(originalSessionId != AUDIO_SESSION_ALLOCATE && mSessionId != originalSessionId,
            "session ID changed from %d to %d", originalSessionId, mSessionId);

    if (status != NO_ERROR) {
        ALOGE("AudioFlinger could not create record track, status: %d", status);
        goto release;
    }
    ALOG_ASSERT(record != 0);

    // AudioFlinger now owns the reference to the I/O handle,
    // so we are no longer responsible for releasing it.

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
        mAudioRecord->asBinder()->unlinkToDeath(mDeathNotifier, this);
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

    mAwaitBoost = false;
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        if (trackFlags & IAudioFlinger::TRACK_FAST) {
            ALOGV("AUDIO_INPUT_FLAG_FAST successful; frameCount %zu", frameCount);
            mAwaitBoost = true;
        } else {
            ALOGV("AUDIO_INPUT_FLAG_FAST denied by server; frameCount %zu", frameCount);
            // once denied, do not request again if IAudioRecord is re-created
            mFlags = (audio_input_flags_t) (mFlags & ~AUDIO_INPUT_FLAG_FAST);
        }
    }

    // Make sure that application is notified with sufficient margin before overrun
    if (notificationFrames == 0 || notificationFrames > frameCount) {
        ALOGW("Received notificationFrames %zu for frameCount %zu", notificationFrames, frameCount);
    }
    mNotificationFramesAct = notificationFrames;

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
    mAudioRecord->asBinder()->linkToDeath(mDeathNotifier, this);

    return NO_ERROR;
    }

release:
    AudioSystem::releaseInput(input, (audio_session_t)mSessionId);
    if (status == NO_ERROR) {
        status = NO_INIT;
    }
    return status;
}

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, int32_t waitCount)
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

void AudioRecord::releaseBuffer(Buffer* audioBuffer)
{
    // all TRANSFER_* are valid

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

audio_io_handle_t AudioRecord::getInput() const
{
    AutoMutex lock(mLock);
    return mInput;
}

// -------------------------------------------------------------------------

ssize_t AudioRecord::read(void* buffer, size_t userSize)
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

        status_t err = obtainBuffer(&audioBuffer, &ClientProxy::kForever);
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
    size_t position = mProxy->getPosition();

    // Manage marker callback
    bool markerReached = false;
    size_t markerPosition = mMarkerPosition;
    // FIXME fails for wraparound, need 64 bits
    if (!mMarkerReached && (markerPosition > 0) && (position >= markerPosition)) {
        mMarkerReached = markerReached = true;
    }

    // Determine the number of new position callback(s) that will be needed, while locked
    size_t newPosCount = 0;
    size_t newPosition = mNewPosition;
    uint32_t updatePeriod = mUpdatePeriod;
    // FIXME fails for wraparound, need 64 bits
    if (updatePeriod > 0 && position >= newPosition) {
        newPosCount = ((position - newPosition) / updatePeriod) + 1;
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
        size_t temp = newPosition;
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
        minFrames = markerPosition - position;
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
    mRemainingFrames = notificationFrames;
    mRetryOnPartialBuffer = true;

    // A lot has transpired since ns was calculated, so run again immediately and re-calculate
    return 0;
}

status_t AudioRecord::restoreRecord_l(const char *from)
{
    ALOGW("dead IAudioRecord, creating a new one from %s()", from);
    ++mSequence;
    status_t result;

    // if the new IAudioRecord is created, openRecord_l() will modify the
    // following member variables: mAudioRecord, mCblkMemory, mCblk, mBufferMemory.
    // It will also delete the strong references on previous IAudioRecord and IMemory
    size_t position = mProxy->getPosition();
    mNewPosition = position + mUpdatePeriod;
    result = openRecord_l(position);
    if (result == NO_ERROR) {
        if (mActive) {
            // callback thread or sync event hasn't changed
            // FIXME this fails if we have a new AudioFlinger instance
            result = mAudioRecord->start(AudioSystem::SYNC_EVENT_SAME, 0);
        }
    }
    if (result != NO_ERROR) {
        ALOGW("restoreRecord_l() failed status %d", result);
        mActive = false;
    }

    return result;
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
        // FIXME increase poll interval, or make event-driven
        ns = 1000000000LL;
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

void AudioRecord::AudioRecordThread::pauseInternal(nsecs_t ns)
{
    AutoMutex _l(mMyLock);
    mPausedInt = true;
    mPausedNs = ns;
}

// -------------------------------------------------------------------------

}; // namespace android
