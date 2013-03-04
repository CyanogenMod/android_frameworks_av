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

#include <sys/resource.h>
#include <sys/types.h>

#include <binder/IPCThreadState.h>
#include <cutils/atomic.h>
#include <cutils/compiler.h>
#include <media/AudioRecord.h>
#include <media/AudioSystem.h>
#include <system/audio.h>
#include <utils/Log.h>

#include <private/media/AudioTrackShared.h>

namespace android {
// ---------------------------------------------------------------------------

// static
status_t AudioRecord::getMinFrameCount(
        size_t* frameCount,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask)
{
    if (frameCount == NULL) return BAD_VALUE;

    // default to 0 in case of error
    *frameCount = 0;

    size_t size = 0;
    status_t status = AudioSystem::getInputBufferSize(sampleRate, format, channelMask, &size);
    if (status != NO_ERROR) {
        ALOGE("AudioSystem could not query the input buffer size; status %d", status);
        return NO_INIT;
    }

    if (size == 0) {
        ALOGE("Unsupported configuration: sampleRate %u, format %d, channelMask %#x",
            sampleRate, format, channelMask);
        return BAD_VALUE;
    }

    // We double the size of input buffer for ping pong use of record buffer.
    size <<= 1;

    if (audio_is_linear_pcm(format)) {
        uint32_t channelCount = popcount(channelMask);
        size /= channelCount * audio_bytes_per_sample(format);
    }

    *frameCount = size;
    return NO_ERROR;
}

// ---------------------------------------------------------------------------

AudioRecord::AudioRecord()
    : mStatus(NO_INIT), mSessionId(0),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL), mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL)
{
}

AudioRecord::AudioRecord(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCount,
        callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId)
    : mStatus(NO_INIT), mSessionId(0),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL),
      mPreviousSchedulingGroup(SP_DEFAULT),
      mProxy(NULL)
{
    mStatus = set(inputSource, sampleRate, format, channelMask,
            frameCount, cbf, user, notificationFrames, false /*threadCanCallJava*/, sessionId);
}

AudioRecord::~AudioRecord()
{
    if (mStatus == NO_ERROR) {
        // Make sure that callback function exits in the case where
        // it is looping on buffer empty condition in obtainBuffer().
        // Otherwise the callback thread will never exit.
        stop();
        if (mAudioRecordThread != 0) {
            mAudioRecordThread->requestExit();  // see comment in AudioRecord.h
            mAudioRecordThread->requestExitAndWait();
            mAudioRecordThread.clear();
        }
        mAudioRecord.clear();
        IPCThreadState::self()->flushCommands();
        AudioSystem::releaseAudioSessionId(mSessionId);
    }
    delete mProxy;
}

status_t AudioRecord::set(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCountInt,
        callback_t cbf,
        void* user,
        int notificationFrames,
        bool threadCanCallJava,
        int sessionId)
{
    // FIXME "int" here is legacy and will be replaced by size_t later
    if (frameCountInt < 0) {
        ALOGE("Invalid frame count %d", frameCountInt);
        return BAD_VALUE;
    }
    size_t frameCount = frameCountInt;

    ALOGV("set(): sampleRate %u, channelMask %#x, frameCount %u", sampleRate, channelMask,
            frameCount);

    AutoMutex lock(mLock);

    if (mAudioRecord != 0) {
        return INVALID_OPERATION;
    }

    if (inputSource == AUDIO_SOURCE_DEFAULT) {
        inputSource = AUDIO_SOURCE_MIC;
    }

    if (sampleRate == 0) {
        sampleRate = DEFAULT_SAMPLE_RATE;
    }
    mSampleRate = sampleRate;

    // these below should probably come from the audioFlinger too...
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }
    // validate parameters
    if (!audio_is_valid_format(format)) {
        ALOGE("Invalid format");
        return BAD_VALUE;
    }
    mFormat = format;

    if (!audio_is_input_channel(channelMask)) {
        return BAD_VALUE;
    }
    mChannelMask = channelMask;
    uint32_t channelCount = popcount(channelMask);
    mChannelCount = channelCount;

    if (audio_is_linear_pcm(format)) {
        mFrameSize = channelCount * audio_bytes_per_sample(format);
    } else {
        mFrameSize = sizeof(uint8_t);
    }

    if (sessionId == 0 ) {
        mSessionId = AudioSystem::newAudioSessionId();
    } else {
        mSessionId = sessionId;
    }
    ALOGV("set(): mSessionId %d", mSessionId);

    audio_io_handle_t input = AudioSystem::getInput(inputSource,
                                                    sampleRate,
                                                    format,
                                                    channelMask,
                                                    mSessionId);
    if (input == 0) {
        ALOGE("Could not get audio input for record source %d", inputSource);
        return BAD_VALUE;
    }

    // validate framecount
    size_t minFrameCount = 0;
    status_t status = getMinFrameCount(&minFrameCount, sampleRate, format, channelMask);
    if (status != NO_ERROR) {
        return status;
    }
    ALOGV("AudioRecord::set() minFrameCount = %d", minFrameCount);

    if (frameCount == 0) {
        frameCount = minFrameCount;
    } else if (frameCount < minFrameCount) {
        return BAD_VALUE;
    }

    if (notificationFrames == 0) {
        notificationFrames = frameCount/2;
    }

    // create the IAudioRecord
    status = openRecord_l(sampleRate, format, frameCount, input);
    if (status != NO_ERROR) {
        return status;
    }

    if (cbf != NULL) {
        mAudioRecordThread = new AudioRecordThread(*this, threadCanCallJava);
        mAudioRecordThread->run("AudioRecord", ANDROID_PRIORITY_AUDIO);
    }

    mStatus = NO_ERROR;

    // Update buffer size in case it has been limited by AudioFlinger during track creation
    mFrameCount = mCblk->frameCount_;

    mActive = false;
    mCbf = cbf;
    mNotificationFrames = notificationFrames;
    mRemainingFrames = notificationFrames;
    mUserData = user;
    // TODO: add audio hardware input latency here
    mLatency = (1000*mFrameCount) / sampleRate;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    mInputSource = inputSource;
    mInput = input;
    AudioSystem::acquireAudioSessionId(mSessionId);

    return NO_ERROR;
}

status_t AudioRecord::initCheck() const
{
    return mStatus;
}

// -------------------------------------------------------------------------

uint32_t AudioRecord::latency() const
{
    return mLatency;
}

audio_format_t AudioRecord::format() const
{
    return mFormat;
}

uint32_t AudioRecord::channelCount() const
{
    return mChannelCount;
}

size_t AudioRecord::frameCount() const
{
    return mFrameCount;
}

audio_source_t AudioRecord::inputSource() const
{
    return mInputSource;
}

// -------------------------------------------------------------------------

status_t AudioRecord::start(AudioSystem::sync_event_t event, int triggerSession)
{
    status_t ret = NO_ERROR;
    sp<AudioRecordThread> t = mAudioRecordThread;

    ALOGV("start, sync event %d trigger session %d", event, triggerSession);

    AutoMutex lock(mLock);
    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;

    if (!mActive) {
        mActive = true;

        cblk->lock.lock();
        if (!(cblk->flags & CBLK_INVALID)) {
            cblk->lock.unlock();
            ALOGV("mAudioRecord->start()");
            ret = mAudioRecord->start(event, triggerSession);
            cblk->lock.lock();
            if (ret == DEAD_OBJECT) {
                android_atomic_or(CBLK_INVALID, &cblk->flags);
            }
        }
        if (cblk->flags & CBLK_INVALID) {
            audio_track_cblk_t* temp = cblk;
            ret = restoreRecord_l(temp);
            cblk = temp;
        }
        cblk->lock.unlock();
        if (ret == NO_ERROR) {
            mNewPosition = cblk->user + mUpdatePeriod;
            cblk->bufferTimeoutMs = (event == AudioSystem::SYNC_EVENT_NONE) ? MAX_RUN_TIMEOUT_MS :
                                            AudioSystem::kSyncRecordStartTimeOutMs;
            cblk->waitTimeMs = 0;
            if (t != 0) {
                t->resume();
            } else {
                mPreviousPriority = getpriority(PRIO_PROCESS, 0);
                get_sched_policy(0, &mPreviousSchedulingGroup);
                androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
            }
        } else {
            mActive = false;
        }
    }

    return ret;
}

void AudioRecord::stop()
{
    sp<AudioRecordThread> t = mAudioRecordThread;

    ALOGV("stop");

    AutoMutex lock(mLock);
    if (mActive) {
        mActive = false;
        mCblk->cv.signal();
        mAudioRecord->stop();
        // the record head position will reset to 0, so if a marker is set, we need
        // to activate it again
        mMarkerReached = false;
        if (t != 0) {
            t->pause();
        } else {
            setpriority(PRIO_PROCESS, 0, mPreviousPriority);
            set_sched_policy(0, mPreviousSchedulingGroup);
        }
    }
}

bool AudioRecord::stopped() const
{
    AutoMutex lock(mLock);
    return !mActive;
}

uint32_t AudioRecord::getSampleRate() const
{
    return mSampleRate;
}

status_t AudioRecord::setMarkerPosition(uint32_t marker)
{
    if (mCbf == NULL) return INVALID_OPERATION;

    AutoMutex lock(mLock);
    mMarkerPosition = marker;
    mMarkerReached = false;

    return NO_ERROR;
}

status_t AudioRecord::getMarkerPosition(uint32_t *marker) const
{
    if (marker == NULL) return BAD_VALUE;

    AutoMutex lock(mLock);
    *marker = mMarkerPosition;

    return NO_ERROR;
}

status_t AudioRecord::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    if (mCbf == NULL) return INVALID_OPERATION;

    uint32_t curPosition;
    getPosition(&curPosition);

    AutoMutex lock(mLock);
    mNewPosition = curPosition + updatePeriod;
    mUpdatePeriod = updatePeriod;

    return NO_ERROR;
}

status_t AudioRecord::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (updatePeriod == NULL) return BAD_VALUE;

    AutoMutex lock(mLock);
    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioRecord::getPosition(uint32_t *position) const
{
    if (position == NULL) return BAD_VALUE;

    AutoMutex lock(mLock);
    *position = mCblk->user;

    return NO_ERROR;
}

unsigned int AudioRecord::getInputFramesLost() const
{
    // no need to check mActive, because if inactive this will return 0, which is what we want
    return AudioSystem::getInputFramesLost(mInput);
}

// -------------------------------------------------------------------------

// must be called with mLock held
status_t AudioRecord::openRecord_l(
        uint32_t sampleRate,
        audio_format_t format,
        size_t frameCount,
        audio_io_handle_t input)
{
    status_t status;
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        ALOGE("Could not get audioflinger");
        return NO_INIT;
    }

    pid_t tid = -1;
    // FIXME see similar logic at AudioTrack

    int originalSessionId = mSessionId;
    sp<IAudioRecord> record = audioFlinger->openRecord(input,
                                                       sampleRate, format,
                                                       mChannelMask,
                                                       frameCount,
                                                       IAudioFlinger::TRACK_DEFAULT,
                                                       tid,
                                                       &mSessionId,
                                                       &status);
    ALOGE_IF(originalSessionId != 0 && mSessionId != originalSessionId,
            "session ID changed from %d to %d", originalSessionId, mSessionId);

    if (record == 0) {
        ALOGE("AudioFlinger could not create record track, status: %d", status);
        return status;
    }
    sp<IMemory> iMem = record->getCblk();
    if (iMem == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    mAudioRecord.clear();
    mAudioRecord = record;
    mCblkMemory.clear();
    mCblkMemory = iMem;
    audio_track_cblk_t* cblk = static_cast<audio_track_cblk_t*>(iMem->pointer());
    mCblk = cblk;
    mBuffers = (char*)cblk + sizeof(audio_track_cblk_t);
    cblk->bufferTimeoutMs = MAX_RUN_TIMEOUT_MS;
    cblk->waitTimeMs = 0;

    // update proxy
    delete mProxy;
    mProxy = new AudioRecordClientProxy(cblk, mBuffers, frameCount, mFrameSize);

    return NO_ERROR;
}

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, int32_t waitCount)
{
    ALOG_ASSERT(mStatus == NO_ERROR && mProxy != NULL);

    AutoMutex lock(mLock);
    bool active;
    status_t result = NO_ERROR;
    audio_track_cblk_t* cblk = mCblk;
    uint32_t framesReq = audioBuffer->frameCount;
    uint32_t waitTimeMs = (waitCount < 0) ? cblk->bufferTimeoutMs : WAIT_PERIOD_MS;

    audioBuffer->frameCount  = 0;
    audioBuffer->size        = 0;

    size_t framesReady = mProxy->framesReady();

    if (framesReady == 0) {
        cblk->lock.lock();
        goto start_loop_here;
        while (framesReady == 0) {
            active = mActive;
            if (CC_UNLIKELY(!active)) {
                cblk->lock.unlock();
                return NO_MORE_BUFFERS;
            }
            if (CC_UNLIKELY(!waitCount)) {
                cblk->lock.unlock();
                return WOULD_BLOCK;
            }
            if (!(cblk->flags & CBLK_INVALID)) {
                mLock.unlock();
                // this condition is in shared memory, so if IAudioRecord and control block
                // are replaced due to mediaserver death or IAudioRecord invalidation then
                // cv won't be signalled, but fortunately the timeout will limit the wait
                result = cblk->cv.waitRelative(cblk->lock, milliseconds(waitTimeMs));
                cblk->lock.unlock();
                mLock.lock();
                if (!mActive) {
                    return status_t(STOPPED);
                }
                // IAudioRecord may have been re-created while mLock was unlocked
                cblk = mCblk;
                cblk->lock.lock();
            }
            if (cblk->flags & CBLK_INVALID) {
                goto create_new_record;
            }
            if (CC_UNLIKELY(result != NO_ERROR)) {
                cblk->waitTimeMs += waitTimeMs;
                if (cblk->waitTimeMs >= cblk->bufferTimeoutMs) {
                    ALOGW(   "obtainBuffer timed out (is the CPU pegged?) "
                            "user=%08x, server=%08x", cblk->user, cblk->server);
                    cblk->lock.unlock();
                    // callback thread or sync event hasn't changed
                    result = mAudioRecord->start(AudioSystem::SYNC_EVENT_SAME, 0);
                    cblk->lock.lock();
                    if (result == DEAD_OBJECT) {
                        android_atomic_or(CBLK_INVALID, &cblk->flags);
create_new_record:
                        audio_track_cblk_t* temp = cblk;
                        result = AudioRecord::restoreRecord_l(temp);
                        cblk = temp;
                    }
                    if (result != NO_ERROR) {
                        ALOGW("obtainBuffer create Track error %d", result);
                        cblk->lock.unlock();
                        return result;
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
            framesReady = mProxy->framesReady();
        }
        cblk->lock.unlock();
    }

    cblk->waitTimeMs = 0;
    // reset time out to running value after obtaining a buffer
    cblk->bufferTimeoutMs = MAX_RUN_TIMEOUT_MS;

    if (framesReq > framesReady) {
        framesReq = framesReady;
    }

    uint32_t u = cblk->user;
    uint32_t bufferEnd = cblk->userBase + mFrameCount;

    if (framesReq > bufferEnd - u) {
        framesReq = bufferEnd - u;
    }

    audioBuffer->frameCount  = framesReq;
    audioBuffer->size        = framesReq * mFrameSize;
    audioBuffer->raw         = mProxy->buffer(u);
    active = mActive;
    return active ? status_t(NO_ERROR) : status_t(STOPPED);
}

void AudioRecord::releaseBuffer(Buffer* audioBuffer)
{
    ALOG_ASSERT(mStatus == NO_ERROR && mProxy != NULL);

    AutoMutex lock(mLock);
    (void) mProxy->stepUser(audioBuffer->frameCount);
}

audio_io_handle_t AudioRecord::getInput() const
{
    AutoMutex lock(mLock);
    return mInput;
}

// must be called with mLock held
audio_io_handle_t AudioRecord::getInput_l()
{
    mInput = AudioSystem::getInput(mInputSource,
                                mSampleRate,
                                mFormat,
                                mChannelMask,
                                mSessionId);
    return mInput;
}

int AudioRecord::getSessionId() const
{
    // no lock needed because session ID doesn't change after first set()
    return mSessionId;
}

// -------------------------------------------------------------------------

ssize_t AudioRecord::read(void* buffer, size_t userSize)
{
    ssize_t read = 0;
    Buffer audioBuffer;
    int8_t *dst = static_cast<int8_t*>(buffer);

    if (ssize_t(userSize) < 0) {
        // sanity-check. user is most-likely passing an error code.
        ALOGE("AudioRecord::read(buffer=%p, size=%u (%d)",
                buffer, userSize, userSize);
        return BAD_VALUE;
    }

    mLock.lock();
    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    mLock.unlock();

    do {

        audioBuffer.frameCount = userSize/frameSize();

        // By using a wait count corresponding to twice the timeout period in
        // obtainBuffer() we give a chance to recover once for a read timeout
        // (if media_server crashed for instance) before returning a length of
        // 0 bytes read to the client
        status_t err = obtainBuffer(&audioBuffer, ((2 * MAX_RUN_TIMEOUT_MS) / WAIT_PERIOD_MS));
        if (err < 0) {
            // out of buffers, return #bytes written
            if (err == status_t(NO_MORE_BUFFERS)) {
                break;
            }
            if (err == status_t(TIMED_OUT)) {
                // return partial transfer count
                return read;
            }
            return ssize_t(err);
        }

        size_t bytesRead = audioBuffer.size;
        memcpy(dst, audioBuffer.i8, bytesRead);

        dst += bytesRead;
        userSize -= bytesRead;
        read += bytesRead;

        releaseBuffer(&audioBuffer);
    } while (userSize);

    return read;
}

// -------------------------------------------------------------------------

bool AudioRecord::processAudioBuffer(const sp<AudioRecordThread>& thread)
{
    Buffer audioBuffer;
    uint32_t frames = mRemainingFrames;
    size_t readSize;

    mLock.lock();
    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;
    bool active = mActive;
    uint32_t markerPosition = mMarkerPosition;
    uint32_t newPosition = mNewPosition;
    uint32_t user = cblk->user;
    // determine whether a marker callback will be needed, while locked
    bool needMarker = !mMarkerReached && (mMarkerPosition > 0) && (user >= mMarkerPosition);
    if (needMarker) {
        mMarkerReached = true;
    }
    // determine the number of new position callback(s) that will be needed, while locked
    uint32_t updatePeriod = mUpdatePeriod;
    uint32_t needNewPos = updatePeriod > 0 && user >= newPosition ?
            ((user - newPosition) / updatePeriod) + 1 : 0;
    mNewPosition = newPosition + updatePeriod * needNewPos;
    mLock.unlock();

    // perform marker callback, while unlocked
    if (needMarker) {
        mCbf(EVENT_MARKER, mUserData, &markerPosition);
    }

    // perform new position callback(s), while unlocked
    for (; needNewPos > 0; --needNewPos) {
        uint32_t temp = newPosition;
        mCbf(EVENT_NEW_POS, mUserData, &temp);
        newPosition += updatePeriod;
    }

    do {
        audioBuffer.frameCount = frames;
        // Calling obtainBuffer() with a wait count of 1
        // limits wait time to WAIT_PERIOD_MS. This prevents from being
        // stuck here not being able to handle timed events (position, markers).
        status_t err = obtainBuffer(&audioBuffer, 1);
        if (err < NO_ERROR) {
            if (err != TIMED_OUT) {
                ALOGE_IF(err != status_t(NO_MORE_BUFFERS),
                        "Error obtaining an audio buffer, giving up.");
                return false;
            }
            break;
        }
        if (err == status_t(STOPPED)) return false;

        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        readSize = audioBuffer.size;

        // Sanity check on returned size
        if (ssize_t(readSize) <= 0) {
            // The callback is done filling buffers
            // Keep this thread going to handle timed events and
            // still try to get more data in intervals of WAIT_PERIOD_MS
            // but don't just loop and block the CPU, so wait
            usleep(WAIT_PERIOD_MS*1000);
            break;
        }
        if (readSize > reqSize) readSize = reqSize;

        audioBuffer.size = readSize;
        audioBuffer.frameCount = readSize/frameSize();
        frames -= audioBuffer.frameCount;

        releaseBuffer(&audioBuffer);

    } while (frames);


    // Manage overrun callback
    if (active && (mProxy->framesAvailable() == 0)) {
        // The value of active is stale, but we are almost sure to be active here because
        // otherwise we would have exited when obtainBuffer returned STOPPED earlier.
        ALOGV("Overrun user: %x, server: %x, flags %04x", cblk->user, cblk->server, cblk->flags);
        if (!(android_atomic_or(CBLK_UNDERRUN, &cblk->flags) & CBLK_UNDERRUN)) {
            mCbf(EVENT_OVERRUN, mUserData, NULL);
        }
    }

    if (frames == 0) {
        mRemainingFrames = mNotificationFrames;
    } else {
        mRemainingFrames = frames;
    }
    return true;
}

// must be called with mLock and cblk.lock held. Callers must also hold strong references on
// the IAudioRecord and IMemory in case they are recreated here.
// If the IAudioRecord is successfully restored, the cblk pointer is updated
status_t AudioRecord::restoreRecord_l(audio_track_cblk_t*& refCblk)
{
    status_t result;

    audio_track_cblk_t* cblk = refCblk;
    audio_track_cblk_t* newCblk = cblk;
    ALOGW("dead IAudioRecord, creating a new one");

    // signal old cblk condition so that other threads waiting for available buffers stop
    // waiting now
    cblk->cv.broadcast();
    cblk->lock.unlock();

    // if the new IAudioRecord is created, openRecord_l() will modify the
    // following member variables: mAudioRecord, mCblkMemory and mCblk.
    // It will also delete the strong references on previous IAudioRecord and IMemory
    result = openRecord_l(mSampleRate, mFormat, mFrameCount, getInput_l());
    if (result == NO_ERROR) {
        newCblk = mCblk;
        // callback thread or sync event hasn't changed
        result = mAudioRecord->start(AudioSystem::SYNC_EVENT_SAME, 0);
    }
    if (result != NO_ERROR) {
        mActive = false;
    }

    ALOGV("restoreRecord_l() status %d mActive %d cblk %p, old cblk %p flags %08x old flags %08x",
        result, mActive, newCblk, cblk, newCblk->flags, cblk->flags);

    if (result == NO_ERROR) {
        // from now on we switch to the newly created cblk
        refCblk = newCblk;
    }
    newCblk->lock.lock();

    ALOGW_IF(result != NO_ERROR, "restoreRecord_l() error %d", result);

    return result;
}

// =========================================================================

AudioRecord::AudioRecordThread::AudioRecordThread(AudioRecord& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver), mPaused(true)
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
    }
    if (!mReceiver.processAudioBuffer(this)) {
        pause();
    }
    return true;
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
    if (mPaused) {
        mPaused = false;
        mMyCond.signal();
    }
}

// -------------------------------------------------------------------------

}; // namespace android
