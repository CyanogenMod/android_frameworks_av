/*
**
** Copyright 2012, The Android Open Source Project
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

#include "Configuration.h"
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cutils/properties.h>
#include <media/AudioParameter.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include <private/media/AudioTrackShared.h>
#include <hardware/audio.h>
#include <audio_effects/effect_ns.h>
#include <audio_effects/effect_aec.h>
#include <audio_utils/primitives.h>
#include <audio_utils/format.h>
#include <audio_utils/minifloat.h>

// NBAIO implementations
#include <media/nbaio/AudioStreamInSource.h>
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
#include "FastCapture.h"
#include "ServiceUtilities.h"
#include "SchedulingPolicyService.h"

#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif

#ifdef DEBUG_CPU_USAGE
#include <cpustats/CentralTendencyStatistics.h>
#include <cpustats/ThreadCpuUsage.h>
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

// maximum time to wait in sendConfigEvent_l() for a status to be received
static const nsecs_t kConfigEventTimeoutNs = seconds(2);

// minimum sleep time for the mixer thread loop when tracks are active but in underrun
static const uint32_t kMinThreadSleepTimeUs = 5000;
// maximum divider applied to the active sleep time in the mixer thread loop
static const uint32_t kMaxThreadSleepTimeShift = 2;

// minimum normal sink buffer size, expressed in milliseconds rather than frames
static const uint32_t kMinNormalSinkBufferSizeMs = 20;
// maximum normal sink buffer size
static const uint32_t kMaxNormalSinkBufferSizeMs = 24;

// Offloaded output thread standby delay: allows track transition without going to standby
static const nsecs_t kOffloadStandbyDelayNs = seconds(1);

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

// Whether to use fast capture
static const enum {
    FastCapture_Never,  // never initialize or use: for debugging only
    FastCapture_Always, // always initialize and use, even if not needed: for debugging only
    FastCapture_Static, // initialize if needed, then use all the time if initialized
} kUseFastCapture = FastCapture_Static;

// Priorities for requestPriority
static const int kPriorityAudioApp = 2;
static const int kPriorityFastMixer = 3;
static const int kPriorityFastCapture = 3;

// IAudioFlinger::createTrack() reports back to client the total size of shared memory area
// for the track.  The client then sub-divides this into smaller buffers for its use.
// Currently the client uses N-buffering by default, but doesn't tell us about the value of N.
// So for now we just assume that client is double-buffered for fast tracks.
// FIXME It would be better for client to tell AudioFlinger the value of N,
// so AudioFlinger could allocate the right amount of memory.
// See the client's minBufCount and mNotificationFramesAct calculations for details.

// This is the default value, if not specified by property.
static const int kFastTrackMultiplier = 2;

// The minimum and maximum allowed values
static const int kFastTrackMultiplierMin = 1;
static const int kFastTrackMultiplierMax = 2;

// The actual value to use, which can be specified per-device via property af.fast_track_multiplier.
static int sFastTrackMultiplier = kFastTrackMultiplier;

// See Thread::readOnlyHeap().
// Initially this heap is used to allocate client buffers for "fast" AudioRecord.
// Eventually it will be the single buffer that FastCapture writes into via HAL read(),
// and that all "fast" AudioRecord clients read from.  In either case, the size can be small.
static const size_t kRecordThreadReadOnlyHeapSize = 0x1000;

// ----------------------------------------------------------------------------

static pthread_once_t sFastTrackMultiplierOnce = PTHREAD_ONCE_INIT;

static void sFastTrackMultiplierInit()
{
    char value[PROPERTY_VALUE_MAX];
    if (property_get("af.fast_track_multiplier", value, NULL) > 0) {
        char *endptr;
        unsigned long ul = strtoul(value, &endptr, 0);
        if (*endptr == '\0' && kFastTrackMultiplierMin <= ul && ul <= kFastTrackMultiplierMax) {
            sFastTrackMultiplier = (int) ul;
        }
    }
}

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

void CpuStats::sample(const String8 &title
#ifndef DEBUG_CPU_USAGE
                __unused
#endif
        ) {
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
        mAudioFlinger(audioFlinger),
        // mSampleRate, mFrameCount, mChannelMask, mChannelCount, mFrameSize, mFormat, mBufferSize
        // are set by PlaybackThread::readOutputParameters_l() or
        // RecordThread::readInputParameters_l()
        //FIXME: mStandby should be true here. Is this some kind of hack?
        mStandby(false), mOutDevice(outDevice), mInDevice(inDevice),
        mAudioSource(AUDIO_SOURCE_DEFAULT), mId(id),
        // mName will be set by concrete (non-virtual) subclass
        mDeathRecipient(new PMDeathRecipient(this))
{
}

AudioFlinger::ThreadBase::~ThreadBase()
{
    // mConfigEvents should be empty, but just in case it isn't, free the memory it owns
    mConfigEvents.clear();

    // do not lock the mutex in destructor
    releaseWakeLock_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = mPowerManager->asBinder();
        binder->unlinkToDeath(mDeathRecipient);
    }
}

status_t AudioFlinger::ThreadBase::readyToRun()
{
    status_t status = initCheck();
    if (status == NO_ERROR) {
        ALOGI("AudioFlinger's thread %p ready to run", this);
    } else {
        ALOGE("No working audio driver found.");
    }
    return status;
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

    return sendSetParameterConfigEvent_l(keyValuePairs);
}

// sendConfigEvent_l() must be called with ThreadBase::mLock held
// Can temporarily release the lock if waiting for a reply from processConfigEvents_l().
status_t AudioFlinger::ThreadBase::sendConfigEvent_l(sp<ConfigEvent>& event)
{
    status_t status = NO_ERROR;

    mConfigEvents.add(event);
    ALOGV("sendConfigEvent_l() num events %d event %d", mConfigEvents.size(), event->mType);
    mWaitWorkCV.signal();
    mLock.unlock();
    {
        Mutex::Autolock _l(event->mLock);
        while (event->mWaitStatus) {
            if (event->mCond.waitRelative(event->mLock, kConfigEventTimeoutNs) != NO_ERROR) {
                event->mStatus = TIMED_OUT;
                event->mWaitStatus = false;
            }
        }
        status = event->mStatus;
    }
    mLock.lock();
    return status;
}

void AudioFlinger::ThreadBase::sendIoConfigEvent(int event, int param)
{
    Mutex::Autolock _l(mLock);
    sendIoConfigEvent_l(event, param);
}

// sendIoConfigEvent_l() must be called with ThreadBase::mLock held
void AudioFlinger::ThreadBase::sendIoConfigEvent_l(int event, int param)
{
    sp<ConfigEvent> configEvent = (ConfigEvent *)new IoConfigEvent(event, param);
    sendConfigEvent_l(configEvent);
}

// sendPrioConfigEvent_l() must be called with ThreadBase::mLock held
void AudioFlinger::ThreadBase::sendPrioConfigEvent_l(pid_t pid, pid_t tid, int32_t prio)
{
    sp<ConfigEvent> configEvent = (ConfigEvent *)new PrioConfigEvent(pid, tid, prio);
    sendConfigEvent_l(configEvent);
}

// sendSetParameterConfigEvent_l() must be called with ThreadBase::mLock held
status_t AudioFlinger::ThreadBase::sendSetParameterConfigEvent_l(const String8& keyValuePair)
{
    sp<ConfigEvent> configEvent = (ConfigEvent *)new SetParameterConfigEvent(keyValuePair);
    return sendConfigEvent_l(configEvent);
}

status_t AudioFlinger::ThreadBase::sendCreateAudioPatchConfigEvent(
                                                        const struct audio_patch *patch,
                                                        audio_patch_handle_t *handle)
{
    Mutex::Autolock _l(mLock);
    sp<ConfigEvent> configEvent = (ConfigEvent *)new CreateAudioPatchConfigEvent(*patch, *handle);
    status_t status = sendConfigEvent_l(configEvent);
    if (status == NO_ERROR) {
        CreateAudioPatchConfigEventData *data =
                                        (CreateAudioPatchConfigEventData *)configEvent->mData.get();
        *handle = data->mHandle;
    }
    return status;
}

status_t AudioFlinger::ThreadBase::sendReleaseAudioPatchConfigEvent(
                                                                const audio_patch_handle_t handle)
{
    Mutex::Autolock _l(mLock);
    sp<ConfigEvent> configEvent = (ConfigEvent *)new ReleaseAudioPatchConfigEvent(handle);
    return sendConfigEvent_l(configEvent);
}


// post condition: mConfigEvents.isEmpty()
void AudioFlinger::ThreadBase::processConfigEvents_l()
{
    bool configChanged = false;

    while (!mConfigEvents.isEmpty()) {
        ALOGV("processConfigEvents_l() remaining events %d", mConfigEvents.size());
        sp<ConfigEvent> event = mConfigEvents[0];
        mConfigEvents.removeAt(0);
        switch (event->mType) {
        case CFG_EVENT_PRIO: {
            PrioConfigEventData *data = (PrioConfigEventData *)event->mData.get();
            // FIXME Need to understand why this has to be done asynchronously
            int err = requestPriority(data->mPid, data->mTid, data->mPrio,
                    true /*asynchronous*/);
            if (err != 0) {
                ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                      data->mPrio, data->mPid, data->mTid, err);
            }
        } break;
        case CFG_EVENT_IO: {
            IoConfigEventData *data = (IoConfigEventData *)event->mData.get();
            audioConfigChanged(data->mEvent, data->mParam);
        } break;
        case CFG_EVENT_SET_PARAMETER: {
            SetParameterConfigEventData *data = (SetParameterConfigEventData *)event->mData.get();
            if (checkForNewParameter_l(data->mKeyValuePairs, event->mStatus)) {
                configChanged = true;
            }
        } break;
        case CFG_EVENT_CREATE_AUDIO_PATCH: {
            CreateAudioPatchConfigEventData *data =
                                            (CreateAudioPatchConfigEventData *)event->mData.get();
            event->mStatus = createAudioPatch_l(&data->mPatch, &data->mHandle);
        } break;
        case CFG_EVENT_RELEASE_AUDIO_PATCH: {
            ReleaseAudioPatchConfigEventData *data =
                                            (ReleaseAudioPatchConfigEventData *)event->mData.get();
            event->mStatus = releaseAudioPatch_l(data->mHandle);
        } break;
        default:
            ALOG_ASSERT(false, "processConfigEvents_l() unknown event type %d", event->mType);
            break;
        }
        {
            Mutex::Autolock _l(event->mLock);
            if (event->mWaitStatus) {
                event->mWaitStatus = false;
                event->mCond.signal();
            }
        }
        ALOGV_IF(mConfigEvents.isEmpty(), "processConfigEvents_l() DONE thread %p", this);
    }

    if (configChanged) {
        cacheParameters_l();
    }
}

String8 channelMaskToString(audio_channel_mask_t mask, bool output) {
    String8 s;
    if (output) {
        if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT) s.append("front-left, ");
        if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT) s.append("front-right, ");
        if (mask & AUDIO_CHANNEL_OUT_FRONT_CENTER) s.append("front-center, ");
        if (mask & AUDIO_CHANNEL_OUT_LOW_FREQUENCY) s.append("low freq, ");
        if (mask & AUDIO_CHANNEL_OUT_BACK_LEFT) s.append("back-left, ");
        if (mask & AUDIO_CHANNEL_OUT_BACK_RIGHT) s.append("back-right, ");
        if (mask & AUDIO_CHANNEL_OUT_FRONT_LEFT_OF_CENTER) s.append("front-left-of-center, ");
        if (mask & AUDIO_CHANNEL_OUT_FRONT_RIGHT_OF_CENTER) s.append("front-right-of-center, ");
        if (mask & AUDIO_CHANNEL_OUT_BACK_CENTER) s.append("back-center, ");
        if (mask & AUDIO_CHANNEL_OUT_SIDE_LEFT) s.append("side-left, ");
        if (mask & AUDIO_CHANNEL_OUT_SIDE_RIGHT) s.append("side-right, ");
        if (mask & AUDIO_CHANNEL_OUT_TOP_CENTER) s.append("top-center ,");
        if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_LEFT) s.append("top-front-left, ");
        if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_CENTER) s.append("top-front-center, ");
        if (mask & AUDIO_CHANNEL_OUT_TOP_FRONT_RIGHT) s.append("top-front-right, ");
        if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_LEFT) s.append("top-back-left, ");
        if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_CENTER) s.append("top-back-center, " );
        if (mask & AUDIO_CHANNEL_OUT_TOP_BACK_RIGHT) s.append("top-back-right, " );
        if (mask & ~AUDIO_CHANNEL_OUT_ALL) s.append("unknown,  ");
    } else {
        if (mask & AUDIO_CHANNEL_IN_LEFT) s.append("left, ");
        if (mask & AUDIO_CHANNEL_IN_RIGHT) s.append("right, ");
        if (mask & AUDIO_CHANNEL_IN_FRONT) s.append("front, ");
        if (mask & AUDIO_CHANNEL_IN_BACK) s.append("back, ");
        if (mask & AUDIO_CHANNEL_IN_LEFT_PROCESSED) s.append("left-processed, ");
        if (mask & AUDIO_CHANNEL_IN_RIGHT_PROCESSED) s.append("right-processed, ");
        if (mask & AUDIO_CHANNEL_IN_FRONT_PROCESSED) s.append("front-processed, ");
        if (mask & AUDIO_CHANNEL_IN_BACK_PROCESSED) s.append("back-processed, ");
        if (mask & AUDIO_CHANNEL_IN_PRESSURE) s.append("pressure, ");
        if (mask & AUDIO_CHANNEL_IN_X_AXIS) s.append("X, ");
        if (mask & AUDIO_CHANNEL_IN_Y_AXIS) s.append("Y, ");
        if (mask & AUDIO_CHANNEL_IN_Z_AXIS) s.append("Z, ");
        if (mask & AUDIO_CHANNEL_IN_VOICE_UPLINK) s.append("voice-uplink, ");
        if (mask & AUDIO_CHANNEL_IN_VOICE_DNLINK) s.append("voice-dnlink, ");
        if (mask & ~AUDIO_CHANNEL_IN_ALL) s.append("unknown,  ");
    }
    int len = s.length();
    if (s.length() > 2) {
        char *str = s.lockBuffer(len);
        s.unlockBuffer(len - 2);
    }
    return s;
}

void AudioFlinger::ThreadBase::dumpBase(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    bool locked = AudioFlinger::dumpTryLock(mLock);
    if (!locked) {
        dprintf(fd, "thread %p maybe dead locked\n", this);
    }

    dprintf(fd, "  I/O handle: %d\n", mId);
    dprintf(fd, "  TID: %d\n", getTid());
    dprintf(fd, "  Standby: %s\n", mStandby ? "yes" : "no");
    dprintf(fd, "  Sample rate: %u\n", mSampleRate);
    dprintf(fd, "  HAL frame count: %zu\n", mFrameCount);
    dprintf(fd, "  HAL buffer size: %u bytes\n", mBufferSize);
    dprintf(fd, "  Channel Count: %u\n", mChannelCount);
    dprintf(fd, "  Channel Mask: 0x%08x (%s)\n", mChannelMask,
            channelMaskToString(mChannelMask, mType != RECORD).string());
    dprintf(fd, "  Format: 0x%x (%s)\n", mFormat, formatToString(mFormat));
    dprintf(fd, "  Frame size: %zu\n", mFrameSize);
    dprintf(fd, "  Pending config events:");
    size_t numConfig = mConfigEvents.size();
    if (numConfig) {
        for (size_t i = 0; i < numConfig; i++) {
            mConfigEvents[i]->dump(buffer, SIZE);
            dprintf(fd, "\n    %s", buffer);
        }
        dprintf(fd, "\n");
    } else {
        dprintf(fd, " none\n");
    }

    if (locked) {
        mLock.unlock();
    }
}

void AudioFlinger::ThreadBase::dumpEffectChains(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    size_t numEffectChains = mEffectChains.size();
    snprintf(buffer, SIZE, "  %zu Effect Chains\n", numEffectChains);
    write(fd, buffer, strlen(buffer));

    for (size_t i = 0; i < numEffectChains; ++i) {
        sp<EffectChain> chain = mEffectChains[i];
        if (chain != 0) {
            chain->dump(fd, args);
        }
    }
}

void AudioFlinger::ThreadBase::acquireWakeLock(int uid)
{
    Mutex::Autolock _l(mLock);
    acquireWakeLock_l(uid);
}

String16 AudioFlinger::ThreadBase::getWakeLockTag()
{
    switch (mType) {
        case MIXER:
            return String16("AudioMix");
        case DIRECT:
            return String16("AudioDirectOut");
        case DUPLICATING:
            return String16("AudioDup");
        case RECORD:
            return String16("AudioIn");
        case OFFLOAD:
            return String16("AudioOffload");
        default:
            ALOG_ASSERT(false);
            return String16("AudioUnknown");
    }
}

void AudioFlinger::ThreadBase::acquireWakeLock_l(int uid)
{
    getPowerManager_l();
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        status_t status;
        if (uid >= 0) {
            status = mPowerManager->acquireWakeLockWithUid(POWERMANAGER_PARTIAL_WAKE_LOCK,
                    binder,
                    getWakeLockTag(),
                    String16("media"),
                    uid);
        } else {
            status = mPowerManager->acquireWakeLock(POWERMANAGER_PARTIAL_WAKE_LOCK,
                    binder,
                    getWakeLockTag(),
                    String16("media"));
        }
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

void AudioFlinger::ThreadBase::updateWakeLockUids(const SortedVector<int> &uids) {
    Mutex::Autolock _l(mLock);
    updateWakeLockUids_l(uids);
}

void AudioFlinger::ThreadBase::getPowerManager_l() {

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
}

void AudioFlinger::ThreadBase::updateWakeLockUids_l(const SortedVector<int> &uids) {

    getPowerManager_l();
    if (mWakeLockToken == NULL) {
        ALOGE("no wake lock to update!");
        return;
    }
    if (mPowerManager != 0) {
        sp<IBinder> binder = new BBinder();
        status_t status;
        status = mPowerManager->updateWakeLockUids(mWakeLockToken, uids.size(), uids.array());
        ALOGV("acquireWakeLock_l() %s status %d", mName, status);
    }
}

void AudioFlinger::ThreadBase::clearPowerManager()
{
    Mutex::Autolock _l(mLock);
    releaseWakeLock_l();
    mPowerManager.clear();
}

void AudioFlinger::ThreadBase::PMDeathRecipient::binderDied(const wp<IBinder>& who __unused)
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
        status_t *status)
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

    // Reject any effect on Direct output threads for now, since the format of
    // mSinkBuffer is not guaranteed to be compatible with effect processing (PCM 16 stereo).
    if (mType == DIRECT) {
        ALOGW("createEffect_l() Cannot add effect %s on Direct output type thread %s",
                desc->name, mName);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    // Allow global effects only on offloaded and mixer threads
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
        switch (mType) {
        case MIXER:
        case OFFLOAD:
            break;
        case DIRECT:
        case DUPLICATING:
        case RECORD:
        default:
            ALOGW("createEffect_l() Cannot add global effect %s on thread %s", desc->name, mName);
            lStatus = BAD_VALUE;
            goto Exit;
        }
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
            effect->setOffloaded(mType == OFFLOAD, mId);

            lStatus = chain->addEffect_l(effect);
            if (lStatus != NO_ERROR) {
                goto Exit;
            }
            effectCreated = true;

            effect->setDevice(mOutDevice);
            effect->setDevice(mInDevice);
            effect->setMode(mAudioFlinger->getMode());
            effect->setAudioSource(mAudioSource);
        }
        // create effect handle and connect it to effect module
        handle = new EffectHandle(effect, client, effectClient, priority);
        lStatus = handle->initCheck();
        if (lStatus == OK) {
            lStatus = effect->addHandle(handle.get());
        }
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

    *status = lStatus;
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

    ALOGD_IF((mType == OFFLOAD) && !effect->isOffloadable(),
             "addEffect_l() on offloaded thread %p: effect %s does not support offload flags %x",
                    this, effect->desc().name, effect->desc().flags);

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

    effect->setOffloaded(mType == OFFLOAD, mId);

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
        mEffectChains[i]->lock();
    }
}

void AudioFlinger::ThreadBase::unlockEffectChains(
        const Vector< sp<AudioFlinger::EffectChain> >& effectChains)
{
    for (size_t i = 0; i < effectChains.size(); i++) {
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
        mNormalFrameCount(0), mSinkBuffer(NULL),
        mMixerBufferEnabled(AudioFlinger::kEnableExtendedPrecision),
        mMixerBuffer(NULL),
        mMixerBufferSize(0),
        mMixerBufferFormat(AUDIO_FORMAT_INVALID),
        mMixerBufferValid(false),
        mEffectBufferEnabled(AudioFlinger::kEnableExtendedPrecision),
        mEffectBuffer(NULL),
        mEffectBufferSize(0),
        mEffectBufferFormat(AUDIO_FORMAT_INVALID),
        mEffectBufferValid(false),
        mSuspended(0), mBytesWritten(0),
        mActiveTracksGeneration(0),
        // mStreamTypes[] initialized in constructor body
        mOutput(output),
        mLastWriteTime(0), mNumWrites(0), mNumDelayedWrites(0), mInWrite(false),
        mMixerStatus(MIXER_IDLE),
        mMixerStatusIgnoringFastTracks(MIXER_IDLE),
        standbyDelay(AudioFlinger::mStandbyTimeInNsecs),
        mBytesRemaining(0),
        mCurrentWriteLength(0),
        mUseAsyncWrite(false),
        mWriteAckSequence(0),
        mDrainSequence(0),
        mSignalPending(false),
        mScreenState(AudioFlinger::mScreenState),
        // index 0 is reserved for normal mixer's submix
        mFastTrackAvailMask(((1 << FastMixerState::kMaxFastTracks) - 1) & ~1),
        // mLatchD, mLatchQ,
        mLatchDValid(false), mLatchQValid(false)
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

    readOutputParameters_l();

    // mStreamTypes[AUDIO_STREAM_CNT] is initialized by stream_type_t default constructor
    // There is no AUDIO_STREAM_MIN, and ++ operator does not compile
    for (audio_stream_type_t stream = AUDIO_STREAM_MIN; stream < AUDIO_STREAM_CNT;
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
    free(mSinkBuffer);
    free(mMixerBuffer);
    free(mEffectBuffer);
}

void AudioFlinger::PlaybackThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    dumpEffectChains(fd, args);
}

void AudioFlinger::PlaybackThread::dumpTracks(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.appendFormat("  Stream volumes in dB: ");
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

    // These values are "raw"; they will wrap around.  See prepareTracks_l() for a better way.
    FastTrackUnderruns underruns = getFastTrackUnderruns(0);
    dprintf(fd, "  Normal mixer raw underrun counters: partial=%u empty=%u\n",
            underruns.mBitFields.mPartial, underruns.mBitFields.mEmpty);

    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    dprintf(fd, "  %d Tracks", numtracks);
    size_t numactiveseen = 0;
    if (numtracks) {
        dprintf(fd, " of which %d are active\n", numactive);
        Track::appendDumpHeader(result);
        for (size_t i = 0; i < numtracks; ++i) {
            sp<Track> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                track->dump(buffer, SIZE, active);
                result.append(buffer);
            }
        }
    } else {
        result.append("\n");
    }
    if (numactiveseen != numactive) {
        // some tracks in the active list were not in the tracks list
        snprintf(buffer, SIZE, "  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(buffer);
        Track::appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<Track> track = mActiveTracks[i].promote();
            if (track != 0 && mTracks.indexOf(track) < 0) {
                track->dump(buffer, SIZE, true);
                result.append(buffer);
            }
        }
    }

    write(fd, result.string(), result.size());
}

void AudioFlinger::PlaybackThread::dumpInternals(int fd, const Vector<String16>& args)
{
    dprintf(fd, "\nOutput thread %p:\n", this);
    dprintf(fd, "  Normal frame count: %zu\n", mNormalFrameCount);
    dprintf(fd, "  Last write occurred (msecs): %llu\n", ns2ms(systemTime() - mLastWriteTime));
    dprintf(fd, "  Total writes: %d\n", mNumWrites);
    dprintf(fd, "  Delayed writes: %d\n", mNumDelayedWrites);
    dprintf(fd, "  Blocked in write: %s\n", mInWrite ? "yes" : "no");
    dprintf(fd, "  Suspend count: %d\n", mSuspended);
    dprintf(fd, "  Sink buffer : %p\n", mSinkBuffer);
    dprintf(fd, "  Mixer buffer: %p\n", mMixerBuffer);
    dprintf(fd, "  Effect buffer: %p\n", mEffectBuffer);
    dprintf(fd, "  Fast track availMask=%#x\n", mFastTrackAvailMask);

    dumpBase(fd, args);
}

// Thread virtuals

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
        size_t *pFrameCount,
        const sp<IMemory>& sharedBuffer,
        int sessionId,
        IAudioFlinger::track_flags_t *flags,
        pid_t tid,
        int uid,
        status_t *status)
{
    size_t frameCount = *pFrameCount;
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
                (frameCount >= mFrameCount))
              )
            ) &&
            // PCM data
            audio_is_linear_pcm(format) &&
            // mono or stereo
            ( (channelMask == AUDIO_CHANNEL_OUT_MONO) ||
              (channelMask == AUDIO_CHANNEL_OUT_STEREO) ) &&
            // hardware sample rate
            (sampleRate == mSampleRate) &&
            // normal mixer has an associated fast mixer
            hasFastMixer() &&
            // there are sufficient fast track slots available
            (mFastTrackAvailMask != 0)
            // FIXME test that MixerThread for this fast track has a capable output HAL
            // FIXME add a permission test also?
        ) {
        // if frameCount not specified, then it defaults to fast mixer (HAL) frame count
        if (frameCount == 0) {
            // read the fast track multiplier property the first time it is needed
            int ok = pthread_once(&sFastTrackMultiplierOnce, sFastTrackMultiplierInit);
            if (ok != 0) {
                ALOGE("%s pthread_once failed: %d", __func__, ok);
            }
            frameCount = mFrameCount * sFastTrackMultiplier;
        }
        ALOGV("AUDIO_OUTPUT_FLAG_FAST accepted: frameCount=%d mFrameCount=%d",
                frameCount, mFrameCount);
      } else {
        ALOGV("AUDIO_OUTPUT_FLAG_FAST denied: isTimed=%d sharedBuffer=%p frameCount=%d "
                "mFrameCount=%d format=%#x mFormat=%#x isLinear=%d channelMask=%#x "
                "sampleRate=%u mSampleRate=%u "
                "hasFastMixer=%d tid=%d fastTrackAvailMask=%#x",
                isTimed, sharedBuffer.get(), frameCount, mFrameCount, format, mFormat,
                audio_is_linear_pcm(format),
                channelMask, sampleRate, mSampleRate, hasFastMixer(), tid, mFastTrackAvailMask);
        *flags &= ~IAudioFlinger::TRACK_FAST;
        // For compatibility with AudioTrack calculation, buffer depth is forced
        // to be at least 2 x the normal mixer frame count and cover audio hardware latency.
        // This is probably too conservative, but legacy application code may depend on it.
        // If you change this calculation, also review the start threshold which is related.
        uint32_t latencyMs = mOutput->stream->get_latency(mOutput->stream);
        uint32_t minBufCount = latencyMs / ((1000 * mNormalFrameCount) / mSampleRate);
        if (minBufCount < 2) {
            minBufCount = 2;
        }
        size_t minFrameCount = mNormalFrameCount * minBufCount;
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
      }
    }
    *pFrameCount = frameCount;

    switch (mType) {

    case DIRECT:
        if (audio_is_linear_pcm(format)) {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("createTrack_l() Bad parameter: sampleRate %u format %#x, channelMask 0x%08x "
                        "for output %p with format %#x",
                        sampleRate, format, channelMask, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
        break;

    case OFFLOAD:
        if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
            ALOGE("createTrack_l() Bad parameter: sampleRate %d format %#x, channelMask 0x%08x \""
                    "for output %p with format %#x",
                    sampleRate, format, channelMask, mOutput, mFormat);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        break;

    default:
        if (!audio_is_linear_pcm(format)) {
                ALOGE("createTrack_l() Bad parameter: format %#x \""
                        "for output %p with format %#x",
                        format, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
        }
        // Resampler implementation limits input sampling rate to 2 x output sampling rate.
        if (sampleRate > mSampleRate*2) {
            ALOGE("Sample rate out of range: %u mSampleRate %u", sampleRate, mSampleRate);
            lStatus = BAD_VALUE;
            goto Exit;
        }
        break;

    }

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createTrack_l() audio driver not initialized");
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
                    channelMask, frameCount, sharedBuffer, sessionId, uid, *flags);
        } else {
            track = TimedTrack::create(this, client, streamType, sampleRate, format,
                    channelMask, frameCount, sharedBuffer, sessionId, uid);
        }

        // new Track always returns non-NULL,
        // but TimedTrack::create() is a factory that could fail by returning NULL
        lStatus = track != 0 ? track->initCheck() : (status_t) NO_MEMORY;
        if (lStatus != NO_ERROR) {
            ALOGE("createTrack_l() initCheck failed %d; no control block?", lStatus);
            // track must be cleared from the caller as the caller has the AF lock
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
    *status = lStatus;
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
    broadcast_l();
}

void AudioFlinger::PlaybackThread::setStreamMute(audio_stream_type_t stream, bool muted)
{
    Mutex::Autolock _l(mLock);
    mStreamTypes[stream].mute = muted;
    broadcast_l();
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
        if (!track->isOutputTrack()) {
            TrackBase::track_state state = track->mState;
            mLock.unlock();
            status = AudioSystem::startOutput(mId, track->streamType(), track->sessionId());
            mLock.lock();
            // abort track was stopped/paused while we released the lock
            if (state != track->mState) {
                if (status == NO_ERROR) {
                    mLock.unlock();
                    AudioSystem::stopOutput(mId, track->streamType(), track->sessionId());
                    mLock.lock();
                }
                return INVALID_OPERATION;
            }
            // abort if start is rejected by audio policy manager
            if (status != NO_ERROR) {
                return PERMISSION_DENIED;
            }
#ifdef ADD_BATTERY_DATA
            // to track the speaker usage
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
#endif
        }

        track->mFillingUpStatus = track->sharedBuffer() != 0 ? Track::FS_FILLED : Track::FS_FILLING;
        track->mResetDone = false;
        track->mPresentationCompleteFrames = 0;
        mActiveTracks.add(track);
        mWakeLockUids.add(track->uid());
        mActiveTracksGeneration++;
        mLatestActiveTrack = track;
        sp<EffectChain> chain = getEffectChain_l(track->sessionId());
        if (chain != 0) {
            ALOGV("addTrack_l() starting track on chain %p for session %d", chain.get(),
                    track->sessionId());
            chain->incActiveTrackCnt();
        }

        status = NO_ERROR;
    }

    onAddNewTrack_l();
    return status;
}

bool AudioFlinger::PlaybackThread::destroyTrack_l(const sp<Track>& track)
{
    track->terminate();
    // active tracks are removed by threadLoop()
    bool trackActive = (mActiveTracks.indexOf(track) >= 0);
    track->mState = TrackBase::STOPPED;
    if (!trackActive) {
        removeTrack_l(track);
    } else if (track->isFastTrack() || track->isOffloaded() || track->isDirect()) {
        track->mState = TrackBase::STOPPING_1;
    }

    return trackActive;
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

void AudioFlinger::PlaybackThread::broadcast_l()
{
    // Thread could be blocked waiting for async
    // so signal it to handle state changes immediately
    // If threadLoop is currently unlocked a signal of mWaitWorkCV will
    // be lost so we also flag to prevent it blocking on mWaitWorkCV
    mSignalPending = true;
    mWaitWorkCV.broadcast();
}

String8 AudioFlinger::PlaybackThread::getParameters(const String8& keys)
{
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return String8();
    }

    char *s = mOutput->stream->common.get_parameters(&mOutput->stream->common, keys.string());
    const String8 out_s8(s);
    free(s);
    return out_s8;
}

void AudioFlinger::PlaybackThread::audioConfigChanged(int event, int param) {
    AudioSystem::OutputDescriptor desc;
    void *param2 = NULL;

    ALOGV("PlaybackThread::audioConfigChanged, thread %p, event %d, param %d", this, event,
            param);

    switch (event) {
    case AudioSystem::OUTPUT_OPENED:
    case AudioSystem::OUTPUT_CONFIG_CHANGED:
        desc.channelMask = mChannelMask;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mNormalFrameCount; // FIXME see
                                             // AudioFlinger::frameCount(audio_io_handle_t)
        desc.latency = latency_l();
        param2 = &desc;
        break;

    case AudioSystem::STREAM_CONFIG_CHANGED:
        param2 = &param;
    case AudioSystem::OUTPUT_CLOSED:
    default:
        break;
    }
    mAudioFlinger->audioConfigChanged(event, mId, param2);
}

void AudioFlinger::PlaybackThread::writeCallback()
{
    ALOG_ASSERT(mCallbackThread != 0);
    mCallbackThread->resetWriteBlocked();
}

void AudioFlinger::PlaybackThread::drainCallback()
{
    ALOG_ASSERT(mCallbackThread != 0);
    mCallbackThread->resetDraining();
}

void AudioFlinger::PlaybackThread::resetWriteBlocked(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    // reject out of sequence requests
    if ((mWriteAckSequence & 1) && (sequence == mWriteAckSequence)) {
        mWriteAckSequence &= ~1;
        mWaitWorkCV.signal();
    }
}

void AudioFlinger::PlaybackThread::resetDraining(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    // reject out of sequence requests
    if ((mDrainSequence & 1) && (sequence == mDrainSequence)) {
        mDrainSequence &= ~1;
        mWaitWorkCV.signal();
    }
}

// static
int AudioFlinger::PlaybackThread::asyncCallback(stream_callback_event_t event,
                                                void *param __unused,
                                                void *cookie)
{
    AudioFlinger::PlaybackThread *me = (AudioFlinger::PlaybackThread *)cookie;
    ALOGV("asyncCallback() event %d", event);
    switch (event) {
    case STREAM_CBK_EVENT_WRITE_READY:
        me->writeCallback();
        break;
    case STREAM_CBK_EVENT_DRAIN_READY:
        me->drainCallback();
        break;
    default:
        ALOGW("asyncCallback() unknown event %d", event);
        break;
    }
    return 0;
}

void AudioFlinger::PlaybackThread::readOutputParameters_l()
{
    // unfortunately we have no way of recovering from errors here, hence the LOG_ALWAYS_FATAL
    mSampleRate = mOutput->stream->common.get_sample_rate(&mOutput->stream->common);
    mChannelMask = mOutput->stream->common.get_channels(&mOutput->stream->common);
    if (!audio_is_output_channel(mChannelMask)) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x not valid for output", mChannelMask);
    }
    if ((mType == MIXER || mType == DUPLICATING) && mChannelMask != AUDIO_CHANNEL_OUT_STEREO) {
        LOG_ALWAYS_FATAL("HAL channel mask %#x not supported for mixed output; "
                "must be AUDIO_CHANNEL_OUT_STEREO", mChannelMask);
    }
    mChannelCount = audio_channel_count_from_out_mask(mChannelMask);
    mFormat = mOutput->stream->common.get_format(&mOutput->stream->common);
    if (!audio_is_valid_format(mFormat)) {
        LOG_ALWAYS_FATAL("HAL format %#x not valid for output", mFormat);
    }
    if ((mType == MIXER || mType == DUPLICATING)
            && !isValidPcmSinkFormat(mFormat)) {
        LOG_FATAL("HAL format %#x not supported for mixed output",
                mFormat);
    }
    mFrameSize = audio_stream_frame_size(&mOutput->stream->common);
    mBufferSize = mOutput->stream->common.get_buffer_size(&mOutput->stream->common);
    mFrameCount = mBufferSize / mFrameSize;
    if (mFrameCount & 15) {
        ALOGW("HAL output buffer size is %u frames but AudioMixer requires multiples of 16 frames",
                mFrameCount);
    }

    if ((mOutput->flags & AUDIO_OUTPUT_FLAG_NON_BLOCKING) &&
            (mOutput->stream->set_callback != NULL)) {
        if (mOutput->stream->set_callback(mOutput->stream,
                                      AudioFlinger::PlaybackThread::asyncCallback, this) == 0) {
            mUseAsyncWrite = true;
            mCallbackThread = new AudioFlinger::AsyncCallbackThread(this);
        }
    }

    // Calculate size of normal sink buffer relative to the HAL output buffer size
    double multiplier = 1.0;
    if (mType == MIXER && (kUseFastMixer == FastMixer_Static ||
            kUseFastMixer == FastMixer_Dynamic)) {
        size_t minNormalFrameCount = (kMinNormalSinkBufferSizeMs * mSampleRate) / 1000;
        size_t maxNormalFrameCount = (kMaxNormalSinkBufferSizeMs * mSampleRate) / 1000;
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
            // SRC (it would be unusual for the normal sink buffer size to not be a multiple of fast
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
    if (mType == MIXER || mType == DUPLICATING) {
        mNormalFrameCount = (mNormalFrameCount + 15) & ~15;
    }
    ALOGI("HAL output buffer size %u frames, normal sink buffer size %u frames", mFrameCount,
            mNormalFrameCount);

    // mSinkBuffer is the sink buffer.  Size is always multiple-of-16 frames.
    // Originally this was int16_t[] array, need to remove legacy implications.
    free(mSinkBuffer);
    mSinkBuffer = NULL;
    // For sink buffer size, we use the frame size from the downstream sink to avoid problems
    // with non PCM formats for compressed music, e.g. AAC, and Offload threads.
    const size_t sinkBufferSize = mNormalFrameCount * mFrameSize;
    (void)posix_memalign(&mSinkBuffer, 32, sinkBufferSize);

    // We resize the mMixerBuffer according to the requirements of the sink buffer which
    // drives the output.
    free(mMixerBuffer);
    mMixerBuffer = NULL;
    if (mMixerBufferEnabled) {
        mMixerBufferFormat = AUDIO_FORMAT_PCM_FLOAT; // also valid: AUDIO_FORMAT_PCM_16_BIT.
        mMixerBufferSize = mNormalFrameCount * mChannelCount
                * audio_bytes_per_sample(mMixerBufferFormat);
        (void)posix_memalign(&mMixerBuffer, 32, mMixerBufferSize);
    }
    free(mEffectBuffer);
    mEffectBuffer = NULL;
    if (mEffectBufferEnabled) {
        mEffectBufferFormat = AUDIO_FORMAT_PCM_16_BIT; // Note: Effects support 16b only
        mEffectBufferSize = mNormalFrameCount * mChannelCount
                * audio_bytes_per_sample(mEffectBufferFormat);
        (void)posix_memalign(&mEffectBuffer, 32, mEffectBufferSize);
    }

    // force reconfiguration of effect chains and engines to take new buffer size and audio
    // parameters into account
    // Note that mLock is not held when readOutputParameters_l() is called from the constructor
    // but in this case nothing is done below as no audio sessions have effect yet so it doesn't
    // matter.
    // create a copy of mEffectChains as calling moveEffectChain_l() can reorder some effect chains
    Vector< sp<EffectChain> > effectChains = mEffectChains;
    for (size_t i = 0; i < effectChains.size(); i ++) {
        mAudioFlinger->moveEffectChain_l(effectChains[i]->sessionId(), this, this, false);
    }
}


status_t AudioFlinger::PlaybackThread::getRenderPosition(uint32_t *halFrames, uint32_t *dspFrames)
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
        status_t status;
        uint32_t frames;
        status = mOutput->stream->get_render_position(mOutput->stream, &frames);
        *dspFrames = (size_t)frames;
        return status;
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
    if (count > 0) {
        for (size_t i = 0 ; i < count ; i++) {
            const sp<Track>& track = tracksToRemove.itemAt(i);
            if (!track->isOutputTrack()) {
                AudioSystem::stopOutput(mId, track->streamType(), track->sessionId());
#ifdef ADD_BATTERY_DATA
                // to track the speaker usage
                addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
#endif
                if (track->isTerminated()) {
                    AudioSystem::releaseOutput(mId);
                }
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
ssize_t AudioFlinger::PlaybackThread::threadLoop_write()
{
    // FIXME rewrite to reduce number of system calls
    mLastWriteTime = systemTime();
    mInWrite = true;
    ssize_t bytesWritten;
    const size_t offset = mCurrentWriteLength - mBytesRemaining;

    // If an NBAIO sink is present, use it to write the normal mixer's submix
    if (mNormalSink != 0) {
        const size_t count = mBytesRemaining / mFrameSize;

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
        ssize_t framesWritten = mNormalSink->write((char *)mSinkBuffer + offset, count);
        ATRACE_END();
        if (framesWritten > 0) {
            bytesWritten = framesWritten * mFrameSize;
        } else {
            bytesWritten = framesWritten;
        }
        status_t status = mNormalSink->getTimestamp(mLatchD.mTimestamp);
        if (status == NO_ERROR) {
            size_t totalFramesWritten = mNormalSink->framesWritten();
            if (totalFramesWritten >= mLatchD.mTimestamp.mPosition) {
                mLatchD.mUnpresentedFrames = totalFramesWritten - mLatchD.mTimestamp.mPosition;
                mLatchDValid = true;
            }
        }
    // otherwise use the HAL / AudioStreamOut directly
    } else {
        // Direct output and offload threads

        if (mUseAsyncWrite) {
            ALOGW_IF(mWriteAckSequence & 1, "threadLoop_write(): out of sequence write request");
            mWriteAckSequence += 2;
            mWriteAckSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
        // FIXME We should have an implementation of timestamps for direct output threads.
        // They are used e.g for multichannel PCM playback over HDMI.
        bytesWritten = mOutput->stream->write(mOutput->stream,
                                                   (char *)mSinkBuffer + offset, mBytesRemaining);
        if (mUseAsyncWrite &&
                ((bytesWritten < 0) || (bytesWritten == (ssize_t)mBytesRemaining))) {
            // do not wait for async callback in case of error of full write
            mWriteAckSequence &= ~1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setWriteBlocked(mWriteAckSequence);
        }
    }

    mNumWrites++;
    mInWrite = false;
    mStandby = false;
    return bytesWritten;
}

void AudioFlinger::PlaybackThread::threadLoop_drain()
{
    if (mOutput->stream->drain) {
        ALOGV("draining %s", (mMixerStatus == MIXER_DRAIN_TRACK) ? "early" : "full");
        if (mUseAsyncWrite) {
            ALOGW_IF(mDrainSequence & 1, "threadLoop_drain(): out of sequence drain request");
            mDrainSequence |= 1;
            ALOG_ASSERT(mCallbackThread != 0);
            mCallbackThread->setDraining(mDrainSequence);
        }
        mOutput->stream->drain(mOutput->stream,
            (mMixerStatus == MIXER_DRAIN_TRACK) ? AUDIO_DRAIN_EARLY_NOTIFY
                                                : AUDIO_DRAIN_ALL);
    }
}

void AudioFlinger::PlaybackThread::threadLoop_exit()
{
    // Default implementation has nothing to do
}

/*
The derived values that are cached:
 - mSinkBufferSize from frame count * frame size
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
    mSinkBufferSize = mNormalFrameCount * mFrameSize;
    activeSleepTime = activeSleepTimeUs();
    idleSleepTime = idleSleepTimeUs();
}

void AudioFlinger::PlaybackThread::invalidateTracks(audio_stream_type_t streamType)
{
    ALOGV("MixerThread::invalidateTracks() mixer %p, streamType %d, mTracks.size %d",
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
    int16_t* buffer = reinterpret_cast<int16_t*>(mEffectBufferEnabled
            ? mEffectBuffer : mSinkBuffer);
    bool ownsBuffer = false;

    ALOGV("addEffectChain_l() %p on thread %p for session %d", chain.get(), this, session);
    if (session > 0) {
        // Only one effect chain can be present in direct output thread and it uses
        // the sink buffer as input
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
    chain->setOutBuffer(reinterpret_cast<int16_t*>(mEffectBufferEnabled
            ? mEffectBuffer : mSinkBuffer));
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
                    track->setMainBuffer(reinterpret_cast<int16_t*>(mSinkBuffer));
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

    // MIXER
    nsecs_t lastWarning = 0;

    // DUPLICATING
    // FIXME could this be made local to while loop?
    writeFrames = 0;

    int lastGeneration = 0;

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

    checkSilentMode_l();

    while (!exitPending())
    {
        cpuStats.sample(myName);

        Vector< sp<EffectChain> > effectChains;

        { // scope for mLock

            Mutex::Autolock _l(mLock);

            processConfigEvents_l();

            if (logString != NULL) {
                mNBLogWriter->logTimestamp();
                mNBLogWriter->log(logString);
                logString = NULL;
            }

            if (mLatchDValid) {
                mLatchQ = mLatchD;
                mLatchDValid = false;
                mLatchQValid = true;
            }

            saveOutputTracks();
            if (mSignalPending) {
                // A signal was raised while we were unlocked
                mSignalPending = false;
            } else if (waitingAsyncCallback_l()) {
                if (exitPending()) {
                    break;
                }
                releaseWakeLock_l();
                mWakeLockUids.clear();
                mActiveTracksGeneration++;
                ALOGV("wait async completion");
                mWaitWorkCV.wait(mLock);
                ALOGV("async completion/wake");
                acquireWakeLock_l();
                standbyTime = systemTime() + standbyDelay;
                sleepTime = 0;

                continue;
            }
            if ((!mActiveTracks.size() && systemTime() > standbyTime) ||
                                   isSuspended()) {
                // put audio hardware into standby after short delay
                if (shouldStandby_l()) {

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
                    mWakeLockUids.clear();
                    mActiveTracksGeneration++;
                    // wait until we have something to do...
                    ALOGV("%s going to sleep", myName.string());
                    mWaitWorkCV.wait(mLock);
                    ALOGV("%s waking up", myName.string());
                    acquireWakeLock_l();

                    mMixerStatus = MIXER_IDLE;
                    mMixerStatusIgnoringFastTracks = MIXER_IDLE;
                    mBytesWritten = 0;
                    mBytesRemaining = 0;
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

            // compare with previously applied list
            if (lastGeneration != mActiveTracksGeneration) {
                // update wakelock
                updateWakeLockUids_l(mWakeLockUids);
                lastGeneration = mActiveTracksGeneration;
            }

            // prevent any changes in effect chain list and in each effect chain
            // during mixing and effect process as the audio buffers could be deleted
            // or modified if an effect is created or deleted
            lockEffectChains_l(effectChains);
        } // mLock scope ends

        if (mBytesRemaining == 0) {
            mCurrentWriteLength = 0;
            if (mMixerStatus == MIXER_TRACKS_READY) {
                // threadLoop_mix() sets mCurrentWriteLength
                threadLoop_mix();
            } else if ((mMixerStatus != MIXER_DRAIN_TRACK)
                        && (mMixerStatus != MIXER_DRAIN_ALL)) {
                // threadLoop_sleepTime sets sleepTime to 0 if data
                // must be written to HAL
                threadLoop_sleepTime();
                if (sleepTime == 0) {
                    mCurrentWriteLength = mSinkBufferSize;
                }
            }
            // Either threadLoop_mix() or threadLoop_sleepTime() should have set
            // mMixerBuffer with data if mMixerBufferValid is true and sleepTime == 0.
            // Merge mMixerBuffer data into mEffectBuffer (if any effects are valid)
            // or mSinkBuffer (if there are no effects).
            //
            // This is done pre-effects computation; if effects change to
            // support higher precision, this needs to move.
            //
            // mMixerBufferValid is only set true by MixerThread::prepareTracks_l().
            // TODO use sleepTime == 0 as an additional condition.
            if (mMixerBufferValid) {
                void *buffer = mEffectBufferValid ? mEffectBuffer : mSinkBuffer;
                audio_format_t format = mEffectBufferValid ? mEffectBufferFormat : mFormat;

                memcpy_by_audio_format(buffer, format, mMixerBuffer, mMixerBufferFormat,
                        mNormalFrameCount * mChannelCount);
            }

            mBytesRemaining = mCurrentWriteLength;
            if (isSuspended()) {
                sleepTime = suspendSleepTimeUs();
                // simulate write to HAL when suspended
                mBytesWritten += mSinkBufferSize;
                mBytesRemaining = 0;
            }

            // only process effects if we're going to write
            if (sleepTime == 0 && mType != OFFLOAD) {
                for (size_t i = 0; i < effectChains.size(); i ++) {
                    effectChains[i]->process_l();
                }
            }
        }
        // Process effect chains for offloaded thread even if no audio
        // was read from audio track: process only updates effect state
        // and thus does have to be synchronized with audio writes but may have
        // to be called while waiting for async write callback
        if (mType == OFFLOAD) {
            for (size_t i = 0; i < effectChains.size(); i ++) {
                effectChains[i]->process_l();
            }
        }

        // Only if the Effects buffer is enabled and there is data in the
        // Effects buffer (buffer valid), we need to
        // copy into the sink buffer.
        // TODO use sleepTime == 0 as an additional condition.
        if (mEffectBufferValid) {
            //ALOGV("writing effect buffer to sink buffer format %#x", mFormat);
            memcpy_by_audio_format(mSinkBuffer, mFormat, mEffectBuffer, mEffectBufferFormat,
                    mNormalFrameCount * mChannelCount);
        }

        // enable changes in effect chain
        unlockEffectChains(effectChains);

        if (!waitingAsyncCallback()) {
            // sleepTime == 0 means we must write to audio hardware
            if (sleepTime == 0) {
                if (mBytesRemaining) {
                    ssize_t ret = threadLoop_write();
                    if (ret < 0) {
                        mBytesRemaining = 0;
                    } else {
                        mBytesWritten += ret;
                        mBytesRemaining -= ret;
                    }
                } else if ((mMixerStatus == MIXER_DRAIN_TRACK) ||
                        (mMixerStatus == MIXER_DRAIN_ALL)) {
                    threadLoop_drain();
                }
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

            } else {
                usleep(sleepTime);
            }
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

    threadLoop_exit();

    // for DuplicatingThread, standby mode is handled by the outputTracks, otherwise ...
    if (mType == MIXER || mType == DIRECT || mType == OFFLOAD) {
        // put output stream into standby mode
        if (!mStandby) {
            mOutput->stream->common.standby(&mOutput->stream->common);
        }
    }

    releaseWakeLock();
    mWakeLockUids.clear();
    mActiveTracksGeneration++;

    ALOGV("Thread %p type %d exiting", this, mType);
    return false;
}

// removeTracks_l() must be called with ThreadBase::mLock held
void AudioFlinger::PlaybackThread::removeTracks_l(const Vector< sp<Track> >& tracksToRemove)
{
    size_t count = tracksToRemove.size();
    if (count > 0) {
        for (size_t i=0 ; i<count ; i++) {
            const sp<Track>& track = tracksToRemove.itemAt(i);
            mActiveTracks.remove(track);
            mWakeLockUids.remove(track->uid());
            mActiveTracksGeneration++;
            ALOGV("removeTracks_l removing track on session %d", track->sessionId());
            sp<EffectChain> chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                ALOGV("stopping track on chain %p for session Id: %d", chain.get(),
                        track->sessionId());
                chain->decActiveTrackCnt();
            }
            if (track->isTerminated()) {
                removeTrack_l(track);
            }
        }
    }

}

status_t AudioFlinger::PlaybackThread::getTimestamp_l(AudioTimestamp& timestamp)
{
    if (mNormalSink != 0) {
        return mNormalSink->getTimestamp(timestamp);
    }
    if ((mType == OFFLOAD || mType == DIRECT) && mOutput->stream->get_presentation_position) {
        uint64_t position64;
        int ret = mOutput->stream->get_presentation_position(
                                                mOutput->stream, &position64, &timestamp.mTime);
        if (ret == 0) {
            timestamp.mPosition = (uint32_t)position64;
            return NO_ERROR;
        }
    }
    return INVALID_OPERATION;
}

status_t AudioFlinger::PlaybackThread::createAudioPatch_l(const struct audio_patch *patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;
    if (mOutput->audioHwDev->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        // store new device and send to effects
        audio_devices_t type = AUDIO_DEVICE_NONE;
        for (unsigned int i = 0; i < patch->num_sinks; i++) {
            type |= patch->sinks[i].ext.device.type;
        }
        mOutDevice = type;
        for (size_t i = 0; i < mEffectChains.size(); i++) {
            mEffectChains[i]->setDevice_l(mOutDevice);
        }

        audio_hw_device_t *hwDevice = mOutput->audioHwDev->hwDevice();
        status = hwDevice->create_audio_patch(hwDevice,
                                               patch->num_sources,
                                               patch->sources,
                                               patch->num_sinks,
                                               patch->sinks,
                                               handle);
    } else {
        ALOG_ASSERT(false, "createAudioPatch_l() called on a pre 3.0 HAL");
    }
    return status;
}

status_t AudioFlinger::PlaybackThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;
    if (mOutput->audioHwDev->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        audio_hw_device_t *hwDevice = mOutput->audioHwDev->hwDevice();
        status = hwDevice->release_audio_patch(hwDevice, handle);
    } else {
        ALOG_ASSERT(false, "releaseAudioPatch_l() called on a pre 3.0 HAL");
    }
    return status;
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
    ALOGV("mSampleRate=%u, mChannelMask=%#x, mChannelCount=%u, mFormat=%d, mFrameSize=%u, "
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
    const NBAIO_Format offers[1] = {Format_from_SR_C(mSampleRate, mChannelCount, mFormat)};
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
        audio_format_t fastMixerFormat;
        if (mMixerBufferEnabled && mEffectBufferEnabled) {
            fastMixerFormat = AUDIO_FORMAT_PCM_FLOAT;
        } else {
            fastMixerFormat = AUDIO_FORMAT_PCM_16_BIT;
        }
        if (mFormat != fastMixerFormat) {
            // change our Sink format to accept our intermediate precision
            mFormat = fastMixerFormat;
            free(mSinkBuffer);
            mFrameSize = mChannelCount * audio_bytes_per_sample(mFormat);
            const size_t sinkBufferSize = mNormalFrameCount * mFrameSize;
            (void)posix_memalign(&mSinkBuffer, 32, sinkBufferSize);
        }

        // create a MonoPipe to connect our submix to FastMixer
        NBAIO_Format format = mOutputSink->format();
        // adjust format to match that of the Fast Mixer
        format.mFormat = fastMixerFormat;
        format.mFrameSize = audio_bytes_per_sample(format.mFormat) * format.mChannelCount;

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
        fastTrack->mChannelMask = mChannelMask; // mPipeSink channel mask for audio to FastMixer
        fastTrack->mFormat = mFormat; // mPipeSink format for audio to FastMixer
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
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand == FastMixerState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastMixerFutex);
            if (old == -1) {
                (void) syscall(__NR_futex, &mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
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
        mFastMixer.clear();
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
    if (mFastMixer != 0) {
        MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
        latency += (pipe->getAvgFrames() * 1000) / mSampleRate;
    }
    return latency;
}


void AudioFlinger::MixerThread::threadLoop_removeTracks(const Vector< sp<Track> >& tracksToRemove)
{
    PlaybackThread::threadLoop_removeTracks(tracksToRemove);
}

ssize_t AudioFlinger::MixerThread::threadLoop_write()
{
    // FIXME we should only do one push per cycle; confirm this is true
    // Start the fast mixer if it's not already running
    if (mFastMixer != 0) {
        FastMixerStateQueue *sq = mFastMixer->sq();
        FastMixerState *state = sq->begin();
        if (state->mCommand != FastMixerState::MIX_WRITE &&
                (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)) {
            if (state->mCommand == FastMixerState::COLD_IDLE) {
                int32_t old = android_atomic_inc(&mFastMixerFutex);
                if (old == -1) {
                    (void) syscall(__NR_futex, &mFastMixerFutex, FUTEX_WAKE_PRIVATE, 1);
                }
#ifdef AUDIO_WATCHDOG
                if (mAudioWatchdog != 0) {
                    mAudioWatchdog->resume();
                }
#endif
            }
            state->mCommand = FastMixerState::MIX_WRITE;
            mFastMixerDumpState.increaseSamplingN(mAudioFlinger->isLowRamDevice() ?
                    FastMixerDumpState::kSamplingNforLowRamDevice : FastMixerDumpState::kSamplingN);
            sq->end();
            sq->push(FastMixerStateQueue::BLOCK_UNTIL_PUSHED);
            if (kUseFastMixer == FastMixer_Dynamic) {
                mNormalSink = mPipeSink;
            }
        } else {
            sq->end(false /*didModify*/);
        }
    }
    return PlaybackThread::threadLoop_write();
}

void AudioFlinger::MixerThread::threadLoop_standby()
{
    // Idle the fast mixer if it's currently running
    if (mFastMixer != 0) {
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

bool AudioFlinger::PlaybackThread::waitingAsyncCallback_l()
{
    return false;
}

bool AudioFlinger::PlaybackThread::shouldStandby_l()
{
    return !mStandby;
}

bool AudioFlinger::PlaybackThread::waitingAsyncCallback()
{
    Mutex::Autolock _l(mLock);
    return waitingAsyncCallback_l();
}

// shared by MIXER and DIRECT, overridden by DUPLICATING
void AudioFlinger::PlaybackThread::threadLoop_standby()
{
    ALOGV("Audio hardware entering standby, mixer %p, suspend count %d", this, mSuspended);
    mOutput->stream->common.standby(&mOutput->stream->common);
    if (mUseAsyncWrite != 0) {
        // discard any pending drain or write ack by incrementing sequence
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
}

void AudioFlinger::PlaybackThread::onAddNewTrack_l()
{
    ALOGV("signal playback thread");
    broadcast_l();
}

void AudioFlinger::MixerThread::threadLoop_mix()
{
    // obtain the presentation timestamp of the next output buffer
    int64_t pts;
    status_t status = INVALID_OPERATION;

    if (mNormalSink != 0) {
        status = mNormalSink->getNextWriteTimestamp(&pts);
    } else {
        status = mOutputSink->getNextWriteTimestamp(&pts);
    }

    if (status != NO_ERROR) {
        pts = AudioBufferProvider::kInvalidPTS;
    }

    // mix buffers...
    mAudioMixer->process(pts);
    mCurrentWriteLength = mSinkBufferSize;
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
        // clear out mMixerBuffer or mSinkBuffer, to ensure buffers are cleared
        // before effects processing or output.
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
        } else {
            memset(mSinkBuffer, 0, mSinkBufferSize);
        }
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
    if (mFastMixer != 0) {
        sq = mFastMixer->sq();
        state = sq->begin();
    }

    mMixerBufferValid = false;  // mMixerBuffer has no valid data until appropriate tracks found.
    mEffectBufferValid = false; // mEffectBuffer has no valid data until tracks found.

    for (size_t i=0 ; i<count ; i++) {
        const sp<Track> t = mActiveTracks[i].promote();
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
            if (!(track->isStopping() || track->isPausing() || track->isStopped()) &&
                    recentUnderruns > 0) {
                // FIXME fast mixer will pull & mix partial buffers, but we count as a full underrun
                track->mAudioTrackServerProxy->tallyUnderrunFrames(recentUnderruns * mFrameCount);
            }

            // This is similar to the state machine for normal tracks,
            // with a few modifications for fast tracks.
            bool isActive = true;
            switch (track->mState) {
            case TrackBase::STOPPING_1:
                // track stays active in STOPPING_1 state until first underrun
                if (recentUnderruns > 0 || track->isTerminated()) {
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
                    android_atomic_or(CBLK_DISABLED, &track->mCblk->mFlags);
                    // remove from active list, but state remains ACTIVE [confusing but true]
                    isActive = false;
                    break;
                }
                // fall through
            case TrackBase::STOPPING_2:
            case TrackBase::PAUSED:
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
                LOG_ALWAYS_FATAL("unexpected track state %d", track->mState);
            }

            if (isActive) {
                // was it previously inactive?
                if (!(state->mTrackMask & (1 << j))) {
                    ExtendedAudioBufferProvider *eabp = track;
                    VolumeProvider *vp = track;
                    fastTrack->mBufferProvider = eabp;
                    fastTrack->mVolumeProvider = vp;
                    fastTrack->mChannelMask = track->mChannelMask;
                    fastTrack->mFormat = track->mFormat;
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
                    LOG_ALWAYS_FATAL("fast track %d should have been active", j);
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
        size_t desiredFrames;
        uint32_t sr = track->sampleRate();
        if (sr == mSampleRate) {
            desiredFrames = mNormalFrameCount;
        } else {
            // +1 for rounding and +1 for additional sample needed for interpolation
            desiredFrames = (mNormalFrameCount * sr) / mSampleRate + 1 + 1;
            // add frames already consumed but not yet released by the resampler
            // because mAudioTrackServerProxy->framesReady() will include these frames
            desiredFrames += mAudioMixer->getUnreleasedFrames(track->name());
#if 0
            // the minimum track buffer size is normally twice the number of frames necessary
            // to fill one buffer and the resampler should not leave more than one buffer worth
            // of unreleased frames after each pass, but just in case...
            ALOG_ASSERT(desiredFrames <= cblk->frameCount_);
#endif
        }
        uint32_t minFrames = 1;
        if ((track->sharedBuffer() == 0) && !track->isStopped() && !track->isPausing() &&
                (mMixerStatusIgnoringFastTracks == MIXER_TRACKS_READY)) {
            minFrames = desiredFrames;
        }

        size_t framesReady = track->framesReady();
        if ((framesReady >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            ALOGVV("track %d s=%08x [OK] on thread %p", name, cblk->mServer, this);

            mixedTracks++;

            // track->mainBuffer() != mSinkBuffer or mMixerBuffer means
            // there is an effect chain connected to the track
            chain.clear();
            if (track->mainBuffer() != mSinkBuffer &&
                    track->mainBuffer() != mMixerBuffer) {
                if (mEffectBufferEnabled) {
                    mEffectBufferValid = true; // Later can set directly.
                }
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
            // FIXME should not make a decision based on mServer
            } else if (cblk->mServer != 0) {
                // If the track is stopped before the first frame was mixed,
                // do not apply ramp
                param = AudioMixer::RAMP_VOLUME;
            }

            // compute volume for this track
            uint32_t vl, vr;       // in U8.24 integer format
            float vlf, vrf, vaf;   // in [0.0, 1.0] float format
            if (track->isPausing() || mStreamTypes[track->streamType()].mute) {
                vl = vr = 0;
                vlf = vrf = vaf = 0.;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {

                // read original volumes with volume control
                float typeVolume = mStreamTypes[track->streamType()].volume;
                float v = masterVolume * typeVolume;
                AudioTrackServerProxy *proxy = track->mAudioTrackServerProxy;
                gain_minifloat_packed_t vlr = proxy->getVolumeLR();
                vlf = float_from_gain(gain_minifloat_unpack_left(vlr));
                vrf = float_from_gain(gain_minifloat_unpack_right(vlr));
                // track volumes come from shared memory, so can't be trusted and must be clamped
                if (vlf > GAIN_FLOAT_UNITY) {
                    ALOGV("Track left volume out of range: %.3g", vlf);
                    vlf = GAIN_FLOAT_UNITY;
                }
                if (vrf > GAIN_FLOAT_UNITY) {
                    ALOGV("Track right volume out of range: %.3g", vrf);
                    vrf = GAIN_FLOAT_UNITY;
                }
                // now apply the master volume and stream type volume
                vlf *= v;
                vrf *= v;
                // assuming master volume and stream type volume each go up to 1.0,
                // then derive vl and vr as U8.24 versions for the effect chain
                const float scaleto8_24 = MAX_GAIN_INT * MAX_GAIN_INT;
                vl = (uint32_t) (scaleto8_24 * vlf);
                vr = (uint32_t) (scaleto8_24 * vrf);
                // vl and vr are now in U8.24 format
                uint16_t sendLevel = proxy->getSendLevel_U4_12();
                // send level comes from shared memory and so may be corrupt
                if (sendLevel > MAX_GAIN_INT) {
                    ALOGV("Track send level out of range: %04X", sendLevel);
                    sendLevel = MAX_GAIN_INT;
                }
                // vaf is represented as [0.0, 1.0] float by rescaling sendLevel
                vaf = v * sendLevel * (1. / MAX_GAIN_INT);
            }

            // Delegate volume control to effect in track effect chain if needed
            if (chain != 0 && chain->setVolume_l(&vl, &vr)) {
                // Do not ramp volume if volume is controlled by effect
                param = AudioMixer::VOLUME;
                // Update remaining floating point volume levels
                vlf = (float)vl / (1 << 24);
                vrf = (float)vr / (1 << 24);
                track->mHasVolumeController = true;
            } else {
                // force no volume ramp when volume controller was just disabled or removed
                // from effect chain to avoid volume spike
                if (track->mHasVolumeController) {
                    param = AudioMixer::VOLUME;
                }
                track->mHasVolumeController = false;
            }

            // XXX: these things DON'T need to be done each time
            mAudioMixer->setBufferProvider(name, track);
            mAudioMixer->enable(name);

            mAudioMixer->setParameter(name, param, AudioMixer::VOLUME0, &vlf);
            mAudioMixer->setParameter(name, param, AudioMixer::VOLUME1, &vrf);
            mAudioMixer->setParameter(name, param, AudioMixer::AUXLEVEL, &vaf);
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::FORMAT, (void *)track->format());
            mAudioMixer->setParameter(
                name,
                AudioMixer::TRACK,
                AudioMixer::CHANNEL_MASK, (void *)(uintptr_t)track->channelMask());
            // limit track sample rate to 2 x output sample rate, which changes at re-configuration
            uint32_t maxSampleRate = mSampleRate * 2;
            uint32_t reqSampleRate = track->mAudioTrackServerProxy->getSampleRate();
            if (reqSampleRate == 0) {
                reqSampleRate = mSampleRate;
            } else if (reqSampleRate > maxSampleRate) {
                reqSampleRate = maxSampleRate;
            }
            mAudioMixer->setParameter(
                name,
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                (void *)(uintptr_t)reqSampleRate);
            /*
             * Select the appropriate output buffer for the track.
             *
             * Tracks with effects go into their own effects chain buffer
             * and from there into either mEffectBuffer or mSinkBuffer.
             *
             * Other tracks can use mMixerBuffer for higher precision
             * channel accumulation.  If this buffer is enabled
             * (mMixerBufferEnabled true), then selected tracks will accumulate
             * into it.
             *
             */
            if (mMixerBufferEnabled
                    && (track->mainBuffer() == mSinkBuffer
                            || track->mainBuffer() == mMixerBuffer)) {
                mAudioMixer->setParameter(
                        name,
                        AudioMixer::TRACK,
                        AudioMixer::MIXER_FORMAT, (void *)mMixerBufferFormat);
                mAudioMixer->setParameter(
                        name,
                        AudioMixer::TRACK,
                        AudioMixer::MAIN_BUFFER, (void *)mMixerBuffer);
                // TODO: override track->mainBuffer()?
                mMixerBufferValid = true;
            } else {
                mAudioMixer->setParameter(
                        name,
                        AudioMixer::TRACK,
                        AudioMixer::MIXER_FORMAT, (void *)AUDIO_FORMAT_PCM_16_BIT);
                mAudioMixer->setParameter(
                        name,
                        AudioMixer::TRACK,
                        AudioMixer::MAIN_BUFFER, (void *)track->mainBuffer());
            }
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
            if (framesReady < desiredFrames && !track->isStopped() && !track->isPaused()) {
                track->mAudioTrackServerProxy->tallyUnderrunFrames(desiredFrames);
            }
            // clear effect chain input buffer if an active track underruns to avoid sending
            // previous audio buffer again to effects
            chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                chain->clearInputBuffer();
            }

            ALOGVV("track %d s=%08x [NOT READY] on thread %p", name, cblk->mServer, this);
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
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                if (--(track->mRetryCount) <= 0) {
                    ALOGI("BUFFER TIMEOUT: remove(%d) from active list on thread %p", name, this);
                    tracksToRemove->add(track);
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED, &cblk->mFlags);
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
    removeTracks_l(*tracksToRemove);

    // sink or mix buffer must be cleared if all tracks are connected to an
    // effect chain as in this case the mixer will not write to the sink or mix buffer
    // and track effects will accumulate into it
    if ((mBytesRemaining == 0) && ((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            (mixedTracks == 0 && fastTracks > 0))) {
        // FIXME as a performance optimization, should remember previous zero status
        if (mMixerBufferValid) {
            memset(mMixerBuffer, 0, mMixerBufferSize);
            // TODO: In testing, mSinkBuffer below need not be cleared because
            // the PlaybackThread::threadLoop() copies mMixerBuffer into mSinkBuffer
            // after mixing.
            //
            // To enforce this guarantee:
            // ((mixedTracks != 0 && mixedTracks == tracksWithEffect) ||
            // (mixedTracks == 0 && fastTracks > 0))
            // must imply MIXER_TRACKS_READY.
            // Later, we may clear buffers regardless, and skip much of this logic.
        }
        // TODO - either mEffectBuffer or mSinkBuffer needs to be cleared.
        if (mEffectBufferValid) {
            memset(mEffectBuffer, 0, mEffectBufferSize);
        }
        // FIXME as a performance optimization, should remember previous zero status
        memset(mSinkBuffer, 0, mNormalFrameCount * mChannelCount * sizeof(int16_t));
    }

    // if any fast tracks, then status is ready
    mMixerStatusIgnoringFastTracks = mixerStatus;
    if (fastTracks > 0) {
        mixerStatus = MIXER_TRACKS_READY;
    }
    return mixerStatus;
}

// getTrackName_l() must be called with ThreadBase::mLock held
int AudioFlinger::MixerThread::getTrackName_l(audio_channel_mask_t channelMask,
        audio_format_t format, int sessionId)
{
    return mAudioMixer->getTrackName(channelMask, format, sessionId);
}

// deleteTrackName_l() must be called with ThreadBase::mLock held
void AudioFlinger::MixerThread::deleteTrackName_l(int name)
{
    ALOGV("remove track (%d) and delete from mixer", name);
    mAudioMixer->deleteTrackName(name);
}

// checkForNewParameter_l() must be called with ThreadBase::mLock held
bool AudioFlinger::MixerThread::checkForNewParameter_l(const String8& keyValuePair,
                                                       status_t& status)
{
    bool reconfig = false;

    status = NO_ERROR;

    // if !&IDLE, holds the FastMixer state to restore after new parameters processed
    FastMixerState::Command previousCommand = FastMixerState::HOT_IDLE;
    if (mFastMixer != 0) {
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

    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
        reconfig = true;
    }
    if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
        if ((audio_format_t) value != AUDIO_FORMAT_PCM_16_BIT) {
            status = BAD_VALUE;
        } else {
            // no need to save value, since it's constant
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
        if ((audio_channel_mask_t) value != AUDIO_CHANNEL_OUT_STEREO) {
            status = BAD_VALUE;
        } else {
            // no need to save value, since it's constant
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
            readOutputParameters_l();
            delete mAudioMixer;
            mAudioMixer = new AudioMixer(mNormalFrameCount, mSampleRate);
            for (size_t i = 0; i < mTracks.size() ; i++) {
                int name = getTrackName_l(mTracks[i]->mChannelMask,
                        mTracks[i]->mFormat, mTracks[i]->mSessionId);
                if (name < 0) {
                    break;
                }
                mTracks[i]->mName = name;
            }
            sendIoConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
        }
    }

    if (!(previousCommand & FastMixerState::IDLE)) {
        ALOG_ASSERT(mFastMixer != 0);
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

    dprintf(fd, "  AudioMixer tracks: 0x%08x\n", mAudioMixer->trackNames());

    // Make a non-atomic copy of fast mixer dump state so it won't change underneath us
    const FastMixerDumpState copy(mFastMixerDumpState);
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

AudioFlinger::DirectOutputThread::DirectOutputThread(const sp<AudioFlinger>& audioFlinger,
        AudioStreamOut* output, audio_io_handle_t id, uint32_t device,
        ThreadBase::type_t type)
    :   PlaybackThread(audioFlinger, output, id, device, type)
        // mLeftVolFloat, mRightVolFloat
{
}

AudioFlinger::DirectOutputThread::~DirectOutputThread()
{
}

void AudioFlinger::DirectOutputThread::processVolume_l(Track *track, bool lastTrack)
{
    audio_track_cblk_t* cblk = track->cblk();
    float left, right;

    if (mMasterMute || mStreamTypes[track->streamType()].mute) {
        left = right = 0;
    } else {
        float typeVolume = mStreamTypes[track->streamType()].volume;
        float v = mMasterVolume * typeVolume;
        AudioTrackServerProxy *proxy = track->mAudioTrackServerProxy;
        gain_minifloat_packed_t vlr = proxy->getVolumeLR();
        left = float_from_gain(gain_minifloat_unpack_left(vlr));
        if (left > GAIN_FLOAT_UNITY) {
            left = GAIN_FLOAT_UNITY;
        }
        left *= v;
        right = float_from_gain(gain_minifloat_unpack_right(vlr));
        if (right > GAIN_FLOAT_UNITY) {
            right = GAIN_FLOAT_UNITY;
        }
        right *= v;
    }

    if (lastTrack) {
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
                mEffectChains[0]->setVolume_l(&vl, &vr);
                left = (float)vl / (1 << 24);
                right = (float)vr / (1 << 24);
            }
            if (mOutput->stream->set_volume) {
                mOutput->stream->set_volume(mOutput->stream, left, right);
            }
        }
    }
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
        // Only consider last track started for volume and mixer state control.
        // In theory an older track could underrun and restart after the new one starts
        // but as we only care about the transition phase between two tracks on a
        // direct output, it is not a problem to ignore the underrun case.
        sp<Track> l = mLatestActiveTrack.promote();
        bool last = l.get() == track;

        // The first time a track is added we wait
        // for all its buffers to be filled before processing it
        uint32_t minFrames;
        if ((track->sharedBuffer() == 0) && !track->isStopping_1() && !track->isPausing()) {
            minFrames = mNormalFrameCount;
        } else {
            minFrames = 1;
        }

        ALOGI("prepareTracks_l minFrames %d state %d frames ready %d, ",
              minFrames, track->mState, track->framesReady());
        if ((track->framesReady() >= minFrames) && track->isReady() && !track->isPaused() &&
                !track->isStopping_2() && !track->isStopped())
        {
            ALOGVV("track %d s=%08x [OK]", track->name(), cblk->mServer);

            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                // make sure processVolume_l() will apply new volume even if 0
                mLeftVolFloat = mRightVolFloat = -1.0;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                }
            }

            // compute volume for this track
            processVolume_l(track, last);
            if (last) {
                // reset retry count
                track->mRetryCount = kMaxTrackRetriesDirect;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            // clear effect chain input buffer if the last active track started underruns
            // to avoid sending previous audio buffer again to effects
            if (!mEffectChains.isEmpty() && last) {
                mEffectChains[0]->clearInputBuffer();
            }
            if (track->isStopping_1()) {
                track->mState = TrackBase::STOPPING_2;
            }
            if ((track->sharedBuffer() != 0) || track->isStopped() ||
                    track->isStopping_2() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                size_t audioHALFrames;
                if (audio_is_linear_pcm(mFormat)) {
                    audioHALFrames = (latency_l() * mSampleRate) / 1000;
                } else {
                    audioHALFrames = 0;
                }

                size_t framesWritten = mBytesWritten / mFrameSize;
                if (mStandby || !last ||
                        track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopping_2()) {
                        track->mState = TrackBase::STOPPED;
                    }
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
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED, &cblk->mFlags);
                } else if (last) {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
    }

    // remove all the tracks that need to be...
    removeTracks_l(*tracksToRemove);

    return mixerStatus;
}

void AudioFlinger::DirectOutputThread::threadLoop_mix()
{
    size_t frameCount = mFrameCount;
    int8_t *curBuf = (int8_t *)mSinkBuffer;
    // output audio to hardware
    while (frameCount) {
        AudioBufferProvider::Buffer buffer;
        buffer.frameCount = frameCount;
        mActiveTrack->getNextBuffer(&buffer);
        if (buffer.raw == NULL) {
            memset(curBuf, 0, frameCount * mFrameSize);
            break;
        }
        memcpy(curBuf, buffer.raw, buffer.frameCount * mFrameSize);
        frameCount -= buffer.frameCount;
        curBuf += buffer.frameCount * mFrameSize;
        mActiveTrack->releaseBuffer(&buffer);
    }
    mCurrentWriteLength = curBuf - (int8_t *)mSinkBuffer;
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
        memset(mSinkBuffer, 0, mFrameCount * mFrameSize);
        sleepTime = 0;
    }
}

// getTrackName_l() must be called with ThreadBase::mLock held
int AudioFlinger::DirectOutputThread::getTrackName_l(audio_channel_mask_t channelMask __unused,
        audio_format_t format __unused, int sessionId __unused)
{
    return 0;
}

// deleteTrackName_l() must be called with ThreadBase::mLock held
void AudioFlinger::DirectOutputThread::deleteTrackName_l(int name __unused)
{
}

// checkForNewParameter_l() must be called with ThreadBase::mLock held
bool AudioFlinger::DirectOutputThread::checkForNewParameter_l(const String8& keyValuePair,
                                                              status_t& status)
{
    bool reconfig = false;

    status = NO_ERROR;

    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    if (param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) {
        // forward device change to effects that have requested to be
        // aware of attached audio device.
        if (value != AUDIO_DEVICE_NONE) {
            mOutDevice = value;
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setDevice_l(mOutDevice);
            }
        }
    }
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
            readOutputParameters_l();
            sendIoConfigEvent_l(AudioSystem::OUTPUT_CONFIG_CHANGED);
        }
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
    if (audio_is_linear_pcm(mFormat)) {
        standbyDelay = microseconds(activeSleepTime*2);
    } else {
        standbyDelay = kOffloadStandbyDelayNs;
    }
}

// ----------------------------------------------------------------------------

AudioFlinger::AsyncCallbackThread::AsyncCallbackThread(
        const wp<AudioFlinger::PlaybackThread>& playbackThread)
    :   Thread(false /*canCallJava*/),
        mPlaybackThread(playbackThread),
        mWriteAckSequence(0),
        mDrainSequence(0)
{
}

AudioFlinger::AsyncCallbackThread::~AsyncCallbackThread()
{
}

void AudioFlinger::AsyncCallbackThread::onFirstRef()
{
    run("Offload Cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}

bool AudioFlinger::AsyncCallbackThread::threadLoop()
{
    while (!exitPending()) {
        uint32_t writeAckSequence;
        uint32_t drainSequence;

        {
            Mutex::Autolock _l(mLock);
            while (!((mWriteAckSequence & 1) ||
                     (mDrainSequence & 1) ||
                     exitPending())) {
                mWaitWorkCV.wait(mLock);
            }

            if (exitPending()) {
                break;
            }
            ALOGV("AsyncCallbackThread mWriteAckSequence %d mDrainSequence %d",
                  mWriteAckSequence, mDrainSequence);
            writeAckSequence = mWriteAckSequence;
            mWriteAckSequence &= ~1;
            drainSequence = mDrainSequence;
            mDrainSequence &= ~1;
        }
        {
            sp<AudioFlinger::PlaybackThread> playbackThread = mPlaybackThread.promote();
            if (playbackThread != 0) {
                if (writeAckSequence & 1) {
                    playbackThread->resetWriteBlocked(writeAckSequence >> 1);
                }
                if (drainSequence & 1) {
                    playbackThread->resetDraining(drainSequence >> 1);
                }
            }
        }
    }
    return false;
}

void AudioFlinger::AsyncCallbackThread::exit()
{
    ALOGV("AsyncCallbackThread::exit");
    Mutex::Autolock _l(mLock);
    requestExit();
    mWaitWorkCV.broadcast();
}

void AudioFlinger::AsyncCallbackThread::setWriteBlocked(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    // bit 0 is cleared
    mWriteAckSequence = sequence << 1;
}

void AudioFlinger::AsyncCallbackThread::resetWriteBlocked()
{
    Mutex::Autolock _l(mLock);
    // ignore unexpected callbacks
    if (mWriteAckSequence & 2) {
        mWriteAckSequence |= 1;
        mWaitWorkCV.signal();
    }
}

void AudioFlinger::AsyncCallbackThread::setDraining(uint32_t sequence)
{
    Mutex::Autolock _l(mLock);
    // bit 0 is cleared
    mDrainSequence = sequence << 1;
}

void AudioFlinger::AsyncCallbackThread::resetDraining()
{
    Mutex::Autolock _l(mLock);
    // ignore unexpected callbacks
    if (mDrainSequence & 2) {
        mDrainSequence |= 1;
        mWaitWorkCV.signal();
    }
}


// ----------------------------------------------------------------------------
AudioFlinger::OffloadThread::OffloadThread(const sp<AudioFlinger>& audioFlinger,
        AudioStreamOut* output, audio_io_handle_t id, uint32_t device)
    :   DirectOutputThread(audioFlinger, output, id, device, OFFLOAD),
        mHwPaused(false),
        mFlushPending(false),
        mPausedBytesRemaining(0)
{
    //FIXME: mStandby should be set to true by ThreadBase constructor
    mStandby = true;
}

void AudioFlinger::OffloadThread::threadLoop_exit()
{
    if (mFlushPending || mHwPaused) {
        // If a flush is pending or track was paused, just discard buffered data
        flushHw_l();
    } else {
        mMixerStatus = MIXER_DRAIN_ALL;
        threadLoop_drain();
    }
    if (mUseAsyncWrite) {
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->exit();
    }
    PlaybackThread::threadLoop_exit();
}

AudioFlinger::PlaybackThread::mixer_state AudioFlinger::OffloadThread::prepareTracks_l(
    Vector< sp<Track> > *tracksToRemove
)
{
    size_t count = mActiveTracks.size();

    mixer_state mixerStatus = MIXER_IDLE;
    bool doHwPause = false;
    bool doHwResume = false;

    ALOGV("OffloadThread::prepareTracks_l active tracks %d", count);

    // find out which tracks need to be processed
    for (size_t i = 0; i < count; i++) {
        sp<Track> t = mActiveTracks[i].promote();
        // The track died recently
        if (t == 0) {
            continue;
        }
        Track* const track = t.get();
        audio_track_cblk_t* cblk = track->cblk();
        // Only consider last track started for volume and mixer state control.
        // In theory an older track could underrun and restart after the new one starts
        // but as we only care about the transition phase between two tracks on a
        // direct output, it is not a problem to ignore the underrun case.
        sp<Track> l = mLatestActiveTrack.promote();
        bool last = l.get() == track;

        if (track->isInvalid()) {
            ALOGW("An invalidated track shouldn't be in active list");
            tracksToRemove->add(track);
            continue;
        }

        if (track->mState == TrackBase::IDLE) {
            ALOGW("An idle track shouldn't be in active list");
            continue;
        }

        if (track->isPausing()) {
            track->setPaused();
            if (last) {
                if (!mHwPaused) {
                    doHwPause = true;
                    mHwPaused = true;
                }
                // If we were part way through writing the mixbuffer to
                // the HAL we must save this until we resume
                // BUG - this will be wrong if a different track is made active,
                // in that case we want to discard the pending data in the
                // mixbuffer and tell the client to present it again when the
                // track is resumed
                mPausedWriteLength = mCurrentWriteLength;
                mPausedBytesRemaining = mBytesRemaining;
                mBytesRemaining = 0;    // stop writing
            }
            tracksToRemove->add(track);
        } else if (track->isFlushPending()) {
            track->flushAck();
            if (last) {
                mFlushPending = true;
            }
        } else if (track->isResumePending()){
            track->resumeAck();
            if (last) {
                if (mPausedBytesRemaining) {
                    // Need to continue write that was interrupted
                    mCurrentWriteLength = mPausedWriteLength;
                    mBytesRemaining = mPausedBytesRemaining;
                    mPausedBytesRemaining = 0;
                }
                if (mHwPaused) {
                    doHwResume = true;
                    mHwPaused = false;
                    // threadLoop_mix() will handle the case that we need to
                    // resume an interrupted write
                }
                // enable write to audio HAL
                sleepTime = 0;

                // Do not handle new data in this iteration even if track->framesReady()
                mixerStatus = MIXER_TRACKS_ENABLED;
            }
        }  else if (track->framesReady() && track->isReady() &&
                !track->isPaused() && !track->isTerminated() && !track->isStopping_2()) {
            ALOGVV("OffloadThread: track %d s=%08x [OK]", track->name(), cblk->mServer);
            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                // make sure processVolume_l() will apply new volume even if 0
                mLeftVolFloat = mRightVolFloat = -1.0;
            }

            if (last) {
                sp<Track> previousTrack = mPreviousTrack.promote();
                if (previousTrack != 0) {
                    if (track != previousTrack.get()) {
                        // Flush any data still being written from last track
                        mBytesRemaining = 0;
                        if (mPausedBytesRemaining) {
                            // Last track was paused so we also need to flush saved
                            // mixbuffer state and invalidate track so that it will
                            // re-submit that unwritten data when it is next resumed
                            mPausedBytesRemaining = 0;
                            // Invalidate is a bit drastic - would be more efficient
                            // to have a flag to tell client that some of the
                            // previously written data was lost
                            previousTrack->invalidate();
                        }
                        // flush data already sent to the DSP if changing audio session as audio
                        // comes from a different source. Also invalidate previous track to force a
                        // seek when resuming.
                        if (previousTrack->sessionId() != track->sessionId()) {
                            previousTrack->invalidate();
                        }
                    }
                }
                mPreviousTrack = track;
                // reset retry count
                track->mRetryCount = kMaxTrackRetriesOffload;
                mActiveTrack = t;
                mixerStatus = MIXER_TRACKS_READY;
            }
        } else {
            ALOGVV("OffloadThread: track %d s=%08x [NOT READY]", track->name(), cblk->mServer);
            if (track->isStopping_1()) {
                // Hardware buffer can hold a large amount of audio so we must
                // wait for all current track's data to drain before we say
                // that the track is stopped.
                if (mBytesRemaining == 0) {
                    // Only start draining when all data in mixbuffer
                    // has been written
                    ALOGV("OffloadThread: underrun and STOPPING_1 -> draining, STOPPING_2");
                    track->mState = TrackBase::STOPPING_2; // so presentation completes after drain
                    // do not drain if no data was ever sent to HAL (mStandby == true)
                    if (last && !mStandby) {
                        // do not modify drain sequence if we are already draining. This happens
                        // when resuming from pause after drain.
                        if ((mDrainSequence & 1) == 0) {
                            sleepTime = 0;
                            standbyTime = systemTime() + standbyDelay;
                            mixerStatus = MIXER_DRAIN_TRACK;
                            mDrainSequence += 2;
                        }
                        if (mHwPaused) {
                            // It is possible to move from PAUSED to STOPPING_1 without
                            // a resume so we must ensure hardware is running
                            doHwResume = true;
                            mHwPaused = false;
                        }
                    }
                }
            } else if (track->isStopping_2()) {
                // Drain has completed or we are in standby, signal presentation complete
                if (!(mDrainSequence & 1) || !last || mStandby) {
                    track->mState = TrackBase::STOPPED;
                    size_t audioHALFrames =
                            (mOutput->stream->get_latency(mOutput->stream)*mSampleRate) / 1000;
                    size_t framesWritten =
                            mBytesWritten / audio_stream_frame_size(&mOutput->stream->common);
                    track->presentationComplete(framesWritten, audioHALFrames);
                    track->reset();
                    tracksToRemove->add(track);
                }
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                if (--(track->mRetryCount) <= 0) {
                    ALOGV("OffloadThread: BUFFER TIMEOUT: remove(%d) from active list",
                          track->name());
                    tracksToRemove->add(track);
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED, &cblk->mFlags);
                } else if (last){
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
        // compute volume for this track
        processVolume_l(track, last);
    }

    // make sure the pause/flush/resume sequence is executed in the right order.
    // If a flush is pending and a track is active but the HW is not paused, force a HW pause
    // before flush and then resume HW. This can happen in case of pause/flush/resume
    // if resume is received before pause is executed.
    if (!mStandby && (doHwPause || (mFlushPending && !mHwPaused && (count != 0)))) {
        mOutput->stream->pause(mOutput->stream);
    }
    if (mFlushPending) {
        flushHw_l();
        mFlushPending = false;
    }
    if (!mStandby && doHwResume) {
        mOutput->stream->resume(mOutput->stream);
    }

    // remove all the tracks that need to be...
    removeTracks_l(*tracksToRemove);

    return mixerStatus;
}

// must be called with thread mutex locked
bool AudioFlinger::OffloadThread::waitingAsyncCallback_l()
{
    ALOGVV("waitingAsyncCallback_l mWriteAckSequence %d mDrainSequence %d",
          mWriteAckSequence, mDrainSequence);
    if (mUseAsyncWrite && ((mWriteAckSequence & 1) || (mDrainSequence & 1))) {
        return true;
    }
    return false;
}

// must be called with thread mutex locked
bool AudioFlinger::OffloadThread::shouldStandby_l()
{
    bool trackPaused = false;

    // do not put the HAL in standby when paused. AwesomePlayer clear the offloaded AudioTrack
    // after a timeout and we will enter standby then.
    if (mTracks.size() > 0) {
        trackPaused = mTracks[mTracks.size() - 1]->isPaused();
    }

    return !mStandby && !trackPaused;
}


bool AudioFlinger::OffloadThread::waitingAsyncCallback()
{
    Mutex::Autolock _l(mLock);
    return waitingAsyncCallback_l();
}

void AudioFlinger::OffloadThread::flushHw_l()
{
    mOutput->stream->flush(mOutput->stream);
    // Flush anything still waiting in the mixbuffer
    mCurrentWriteLength = 0;
    mBytesRemaining = 0;
    mPausedWriteLength = 0;
    mPausedBytesRemaining = 0;
    mHwPaused = false;

    if (mUseAsyncWrite) {
        // discard any pending drain or write ack by incrementing sequence
        mWriteAckSequence = (mWriteAckSequence + 2) & ~1;
        mDrainSequence = (mDrainSequence + 2) & ~1;
        ALOG_ASSERT(mCallbackThread != 0);
        mCallbackThread->setWriteBlocked(mWriteAckSequence);
        mCallbackThread->setDraining(mDrainSequence);
    }
}

void AudioFlinger::OffloadThread::onAddNewTrack_l()
{
    sp<Track> previousTrack = mPreviousTrack.promote();
    sp<Track> latestTrack = mLatestActiveTrack.promote();

    if (previousTrack != 0 && latestTrack != 0 &&
        (previousTrack->sessionId() != latestTrack->sessionId())) {
        mFlushPending = true;
    }
    PlaybackThread::onAddNewTrack_l();
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
        memset(mSinkBuffer, 0, mSinkBufferSize);
    }
    sleepTime = 0;
    writeFrames = mNormalFrameCount;
    mCurrentWriteLength = mSinkBufferSize;
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
            memset(mSinkBuffer, 0, mSinkBufferSize);
        } else {
            // flush remaining overflow buffers in output tracks
            writeFrames = 0;
        }
        sleepTime = 0;
    }
}

ssize_t AudioFlinger::DuplicatingThread::threadLoop_write()
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        // We convert the duplicating thread format to AUDIO_FORMAT_PCM_16_BIT
        // for delivery downstream as needed. This in-place conversion is safe as
        // AUDIO_FORMAT_PCM_16_BIT is smaller than any other supported format
        // (AUDIO_FORMAT_PCM_8_BIT is not allowed here).
        if (mFormat != AUDIO_FORMAT_PCM_16_BIT) {
            memcpy_by_audio_format(mSinkBuffer, AUDIO_FORMAT_PCM_16_BIT,
                    mSinkBuffer, mFormat, writeFrames * mChannelCount);
        }
        outputTracks[i]->write(reinterpret_cast<int16_t*>(mSinkBuffer), writeFrames);
    }
    mStandby = false;
    return (ssize_t)mSinkBufferSize;
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
    size_t frameCount = (3 * mNormalFrameCount * mSampleRate) / thread->sampleRate();
    // OutputTrack is forced to AUDIO_FORMAT_PCM_16_BIT regardless of mFormat
    // due to current usage case and restrictions on the AudioBufferProvider.
    // Actual buffer conversion is done in threadLoop_write().
    //
    // TODO: This may change in the future, depending on multichannel
    // (and non int16_t*) support on AF::PlaybackThread::OutputTrack
    OutputTrack *outputTrack = new OutputTrack(thread,
                                            this,
                                            mSampleRate,
                                            AUDIO_FORMAT_PCM_16_BIT,
                                            mChannelMask,
                                            frameCount,
                                            IPCThreadState::self()->getCallingUid());
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
                                         audio_io_handle_t id,
                                         audio_devices_t outDevice,
                                         audio_devices_t inDevice
#ifdef TEE_SINK
                                         , const sp<NBAIO_Sink>& teeSink
#endif
                                         ) :
    ThreadBase(audioFlinger, id, outDevice, inDevice, RECORD),
    mInput(input), mActiveTracksGen(0), mRsmpInBuffer(NULL),
    // mRsmpInFrames and mRsmpInFramesP2 are set by readInputParameters_l()
    mRsmpInRear(0)
#ifdef TEE_SINK
    , mTeeSink(teeSink)
#endif
    , mReadOnlyHeap(new MemoryDealer(kRecordThreadReadOnlyHeapSize,
            "RecordThreadRO", MemoryHeapBase::READ_ONLY))
    // mFastCapture below
    , mFastCaptureFutex(0)
    // mInputSource
    // mPipeSink
    // mPipeSource
    , mPipeFramesP2(0)
    // mPipeMemory
    // mFastCaptureNBLogWriter
    , mFastTrackAvail(true)
{
    snprintf(mName, kNameLength, "AudioIn_%X", id);
    mNBLogWriter = audioFlinger->newWriter_l(kLogSize, mName);

    readInputParameters_l();

    // create an NBAIO source for the HAL input stream, and negotiate
    mInputSource = new AudioStreamInSource(input->stream);
    size_t numCounterOffers = 0;
    const NBAIO_Format offers[1] = {Format_from_SR_C(mSampleRate, mChannelCount, mFormat)};
    ssize_t index = mInputSource->negotiate(offers, 1, NULL, numCounterOffers);
    ALOG_ASSERT(index == 0);

    // initialize fast capture depending on configuration
    bool initFastCapture;
    switch (kUseFastCapture) {
    case FastCapture_Never:
        initFastCapture = false;
        break;
    case FastCapture_Always:
        initFastCapture = true;
        break;
    case FastCapture_Static:
        uint32_t primaryOutputSampleRate;
        {
            AutoMutex _l(audioFlinger->mHardwareLock);
            primaryOutputSampleRate = audioFlinger->mPrimaryOutputSampleRate;
        }
        initFastCapture =
                // either capture sample rate is same as (a reasonable) primary output sample rate
                (((primaryOutputSampleRate == 44100 || primaryOutputSampleRate == 48000) &&
                    (mSampleRate == primaryOutputSampleRate)) ||
                // or primary output sample rate is unknown, and capture sample rate is reasonable
                ((primaryOutputSampleRate == 0) &&
                    ((mSampleRate == 44100 || mSampleRate == 48000)))) &&
                // and the buffer size is < 10 ms
                (mFrameCount * 1000) / mSampleRate < 10;
        break;
    // case FastCapture_Dynamic:
    }

    if (initFastCapture) {
        // create a Pipe for FastMixer to write to, and for us and fast tracks to read from
        NBAIO_Format format = mInputSource->format();
        size_t pipeFramesP2 = roundup(mFrameCount * 8);
        size_t pipeSize = pipeFramesP2 * Format_frameSize(format);
        void *pipeBuffer;
        const sp<MemoryDealer> roHeap(readOnlyHeap());
        sp<IMemory> pipeMemory;
        if ((roHeap == 0) ||
                (pipeMemory = roHeap->allocate(pipeSize)) == 0 ||
                (pipeBuffer = pipeMemory->pointer()) == NULL) {
            ALOGE("not enough memory for pipe buffer size=%zu", pipeSize);
            goto failed;
        }
        // pipe will be shared directly with fast clients, so clear to avoid leaking old information
        memset(pipeBuffer, 0, pipeSize);
        Pipe *pipe = new Pipe(pipeFramesP2, format, pipeBuffer);
        const NBAIO_Format offers[1] = {format};
        size_t numCounterOffers = 0;
        ssize_t index = pipe->negotiate(offers, 1, NULL, numCounterOffers);
        ALOG_ASSERT(index == 0);
        mPipeSink = pipe;
        PipeReader *pipeReader = new PipeReader(*pipe);
        numCounterOffers = 0;
        index = pipeReader->negotiate(offers, 1, NULL, numCounterOffers);
        ALOG_ASSERT(index == 0);
        mPipeSource = pipeReader;
        mPipeFramesP2 = pipeFramesP2;
        mPipeMemory = pipeMemory;

        // create fast capture
        mFastCapture = new FastCapture();
        FastCaptureStateQueue *sq = mFastCapture->sq();
#ifdef STATE_QUEUE_DUMP
        // FIXME
#endif
        FastCaptureState *state = sq->begin();
        state->mCblk = NULL;
        state->mInputSource = mInputSource.get();
        state->mInputSourceGen++;
        state->mPipeSink = pipe;
        state->mPipeSinkGen++;
        state->mFrameCount = mFrameCount;
        state->mCommand = FastCaptureState::COLD_IDLE;
        // already done in constructor initialization list
        //mFastCaptureFutex = 0;
        state->mColdFutexAddr = &mFastCaptureFutex;
        state->mColdGen++;
        state->mDumpState = &mFastCaptureDumpState;
#ifdef TEE_SINK
        // FIXME
#endif
        mFastCaptureNBLogWriter = audioFlinger->newWriter_l(kFastCaptureLogSize, "FastCapture");
        state->mNBLogWriter = mFastCaptureNBLogWriter.get();
        sq->end();
        sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);

        // start the fast capture
        mFastCapture->run("FastCapture", ANDROID_PRIORITY_URGENT_AUDIO);
        pid_t tid = mFastCapture->getTid();
        int err = requestPriority(getpid_cached, tid, kPriorityFastMixer);
        if (err != 0) {
            ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
                    kPriorityFastCapture, getpid_cached, tid, err);
        }

#ifdef AUDIO_WATCHDOG
        // FIXME
#endif

    }
failed: ;

    // FIXME mNormalSource
}


AudioFlinger::RecordThread::~RecordThread()
{
    if (mFastCapture != 0) {
        FastCaptureStateQueue *sq = mFastCapture->sq();
        FastCaptureState *state = sq->begin();
        if (state->mCommand == FastCaptureState::COLD_IDLE) {
            int32_t old = android_atomic_inc(&mFastCaptureFutex);
            if (old == -1) {
                (void) syscall(__NR_futex, &mFastCaptureFutex, FUTEX_WAKE_PRIVATE, 1);
            }
        }
        state->mCommand = FastCaptureState::EXIT;
        sq->end();
        sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);
        mFastCapture->join();
        mFastCapture.clear();
    }
    mAudioFlinger->unregisterWriter(mFastCaptureNBLogWriter);
    mAudioFlinger->unregisterWriter(mNBLogWriter);
    delete[] mRsmpInBuffer;
}

void AudioFlinger::RecordThread::onFirstRef()
{
    run(mName, PRIORITY_URGENT_AUDIO);
}

bool AudioFlinger::RecordThread::threadLoop()
{
    nsecs_t lastWarning = 0;

    inputStandBy();

reacquire_wakelock:
    sp<RecordTrack> activeTrack;
    int activeTracksGen;
    {
        Mutex::Autolock _l(mLock);
        size_t size = mActiveTracks.size();
        activeTracksGen = mActiveTracksGen;
        if (size > 0) {
            // FIXME an arbitrary choice
            activeTrack = mActiveTracks[0];
            acquireWakeLock_l(activeTrack->uid());
            if (size > 1) {
                SortedVector<int> tmp;
                for (size_t i = 0; i < size; i++) {
                    tmp.add(mActiveTracks[i]->uid());
                }
                updateWakeLockUids_l(tmp);
            }
        } else {
            acquireWakeLock_l(-1);
        }
    }

    // used to request a deferred sleep, to be executed later while mutex is unlocked
    uint32_t sleepUs = 0;

    // loop while there is work to do
    for (;;) {
        Vector< sp<EffectChain> > effectChains;

        // sleep with mutex unlocked
        if (sleepUs > 0) {
            usleep(sleepUs);
            sleepUs = 0;
        }

        // activeTracks accumulates a copy of a subset of mActiveTracks
        Vector< sp<RecordTrack> > activeTracks;

        // reference to the (first and only) fast track
        sp<RecordTrack> fastTrack;

        { // scope for mLock
            Mutex::Autolock _l(mLock);

            processConfigEvents_l();

            // check exitPending here because checkForNewParameters_l() and
            // checkForNewParameters_l() can temporarily release mLock
            if (exitPending()) {
                break;
            }

            // if no active track(s), then standby and release wakelock
            size_t size = mActiveTracks.size();
            if (size == 0) {
                standbyIfNotAlreadyInStandby();
                // exitPending() can't become true here
                releaseWakeLock_l();
                ALOGV("RecordThread: loop stopping");
                // go to sleep
                mWaitWorkCV.wait(mLock);
                ALOGV("RecordThread: loop starting");
                goto reacquire_wakelock;
            }

            if (mActiveTracksGen != activeTracksGen) {
                activeTracksGen = mActiveTracksGen;
                SortedVector<int> tmp;
                for (size_t i = 0; i < size; i++) {
                    tmp.add(mActiveTracks[i]->uid());
                }
                updateWakeLockUids_l(tmp);
            }

            bool doBroadcast = false;
            for (size_t i = 0; i < size; ) {

                activeTrack = mActiveTracks[i];
                if (activeTrack->isTerminated()) {
                    removeTrack_l(activeTrack);
                    mActiveTracks.remove(activeTrack);
                    mActiveTracksGen++;
                    size--;
                    continue;
                }

                TrackBase::track_state activeTrackState = activeTrack->mState;
                switch (activeTrackState) {

                case TrackBase::PAUSING:
                    mActiveTracks.remove(activeTrack);
                    mActiveTracksGen++;
                    doBroadcast = true;
                    size--;
                    continue;

                case TrackBase::STARTING_1:
                    sleepUs = 10000;
                    i++;
                    continue;

                case TrackBase::STARTING_2:
                    doBroadcast = true;
                    mStandby = false;
                    activeTrack->mState = TrackBase::ACTIVE;
                    break;

                case TrackBase::ACTIVE:
                    break;

                case TrackBase::IDLE:
                    i++;
                    continue;

                default:
                    LOG_ALWAYS_FATAL("Unexpected activeTrackState %d", activeTrackState);
                }

                activeTracks.add(activeTrack);
                i++;

                if (activeTrack->isFastTrack()) {
                    ALOG_ASSERT(!mFastTrackAvail);
                    ALOG_ASSERT(fastTrack == 0);
                    fastTrack = activeTrack;
                }
            }
            if (doBroadcast) {
                mStartStopCond.broadcast();
            }

            // sleep if there are no active tracks to process
            if (activeTracks.size() == 0) {
                if (sleepUs == 0) {
                    sleepUs = kRecordThreadSleepUs;
                }
                continue;
            }
            sleepUs = 0;

            lockEffectChains_l(effectChains);
        }

        // thread mutex is now unlocked, mActiveTracks unknown, activeTracks.size() > 0

        size_t size = effectChains.size();
        for (size_t i = 0; i < size; i++) {
            // thread mutex is not locked, but effect chain is locked
            effectChains[i]->process_l();
        }

        // Start the fast capture if it's not already running
        if (mFastCapture != 0) {
            FastCaptureStateQueue *sq = mFastCapture->sq();
            FastCaptureState *state = sq->begin();
            if (state->mCommand != FastCaptureState::READ_WRITE /* FIXME &&
                    (kUseFastMixer != FastMixer_Dynamic || state->mTrackMask > 1)*/) {
                if (state->mCommand == FastCaptureState::COLD_IDLE) {
                    int32_t old = android_atomic_inc(&mFastCaptureFutex);
                    if (old == -1) {
                        (void) syscall(__NR_futex, &mFastCaptureFutex, FUTEX_WAKE_PRIVATE, 1);
                    }
                }
                state->mCommand = FastCaptureState::READ_WRITE;
#if 0   // FIXME
                mFastCaptureDumpState.increaseSamplingN(mAudioFlinger->isLowRamDevice() ?
                        FastCaptureDumpState::kSamplingNforLowRamDevice : FastMixerDumpState::kSamplingN);
#endif
                state->mCblk = fastTrack != 0 ? fastTrack->cblk() : NULL;
                sq->end();
                sq->push(FastCaptureStateQueue::BLOCK_UNTIL_PUSHED);
#if 0
                if (kUseFastCapture == FastCapture_Dynamic) {
                    mNormalSource = mPipeSource;
                }
#endif
            } else {
                sq->end(false /*didModify*/);
            }
        }

        // Read from HAL to keep up with fastest client if multiple active tracks, not slowest one.
        // Only the client(s) that are too slow will overrun. But if even the fastest client is too
        // slow, then this RecordThread will overrun by not calling HAL read often enough.
        // If destination is non-contiguous, first read past the nominal end of buffer, then
        // copy to the right place.  Permitted because mRsmpInBuffer was over-allocated.

        int32_t rear = mRsmpInRear & (mRsmpInFramesP2 - 1);
        ssize_t framesRead;

        // If an NBAIO source is present, use it to read the normal capture's data
        if (mPipeSource != 0) {
            size_t framesToRead = mBufferSize / mFrameSize;
            framesRead = mPipeSource->read(&mRsmpInBuffer[rear * mChannelCount],
                    framesToRead, AudioBufferProvider::kInvalidPTS);
            if (framesRead == 0) {
                // since pipe is non-blocking, simulate blocking input
                sleepUs = (framesToRead * 1000000LL) / mSampleRate;
            }
        // otherwise use the HAL / AudioStreamIn directly
        } else {
            ssize_t bytesRead = mInput->stream->read(mInput->stream,
                    &mRsmpInBuffer[rear * mChannelCount], mBufferSize);
            if (bytesRead < 0) {
                framesRead = bytesRead;
            } else {
                framesRead = bytesRead / mFrameSize;
            }
        }

        if (framesRead < 0 || (framesRead == 0 && mPipeSource == 0)) {
            ALOGE("read failed: framesRead=%d", framesRead);
            // Force input into standby so that it tries to recover at next read attempt
            inputStandBy();
            sleepUs = kRecordThreadSleepUs;
        }
        if (framesRead <= 0) {
            goto unlock;
        }
        ALOG_ASSERT(framesRead > 0);

        if (mTeeSink != 0) {
            (void) mTeeSink->write(&mRsmpInBuffer[rear * mChannelCount], framesRead);
        }
        // If destination is non-contiguous, we now correct for reading past end of buffer.
        {
            size_t part1 = mRsmpInFramesP2 - rear;
            if ((size_t) framesRead > part1) {
                memcpy(mRsmpInBuffer, &mRsmpInBuffer[mRsmpInFramesP2 * mChannelCount],
                        (framesRead - part1) * mFrameSize);
            }
        }
        rear = mRsmpInRear += framesRead;

        size = activeTracks.size();
        // loop over each active track
        for (size_t i = 0; i < size; i++) {
            activeTrack = activeTracks[i];

            // skip fast tracks, as those are handled directly by FastCapture
            if (activeTrack->isFastTrack()) {
                continue;
            }

            enum {
                OVERRUN_UNKNOWN,
                OVERRUN_TRUE,
                OVERRUN_FALSE
            } overrun = OVERRUN_UNKNOWN;

            // loop over getNextBuffer to handle circular sink
            for (;;) {

                activeTrack->mSink.frameCount = ~0;
                status_t status = activeTrack->getNextBuffer(&activeTrack->mSink);
                size_t framesOut = activeTrack->mSink.frameCount;
                LOG_ALWAYS_FATAL_IF((status == OK) != (framesOut > 0));

                int32_t front = activeTrack->mRsmpInFront;
                ssize_t filled = rear - front;
                size_t framesIn;

                if (filled < 0) {
                    // should not happen, but treat like a massive overrun and re-sync
                    framesIn = 0;
                    activeTrack->mRsmpInFront = rear;
                    overrun = OVERRUN_TRUE;
                } else if ((size_t) filled <= mRsmpInFrames) {
                    framesIn = (size_t) filled;
                } else {
                    // client is not keeping up with server, but give it latest data
                    framesIn = mRsmpInFrames;
                    activeTrack->mRsmpInFront = front = rear - framesIn;
                    overrun = OVERRUN_TRUE;
                }

                if (framesOut == 0 || framesIn == 0) {
                    break;
                }

                if (activeTrack->mResampler == NULL) {
                    // no resampling
                    if (framesIn > framesOut) {
                        framesIn = framesOut;
                    } else {
                        framesOut = framesIn;
                    }
                    int8_t *dst = activeTrack->mSink.i8;
                    while (framesIn > 0) {
                        front &= mRsmpInFramesP2 - 1;
                        size_t part1 = mRsmpInFramesP2 - front;
                        if (part1 > framesIn) {
                            part1 = framesIn;
                        }
                        int8_t *src = (int8_t *)mRsmpInBuffer + (front * mFrameSize);
                        if (mChannelCount == activeTrack->mChannelCount) {
                            memcpy(dst, src, part1 * mFrameSize);
                        } else if (mChannelCount == 1) {
                            upmix_to_stereo_i16_from_mono_i16((int16_t *)dst, (int16_t *)src,
                                    part1);
                        } else {
                            downmix_to_mono_i16_from_stereo_i16((int16_t *)dst, (int16_t *)src,
                                    part1);
                        }
                        dst += part1 * activeTrack->mFrameSize;
                        front += part1;
                        framesIn -= part1;
                    }
                    activeTrack->mRsmpInFront += framesOut;

                } else {
                    // resampling
                    // FIXME framesInNeeded should really be part of resampler API, and should
                    //       depend on the SRC ratio
                    //       to keep mRsmpInBuffer full so resampler always has sufficient input
                    size_t framesInNeeded;
                    // FIXME only re-calculate when it changes, and optimize for common ratios
                    double inOverOut = (double) mSampleRate / activeTrack->mSampleRate;
                    double outOverIn = (double) activeTrack->mSampleRate / mSampleRate;
                    framesInNeeded = ceil(framesOut * inOverOut) + 1;
                    ALOGV("need %u frames in to produce %u out given in/out ratio of %.4g",
                                framesInNeeded, framesOut, inOverOut);
                    // Although we theoretically have framesIn in circular buffer, some of those are
                    // unreleased frames, and thus must be discounted for purpose of budgeting.
                    size_t unreleased = activeTrack->mRsmpInUnrel;
                    framesIn = framesIn > unreleased ? framesIn - unreleased : 0;
                    if (framesIn < framesInNeeded) {
                        ALOGV("not enough to resample: have %u frames in but need %u in to "
                                "produce %u out given in/out ratio of %.4g",
                                framesIn, framesInNeeded, framesOut, inOverOut);
                        size_t newFramesOut = framesIn > 0 ? floor((framesIn - 1) * outOverIn) : 0;
                        LOG_ALWAYS_FATAL_IF(newFramesOut >= framesOut);
                        if (newFramesOut == 0) {
                            break;
                        }
                        framesInNeeded = ceil(newFramesOut * inOverOut) + 1;
                        ALOGV("now need %u frames in to produce %u out given out/in ratio of %.4g",
                                framesInNeeded, newFramesOut, outOverIn);
                        LOG_ALWAYS_FATAL_IF(framesIn < framesInNeeded);
                        ALOGV("success 2: have %u frames in and need %u in to produce %u out "
                              "given in/out ratio of %.4g",
                              framesIn, framesInNeeded, newFramesOut, inOverOut);
                        framesOut = newFramesOut;
                    } else {
                        ALOGV("success 1: have %u in and need %u in to produce %u out "
                            "given in/out ratio of %.4g",
                            framesIn, framesInNeeded, framesOut, inOverOut);
                    }

                    // reallocate mRsmpOutBuffer as needed; we will grow but never shrink
                    if (activeTrack->mRsmpOutFrameCount < framesOut) {
                        // FIXME why does each track need it's own mRsmpOutBuffer? can't they share?
                        delete[] activeTrack->mRsmpOutBuffer;
                        // resampler always outputs stereo
                        activeTrack->mRsmpOutBuffer = new int32_t[framesOut * FCC_2];
                        activeTrack->mRsmpOutFrameCount = framesOut;
                    }

                    // resampler accumulates, but we only have one source track
                    memset(activeTrack->mRsmpOutBuffer, 0, framesOut * FCC_2 * sizeof(int32_t));
                    activeTrack->mResampler->resample(activeTrack->mRsmpOutBuffer, framesOut,
                            // FIXME how about having activeTrack implement this interface itself?
                            activeTrack->mResamplerBufferProvider
                            /*this*/ /* AudioBufferProvider* */);
                    // ditherAndClamp() works as long as all buffers returned by
                    // activeTrack->getNextBuffer() are 32 bit aligned which should be always true.
                    if (activeTrack->mChannelCount == 1) {
                        // temporarily type pun mRsmpOutBuffer from Q4.27 to int16_t
                        ditherAndClamp(activeTrack->mRsmpOutBuffer, activeTrack->mRsmpOutBuffer,
                                framesOut);
                        // the resampler always outputs stereo samples:
                        // do post stereo to mono conversion
                        downmix_to_mono_i16_from_stereo_i16(activeTrack->mSink.i16,
                                (int16_t *)activeTrack->mRsmpOutBuffer, framesOut);
                    } else {
                        ditherAndClamp((int32_t *)activeTrack->mSink.raw,
                                activeTrack->mRsmpOutBuffer, framesOut);
                    }
                    // now done with mRsmpOutBuffer

                }

                if (framesOut > 0 && (overrun == OVERRUN_UNKNOWN)) {
                    overrun = OVERRUN_FALSE;
                }

                if (activeTrack->mFramesToDrop == 0) {
                    if (framesOut > 0) {
                        activeTrack->mSink.frameCount = framesOut;
                        activeTrack->releaseBuffer(&activeTrack->mSink);
                    }
                } else {
                    // FIXME could do a partial drop of framesOut
                    if (activeTrack->mFramesToDrop > 0) {
                        activeTrack->mFramesToDrop -= framesOut;
                        if (activeTrack->mFramesToDrop <= 0) {
                            activeTrack->clearSyncStartEvent();
                        }
                    } else {
                        activeTrack->mFramesToDrop += framesOut;
                        if (activeTrack->mFramesToDrop >= 0 || activeTrack->mSyncStartEvent == 0 ||
                                activeTrack->mSyncStartEvent->isCancelled()) {
                            ALOGW("Synced record %s, session %d, trigger session %d",
                                  (activeTrack->mFramesToDrop >= 0) ? "timed out" : "cancelled",
                                  activeTrack->sessionId(),
                                  (activeTrack->mSyncStartEvent != 0) ?
                                          activeTrack->mSyncStartEvent->triggerSession() : 0);
                            activeTrack->clearSyncStartEvent();
                        }
                    }
                }

                if (framesOut == 0) {
                    break;
                }
            }

            switch (overrun) {
            case OVERRUN_TRUE:
                // client isn't retrieving buffers fast enough
                if (!activeTrack->setOverflow()) {
                    nsecs_t now = systemTime();
                    // FIXME should lastWarning per track?
                    if ((now - lastWarning) > kWarningThrottleNs) {
                        ALOGW("RecordThread: buffer overflow");
                        lastWarning = now;
                    }
                }
                break;
            case OVERRUN_FALSE:
                activeTrack->clearOverflow();
                break;
            case OVERRUN_UNKNOWN:
                break;
            }

        }

unlock:
        // enable changes in effect chain
        unlockEffectChains(effectChains);
        // effectChains doesn't need to be cleared, since it is cleared by destructor at scope end
    }

    standbyIfNotAlreadyInStandby();

    {
        Mutex::Autolock _l(mLock);
        for (size_t i = 0; i < mTracks.size(); i++) {
            sp<RecordTrack> track = mTracks[i];
            track->invalidate();
        }
        mActiveTracks.clear();
        mActiveTracksGen++;
        mStartStopCond.broadcast();
    }

    releaseWakeLock();

    ALOGV("RecordThread %p exiting", this);
    return false;
}

void AudioFlinger::RecordThread::standbyIfNotAlreadyInStandby()
{
    if (!mStandby) {
        inputStandBy();
        mStandby = true;
    }
}

void AudioFlinger::RecordThread::inputStandBy()
{
    // Idle the fast capture if it's currently running
    if (mFastCapture != 0) {
        FastCaptureStateQueue *sq = mFastCapture->sq();
        FastCaptureState *state = sq->begin();
        if (!(state->mCommand & FastCaptureState::IDLE)) {
            state->mCommand = FastCaptureState::COLD_IDLE;
            state->mColdFutexAddr = &mFastCaptureFutex;
            state->mColdGen++;
            mFastCaptureFutex = 0;
            sq->end();
            // BLOCK_UNTIL_PUSHED would be insufficient, as we need it to stop doing I/O now
            sq->push(FastCaptureStateQueue::BLOCK_UNTIL_ACKED);
#if 0
            if (kUseFastCapture == FastCapture_Dynamic) {
                // FIXME
            }
#endif
#ifdef AUDIO_WATCHDOG
            // FIXME
#endif
        } else {
            sq->end(false /*didModify*/);
        }
    }
    mInput->stream->common.standby(&mInput->stream->common);
}

// RecordThread::createRecordTrack_l() must be called with AudioFlinger::mLock held
sp<AudioFlinger::RecordThread::RecordTrack> AudioFlinger::RecordThread::createRecordTrack_l(
        const sp<AudioFlinger::Client>& client,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *pFrameCount,
        int sessionId,
        size_t *notificationFrames,
        int uid,
        IAudioFlinger::track_flags_t *flags,
        pid_t tid,
        status_t *status)
{
    size_t frameCount = *pFrameCount;
    sp<RecordTrack> track;
    status_t lStatus;

    // client expresses a preference for FAST, but we get the final say
    if (*flags & IAudioFlinger::TRACK_FAST) {
      if (
            // use case: callback handler and frame count is default or at least as large as HAL
            (
                (tid != -1) &&
                ((frameCount == 0) /*||
                // FIXME must be equal to pipe depth, so don't allow it to be specified by client
                // FIXME not necessarily true, should be native frame count for native SR!
                (frameCount >= mFrameCount)*/)
            ) &&
            // PCM data
            audio_is_linear_pcm(format) &&
            // native format
            (format == mFormat) &&
            // mono or stereo
            ( (channelMask == AUDIO_CHANNEL_IN_MONO) ||
              (channelMask == AUDIO_CHANNEL_IN_STEREO) ) &&
            // native channel mask
            (channelMask == mChannelMask) &&
            // native hardware sample rate
            (sampleRate == mSampleRate) &&
            // record thread has an associated fast capture
            hasFastCapture() &&
            // there are sufficient fast track slots available
            mFastTrackAvail
        ) {
        // if frameCount not specified, then it defaults to pipe frame count
        if (frameCount == 0) {
            frameCount = mPipeFramesP2;
        }
        ALOGV("AUDIO_INPUT_FLAG_FAST accepted: frameCount=%d mFrameCount=%d",
                frameCount, mFrameCount);
      } else {
        ALOGV("AUDIO_INPUT_FLAG_FAST denied: frameCount=%d "
                "mFrameCount=%d format=%d isLinear=%d channelMask=%#x sampleRate=%u mSampleRate=%u "
                "hasFastCapture=%d tid=%d mFastTrackAvail=%d",
                frameCount, mFrameCount, format,
                audio_is_linear_pcm(format),
                channelMask, sampleRate, mSampleRate, hasFastCapture(), tid, mFastTrackAvail);
        *flags &= ~IAudioFlinger::TRACK_FAST;
        // FIXME It's not clear that we need to enforce this any more, since we have a pipe.
        // For compatibility with AudioRecord calculation, buffer depth is forced
        // to be at least 2 x the record thread frame count and cover audio hardware latency.
        // This is probably too conservative, but legacy application code may depend on it.
        // If you change this calculation, also review the start threshold which is related.
        // FIXME It's not clear how input latency actually matters.  Perhaps this should be 0.
        uint32_t latencyMs = 50; // FIXME mInput->stream->get_latency(mInput->stream);
        size_t mNormalFrameCount = 2048; // FIXME
        uint32_t minBufCount = latencyMs / ((1000 * mNormalFrameCount) / mSampleRate);
        if (minBufCount < 2) {
            minBufCount = 2;
        }
        size_t minFrameCount = mNormalFrameCount * minBufCount;
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
      }
    }
    *pFrameCount = frameCount;
    *notificationFrames = 0;    // FIXME implement

    lStatus = initCheck();
    if (lStatus != NO_ERROR) {
        ALOGE("createRecordTrack_l() audio driver not initialized");
        goto Exit;
    }

    { // scope for mLock
        Mutex::Autolock _l(mLock);

        track = new RecordTrack(this, client, sampleRate,
                      format, channelMask, frameCount, sessionId, uid,
                      *flags);

        lStatus = track->initCheck();
        if (lStatus != NO_ERROR) {
            ALOGE("createRecordTrack_l() initCheck failed %d; no control block?", lStatus);
            // track must be cleared from the caller as the caller has the AF lock
            goto Exit;
        }
        mTracks.add(track);

        // disable AEC and NS if the device is a BT SCO headset supporting those pre processings
        bool suspend = audio_is_bluetooth_sco_device(mInDevice) &&
                        mAudioFlinger->btNrecIsOff();
        setEffectSuspended_l(FX_IID_AEC, suspend, sessionId);
        setEffectSuspended_l(FX_IID_NS, suspend, sessionId);

        if ((*flags & IAudioFlinger::TRACK_FAST) && (tid != -1)) {
            pid_t callingPid = IPCThreadState::self()->getCallingPid();
            // we don't have CAP_SYS_NICE, nor do we want to have it as it's too powerful,
            // so ask activity manager to do this on our behalf
            sendPrioConfigEvent_l(callingPid, tid, kPriorityAudioApp);
        }
    }

    lStatus = NO_ERROR;

Exit:
    *status = lStatus;
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
        recordTrack->clearSyncStartEvent();
    } else if (event != AudioSystem::SYNC_EVENT_SAME) {
        recordTrack->mSyncStartEvent = mAudioFlinger->createSyncEvent(event,
                                       triggerSession,
                                       recordTrack->sessionId(),
                                       syncStartEventCallback,
                                       recordTrack);
        // Sync event can be cancelled by the trigger session if the track is not in a
        // compatible state in which case we start record immediately
        if (recordTrack->mSyncStartEvent->isCancelled()) {
            recordTrack->clearSyncStartEvent();
        } else {
            // do not wait for the event for more than AudioSystem::kSyncRecordStartTimeOutMs
            recordTrack->mFramesToDrop = -
                    ((AudioSystem::kSyncRecordStartTimeOutMs * recordTrack->mSampleRate) / 1000);
        }
    }

    {
        // This section is a rendezvous between binder thread executing start() and RecordThread
        AutoMutex lock(mLock);
        if (mActiveTracks.indexOf(recordTrack) >= 0) {
            if (recordTrack->mState == TrackBase::PAUSING) {
                ALOGV("active record track PAUSING -> ACTIVE");
                recordTrack->mState = TrackBase::ACTIVE;
            } else {
                ALOGV("active record track state %d", recordTrack->mState);
            }
            return status;
        }

        // TODO consider other ways of handling this, such as changing the state to :STARTING and
        //      adding the track to mActiveTracks after returning from AudioSystem::startInput(),
        //      or using a separate command thread
        recordTrack->mState = TrackBase::STARTING_1;
        mActiveTracks.add(recordTrack);
        mActiveTracksGen++;
        mLock.unlock();
        status_t status = AudioSystem::startInput(mId);
        mLock.lock();
        // FIXME should verify that recordTrack is still in mActiveTracks
        if (status != NO_ERROR) {
            mActiveTracks.remove(recordTrack);
            mActiveTracksGen++;
            recordTrack->clearSyncStartEvent();
            return status;
        }
        // Catch up with current buffer indices if thread is already running.
        // This is what makes a new client discard all buffered data.  If the track's mRsmpInFront
        // was initialized to some value closer to the thread's mRsmpInFront, then the track could
        // see previously buffered data before it called start(), but with greater risk of overrun.

        recordTrack->mRsmpInFront = mRsmpInRear;
        recordTrack->mRsmpInUnrel = 0;
        // FIXME why reset?
        if (recordTrack->mResampler != NULL) {
            recordTrack->mResampler->reset();
        }
        recordTrack->mState = TrackBase::STARTING_2;
        // signal thread to start
        mWaitWorkCV.broadcast();
        if (mActiveTracks.indexOf(recordTrack) < 0) {
            ALOGV("Record failed to start");
            status = BAD_VALUE;
            goto startError;
        }
        return status;
    }

startError:
    AudioSystem::stopInput(mId);
    recordTrack->clearSyncStartEvent();
    // FIXME I wonder why we do not reset the state here?
    return status;
}

void AudioFlinger::RecordThread::syncStartEventCallback(const wp<SyncEvent>& event)
{
    sp<SyncEvent> strongEvent = event.promote();

    if (strongEvent != 0) {
        sp<RefBase> ptr = strongEvent->cookie().promote();
        if (ptr != 0) {
            RecordTrack *recordTrack = (RecordTrack *)ptr.get();
            recordTrack->handleSyncStartEvent(strongEvent);
        }
    }
}

bool AudioFlinger::RecordThread::stop(RecordThread::RecordTrack* recordTrack) {
    ALOGV("RecordThread::stop");
    AutoMutex _l(mLock);
    if (mActiveTracks.indexOf(recordTrack) != 0 || recordTrack->mState == TrackBase::PAUSING) {
        return false;
    }
    // note that threadLoop may still be processing the track at this point [without lock]
    recordTrack->mState = TrackBase::PAUSING;
    // do not wait for mStartStopCond if exiting
    if (exitPending()) {
        return true;
    }
    // FIXME incorrect usage of wait: no explicit predicate or loop
    mStartStopCond.wait(mLock);
    // if we have been restarted, recordTrack is in mActiveTracks here
    if (exitPending() || mActiveTracks.indexOf(recordTrack) != 0) {
        ALOGV("Record stopped OK");
        return true;
    }
    return false;
}

bool AudioFlinger::RecordThread::isValidSyncEvent(const sp<SyncEvent>& event __unused) const
{
    return false;
}

status_t AudioFlinger::RecordThread::setSyncEvent(const sp<SyncEvent>& event __unused)
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
    track->terminate();
    track->mState = TrackBase::STOPPED;
    // active tracks are removed by threadLoop()
    if (mActiveTracks.indexOf(track) < 0) {
        removeTrack_l(track);
    }
}

void AudioFlinger::RecordThread::removeTrack_l(const sp<RecordTrack>& track)
{
    mTracks.remove(track);
    // need anything related to effects here?
    if (track->isFastTrack()) {
        ALOG_ASSERT(!mFastTrackAvail);
        mFastTrackAvail = true;
    }
}

void AudioFlinger::RecordThread::dump(int fd, const Vector<String16>& args)
{
    dumpInternals(fd, args);
    dumpTracks(fd, args);
    dumpEffectChains(fd, args);
}

void AudioFlinger::RecordThread::dumpInternals(int fd, const Vector<String16>& args)
{
    dprintf(fd, "\nInput thread %p:\n", this);

    if (mActiveTracks.size() > 0) {
        dprintf(fd, "  Buffer size: %zu bytes\n", mBufferSize);
    } else {
        dprintf(fd, "  No active record clients\n");
    }
    dprintf(fd, "  Fast track available: %s\n", mFastTrackAvail ? "yes" : "no");

    dumpBase(fd, args);
}

void AudioFlinger::RecordThread::dumpTracks(int fd, const Vector<String16>& args __unused)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    size_t numtracks = mTracks.size();
    size_t numactive = mActiveTracks.size();
    size_t numactiveseen = 0;
    dprintf(fd, "  %d Tracks", numtracks);
    if (numtracks) {
        dprintf(fd, " of which %d are active\n", numactive);
        RecordTrack::appendDumpHeader(result);
        for (size_t i = 0; i < numtracks ; ++i) {
            sp<RecordTrack> track = mTracks[i];
            if (track != 0) {
                bool active = mActiveTracks.indexOf(track) >= 0;
                if (active) {
                    numactiveseen++;
                }
                track->dump(buffer, SIZE, active);
                result.append(buffer);
            }
        }
    } else {
        dprintf(fd, "\n");
    }

    if (numactiveseen != numactive) {
        snprintf(buffer, SIZE, "  The following tracks are in the active list but"
                " not in the track list\n");
        result.append(buffer);
        RecordTrack::appendDumpHeader(result);
        for (size_t i = 0; i < numactive; ++i) {
            sp<RecordTrack> track = mActiveTracks[i];
            if (mTracks.indexOf(track) < 0) {
                track->dump(buffer, SIZE, true);
                result.append(buffer);
            }
        }

    }
    write(fd, result.string(), result.size());
}

// AudioBufferProvider interface
status_t AudioFlinger::RecordThread::ResamplerBufferProvider::getNextBuffer(
        AudioBufferProvider::Buffer* buffer, int64_t pts __unused)
{
    RecordTrack *activeTrack = mRecordTrack;
    sp<ThreadBase> threadBase = activeTrack->mThread.promote();
    if (threadBase == 0) {
        buffer->frameCount = 0;
        buffer->raw = NULL;
        return NOT_ENOUGH_DATA;
    }
    RecordThread *recordThread = (RecordThread *) threadBase.get();
    int32_t rear = recordThread->mRsmpInRear;
    int32_t front = activeTrack->mRsmpInFront;
    ssize_t filled = rear - front;
    // FIXME should not be P2 (don't want to increase latency)
    // FIXME if client not keeping up, discard
    LOG_ALWAYS_FATAL_IF(!(0 <= filled && (size_t) filled <= recordThread->mRsmpInFrames));
    // 'filled' may be non-contiguous, so return only the first contiguous chunk
    front &= recordThread->mRsmpInFramesP2 - 1;
    size_t part1 = recordThread->mRsmpInFramesP2 - front;
    if (part1 > (size_t) filled) {
        part1 = filled;
    }
    size_t ask = buffer->frameCount;
    ALOG_ASSERT(ask > 0);
    if (part1 > ask) {
        part1 = ask;
    }
    if (part1 == 0) {
        // Higher-level should keep mRsmpInBuffer full, and not call resampler if empty
        LOG_ALWAYS_FATAL("RecordThread::getNextBuffer() starved");
        buffer->raw = NULL;
        buffer->frameCount = 0;
        activeTrack->mRsmpInUnrel = 0;
        return NOT_ENOUGH_DATA;
    }

    buffer->raw = recordThread->mRsmpInBuffer + front * recordThread->mChannelCount;
    buffer->frameCount = part1;
    activeTrack->mRsmpInUnrel = part1;
    return NO_ERROR;
}

// AudioBufferProvider interface
void AudioFlinger::RecordThread::ResamplerBufferProvider::releaseBuffer(
        AudioBufferProvider::Buffer* buffer)
{
    RecordTrack *activeTrack = mRecordTrack;
    size_t stepCount = buffer->frameCount;
    if (stepCount == 0) {
        return;
    }
    ALOG_ASSERT(stepCount <= activeTrack->mRsmpInUnrel);
    activeTrack->mRsmpInUnrel -= stepCount;
    activeTrack->mRsmpInFront += stepCount;
    buffer->raw = NULL;
    buffer->frameCount = 0;
}

bool AudioFlinger::RecordThread::checkForNewParameter_l(const String8& keyValuePair,
                                                        status_t& status)
{
    bool reconfig = false;

    status = NO_ERROR;

    audio_format_t reqFormat = mFormat;
    uint32_t samplingRate = mSampleRate;
    audio_channel_mask_t channelMask = audio_channel_in_mask_from_count(mChannelCount);

    AudioParameter param = AudioParameter(keyValuePair);
    int value;
    // TODO Investigate when this code runs. Check with audio policy when a sample rate and
    //      channel count change can be requested. Do we mandate the first client defines the
    //      HAL sampling rate and channel count or do we allow changes on the fly?
    if (param.getInt(String8(AudioParameter::keySamplingRate), value) == NO_ERROR) {
        samplingRate = value;
        reconfig = true;
    }
    if (param.getInt(String8(AudioParameter::keyFormat), value) == NO_ERROR) {
        if ((audio_format_t) value != AUDIO_FORMAT_PCM_16_BIT) {
            status = BAD_VALUE;
        } else {
            reqFormat = (audio_format_t) value;
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyChannels), value) == NO_ERROR) {
        audio_channel_mask_t mask = (audio_channel_mask_t) value;
        if (mask != AUDIO_CHANNEL_IN_MONO && mask != AUDIO_CHANNEL_IN_STEREO) {
            status = BAD_VALUE;
        } else {
            channelMask = mask;
            reconfig = true;
        }
    }
    if (param.getInt(String8(AudioParameter::keyFrameCount), value) == NO_ERROR) {
        // do not accept frame count changes if tracks are open as the track buffer
        // size depends on frame count and correct behavior would not be guaranteed
        // if frame count is changed after track creation
        if (mActiveTracks.size() > 0) {
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
                        <= (2 * samplingRate)) &&
                audio_channel_count_from_in_mask(
                        mInput->stream->common.get_channels(&mInput->stream->common)) <= FCC_2 &&
                (channelMask == AUDIO_CHANNEL_IN_MONO ||
                        channelMask == AUDIO_CHANNEL_IN_STEREO)) {
                status = NO_ERROR;
            }
            if (status == NO_ERROR) {
                readInputParameters_l();
                sendIoConfigEvent_l(AudioSystem::INPUT_CONFIG_CHANGED);
            }
        }
    }

    return reconfig;
}

String8 AudioFlinger::RecordThread::getParameters(const String8& keys)
{
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return String8();
    }

    char *s = mInput->stream->common.get_parameters(&mInput->stream->common, keys.string());
    const String8 out_s8(s);
    free(s);
    return out_s8;
}

void AudioFlinger::RecordThread::audioConfigChanged(int event, int param __unused) {
    AudioSystem::OutputDescriptor desc;
    const void *param2 = NULL;

    switch (event) {
    case AudioSystem::INPUT_OPENED:
    case AudioSystem::INPUT_CONFIG_CHANGED:
        desc.channelMask = mChannelMask;
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
    mAudioFlinger->audioConfigChanged(event, mId, param2);
}

void AudioFlinger::RecordThread::readInputParameters_l()
{
    mSampleRate = mInput->stream->common.get_sample_rate(&mInput->stream->common);
    mChannelMask = mInput->stream->common.get_channels(&mInput->stream->common);
    mChannelCount = audio_channel_count_from_in_mask(mChannelMask);
    mFormat = mInput->stream->common.get_format(&mInput->stream->common);
    if (mFormat != AUDIO_FORMAT_PCM_16_BIT) {
        ALOGE("HAL format %#x not supported; must be AUDIO_FORMAT_PCM_16_BIT", mFormat);
    }
    mFrameSize = audio_stream_frame_size(&mInput->stream->common);
    mBufferSize = mInput->stream->common.get_buffer_size(&mInput->stream->common);
    mFrameCount = mBufferSize / mFrameSize;
    // This is the formula for calculating the temporary buffer size.
    // With 7 HAL buffers, we can guarantee ability to down-sample the input by ratio of 6:1 to
    // 1 full output buffer, regardless of the alignment of the available input.
    // The value is somewhat arbitrary, and could probably be even larger.
    // A larger value should allow more old data to be read after a track calls start(),
    // without increasing latency.
    mRsmpInFrames = mFrameCount * 7;
    mRsmpInFramesP2 = roundup(mRsmpInFrames);
    delete[] mRsmpInBuffer;
    // Over-allocate beyond mRsmpInFramesP2 to permit a HAL read past end of buffer
    mRsmpInBuffer = new int16_t[(mRsmpInFramesP2 + mFrameCount - 1) * mChannelCount];

    // AudioRecord mSampleRate and mChannelCount are constant due to AudioRecord API constraints.
    // But if thread's mSampleRate or mChannelCount changes, how will that affect active tracks?
}

uint32_t AudioFlinger::RecordThread::getInputFramesLost()
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

status_t AudioFlinger::RecordThread::createAudioPatch_l(const struct audio_patch *patch,
                                                          audio_patch_handle_t *handle)
{
    status_t status = NO_ERROR;
    if (mInput->audioHwDev->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        // store new device and send to effects
        mInDevice = patch->sources[0].ext.device.type;
        for (size_t i = 0; i < mEffectChains.size(); i++) {
            mEffectChains[i]->setDevice_l(mInDevice);
        }

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

        // store new source and send to effects
        if (mAudioSource != patch->sinks[0].ext.mix.usecase.source) {
            mAudioSource = patch->sinks[0].ext.mix.usecase.source;
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setAudioSource_l(mAudioSource);
            }
        }

        audio_hw_device_t *hwDevice = mInput->audioHwDev->hwDevice();
        status = hwDevice->create_audio_patch(hwDevice,
                                               patch->num_sources,
                                               patch->sources,
                                               patch->num_sinks,
                                               patch->sinks,
                                               handle);
    } else {
        ALOG_ASSERT(false, "createAudioPatch_l() called on a pre 3.0 HAL");
    }
    return status;
}

status_t AudioFlinger::RecordThread::releaseAudioPatch_l(const audio_patch_handle_t handle)
{
    status_t status = NO_ERROR;
    if (mInput->audioHwDev->version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        audio_hw_device_t *hwDevice = mInput->audioHwDev->hwDevice();
        status = hwDevice->release_audio_patch(hwDevice, handle);
    } else {
        ALOG_ASSERT(false, "releaseAudioPatch_l() called on a pre 3.0 HAL");
    }
    return status;
}


}; // namespace android
