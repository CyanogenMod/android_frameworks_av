/*
**
** Copyright 2012, The Android Open Source Project
** Copyright (c) 2013, The Linux Foundation. All rights reserved.
** Not a Contribution.
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


#define LOG_TAG "AudioFlinger"
//#define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cutils/properties.h>
#include <cutils/compiler.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include <private/media/AudioTrackShared.h>
#include <hardware/audio.h>
#include <audio_effects/effect_ns.h>
#include <audio_effects/effect_aec.h>
#include <audio_utils/primitives.h>

// NBAIO implementations
#include <media/nbaio/AudioStreamOutSink.h>
#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/nbaio/SourceAudioBufferProvider.h>

#include <powermanager/PowerManager.h>

#include <common_time/cc_helper.h>
#include <common_time/local_clock.h>

#include "AudioFlinger.h"
#include "AudioMixer.h"
#include "FastMixer.h"
#include "ServiceUtilities.h"
#include "SchedulingPolicyService.h"

#undef ADD_BATTERY_DATA

#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif

// #define DEBUG_CPU_USAGE 10  // log statistics every n wall clock seconds
#ifdef DEBUG_CPU_USAGE
#include <cpustats/CentralTendencyStatistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif

#ifdef SRS_PROCESSING
#include "srs_processing.h"
#include "postpro_patch_ics.h"
#endif

// ----------------------------------------------------------------------------

// Note: the following macro is used for extremely verbose logging message.  In
// order to run with ALOG_ASSERT turned on, we need to have LOG_NDEBUG set to
// 0; but one side effect of this is to turn all LOGV's as well.  Some messages
// are so verbose that we want to suppress them even when we have ALOG_ASSERT
// turned on.  Do not uncomment the #def below unless you really know what you
// are doing and want to see all of the extremely verbose messages.
//#define VERY_VERY_VERBOSE_LOGGING
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#ifdef QCOM_HARDWARE
#define DIRECT_TRACK_EOS 1
#define DIRECT_TRACK_HW_FAIL 6
static const char lockName[] = "DirectTrack";
#endif

namespace android {

// retry counts for buffer fill timeout
// 50 * ~20msecs = 1 second
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;
// allow less retry attempts on direct output thread.
// direct outputs can be a scarce resource in audio hardware and should
// be released as quickly as possible.
static const int8_t kMaxTrackRetriesDirect = 2;

// don't warn about blocked writes or record buffer overflows more often than this
static const nsecs_t kWarningThrottleNs = seconds(5);

// RecordThread loop sleep time upon application overrun or audio HAL read error
static const int kRecordThreadSleepUs = 5000;

// maximum time to wait for setParameters to complete
static const nsecs_t kSetParametersTimeoutNs = seconds(2);

// minimum sleep time for the mixer thread loop when tracks are active but in underrun
static const uint32_t kMinThreadSleepTimeUs = 5000;
// maximum divider applied to the active sleep time in the mixer thread loop
static const uint32_t kMaxThreadSleepTimeShift = 2;

// minimum normal mix buffer size, expressed in milliseconds rather than frames
static const uint32_t kMinNormalMixBufferSizeMs = 20;
// maximum normal mix buffer size
static const uint32_t kMaxNormalMixBufferSizeMs = 24;

// Whether to use fast mixer
static const enum {
    FastMixer_Never,    // never initialize or use: for debugging only
    FastMixer_Always,   // always initialize and use, even if not needed: for debugging only
                        // normal mixer multiplier is 1
    FastMixer_Static,   // initialize if needed, then use all the time if initialized,
                        // multiplier is calculated based on min & max normal mixer buffer size
    FastMixer_Dynamic,  // initialize if needed, then use dynamically depending on track load,
                        // multiplier is calculated based on min & max normal mixer buffer size
    // FIXME for FastMixer_Dynamic:
    //  Supporting this option will require fixing HALs that can't handle large writes.
    //  For example, one HAL implementation returns an error from a large write,
    //  and another HAL implementation corrupts memory, possibly in the sample rate converter.
    //  We could either fix the HAL implementations, or provide a wrapper that breaks
    //  up large writes into smaller ones, and the wrapper would need to deal with scheduler.
} kUseFastMixer = FastMixer_Static;

// Priorities for requestPriority
static const int kPriorityAudioApp = 2;
static const int kPriorityFastMixer = 3;

// IAudioFlinger::createTrack() reports back to client the total size of shared memory area
// for the track.  The client then sub-divides this into smaller buffers for its use.
// Currently the client uses double-buffering by default, but doesn't tell us about that.
// So for now we just assume that client is double-buffered.
// FIXME It would be better for client to tell AudioFlinger whether it wants double-buffering or
// N-buffering, so AudioFlinger could allocate the right amount of memory.
// See the client's minBufCount and mNotificationFramesAct calculations for details.
static const int kFastTrackMultiplier = 2;

// ----------------------------------------------------------------------------

#ifdef ADD_BATTERY_DATA
// To collect the amplifier usage
static void addBatteryData(uint32_t params) {
    sp<IMediaPlayerService> service = IMediaDeathNotifier::getMediaPlayerService();
    if (service == NULL) {
        // it already logged
        return;
    }

    service->addBatteryData(params);
}
#endif


// ----------------------------------------------------------------------------
//      CPU Stats
// ----------------------------------------------------------------------------

class CpuStats {
public:
    CpuStats();
    void sample(const String8 &title);
#ifdef DEBUG_CPU_USAGE
private:
    ThreadCpuUsage mCpuUsage;           // instantaneous thread CPU usage in wall clock ns
    CentralTendencyStatistics mWcStats; // statistics on thread CPU usage in wall clock ns

    CentralTendencyStatistics mHzStats; // statistics on thread CPU usage in cycles

    int mCpuNum;                        // thread's current CPU number
    int mCpukHz;                        // frequency of thread's current CPU in kHz
#endif
};

CpuStats::CpuStats()
#ifdef DEBUG_CPU_USAGE
    : mCpuNum(-1), mCpukHz(-1)
#endif
{
}

void CpuStats::sample(const String8 &title) {
#ifdef DEBUG_CPU_USAGE
    // get current thread's delta CPU time in wall clock ns
    double wcNs;
    bool valid = mCpuUsage.sampleAndEnable(wcNs);

    // record sample for wall clock statistics
    if (valid) {
        mWcStats.sample(wcNs);
    }

    // get the current CPU number
    int cpuNum = sched_getcpu();

    // get the current CPU frequency in kHz
    int cpukHz = mCpuUsage.getCpukHz(cpuNum);

    // check if either CPU number or frequency changed
    if (cpuNum != mCpuNum || cpukHz != mCpukHz) {
        mCpuNum = cpuNum;
        mCpukHz = cpukHz;
        // ignore sample for purposes of cycles
        valid = false;
    }

    // if no change in CPU number or frequency, then record sample for cycle statistics
    if (valid && mCpukHz > 0) {
        double cycles = wcNs * cpukHz * 0.000001;
        mHzStats.sample(cycles);
    }

    unsigned n = mWcStats.n();
    // mCpuUsage.elapsed() is expensive, so don't call it every loop
    if ((n & 127) == 1) {
        long long elapsed = mCpuUsage.elapsed();
        if (elapsed >= DEBUG_CPU_USAGE * 1000000000LL) {
            double perLoop = elapsed / (double) n;
            double perLoop100 = perLoop * 0.01;
            double perLoop1k = perLoop * 0.001;
            double mean = mWcStats.mean();
            double stddev = mWcStats.stddev();
            double minimum = mWcStats.minimum();
            double maximum = mWcStats.maximum();
            double meanCycles = mHzStats.mean();
            double stddevCycles = mHzStats.stddev();
            double minCycles = mHzStats.minimum();
            double maxCycles = mHzStats.maximum();
            mCpuUsage.resetElapsed();
            mWcStats.reset();
            mHzStats.reset();
            ALOGD("CPU usage for %s over past %.1f secs\n"
                "  (%u mixer loops at %.1f mean ms per loop):\n"
                "  us per mix loop: mean=%.0f stddev=%.0f min=%.0f max=%.0f\n"
                "  %% of wall: mean=%.1f stddev=%.1f min=%.1f max=%.1f\n"
                "  MHz: mean=%.1f, stddev=%.1f, min=%.1f max=%.1f",
                    title.string(),
                    elapsed * .000000001, n, perLoop * .000001,
                    mean * .001,
                    stddev * .001,
                    minimum * .001,
                    maximum * .001,
                    mean / perLoop100,
                    stddev / perLoop100,
                    minimum / perLoop100,
                    maximum / perLoop100,
                    meanCycles / perLoop1k,
                    stddevCycles / perLoop1k,
                    minCycles / perLoop1k,
                    maxCycles / perLoop1k);

        }
    }
#endif
};

// ----------------------------------------------------------------------------
//      ThreadBase
// ----------------------------------------------------------------------------

AudioFlinger::ThreadBase::ThreadBase(const sp<AudioFlinger>& audioFlinger, audio_io_handle_t id,
        audio_devices_t outDevice, audio_devices_t inDevice, type_t type)
    :   Thread(false /*canCallJava*/),
        mType(type),
        mAudioFlinger(audioFlinger), mSampleRate(0), mFrameCount(0), mNormalFrameCount(0),
        // mChannelMask
        mChannelCount(0),
        mFrameSize(1), mFormat(AUDIO_FORMAT_INVALID),
        mParamStatus(NO_ERROR),
        mStandby(false), mOutDevice(outDevice), mInDevice(inDevice),
        mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id),
        // mName will be set by concrete (non-virtual) subclass
        mDeathRecipient(new PMDeathRecipient(this))
{
}

AudioFlinger::ThreadBase::~ThreadBase()
{
    mParamCond.broadcast();
    // do not lock the mutex in destructor
    releaseWakeLock_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = mPowerManager->asBinder();
        binder->unlinkToDeath(mDeathRecipient);
    }
}

void AudioFlinger::ThreadBase::exit()
{
    ALOGV("ThreadBase::exit");
    // do any cleanup required for exit to succeed
    preExit();
    {
        // This lock prevents the following race in thread (uniprocessor for illustration):
        //  if (!exitPending()) {
        //      // context switch from here to exit()
        //      // exit() calls requestExit(), what exitPending() observes
        //      // exit() calls signal(), which is dropped since no waiters
        //      // context switch back from exit() to here
        //      mWaitWorkCV.wait(...);
        //      // now thread is hung
        //  }
        AutoMutex lock(mLock);
        requestExit();
        mWaitWorkCV.broadcast();
    }
    // When Thread::requestExitAndWait is made virtual and this method is renamed to
    // "virtual status_t requestExitAndWait()", replace by "return Thread::requestExitAndWait();"
    requestExitAndWait();
}

status_t AudioFlinger::ThreadBase::setParameters(const String8& keyValuePairs)
{
    status_t status;

    ALOGV("ThreadBase::setParameters() %s", keyValuePairs.string());
    Mutex::Autolock _l(mLock);

    mNewParameters.add(keyValuePairs);
    mWaitWorkCV.signal();
    // wait condition with timeout in case the thread loop has exited
    // before the request could be processed
    if (mParamCond.waitRelative(mLock, kSetParametersTimeoutNs) == NO_ERROR) {
        status = mParamStatus;
        mWaitWorkCV.signal();
    } else {
        status = TIMED_OUT;
    }
    return status;
}

#ifdef QCOM_HARDWARE
void AudioFlinger::ThreadBase::effectConfigChanged() {
    ALOGV("New effect is being added to LPA chain, Notifying LPA Direct Track");
    mAudioFlinger->audioConfigChanged_l(AudioSystem::EFFECT_CONFIG_CHANGED, 0, NULL);
}
#endif

void AudioFlinger::ThreadBase::sendIoConfigEvent(int event, int param)
{
    Mutex::Autolock _l(mLock);
    sendIoConfigEvent_l(event, param);
}

// sendIoConfigEvent_l() must be called with ThreadBase::mLock held
void AudioFlinger::ThreadBase::sendIoConfigEvent_l(int event, int param)
{
    IoConfigEvent *ioEvent = new IoConfigEvent(event, param);
    mConfigEvents.add(static_cast<ConfigEvent *>(ioEvent));
    ALOGV("sendIoConfigEvent() num events %d event %d, param %d", mConfigEvents.size(), event,
            param);
    mWaitWorkCV.signal();
}

// sendPrioConfigEvent_l() must be called with ThreadBase::mLock held
void AudioFlinger::ThreadBase::sendPrioConfigEvent_l(pid_t pid, pid_t tid, int32_t prio)
{
    PrioConfigEvent *prioEvent = new PrioConfigEvent(pid, tid, prio);
    mConfigEvents.add(static_cast<ConfigEvent *>(prioEvent));
    ALOGV("sendPrioConfigEvent_l() num events %d pid %d, tid %d prio %d",
          mConfigEvents.size(), pid, tid, prio);
    mWaitWorkCV.signal();
}

void AudioFlinger::ThreadBase::processConfigEvents()
{
    mLock.lock();
    while (!mConfigEvents.isEmpty()) {
        ALOGV("processConfigEvents() remaining events %d", mConfigEvents.size());
        ConfigEvent *event = mConfigEvents[0];
        mConfigEvents.removeAt(0);
        // release mLock before locking AudioFlinger mLock: lock order is always
        // AudioFlinger then ThreadBase to avoid cross deadlock
        mLock.unlock();
        switch(event->type()) {
            case CFG_EVENT_PRIO: {
                PrioConfigEvent *prioEvent = static_cast<PrioConfigEvent *>(event);
                // FIXME Need to understand why this has be done asynchronously
                int err = requestPriority(prioEvent->pid(), prioEvent->tid(), prioEvent->prio(),
                        true /*asynchronous*/);
                if (err != 0) {
                    ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; "
                          "error %d",
                          prioEvent->prio(), prioEvent->pid(), prioEvent->tid(), err);
                }
            } break;
            case CFG_EVENT_IO: {
                IoConfigEvent *ioEvent = static_cast<IoConfigEvent *>(event);
                mAudioFlinger->mLock.lock();
                audioConfigChanged_l(ioEvent->event(), ioEvent->param());
                mAudioFlinger->mLock.unlock();
            } break;
            default:
                ALOGE("processConfigEvents() unknown event type %d", event->type());
                break;
        }
        delete event;
        mLock.lock();
    }
    mLock.unlock();
}

void AudioFlinger::ThreadBase::dumpBase(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    bool locked = AudioFlinger::dumpTryLock(mLock);
    if (!locked) {
        snprintf(buffer, SIZE, "thread %p maybe dead locked\n", this);
        write(fd, buffer, strlen(buffer));
    }

    snprintf(buffer, SIZE, "io handle: %d\n", mId);
    result.append(buffer);
    snprintf(buffer, SIZE, "TID: %d\n", getTid());
    result.append(buffer);
    snprintf(buffer, SIZE, "standby: %d\n", mStandby);
    result.append(buffer);
    snprintf(buffer, SIZE, "Sample rate: %u\n", mSampleRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "HAL frame count: %d\n", mFrameCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Normal frame count: %d\n", mNormalFrameCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Channel Count: %d\n", mChannelCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "Channel Mask: 0x%08x\n", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, "Format: %d\n", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, "Frame size: %u\n", mFrameSize);
    result.append(buffer);

    snprintf(buffer, SIZE, "\nPending setParameters commands: \n");
    result.append(buffer);
    result.append(" Index Command");
    for (size_t i = 0; i < mNewParameters.size(); ++i) {
        snprintf(buffer, SIZE, "\n %02d    ", i);
        result.append(buffer);
        result.append(mNewParameters[i]);
    }

    snprintf(buffer, SIZE, "\n\nPending config events: \n");
    result.append(buffer);
    for (size_t i = 0; i < mConfigEvents.size(); i++) {
        mConfigEvents[i]->dump(buffer, SIZE);
        result.append(buffer);
    }
    result.append("\n");

    write(fd, result.string(), result.size());

    if (locked) {
        mLock.unlock();
    }
}

void AudioFlinger::ThreadBase::dumpEffectChains(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "\n- %d Effect Chains:\n", mEffectChains.size());
    write(fd, buffer, strlen(buffer));

    for (size_t i = 0; i < mEffectChains.size(); ++i) {
        sp<EffectChain> chain = mEffectChains[i];
        if (chain != 0) {
            chain->dump(fd, args);
        }
    }
}

void AudioFlinger::ThreadBase::acquireWakeLock()
{
    Mutex::Autolock _l(mLock);
    acquireWakeLock_l();
}

void AudioFlinger::ThreadBase::acquireWakeLock_l()
{
    if (mPowerManager == 0) {
        // use checkService() to avoid blocking if power service is not up yet
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            ALOGW("Thread %s cannot connect to the power manager service", mName);
        } else {
            mPowerManager = interface_cast<IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        status_t status = mPowerManager->acquireWakeLock(POWERMANAGER_PARTIAL_WAKE_LOCK,
                                                         binder,
                                                         String16(mName));
        if (status == NO_ERROR) {
            mWakeLockToken = binder;
        }
        ALOGV("acquireWakeLock_l() %s status %d", mName, status);
    }
}

void AudioFlinger::ThreadBase::releaseWakeLock()
{
    Mutex::Autolock _l(mLock);
    releaseWakeLock_l();
}

void AudioFlinger::ThreadBase::releaseWakeLock_l()
{
    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock_l() %s", mName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLock(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
}

void AudioFlinger::ThreadBase::clearPowerManager()
{
    Mutex::Autolock _l(mLock);
    releaseWakeLock_l();
    mPowerManager.clear();
}

void AudioFlinger::ThreadBase::PMDeathRecipient::binderDied(const wp<IBinder>& who)
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        thread->clearPowerManager();
    }
    ALOGW("power manager service died !!!");
}

void AudioFlinger::ThreadBase::setEffectSuspended(
        const effect_uuid_t *type, bool suspend, int sessionId)
{
    Mutex::Autolock _l(mLock);
    setEffectSuspended_l(type, suspend, sessionId);
}

void AudioFlinger::ThreadBase::setEffectSuspended_l(
        const effect_uuid_t *type, bool suspend, int sessionId)
{
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    if (chain != 0) {
        if (type != NULL) {
            chain->setEffectSuspended_l(type, suspend);
        } else {
            chain->setEffectSuspendedAll_l(suspend);
        }
    }

    updateSuspendedSessions_l(type, suspend, sessionId);
}

void AudioFlinger::ThreadBase::checkSuspendOnAddEffectChain_l(const sp<EffectChain>& chain)
{
    ssize_t index = mSuspendedSessions.indexOfKey(chain->sessionId());
    if (index < 0) {
        return;
    }

    const KeyedVector <int, sp<SuspendedSessionDesc> >& sessionEffects =
            mSuspendedSessions.valueAt(index);

    for (size_t i = 0; i < sessionEffects.size(); i++) {
        sp<SuspendedSessionDesc> desc = sessionEffects.valueAt(i);
        for (int j = 0; j < desc->mRefCount; j++) {
            if (sessionEffects.keyAt(i) == EffectChain::kKeyForSuspendAll) {
                chain->setEffectSuspendedAll_l(true);
            } else {
                ALOGV("checkSuspendOnAddEffectChain_l() suspending effects %08x",
                    desc->mType.timeLow);
                chain->setEffectSuspended_l(&desc->mType, true);
            }
        }
    }
}

void AudioFlinger::ThreadBase::updateSuspendedSessions_l(const effect_uuid_t *type,
                                                         bool suspend,
                                                         int sessionId)
{
    ssize_t index = mSuspendedSessions.indexOfKey(sessionId);

    KeyedVector <int, sp<SuspendedSessionDesc> > sessionEffects;

    if (suspend) {
        if (index >= 0) {
            sessionEffects = mSuspendedSessions.valueAt(index);
        } else {
            mSuspendedSessions.add(sessionId, sessionEffects);
        }
    } else {
        if (index < 0) {
            return;
        }
        sessionEffects = mSuspendedSessions.valueAt(index);
    }


    int key = EffectChain::kKeyForSuspendAll;
    if (type != NULL) {
        key = type->timeLow;
    }
    index = sessionEffects.indexOfKey(key);

    sp<SuspendedSessionDesc> desc;
    if (suspend) {
        if (index >= 0) {
            desc = sessionEffects.valueAt(index);
        } else {
            desc = new SuspendedSessionDesc();
            if (type != NULL) {
                desc->mType = *type;
            }
            sessionEffects.add(key, desc);
            ALOGV("updateSuspendedSessions_l() suspend adding effect %08x", key);
        }
        desc->mRefCount++;
    } else {
        if (index < 0) {
            return;
        }
        desc = sessionEffects.valueAt(index);
        if (--desc->mRefCount == 0) {
            ALOGV("updateSuspendedSessions_l() restore removing effect %08x", key);
            sessionEffects.removeItemsAt(index);
            if (sessionEffects.isEmpty()) {
                ALOGV("updateSuspendedSessions_l() restore removing session %d",
                                 sessionId);
                mSuspendedSessions.removeItem(sessionId);
            }
        }
    }
    if (!sessionEffects.isEmpty()) {
        mSuspendedSessions.replaceValueFor(sessionId, sessionEffects);
    }
}

void AudioFlinger::ThreadBase::checkSuspendOnEffectEnabled(const sp<EffectModule>& effect,
                                                            bool enabled,
                                                            int sessionId)
{
    Mutex::Autolock _l(mLock);
    checkSuspendOnEffectEnabled_l(effect, enabled, sessionId);
}

void AudioFlinger::ThreadBase::checkSuspendOnEffectEnabled_l(const sp<EffectModule>& effect,
                                                            bool enabled,
                                                            int sessionId)
{
    if (mType != RECORD) {
        // suspend all effects in AUDIO_SESSION_OUTPUT_MIX when enabling any effect on
        // another session. This gives the priority to well behaved effect control panels
        // and applications not using global effects.
        // Enabling post processing in AUDIO_SESSION_OUTPUT_STAGE session does not affect
        // global effects
        if ((sessionId != AUDIO_SESSION_OUTPUT_MIX) && (sessionId != AUDIO_SESSION_OUTPUT_STAGE)) {
            setEffectSuspended_l(NULL, enabled, AUDIO_SESSION_OUTPUT_MIX);
        }
    }

    sp<EffectChain> chain = getEffectChain_l(sessionId);
    if (chain != 0) {
        chain->checkSuspendOnEffectEnabled(effect, enabled);
    }
}

// ThreadBase::createEffect_l() must be called with AudioFlinger::mLock held
sp<AudioFlinger::EffectHandle> AudioFlinger::ThreadBase::createEffect_l(
        const sp<AudioFlinger::Client>& client,
        const sp<IEffectClient>& effectClient,
        int32_t priority,
        int sessionId,
        effect_descriptor_t *desc,
        int *enabled,
        status_t *status
        )
{
    sp<EffectModule> effect;
    sp<EffectHandle> handle;
    status_t lStatus;
    sp<EffectChain> chain;
    bool chainCreated = false;
    bool effectCreated = false;
    bool effectRegistered = false;

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGW("createEffect_l() Audio driver not initialized.");
        goto Exit;
    }

    // Do not allow effects with session ID 0 on direct output or duplicating threads
    // TODO: add rule for hw accelerated effects on direct outputs with non PCM format
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX && mType != MIXER) {
        ALOGW("createEffect_l() Cannot add auxiliary effect %s to session %d",
                desc->name, sessionId);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    // Only Pre processor effects are allowed on input threads and only on input threads
    if ((mType == RECORD) != ((desc->flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC)) {
        ALOGW("createEffect_l() effect %s (flags %08x) created on wrong thread type %d",
                desc->name, desc->flags, mType);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    ALOGV("createEffect_l() thread %p effect %s on session %d", this, desc->name, sessionId);

    { // scope for mLock
        Mutex::Autolock _l(mLock);

        // check for existing effect chain with the requested audio session
        chain = getEffectChain_l(sessionId);
        if (chain == 0) {
            // create a new chain for this session
            ALOGV("createEffect_l() new effect chain for session %d", sessionId);
            chain = new EffectChain(this, sessionId);
            addEffectChain_l(chain);
            chain->setStrategy(getStrategyForSession_l(sessionId));
            chainCreated = true;
#ifdef QCOM_HARDWARE
            if(sessionId == mAudioFlinger->mLPASessionId) {
                // Clear reference to previous effect chain if any
                if(mAudioFlinger->mLPAEffectChain.get()) {
                    mAudioFlinger->mLPAEffectChain.clear();
                }
                ALOGV("New EffectChain is created for LPA session ID %d", sessionId);
                mAudioFlinger->mLPAEffectChain = chain;
                chain->setLPAFlag(true);
                // For LPA, the volume will be applied in DSP. No need for volume
                // control in the Effect chain, so setting it to unity.
                uint32_t volume = 0x1000000; // Equals to 1.0 in 8.24 format
                chain->setVolume_l(&volume,&volume);
            }
#endif
        } else {
            effect = chain->getEffectFromDesc_l(desc);
        }

        ALOGV("createEffect_l() got effect %p on chain %p", effect.get(), chain.get());

        if (effect == 0) {
            int id = mAudioFlinger->nextUniqueId();
            // Check CPU and memory usage
            lStatus = AudioSystem::registerEffect(desc, mId, chain->strategy(), sessionId, id);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectRegistered = true;
            // create a new effect module if none present in the chain
            effect = new EffectModule(this, chain, desc, id, sessionId);
            lStatus = effect->status();
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            lStatus = chain->addEffect_l(effect);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectCreated = true;

#ifdef QCOM_HARDWARE
            effect->setDevice(mAudioFlinger->mLPASessionId == sessionId ? mAudioFlinger->mDirectDevice:mOutDevice);
#else
            effect->setDevice(mOutDevice);
#endif
            effect->setDevice(mInDevice);
            effect->setMode(mAudioFlinger->getMode());
            effect->setAudioSource(mAudioSource);
        }
        // create effect handle and connect it to effect module
        handle = new EffectHandle(effect, client, effectClient, priority);
        lStatus = effect->addHandle(handle.get());
        if (enabled != NULL) {
            *enabled = (int)effect->isEnabled();
        }
    }

Exit:
    if (lStatus != NO_ERROR && lStatus != ALREADY_EXISTS) {
        Mutex::Autolock _l(mLock);
        if (effectCreated) {
            chain->removeEffect_l(effect);
        }
        if (effectRegistered) {
            AudioSystem::unregisterEffect(effect->id());
        }
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
        handle.clear();
    }

    if (status != NULL) {
        *status = lStatus;
    }
    return handle;
}

sp<AudioFlinger::EffectModule> AudioFlinger::ThreadBase::getEffect(int sessionId, int effectId)
{
    Mutex::Autolock _l(mLock);
    return getEffect_l(sessionId, effectId);
}

sp<AudioFlinger::EffectModule> AudioFlinger::ThreadBase::getEffect_l(int sessionId, int effectId)
{
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    return chain != 0 ? chain->getEffectFromId_l(effectId) : 0;
}

// PlaybackThread::addEffect_l() must be called with AudioFlinger::mLock and
// PlaybackThread::mLock held
status_t AudioFlinger::ThreadBase::addEffect_l(const sp<EffectModule>& effect)
{
    // check for existing effect chain with the requested audio session
    int sessionId = effect->sessionId();
    sp<EffectChain> chain = getEffectChain_l(sessionId);
    bool chainCreated = false;

    if (chain == 0) {
        // create a new chain for this session
        ALOGV("addEffect_l() new effect chain for session %d", sessionId);
        chain = new EffectChain(this, sessionId);
        addEffectChain_l(chain);
        chain->setStrategy(getStrategyForSession_l(sessionId));
        chainCreated = true;
    }
    ALOGV("addEffect_l() %p chain %p effect %p", this, chain.get(), effect.get());

    if (chain->getEffectFromId_l(effect->id()) != 0) {
        ALOGW("addEffect_l() %p effect %s already present in chain %p",
                this, effect->desc().name, chain.get());
        return BAD_VALUE;
    }

    status_t status = chain->addEffect_l(effect);
    if (status != NO_ERROR) {
        if (chainCreated) {
            removeEffectChain_l(chain);
        }
        return status;
    }

    effect->setDevice(mOutDevice);
    effect->setDevice(mInDevice);
    effect->setMode(mAudioFlinger->getMode());
    effect->setAudioSource(mAudioSource);
    return NO_ERROR;
}

void AudioFlinger::ThreadBase::removeEffect_l(const sp<EffectModule>& effect) {

    ALOGV("removeEffect_l() %p effect %p", this, effect.get());
    effect_descriptor_t desc = effect->desc();
    if ((desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
        detachAuxEffect_l(effect->id());
    }

    sp<EffectChain> chain = effect->chain().promote();
    if (chain != 0) {
        // remove effect chain if removing last effect
        if (chain->removeEffect_l(effect) == 0) {
            removeEffectChain_l(chain);
        }
    } else {
        ALOGW("removeEffect_l() %p cannot promote chain for effect %p", this, effect.get());
    }
}

void AudioFlinger::ThreadBase::lockEffectChains_l(
        Vector< sp<AudioFlinger::EffectChain> >& effectChains)
{
    effectChains = mEffectChains;
    for (size_t i = 0; i < mEffectChains.size(); i++) {
#ifdef QCOM_HARDWARE
        if (mEffectChains[i] != mAudioFlinger->mLPAEffectChain)
#endif
            mEffectChains[i]->lock();
    }
}

void AudioFlinger::ThreadBase::unlockEffectChains(
        const Vector< sp<AudioFlinger::EffectChain> >& effectChains)
{
    for (size_t i = 0; i < effectChains.size(); i++) {
#ifdef QCOM_HARDWARE
        if (mEffectChains[i] != mAudioFlinger->mLPAEffectChain)
#endif
            effectChains[i]->unlock();
    }
}

sp<AudioFlinger::EffectChain> AudioFlinger::ThreadBase::getEffectChain(int sessionId)
{
    Mutex::Autolock _l(mLock);
    return getEffectChain_l(sessionId);
}

sp<AudioFlinger::EffectChain> AudioFlinger::ThreadBase::getEffectChain_l(int sessionId) const
{
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() == sessionId) {
            return mEffectChains[i];
        }
    }
    return 0;
}

void AudioFlinger::ThreadBase::setMode(audio_mode_t mode)
{
    Mutex::Autolock _l(mLock);
    size_t size = mEffectChains.size();
    for (size_t i = 0; i < size; i++) {
        mEffectChains[i]->setMode_l(mode);
    }
}

void AudioFlinger::ThreadBase::disconnectEffect(const sp<EffectModule>& effect,
                                                    EffectHandle *handle,
                                                    bool unpinIfLast) {

    Mutex::Autolock _l(mLock);
    ALOGV("disconnectEffect() %p effect %p", this, effect.get());
    // delete the effect module if removing last handle on it
    if (effect->removeHandle(handle) == 0) {
        if (!effect->isPinned() || unpinIfLast) {
            removeEffect_l(effect);
            AudioSystem::unregisterEffect(effect->id());
        }
    }
}

// ----------------------------------------------------------------------------
//      Playback
// ----------------------------------------------------------------------------

AudioFlinger::PlaybackThread::PlaybackThread(const sp<AudioFlinger>& audioFlinger,
                                             AudioStreamOut* output,
                                             audio_io_handle_t id,
                                             audio_devices_t device,
                                             type_t type)
    :   ThreadBase(audioFlinger, id, device, AUDIO_DEVICE_NONE, type),
        mMixBuffer(NULL), mSuspended(0), mBytesWritten(0),
        // mStreamTypes[] initialized in constructor body
        mOutput(output),
        mLastWriteTime(0), mNumWrites(0), mNumDelayedWrites(0), mInWrite(false),
        mMixerStatus(MIXER_IDLE),
        mMixerStatusIgnoringFastTracks(MIXER_IDLE),
        standbyDelay(AudioFlinger::mStandbyTimeInNsecs),
        mScreenState(AudioFlinger::mScreenState),
        mOutputFlags(AUDIO_OUTPUT_FLAG_NONE),
        // index 0 is reserved for normal mixer's submix
        mFastTrackAvailMask(((1 << FastMixerState::kMaxFastTracks) - 1) & ~1)
{
    snprintf(mName, kNameLength, "AudioOut_%X", id);
    mNBLogWriter = audioFlinger->newWriter_l(kLogSize, mName);

    // Assumes constructor is called by AudioFlinger with it's mLock held, but
    // it would be safer to explicitly pass initial masterVolume/masterMute as
    // parameter.
    //
    // If the HAL we are using has support for master volume or master mute,
    // then do not attenuate or mute during mixing (just leave the volume at 1.0
    // and the mute set to false).
    mMasterVolume = audioFlinger->masterVolume_l();
    mMasterMute = audioFlinger->masterMute_l();
    if (mOutput && mOutput->audioHwDev) {
        if (mOutput->audioHwDev->canSetMasterVolume()) {
            mMasterVolume = 1.0;
        }

        if (mOutput->audioHwDev->canSetMasterMute()) {
            mMasterMute = false;
        }
    }

    readOutputParameters();

    // mStreamTypes[AUDIO_STREAM_CNT] is initialized by stream_type_t default constructor
    // There is no AUDIO_STREAM_MIN, and ++ operator does not compile
    for (audio_stream_type_t stream = (audio_stream_type_t) 0; stream < AUDIO_STREAM_CNT;
            stream = (audio_stream_type_t) (stream + 1)) {
        mStreamTypes[stream].volume = mAudioFlinger->streamVolume_l(stream);
        mStreamTypes[stream].mute = mAudioFlinger->streamMute_l(stream);
    }
    // mStreamTypes[AUDIO_STREAM_CNT] exists but isn't explicitly initialized here,
    // because mAudioFlinger doesn't have one to copy from
}

AudioFlinger::PlaybackThread::~PlaybackThread()
{
    mAudioFlinger->unregisterWriter(mNBLogWriter);
    delete [] mMixBuffer;
}

void AudioFlinger::PlaybackThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    dumpEffectChains(fd, args);
}

void AudioFlinger::PlaybackThread::dumpTracks(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.appendFormat("Output thread %p stream volumes in dB:\n    ", this);
    for (int i = 0; i < AUDIO_STREAM_CNT; ++i) {
        const stream_type_t *st = &mStreamTypes[i];
        if (i > 0) {
            result.appendFormat(", ");
        }
        result.appendFormat("%d:%.2g", i, 20.0 * log10(st->volume));
        if (st->mute) {
            result.append("M");
        }
    }
    result.append("\n");
    write(fd, result.string(), result.length());
    result.clear();

    snprintf(buffer, SIZE, "Output thread %p tracks\n", this);
    result.append(buffer);
    Track::appendDumpHeader(result);
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (track != 0) {
            track->dump(buffer, SIZE);
            result.append(buffer);
        }
    }

    snprintf(buffer, SIZE, "Output thread %p active tracks\n", this);
    result.append(buffer);
    Track::appendDumpHeader(result);
    for (size_t i = 0; i < mActiveTracks.size(); ++i) {
        sp<Track> track = mActiveTracks[i].promote();
        if (track != 0) {
            track->dump(buffer, SIZE);
            result.append(buffer);
        }
    }
    write(fd, result.string(), result.size());

    // These values are "raw"; they will wrap around.  See prepareTracks_l() for a better way.
    FastTrackUnderruns underruns = getFastTrackUnderruns(0);
    fdprintf(fd, "Normal mixer raw underrun counters: partial=%u empty=%u\n",
            underruns.mBitFields.mPartial, underruns.mBitFields.mEmpty);
}

void AudioFlinger::PlaybackThread::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "\nOutput thread %p internals\n", this);
    result.append(buffer);
    snprintf(buffer, SIZE, "last write occurred (msecs): %llu\n",
            ns2ms(systemTime() - mLastWriteTime));
    result.append(buffer);
    snprintf(buffer, SIZE, "total writes: %d\n", mNumWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "delayed writes: %d\n", mNumDelayedWrites);
    result.append(buffer);
    snprintf(buffer, SIZE, "blocked in write: %d\n", mInWrite);
    result.append(buffer);
    snprintf(buffer, SIZE, "suspend count: %d\n", mSuspended);
    result.append(buffer);
    snprintf(buffer, SIZE, "mix buffer : %p\n", mMixBuffer);
    result.append(buffer);
    write(fd, result.string(), result.size());
    fdprintf(fd, "Fast track availMask=%#x\n", mFastTrackAvailMask);

    dumpBase(fd, args);
}

// Thread virtuals
status_t AudioFlinger::PlaybackThread::readyToRun()
{
    status_t status = initCheck();
    if (status == NO_ERROR) {
        ALOGI("AudioFlinger's thread %p ready to run", this);
    } else {
        ALOGE("No working audio driver found.");
    }
    return status;
}

void AudioFlinger::PlaybackThread::onFirstRef()
{
    run(mName, ANDROID_PRIORITY_URGENT_AUDIO);
}

// ThreadBase virtuals
void AudioFlinger::PlaybackThread::preExit()
{
    ALOGV("  preExit()");
    // FIXME this is using hard-coded strings but in the future, this functionality will be
    //       converted to use audio HAL extensions required to support tunneling
    mOutput->stream->common.set_parameters(&mOutput->stream->common, "exiting=1");
}

// PlaybackThread::createTrack_l() must be called with AudioFlinger::mLock held
sp<AudioFlinger::PlaybackThread::Track> AudioFlinger::PlaybackThread::createTrack_l(
        const sp<AudioFlinger::Client>& client,
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        const sp<IMemory>& sharedBuffer,
        int sessionId,
        IAudioFlinger::track_flags_t *flags,
        pid_t tid,
        status_t *status)
{
    sp<Track> track;
    status_t lStatus;

    bool isTimed = (*flags & IAudioFlinger::TRACK_TIMED) != 0;

    // client expresses a preference for FAST, but we get the final say
    if (*flags & IAudioFlinger::TRACK_FAST) {
      if (
            // not timed
            (!isTimed) &&
            // either of these use cases:
            (
              // use case 1: shared buffer with any frame count
              (
                (sharedBuffer != 0)
              ) ||
              // use case 2: callback handler and frame count is default or at least as large as HAL
              (
                (tid != -1) &&
                ((frameCount == 0) ||
                (frameCount >= (mFrameCount * kFastTrackMultiplier)))
              )
            ) &&
            // PCM data
            audio_is_linear_pcm(format) &&
            // mono or stereo
            ( (channelMask == AUDIO_CHANNEL_OUT_MONO) ||
              (channelMask == AUDIO_CHANNEL_OUT_STEREO) ) &&
#ifndef FAST_TRACKS_AT_NON_NATIVE_SAMPLE_RATE
            // hardware sample rate
            (sampleRate == mSampleRate) &&
#endif
            // normal mixer has an associated fast mixer
            hasFastMixer() &&
            // there are sufficient fast track slots available
            (mFastTrackAvailMask != 0)
            // FIXME test that MixerThread for this fast track has a capable output HAL
            // FIXME add a permission test also?
        ) {
        // if frameCount not specified, then it defaults to fast mixer (HAL) frame count
        if (frameCount == 0) {
            frameCount = mFrameCount * kFastTrackMultiplier;
        }
        ALOGV("AUDIO_OUTPUT_FLAG_FAST accepted: frameCount=%d mFrameCount=%d",
                frameCount, mFrameCount);
      } else {
        ALOGV("AUDIO_OUTPUT_FLAG_FAST denied: isTimed=%d sharedBuffer=%p frameCount=%d "
                "mFrameCount=%d format=%d isLinear=%d channelMask=%#x sampleRate=%u mSampleRate=%u "
                "hasFastMixer=%d tid=%d fastTrackAvailMask=%#x",
                isTimed, sharedBuffer.get(), frameCount, mFrameCount, format,
                audio_is_linear_pcm(format),
                channelMask, sampleRate, mSampleRate, hasFastMixer(), tid, mFastTrackAvailMask);
        *flags &= ~IAudioFlinger::TRACK_FAST;
        // For compatibility with AudioTrack calculation, buffer depth is forced
        // to be at least 2 x the normal mixer frame count and cover audio hardware latency.
        // This is probably too conservative, but legacy application code may depend on it.
        // If you change this calculation, also review the start threshold which is related.
        uint32_t latencyMs = mOutput->stream->get_latency(mOutput->stream);
        uint32_t minBufCount = 0;
        if(mSampleRate)
            minBufCount = latencyMs / ((1000 * mNormalFrameCount) / mSampleRate);
        if (minBufCount < 2) {
            minBufCount = 2;
        }
        size_t minFrameCount = mNormalFrameCount * minBufCount;
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
      }
    }

    if (mType == DIRECT) {
        if ((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM) {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("createTrack_l() Bad parameter: sampleRate %u format %d, channelMask 0x%08x "
                        "for output %p with format %d",
                        sampleRate, format, channelMask, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
    } else {
        // Resampler implementation limits input sampling rate to 2 x output sampling rate.
        if (sampleRate > mSampleRate*2) {
            ALOGE("Sample rate out of range: %u mSampleRate %u", sampleRate, mSampleRate);
            lStatus = BAD_VALUE;
            goto Exit;
        }
    }

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("Audio driver not initialized.");
        goto Exit;
    }

    { // scope for mLock
        Mutex::Autolock _l(mLock);

        // all tracks in same audio session must share the same routing strategy otherwise
        // conflicts will happen when tracks are moved from one output to another by audio policy
        // manager
        uint32_t strategy = AudioSystem::getStrategyForStream(streamType);
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<Track> t = mTracks[i];
            if (t != 0 && !t->isOutputTrack()) {
                uint32_t actual = AudioSystem::getStrategyForStream(t->streamType());
                if (sessionId == t->sessionId() && strategy != actual) {
                    ALOGE("createTrack_l() mismatched strategy; expected %u but found %u",
                            strategy, actual);
                    lStatus = BAD_VALUE;
                    goto Exit;
                }
            }
        }

        if (!isTimed) {
            track = new Track(this, client, streamType, sampleRate, format,
                    channelMask, frameCount, sharedBuffer, sessionId, *flags);
        } else {
            track = TimedTrack::create(this, client, streamType, sampleRate, format,
                    channelMask, frameCount, sharedBuffer, sessionId);
        }
        if (track == 0 || track->getCblk() == NULL || track->name() < 0) {
            lStatus = NO_MEMORY;
            goto Exit;
        }
        mTracks.add(track);

        sp<EffectChain> chain = getEffectChain_l(sessionId);
        if (chain != 0) {
            ALOGV("createTrack_l() setting main buffer %p", chain->inBuffer());
            track->setMainBuffer(chain->inBuffer());
            chain->setStrategy(AudioSystem::getStrategyForStream(track->streamType()));
            chain->incTrackCnt();
        }

        if ((*flags & IAudioFlinger::TRACK_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            // we don't have CAP_SYS_NICE, nor do we want to have it as it's too powerful,
            // so ask activity manager to do this on our behalf
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp);
        }
    }

    lStatus = NO_ERROR;

Exit:
    if (status) {
        *status = lStatus;
    }
    return track;
}

uint32_t AudioFlinger::PlaybackThread::correctLatency_l(uint32_t latency) const
{
    return latency;
}

uint32_t AudioFlinger::PlaybackThread::latency() const
{
    Mutex::Autolock _l(mLock);
    return latency_l();
}
uint32_t AudioFlinger::PlaybackThread::latency_l() const
{
    if (initCheck() == NO_ERROR) {
        return correctLatency_l(mOutput->stream->get_latency(mOutput->stream));
    } else {
        return 0;
    }
}

void AudioFlinger::PlaybackThread::setMasterVolume(float value)
{
    Mutex::Autolock _l(mLock);
    // Don't apply master volume in SW if our HAL can do it for us.
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterVolume()) {
        mMasterVolume = 1.0;
    } else {
        mMasterVolume = value;
    }
}

void AudioFlinger::PlaybackThread::setMasterMute(bool muted)
{
    Mutex::Autolock _l(mLock);
    // Don't apply master mute in SW if our HAL can do it for us.
    if (mOutput && mOutput->audioHwDev &&
        mOutput->audioHwDev->canSetMasterMute()) {
        mMasterMute = false;
    } else {
        mMasterMute = muted;
    }
}

void AudioFlinger::PlaybackThread::setStreamVolume(audio_stream_type_t stream, float value)
{
    Mutex::Autolock _l(mLock);
    mStreamTypes[stream].volume = value;
}

void AudioFlinger::PlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    Mutex::Autolock _l(mLock);
    mStreamTypes[stream].mute = muted;
}

float AudioFlinger::PlaybackThread::streamVolume(audio_stream_type_t stream) const
{
    Mutex::Autolock _l(mLock);
    return mStreamTypes[stream].volume;
}

// addTrack_l() must be called with ThreadBase::mLock held
status_t AudioFlinger::PlaybackThread::addTrack_l(const sp<Track>& track)
{
    status_t status = ALREADY_EXISTS;

    // set retry count for buffer fill
    track->mRetryCount = kMaxTrackStartupRetries;
    if (mActiveTracks.indexOf(track) < 0) {
        // the track is newly added, make sure it fills up all its
        // buffers before playing. This is to ensure the client will
        // effectively get the latency it requested.
        track->mFillingUpStatus = Track::FS_FILLING;
        track->mResetDone = false;
        track->mPresentationCompleteFrames = 0;
        mActiveTracks.add(track);
        sp<EffectChain> chain = getEffectChain_l(track->sessionId());
        if (chain != 0) {
            ALOGV("addTrack_l() starting track on chain %p for session %d", chain.get(),
                    track->sessionId());
            chain->incActiveTrackCnt();
        }

        status = NO_ERROR;
    }

    ALOGV("mWaitWorkCV.broadcast");
    mWaitWorkCV.broadcast();

    return status;
}

// destroyTrack_l() must be called with ThreadBase::mLock held
void AudioFlinger::PlaybackThread::destroyTrack_l(const sp<Track>& track)
{
    track->mState = TrackBase::TERMINATED;
    // active tracks are removed by threadLoop()
    if (mActiveTracks.indexOf(track) < 0) {
        removeTrack_l(track);
    }
}

void AudioFlinger::PlaybackThread::removeTrack_l(const sp<Track>& track)
{
    track->triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
    mTracks.remove(track);
    deleteTrackName_l(track->name());
    // redundant as track is about to be destroyed, for dumpsys only
    track->mName = -1;
    if (track->isFastTrack()) {
        int index = track->mFastIndex;
        ALOG_ASSERT(0 < index && index < (int)FastMixerState::kMaxFastTracks);
        ALOG_ASSERT(!(mFastTrackAvailMask & (1 << index)));
        mFastTrackAvailMask |= 1 << index;
        // redundant as track is about to be destroyed, for dumpsys only
        track->mFastIndex = -1;
    }
    sp<EffectChain> chain = getEffectChain_l(track->sessionId());
    if (chain != 0) {
        chain->decTrackCnt();
    }
}

String8 AudioFlinger::PlaybackThread::getParameters(const String8& keys)
{
    String8 out_s8 = String8("");
    char *s;

    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return out_s8;
    }

    s = mOutput->stream->common.get_parameters(&mOutput->stream->common, keys.string());
    out_s8 = String8(s);
    free(s);
    return out_s8;
}

// audioConfigChanged_l() must be called with AudioFlinger::mLock held
void AudioFlinger::PlaybackThread::audioConfigChanged_l(int event, int param) {
    AudioSystem::OutputDescriptor desc;
    void *param2 = NULL;

    ALOGV("PlaybackThread::audioConfigChanged_l, thread %p, event %d, param %d", this, event,
            param);

    switch (event) {
    case AudioSystem::OUTPUT_OPENED:
    case AudioSystem::OUTPUT_CONFIG_CHANGED:
        desc.channels = mChannelMask;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mNormalFrameCount; // FIXME see
                                             // AudioFlinger::frameCount(audio_io_handle_t)
        desc.latency = latency();
        param2 = &desc;
        break;

    case AudioSystem::STREAM_CONFIG_CHANGED:
        param2 = &param;
    case AudioSystem::OUTPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged_l(event, mId, param2);
}

void AudioFlinger::PlaybackThread::readOutputParameters()
{
    mSampleRate = mOutput->stream->common.get_sample_rate(&mOutput->stream->common);
    mChannelMask = mOutput->stream->common.get_channels(&mOutput->stream->common);
    mChannelCount = (uint16_t)popcount(mChannelMask);
    mFormat = mOutput->stream->common.get_format(&mOutput->stream->common);
    mFrameSize = audio_stream_frame_size(&mOutput->stream->common);
    mFrameCount = mOutput->stream->common.get_buffer_size(&mOutput->stream->common) / mFrameSize;
    if (mFrameCount & 15) {
        ALOGW("HAL output buffer size is %u frames but AudioMixer requires multiples of 16 frames",
                mFrameCount);
    }

    // Calculate size of normal mix buffer relative to the HAL output buffer size
    double multiplier = 1.0;
    if (mType == MIXER && (kUseFastMixer == FastMixer_Static ||
            kUseFastMixer == FastMixer_Dynamic)) {
        size_t minNormalFrameCount = (kMinNormalMixBufferSizeMs * mSampleRate) / 1000;
        size_t maxNormalFrameCount = (kMaxNormalMixBufferSizeMs * mSampleRate) / 1000;
        // round up minimum and round down maximum to nearest 16 frames to satisfy AudioMixer
        minNormalFrameCount = (minNormalFrameCount + 15) & ~15;
        maxNormalFrameCount = maxNormalFrameCount & ~15;
        if (maxNormalFrameCount < minNormalFrameCount) {
            maxNormalFrameCount = minNormalFrameCount;
        }
        multiplier = (double) minNormalFrameCount / (double) mFrameCount;
        if (multiplier <= 1.0) {
            multiplier = 1.0;
        } else if (multiplier <= 2.0) {
            if (2 * mFrameCount <= maxNormalFrameCount) {
                multiplier = 2.0;
            } else {
                multiplier = (double) maxNormalFrameCount / (double) mFrameCount;
            }
        } else {
            // prefer an even multiplier, for compatibility with doubling of fast tracks due to HAL
            // SRC (it would be unusual for the normal mix buffer size to not be a multiple of fast
            // track, but we sometimes have to do this to satisfy the maximum frame count
            // constraint)
            // FIXME this rounding up should not be done if no HAL SRC
            uint32_t truncMult = (uint32_t) multiplier;
            if ((truncMult & 1)) {
                if ((truncMult + 1) * mFrameCount <= maxNormalFrameCount) {
                    ++truncMult;
                }
            }
            multiplier = (double) truncMult;
        }
    }
    mNormalFrameCount = multiplier * mFrameCount;
    // round up to nearest 16 frames to satisfy AudioMixer
    mNormalFrameCount = (mNormalFrameCount + 15) & ~15;
    ALOGI("HAL output buffer size %u frames, normal mix buffer size %u frames", mFrameCount,
            mNormalFrameCount);

    delete[] mMixBuffer;
    mMixBuffer = new int16_t[mNormalFrameCount * mChannelCount];
    memset(mMixBuffer, 0, mNormalFrameCount * mChannelCount * sizeof(int16_t));

    // force reconfiguration of effect chains and engines to take new buffer size and audio
    // parameters into account
    // Note that mLock is not held when readOutputParameters() is called from the constructor
    // but in this case nothing is done below as no audio sessions have effect yet so it doesn't
    // matter.
    // create a copy of mEffectChains as calling moveEffectChain_l() can reorder some effect chains
    Vector< sp<EffectChain> > effectChains = mEffectChains;
    for (size_t i = 0; i < effectChains.size(); i ++) {
        mAudioFlinger->moveEffectChain_l(effectChains[i]->sessionId(), this, this, false);
    }
}


status_t AudioFlinger::PlaybackThread::getRenderPosition(size_t *halFrames, size_t *dspFrames)
{
    if (halFrames == NULL || dspFrames == NULL) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return INVALID_OPERATION;
    }
    size_t framesWritten = mBytesWritten / mFrameSize;
    *halFrames = framesWritten;

    if (isSuspended()) {
        // return an estimation of rendered frames when the output is suspended
        size_t latencyFrames = (latency_l() * mSampleRate) / 1000;
        *dspFrames = framesWritten >= latencyFrames ? framesWritten - latencyFrames : 0;
        return NO_ERROR;
    } else {
        return mOutput->stream->get_render_position(mOutput->stream, dspFrames);
    }
}

uint32_t AudioFlinger::PlaybackThread::hasAudioSession(int sessionId) const
{
    Mutex::Autolock _l(mLock);
    uint32_t result = 0;
    if (getEffectChain_l(sessionId) != 0) {
        result = EFFECT_SESSION;
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (sessionId == track->sessionId() && !track->isInvalid()) {
            result |= TRACK_SESSION;
            break;
        }
    }

    return result;
}

uint32_t AudioFlinger::PlaybackThread::getStrategyForSession_l(int sessionId)
{
    // session AUDIO_SESSION_OUTPUT_MIX is placed in same strategy as MUSIC stream so that
    // it is moved to correct output by audio policy manager when A2DP is connected or disconnected
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        return AudioSystem::getStrategyForStream(AUDIO_STREAM_MUSIC);
    }
    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<Track> track = mTracks[i];
        if (sessionId == track->sessionId() && !track->isInvalid()) {
            return AudioSystem::getStrategyForStream(track->streamType());
        }
    }
    return AudioSystem::getStrategyForStream(AUDIO_STREAM_MUSIC);
}


AudioFlinger::AudioStreamOut* AudioFlinger::PlaybackThread::getOutput() const
{
    Mutex::Autolock _l(mLock);
    return mOutput;
}

AudioFlinger::AudioStreamOut* AudioFlinger::PlaybackThread::clearOutput()
{
    Mutex::Autolock _l(mLock);
    AudioStreamOut *output = mOutput;
    mOutput = NULL;
    // FIXME FastMixer might also have a raw ptr to mOutputSink;
    //       must push a NULL and wait for ack
    mOutputSink.clear();
    mPipeSink.clear();
    mNormalSink.clear();
    return output;
}

// this method must always be called either with ThreadBase mLock held or inside the thread loop
audio_stream_t* AudioFlinger::PlaybackThread::stream() const
{
    if (mOutput == NULL) {
        return NULL;
    }
    return &mOutput->stream->common;
}

uint32_t AudioFlinger::PlaybackThread::activeSleepTimeUs() const
{
    return (uint32_t)((uint32_t)((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}

status_t AudioFlinger::PlaybackThread::setSyncEvent(const sp<SyncEvent>& event)
{
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mLock);

    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (event->triggerSession() == track->sessionId()) {
            (void) track->setSyncEvent(event);
            return NO_ERROR;
        }
    }

    return NAME_NOT_FOUND;
}

bool AudioFlinger::PlaybackThread::isValidSyncEvent(const sp<SyncEvent>& event) const
{
    return event->type() == AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE;
}

void AudioFlinger::PlaybackThread::threadLoop_removeTracks(
        const Vector< sp<Track> >& tracksToRemove)
{
    size_t count = tracksToRemove.size();
    if (CC_UNLIKELY(count)) {
        for (size_t i = 0 ; i < count ; i++) {
            const sp<Track>& track = tracksToRemove.itemAt(i);
            if ((track->sharedBuffer() != 0) &&
                    (track->mState == TrackBase::ACTIVE || track->mState == TrackBase::RESUMING)) {
                AudioSystem::stopOutput(mId, track->streamType(), track->sessionId());
            }
        }
    }

}

void AudioFlinger::PlaybackThread::checkSilentMode_l()
{
    if (!mMasterMute) {
        char value[PROPERTY_VALUE_MAX];
        if (property_get("ro.audio.silent", value, "0") > 0) {
            char *endptr;
            unsigned long ul = strtoul(value, &endptr, 0);
            if (*endptr == '\0' && ul != 0) {
                ALOGD("Silence is golden");
                // The setprop command will not allow a property to be changed after
                // the first time it is set, so we don't have to worry about un-muting.
                setMasterMute_l(true);
            }
        }
    }
}

// shared by MIXER and DIRECT, overridden by DUPLICATING
void AudioFlinger::PlaybackThread::threadLoop_write()
{
    // FIXME rewrite to reduce number of system calls
    mLastWriteTime = systemTime();
    mInWrite = true;
    int bytesWritten;

    // If an NBAIO sink is present, use it to write the normal mixer's submix
    if (mNormalSink != 0) {
#define mBitShift 2 // FIXME
        size_t count = mixBufferSize >> mBitShift;
        ATRACE_BEGIN("write");
        // update the setpoint when AudioFlinger::mScreenState changes
        uint32_t screenState = AudioFlinger::mScreenState;
        if (screenState != mScreenState) {
            mScreenState = screenState;
            MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
            if (pipe != NULL) {
                pipe->setAvgFrames((mScreenState & 1) ?
                        (pipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
            }
        }
        ssize_t framesWritten = mNormalSink->write(mMixBuffer, count);
        ATRACE_END();
        if (framesWritten > 0) {
            bytesWritten = framesWritten << mBitShift;
        } else {
            bytesWritten = framesWritten;
        }
    // otherwise use the HAL / AudioStreamOut directly
    } else {
        // Direct output thread.
        bytesWritten = (int)mOutput->stream->write(mOutput->stream, mMixBuffer, mixBufferSize);
    }

    if (bytesWritten > 0) {
        mBytesWritten += mixBufferSize;
    }
    mNumWrites++;
    mInWrite = false;
}

/*
The derived values that are cached:
 - mixBufferSize from frame count * frame size
 - activeSleepTime from activeSleepTimeUs()
 - idleSleepTime from idleSleepTimeUs()
 - standbyDelay from mActiveSleepTimeUs (DIRECT only)
 - maxPeriod from frame count and sample rate (MIXER only)

The parameters that affect these derived values are:
 - frame count
 - frame size
 - sample rate
 - device type: A2DP or not
 - device latency
 - format: PCM or not
 - active sleep time
 - idle sleep time
*/

void AudioFlinger::PlaybackThread::cacheParameters_l()
{
    mixBufferSize = mNormalFrameCount * mFrameSize;
    activeSleepTime = activeSleepTimeUs();
    idleSleepTime = idleSleepTimeUs();
}

void AudioFlinger::PlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    ALOGV ("MixerThread::invalidateTracks() mixer %p, streamType %d, mTracks.size %d",
            this,  streamType, mTracks.size());
    Mutex::Autolock _l(mLock);

    size_t size = mTracks.size();
    for (size_t i = 0; i < size; i++) {
        sp<Track> t = mTracks[i];
        if (t->streamType() == streamType) {
            t->invalidate();
        }
    }
}

status_t AudioFlinger::PlaybackThread::addEffectChain_l(const sp<EffectChain>& chain)
{
    int session = chain->sessionId();
    int16_t *buffer = mMixBuffer;
    bool ownsBuffer = false;

    ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
    if (session > 0) {
        // Only one effect chain can be present in direct output thread and it uses
        // the mix buffer as input
        if (mType != DIRECT) {
            size_t numSamples = mNormalFrameCount * mChannelCount;
            buffer = new int16_t[numSamples];
            memset(buffer, 0, numSamples * sizeof(int16_t));
            ALOGV("addEffectChain_l() creating new input buffer %p session %d", buffer, session);
            ownsBuffer = true;
        }

        // Attach all tracks with same session ID to this chain.
        for (size_t i = 0; i < mTracks.size(); ++i) {
            sp<Track> track = mTracks[i];
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() track->setMainBuffer track %p buffer %p", track.get(),
                        buffer);
                track->setMainBuffer(buffer);
                chain->incTrackCnt();
            }
        }

        // indicate all active tracks in the chain
        for (size_t i = 0 ; i < mActiveTracks.size() ; ++i) {
            sp<Track> track = mActiveTracks[i].promote();
            if (track == 0) {
                continue;
            }
            if (session == track->sessionId()) {
                ALOGV("addEffectChain_l() activating track %p on session %d", track.get(), session);
                chain->incActiveTrackCnt();
            }
        }
    }

    chain->setInBuffer(buffer, ownsBuffer);
    chain->setOutBuffer(mMixBuffer);
    // Effect chain for session AUDIO_SESSION_OUTPUT_STAGE is inserted at end of effect
    // chains list in order to be processed last as it contains output stage effects
    // Effect chain for session AUDIO_SESSION_OUTPUT_MIX is inserted before
    // session AUDIO_SESSION_OUTPUT_STAGE to be processed
    // after track specific effects and before output stage
    // It is therefore mandatory that AUDIO_SESSION_OUTPUT_MIX == 0 and
    // that AUDIO_SESSION_OUTPUT_STAGE < AUDIO_SESSION_OUTPUT_MIX
    // Effect chain for other sessions are inserted at beginning of effect
    // chains list to be processed before output mix effects. Relative order between other
    // sessions is not important
    size_t size = mEffectChains.size();
    size_t i = 0;
    for (i = 0; i < size; i++) {
        if (mEffectChains[i]->sessionId() < session) {
            break;
        }
    }
    mEffectChains.insertAt(chain, i);
    checkSuspendOnAddEffectChain_l(chain);

    return NO_ERROR;
}

size_t AudioFlinger::PlaybackThread::removeEffectChain_l(const sp<EffectChain>& chain)
{
    int session = chain->sessionId();

    ALOGV("removeEffectChain_l() %p from thread %p for session %d", chain.get(), this, session);

    for (size_t i = 0; i < mEffectChains.size(); i++) {
        if (chain == mEffectChains[i]) {
            mEffectChains.removeAt(i);
            // detach all active tracks from the chain
            for (size_t i = 0 ; i < mActiveTracks.size() ; ++i) {
                sp<Track> track = mActiveTracks[i].promote();
                if (track == 0) {
                    continue;
                }
                if (session == track->sessionId()) {
                    ALOGV("removeEffectChain_l(): stopping track on chain %p for session Id: %d",
                            chain.get(), session);
                    chain->decActiveTrackCnt();
                }
            }

            // detach all tracks with same session ID from this chain
            for (size_t i = 0; i < mTracks.size(); ++i) {
                sp<Track> track = mTracks[i];
                if (session == track->sessionId()) {
                    track->setMainBuffer(mMixBuffer);
                    chain->decTrackCnt();
                }
            }
            break;
        }
    }
    return mEffectChains.size();
}

status_t AudioFlinger::PlaybackThread::attachAuxEffect(
        const sp<AudioFlinger::PlaybackThread::Track> track, int EffectId)
{
    Mutex::Autolock _l(mLock);
    return attachAuxEffect_l(track, EffectId);
}

status_t AudioFlinger::PlaybackThread::attachAuxEffect_l(
        const sp<AudioFlinger::PlaybackThread::Track> track, int EffectId)
{
    status_t status = NO_ERROR;

    if (EffectId == 0) {
        track->setAuxBuffer(0, NULL);
    } else {
        // Auxiliary effects are always in audio session AUDIO_SESSION_OUTPUT_MIX
        sp<EffectModule> effect = getEffect_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);
        if (effect != 0) {
            if ((effect->desc().flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
                track->setAuxBuffer(EffectId, (int32_t *)effect->inBuffer());
            } else {
                status = INVALID_OPERATION;
            }
        } else {
            status = BAD_VALUE;
        }
    }
    return status;
}

void AudioFlinger::PlaybackThread::detachAuxEffect_l(int effectId)
{
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<Track> track = mTracks[i];
        if (track->auxEffectId() == effectId) {
            attachAuxEffect_l(track, 0);
        }
    }
}

bool AudioFlinger::PlaybackThread::threadLoop()
{
    Vector< sp<Track> > tracksToRemove;

    standbyTime = systemTime();
#ifdef SRS_PROCESSING
if (mType == MIXER) {
        POSTPRO_PATCH_ICS_OUTPROC_MIX_INIT(this, gettid());
    } else if (mType == DUPLICATING) {
        POSTPRO_PATCH_ICS_OUTPROC_DUPE_INIT(this, gettid());
    }
#endif
    // MIXER
    nsecs_t lastWarning = 0;

    // DUPLICATING
    // FIXME could this be made local to while loop?
    writeFrames = 0;

    cacheParameters_l();
    sleepTime = idleSleepTime;

    if (mType == MIXER) {
        sleepTimeShift = 0;
    }

    CpuStats cpuStats;
    const String8 myName(String8::format("thread %p type %d TID %d", this, mType, gettid()));

    acquireWakeLock();

    // mNBLogWriter->log can only be called while thread mutex mLock is held.
    // So if you need to log when mutex is unlocked, set logString to a non-NULL string,
    // and then that string will be logged at the next convenient opportunity.
    const char *logString = NULL;

    while (!exitPending())
    {
        cpuStats.sample(myName);

        Vector< sp<EffectChain> > effectChains;

        processConfigEvents();

        { // scope for mLock

            Mutex::Autolock _l(mLock);

            if (logString != NULL) {
                mNBLogWriter->logTimestamp();
                mNBLogWriter->log(logString);
                logString = NULL;
            }

            if (checkForNewParameters_l()) {
                cacheParameters_l();
            }

            saveOutputTracks();

            // put audio hardware into standby after short delay
            if (CC_UNLIKELY((!mActiveTracks.size() && systemTime() > standbyTime) ||
                        isSuspended())) {
                if (!mStandby) {

                    threadLoop_standby();

                    mStandby = true;
                }

                if (!mActiveTracks.size() && mConfigEvents.isEmpty()) {
                    // we're about to wait, flush the binder command buffer
                    IPCThreadState::self()->flushCommands();

                    clearOutputTracks();

                    if (exitPending()) {
                        break;
                    }

                    releaseWakeLock_l();
                    // wait until we have something to do...
                    ALOGV("%s going to sleep", myName.string());
                    mWaitWorkCV.wait(mLock);
                    ALOGV("%s waking up", myName.string());
                    acquireWakeLock_l();

                    mMixerStatus = MIXER_IDLE;
                    mMixerStatusIgnoringFastTracks = MIXER_IDLE;
                    mBytesWritten = 0;

                    checkSilentMode_l();

                    standbyTime = systemTime() + standbyDelay;
                    sleepTime = idleSleepTime;
                    if (mType == MIXER) {
                        sleepTimeShift = 0;
                    }

                    continue;
                }
            }

            // mMixerStatusIgnoringFastTracks is also updated internally
            mMixerStatus = prepareTracks_l(&tracksToRemove);

            // prevent any changes in effect chain list and in each effect chain
            // during mixing and effect process as the audio buffers could be deleted
            // or modified if an effect is created or deleted
            lockEffectChains_l(effectChains);
        }

        if (CC_LIKELY(mMixerStatus == MIXER_TRACKS_READY)) {
            threadLoop_mix();
        } else {
            threadLoop_sleepTime();
        }

        if (isSuspended()) {
            sleepTime = suspendSleepTimeUs();
            mBytesWritten += mixBufferSize;
        }

        // only process effects if we're going to write
        if (sleepTime == 0) {
            for (size_t i = 0; i < effectChains.size(); i ++) {
                effectChains[i]->process_l();
            }
        }

        // enable changes in effect chain
        unlockEffectChains(effectChains);

        // sleepTime == 0 means we must write to audio hardware
        if (sleepTime == 0) {
#ifdef SRS_PROCESSING
        if (mType == MIXER) {
             POSTPRO_PATCH_ICS_OUTPROC_MIX_SAMPLES(this, mFormat, mMixBuffer,
                mixBufferSize, mSampleRate, mChannelCount);
        } else if (mType == DUPLICATING) {
            POSTPRO_PATCH_ICS_OUTPROC_DUPE_SAMPLES(this, mFormat, mMixBuffer,
                mixBufferSize, mSampleRate, mChannelCount);
        }
#endif
            threadLoop_write();

if (mType == MIXER) {
            // write blocked detection
            nsecs_t now = systemTime();
            nsecs_t delta = now - mLastWriteTime;
            if (!mStandby && delta > maxPeriod) {
                mNumDelayedWrites++;
                if ((now - lastWarning) > kWarningThrottleNs) {
                    ATRACE_NAME("underrun");
                    ALOGW("write blocked for %llu msecs, %d delayed writes, thread %p",
                            ns2ms(delta), mNumDelayedWrites, this);
                    lastWarning = now;
                }
            }
}

            mStandby = false;
        } else {
            usleep(sleepTime);
        }

        // Finally let go of removed track(s), without the lock held
        // since we can't guarantee the destructors won't acquire that
        // same lock.  This will also mutate and push a new fast mixer state.
        threadLoop_removeTracks(tracksToRemove);
        tracksToRemove.clear();

        // FIXME I don't understand the need for this here;
        //       it was in the original code but maybe the
        //       assignment in saveOutputTracks() makes this unnecessary?
        clearOutputTracks();

        // Effect chains will be actually deleted here if they were removed from
        // mEffectChains list during mixing or effects processing
        effectChains.clear();

        // FIXME Note that the above .clear() is no longer necessary since effectChains
        // is now local to this block, but will keep it for now (at least until merge done).
    }

    // for DuplicatingThread, standby mode is handled by the outputTracks, otherwise ...
    if (mType == MIXER || mType == DIRECT) {
        // put output stream into standby mode
        if (!mStandby) {
            mOutput->stream->common.standby(&mOutput->stream->common);
        }
    }
#ifdef SRS_PROCESSING
        if (mType == MIXER) {
            POSTPRO_PATCH_ICS_OUTPROC_MIX_EXIT(this, gettid());
        } else if (mType == DUPLICATING) {
            POSTPRO_PATCH_ICS_OUTPROC_DUPE_EXIT(this, gettid());
        }
#endif
    releaseWakeLock();

    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}


// ----------------------------------------------------------------------------

AudioFlinger::MixerThread::MixerThread(const sp<AudioFlinger>& audioFlinger, AudioStreamOut* output,
        audio_io_handle_t id, audio_devices_t device, type_t type)
    :   PlaybackThread(audioFlinger, output, id, device, type),
        // mAudioMixer below
        // mFastMixer below
        mFastMixerFutex(0)
        // mOutputSink below
        // mPipeSink below
        // mNormalSink below
{
    ALOGV("MixerThread() id=%d device=%#x type=%d", id, device, type);
    ALOGV("mSampleRate=%u, mChannelMask=%#x, mChannelCount=%d, mFormat=%d, mFrameSize=%u, "
            "mFrameCount=%d, mNormalFrameCount=%d",
            mSampleRate, mChannelMask, mChannelCount, mFormat, mFrameSize, mFrameCount,
            mNormalFrameCount);
    mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);

    // FIXME - Current mixer implementation only supports stereo output
    if (mChannelCount != FCC_2) {
        ALOGE("Invalid audio hardware channel count %d", mChannelCount);
    }

    // create an NBAIO sink for the HAL output stream, and negotiate
    mOutputSink = new AudioStreamOutSink(output->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(mSampleRate, mChannelCount)};
    ssize_t index = mOutputSink->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);

    // initialize fast mixer depending on configuration
    bool initFastMixer;
    switch (kUseFastMixer) {
    case FastMixer_Never:
        initFastMixer = false;
        break;
    case FastMixer_Always:
        initFastMixer = true;
        break;
    case FastMixer_Static:
    case FastMixer_Dynamic:
        initFastMixer = mFrameCount < mNormalFrameCount;
        break;
    }
    if (initFastMixer) {

        // create a MonoPipe to connect our submix to FastMixer
        NBAIO_Format format = mOutputSink->format();
        // This pipe depth compensates for scheduling latency of the normal mixer thread.
        // When it wakes up after a maximum latency, it runs a few cycles quickly before
        // finally blocking.  Note the pipe implementation rounds up the request to a power of 2.
        MonoPipe *monoPipe = new MonoPipe(mNormalFrameCount * 4, format, true /*writeCanBlock*/);
        const NBAIO_Format offers[1] = {format};
        size_t numCounterOffers = 0;
        ssize_t index = monoPipe->negotiate(offers, 1, NULL, numCounterOffers);
        ALOG_ASSERT(index == 0);
        monoPipe->setAvgFrames((mScreenState & 1) ?
                (monoPipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
        mPipeSink = monoPipe;

#ifdef TEE_SINK
        if (mTeeSinkOutputEnabled) {
            // create a Pipe to archive a copy of FastMixer's output for dumpsys
            Pipe *teeSink = new Pipe(mTeeSinkOutputFrames, format);
            numCounterOffers = 0;
            index = teeSink->negotiate(offers, 1, NULL, numCounterOffers);
            ALOG_ASSERT(index == 0);
            mTeeSink = teeSink;
            PipeReader *teeSource = new PipeReader(*teeSink);
            numCounterOffers = 0;
            index = teeSource->negotiate(offers, 1, NULL, numCounterOffers);
            ALOG_ASSERT(index == 0);
            mTeeSource = teeSource;
        }
#endif

        // create fast mixer and configure it initially with just one fast track for our submix
        mFastMixer = new FastMixer();
        FastMixerStateQueue *sq = mFastMixer->sq();
#ifdef STATE_QUEUE_DUMP
        sq->setObserverDump(&mStateQueueObserverDump);
        sq->setMutatorDump(&mStateQueueMutatorDump);
#endif
        FastMixerState *state = sq->begin();
        FastTrack *fastTrack = &state->mFastTracks[0];
        // wrap the source side of the MonoPipe to make it an AudioBufferProvider
        fastTrack->mBufferProvider = new SourceAudioBufferProvider(new MonoPipeReader(monoPipe));
        fastTrack->mVolumeProvider = NULL;
        fastTrack->mGeneration++;
        state->mFastTracksGen++;
        state->mTrackMask = 1;
        // fast mixer will use the HAL output sink
        state->mOutputSink = mOutputSink.get();
        state->mOutputSinkGen++;
        state->mFrameCount = mFrameCount;
        state->mCommand = FastMixerState::COLD_IDLE;
        // already done in constructor initialization list
        //mFastMixerFutex = 0;
        state->mColdFutexAddr = &mFastMixerFutex;
        state->mColdGen++;
        state->mDumpState = &mFastMixerDumpState;
#ifdef TEE_SINK
        state->mTeeSink = mTeeSink.get();
#endif
        mFastMixerNBLogWriter = audioFlinger->newWriter_l(kFastMixerLogSize, "FastMixer");
        state->mNBLogWriter = mFastMixerNBLogWriter.get();
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);

        // start the fast mixer
        mFastMixer->run("FastMixer", PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastMixer->getTid();
        int err = requestPriority(getpid_cached, tid, kPriorityFastMixer);
        if (err != 0) {
            ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                    kPriorityFastMixer, getpid_cached, tid, err);
        }

#ifdef AUDIO_WATCHDOG
        // create and start the watchdog
        mAudioWatchdog = new AudioWatchdog();
        mAudioWatchdog->setDump(&mAudioWatchdogDump);
        mAudioWatchdog->run("AudioWatchdog", PRIORITY_URGENT_AUDIO);
        tid = mAudioWatchdog->getTid();
        err = requestPriority(getpid_cached, tid, kPriorityFastMixer);
        if (err != 0) {
            ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                    kPriorityFastMixer, getpid_cached, tid, err);
        }
#endif

    } else {
        mFastMixer = NULL;
    }

    switch (kUseFastMixer) {
    case FastMixer_Never:
    case FastMixer_Dynamic:
        mNormalSink = mOutputSink;
        break;
    case FastMixer_Always:
        mNormalSink = mPipeSink;
        break;
    case FastMixer_Static:
        mNormalSink = initFastMixer ? mPipeSink : mOutputSink;
        break;
    }
}

AudioFlinger::MixerThread::~MixerThread()
{
    if (mFastMixer != NULL) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand == FastMixerState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastMixerFutex);
            if (old == -1) {
                __futex_syscall3(&mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
            }
        }
        state->mCommand = FastMixerState::EXIT;
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
        mFastMixer->join();
        // Though the fast mixer thread has exited, it's state queue is still valid.
        // We'll use that extract the final state which contains one remaining fast track
        // corresponding to our sub-mix.
        state = sq->begin();
        ALOG_ASSERT(state->mTrackMask == 1);
        FastTrack *fastTrack = &state->mFastTracks[0];
        ALOG_ASSERT(fastTrack->mBufferProvider != NULL);
        delete fastTrack->mBufferProvider;
        sq->end(false /*didModify*/);
        delete mFastMixer;
#ifdef AUDIO_WATCHDOG
        if (mAudioWatchdog != 0) {
            mAudioWatchdog->requestExit();
            mAudioWatchdog->requestExitAndWait();
            mAudioWatchdog.clear();
        }
#endif
    }
    mAudioFlinger->unregisterWriter(mFastMixerNBLogWriter);
    delete mAudioMixer;
}


uint32_t AudioFlinger::MixerThread::correctLatency_l(uint32_t latency) const
{
    if (mFastMixer != NULL) {
        MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
        if(mSampleRate)
            latency += (pipe->getAvgFrames() * 1000) / mSampleRate;
        else
            ALOGW("SampleRate is 0");
    }
    return latency;
}


void AudioFlinger::MixerThread::threadLoop_removeTracks(const Vector< sp<Track> >& tracksToRemove)
{
    PlaybackThread::threadLoop_removeTracks(tracksToRemove);
}

void AudioFlinger::MixerThread::threadLoop_write()
{
    // FIXME we should only do one push per cycle; confirm this is true
    // Start the fast mixer if it's not already running
    if (mFastMixer != NULL) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand != FastMixerState::MIX_WRITE &&
                (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)) {
            if (state->mCommand == FastMixerState::COLD_IDLE) {
                int32_t old = android_atomic_inc(&mFastMixerFutex);
                if (old == -1) {
                    __futex_syscall3(&mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
                }
#ifdef AUDIO_WATCHDOG
                if (mAudioWatchdog != 0) {
                    mAudioWatchdog->resume();
                }
#endif
            }
            state->mCommand = FastMixerState::MIX_WRITE;
            sq->end();
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mPipeSink;
            }
        } else {
            sq->end(false /*didModify*/);
        }
    }
    PlaybackThread::threadLoop_write();
}

void AudioFlinger::MixerThread::threadLoop_standby()
{
    // Idle the fast mixer if it's currently running
    if (mFastMixer != NULL) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (!(state->mCommand & FastMixerState::IDLE)) {
            state->mCommand = FastMixerState::COLD_IDLE;
            state->mColdFutexAddr = &mFastMixerFutex;
            state->mColdGen++;
            mFastMixerFutex = 0;
            sq->end();
            // BLOCK_UNTIL_PUSHED would be insufficient, as we need it to stop doing I/O now
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_ACKED);
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mOutputSink;
            }
#ifdef AUDIO_WATCHDOG
            if (mAudioWatchdog != 0) {
                mAudioWatchdog->pause();
            }
#endif
        } else {
            sq->end(false /*didModify*/);
        }
    }
    PlaybackThread::threadLoop_standby();
}

// shared by MIXER and DIRECT, overridden by DUPLICATING
void AudioFlinger::PlaybackThread::threadLoop_standby()
{
    ALOGV("Audio hardware entering standby, mixer %p, suspend count %d", this, mSuspended);
    mOutput->stream->common.standby(&mOutput->stream->common);
}

void AudioFlinger::MixerThread::threadLoop_mix()
{
    // obtain the presentation timestamp of the next output buffer
    int64_t pts;
    status_t status = INVALID_OPERATION;

#ifndef ICS_AUDIO_BLOB
    if (mNormalSink != 0) {
        status = mNormalSink->getNextWriteTimestamp(&pts);
    } else {
        status = mOutputSink->getNextWriteTimestamp(&pts);
    }
#endif

    if (status != NO_ERROR) {
        pts = AudioBufferProvider::kInvalidPTS;
    }

    // mix buffers...
    mAudioMixer->process(pts);
    // increase sleep time progressively when application underrun condition clears.
    // Only increase sleep time if the mixer is ready for two consecutive times to avoid
    // that a steady state of alternating ready/not ready conditions keeps the sleep time
    // such that we would underrun the audio HAL.
    if ((sleepTime == 0) && (sleepTimeShift > 0)) {
        sleepTimeShift--;
    }
    sleepTime = 0;
    standbyTime = systemTime() + standbyDelay;
    //TODO: delay standby when effects have a tail
}

void AudioFlinger::MixerThread::threadLoop_sleepTime()
{
    // If no tracks are ready, sleep once for the duration of an output
    // buffer size, then write 0s to the output
    if (sleepTime == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            sleepTime = activeSleepTime >> sleepTimeShift;
            if (sleepTime < kMinThreadSleepTimeUs) {
                sleepTime = kMinThreadSleepTimeUs;
            }
            // reduce sleep time in case of consecutive application underruns to avoid
            // starving the audio HAL. As activeSleepTimeUs() is larger than a buffer
            // duration we would end up writing less data than needed by the audio HAL if
            // the condition persists.
            if (sleepTimeShift < kMaxThreadSleepTimeShift) {
                sleepTimeShift++;
            }
        } else {
            sleepTime = idleSleepTime;
        }
    } else if (mBytesWritten != 0 || (mMixerStatus == MIXER_TRACKS_ENABLED)) {
        memset (mMixBuffer, 0, mixBufferSize);
        sleepTime = 0;
        ALOGV_IF(mBytesWritten == 0 && (mMixerStatus == MIXER_TRACKS_ENABLED),
                "anticipated start");
    }
    // TODO add standby time extension fct of effect tail
}

// prepareTracks_l() must be called with ThreadBase::mLock held
AudioFlinger::PlaybackThread::mixer_state AudioFlinger::MixerThread::prepareTracks_l(
        Vector< sp<Track> > *tracksToRemove)
{

    mixer_state mixerStatus = MIXER_IDLE;
    // find out which tracks need to be processed
    size_t count = mActiveTracks.size();
    size_t mixedTracks = 0;
    size_t tracksWithEffect = 0;
    // counts only _active_ fast tracks
    size_t fastTracks = 0;
    uint32_t resetMask = 0; // bit mask of fast tracks that need to be reset

    float masterVolume = mMasterVolume;
    bool masterMute = mMasterMute;

    if (masterMute) {
        masterVolume = 0;
    }
    // Delegate master volume control to effect in output mix effect chain if needed
    sp<EffectChain> chain = getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
    if (chain != 0) {
        uint32_t v = (uint32_t)(masterVolume * (1 << 24));
        chain->setVolume_l(&v, &v);
        masterVolume = (float)((v + (1 << 23)) >> 24);
        chain.clear();
    }

    // prepare a new state to push
    FastMixerStateQueue *sq = NULL;
    FastMixerState *state = NULL;
    bool didModify = false;
    FastMixerStateQueue::block_t block = FastMixerStateQueue::BLOCK_UNTIL_PUSHED;
    if (mFastMixer != NULL) {
        sq = mFastMixer->sq();
        state = sq->begin();
    }

    for (size_t i=0 ; i<count ; i++) {
        sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) {
            continue;
        }

        // this const just means the local variable doesn't change
        Track* const track = t.get();

        // process fast tracks
        if (track->isFastTrack()) {

            // It's theoretically possible (though unlikely) for a fast track to be created
            // and then removed within the same normal mix cycle.  This is not a problem, as
            // the track never becomes active so it's fast mixer slot is never touched.
            // The converse, of removing an (active) track and then creating a new track
            // at the identical fast mixer slot within the same normal mix cycle,
            // is impossible because the slot isn't marked available until the end of each cycle.
            int j = track->mFastIndex;
            ALOG_ASSERT(0 < j && j < (int)FastMixerState::kMaxFastTracks);
            ALOG_ASSERT(!(mFastTrackAvailMask & (1 << j)));
            FastTrack *fastTrack = &state->mFastTracks[j];

            // Determine whether the track is currently in underrun condition,
            // and whether it had a recent underrun.
            FastTrackDump *ftDump = &mFastMixerDumpState.mTracks[j];
            FastTrackUnderruns underruns = ftDump->mUnderruns;
            uint32_t recentFull = (underruns.mBitFields.mFull -
                    track->mObservedUnderruns.mBitFields.mFull) & UNDERRUN_MASK;
            uint32_t recentPartial = (underruns.mBitFields.mPartial -
                    track->mObservedUnderruns.mBitFields.mPartial) & UNDERRUN_MASK;
            uint32_t recentEmpty = (underruns.mBitFields.mEmpty -
                    track->mObservedUnderruns.mBitFields.mEmpty) & UNDERRUN_MASK;
            uint32_t recentUnderruns = recentPartial + recentEmpty;
            track->mObservedUnderruns = underruns;
            // don't count underruns that occur while stopping or pausing
            // or stopped which can occur when flush() is called while active
            if (!(track->isStopping() || track->isPausing() || track->isStopped())) {
                track->mUnderrunCount += recentUnderruns;
            }

            // This is similar to the state machine for normal tracks,
            // with a few modifications for fast tracks.
            bool isActive = true;
            switch (track->mState) {
            case TrackBase::STOPPING_1:
                // track stays active in STOPPING_1 state until first underrun
                if (recentUnderruns > 0) {
                    track->mState = TrackBase::STOPPING_2;
                }
                break;
            case TrackBase::PAUSING:
                // ramp down is not yet implemented
                track->setPaused();
                break;
            case TrackBase::RESUMING:
                // ramp up is not yet implemented
                track->mState = TrackBase::ACTIVE;
                break;
            case TrackBase::ACTIVE:
                if (recentFull > 0 || recentPartial > 0) {
                    // track has provided at least some frames recently: reset retry count
                    track->mRetryCount = kMaxTrackRetries;
                }
                if (recentUnderruns == 0) {
                    // no recent underruns: stay active
                    break;
                }
                // there has recently been an underrun of some kind
                if (track->sharedBuffer() == 0) {
                    // were any of the recent underruns "empty" (no frames available)?
                    if (recentEmpty == 0) {
                        // no, then ignore the partial underruns as they are allowed indefinitely
                        break;
                    }
                    // there has recently been an "empty" underrun: decrement the retry counter
                    if (--(track->mRetryCount) > 0) {
                        break;
                    }
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED, &track->mCblk->flags);
                    // remove from active list, but state remains ACTIVE [confusing but true]
                    isActive = false;
                    break;
                }
                // fall through
            case TrackBase::STOPPING_2:
            case TrackBase::PAUSED:
            case TrackBase::TERMINATED:
            case TrackBase::STOPPED:
            case TrackBase::FLUSHED:   // flush() while active
                // Check for presentation complete if track is inactive
                // We have consumed all the buffers of this track.
                // This would be incomplete if we auto-paused on underrun
                {
                    size_t audioHALFrames =
                            (mOutput->stream->get_latency(mOutput->stream)*mSampleRate) / 1000;
                    size_t framesWritten = mBytesWritten / mFrameSize;
                    if (!(mStandby || track->presentationComplete(framesWritten, audioHALFrames))) {
                        // track stays in active list until presentation is complete
                        break;
                    }
                }
                if (track->isStopping_2()) {
                    track->mState = TrackBase::STOPPED;
                }
                if (track->isStopped()) {
                    // Can't reset directly, as fast mixer is still polling this track
                    //   track->reset();
                    // So instead mark this track as needing to be reset after push with ack
                    resetMask |= 1 << i;
                }
                isActive = false;
                break;
            case TrackBase::IDLE:
            default:
                LOG_FATAL("unexpected track state %d", track->mState);
            }

            if (isActive) {
                // was it previously inactive?
                if (!(state->mTrackMask & (1 << j))) {
                    ExtendedAudioBufferProvider *eabp = track;
                    VolumeProvider *vp = track;
                    fastTrack->mBufferProvider = eabp;
                    fastTrack->mVolumeProvider = vp;
                    fastTrack->mSampleRate = track->mSampleRate;
                    fastTrack->mChannelMask = track->mChannelMask;
                    fastTrack->mGeneration++;
                    state->mTrackMask |= 1 << j;
                    didModify = true;
                    // no acknowledgement required for newly active tracks
                }
                // cache the combined master volume and stream type volume for fast mixer; this
                // lacks any synchronization or barrier so VolumeProvider may read a stale value
                track->mCachedVolume = masterVolume * mStreamTypes[track->streamType()].volume;
                ++fastTracks;
            } else {
                // was it previously active?
                if (state->mTrackMask & (1 << j)) {
                    fastTrack->mBufferProvider = NULL;
                    fastTrack->mGeneration++;
                    state->mTrackMask &= ~(1 << j);
                    didModify = true;
                    // If any fast tracks were removed, we must wait for acknowledgement
                    // because we're about to decrement the last sp<> on those tracks.
                    block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
                } else {
                    LOG_FATAL("fast track %d should have been active", j);
                }
                tracksToRemove->add(track);
                // Avoids a misleading display in dumpsys
                track->mObservedUnderruns.mBitFields.mMostRecent = UNDERRUN_FULL;
            }
            continue;
        }

        {   // local variable scope to avoid goto warning

        audio_track_cblk_t* cblk = track->cblk();

        // The first time a track is added we wait
        // for all its buffers to be filled before processing it
        int name = track->name();
        // make sure that we have enough frames to mix one full buffer.
        // enforce this condition only once to enable draining the buffer in case the client
        // app does not call stop() and relies on underrun to stop:
        // hence the test on (mMixerStatus == MIXER_TRACKS_READY) meaning the track was mixed
        // during last round
        uint32_t minFrames = 1;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing() &&
                (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY)) {
            if (t->sampleRate() == mSampleRate) {
                minFrames = mNormalFrameCount;
            } else {
                // +1 for rounding and +1 for additional sample needed for interpolation
                if(mSampleRate)
                    minFrames = (mNormalFrameCount * t->sampleRate()) / mSampleRate + 1 + 1;
                else {
                    minFrames = 2;
                    ALOGW("SampleRate is 0");
                }
                // add frames already consumed but not yet released by the resampler
                // because cblk->framesReady() will include these frames
                minFrames += mAudioMixer->getUnreleasedFrames(track->name());
                // the minimum track buffer size is normally twice the number of frames necessary
                // to fill one buffer and the resampler should not leave more than one buffer worth
                // of unreleased frames after each pass, but just in case...
                ALOG_ASSERT(minFrames <= cblk->frameCount_);
            }
        }
        if ((track->framesReady() >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track %d u=%08x, s=%08x [OK] on thread %p", name, cblk->user, cblk->server,
                    this);

            mixedTracks++;

            // track->mainBuffer() != mMixBuffer means there is an effect chain
            // connected to the track
            chain.clear();
            if (track->mainBuffer() != mMixBuffer) {
                chain = getEffectChain_l(track->sessionId());
                // Delegate volume control to effect in track effect chain if needed
                if (chain != 0) {
                    tracksWithEffect++;
                } else {
                    ALOGW("prepareTracks_l(): track %d attached to effect but no chain found on "
                            "session %d",
                            name, track->sessionId());
                }
            }


            int param = AudioMixer::VOLUME;
            if (track->mFillingUpStatus == Track::FS_FILLED) {
                // no ramp for the first volume setting
                track->mFillingUpStatus = Track::FS_ACTIVE;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                    param = AudioMixer::RAMP_VOLUME;
                }
                mAudioMixer->setParameter(name, AudioMixer::RESAMPLE, AudioMixer::RESET, NULL);
            } else if (cblk->server != 0) {
                // If the track is stopped before the first frame was mixed,
                // do not apply ramp
                param = AudioMixer::RAMP_VOLUME;
            }

            // compute volume for this track
            uint32_t vl, vr, va;
            if (track->isPausing() || mStreamTypes[track->streamType()].mute) {
                vl = vr = va = 0;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {

                // read original volumes with volume control
                float typeVolume = mStreamTypes[track->streamType()].volume;
                float v = masterVolume * typeVolume;
                ServerProxy *proxy = track->mServerProxy;
                uint32_t vlr = proxy->getVolumeLR();
                vl = vlr & 0xFFFF;
                vr = vlr >> 16;
                // track volumes come from shared memory, so can't be trusted and must be clamped
                if (vl > MAX_GAIN_INT) {
                    ALOGV("Track left volume out of range: %04X", vl);
                    vl = MAX_GAIN_INT;
                }
                if (vr > MAX_GAIN_INT) {
                    ALOGV("Track right volume out of range: %04X", vr);
                    vr = MAX_GAIN_INT;
                }
                // now apply the master volume and stream type volume
                vl = (uint32_t)(v * vl) << 12;
                vr = (uint32_t)(v * vr) << 12;
                // assuming master volume and stream type volume each go up to 1.0,
                // vl and vr are now in 8.24 format

                uint16_t sendLevel = proxy->getSendLevel_U4_12();
                // send level comes from shared memory and so may be corrupt
                if (sendLevel > MAX_GAIN_INT) {
                    ALOGV("Track send level out of range: %04X", sendLevel);
                    sendLevel = MAX_GAIN_INT;
                }
                va = (uint32_t)(v * sendLevel);
            }
            // Delegate volume control to effect in track effect chain if needed
            if (chain != 0 && chain->setVolume_l(&vl, &vr)) {
                // Do not ramp volume if volume is controlled by effect
                param = AudioMixer::VOLUME;
                track->mHasVolumeController = true;
            } else {
                // force no volume ramp when volume controller was just disabled or removed
                // from effect chain to avoid volume spike
                if (track->mHasVolumeController) {
                    param = AudioMixer::VOLUME;
                }
                track->mHasVolumeController = false;
            }

            // Convert volumes from 8.24 to 4.12 format
            // This additional clamping is needed in case chain->setVolume_l() overshot
            vl = (vl + (1 << 11)) >> 12;
            if (vl > MAX_GAIN_INT) {
                vl = MAX_GAIN_INT;
            }
            vr = (vr + (1 << 11)) >> 12;
            if (vr > MAX_GAIN_INT) {
                vr = MAX_GAIN_INT;
            }

            if (va > MAX_GAIN_INT) {
                va = MAX_GAIN_INT;   // va is uint32_t, so no need to check for -
            }

            // XXX: these things DON'T need to be done each time
            mAudioMixer->setBufferProvider(name, track);
            mAudioMixer->enable(name);

            mAudioMixer->setParameter(name, param, AudioMixer::VOLUME0, (void *)vl);
            mAudioMixer->setParameter(name, param, AudioMixer::VOLUME1, (void *)vr);
            mAudioMixer->setParameter(name, param, AudioMixer::AUXLEVEL, (void *)va);
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::FORMAT, (void *)track->format());
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::CHANNEL_MASK, (void *)track->channelMask());
            // limit track sample rate to 2 x output sample rate, which changes at re-configuration
            uint32_t maxSampleRate = mSampleRate * 2;
            uint32_t reqSampleRate = track->mServerProxy->getSampleRate();
            if (reqSampleRate == 0) {
                reqSampleRate = mSampleRate;
            } else if (reqSampleRate > maxSampleRate) {
                reqSampleRate = maxSampleRate;
            }
            mAudioMixer->setParameter(
                name,
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                (void *)reqSampleRate);
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::MAIN_BUFFER, (void *)track->mainBuffer());
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::AUX_BUFFER, (void *)track->auxBuffer());

            // reset retry count
            track->mRetryCount = kMaxTrackRetries;

            // If one track is ready, set the mixer ready if:
            //  - the mixer was not ready during previous round OR
            //  - no other track is not ready
            if (mMixerStatusIgnoringFastTracks != MIXER_TRACKS_READY ||
                    mixerStatus != MIXER_TRACKS_ENABLED) {
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            // clear effect chain input buffer if an active track underruns to avoid sending
            // previous audio buffer again to effects
            chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                chain->clearInputBuffer();
            }

            ALOGVV("track %d u=%08x, s=%08x [NOT READY] on thread %p", name, cblk->user,
                    cblk->server, this);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                // TODO: use actual buffer filling status instead of latency when available from
                // audio HAL
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                size_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                track->mUnderrunCount++;
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                if (--(track->mRetryCount) <= 0) {
                    ALOGI("BUFFER TIMEOUT: remove(%d) from active list on thread %p", name, this);
                    tracksToRemove->add(track);
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED, &cblk->flags);
                // If one track is not ready, mark the mixer also not ready if:
                //  - the mixer was ready during previous round OR
                //  - no other track is ready
                } else if (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY ||
                                mixerStatus != MIXER_TRACKS_READY) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
            mAudioMixer->disable(name);
        }

        }   // local variable scope to avoid goto warning
track_is_ready: ;

    }

    // Push the new FastMixer state if necessary
    bool pauseAudioWatchdog = false;
    if (didModify) {
        state->mFastTracksGen++;
        // if the fast mixer was active, but now there are no fast tracks, then put it in cold idle
        if (kUseFastMixer == FastMixer_Dynamic &&
                state->mCommand == FastMixerState::MIX_WRITE && state->mTrackMask <= 1) {
            state->mCommand = FastMixerState::COLD_IDLE;
            state->mColdFutexAddr = &mFastMixerFutex;
            state->mColdGen++;
            mFastMixerFutex = 0;
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mOutputSink;
            }
            // If we go into cold idle, need to wait for acknowledgement
            // so that fast mixer stops doing I/O.
            block = FastMixerStateQueue::BLOCK_UNTIL_ACKED;
            pauseAudioWatchdog = true;
        }
    }
    if (sq != NULL) {
        sq->end(didModify);
        sq->push(block);
    }
#ifdef AUDIO_WATCHDOG
    if (pauseAudioWatchdog && mAudioWatchdog != 0) {
        mAudioWatchdog->pause();
    }
#endif

    // Now perform the deferred reset on fast tracks that have stopped
    while (resetMask != 0) {
        size_t i = __builtin_ctz(resetMask);
        ALOG_ASSERT(i < count);
        resetMask &= ~(1 << i);
        sp<Track> t = mActiveTracks[i].promote();
        if (t == 0) {
            continue;
        }
        Track* track = t.get();
        ALOG_ASSERT(track->isFastTrack() && track->isStopped());
        track->reset();
    }

    // remove all the tracks that need to be...
    count = tracksToRemove->size();
    if (CC_UNLIKELY(count)) {
        for (size_t i=0 ; i<count ; i++) {
            const sp<Track>& track = tracksToRemove->itemAt(i);
            mActiveTracks.remove(track);
            if (track->mainBuffer() != mMixBuffer) {
                chain = getEffectChain_l(track->sessionId());
                if (chain != 0) {
                    ALOGV("stopping track on chain %p for session Id: %d", chain.get(),
                            track->sessionId());
                    chain->decActiveTrackCnt();
                }
            }
            if (track->isTerminated()) {
                removeTrack_l(track);
            }
        }
    }

    // mix buffer must be cleared if all tracks are connected to an
    // effect chain as in this case the mixer will not write to
    // mix buffer and track effects will accumulate into it
    if ((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            (mixedTracks == 0 && fastTracks > 0)) {
        // FIXME as a performance optimization, should remember previous zero status
        memset(mMixBuffer, 0, mNormalFrameCount * mChannelCount * sizeof(int16_t));
    }

    // if any fast tracks, then status is ready
    mMixerStatusIgnoringFastTracks = mixerStatus;
    if (fastTracks > 0) {
        mixerStatus = MIXER_TRACKS_READY;
    }
    return mixerStatus;
}

// getTrackName_l() must be called with ThreadBase::mLock held
int AudioFlinger::MixerThread::getTrackName_l(audio_channel_mask_t channelMask, int sessionId)
{
    return mAudioMixer->getTrackName(channelMask, sessionId);
}

// deleteTrackName_l() must be called with ThreadBase::mLock held
void AudioFlinger::MixerThread::deleteTrackName_l(int name)
{
    ALOGV("remove track (%d) and delete from mixer", name);
    mAudioMixer->deleteTrackName(name);
}

// checkForNewParameters_l() must be called with ThreadBase::mLock held
bool AudioFlinger::MixerThread::checkForNewParameters_l()
{
    // if !&IDLE, holds the FastMixer state to restore after new parameters processed
    FastMixerState::Command previousCommand = FastMixerState::HOT_IDLE;
    bool reconfig = false;

    while (!mNewParameters.isEmpty()) {

        if (mFastMixer != NULL) {
            FastMixerStateQueue *sq = mFastMixer->sq();
            FastMixerState *state = sq->begin();
            if (!(state->mCommand & FastMixerState::IDLE)) {
                previousCommand = state->mCommand;
                state->mCommand = FastMixerState::HOT_IDLE;
                sq->end();
                sq->push(FastMixerStateQueue::BLOCK_UNTIL_ACKED);
            } else {
                sq->end(false /*didModify*/);
            }
        }

        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;
#ifdef SRS_PROCESSING
        POSTPRO_PATCH_ICS_OUTPROC_MIX_ROUTE(this, param, value);
#endif
        if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
            if ((audio_format_t) value != AUDIO_FORMAT_PCM_16_BIT) {
                status = BAD_VALUE;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
            if (value != AUDIO_CHANNEL_OUT_STEREO) {
                status = BAD_VALUE;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            // do not accept frame count changes if tracks are open as the track buffer
            // size depends on frame count and correct behavior would not be guaranteed
            // if frame count is changed after track creation
            if (!mTracks.isEmpty()) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
#ifdef ADD_BATTERY_DATA
            // when changing the audio output device, call addBatteryData to notify
            // the change
            if (mOutDevice != value) {
                uint32_t params = 0;
                // check whether speaker is on
                if (value & AUDIO_DEVICE_OUT_SPEAKER) {
                    params |= IMediaPlayerService::kBatteryDataSpeakerOn;
                }

                audio_devices_t deviceWithoutSpeaker
                    = AUDIO_DEVICE_OUT_ALL & ~AUDIO_DEVICE_OUT_SPEAKER;
                // check if any other device (except speaker) is on
                if (value & deviceWithoutSpeaker ) {
                    params |= IMediaPlayerService::kBatteryDataOtherAudioDeviceOn;
                }

                if (params != 0) {
                    addBatteryData(params);
                }
            }
#endif

            // forward device change to effects that have requested to be
            // aware of attached audio device.
            if (value != AUDIO_DEVICE_NONE) {
                mOutDevice = value;
                for (size_t i = 0; i < mEffectChains.size(); i++) {
                    mEffectChains[i]->setDevice_l(mOutDevice);
                }
            }
        }

        if (status == NO_ERROR) {
            status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                    keyValuePair.string());
            if (!mStandby && status == INVALID_OPERATION) {
                mOutput->stream->common.standby(&mOutput->stream->common);
                mStandby = true;
                mBytesWritten = 0;
                status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                       keyValuePair.string());
            }
            if (status == NO_ERROR && reconfig) {
                delete mAudioMixer;
                // for safety in case readOutputParameters() accesses mAudioMixer (it doesn't)
                mAudioMixer = NULL;
                readOutputParameters();
                mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
                for (size_t i = 0; i < mTracks.size() ; i++) {
                    int name = getTrackName_l(mTracks[i]->mChannelMask, mTracks[i]->mSessionId);
                    if (name < 0) {
                        break;
                    }
                    mTracks[i]->mName = name;
                }
                sendIoConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
            }
        }

        mNewParameters.removeAt(0);

        mParamStatus = status;
        mParamCond.signal();
        // wait for condition with time out in case the thread calling ThreadBase::setParameters()
        // already timed out waiting for the status and will never signal the condition.
        mWaitWorkCV.waitRelative(mLock, kSetParametersTimeoutNs);
    }

    if (!(previousCommand & FastMixerState::IDLE)) {
        ALOG_ASSERT(mFastMixer != NULL);
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        ALOG_ASSERT(state->mCommand == FastMixerState::HOT_IDLE);
        state->mCommand = previousCommand;
        sq->end();
        sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
    }

    return reconfig;
}


void AudioFlinger::MixerThread::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    PlaybackThread::dumpInternals(fd, args);

    snprintf(buffer, SIZE, "AudioMixer tracks: %08x\n", mAudioMixer->trackNames());
    result.append(buffer);
    write(fd, result.string(), result.size());

    // Make a non-atomic copy of fast mixer dump state so it won't change underneath us
    FastMixerDumpState copy = mFastMixerDumpState;
    copy.dump(fd);

#ifdef STATE_QUEUE_DUMP
    // Similar for state queue
    StateQueueObserverDump observerCopy = mStateQueueObserverDump;
    observerCopy.dump(fd);
    StateQueueMutatorDump mutatorCopy = mStateQueueMutatorDump;
    mutatorCopy.dump(fd);
#endif

#ifdef TEE_SINK
    // Write the tee output to a .wav file
    dumpTee(fd, mTeeSource, mId);
#endif

#ifdef AUDIO_WATCHDOG
    if (mAudioWatchdog != 0) {
        // Make a non-atomic copy of audio watchdog dump so it won't change underneath us
        AudioWatchdogDump wdCopy = mAudioWatchdogDump;
        wdCopy.dump(fd);
    }
#endif
}

uint32_t AudioFlinger::MixerThread::idleSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000) / 2;
}

uint32_t AudioFlinger::MixerThread::suspendSleepTimeUs() const
{
    return (uint32_t)(((mNormalFrameCount * 1000) / mSampleRate) * 1000);
}

void AudioFlinger::MixerThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();

    // FIXME: Relaxed timing because of a certain device that can't meet latency
    // Should be reduced to 2x after the vendor fixes the driver issue
    // increase threshold again due to low power audio mode. The way this warning
    // threshold is calculated and its usefulness should be reconsidered anyway.
    maxPeriod = seconds(mNormalFrameCount) / mSampleRate * 15;
}

// ----------------------------------------------------------------------------
AudioFlinger::DirectOutputThread::DirectOutputThread(const sp<AudioFlinger>& audioFlinger,
        AudioStreamOut* output, audio_io_handle_t id, audio_devices_t device)
    :   PlaybackThread(audioFlinger, output, id, device, DIRECT)
        // mLeftVolFloat, mRightVolFloat
{
}

AudioFlinger::DirectOutputThread::~DirectOutputThread()
{
}

AudioFlinger::PlaybackThread::mixer_state AudioFlinger::DirectOutputThread::prepareTracks_l(
    Vector< sp<Track> > *tracksToRemove
)
{
    size_t count = mActiveTracks.size();
    mixer_state mixerStatus = MIXER_IDLE;

    // find out which tracks need to be processed
    for (size_t i = 0; i < count; i++) {
        sp<Track> t = mActiveTracks[i].promote();
        // The track died recently
        if (t == 0) {
            continue;
        }

        Track* const track = t.get();
        audio_track_cblk_t* cblk = track->cblk();

        // The first time a track is added we wait
        // for all its buffers to be filled before processing it
        uint32_t minFrames;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing()) {
            minFrames = mNormalFrameCount;
        } else {
            minFrames = 1;
        }
        if ((track->framesReady() >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track %d u=%08x, s=%08x [OK]", track->name(), cblk->user, cblk->server);

            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                mLeftVolFloat = mRightVolFloat = 0;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                }
            }

            // compute volume for this track
            float left, right;
            if (mMasterMute || track->isPausing() || mStreamTypes[track->streamType()].mute) {
                left = right = 0;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {
                float typeVolume = mStreamTypes[track->streamType()].volume;
                float v = mMasterVolume * typeVolume;
                uint32_t vlr = track->mServerProxy->getVolumeLR();
                float v_clamped = v * (vlr & 0xFFFF);
                if (v_clamped > MAX_GAIN) {
                    v_clamped = MAX_GAIN;
                }
                left = v_clamped/MAX_GAIN;
                v_clamped = v * (vlr >> 16);
                if (v_clamped > MAX_GAIN) {
                    v_clamped = MAX_GAIN;
                }
                right = v_clamped/MAX_GAIN;
            }
            // Only consider last track started for volume and mixer state control.
            // This is the last entry in mActiveTracks unless a track underruns.
            // As we only care about the transition phase between two tracks on a
            // direct output, it is not a problem to ignore the underrun case.
            if (i == (count - 1)) {
                if (left != mLeftVolFloat || right != mRightVolFloat) {
                    mLeftVolFloat = left;
                    mRightVolFloat = right;

                    // Convert volumes from float to 8.24
                    uint32_t vl = (uint32_t)(left * (1 << 24));
                    uint32_t vr = (uint32_t)(right * (1 << 24));

                    // Delegate volume control to effect in track effect chain if needed
                    // only one effect chain can be present on DirectOutputThread, so if
                    // there is one, the track is connected to it
                    if (!mEffectChains.isEmpty()) {
                        // Do not ramp volume if volume is controlled by effect
                        mEffectChains[0]->setVolume_l(&vl, &vr);
                        left = (float)vl / (1 << 24);
                        right = (float)vr / (1 << 24);
                    }
                    mOutput->stream->set_volume(mOutput->stream, left, right);
                }

                // reset retry count
                track->mRetryCount = kMaxTrackRetriesDirect;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            // clear effect chain input buffer if the last active track started underruns
            // to avoid sending previous audio buffer again to effects
            if (!mEffectChains.isEmpty() && (i == (count -1))) {
                mEffectChains[0]->clearInputBuffer();
            }

            ALOGVV("track %d u=%08x, s=%08x [NOT READY]", track->name(), cblk->user, cblk->server);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                // TODO: implement behavior for compressed audio
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                size_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    tracksToRemove->add(track);
                }
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                // Only consider last track started for mixer state control
                if (--(track->mRetryCount) <= 0) {
                    ALOGV("BUFFER TIMEOUT: remove(%d) from active list", track->name());
                    tracksToRemove->add(track);
                    // indicate to client process that the track was disabled because of underrun
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED, &cblk->flags);
                } else if (i == (count -1)){
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
    }

    // remove all the tracks that need to be...
    count = tracksToRemove->size();
    if (CC_UNLIKELY(count)) {
        for (size_t i = 0 ; i < count ; i++) {
            const sp<Track>& track = tracksToRemove->itemAt(i);
            mActiveTracks.remove(track);
            if (!mEffectChains.isEmpty()) {
                ALOGV("stopping track on chain %p for session Id: %d", mEffectChains[0].get(),
                      track->sessionId());
                mEffectChains[0]->decActiveTrackCnt();
            }
            if (track->isTerminated()) {
                removeTrack_l(track);
            }
        }
    }

    return mixerStatus;
}

void AudioFlinger::DirectOutputThread::threadLoop_mix()
{
    AudioBufferProvider::Buffer buffer;
    size_t frameCount = mFrameCount;
    int8_t *curBuf = (int8_t *)mMixBuffer;
    // output audio to hardware
    while (frameCount) {
        buffer.frameCount = frameCount;
        mActiveTrack->getNextBuffer(&buffer);
        if (CC_UNLIKELY(buffer.raw == NULL)) {
            memset(curBuf, 0, frameCount * mFrameSize);
            break;
        }
        memcpy(curBuf, buffer.raw, buffer.frameCount * mFrameSize);
        frameCount -= buffer.frameCount;
        curBuf += buffer.frameCount * mFrameSize;
        mActiveTrack->releaseBuffer(&buffer);
    }
    sleepTime = 0;
    standbyTime = systemTime() + standbyDelay;
    mActiveTrack.clear();

}

void AudioFlinger::DirectOutputThread::threadLoop_sleepTime()
{
    if (sleepTime == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            sleepTime = activeSleepTime;
        } else {
            sleepTime = idleSleepTime;
        }
    } else if (mBytesWritten != 0 && audio_is_linear_pcm(mFormat)) {
        memset(mMixBuffer, 0, mFrameCount * mFrameSize);
        sleepTime = 0;
    }
}

// getTrackName_l() must be called with ThreadBase::mLock held
int AudioFlinger::DirectOutputThread::getTrackName_l(audio_channel_mask_t channelMask,
        int sessionId)
{
    return 0;
}

// deleteTrackName_l() must be called with ThreadBase::mLock held
void AudioFlinger::DirectOutputThread::deleteTrackName_l(int name)
{
}

// checkForNewParameters_l() must be called with ThreadBase::mLock held
bool AudioFlinger::DirectOutputThread::checkForNewParameters_l()
{
    bool reconfig = false;

    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;

        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            // do not accept frame count changes if tracks are open as the track buffer
            // size depends on frame count and correct behavior would not be garantied
            // if frame count is changed after track creation
            if (!mTracks.isEmpty()) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (status == NO_ERROR) {
            status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                    keyValuePair.string());
            if (!mStandby && status == INVALID_OPERATION) {
                mOutput->stream->common.standby(&mOutput->stream->common);
                mStandby = true;
                mBytesWritten = 0;
                status = mOutput->stream->common.set_parameters(&mOutput->stream->common,
                                                       keyValuePair.string());
            }
            if (status == NO_ERROR && reconfig) {
                readOutputParameters();
                sendIoConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
            }
        }

        mNewParameters.removeAt(0);

        mParamStatus = status;
        mParamCond.signal();
        // wait for condition with time out in case the thread calling ThreadBase::setParameters()
        // already timed out waiting for the status and will never signal the condition.
        mWaitWorkCV.waitRelative(mLock, kSetParametersTimeoutNs);
    }
    return reconfig;
}

uint32_t AudioFlinger::DirectOutputThread::activeSleepTimeUs() const
{
    uint32_t time;
    if (audio_is_linear_pcm(mFormat)) {
        time = PlaybackThread::activeSleepTimeUs();
    } else {
        time = 10000;
    }
    return time;
}

uint32_t AudioFlinger::DirectOutputThread::idleSleepTimeUs() const
{
    uint32_t time;
    if (audio_is_linear_pcm(mFormat)) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000) / 2;
    } else {
        time = 10000;
    }
    return time;
}

uint32_t AudioFlinger::DirectOutputThread::suspendSleepTimeUs() const
{
    uint32_t time;
    if (audio_is_linear_pcm(mFormat)) {
        time = (uint32_t)(((mFrameCount * 1000) / mSampleRate) * 1000);
    } else {
        time = 10000;
    }
    return time;
}

void AudioFlinger::DirectOutputThread::cacheParameters_l()
{
    PlaybackThread::cacheParameters_l();

    // use shorter standby delay as on normal output to release
    // hardware resources as soon as possible
    standbyDelay = microseconds(activeSleepTime*2);
}

// ----------------------------------------------------------------------------

AudioFlinger::DuplicatingThread::DuplicatingThread(const sp<AudioFlinger>& audioFlinger,
        AudioFlinger::MixerThread* mainThread, audio_io_handle_t id)
    :   MixerThread(audioFlinger, mainThread->getOutput(), id, mainThread->outDevice(),
                DUPLICATING),
        mWaitTimeMs(UINT_MAX)
{
    addOutputTrack(mainThread);
}

AudioFlinger::DuplicatingThread::~DuplicatingThread()
{
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        mOutputTracks[i]->destroy();
    }
}

void AudioFlinger::DuplicatingThread::threadLoop_mix()
{
    // mix buffers...
    if (outputsReady(outputTracks)) {
        mAudioMixer->process(AudioBufferProvider::kInvalidPTS);
    } else {
        memset(mMixBuffer, 0, mixBufferSize);
    }
    sleepTime = 0;
    writeFrames = mNormalFrameCount;
    standbyTime = systemTime() + standbyDelay;
}

void AudioFlinger::DuplicatingThread::threadLoop_sleepTime()
{
    if (sleepTime == 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            sleepTime = activeSleepTime;
        } else {
            sleepTime = idleSleepTime;
        }
    } else if (mBytesWritten != 0) {
        if (mMixerStatus == MIXER_TRACKS_ENABLED) {
            writeFrames = mNormalFrameCount;
            memset(mMixBuffer, 0, mixBufferSize);
        } else {
            // flush remaining overflow buffers in output tracks
            writeFrames = 0;
        }
        sleepTime = 0;
    }
}

void AudioFlinger::DuplicatingThread::threadLoop_write()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        outputTracks[i]->write(mMixBuffer, writeFrames);
    }
    mBytesWritten += mixBufferSize;
}

void AudioFlinger::DuplicatingThread::threadLoop_standby()
{
    // DuplicatingThread implements standby by stopping all tracks
    for (size_t i = 0; i < outputTracks.size(); i++) {
        outputTracks[i]->stop();
    }
}

void AudioFlinger::DuplicatingThread::saveOutputTracks()
{
    outputTracks = mOutputTracks;
}

void AudioFlinger::DuplicatingThread::clearOutputTracks()
{
    outputTracks.clear();
}

void AudioFlinger::DuplicatingThread::addOutputTrack(MixerThread *thread)
{
    Mutex::Autolock _l(mLock);
    // FIXME explain this formula
    int sampleRate = thread->sampleRate();
    int frameCount = 0;
    if (sampleRate)
        frameCount = (3 * mNormalFrameCount * mSampleRate) / thread->sampleRate();
    OutputTrack *outputTrack = new OutputTrack(thread,
                                            this,
                                            mSampleRate,
                                            mFormat,
                                            mChannelMask,
                                            frameCount);
    if (outputTrack->cblk() != NULL) {
        thread->setStreamVolume(AUDIO_STREAM_CNT, 1.0f);
        mOutputTracks.add(outputTrack);
        ALOGV("addOutputTrack() track %p, on thread %p", outputTrack, thread);
        updateWaitTime_l();
    }
}

void AudioFlinger::DuplicatingThread::removeOutputTrack(MixerThread *thread)
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        if (mOutputTracks[i]->thread() == thread) {
            mOutputTracks[i]->destroy();
            mOutputTracks.removeAt(i);
            updateWaitTime_l();
            return;
        }
    }
    ALOGV("removeOutputTrack(): unkonwn thread: %p", thread);
}

// caller must hold mLock
void AudioFlinger::DuplicatingThread::updateWaitTime_l()
{
    mWaitTimeMs = UINT_MAX;
    for (size_t i = 0; i < mOutputTracks.size(); i++) {
        sp<ThreadBase> strong = mOutputTracks[i]->thread().promote();
        if (strong != 0) {
            uint32_t waitTimeMs = (strong->frameCount() * 2 * 1000) / strong->sampleRate();
            if (waitTimeMs < mWaitTimeMs) {
                mWaitTimeMs = waitTimeMs;
            }
        }
    }
}


bool AudioFlinger::DuplicatingThread::outputsReady(
        const SortedVector< sp<OutputTrack> > &outputTracks)
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        sp<ThreadBase> thread = outputTracks[i]->thread().promote();
        if (thread == 0) {
            ALOGW("DuplicatingThread::outputsReady() could not promote thread on output track %p",
                    outputTracks[i].get());
            return false;
        }
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        // see note at standby() declaration
        if (playbackThread->standby() && !playbackThread->isSuspended()) {
            ALOGV("DuplicatingThread output track %p on thread %p Not Ready", outputTracks[i].get(),
                    thread.get());
            return false;
        }
    }
    return true;
}

uint32_t AudioFlinger::DuplicatingThread::activeSleepTimeUs() const
{
    return (mWaitTimeMs * 1000) / 2;
}

void AudioFlinger::DuplicatingThread::cacheParameters_l()
{
    // updateWaitTime_l() sets mWaitTimeMs, which affects activeSleepTimeUs(), so call it first
    updateWaitTime_l();

    MixerThread::cacheParameters_l();
}

// ----------------------------------------------------------------------------
//      Record
// ----------------------------------------------------------------------------

AudioFlinger::RecordThread::RecordThread(const sp<AudioFlinger>& audioFlinger,
                                         AudioStreamIn *input,
                                         uint32_t sampleRate,
                                         audio_channel_mask_t channelMask,
                                         audio_io_handle_t id,
                                         audio_devices_t outDevice,
                                         audio_devices_t inDevice
#ifdef TEE_SINK
                                         , const sp<NBAIO_Sink>& teeSink
#endif
                                         ) :
    ThreadBase(audioFlinger, id, outDevice, inDevice, RECORD),
    mInput(input), mResampler(NULL), mRsmpOutBuffer(NULL), mRsmpInBuffer(NULL),
    // mRsmpInIndex and mInputBytes set by readInputParameters()
    mReqChannelCount(getInputChannelCount(channelMask)),
    mReqSampleRate(sampleRate)
    // mBytesRead is only meaningful while active, and so is cleared in start()
    // (but might be better to also clear here for dump?)
#ifdef TEE_SINK
    , mTeeSink(teeSink)
#endif
{
    snprintf(mName, kNameLength, "AudioIn_%X", id);

    readInputParameters();

}


AudioFlinger::RecordThread::~RecordThread()
{
    delete[] mRsmpInBuffer;
    delete mResampler;
    delete[] mRsmpOutBuffer;
}

void AudioFlinger::RecordThread::onFirstRef()
{
    run(mName, PRIORITY_URGENT_AUDIO);
}

status_t AudioFlinger::RecordThread::readyToRun()
{
    status_t status = initCheck();
    ALOGW_IF(status != NO_ERROR,"RecordThread %p could not initialize", this);
    return status;
}

bool AudioFlinger::RecordThread::threadLoop()
{
    AudioBufferProvider::Buffer buffer;
    sp<RecordTrack> activeTrack;
    Vector< sp<EffectChain> > effectChains;

    nsecs_t lastWarning = 0;

    inputStandBy();
    acquireWakeLock();

    // used to verify we've read at least once before evaluating how many bytes were read
    bool readOnce = false;

    // start recording
    while (!exitPending()) {

        processConfigEvents();

        { // scope for mLock
            Mutex::Autolock _l(mLock);
            checkForNewParameters_l();
            if (mActiveTrack == 0 && mConfigEvents.isEmpty()) {
                standby();

                if (exitPending()) {
                    break;
                }

                releaseWakeLock_l();
                ALOGV("RecordThread: loop stopping");
                // go to sleep
                mWaitWorkCV.wait(mLock);
                ALOGV("RecordThread: loop starting");
                acquireWakeLock_l();
                continue;
            }
            if (mActiveTrack != 0) {
                if (mActiveTrack->mState == TrackBase::PAUSING) {
                    standby();
                    mActiveTrack.clear();
                    mStartStopCond.broadcast();
                } else if (mActiveTrack->mState == TrackBase::RESUMING) {
                    if (mReqChannelCount != mActiveTrack->channelCount()) {
                        mActiveTrack.clear();
                        mStartStopCond.broadcast();
                    } else if (readOnce) {
                        // record start succeeds only if first read from audio input
                        // succeeds
                        if (mBytesRead >= 0) {
                            mActiveTrack->mState = TrackBase::ACTIVE;
                        } else {
                            mActiveTrack.clear();
                        }
                        mStartStopCond.broadcast();
                    }
                    mStandby = false;
                } else if (mActiveTrack->mState == TrackBase::TERMINATED) {
                    removeTrack_l(mActiveTrack);
                    mActiveTrack.clear();
                }
            }
            lockEffectChains_l(effectChains);
        }

        if (mActiveTrack != 0) {
            if (mActiveTrack->mState != TrackBase::ACTIVE &&
                mActiveTrack->mState != TrackBase::RESUMING) {
                unlockEffectChains(effectChains);
                usleep(kRecordThreadSleepUs);
                continue;
            }
            for (size_t i = 0; i < effectChains.size(); i ++) {
                effectChains[i]->process_l();
            }

            buffer.frameCount = mFrameCount;
            if (CC_LIKELY(mActiveTrack->getNextBuffer(&buffer) == NO_ERROR)) {
                readOnce = true;
                size_t framesOut = buffer.frameCount;
                if (mResampler == NULL) {
                    // no resampling
                    while (framesOut) {
                        size_t framesIn = mFrameCount - mRsmpInIndex;
                        if (framesIn) {
                            int8_t *src = (int8_t *)mRsmpInBuffer + mRsmpInIndex * mFrameSize;
                            int8_t *dst = buffer.i8 + (buffer.frameCount - framesOut) *
                                    mActiveTrack->mFrameSize;
                            if (framesIn > framesOut)
                                framesIn = framesOut;
                            mRsmpInIndex += framesIn;
                            framesOut -= framesIn;
                            if (mChannelCount == mReqChannelCount ||
                                mFormat != AUDIO_FORMAT_PCM_16_BIT) {
                                memcpy(dst, src, framesIn * mFrameSize);
                            } else {
                                if (mChannelCount == 1) {
                                    upmix_to_stereo_i16_from_mono_i16((int16_t *)dst,
                                            (int16_t *)src, framesIn);
                                } else {
                                    downmix_to_mono_i16_from_stereo_i16((int16_t *)dst,
                                            (int16_t *)src, framesIn);
                                }
                            }
                        }
                        if (framesOut && mFrameCount == mRsmpInIndex) {
                            void *readInto;
                            if (framesOut == mFrameCount &&
                                (mChannelCount == mReqChannelCount ||
                                        mFormat != AUDIO_FORMAT_PCM_16_BIT)) {
                                readInto = buffer.raw;
                                framesOut = 0;
                            } else {
                                readInto = mRsmpInBuffer;
                                mRsmpInIndex = 0;
                            }
                            mBytesRead = mInput->stream->read(mInput->stream, readInto,
                                    mInputBytes);
                            if (mBytesRead <= 0) {
                                if ((mBytesRead < 0) && (mActiveTrack->mState == TrackBase::ACTIVE))
                                {
                                    ALOGE("Error reading audio input");
                                    // Force input into standby so that it tries to
                                    // recover at next read attempt
                                    inputStandBy();
                                    usleep(kRecordThreadSleepUs);
                                }
                                mRsmpInIndex = mFrameCount;
                                framesOut = 0;
                                buffer.frameCount = 0;
                            }
#ifdef TEE_SINK
                            else if (mTeeSink != 0) {
                                (void) mTeeSink->write(readInto,
                                        mBytesRead >> Format_frameBitShift(mTeeSink->format()));
                            }
#endif
                        }
                    }
                } else {
                    // resampling

                    memset(mRsmpOutBuffer, 0, framesOut * 2 * sizeof(int32_t));
                    // alter output frame count as if we were expecting stereo samples
                    if (mChannelCount == 1 && mReqChannelCount == 1) {
                        framesOut >>= 1;
                    }
                    mResampler->resample(mRsmpOutBuffer, framesOut,
                            this /* AudioBufferProvider* */);
                    // ditherAndClamp() works as long as all buffers returned by
                    // mActiveTrack->getNextBuffer() are 32 bit aligned which should be always true.
                    if (mChannelCount == 2 && mReqChannelCount == 1) {
                        ditherAndClamp(mRsmpOutBuffer, mRsmpOutBuffer, framesOut);
                        // the resampler always outputs stereo samples:
                        // do post stereo to mono conversion
                        downmix_to_mono_i16_from_stereo_i16(buffer.i16, (int16_t *)mRsmpOutBuffer,
                                framesOut);
                    } else {
                        ditherAndClamp((int32_t *)buffer.raw, mRsmpOutBuffer, framesOut);
                    }

                }
                if (mFramestoDrop == 0) {
                    mActiveTrack->releaseBuffer(&buffer);
                } else {
                    if (mFramestoDrop > 0) {
                        mFramestoDrop -= buffer.frameCount;
                        if (mFramestoDrop <= 0) {
                            clearSyncStartEvent();
                        }
                    } else {
                        mFramestoDrop += buffer.frameCount;
                        if (mFramestoDrop >= 0 || mSyncStartEvent == 0 ||
                                mSyncStartEvent->isCancelled()) {
                            ALOGW("Synced record %s, session %d, trigger session %d",
                                  (mFramestoDrop >= 0) ? "timed out" : "cancelled",
                                  mActiveTrack->sessionId(),
                                  (mSyncStartEvent != 0) ? mSyncStartEvent->triggerSession() : 0);
                            clearSyncStartEvent();
                        }
                    }
                }
                mActiveTrack->clearOverflow();
            }
            // client isn't retrieving buffers fast enough
            else {
                if (!mActiveTrack->setOverflow()) {
                    nsecs_t now = systemTime();
                    if ((now - lastWarning) > kWarningThrottleNs) {
                        ALOGW("RecordThread: buffer overflow");
                        lastWarning = now;
                    }
                }
                // Release the processor for a while before asking for a new buffer.
                // This will give the application more chance to read from the buffer and
                // clear the overflow.
                usleep(kRecordThreadSleepUs);
            }
        }
        // enable changes in effect chain
        unlockEffectChains(effectChains);
        effectChains.clear();
    }

    standby();

    {
        Mutex::Autolock _l(mLock);
        mActiveTrack.clear();
        mStartStopCond.broadcast();
    }

    releaseWakeLock();

    ALOGV("RecordThread %p exiting", this);
    return false;
}

void AudioFlinger::RecordThread::standby()
{
    if (!mStandby) {
        inputStandBy();
        mStandby = true;
    }
}

void AudioFlinger::RecordThread::inputStandBy()
{
    mInput->stream->common.standby(&mInput->stream->common);
}

sp<AudioFlinger::RecordThread::RecordTrack>  AudioFlinger::RecordThread::createRecordTrack_l(
        const sp<AudioFlinger::Client>& client,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        int sessionId,
        IAudioFlinger::track_flags_t flags,
        pid_t tid,
        status_t *status)
{
    sp<RecordTrack> track;
    status_t lStatus;

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("Audio driver not initialized.");
        goto Exit;
    }

    // FIXME use flags and tid similar to createTrack_l()

    { // scope for mLock
        Mutex::Autolock _l(mLock);

        track = new RecordTrack(this, client, sampleRate,
                      format, channelMask, frameCount, sessionId);

        if (track->getCblk() == 0) {
            lStatus = NO_MEMORY;
            goto Exit;
        }
        mTracks.add(track);

        // disable AEC and NS if the device is a BT SCO headset supporting those pre processings
        bool suspend = audio_is_bluetooth_sco_device(mInDevice) &&
                        mAudioFlinger->btNrecIsOff();
        setEffectSuspended_l(FX_IID_AEC, suspend, sessionId);
        setEffectSuspended_l(FX_IID_NS, suspend, sessionId);
    }
    lStatus = NO_ERROR;

Exit:
    if (status) {
        *status = lStatus;
    }
    return track;
}

status_t AudioFlinger::RecordThread::start(RecordThread::RecordTrack* recordTrack,
                                           AudioSystem::sync_event_t event,
                                           int triggerSession)
{
    ALOGV("RecordThread::start event %d, triggerSession %d", event, triggerSession);
    sp<ThreadBase> strongMe = this;
    status_t status = NO_ERROR;

    if (event == AudioSystem::SYNC_EVENT_NONE) {
        clearSyncStartEvent();
    } else if (event != AudioSystem::SYNC_EVENT_SAME) {
        mSyncStartEvent = mAudioFlinger->createSyncEvent(event,
                                       triggerSession,
                                       recordTrack->sessionId(),
                                       syncStartEventCallback,
                                       this);
        // Sync event can be cancelled by the trigger session if the track is not in a
        // compatible state in which case we start record immediately
        if (mSyncStartEvent->isCancelled()) {
            clearSyncStartEvent();
        } else {
            // do not wait for the event for more than AudioSystem::kSyncRecordStartTimeOutMs
            mFramestoDrop = - ((AudioSystem::kSyncRecordStartTimeOutMs * mReqSampleRate) / 1000);
        }
    }

    {
        AutoMutex lock(mLock);
        if (mActiveTrack != 0) {
            if (recordTrack != mActiveTrack.get()) {
                status = -EBUSY;
            } else if (mActiveTrack->mState == TrackBase::PAUSING) {
                mActiveTrack->mState = TrackBase::ACTIVE;
            }
            return status;
        }

        recordTrack->mState = TrackBase::IDLE;
        mActiveTrack = recordTrack;
        mLock.unlock();
        status_t status = AudioSystem::startInput(mId);
        mLock.lock();
        if (status != NO_ERROR) {
            mActiveTrack.clear();
            clearSyncStartEvent();
            return status;
        }
        mRsmpInIndex = mFrameCount;
        mBytesRead = 0;
        if (mResampler != NULL) {
            mResampler->reset();
        }
        mActiveTrack->mState = TrackBase::RESUMING;
        // signal thread to start
        ALOGV("Signal record thread");
        mWaitWorkCV.broadcast();
        // do not wait for mStartStopCond if exiting
        if (exitPending()) {
            mActiveTrack.clear();
            status = INVALID_OPERATION;
            goto startError;
        }
        mStartStopCond.wait(mLock);
        if (mActiveTrack == 0) {
            ALOGV("Record failed to start");
            status = BAD_VALUE;
            goto startError;
        }
        ALOGV("Record started OK");
        return status;
    }
startError:
    AudioSystem::stopInput(mId);
    clearSyncStartEvent();
    return status;
}

void AudioFlinger::RecordThread::clearSyncStartEvent()
{
    if (mSyncStartEvent != 0) {
        mSyncStartEvent->cancel();
    }
    mSyncStartEvent.clear();
    mFramestoDrop = 0;
}

void AudioFlinger::RecordThread::syncStartEventCallback(const wp<SyncEvent>& event)
{
    sp<SyncEvent> strongEvent = event.promote();

    if (strongEvent != 0) {
        RecordThread *me = (RecordThread *)strongEvent->cookie();
        me->handleSyncStartEvent(strongEvent);
    }
}

void AudioFlinger::RecordThread::handleSyncStartEvent(const sp<SyncEvent>& event)
{
    if (event == mSyncStartEvent) {
        // TODO: use actual buffer filling status instead of 2 buffers when info is available
        // from audio HAL
        mFramestoDrop = mFrameCount * 2;
    }
}

bool AudioFlinger::RecordThread::stop_l(RecordThread::RecordTrack* recordTrack) {
    ALOGV("RecordThread::stop");
    if (recordTrack != mActiveTrack.get() || recordTrack->mState == TrackBase::PAUSING) {
        return false;
    }
    recordTrack->mState = TrackBase::PAUSING;
    // do not wait for mStartStopCond if exiting
    if (exitPending()) {
        return true;
    }
    mStartStopCond.wait(mLock);
    // if we have been restarted, recordTrack == mActiveTrack.get() here
    if (exitPending() || recordTrack != mActiveTrack.get()) {
        ALOGV("Record stopped OK");
        return true;
    }
    return false;
}

bool AudioFlinger::RecordThread::isValidSyncEvent(const sp<SyncEvent>& event) const
{
    return false;
}

status_t AudioFlinger::RecordThread::setSyncEvent(const sp<SyncEvent>& event)
{
#if 0   // This branch is currently dead code, but is preserved in case it will be needed in future
    if (!isValidSyncEvent(event)) {
        return BAD_VALUE;
    }

    int eventSession = event->triggerSession();
    status_t ret = NAME_NOT_FOUND;

    Mutex::Autolock _l(mLock);

    for (size_t i = 0; i < mTracks.size(); i++) {
        sp<RecordTrack> track = mTracks[i];
        if (eventSession == track->sessionId()) {
            (void) track->setSyncEvent(event);
            ret = NO_ERROR;
        }
    }
    return ret;
#else
    return BAD_VALUE;
#endif
}

// destroyTrack_l() must be called with ThreadBase::mLock held
void AudioFlinger::RecordThread::destroyTrack_l(const sp<RecordTrack>& track)
{
    track->mState = TrackBase::TERMINATED;
    // active tracks are removed by threadLoop()
    if (mActiveTrack != track) {
        removeTrack_l(track);
    }
}

void AudioFlinger::RecordThread::removeTrack_l(const sp<RecordTrack>& track)
{
    mTracks.remove(track);
    // need anything related to effects here?
}

void AudioFlinger::RecordThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    dumpEffectChains(fd, args);
}

void AudioFlinger::RecordThread::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "\nInput thread %p internals\n", this);
    result.append(buffer);

    if (mActiveTrack != 0) {
        snprintf(buffer, SIZE, "In index: %d\n", mRsmpInIndex);
        result.append(buffer);
        snprintf(buffer, SIZE, "In size: %d\n", mInputBytes);
        result.append(buffer);
        snprintf(buffer, SIZE, "Resampling: %d\n", (mResampler != NULL));
        result.append(buffer);
        snprintf(buffer, SIZE, "Out channel count: %u\n", mReqChannelCount);
        result.append(buffer);
        snprintf(buffer, SIZE, "Out sample rate: %u\n", mReqSampleRate);
        result.append(buffer);
    } else {
        result.append("No active record client\n");
    }

    write(fd, result.string(), result.size());

    dumpBase(fd, args);
}

void AudioFlinger::RecordThread::dumpTracks(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "Input thread %p tracks\n", this);
    result.append(buffer);
    RecordTrack::appendDumpHeader(result);
    for (size_t i = 0; i < mTracks.size(); ++i) {
        sp<RecordTrack> track = mTracks[i];
        if (track != 0) {
            track->dump(buffer, SIZE);
            result.append(buffer);
        }
    }

    if (mActiveTrack != 0) {
        snprintf(buffer, SIZE, "\nInput thread %p active tracks\n", this);
        result.append(buffer);
        RecordTrack::appendDumpHeader(result);
        mActiveTrack->dump(buffer, SIZE);
        result.append(buffer);

    }
    write(fd, result.string(), result.size());
}

// AudioBufferProvider interface
status_t AudioFlinger::RecordThread::getNextBuffer(AudioBufferProvider::Buffer* buffer, int64_t pts)
{
    size_t framesReq = buffer->frameCount;
    size_t framesReady = mFrameCount - mRsmpInIndex;
    int channelCount;

    if (framesReady == 0) {
        mBytesRead = mInput->stream->read(mInput->stream, mRsmpInBuffer, mInputBytes);
        if (mBytesRead <= 0) {
            if ((mBytesRead < 0) && (mActiveTrack->mState == TrackBase::ACTIVE)) {
                ALOGE("RecordThread::getNextBuffer() Error reading audio input");
                // Force input into standby so that it tries to
                // recover at next read attempt
                inputStandBy();
                usleep(kRecordThreadSleepUs);
            }
            buffer->raw = NULL;
            buffer->frameCount = 0;
            return NOT_ENOUGH_DATA;
        }
        mRsmpInIndex = 0;
        framesReady = mFrameCount;
    }

    if (framesReq > framesReady) {
        framesReq = framesReady;
    }

    if (mChannelCount == 1 && mReqChannelCount == 2) {
        channelCount = 1;
    } else {
        channelCount = 2;
    }
    buffer->raw = mRsmpInBuffer + mRsmpInIndex * channelCount;
    buffer->frameCount = framesReq;
    return NO_ERROR;
}

// AudioBufferProvider interface
void AudioFlinger::RecordThread::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    mRsmpInIndex += buffer->frameCount;
    buffer->frameCount = 0;
}

bool AudioFlinger::RecordThread::checkForNewParameters_l()
{
    bool reconfig = false;

    while (!mNewParameters.isEmpty()) {
        status_t status = NO_ERROR;
        String8 keyValuePair = mNewParameters[0];
        AudioParameter param = AudioParameter(keyValuePair);
        int value;
        audio_format_t reqFormat = mFormat;
        uint32_t reqSamplingRate = mReqSampleRate;
        uint32_t reqChannelCount = mReqChannelCount;

        if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
            reqSamplingRate = value;
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
            reqFormat = (audio_format_t) value;
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
            reqChannelCount = getInputChannelCount(value);
            reconfig = true;
        }
        if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
            // do not accept frame count changes if tracks are open as the track buffer
            // size depends on frame count and correct behavior would not be guaranteed
            // if frame count is changed after track creation
            if (mActiveTrack != 0) {
                status = INVALID_OPERATION;
            } else {
                reconfig = true;
            }
        }
        if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
            // forward device change to effects that have requested to be
            // aware of attached audio device.
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setDevice_l(value);
            }

            // store input device and output device but do not forward output device to audio HAL.
            // Note that status is ignored by the caller for output device
            // (see AudioFlinger::setParameters()
            if (audio_is_output_devices(value)) {
                mOutDevice = value;
                status = BAD_VALUE;
            } else {
                mInDevice = value;
                // disable AEC and NS if the device is a BT SCO headset supporting those
                // pre processings
                if (mTracks.size() > 0) {
                    bool suspend = audio_is_bluetooth_sco_device(mInDevice) &&
                                        mAudioFlinger->btNrecIsOff();
                    for (size_t i = 0; i < mTracks.size(); i++) {
                        sp<RecordTrack> track = mTracks[i];
                        setEffectSuspended_l(FX_IID_AEC, suspend, track->sessionId());
                        setEffectSuspended_l(FX_IID_NS, suspend, track->sessionId());
                    }
                }
            }
        }
        if (param.getInt(String8(AudioParameter::keyInputSource), value) == NO_ERROR &&
                mAudioSource != (audio_source_t)value) {
            // forward device change to effects that have requested to be
            // aware of attached audio device.
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setAudioSource_l((audio_source_t)value);
            }
            mAudioSource = (audio_source_t)value;
        }
        if (status == NO_ERROR) {
            status = mInput->stream->common.set_parameters(&mInput->stream->common,
                    keyValuePair.string());
            if (status == INVALID_OPERATION) {
                inputStandBy();
                status = mInput->stream->common.set_parameters(&mInput->stream->common,
                        keyValuePair.string());
            }
            if (reconfig) {
                if (status == BAD_VALUE &&
                    reqFormat == mInput->stream->common.get_format(&mInput->stream->common) &&
                    reqFormat == AUDIO_FORMAT_PCM_16_BIT &&
                    (mInput->stream->common.get_sample_rate(&mInput->stream->common)
                            <= (2 * reqSamplingRate)) &&
                    getInputChannelCount(mInput->stream->common.get_channels(&mInput->stream->common))
                            <= FCC_2 &&
                    (reqChannelCount <= FCC_2)) {
                    status = NO_ERROR;
                }
                if (status == NO_ERROR) {
                    readInputParameters();
                    sendIoConfigEvent_l(AudioSystem::INPUT_CONFIG_CHANGED);
                }
            }
        }

        mNewParameters.removeAt(0);

        mParamStatus = status;
        mParamCond.signal();
        // wait for condition with time out in case the thread calling ThreadBase::setParameters()
        // already timed out waiting for the status and will never signal the condition.
        mWaitWorkCV.waitRelative(mLock, kSetParametersTimeoutNs);
    }
    return reconfig;
}

String8 AudioFlinger::RecordThread::getParameters(const String8& keys)
{
    char *s;
    String8 out_s8 = String8();

    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return out_s8;
    }

    s = mInput->stream->common.get_parameters(&mInput->stream->common, keys.string());
    out_s8 = String8(s);
    free(s);
    return out_s8;
}

void AudioFlinger::RecordThread::audioConfigChanged_l(int event, int param) {
    AudioSystem::OutputDescriptor desc;
    void *param2 = NULL;

    switch (event) {
    case AudioSystem::INPUT_OPENED:
    case AudioSystem::INPUT_CONFIG_CHANGED:
        desc.channels = mChannelMask;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mFrameCount;
        desc.latency = 0;
        param2 = &desc;
        break;

    case AudioSystem::INPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged_l(event, mId, param2);
}

void AudioFlinger::RecordThread::readInputParameters()
{
    delete mRsmpInBuffer;
    // mRsmpInBuffer is always assigned a new[] below
    delete mRsmpOutBuffer;
    mRsmpOutBuffer = NULL;
    delete mResampler;
    mResampler = NULL;

    mSampleRate = mInput->stream->common.get_sample_rate(&mInput->stream->common);
    mChannelMask = mInput->stream->common.get_channels(&mInput->stream->common);
    mChannelCount = (uint16_t)getInputChannelCount(mChannelMask);
    mFormat = mInput->stream->common.get_format(&mInput->stream->common);
    mFrameSize = audio_stream_frame_size(&mInput->stream->common);
    mInputBytes = mInput->stream->common.get_buffer_size(&mInput->stream->common);
    mFrameCount = mInputBytes / mFrameSize;
    mNormalFrameCount = mFrameCount; // not used by record, but used by input effects
    mRsmpInBuffer = new int16_t[mFrameCount * mChannelCount];

    if (mSampleRate != mReqSampleRate && mChannelCount <= FCC_2 && mReqChannelCount <= FCC_2)
    {
        int channelCount;
        // optimization: if mono to mono, use the resampler in stereo to stereo mode to avoid
        // stereo to mono post process as the resampler always outputs stereo.
        if (mChannelCount == 1 && mReqChannelCount == 2) {
            channelCount = 1;
        } else {
            channelCount = 2;
        }
        mResampler = AudioResampler::create(16, channelCount, mReqSampleRate);
        mResampler->setSampleRate(mSampleRate);
        mResampler->setVolume(AudioMixer::UNITY_GAIN, AudioMixer::UNITY_GAIN);
        mRsmpOutBuffer = new int32_t[mFrameCount * 2];

        // optmization: if mono to mono, alter input frame count as if we were inputing
        // stereo samples
        if (mChannelCount == 1 && mReqChannelCount == 1) {
            mFrameCount >>= 1;
        }

    }
    mRsmpInIndex = mFrameCount;
}

unsigned int AudioFlinger::RecordThread::getInputFramesLost()
{
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return 0;
    }

    return mInput->stream->get_input_frames_lost(mInput->stream);
}

uint32_t AudioFlinger::RecordThread::hasAudioSession(int sessionId) const
{
    Mutex::Autolock _l(mLock);
    uint32_t result = 0;
    if (getEffectChain_l(sessionId) != 0) {
        result = EFFECT_SESSION;
    }

    for (size_t i = 0; i < mTracks.size(); ++i) {
        if (sessionId == mTracks[i]->sessionId()) {
            result |= TRACK_SESSION;
            break;
        }
    }

    return result;
}

KeyedVector<int, bool> AudioFlinger::RecordThread::sessionIds() const
{
    KeyedVector<int, bool> ids;
    Mutex::Autolock _l(mLock);
    for (size_t j = 0; j < mTracks.size(); ++j) {
        sp<RecordThread::RecordTrack> track = mTracks[j];
        int sessionId = track->sessionId();
        if (ids.indexOfKey(sessionId) < 0) {
            ids.add(sessionId, true);
        }
    }
    return ids;
}

AudioFlinger::AudioStreamIn* AudioFlinger::RecordThread::clearInput()
{
    Mutex::Autolock _l(mLock);
    AudioStreamIn *input = mInput;
    mInput = NULL;
    return input;
}

// this method must always be called either with ThreadBase mLock held or inside the thread loop
audio_stream_t* AudioFlinger::RecordThread::stream() const
{
    if (mInput == NULL) {
        return NULL;
    }
    return &mInput->stream->common;
}

status_t AudioFlinger::RecordThread::addEffectChain_l(const sp<EffectChain>& chain)
{
    // only one chain per input thread
    if (mEffectChains.size() != 0) {
        return INVALID_OPERATION;
    }
    ALOGV("addEffectChain_l() %p on thread %p", chain.get(), this);

    chain->setInBuffer(NULL);
    chain->setOutBuffer(NULL);

    checkSuspendOnAddEffectChain_l(chain);

    mEffectChains.add(chain);

    return NO_ERROR;
}

size_t AudioFlinger::RecordThread::removeEffectChain_l(const sp<EffectChain>& chain)
{
    ALOGV("removeEffectChain_l() %p from thread %p", chain.get(), this);
    ALOGW_IF(mEffectChains.size() != 1,
            "removeEffectChain_l() %p invalid chain size %d on thread %p",
            chain.get(), mEffectChains.size(), this);
    if (mEffectChains.size() == 1) {
        mEffectChains.removeAt(0);
    }
    return 0;
}

// ----------------------------------------------------------------------------
#ifdef QCOM_HARDWARE
AudioFlinger::DirectAudioTrack::DirectAudioTrack(const sp<AudioFlinger>& audioFlinger,
                                                 int output, AudioSessionDescriptor *outputDesc,
                                                 IDirectTrackClient* client, audio_output_flags_t outflag)
    : BnDirectTrack(), mIsPaused(false), mAudioFlinger(audioFlinger), mOutput(output), mOutputDesc(outputDesc),
      mClient(client), mEffectConfigChanged(false), mKillEffectsThread(false), mFlag(outflag),
      mEffectsThreadScratchBuffer(NULL)
{
#ifdef SRS_PROCESSING
    ALOGD("SRS_Processing - DirectAudioTrack - OutNotify_Init: %p TID %d\n", this, gettid());
    POSTPRO_PATCH_ICS_OUTPROC_DIRECT_INIT(this, gettid());
    SRS_Processing::ProcessOutRoute(SRS_Processing::AUTO, this, outputDesc->device);
#endif
    if (mFlag & AUDIO_OUTPUT_FLAG_LPA) {
        ALOGV("create effects thread for LPA");
        createEffectThread();
        allocateBufPool();
#ifdef SRS_PROCESSING
    } else if (mFlag & AUDIO_OUTPUT_FLAG_TUNNEL) {
        ALOGV("create effects thread for TUNNEL");
        createEffectThread();
#endif
    }
    outputDesc->mVolumeScale = 1.0;
    mDeathRecipient = new PMDeathRecipient(this);
    acquireWakeLock();
}

void AudioFlinger::DirectAudioTrack::signalEffect() {
    if (mFlag & AUDIO_OUTPUT_FLAG_LPA){
        mEffectConfigChanged = true;
        mEffectCv.signal();
    }
#ifdef SRS_PROCESSING
    if (mFlag & AUDIO_OUTPUT_FLAG_TUNNEL){
        mEffectConfigChanged = true;
        mEffectCv.signal();
    }
#endif
}

AudioFlinger::DirectAudioTrack::~DirectAudioTrack() {
#ifdef SRS_PROCESSING
    ALOGD("SRS_Processing - DirectAudioTrack - OutNotify_Init: %p TID %d\n", this, gettid());
    POSTPRO_PATCH_ICS_OUTPROC_DIRECT_EXIT(this, gettid());
#endif
    if (mFlag & AUDIO_OUTPUT_FLAG_LPA) {
        requestAndWaitForEffectsThreadExit();
        mAudioFlinger->deleteEffectSession();
        deallocateBufPool();
#ifdef SRS_PROCESSING
    } else if (mFlag & AUDIO_OUTPUT_FLAG_TUNNEL) {
        requestAndWaitForEffectsThreadExit();
        mAudioFlinger->deleteEffectSession();
#endif
    }
    AudioSystem::releaseOutput(mOutput);
    releaseWakeLock();

    {
        Mutex::Autolock _l(pmLock);
        if (mPowerManager != 0) {
            sp<IBinder> binder = mPowerManager->asBinder();
            binder->unlinkToDeath(mDeathRecipient);
        }
    }
}

status_t AudioFlinger::DirectAudioTrack::start() {
    AudioSystem::startOutput(mOutput, (audio_stream_type_t)mOutputDesc->mStreamType);
    if(mIsPaused) {
        mIsPaused = false;
        mOutputDesc->stream->start(mOutputDesc->stream);
    }
    mOutputDesc->mActive = true;
    mOutputDesc->stream->set_volume(mOutputDesc->stream,
                                    mOutputDesc->mVolumeLeft * mOutputDesc->mVolumeScale,
                                    mOutputDesc->mVolumeRight* mOutputDesc->mVolumeScale);
    return NO_ERROR;
}

void AudioFlinger::DirectAudioTrack::stop() {
    ALOGV("DirectAudioTrack::stop");
    mOutputDesc->mActive = false;
    mOutputDesc->stream->stop(mOutputDesc->stream);
    AudioSystem::stopOutput(mOutput, (audio_stream_type_t)mOutputDesc->mStreamType);
}

void AudioFlinger::DirectAudioTrack::pause() {
    if(!mIsPaused) {
        mIsPaused = true;
        mOutputDesc->stream->pause(mOutputDesc->stream);
        mOutputDesc->mActive = false;
        AudioSystem::stopOutput(mOutput, (audio_stream_type_t)mOutputDesc->mStreamType);
    }
}

ssize_t AudioFlinger::DirectAudioTrack::write(const void *buffer, size_t size) {
    ALOGV("Writing to AudioSessionOut");
    int isAvail = 0;
    mOutputDesc->stream->is_buffer_available(mOutputDesc->stream, &isAvail);
    if (!isAvail) {
        return 0;
    }

    if (mFlag & AUDIO_OUTPUT_FLAG_LPA) {
        mEffectLock.lock();
        List<BufferInfo>::iterator it = mEffectsPool.begin();
        BufferInfo buf = *it;
        mEffectsPool.erase(it);
        memcpy((char *) buf.localBuf, (char *)buffer, size);
        buf.bytesToWrite = size;
        mEffectsPool.push_back(buf);
        mAudioFlinger->applyEffectsOn(static_cast<void *>(this),
            (int16_t*)buf.localBuf, (int16_t*)buffer, (int)size, true);
        mEffectLock.unlock();
    }
    ALOGV("out of Writing to AudioSessionOut");
    return mOutputDesc->stream->write(mOutputDesc->stream, buffer, size);
}

void AudioFlinger::DirectAudioTrack::flush() {
    if (mFlag & AUDIO_OUTPUT_FLAG_LPA) {
        mEffectsPool.clear();
        mEffectsPool = mBufPool;
    }
    mOutputDesc->stream->flush(mOutputDesc->stream);
}

void AudioFlinger::DirectAudioTrack::mute(bool muted) {
}

void AudioFlinger::DirectAudioTrack::setVolume(float left, float right) {
    ALOGV("DirectAudioTrack::setVolume left: %f, right: %f", left, right);
    if(mOutputDesc) {
        mOutputDesc->mVolumeLeft = left;
        mOutputDesc->mVolumeRight = right;
        if(mOutputDesc->mActive &&  mOutputDesc->stream) {
            mOutputDesc->stream->set_volume(mOutputDesc->stream,
                                    left * mOutputDesc->mVolumeScale,
                                    right* mOutputDesc->mVolumeScale);
        } else {
            ALOGD("stream is not active, so cache and send when stream is active");
        }
    } else {
        ALOGD("output descriptor is not valid to set the volume");
    }
}

int64_t AudioFlinger::DirectAudioTrack::getTimeStamp() {
    int64_t time;
    mOutputDesc->stream->get_next_write_timestamp(mOutputDesc->stream, &time);
    ALOGV("Timestamp %lld",time);
    return time;
}

void AudioFlinger::DirectAudioTrack::postEOS(int64_t delayUs) {
    if (delayUs == 0 ) {
       ALOGV("Notify Audio Track of EOS event");
       mClient->notify(DIRECT_TRACK_EOS);
    } else {
       ALOGV("Notify Audio Track of hardware failure event");
       mClient->notify(DIRECT_TRACK_HW_FAIL);
    }
}

void AudioFlinger::DirectAudioTrack::allocateBufPool() {
    void *dsp_buf = NULL;
    void *local_buf = NULL;

    //1. get the ion buffer information
    struct buf_info* buf = NULL;
    mOutputDesc->stream->get_buffer_info(mOutputDesc->stream, &buf);
    ALOGV("get buffer info %p",buf);
    if (!buf) {
        ALOGV("buffer is NULL");
        return;
    }
    int nSize = buf->bufsize;
    int bufferCount = buf->nBufs;

    //2. allocate the buffer pool, allocate local buffers
    for (int i = 0; i < bufferCount; i++) {
        dsp_buf = (void *)buf->buffers[i];
        local_buf = malloc(nSize);
        memset(local_buf, 0, nSize);
        // Store this information for internal mapping / maintanence
        BufferInfo buf(local_buf, dsp_buf, nSize);
        buf.bytesToWrite = 0;
        mBufPool.push_back(buf);
        mEffectsPool.push_back(buf);

        ALOGV("The MEM that is allocated buffer is %x, size %d",(unsigned int)dsp_buf,nSize);
    }

    mEffectsThreadScratchBuffer = malloc(nSize);
    ALOGV("effectsThreadScratchBuffer = %x",mEffectsThreadScratchBuffer);

    free(buf);
}

void AudioFlinger::DirectAudioTrack::deallocateBufPool() {

    //1. Deallocate the local memory
    //2. Remove all the buffers from bufpool
    while (!mBufPool.empty())  {
        List<BufferInfo>::iterator it = mBufPool.begin();
        BufferInfo &memBuffer = *it;
        // free the local buffer corresponding to mem buffer
        if (memBuffer.localBuf) {
            free(memBuffer.localBuf);
            memBuffer.localBuf = NULL;
        }
        ALOGV("Removing from bufpool");
        mBufPool.erase(it);
    }

    free(mEffectsThreadScratchBuffer);
    mEffectsThreadScratchBuffer = NULL;
}

status_t AudioFlinger::DirectAudioTrack::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnDirectTrack::onTransact(code, data, reply, flags);
}

AudioFlinger::DirectAudioTrack::AudioFlingerDirectTrackClient::AudioFlingerDirectTrackClient(void *obj)
{
    ALOGV("AudioFlinger::DirectAudioTrack::AudioFlingerDirectTrackClient");
    pBaseClass = (DirectAudioTrack*)obj;
}

void AudioFlinger::DirectAudioTrack::AudioFlingerDirectTrackClient::binderDied(const wp<IBinder>& who) {
    pBaseClass->mAudioFlinger.clear();
    ALOGW("AudioFlinger server died!");
}

void AudioFlinger::DirectAudioTrack::AudioFlingerDirectTrackClient
     ::ioConfigChanged(int event, audio_io_handle_t ioHandle, const void *param2) {
    ALOGV("ioConfigChanged() event %d", event);
    if (event == AudioSystem::EFFECT_CONFIG_CHANGED) {
        ALOGV("Received notification for change in effect module");
        // Seek to current media time - flush the decoded buffers with the driver
        pBaseClass->mEffectConfigChanged = true;
        // Signal effects thread to re-apply effects
        ALOGV("Signalling Effects Thread");
        pBaseClass->mEffectCv.signal();

    }
    ALOGV("ioConfigChanged Out");
}

void AudioFlinger::DirectAudioTrack::acquireWakeLock()
{
    Mutex::Autolock _l(pmLock);

    if (mPowerManager == 0) {
        // use checkService() to avoid blocking if power service is not up yet
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            ALOGW("Thread %s cannot connect to the power manager service", lockName);
        } else {
            mPowerManager = interface_cast<IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
    if (mPowerManager != 0 && mWakeLockToken == 0) {
        sp<IBinder> binder = new BBinder();
        status_t status = mPowerManager->acquireWakeLock(POWERMANAGER_PARTIAL_WAKE_LOCK,
                                                         binder,
                                                         String16(lockName));
        if (status == NO_ERROR) {
            mWakeLockToken = binder;
        }
        ALOGV("acquireWakeLock() status %d", status);
    }
}

void AudioFlinger::DirectAudioTrack::releaseWakeLock()
{
   Mutex::Autolock _l(pmLock);

    if (mWakeLockToken != 0) {
        ALOGV("releaseWakeLock()");
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLock(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
}

void AudioFlinger::DirectAudioTrack::clearPowerManager()
{
    releaseWakeLock();
    Mutex::Autolock _l(pmLock);
    mPowerManager.clear();
}

void AudioFlinger::DirectAudioTrack::PMDeathRecipient::binderDied(const wp<IBinder>& who)
{
    parentClass->clearPowerManager();
    ALOGW("power manager service died !!!");
}
#endif
// ----------------------------------------------------------------------------

}; // namespace android
