/*
**
** Copyright 2007, The Android Open Source Project
** Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
**
** Not a Contribution, Apache license notifications and license are retained
** for attribution purposes only.
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

#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include <utils/Atomic.h>

#include <cutils/bitops.h>
#include <cutils/properties.h>
#include <cutils/compiler.h>

#undef ADD_BATTERY_DATA

#ifdef ADD_BATTERY_DATA
#include <media/IMediaPlayerService.h>
#include <media/IMediaDeathNotifier.h>
#endif

#include <private/media/AudioTrackShared.h>
#include <private/media/AudioEffectShared.h>

#include <system/audio.h>
#include <hardware/audio.h>

#include "AudioMixer.h"
#include "AudioFlinger.h"
#include "ServiceUtilities.h"

#include <media/EffectsFactoryApi.h>
#include <audio_effects/effect_visualizer.h>
#include <audio_effects/effect_ns.h>
#include <audio_effects/effect_aec.h>

#include <audio_utils/primitives.h>

#include <powermanager/PowerManager.h>

// #define DEBUG_CPU_USAGE 10  // log statistics every n wall clock seconds
#ifdef DEBUG_CPU_USAGE
#include <cpustats/CentralTendencyStatistics.h>
#include <cpustats/ThreadCpuUsage.h>
#endif

#include <common_time/cc_helper.h>
#include <common_time/local_clock.h>

#include "FastMixer.h"

// NBAIO implementations
#include <media/nbaio/AudioStreamOutSink.h>
#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>
#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/nbaio/SourceAudioBufferProvider.h>

#include "SchedulingPolicyService.h"
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

static const char kDeadlockedString[] = "AudioFlinger may be deadlocked\n";
static const char kHardwareLockedString[] = "Hardware lock is taken\n";

static const float MAX_GAIN = 4096.0f;
static const uint32_t MAX_GAIN_INT = 0x1000;

// retry counts for buffer fill timeout
// 50 * ~20msecs = 1 second
static const int8_t kMaxTrackRetries = 50;
static const int8_t kMaxTrackStartupRetries = 50;
// allow less retry attempts on direct output thread.
// direct outputs can be a scarce resource in audio hardware and should
// be released as quickly as possible.
static const int8_t kMaxTrackRetriesDirect = 5;

static const int kDumpLockRetries = 50;
static const int kDumpLockSleepUs = 20000;

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

nsecs_t AudioFlinger::mStandbyTimeInNsecs = kDefaultStandbyTimeInNsecs;

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

static uint32_t gScreenState; // incremented by 2 when screen state changes, bit 0 == 1 means "off"
                              // AudioFlinger::setParameters() updates, other threads read w/o lock

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

static int load_audio_interface(const char *if_name, audio_hw_device_t **dev)
{
    const hw_module_t *mod;
    int rc;

    rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, if_name, &mod);
    ALOGE_IF(rc, "%s couldn't load audio hw module %s.%s (%s)", __func__,
                 AUDIO_HARDWARE_MODULE_ID, if_name, strerror(-rc));
    if (rc) {
        goto out;
    }
    rc = audio_hw_device_open(mod, dev);
    ALOGE_IF(rc, "%s couldn't open audio hw device in %s.%s (%s)", __func__,
                 AUDIO_HARDWARE_MODULE_ID, if_name, strerror(-rc));
    if (rc) {
        goto out;
    }
#if !defined(ICS_AUDIO_BLOB) && !defined(MR0_AUDIO_BLOB)
    if ((*dev)->common.version != AUDIO_DEVICE_API_VERSION_CURRENT) {
        ALOGE("%s wrong audio hw device version %04x", __func__, (*dev)->common.version);
        rc = BAD_VALUE;
        goto out;
    }
#endif
    return 0;

out:
    *dev = NULL;
    return rc;
}

static uint32_t getInputChannelCount(uint32_t channels) {
#ifdef QCOM_HARDWARE
   // only mono or stereo and 5.1 are supported for input sources
   return popcount((channels) & (AUDIO_CHANNEL_IN_STEREO | AUDIO_CHANNEL_IN_MONO | AUDIO_CHANNEL_IN_5POINT1));
#else
   return popcount(channels);
#endif
}
// ----------------------------------------------------------------------------

AudioFlinger::AudioFlinger()
    : BnAudioFlinger(),
      mPrimaryHardwareDev(NULL),
      mHardwareStatus(AUDIO_HW_IDLE),
      mMasterVolume(1.0f),
      mMasterMute(false),
      mNextUniqueId(1),
      mMode(AUDIO_MODE_INVALID),
      mBtNrecIsOff(false)
#ifdef QCOM_HARDWARE
      ,mAllChainsLocked(false)
#endif
{
}

void AudioFlinger::onFirstRef()
{
    int rc = 0;
#ifdef QCOM_HARDWARE
    mA2DPHandle = -1;
#endif

    Mutex::Autolock _l(mLock);

    /* TODO: move all this work into an Init() function */
#ifdef QCOM_HARDWARE
    mLPASessionId = -2; // -2 is invalid session ID
    mIsEffectConfigChanged = false;
    mLPAEffectChain = NULL;
#endif
    char val_str[PROPERTY_VALUE_MAX] = { 0 };
    if (property_get("ro.audio.flinger_standbytime_ms", val_str, NULL) >= 0) {
        uint32_t int_val;
        if (1 == sscanf(val_str, "%u", &int_val)) {
            mStandbyTimeInNsecs = milliseconds(int_val);
            ALOGI("Using %u mSec as standby time.", int_val);
        } else {
            mStandbyTimeInNsecs = kDefaultStandbyTimeInNsecs;
            ALOGI("Using default %u mSec as standby time.",
                    (uint32_t)(mStandbyTimeInNsecs / 1000000));
        }
    }

    mMode = AUDIO_MODE_NORMAL;
}

AudioFlinger::~AudioFlinger()
{
    while (!mRecordThreads.isEmpty()) {
        // closeInput_nonvirtual() will remove specified entry from mRecordThreads
        closeInput_nonvirtual(mRecordThreads.keyAt(0));
    }
    while (!mPlaybackThreads.isEmpty()) {
        // closeOutput_nonvirtual() will remove specified entry from mPlaybackThreads
        closeOutput_nonvirtual(mPlaybackThreads.keyAt(0));
    }

    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        // no mHardwareLock needed, as there are no other references to this
        audio_hw_device_close(mAudioHwDevs.valueAt(i)->hwDevice());
        delete mAudioHwDevs.valueAt(i);
    }
}

static const char * const audio_interfaces[] = {
    AUDIO_HARDWARE_MODULE_ID_PRIMARY,
    AUDIO_HARDWARE_MODULE_ID_A2DP,
    AUDIO_HARDWARE_MODULE_ID_USB,
};
#define ARRAY_SIZE(x) (sizeof((x))/sizeof(((x)[0])))

AudioFlinger::AudioHwDevice* AudioFlinger::findSuitableHwDev_l(
        audio_module_handle_t module,
        audio_devices_t devices)
{
    // if module is 0, the request comes from an old policy manager and we should load
    // well known modules
    if (module == 0) {
        ALOGW("findSuitableHwDev_l() loading well know audio hw modules");
        for (size_t i = 0; i < ARRAY_SIZE(audio_interfaces); i++) {
            loadHwModule_l(audio_interfaces[i]);
        }
        // then try to find a module supporting the requested device.
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            AudioHwDevice *audioHwDevice = mAudioHwDevs.valueAt(i);
            audio_hw_device_t *dev = audioHwDevice->hwDevice();
            if ((dev->get_supported_devices != NULL) &&
                    (dev->get_supported_devices(dev) & devices) == devices)
                return audioHwDevice;
#ifdef ICS_AUDIO_BLOB
            else if (dev->get_supported_devices == NULL && i != 0 &&
                    devices == 0x80)
                // Reasonably safe assumption: A non-primary HAL without
                // get_supported_devices is a locally-built A2DP binary
                return audioHwDevice;
#endif
        }
    } else {
        // check a match for the requested module handle
        AudioHwDevice *audioHwDevice = mAudioHwDevs.valueFor(module);
        if (audioHwDevice != NULL) {
            return audioHwDevice;
        }
    }

    return NULL;
}

void AudioFlinger::dumpClients(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.append("Clients:\n");
    for (size_t i = 0; i < mClients.size(); ++i) {
        sp<Client> client = mClients.valueAt(i).promote();
        if (client != 0) {
            snprintf(buffer, SIZE, "  pid: %d\n", client->pid());
            result.append(buffer);
        }
    }

    result.append("Global session refs:\n");
    result.append(" session pid count\n");
    for (size_t i = 0; i < mAudioSessionRefs.size(); i++) {
        AudioSessionRef *r = mAudioSessionRefs[i];
        snprintf(buffer, SIZE, " %7d %3d %3d\n", r->mSessionid, r->mPid, r->mCnt);
        result.append(buffer);
    }
    write(fd, result.string(), result.size());
}


void AudioFlinger::dumpInternals(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    hardware_call_state hardwareStatus = mHardwareStatus;

    snprintf(buffer, SIZE, "Hardware status: %d\n"
                           "Standby Time mSec: %u\n",
                            hardwareStatus,
                            (uint32_t)(mStandbyTimeInNsecs / 1000000));
    result.append(buffer);
    write(fd, result.string(), result.size());
}

void AudioFlinger::dumpPermissionDenial(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, "Permission Denial: "
            "can't dump AudioFlinger from pid=%d, uid=%d\n",
            IPCThreadState::self()->getCallingPid(),
            IPCThreadState::self()->getCallingUid());
    result.append(buffer);
    write(fd, result.string(), result.size());
}

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleepUs);
    }
    return locked;
}

status_t AudioFlinger::dump(int fd, const Vector<String16>& args)
{
    if (!dumpAllowed()) {
        dumpPermissionDenial(fd, args);
    } else {
        // get state of hardware lock
        bool hardwareLocked = tryLock(mHardwareLock);
        if (!hardwareLocked) {
            String8 result(kHardwareLockedString);
            write(fd, result.string(), result.size());
        } else {
            mHardwareLock.unlock();
        }

        bool locked = tryLock(mLock);

        // failed to lock - AudioFlinger is probably deadlocked
        if (!locked) {
            String8 result(kDeadlockedString);
            write(fd, result.string(), result.size());
        }

        dumpClients(fd, args);
        dumpInternals(fd, args);

        // dump playback threads
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->dump(fd, args);
        }

        // dump record threads
        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            mRecordThreads.valueAt(i)->dump(fd, args);
        }

        // dump all hardware devs
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            audio_hw_device_t *dev = mAudioHwDevs.valueAt(i)->hwDevice();
            dev->dump(dev, fd);
        }
        if (locked) mLock.unlock();
    }
    return NO_ERROR;
}

sp<AudioFlinger::Client> AudioFlinger::registerPid_l(pid_t pid)
{
    // If pid is already in the mClients wp<> map, then use that entry
    // (for which promote() is always != 0), otherwise create a new entry and Client.
    sp<Client> client = mClients.valueFor(pid).promote();
    if (client == 0) {
        client = new Client(this, pid);
        mClients.add(pid, client);
    }

    return client;
}

// IAudioFlinger interface


sp<IAudioTrack> AudioFlinger::createTrack(
        pid_t pid,
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCount,
        IAudioFlinger::track_flags_t flags,
        const sp<IMemory>& sharedBuffer,
        audio_io_handle_t output,
        pid_t tid,
        int *sessionId,
        status_t *status)
{
    sp<PlaybackThread::Track> track;
    sp<TrackHandle> trackHandle;
    sp<Client> client;
    status_t lStatus;
    int lSessionId;

    // client AudioTrack::set already implements AUDIO_STREAM_DEFAULT => AUDIO_STREAM_MUSIC,
    // but if someone uses binder directly they could bypass that and cause us to crash
    if (uint32_t(streamType) >= AUDIO_STREAM_CNT) {
        ALOGE("createTrack() invalid stream type %d", streamType);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    {
        Mutex::Autolock _l(mLock);
        PlaybackThread *thread = checkPlaybackThread_l(output);
        PlaybackThread *effectThread = NULL;
        if (thread == NULL) {
            ALOGE("unknown output thread");
            lStatus = BAD_VALUE;
            goto Exit;
        }

        client = registerPid_l(pid);

        ALOGV("createTrack() sessionId: %d", (sessionId == NULL) ? -2 : *sessionId);
        if (sessionId != NULL && *sessionId != AUDIO_SESSION_OUTPUT_MIX) {
            // check if an effect chain with the same session ID is present on another
            // output thread and move it here.
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                sp<PlaybackThread> t = mPlaybackThreads.valueAt(i);
                if (mPlaybackThreads.keyAt(i) != output) {
                    uint32_t sessions = t->hasAudioSession(*sessionId);
                    if (sessions & PlaybackThread::EFFECT_SESSION) {
                        effectThread = t.get();
                        break;
                    }
                }
            }
            lSessionId = *sessionId;
        } else {
            // if no audio session id is provided, create one here
            lSessionId = nextUniqueId();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
        }
        ALOGV("createTrack() lSessionId: %d", lSessionId);

        track = thread->createTrack_l(client, streamType, sampleRate, format,
                channelMask, frameCount, sharedBuffer, lSessionId, flags, tid, &lStatus);

        // move effect chain to this output thread if an effect on same session was waiting
        // for a track to be created
        if (lStatus == NO_ERROR && effectThread != NULL) {
            Mutex::Autolock _dl(thread->mLock);
            Mutex::Autolock _sl(effectThread->mLock);
            moveEffectChain_l(lSessionId, effectThread, thread, true);
        }

        // Look for sync events awaiting for a session to be used.
        for (int i = 0; i < (int)mPendingSyncEvents.size(); i++) {
            if (mPendingSyncEvents[i]->triggerSession() == lSessionId) {
                if (thread->isValidSyncEvent(mPendingSyncEvents[i])) {
                    if (lStatus == NO_ERROR) {
                        (void) track->setSyncEvent(mPendingSyncEvents[i]);
                    } else {
                        mPendingSyncEvents[i]->cancel();
                    }
                    mPendingSyncEvents.removeAt(i);
                    i--;
                }
            }
        }
    }
    if (lStatus == NO_ERROR) {
        trackHandle = new TrackHandle(track);
    } else {
        // remove local strong reference to Client before deleting the Track so that the Client
        // destructor is called by the TrackBase destructor with mLock held
        client.clear();
        track.clear();
    }

Exit:
    if (status != NULL) {
        *status = lStatus;
    }
    return trackHandle;
}

#ifdef QCOM_HARDWARE
sp<IDirectTrack> AudioFlinger::createDirectTrack(
        pid_t pid,
        uint32_t sampleRate,
        audio_channel_mask_t channelMask,
        audio_io_handle_t output,
        int *sessionId,
        IDirectTrackClient *client,
        audio_stream_type_t streamType,
        status_t *status)
{
    *status = NO_ERROR;
    status_t lStatus = NO_ERROR;
    sp<IDirectTrack> track = NULL;
    DirectAudioTrack* directTrack = NULL;
    Mutex::Autolock _l(mLock);

    ALOGV("createDirectTrack() sessionId: %d sampleRate %d channelMask %d",
         *sessionId, sampleRate, channelMask);
    AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(output);
    if(desc == NULL) {
        ALOGE("Error: Invalid output (%d) to create direct audio track", output);
        lStatus = BAD_VALUE;
        goto Exit;
    }
    desc->mStreamType = streamType;
    if (desc->flag & AUDIO_OUTPUT_FLAG_LPA) {
        if (sessionId != NULL && *sessionId != AUDIO_SESSION_OUTPUT_MIX) {
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                sp<PlaybackThread> t = mPlaybackThreads.valueAt(i);
                // Check if the session ID is already associated with a track
                uint32_t sessions = t->hasAudioSession(*sessionId);

                // check if an effect with same session ID is waiting for a ssession to be created
                ALOGV("check if an effect with same session ID is waiting for a ssession to be created");
                if ((mLPAEffectChain == NULL) && (sessions & PlaybackThread::EFFECT_SESSION)) {
                    // Clear reference to previous effect chain if any
                    t->mLock.lock();
                    ALOGV("getting the LPA effect chain and setting LPA flag to true.");
                    mLPAEffectChain = t->getEffectChain_l(*sessionId);
                    t->mLock.unlock();
                }
            }
            mLPASessionId = *sessionId;
            if (mLPAEffectChain != NULL) {
                mLPAEffectChain->setLPAFlag(true);
                // For LPA, the volume will be applied in DSP. No need for volume
                // control in the Effect chain, so setting it to unity.
                uint32_t volume = 0x1000000; // Equals to 1.0 in 8.24 format
                mLPAEffectChain->setVolume_l(&volume,&volume);
            } else {
                ALOGW("There was no effectChain created for the sessionId(%d)", mLPASessionId);
            }
            mLPASampleRate  = sampleRate;
            mLPANumChannels = popcount(channelMask);
        } else {
            if(sessionId != NULL) {
                ALOGE("Error: Invalid sessionID (%d) for direct audio track", *sessionId);
            }
        }
    }
    mLock.unlock();
    directTrack = new DirectAudioTrack(this, output, desc, client, desc->flag);
    desc->trackRefPtr = dynamic_cast<void *>(directTrack);
    mLock.lock();
    if (directTrack != 0) {
        track = dynamic_cast<IDirectTrack *>(directTrack);
        AudioEventObserver* obv = dynamic_cast<AudioEventObserver *>(directTrack);
        ALOGE("setting observer mOutputDesc track %p, obv %p", track.get(), obv);
        desc->stream->set_observer(desc->stream, reinterpret_cast<void *>(obv));
    } else {
        lStatus = BAD_VALUE;
    }
Exit:
    if(lStatus) {
        if (track != NULL) {
            track.clear();
        }
        *status = lStatus;
    }
    return track;
}

void AudioFlinger::deleteEffectSession()
{
    ALOGV("deleteSession");
    // -2 is invalid session ID
    mLPASessionId = -2;
    if (mLPAEffectChain != NULL) {
        mLPAEffectChain->lock();
        mLPAEffectChain->setLPAFlag(false);
        size_t i, numEffects = mLPAEffectChain->getNumEffects();
        for(i = 0; i < numEffects; i++) {
            sp<EffectModule> effect = mLPAEffectChain->getEffectFromIndex_l(i);
            effect->setInBuffer(mLPAEffectChain->inBuffer());
            if (i == numEffects-1) {
                effect->setOutBuffer(mLPAEffectChain->outBuffer());
            } else {
                effect->setOutBuffer(mLPAEffectChain->inBuffer());
            }
            effect->configure();
        }
        mLPAEffectChain->unlock();
        mLPAEffectChain.clear();
        mLPAEffectChain = NULL;
    }
}

// ToDo: Should we go ahead with this frameCount?
#define DEAFULT_FRAME_COUNT 1200
bool AudioFlinger::applyEffectsOn(void *token, int16_t *inBuffer,
                            int16_t *outBuffer, int size, bool force)
{
    ALOGV("applyEffectsOn: inBuf %p outBuf %p size %d token %p", inBuffer, outBuffer, size, token);
    // This might be the first buffer to apply effects after effect config change
    // should not skip effects processing
    mIsEffectConfigChanged = false;

    volatile size_t numEffects = 0;

#ifdef SRS_PROCESSING
    POSTPRO_PATCH_ICS_OUTPROC_DIRECT_SAMPLES(token, AUDIO_FORMAT_PCM_16_BIT, outBuffer, size,
        mLPASampleRate, mLPANumChannels);
#endif

    if(mLPAEffectChain != NULL) {
        numEffects = mLPAEffectChain->getNumEffects();
    }

    if( numEffects > 0) {
        size_t  i     = 0;
        int16_t *pIn  = inBuffer;
        int16_t *pOut = outBuffer;

        int frameCount = size / (sizeof(int16_t) * mLPANumChannels);

        while(frameCount > 0) {
            if(mLPAEffectChain == NULL) {
                ALOGV("LPA Effect Chain is removed - No effects processing !!");
                numEffects = 0;
                break;
            }
            mLPAEffectChain->lock();

            numEffects = mLPAEffectChain->getNumEffects();
            if(!numEffects) {
                ALOGV("applyEffectsOn: All the effects are removed - nothing to process");
                mLPAEffectChain->unlock();
                break;
            }

            int outFrameCount = (frameCount > DEAFULT_FRAME_COUNT ? DEAFULT_FRAME_COUNT: frameCount);
            bool isEffectEnabled = false;
            for(i = 0; i < numEffects; i++) {
                // If effect configuration is changed while applying effects do not process further

                if(mIsEffectConfigChanged && !force) {
                    mLPAEffectChain->unlock();
                    ALOGV("applyEffectsOn: mIsEffectConfigChanged is set - no further processing %d",frameCount);
                    return false;
                }
                sp<EffectModule> effect = mLPAEffectChain->getEffectFromIndex_l(i);
                if(effect == NULL) {
                    ALOGE("getEffectFromIndex_l(%d) returned NULL ptr", i);
                    mLPAEffectChain->unlock();
                    return false;
                }
                if(i == 0) {
                    // For the first set input and output buffers different
                    isEffectEnabled = effect->isProcessEnabled();
                    effect->setInBuffer(pIn);
                    effect->setOutBuffer(pOut);
                } else {
                    // For the remaining use previous effect's output buffer as input buffer
                    effect->setInBuffer(pOut);
                    effect->setOutBuffer(pOut);
                }
                // true indicates that it is being applied on LPA output
                effect->configure(true, mLPASampleRate, mLPANumChannels, outFrameCount);
            }

            if(isEffectEnabled) {
                // Clear the output buffer
                memset(pOut, 0, (outFrameCount * mLPANumChannels * sizeof(int16_t)));
            } else {
                // Copy input buffer content to the output buffer
                memcpy(pOut, pIn, (outFrameCount * mLPANumChannels * sizeof(int16_t)));
            }

            mLPAEffectChain->process_l();

            mLPAEffectChain->unlock();

            // Update input and output buffer pointers
            pIn        += (outFrameCount * mLPANumChannels);
            pOut       += (outFrameCount * mLPANumChannels);
            frameCount -= outFrameCount;
        }
    }

    if (!numEffects && !force) {
        ALOGV("applyEffectsOn: There are no effects to be applied");
        if(inBuffer != outBuffer) {
            // No effect applied so just copy input buffer to output buffer
            memcpy(outBuffer, inBuffer, size);
        }
    }
    return true;
}
#endif

uint32_t AudioFlinger::sampleRate(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
#ifdef QCOM_HARDWARE
    if (!mDirectAudioTracks.isEmpty()) {
        AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(output);
        if(desc != NULL) {
            return desc->stream->common.get_sample_rate(&desc->stream->common);
        }
    }
#endif
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("sampleRate() unknown thread %d", output);
        return 0;
    }
    return thread->sampleRate();
}

int AudioFlinger::channelCount(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
#ifdef QCOM_HARDWARE
    AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(output);
    if(desc != NULL) {
        return desc->stream->common.get_channels(&desc->stream->common);
    }
#endif
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("channelCount() unknown thread %d", output);
        return 0;
    }
    return thread->channelCount();
}

audio_format_t AudioFlinger::format(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("format() unknown thread %d", output);
        return AUDIO_FORMAT_INVALID;
    }
    return thread->format();
}

size_t AudioFlinger::frameCount(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
#ifdef QCOM_HARDWARE
    AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(output);
    if(desc != NULL) {
        return desc->stream->common.get_buffer_size(&desc->stream->common);
    }
#endif
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("frameCount() unknown thread %d", output);
        return 0;
    }
    // FIXME currently returns the normal mixer's frame count to avoid confusing legacy callers;
    //       should examine all callers and fix them to handle smaller counts
    return thread->frameCount();
}

uint32_t AudioFlinger::latency(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);
    if (thread == NULL) {
        ALOGW("latency() unknown thread %d", output);
        return 0;
    }
    return thread->latency();
}

status_t AudioFlinger::setMasterVolume(float value)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

#ifdef QCOM_HARDWARE
    mA2DPHandle = -1;
#endif
    Mutex::Autolock _l(mLock);
    mMasterVolume = value;

    // Set master volume in the HALs which support it.
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        AutoMutex lock(mHardwareLock);
        AudioHwDevice *dev = mAudioHwDevs.valueAt(i);

        mHardwareStatus = AUDIO_HW_SET_MASTER_VOLUME;
        if (dev->canSetMasterVolume()) {
            dev->hwDevice()->set_master_volume(dev->hwDevice(), value);
        }
        mHardwareStatus = AUDIO_HW_IDLE;
    }

    // Now set the master volume in each playback thread.  Playback threads
    // assigned to HALs which do not have master volume support will apply
    // master volume during the mix operation.  Threads with HALs which do
    // support master volume will simply ignore the setting.
    for (size_t i = 0; i < mPlaybackThreads.size(); i++)
        mPlaybackThreads.valueAt(i)->setMasterVolume(value);

    return NO_ERROR;
}

status_t AudioFlinger::setMode(audio_mode_t mode)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }
    if (uint32_t(mode) >= AUDIO_MODE_CNT) {
        ALOGW("Illegal value: setMode(%d)", mode);
        return BAD_VALUE;
    }

    { // scope for the lock
        AutoMutex lock(mHardwareLock);
        audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
        mHardwareStatus = AUDIO_HW_SET_MODE;
        ret = dev->set_mode(dev, mode);
        mHardwareStatus = AUDIO_HW_IDLE;
    }

    if (NO_ERROR == ret) {
        Mutex::Autolock _l(mLock);
        mMode = mode;
        for (size_t i = 0; i < mPlaybackThreads.size(); i++)
            mPlaybackThreads.valueAt(i)->setMode(mode);
    }

    return ret;
}

status_t AudioFlinger::setMicMute(bool state)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mHardwareLock);
    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
    mHardwareStatus = AUDIO_HW_SET_MIC_MUTE;
    ret = dev->set_mic_mute(dev, state);
    mHardwareStatus = AUDIO_HW_IDLE;
    return ret;
}

bool AudioFlinger::getMicMute() const
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return false;
    }

    bool state = AUDIO_MODE_INVALID;
    AutoMutex lock(mHardwareLock);
    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
    mHardwareStatus = AUDIO_HW_GET_MIC_MUTE;
    dev->get_mic_mute(dev, &state);
    mHardwareStatus = AUDIO_HW_IDLE;
    return state;
}

status_t AudioFlinger::setMasterMute(bool muted)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    Mutex::Autolock _l(mLock);
    mMasterMute = muted;

    // Set master mute in the HALs which support it.
#ifndef ICS_AUDIO_BLOB
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        AutoMutex lock(mHardwareLock);
        AudioHwDevice *dev = mAudioHwDevs.valueAt(i);

        mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
        if (dev->canSetMasterMute()) {
            dev->hwDevice()->set_master_mute(dev->hwDevice(), muted);
        }
        mHardwareStatus = AUDIO_HW_IDLE;
    }
#endif

    // Now set the master mute in each playback thread.  Playback threads
    // assigned to HALs which do not have master mute support will apply master
    // mute during the mix operation.  Threads with HALs which do support master
    // mute will simply ignore the setting.
    for (size_t i = 0; i < mPlaybackThreads.size(); i++)
        mPlaybackThreads.valueAt(i)->setMasterMute(muted);

    return NO_ERROR;
}

float AudioFlinger::masterVolume() const
{
    Mutex::Autolock _l(mLock);
    return masterVolume_l();
}

bool AudioFlinger::masterMute() const
{
    Mutex::Autolock _l(mLock);
    return masterMute_l();
}

float AudioFlinger::masterVolume_l() const
{
    return mMasterVolume;
}

bool AudioFlinger::masterMute_l() const
{
    return mMasterMute;
}

status_t AudioFlinger::setStreamVolume(audio_stream_type_t stream, float value,
        audio_io_handle_t output)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        ALOGE("setStreamVolume() invalid stream %d", stream);
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
#ifdef QCOM_HARDWARE
    ALOGV("setStreamVolume stream %d, output %d, value %f",stream, output, value);
    AudioSessionDescriptor *desc = NULL;
    if (!mDirectAudioTracks.isEmpty()) {
        desc = mDirectAudioTracks.valueFor(output);
        if (desc != NULL) {
            ALOGV("setStreamVolume for mAudioTracks size %d desc %p",mDirectAudioTracks.size(),desc);
            if (desc->mStreamType == stream) {
                mStreamTypes[stream].volume = value;
                desc->mVolumeScale = value;
                desc->stream->set_volume(desc->stream,
                                         desc->mVolumeLeft * mStreamTypes[stream].volume,
                                         desc->mVolumeRight* mStreamTypes[stream].volume);
                return NO_ERROR;
            }
        }
    }
#endif
    PlaybackThread *thread = NULL;
    if (output) {
        thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
#ifdef QCOM_HARDWARE
            if (desc != NULL) {
                return NO_ERROR;
            }
#endif
            return BAD_VALUE;
        }

    }

    mStreamTypes[stream].volume = value;

    if (thread == NULL) {
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->setStreamVolume(stream, value);
        }
    } else {
        thread->setStreamVolume(stream, value);
    }

    return NO_ERROR;
}

status_t AudioFlinger::setStreamMute(audio_stream_type_t stream, bool muted)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    if (uint32_t(stream) >= AUDIO_STREAM_CNT ||
        uint32_t(stream) == AUDIO_STREAM_ENFORCED_AUDIBLE) {
        ALOGE("setStreamMute() invalid stream %d", stream);
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mStreamTypes[stream].mute = muted;
    for (uint32_t i = 0; i < mPlaybackThreads.size(); i++)
        mPlaybackThreads.valueAt(i)->setStreamMute(stream, muted);

    return NO_ERROR;
}

float AudioFlinger::streamVolume(audio_stream_type_t stream, audio_io_handle_t output) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return 0.0f;
    }

    AutoMutex lock(mLock);
    float volume;
    if (output) {
        PlaybackThread *thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            return 0.0f;
        }
        volume = thread->streamVolume(stream);
    } else {
        volume = streamVolume_l(stream);
    }

    return volume;
}

bool AudioFlinger::streamMute(audio_stream_type_t stream) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        return true;
    }

    AutoMutex lock(mLock);
    return streamMute_l(stream);
}

status_t AudioFlinger::setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs)
{
    ALOGV("setParameters(): io %d, keyvalue %s, tid %d, calling pid %d",
            ioHandle, keyValuePairs.string(), gettid(), IPCThreadState::self()->getCallingPid());
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    // ioHandle == 0 means the parameters are global to the audio hardware interface
    if (ioHandle == 0) {
        Mutex::Autolock _l(mLock);
#ifdef SRS_PROCESSING
        POSTPRO_PATCH_ICS_PARAMS_SET(keyValuePairs);
        if (!mDirectAudioTracks.isEmpty())
            audioConfigChanged_l(AudioSystem::EFFECT_CONFIG_CHANGED, 0, NULL);
#endif
        status_t final_result = NO_ERROR;
        {
            AutoMutex lock(mHardwareLock);
            mHardwareStatus = AUDIO_HW_SET_PARAMETER;
            for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
                audio_hw_device_t *dev = mAudioHwDevs.valueAt(i)->hwDevice();
                status_t result = dev->set_parameters(dev, keyValuePairs.string());
                final_result = result ?: final_result;
            }
            mHardwareStatus = AUDIO_HW_IDLE;
        }
#ifdef QCOM_HARDWARE
        AudioParameter param = AudioParameter(keyValuePairs);
        String8 value, key;
        int i = 0;

        key = String8(AudioParameter::keyADSPStatus);
        if (param.get(key, value) == NO_ERROR) {
            ALOGV("Set keyADSPStatus:%s", value.string());
            if (value == "ONLINE" || value == "OFFLINE") {
               if (!mDirectAudioTracks.isEmpty()) {
                   for (i=0; i < mDirectAudioTracks.size(); i++) {
                       mDirectAudioTracks.valueAt(i)->stream->common.set_parameters(
                          &mDirectAudioTracks.valueAt(i)->stream->common, keyValuePairs.string());
                   }
               }
           }
        }
#else
        // disable AEC and NS if the device is a BT SCO headset supporting those pre processings
        AudioParameter param = AudioParameter(keyValuePairs);
        String8 value;
#endif

        if (param.get(String8(AUDIO_PARAMETER_KEY_BT_NREC), value) == NO_ERROR) {
            bool btNrecIsOff = (value == AUDIO_PARAMETER_VALUE_OFF);
            if (mBtNrecIsOff != btNrecIsOff) {
                for (size_t i = 0; i < mRecordThreads.size(); i++) {
                    sp<RecordThread> thread = mRecordThreads.valueAt(i);
                    audio_devices_t device = thread->inDevice();
                    bool suspend = audio_is_bluetooth_sco_device(device) && btNrecIsOff;
                    // collect all of the thread's session IDs
                    KeyedVector<int, bool> ids = thread->sessionIds();
                    // suspend effects associated with those session IDs
                    for (size_t j = 0; j < ids.size(); ++j) {
                        int sessionId = ids.keyAt(j);
                        thread->setEffectSuspended(FX_IID_AEC,
                                                   suspend,
                                                   sessionId);
                        thread->setEffectSuspended(FX_IID_NS,
                                                   suspend,
                                                   sessionId);
                    }
                }
                mBtNrecIsOff = btNrecIsOff;
            }
        }
        String8 screenState;
        if (param.get(String8(AudioParameter::keyScreenState), screenState) == NO_ERROR) {
            bool isOff = screenState == "off";
            if (isOff != (gScreenState & 1)) {
                gScreenState = ((gScreenState & ~1) + 2) | isOff;
            }
        }
        return final_result;
    }

#ifdef QCOM_HARDWARE
    AudioSessionDescriptor *desc = NULL;
    if (!mDirectAudioTracks.isEmpty()) {
        desc = mDirectAudioTracks.valueFor(ioHandle);
        if (desc != NULL) {
            ALOGV("setParameters for mAudioTracks size %d desc %p",mDirectAudioTracks.size(),desc);
            desc->stream->common.set_parameters(&desc->stream->common, keyValuePairs.string());
            AudioParameter param = AudioParameter(keyValuePairs);
            String8 key = String8(AudioParameter::keyRouting);
            int device;
            if (param.getInt(key, device) == NO_ERROR) {

#ifdef SRS_PROCESSING
                ALOGV("setParameters:: routing change to device %d", device);
                desc->device = (audio_devices_t)device;
                POSTPRO_PATCH_ICS_OUTPROC_MIX_ROUTE(desc->trackRefPtr, param, device);
#endif
                if(mLPAEffectChain != NULL){
                    mLPAEffectChain->setDevice_l(device);
                    audioConfigChanged_l(AudioSystem::EFFECT_CONFIG_CHANGED, 0, NULL);
                }
            }
        }
    }
#endif

    // hold a strong ref on thread in case closeOutput() or closeInput() is called
    // and the thread is exited once the lock is released
    sp<ThreadBase> thread;
    {
        Mutex::Autolock _l(mLock);
        thread = checkPlaybackThread_l(ioHandle);
        if (thread == 0) {
            thread = checkRecordThread_l(ioHandle);
        } else if (thread == primaryPlaybackThread_l()) {
            // indicate output device change to all input threads for pre processing
            AudioParameter param = AudioParameter(keyValuePairs);
            int value;
            DefaultKeyedVector< int, sp<RecordThread> >    recordThreads = mRecordThreads;
            mLock.unlock();
            if ((param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) &&
                    (value != 0)) {
                for (size_t i = 0; i < recordThreads.size(); i++) {
                    recordThreads.valueAt(i)->setParameters(keyValuePairs);
                }
            }
            mLock.lock();
        }
        mLock.unlock();
    }
    if (thread != 0) {
        return thread->setParameters(keyValuePairs);
    }
    return BAD_VALUE;
}

String8 AudioFlinger::getParameters(audio_io_handle_t ioHandle, const String8& keys) const
{
//    ALOGV("getParameters() io %d, keys %s, tid %d, calling pid %d",
//            ioHandle, keys.string(), gettid(), IPCThreadState::self()->getCallingPid());

    Mutex::Autolock _l(mLock);

    if (ioHandle == 0) {
        String8 out_s8;

#ifdef SRS_PROCESSING
        POSTPRO_PATCH_ICS_PARAMS_GET(keys, out_s8);
#endif
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            char *s;
            {
            AutoMutex lock(mHardwareLock);
            mHardwareStatus = AUDIO_HW_GET_PARAMETER;
            audio_hw_device_t *dev = mAudioHwDevs.valueAt(i)->hwDevice();
            s = dev->get_parameters(dev, keys.string());
            mHardwareStatus = AUDIO_HW_IDLE;
            }
            out_s8 += String8(s ? s : "");
            free(s);
        }
        return out_s8;
    }

    PlaybackThread *playbackThread = checkPlaybackThread_l(ioHandle);
    if (playbackThread != NULL) {
        return playbackThread->getParameters(keys);
    }
    RecordThread *recordThread = checkRecordThread_l(ioHandle);
    if (recordThread != NULL) {
        return recordThread->getParameters(keys);
    }
    return String8("");
}

size_t AudioFlinger::getInputBufferSize(uint32_t sampleRate, audio_format_t format,
        audio_channel_mask_t channelMask) const
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return 0;
    }

    AutoMutex lock(mHardwareLock);
    mHardwareStatus = AUDIO_HW_GET_INPUT_BUFFER_SIZE;
    struct audio_config config = {
        sample_rate: sampleRate,
        channel_mask: channelMask,
        format: format,
    };
    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
#ifndef ICS_AUDIO_BLOB
    size_t size = dev->get_input_buffer_size(dev, &config);
#else
    size_t size = dev->get_input_buffer_size(dev, sampleRate, format, popcount(channelMask));
#endif
    mHardwareStatus = AUDIO_HW_IDLE;
    return size;
}

unsigned int AudioFlinger::getInputFramesLost(audio_io_handle_t ioHandle) const
{
    Mutex::Autolock _l(mLock);

    RecordThread *recordThread = checkRecordThread_l(ioHandle);
    if (recordThread != NULL) {
        return recordThread->getInputFramesLost();
    }
    return 0;
}

status_t AudioFlinger::setVoiceVolume(float value)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mHardwareLock);
    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
    mHardwareStatus = AUDIO_HW_SET_VOICE_VOLUME;
    ret = dev->set_voice_volume(dev, value);
    mHardwareStatus = AUDIO_HW_IDLE;

    return ret;
}

status_t AudioFlinger::getRenderPosition(uint32_t *halFrames, uint32_t *dspFrames,
        audio_io_handle_t output) const
{
    status_t status;

    Mutex::Autolock _l(mLock);

    PlaybackThread *playbackThread = checkPlaybackThread_l(output);
    if (playbackThread != NULL) {
        return playbackThread->getRenderPosition(halFrames, dspFrames);
    }

    return BAD_VALUE;
}

#ifdef QCOM_FM_ENABLED
status_t AudioFlinger::setFmVolume(float value)
{
    status_t ret = initCheck();
    if (ret != NO_ERROR) {
        return ret;
    }

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mHardwareLock);
    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
    mHardwareStatus = AUDIO_SET_FM_VOLUME;
    ret = dev->set_fm_volume(dev, value);
    mHardwareStatus = AUDIO_HW_IDLE;

    return ret;
}
#endif

void AudioFlinger::registerClient(const sp<IAudioFlingerClient>& client)
{

    Mutex::Autolock _l(mLock);

    sp<IBinder> binder = client->asBinder();
    if (mNotificationClients.indexOfKey(binder) < 0) {
        sp<NotificationClient> notificationClient = new NotificationClient(this,
                                                                            client,
                                                                            binder);
        ALOGV("registerClient() client %p, binder %d", notificationClient.get(), binder.get());

        mNotificationClients.add(binder, notificationClient);

        sp<IBinder> binder = client->asBinder();
        binder->linkToDeath(notificationClient);

        // the config change is always sent from playback or record threads to avoid deadlock
        // with AudioSystem::gLock
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->sendIoConfigEvent(AudioSystem::OUTPUT_OPENED);
        }

        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            mRecordThreads.valueAt(i)->sendIoConfigEvent(AudioSystem::INPUT_OPENED);
        }
    }
#ifdef QCOM_HARDWARE
    // Send the notification to the client only once.
    if (mA2DPHandle != -1) {
        ALOGV("A2DP active. Notifying the registered client");
        client->ioConfigChanged(AudioSystem::A2DP_OUTPUT_STATE, mA2DPHandle, &mA2DPHandle);
    }
#endif
}

#ifdef QCOM_HARDWARE
status_t AudioFlinger::deregisterClient(const sp<IAudioFlingerClient>& client)
{
    ALOGV("deregisterClient() %p, tid %d, calling tid %d", client.get(), gettid(), IPCThreadState::self()->getCallingPid());
    Mutex::Autolock _l(mLock);

    sp<IBinder> binder = client->asBinder();
    int index = mNotificationClients.indexOfKey(binder);
    if (index >= 0) {
        mNotificationClients.removeItemsAt(index);
        return true;
    }

    return false;
}
#endif

void AudioFlinger::removeNotificationClient(sp<IBinder> binder)
{
    Mutex::Autolock _l(mLock);

    mNotificationClients.removeItem(binder);

    int pid = IPCThreadState::self()->getCallingPid();
    ALOGV("%d died, releasing its sessions", pid);
    size_t num = mAudioSessionRefs.size();
    bool removed = false;
    for (size_t i = 0; i< num; ) {
        AudioSessionRef *ref = mAudioSessionRefs.itemAt(i);
        ALOGV(" pid %d @ %d", ref->mPid, i);
        if (ref->mPid == pid) {
            ALOGV(" removing entry for pid %d session %d", pid, ref->mSessionid);
            mAudioSessionRefs.removeAt(i);
            delete ref;
            removed = true;
            num--;
        } else {
            i++;
        }
    }
    if (removed) {
        purgeStaleEffects_l();
    }
}

// audioConfigChanged_l() must be called with AudioFlinger::mLock held
void AudioFlinger::audioConfigChanged_l(int event, audio_io_handle_t ioHandle, const void *param2)
{
#ifdef QCOM_HARDWARE
    ALOGV("AudioFlinger::audioConfigChanged_l: event %d", event);
    if (event == AudioSystem::EFFECT_CONFIG_CHANGED) {
        mIsEffectConfigChanged = true;
    }
#endif
    size_t size = mNotificationClients.size();
    for (size_t i = 0; i < size; i++) {
        mNotificationClients.valueAt(i)->audioFlingerClient()->ioConfigChanged(event, ioHandle,
                                                                               param2);
    }
}

// removeClient_l() must be called with AudioFlinger::mLock held
void AudioFlinger::removeClient_l(pid_t pid)
{
    ALOGV("removeClient_l() pid %d, tid %d, calling tid %d", pid, gettid(), IPCThreadState::self()->getCallingPid());
    mClients.removeItem(pid);
}

// getEffectThread_l() must be called with AudioFlinger::mLock held
sp<AudioFlinger::PlaybackThread> AudioFlinger::getEffectThread_l(int sessionId, int EffectId)
{
    sp<PlaybackThread> thread;

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        if (mPlaybackThreads.valueAt(i)->getEffect(sessionId, EffectId) != 0) {
            ALOG_ASSERT(thread == 0);
            thread = mPlaybackThreads.valueAt(i);
        }
    }

    return thread;
}

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
    ALOGV("sendIoConfigEvent() num events %d event %d, param %d", mConfigEvents.size(), event, param);
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
                int err = requestPriority(prioEvent->pid(), prioEvent->tid(), prioEvent->prio());
                if (err != 0) {
                    ALOGW("Policy SCHED_FIFO priority %d is unavailable for pid %d tid %d; error %d",
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

    bool locked = tryLock(mLock);
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
    snprintf(buffer, SIZE, "Sample rate: %d\n", mSampleRate);
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
        mScreenState(gScreenState),
        // index 0 is reserved for normal mixer's submix
        mFastTrackAvailMask(((1 << FastMixerState::kMaxFastTracks) - 1) & ~1)
{
    snprintf(mName, kNameLength, "AudioOut_%X", id);

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
    snprintf(buffer, SIZE, "last write occurred (msecs): %llu\n", ns2ms(systemTime() - mLastWriteTime));
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
        int frameCount,
        const sp<IMemory>& sharedBuffer,
        int sessionId,
        IAudioFlinger::track_flags_t flags,
        pid_t tid,
        status_t *status)
{
    sp<Track> track;
    status_t lStatus;

    bool isTimed = (flags & IAudioFlinger::TRACK_TIMED) != 0;

    // client expresses a preference for FAST, but we get the final say
    if (flags & IAudioFlinger::TRACK_FAST) {
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
                (frameCount >= (int) (mFrameCount * kFastTrackMultiplier)))
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
                "mFrameCount=%d format=%d isLinear=%d channelMask=%#x sampleRate=%d mSampleRate=%d "
                "hasFastMixer=%d tid=%d fastTrackAvailMask=%#x",
                isTimed, sharedBuffer.get(), frameCount, mFrameCount, format,
                audio_is_linear_pcm(format),
                channelMask, sampleRate, mSampleRate, hasFastMixer(), tid, mFastTrackAvailMask);
        flags &= ~IAudioFlinger::TRACK_FAST;
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
        int minFrameCount = mNormalFrameCount * minBufCount;
        if (frameCount < minFrameCount) {
            frameCount = minFrameCount;
        }
      }
    }

    if (mType == DIRECT) {
#ifdef QCOM_ENHANCED_AUDIO
        if (((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM)
              ||((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AMR_NB)
              ||((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AMR_WB)
              ||((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_EVRC)
              ||((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_EVRCB)
              ||((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_EVRCWB))
#else
        if ((format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM)
#endif
        {
            if (sampleRate != mSampleRate || format != mFormat || channelMask != mChannelMask) {
                ALOGE("createTrack_l() Bad parameter: sampleRate %d format %d, channelMask 0x%08x \""
                        "for output %p with format %d",
                        sampleRate, format, channelMask, mOutput, mFormat);
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }
    } else {
        // Resampler implementation limits input sampling rate to 2 x output sampling rate.
        if (sampleRate > mSampleRate*2) {
            ALOGE("Sample rate out of range: %d mSampleRate %d", sampleRate, mSampleRate);
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
                    channelMask, frameCount, sharedBuffer, sessionId, flags);
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

        if ((flags & IAudioFlinger::TRACK_FAST) && (tid != -1)) {
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

uint32_t AudioFlinger::MixerThread::correctLatency(uint32_t latency) const
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

uint32_t AudioFlinger::PlaybackThread::correctLatency(uint32_t latency) const
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
        return correctLatency(mOutput->stream->get_latency(mOutput->stream));
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
        if (track->mainBuffer() != mMixBuffer) {
            sp<EffectChain> chain = getEffectChain_l(track->sessionId());
            if (chain != 0) {
                ALOGV("addTrack_l() starting track on chain %p for session %d", chain.get(), track->sessionId());
                chain->incActiveTrackCnt();
            }
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

    ALOGV("PlaybackThread::audioConfigChanged_l, thread %p, event %d, param %d", this, event, param);

    switch (event) {
    case AudioSystem::OUTPUT_OPENED:
    case AudioSystem::OUTPUT_CONFIG_CHANGED:
        desc.channels = mChannelMask;
        desc.samplingRate = mSampleRate;
        desc.format = mFormat;
        desc.frameCount = mNormalFrameCount; // FIXME see AudioFlinger::frameCount(audio_io_handle_t)
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
    if (mType == MIXER && (kUseFastMixer == FastMixer_Static || kUseFastMixer == FastMixer_Dynamic)) {
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
            // prefer an even multiplier, for compatibility with doubling of fast tracks due to HAL SRC
            // (it would be unusual for the normal mix buffer size to not be a multiple of fast
            // track, but we sometimes have to do this to satisfy the maximum frame count constraint)
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
    ALOGI("HAL output buffer size %u frames, normal mix buffer size %u frames", mFrameCount, mNormalFrameCount);

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


status_t AudioFlinger::PlaybackThread::getRenderPosition(uint32_t *halFrames, uint32_t *dspFrames)
{
    if (halFrames == NULL || dspFrames == NULL) {
        return BAD_VALUE;
    }
    Mutex::Autolock _l(mLock);
    if (initCheck() != NO_ERROR) {
        return INVALID_OPERATION;
    }
    *halFrames = mBytesWritten / audio_stream_frame_size(&mOutput->stream->common);

    if (isSuspended()) {
        // return an estimation of rendered frames when the output is suspended
        int32_t frames = mBytesWritten - latency_l();
        if (frames < 0) {
            frames = 0;
        }
        *dspFrames = (uint32_t)frames;
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
        if (sessionId == track->sessionId() &&
                !(track->mCblk->flags & CBLK_INVALID_MSK)) {
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
        if (sessionId == track->sessionId() &&
                !(track->mCblk->flags & CBLK_INVALID_MSK)) {
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

void AudioFlinger::PlaybackThread::threadLoop_removeTracks(const Vector< sp<Track> >& tracksToRemove)
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
    ALOGV("mSampleRate=%d, mChannelMask=%#x, mChannelCount=%d, mFormat=%d, mFrameSize=%d, "
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

#ifdef TEE_SINK_FRAMES
        // create a Pipe to archive a copy of FastMixer's output for dumpsys
        Pipe *teeSink = new Pipe(TEE_SINK_FRAMES, format);
        numCounterOffers = 0;
        index = teeSink->negotiate(offers, 1, NULL, numCounterOffers);
        ALOG_ASSERT(index == 0);
        mTeeSink = teeSink;
        PipeReader *teeSource = new PipeReader(*teeSink);
        numCounterOffers = 0;
        index = teeSource->negotiate(offers, 1, NULL, numCounterOffers);
        ALOG_ASSERT(index == 0);
        mTeeSource = teeSource;
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
        state->mTeeSink = mTeeSink.get();
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
    delete mAudioMixer;
}

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

    while (!exitPending())
    {
        cpuStats.sample(myName);

        Vector< sp<EffectChain> > effectChains;

        processConfigEvents();

        { // scope for mLock

            Mutex::Autolock _l(mLock);

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

                    if (exitPending()) break;

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
#ifdef QCOM_HARDWARE
                if (effectChains[i] != mAudioFlinger->mLPAEffectChain)
#endif
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
#if defined(ATRACE_TAG) && (ATRACE_TAG != ATRACE_TAG_NEVER)
                    ScopedTrace st(ATRACE_TAG, "underrun");
#endif
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
#if defined(ATRACE_TAG) && (ATRACE_TAG != ATRACE_TAG_NEVER)
        Tracer::traceBegin(ATRACE_TAG, "write");
#endif
        // update the setpoint when gScreenState changes
        uint32_t screenState = gScreenState;
        if (screenState != mScreenState) {
            mScreenState = screenState;
            MonoPipe *pipe = (MonoPipe *)mPipeSink.get();
            if (pipe != NULL) {
                pipe->setAvgFrames((mScreenState & 1) ?
                        (pipe->maxFrames() * 7) / 8 : mNormalFrameCount * 2);
            }
        }
        ssize_t framesWritten = mNormalSink->write(mMixBuffer, count);
#if defined(ATRACE_TAG) && (ATRACE_TAG != ATRACE_TAG_NEVER)
        Tracer::traceEnd(ATRACE_TAG);
#endif
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

    if (bytesWritten > 0) mBytesWritten += mixBufferSize;
    mNumWrites++;
    mInWrite = false;
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
        ALOGV_IF((mBytesWritten == 0 && (mMixerStatus == MIXER_TRACKS_ENABLED)), "anticipated start");
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
        if (t == 0) continue;

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
                    android_atomic_or(CBLK_DISABLED_ON, &track->mCblk->flags);
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
                    size_t framesWritten =
                            mBytesWritten / audio_stream_frame_size(&mOutput->stream->common);
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
                track->mCachedVolume = track->isMuted() ?
                        0 : masterVolume * mStreamTypes[track->streamType()].volume;
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
            if (t->sampleRate() == (int)mSampleRate) {
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
                ALOG_ASSERT(minFrames <= cblk->frameCount);
            }
        }
        if ((track->framesReady() >= minFrames) && track->isReady() &&
                !track->isPaused() && !track->isTerminated())
        {
            //ALOGV("track %d u=%08x, s=%08x [OK] on thread %p", name, cblk->user, cblk->server, this);

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
                    ALOGW("prepareTracks_l(): track %d attached to effect but no chain found on session %d",
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
            if (track->isMuted() || track->isPausing() ||
                mStreamTypes[track->streamType()].mute) {
                vl = vr = va = 0;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {

                // read original volumes with volume control
                float typeVolume = mStreamTypes[track->streamType()].volume;
                float v = masterVolume * typeVolume;
                uint32_t vlr = cblk->getVolumeLR();
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

                uint16_t sendLevel = cblk->getSendLevel_U4_12();
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
            if (vl > MAX_GAIN_INT) vl = MAX_GAIN_INT;
            vr = (vr + (1 << 11)) >> 12;
            if (vr > MAX_GAIN_INT) vr = MAX_GAIN_INT;

            if (va > MAX_GAIN_INT) va = MAX_GAIN_INT;   // va is uint32_t, so no need to check for -

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
            mAudioMixer->setParameter(
                name,
                AudioMixer::RESAMPLE,
                AudioMixer::SAMPLE_RATE,
                (void *)(cblk->sampleRate));
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

            //ALOGV("track %d u=%08x, s=%08x [NOT READY] on thread %p", name, cblk->user, cblk->server, this);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                // TODO: use actual buffer filling status instead of latency when available from
                // audio HAL
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                size_t framesWritten =
                        mBytesWritten / audio_stream_frame_size(&mOutput->stream->common);
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
                    ALOGV("BUFFER TIMEOUT: remove(%d) from active list on thread %p", name, this);
                    tracksToRemove->add(track);
                    // indicate to client process that the track was disabled because of underrun;
                    // it will then automatically call start() when data is available
                    android_atomic_or(CBLK_DISABLED_ON, &cblk->flags);
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
        sq->end();
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
        if (t == 0) continue;
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
                    ALOGV("stopping track on chain %p for session Id: %d", chain.get(), track->sessionId());
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
    if ((mixedTracks != 0 && mixedTracks == tracksWithEffect) || (mixedTracks == 0 && fastTracks > 0)) {
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
            android_atomic_or(CBLK_INVALID_ON, &t->mCblk->flags);
            t->mCblk->cv.signal();
        }
    }
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
            mOutDevice = value;
            for (size_t i = 0; i < mEffectChains.size(); i++) {
                mEffectChains[i]->setDevice_l(mOutDevice);
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
                    if (name < 0) break;
                    mTracks[i]->mName = name;
                    // limit track sample rate to 2 x new output sample rate
                    if (mTracks[i]->mCblk->sampleRate > 2 * sampleRate()) {
                        mTracks[i]->mCblk->sampleRate = 2 * sampleRate();
                    }
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

    // Write the tee output to a .wav file
    NBAIO_Source *teeSource = mTeeSource.get();
    if (teeSource != NULL) {
        char teePath[64];
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        strftime(teePath, sizeof(teePath), "/data/misc/media/%T.wav", &tm);
        int teeFd = open(teePath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
        if (teeFd >= 0) {
            char wavHeader[44];
            memcpy(wavHeader,
                "RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\2\0\104\254\0\0\0\0\0\0\4\0\20\0data\0\0\0\0",
                sizeof(wavHeader));
            NBAIO_Format format = teeSource->format();
            unsigned channelCount = Format_channelCount(format);
            ALOG_ASSERT(channelCount <= FCC_2);
            unsigned sampleRate = Format_sampleRate(format);
            wavHeader[22] = channelCount;       // number of channels
            wavHeader[24] = sampleRate;         // sample rate
            wavHeader[25] = sampleRate >> 8;
            wavHeader[32] = channelCount * 2;   // block alignment
            write(teeFd, wavHeader, sizeof(wavHeader));
            size_t total = 0;
            bool firstRead = true;
            for (;;) {
#define TEE_SINK_READ 1024
                short buffer[TEE_SINK_READ * FCC_2];
                size_t count = TEE_SINK_READ;
                ssize_t actual = teeSource->read(buffer, count,
                        AudioBufferProvider::kInvalidPTS);
                bool wasFirstRead = firstRead;
                firstRead = false;
                if (actual <= 0) {
                    if (actual == (ssize_t) OVERRUN && wasFirstRead) {
                        continue;
                    }
                    break;
                }
                ALOG_ASSERT(actual <= (ssize_t)count);
                write(teeFd, buffer, actual * channelCount * sizeof(short));
                total += actual;
            }
            lseek(teeFd, (off_t) 4, SEEK_SET);
            uint32_t temp = 44 + total * channelCount * sizeof(short) - 8;
            write(teeFd, &temp, sizeof(temp));
            lseek(teeFd, (off_t) 40, SEEK_SET);
            temp =  total * channelCount * sizeof(short);
            write(teeFd, &temp, sizeof(temp));
            close(teeFd);
            fdprintf(fd, "FastMixer tee copied to %s\n", teePath);
        } else {
            fdprintf(fd, "FastMixer unable to create tee %s: \n", strerror(errno));
        }
    }

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
    sp<Track> trackToRemove;

    mixer_state mixerStatus = MIXER_IDLE;

    // find out which tracks need to be processed
    if (mActiveTracks.size() != 0) {
        sp<Track> t = mActiveTracks[0].promote();
        // The track died recently
        if (t == 0) return MIXER_IDLE;

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
            //ALOGV("track %d u=%08x, s=%08x [OK]", track->name(), cblk->user, cblk->server);

            if (track->mFillingUpStatus == Track::FS_FILLED) {
                track->mFillingUpStatus = Track::FS_ACTIVE;
                mLeftVolFloat = mRightVolFloat = 0;
                if (track->mState == TrackBase::RESUMING) {
                    track->mState = TrackBase::ACTIVE;
                }
            }

            // compute volume for this track
            float left, right;
            if (track->isMuted() || mMasterMute || track->isPausing() ||
                mStreamTypes[track->streamType()].mute) {
                left = right = 0;
                if (track->isPausing()) {
                    track->setPaused();
                }
            } else {
                float typeVolume = mStreamTypes[track->streamType()].volume;
                float v = mMasterVolume * typeVolume;
                uint32_t vlr = cblk->getVolumeLR();
                float v_clamped = v * (vlr & 0xFFFF);
                if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
                left = v_clamped/MAX_GAIN;
                v_clamped = v * (vlr >> 16);
                if (v_clamped > MAX_GAIN) v_clamped = MAX_GAIN;
                right = v_clamped/MAX_GAIN;
            }

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
        } else {
            // clear effect chain input buffer if an active track underruns to avoid sending
            // previous audio buffer again to effects
            if (!mEffectChains.isEmpty()) {
                mEffectChains[0]->clearInputBuffer();
            }

            //ALOGV("track %d u=%08x, s=%08x [NOT READY]", track->name(), cblk->user, cblk->server);
            if ((track->sharedBuffer() != 0) || track->isTerminated() ||
                    track->isStopped() || track->isPaused()) {
                // We have consumed all the buffers of this track.
                // Remove it from the list of active tracks.
                // TODO: implement behavior for compressed audio
                size_t audioHALFrames = (latency_l() * mSampleRate) / 1000;
                size_t framesWritten =
                        mBytesWritten / audio_stream_frame_size(&mOutput->stream->common);
                if (mStandby || track->presentationComplete(framesWritten, audioHALFrames)) {
                    if (track->isStopped()) {
                        track->reset();
                    }
                    trackToRemove = track;
                }
            } else {
                // No buffers for this track. Give it a few chances to
                // fill a buffer, then remove it from active list.
                if (--(track->mRetryCount) <= 0) {
                    ALOGV("BUFFER TIMEOUT: remove(%d) from active list", track->name());
                    trackToRemove = track;
                } else {
                    mixerStatus = MIXER_TRACKS_ENABLED;
                }
            }
        }
    }

    // FIXME merge this with similar code for removing multiple tracks
    // remove all the tracks that need to be...
    if (CC_UNLIKELY(trackToRemove != 0)) {
        tracksToRemove->add(trackToRemove);
        mActiveTracks.remove(trackToRemove);
        if (!mEffectChains.isEmpty()) {
            ALOGV("stopping track on chain %p for session Id: %d", mEffectChains[0].get(),
                    trackToRemove->sessionId());
            mEffectChains[0]->decActiveTrackCnt();
        }
        if (trackToRemove->isTerminated()) {
            removeTrack_l(trackToRemove);
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
    :   MixerThread(audioFlinger, mainThread->getOutput(), id, mainThread->outDevice(), DUPLICATING),
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
        frameCount = (3 * mNormalFrameCount * mSampleRate) / sampleRate;
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


bool AudioFlinger::DuplicatingThread::outputsReady(const SortedVector< sp<OutputTrack> > &outputTracks)
{
    for (size_t i = 0; i < outputTracks.size(); i++) {
        sp<ThreadBase> thread = outputTracks[i]->thread().promote();
        if (thread == 0) {
            ALOGW("DuplicatingThread::outputsReady() could not promote thread on output track %p", outputTracks[i].get());
            return false;
        }
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        // see note at standby() declaration
        if (playbackThread->standby() && !playbackThread->isSuspended()) {
            ALOGV("DuplicatingThread output track %p on thread %p Not Ready", outputTracks[i].get(), thread.get());
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

// TrackBase constructor must be called with AudioFlinger::mLock held
AudioFlinger::ThreadBase::TrackBase::TrackBase(
            ThreadBase *thread,
            const sp<Client>& client,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            int frameCount,
#ifdef QCOM_ENHANCED_AUDIO
            uint32_t flags,
#endif
            const sp<IMemory>& sharedBuffer,
            int sessionId)
    :   RefBase(),
        mThread(thread),
        mClient(client),
        mCblk(NULL),
        // mBuffer
        // mBufferEnd
        mFrameCount(0),
        mState(IDLE),
        mSampleRate(sampleRate),
        mFormat(format),
        mStepServerFailed(false),
#ifdef QCOM_ENHANCED_AUDIO
        mFlags(0),
#endif
        mSessionId(sessionId)
        // mChannelCount
        // mChannelMask
{
    ALOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(), sharedBuffer->size());

    // ALOGD("Creating track with %d buffers @ %d bytes", bufferCount, bufferSize);
    size_t size = sizeof(audio_track_cblk_t);
    uint8_t channelCount = popcount(channelMask);
#ifdef QCOM_ENHANCED_AUDIO
    size_t bufferSize = 0;
    if ((int16_t)flags == 0x1) {
       bufferSize = frameCount*channelCount*sizeof(int16_t);
    } else {
       if ( (format == AUDIO_FORMAT_PCM_16_BIT) ||
            (format == AUDIO_FORMAT_PCM_8_BIT))
       {
          bufferSize = frameCount*channelCount*sizeof(int16_t);
       }
       else if (format == AUDIO_FORMAT_AMR_NB)
       {
          bufferSize = frameCount*channelCount*32; // full rate frame size
       }
       else if (format == AUDIO_FORMAT_EVRC)
       {
          bufferSize = frameCount*channelCount*23; // full rate frame size
       }
       else if (format == AUDIO_FORMAT_QCELP)
       {
          bufferSize = frameCount*channelCount*35; // full rate frame size
       }
       else if (format == AUDIO_FORMAT_AAC)
       {
          bufferSize = frameCount*2048; // full rate frame size
       }
       else if (format == AUDIO_FORMAT_AMR_WB)
       {
          bufferSize = frameCount*channelCount*61; // full rate frame size
       }
    }
#else
    size_t bufferSize = frameCount*channelCount*sizeof(int16_t);
#endif
    if (sharedBuffer == 0) {
        size += bufferSize;
    }

    if (client != NULL) {
        mCblkMemory = client->heap()->allocate(size);
        if (mCblkMemory != 0) {
            mCblk = static_cast<audio_track_cblk_t *>(mCblkMemory->pointer());
            if (mCblk != NULL) { // construct the shared structure in-place.
                new(mCblk) audio_track_cblk_t();
                // clear all buffers
                mCblk->frameCount = frameCount;
                mCblk->sampleRate = sampleRate;
// uncomment the following lines to quickly test 32-bit wraparound
//                mCblk->user = 0xffff0000;
//                mCblk->server = 0xffff0000;
//                mCblk->userBase = 0xffff0000;
//                mCblk->serverBase = 0xffff0000;
                mChannelCount = channelCount;
                mChannelMask = channelMask;
                if (sharedBuffer == 0) {
                    mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
#ifdef QCOM_ENHANCED_AUDIO
                    if ((int16_t)flags == 0x1) {
                        bufferSize = frameCount*channelCount*sizeof(int16_t);
                    } else {
                       if ((format == AUDIO_FORMAT_PCM_16_BIT) ||
                           (format == AUDIO_FORMAT_PCM_8_BIT))
                       {
                          memset(mBuffer, 0, frameCount*channelCount*sizeof(int16_t));
                       }
                       else if (format == AUDIO_FORMAT_AMR_NB)
                       {
                          memset(mBuffer, 0, frameCount*channelCount*32); // full rate frame size
                       }
                       else if (format == AUDIO_FORMAT_EVRC)
                       {
                          memset(mBuffer, 0, frameCount*channelCount*23); // full rate frame size
                       }
                       else if (format == AUDIO_FORMAT_QCELP)
                       {
                          memset(mBuffer, 0, frameCount*channelCount*35); // full rate frame size
                       }
                       else if (format == AUDIO_FORMAT_AAC)
                       {
                          memset(mBuffer, 0, frameCount*2048); // full rate frame size
                       }
                       else if (format == AUDIO_FORMAT_AMR_WB)
                       {
                          memset(mBuffer, 0, frameCount*channelCount*61); // full rate frame size
                       }
                    }
#else
                    memset(mBuffer, 0, frameCount*channelCount*sizeof(int16_t));
#endif
                    // Force underrun condition to avoid false underrun callback until first data is
                    // written to buffer (other flags are cleared)
                    mCblk->flags = CBLK_UNDERRUN_ON;
                } else {
                    mBuffer = sharedBuffer->pointer();
                }
                mBufferEnd = (uint8_t *)mBuffer + bufferSize;
            }
        } else {
            ALOGE("not enough memory for AudioTrack size=%u", size);
            client->heap()->dump("AudioTrack");
            return;
        }
    } else {
        mCblk = (audio_track_cblk_t *)(new uint8_t[size]);
        // construct the shared structure in-place.
        new(mCblk) audio_track_cblk_t();
        // clear all buffers
        mCblk->frameCount = frameCount;
        mCblk->sampleRate = sampleRate;
// uncomment the following lines to quickly test 32-bit wraparound
//        mCblk->user = 0xffff0000;
//        mCblk->server = 0xffff0000;
//        mCblk->userBase = 0xffff0000;
//        mCblk->serverBase = 0xffff0000;
        mChannelCount = channelCount;
        mChannelMask = channelMask;
        mBuffer = (char*)mCblk + sizeof(audio_track_cblk_t);
        memset(mBuffer, 0, frameCount*channelCount*sizeof(int16_t));
        // Force underrun condition to avoid false underrun callback until first data is
        // written to buffer (other flags are cleared)
        mCblk->flags = CBLK_UNDERRUN_ON;
        mBufferEnd = (uint8_t *)mBuffer + bufferSize;
    }
}

AudioFlinger::ThreadBase::TrackBase::~TrackBase()
{
    if (mCblk != NULL) {
        if (mClient == 0) {
            delete mCblk;
        } else {
            mCblk->~audio_track_cblk_t();   // destroy our shared-structure.
        }
    }
    mCblkMemory.clear();    // free the shared memory before releasing the heap it belongs to
    if (mClient != 0) {
        // Client destructor must run with AudioFlinger mutex locked
        Mutex::Autolock _l(mClient->audioFlinger()->mLock);
        // If the client's reference count drops to zero, the associated destructor
        // must run with AudioFlinger lock held. Thus the explicit clear() rather than
        // relying on the automatic clear() at end of scope.
        mClient.clear();
    }
}

// AudioBufferProvider interface
// getNextBuffer() = 0;
// This implementation of releaseBuffer() is used by Track and RecordTrack, but not TimedTrack
void AudioFlinger::ThreadBase::TrackBase::releaseBuffer(AudioBufferProvider::Buffer* buffer)
{
    buffer->raw = NULL;
    mFrameCount = buffer->frameCount;
    // FIXME See note at getNextBuffer()
    (void) step();      // ignore return value of step()
    buffer->frameCount = 0;
}

bool AudioFlinger::ThreadBase::TrackBase::step() {
    bool result;
    audio_track_cblk_t* cblk = this->cblk();

    result = cblk->stepServer(mFrameCount);
    if (!result) {
        ALOGV("stepServer failed acquiring cblk mutex");
        mStepServerFailed = true;
    }
    return result;
}

void AudioFlinger::ThreadBase::TrackBase::reset() {
    audio_track_cblk_t* cblk = this->cblk();

    cblk->user = 0;
    cblk->server = 0;
    cblk->userBase = 0;
    cblk->serverBase = 0;
    mStepServerFailed = false;
    ALOGV("TrackBase::reset");
}

int AudioFlinger::ThreadBase::TrackBase::sampleRate() const {
    return (int)mCblk->sampleRate;
}

void* AudioFlinger::ThreadBase::TrackBase::getBuffer(uint32_t offset, uint32_t frames) const {
    audio_track_cblk_t* cblk = this->cblk();
    size_t frameSize = cblk->frameSize;
    int8_t *bufferStart = (int8_t *)mBuffer + (offset-cblk->serverBase)*frameSize;
    int8_t *bufferEnd = bufferStart + frames * frameSize;

    // Check validity of returned pointer in case the track control block would have been corrupted.
#ifdef QCOM_ENHANCED_AUDIO
    if (bufferStart < mBuffer || bufferStart > bufferEnd || bufferEnd > mBufferEnd){
        ALOGE("TrackBase::getBuffer buffer out of range:\n    start: %p, end %p , mBuffer %p mBufferEnd %p\n    \
                server %u, serverBase %u, user %u, userBase %u",
                bufferStart, bufferEnd, mBuffer, mBufferEnd,
                cblk->server, cblk->serverBase, cblk->user, cblk->userBase);
        return 0;
    }
#else
    ALOG_ASSERT(!(bufferStart < mBuffer || bufferStart > bufferEnd || bufferEnd > mBufferEnd),
            "TrackBase::getBuffer buffer out of range:\n"
                "    start: %p, end %p , mBuffer %p mBufferEnd %p\n"
                "    server %u, serverBase %u, user %u, userBase %u, frameSize %d",
                cblk->server, cblk->serverBase, cblk->user, cblk->userBase, frameSize);
#endif
    return bufferStart;
}

status_t AudioFlinger::ThreadBase::TrackBase::setSyncEvent(const sp<SyncEvent>& event)
{
    mSyncEvents.add(event);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

// Track constructor must be called with AudioFlinger::mLock and ThreadBase::mLock held
AudioFlinger::PlaybackThread::Track::Track(
            PlaybackThread *thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            int frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId,
            IAudioFlinger::track_flags_t flags)
    :   TrackBase(thread, client, sampleRate, format, channelMask, frameCount,
#ifdef QCOM_ENHANCED_AUDIO
                ((audio_stream_type_t)streamType == AUDIO_STREAM_VOICE_CALL)?0x1:0x0,
#endif
                sharedBuffer, sessionId),
    mMute(false),
    mFillingUpStatus(FS_INVALID),
    // mRetryCount initialized later when needed
    mSharedBuffer(sharedBuffer),
    mStreamType(streamType),
    mName(-1),  // see note below
    mMainBuffer(thread->mixBuffer()),
    mAuxBuffer(NULL),
    mAuxEffectId(0), mHasVolumeController(false),
    mPresentationCompleteFrames(0),
    mFlags(flags),
    mFastIndex(-1),
    mUnderrunCount(0),
    mCachedVolume(1.0)
{
    if (mCblk != NULL) {
        // NOTE: audio_track_cblk_t::frameSize for 8 bit PCM data is based on a sample size of
        // 16 bit because data is converted to 16 bit before being stored in buffer by AudioTrack
#ifdef QCOM_ENHANCED_AUDIO
        if ((audio_stream_type_t)streamType == AUDIO_STREAM_VOICE_CALL)
            mCblk->frameSize = mChannelCount * sizeof(int16_t);
        else
#endif
            mCblk->frameSize = audio_is_linear_pcm(format) ? mChannelCount * sizeof(int16_t) : sizeof(uint8_t);
        // to avoid leaking a track name, do not allocate one unless there is an mCblk
        mName = thread->getTrackName_l(channelMask, sessionId);
        mCblk->mName = mName;
        if (mName < 0) {
            ALOGE("no more track names available");
            return;
        }
        // only allocate a fast track index if we were able to allocate a normal track name
        if (flags & IAudioFlinger::TRACK_FAST) {
            mCblk->flags |= CBLK_FAST;  // atomic op not needed yet
            ALOG_ASSERT(thread->mFastTrackAvailMask != 0);
            int i = __builtin_ctz(thread->mFastTrackAvailMask);
            ALOG_ASSERT(0 < i && i < (int)FastMixerState::kMaxFastTracks);
            // FIXME This is too eager.  We allocate a fast track index before the
            //       fast track becomes active.  Since fast tracks are a scarce resource,
            //       this means we are potentially denying other more important fast tracks from
            //       being created.  It would be better to allocate the index dynamically.
            mFastIndex = i;
            mCblk->mName = i;
            // Read the initial underruns because this field is never cleared by the fast mixer
            mObservedUnderruns = thread->getFastTrackUnderruns(i);
            thread->mFastTrackAvailMask &= ~(1 << i);
        }
    }
    ALOGV("Track constructor name %d, calling pid %d", mName, IPCThreadState::self()->getCallingPid());
}

AudioFlinger::PlaybackThread::Track::~Track()
{
    ALOGV("PlaybackThread::Track destructor");
}

void AudioFlinger::PlaybackThread::Track::destroy()
{
    // NOTE: destroyTrack_l() can remove a strong reference to this Track
    // by removing it from mTracks vector, so there is a risk that this Tracks's
    // destructor is called. As the destructor needs to lock mLock,
    // we must acquire a strong reference on this Track before locking mLock
    // here so that the destructor is called only when exiting this function.
    // On the other hand, as long as Track::destroy() is only called by
    // TrackHandle destructor, the TrackHandle still holds a strong ref on
    // this Track with its member mTrack.
    sp<Track> keep(this);
    { // scope for mLock
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            if (!isOutputTrack()) {
                if (mState == ACTIVE || mState == RESUMING) {
                    AudioSystem::stopOutput(thread->id(), mStreamType, mSessionId);

#ifdef ADD_BATTERY_DATA
                    // to track the speaker usage
                    addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
#endif
                }
                AudioSystem::releaseOutput(thread->id());
            }
            Mutex::Autolock _l(thread->mLock);
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            playbackThread->destroyTrack_l(this);
        }
    }
}

/*static*/ void AudioFlinger::PlaybackThread::Track::appendDumpHeader(String8& result)
{
    result.append("   Name Client Type Fmt Chn mask   Session mFrCnt fCount S M F SRate  L dB  R dB  "
                  "  Server      User     Main buf    Aux Buf  Flags Underruns\n");
}

void AudioFlinger::PlaybackThread::Track::dump(char* buffer, size_t size)
{
    uint32_t vlr = mCblk->getVolumeLR();
    if (isFastTrack()) {
        sprintf(buffer, "   F %2d", mFastIndex);
    } else {
        sprintf(buffer, "   %4d", mName - AudioMixer::TRACK0);
    }
    track_state state = mState;
    char stateChar;
    switch (state) {
    case IDLE:
        stateChar = 'I';
        break;
    case TERMINATED:
        stateChar = 'T';
        break;
    case STOPPING_1:
        stateChar = 's';
        break;
    case STOPPING_2:
        stateChar = '5';
        break;
    case STOPPED:
        stateChar = 'S';
        break;
    case RESUMING:
        stateChar = 'R';
        break;
    case ACTIVE:
        stateChar = 'A';
        break;
    case PAUSING:
        stateChar = 'p';
        break;
    case PAUSED:
        stateChar = 'P';
        break;
    case FLUSHED:
        stateChar = 'F';
        break;
    default:
        stateChar = '?';
        break;
    }
    char nowInUnderrun;
    switch (mObservedUnderruns.mBitFields.mMostRecent) {
    case UNDERRUN_FULL:
        nowInUnderrun = ' ';
        break;
    case UNDERRUN_PARTIAL:
        nowInUnderrun = '<';
        break;
    case UNDERRUN_EMPTY:
        nowInUnderrun = '*';
        break;
    default:
        nowInUnderrun = '?';
        break;
    }
    snprintf(&buffer[7], size-7, " %6d %4u %3u 0x%08x %7u %6u %6u %1c %1d %1d %5u %5.2g %5.2g  "
            "0x%08x 0x%08x 0x%08x 0x%08x %#5x %9u%c\n",
            (mClient == 0) ? getpid_cached : mClient->pid(),
            mStreamType,
            mFormat,
            mChannelMask,
            mSessionId,
            mFrameCount,
            mCblk->frameCount,
            stateChar,
            mMute,
            mFillingUpStatus,
            mCblk->sampleRate,
            20.0 * log10((vlr & 0xFFFF) / 4096.0),
            20.0 * log10((vlr >> 16) / 4096.0),
            mCblk->server,
            mCblk->user,
            (int)mMainBuffer,
            (int)mAuxBuffer,
            mCblk->flags,
            mUnderrunCount,
            nowInUnderrun);
}

// AudioBufferProvider interface
status_t AudioFlinger::PlaybackThread::Track::getNextBuffer(
        AudioBufferProvider::Buffer* buffer, int64_t pts)
{
    audio_track_cblk_t* cblk = this->cblk();
    uint32_t framesReady;
    uint32_t framesReq = buffer->frameCount;

    // Check if last stepServer failed, try to step now
    if (mStepServerFailed) {
        // FIXME When called by fast mixer, this takes a mutex with tryLock().
        //       Since the fast mixer is higher priority than client callback thread,
        //       it does not result in priority inversion for client.
        //       But a non-blocking solution would be preferable to avoid
        //       fast mixer being unable to tryLock(), and
        //       to avoid the extra context switches if the client wakes up,
        //       discovers the mutex is locked, then has to wait for fast mixer to unlock.
        if (!step())  goto getNextBuffer_exit;
        ALOGV("stepServer recovered");
        mStepServerFailed = false;
    }

    // FIXME Same as above
    framesReady = cblk->framesReady();

    if (CC_LIKELY(framesReady)) {
        uint32_t s = cblk->server;
        uint32_t bufferEnd = cblk->serverBase + cblk->frameCount;

        bufferEnd = (cblk->loopEnd < bufferEnd) ? cblk->loopEnd : bufferEnd;
        if (framesReq > framesReady) {
            framesReq = framesReady;
        }
        if (framesReq > bufferEnd - s) {
            framesReq = bufferEnd - s;
        }

        buffer->raw = getBuffer(s, framesReq);
        buffer->frameCount = framesReq;
        return NO_ERROR;
    }

getNextBuffer_exit:
    buffer->raw = NULL;
    buffer->frameCount = 0;
    ALOGV("getNextBuffer() no more data for track %d on thread %p", mName, mThread.unsafe_get());
    return NOT_ENOUGH_DATA;
}

// Note that framesReady() takes a mutex on the control block using tryLock().
// This could result in priority inversion if framesReady() is called by the normal mixer,
// as the normal mixer thread runs at lower
// priority than the client's callback thread:  there is a short window within framesReady()
// during which the normal mixer could be preempted, and the client callback would block.
// Another problem can occur if framesReady() is called by the fast mixer:
// the tryLock() could block for up to 1 ms, and a sequence of these could delay fast mixer.
// FIXME Replace AudioTrackShared control block implementation by a non-blocking FIFO queue.
size_t AudioFlinger::PlaybackThread::Track::framesReady() const {
    return mCblk->framesReady();
}

// Don't call for fast tracks; the framesReady() could result in priority inversion
bool AudioFlinger::PlaybackThread::Track::isReady() const {
    if (mFillingUpStatus != FS_FILLING || isStopped() || isPausing()) return true;

    if (framesReady() >= mCblk->frameCount ||
            (mCblk->flags & CBLK_FORCEREADY_MSK)) {
        mFillingUpStatus = FS_FILLED;
        android_atomic_and(~CBLK_FORCEREADY_MSK, &mCblk->flags);
        return true;
    }
    return false;
}

status_t AudioFlinger::PlaybackThread::Track::start(AudioSystem::sync_event_t event,
                                                    int triggerSession)
{
    status_t status = NO_ERROR;
    ALOGV("start(%d), calling pid %d session %d",
            mName, IPCThreadState::self()->getCallingPid(), mSessionId);

    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        track_state state = mState;
        // here the track could be either new, or restarted
        // in both cases "unstop" the track
        if (mState == PAUSED) {
            mState = TrackBase::RESUMING;
            ALOGV("PAUSED => RESUMING (%d) on thread %p", mName, this);
        } else {
            mState = TrackBase::ACTIVE;
            ALOGV("? => ACTIVE (%d) on thread %p", mName, this);
        }

        if (!isOutputTrack() && state != ACTIVE && state != RESUMING) {
            thread->mLock.unlock();
            status = AudioSystem::startOutput(thread->id(), mStreamType, mSessionId);
            thread->mLock.lock();

#ifdef ADD_BATTERY_DATA
            // to track the speaker usage
            if (status == NO_ERROR) {
                addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStart);
            }
#endif
        }
        if (status == NO_ERROR) {
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            playbackThread->addTrack_l(this);
        } else {
            mState = state;
            triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
        }
    } else {
        status = BAD_VALUE;
    }
    return status;
}

void AudioFlinger::PlaybackThread::Track::stop()
{
    ALOGV("stop(%d), calling pid %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        track_state state = mState;
        if (state == RESUMING || state == ACTIVE || state == PAUSING || state == PAUSED) {
            // If the track is not active (PAUSED and buffers full), flush buffers
            PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
            if (playbackThread->mActiveTracks.indexOf(this) < 0) {
                reset();
                mState = STOPPED;
            } else if (!isFastTrack()) {
                mState = STOPPED;
            } else {
                // prepareTracks_l() will set state to STOPPING_2 after next underrun,
                // and then to STOPPED and reset() when presentation is complete
                mState = STOPPING_1;
            }
            ALOGV("not stopping/stopped => stopping/stopped (%d) on thread %p", mName, playbackThread);
        }
        if (!isOutputTrack() && (state == ACTIVE || state == RESUMING)) {
            thread->mLock.unlock();
            AudioSystem::stopOutput(thread->id(), mStreamType, mSessionId);
            thread->mLock.lock();

#ifdef ADD_BATTERY_DATA
            // to track the speaker usage
            addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
#endif
        }
    }
}

void AudioFlinger::PlaybackThread::Track::pause()
{
    ALOGV("pause(%d), calling pid %d", mName, IPCThreadState::self()->getCallingPid());
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        if (mState == ACTIVE || mState == RESUMING) {
            mState = PAUSING;
            ALOGV("ACTIVE/RESUMING => PAUSING (%d) on thread %p", mName, thread.get());
            if (!isOutputTrack()) {
                thread->mLock.unlock();
                AudioSystem::stopOutput(thread->id(), mStreamType, mSessionId);
                thread->mLock.lock();

#ifdef ADD_BATTERY_DATA
                // to track the speaker usage
                addBatteryData(IMediaPlayerService::kBatteryDataAudioFlingerStop);
#endif
            }
        }
    }
}

void AudioFlinger::PlaybackThread::Track::flush()
{
    ALOGV("flush(%d)", mName);
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        Mutex::Autolock _l(thread->mLock);
        if (mState != STOPPING_1 && mState != STOPPING_2 && mState != STOPPED && mState != PAUSED &&
                mState != PAUSING && mState != IDLE && mState != FLUSHED) {
            return;
        }
        // No point remaining in PAUSED state after a flush => go to
        // FLUSHED state
        mState = FLUSHED;
        // do not reset the track if it is still in the process of being stopped or paused.
        // this will be done by prepareTracks_l() when the track is stopped.
        // prepareTracks_l() will see mState == FLUSHED, then
        // remove from active track list, reset(), and trigger presentation complete
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        if (playbackThread->mActiveTracks.indexOf(this) < 0) {
            reset();
        }
    }
}

void AudioFlinger::PlaybackThread::Track::reset()
{
    // Do not reset twice to avoid discarding data written just after a flush and before
    // the audioflinger thread detects the track is stopped.
    if (!mResetDone) {
        TrackBase::reset();
        // Force underrun condition to avoid false underrun callback until first data is
        // written to buffer
        android_atomic_and(~CBLK_FORCEREADY_MSK, &mCblk->flags);
        android_atomic_or(CBLK_UNDERRUN_ON, &mCblk->flags);
        mFillingUpStatus = FS_FILLING;
        mResetDone = true;
        if (mState == FLUSHED) {
            mState = IDLE;
        }
    }
}

void AudioFlinger::PlaybackThread::Track::mute(bool muted)
{
    mMute = muted;
}

status_t AudioFlinger::PlaybackThread::Track::attachAuxEffect(int EffectId)
{
    status_t status = DEAD_OBJECT;
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        PlaybackThread *playbackThread = (PlaybackThread *)thread.get();
        sp<AudioFlinger> af = mClient->audioFlinger();

        Mutex::Autolock _l(af->mLock);

        sp<PlaybackThread> srcThread = af->getEffectThread_l(AUDIO_SESSION_OUTPUT_MIX, EffectId);

        if (EffectId != 0 && srcThread != 0 && playbackThread != srcThread.get()) {
            Mutex::Autolock _dl(playbackThread->mLock);
            Mutex::Autolock _sl(srcThread->mLock);
            sp<EffectChain> chain = srcThread->getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
            if (chain == 0) {
                return INVALID_OPERATION;
            }

            sp<EffectModule> effect = chain->getEffectFromId_l(EffectId);
            if (effect == 0) {
                return INVALID_OPERATION;
            }
            srcThread->removeEffect_l(effect);
            playbackThread->addEffect_l(effect);
            // removeEffect_l() has stopped the effect if it was active so it must be restarted
            if (effect->state() == EffectModule::ACTIVE ||
                    effect->state() == EffectModule::STOPPING) {
                effect->start();
            }

            sp<EffectChain> dstChain = effect->chain().promote();
            if (dstChain == 0) {
                srcThread->addEffect_l(effect);
                return INVALID_OPERATION;
            }
            AudioSystem::unregisterEffect(effect->id());
            AudioSystem::registerEffect(&effect->desc(),
                                        srcThread->id(),
                                        dstChain->strategy(),
                                        AUDIO_SESSION_OUTPUT_MIX,
                                        effect->id());
        }
        status = playbackThread->attachAuxEffect(this, EffectId);
    }
    return status;
}

void AudioFlinger::PlaybackThread::Track::setAuxBuffer(int EffectId, int32_t *buffer)
{
    mAuxEffectId = EffectId;
    mAuxBuffer = buffer;
}

bool AudioFlinger::PlaybackThread::Track::presentationComplete(size_t framesWritten,
                                                         size_t audioHalFrames)
{
    // a track is considered presented when the total number of frames written to audio HAL
    // corresponds to the number of frames written when presentationComplete() is called for the
    // first time (mPresentationCompleteFrames == 0) plus the buffer filling status at that time.
    if (mPresentationCompleteFrames == 0) {
        mPresentationCompleteFrames = framesWritten + audioHalFrames;
        ALOGV("presentationComplete() reset: mPresentationCompleteFrames %d audioHalFrames %d",
                  mPresentationCompleteFrames, audioHalFrames);
    }
    if (framesWritten >= mPresentationCompleteFrames) {
        ALOGV("presentationComplete() session %d complete: framesWritten %d",
                  mSessionId, framesWritten);
        triggerEvents(AudioSystem::SYNC_EVENT_PRESENTATION_COMPLETE);
        return true;
    }
    return false;
}

void AudioFlinger::PlaybackThread::Track::triggerEvents(AudioSystem::sync_event_t type)
{
    for (int i = 0; i < (int)mSyncEvents.size(); i++) {
        if (mSyncEvents[i]->type() == type) {
            mSyncEvents[i]->trigger();
            mSyncEvents.removeAt(i);
            i--;
        }
    }
}

// implement VolumeBufferProvider interface

uint32_t AudioFlinger::PlaybackThread::Track::getVolumeLR()
{
    // called by FastMixer, so not allowed to take any locks, block, or do I/O including logs
    ALOG_ASSERT(isFastTrack() && (mCblk != NULL));
    uint32_t vlr = mCblk->getVolumeLR();
    uint32_t vl = vlr & 0xFFFF;
    uint32_t vr = vlr >> 16;
    // track volumes come from shared memory, so can't be trusted and must be clamped
    if (vl > MAX_GAIN_INT) {
        vl = MAX_GAIN_INT;
    }
    if (vr > MAX_GAIN_INT) {
        vr = MAX_GAIN_INT;
    }
    // now apply the cached master volume and stream type volume;
    // this is trusted but lacks any synchronization or barrier so may be stale
    float v = mCachedVolume;
    vl *= v;
    vr *= v;
    // re-combine into U4.16
    vlr = (vr << 16) | (vl & 0xFFFF);
    // FIXME look at mute, pause, and stop flags
    return vlr;
}

status_t AudioFlinger::PlaybackThread::Track::setSyncEvent(const sp<SyncEvent>& event)
{
    if (mState == TERMINATED || mState == PAUSED ||
            ((framesReady() == 0) && ((mSharedBuffer != 0) ||
                                      (mState == STOPPED)))) {
        ALOGW("Track::setSyncEvent() in invalid state %d on session %d %s mode, framesReady %d ",
              mState, mSessionId, (mSharedBuffer != 0) ? "static" : "stream", framesReady());
        event->cancel();
        return INVALID_OPERATION;
    }
    (void) TrackBase::setSyncEvent(event);
    return NO_ERROR;
}

// timed audio tracks

sp<AudioFlinger::PlaybackThread::TimedTrack>
AudioFlinger::PlaybackThread::TimedTrack::create(
            PlaybackThread *thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            int frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId) {
    if (!client->reserveTimedTrack())
        return 0;

    return new TimedTrack(
        thread, client, streamType, sampleRate, format, channelMask, frameCount,
        sharedBuffer, sessionId);
}

AudioFlinger::PlaybackThread::TimedTrack::TimedTrack(
            PlaybackThread *thread,
            const sp<Client>& client,
            audio_stream_type_t streamType,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            int frameCount,
            const sp<IMemory>& sharedBuffer,
            int sessionId)
    : Track(thread, client, streamType, sampleRate, format, channelMask,
            frameCount, sharedBuffer, sessionId, IAudioFlinger::TRACK_TIMED),
      mQueueHeadInFlight(false),
      mTrimQueueHeadOnRelease(false),
      mFramesPendingInQueue(0),
      mTimedSilenceBuffer(NULL),
      mTimedSilenceBufferSize(0),
      mTimedAudioOutputOnTime(false),
      mMediaTimeTransformValid(false)
{
    LocalClock lc;
    mLocalTimeFreq = lc.getLocalFreq();

    mLocalTimeToSampleTransform.a_zero = 0;
    mLocalTimeToSampleTransform.b_zero = 0;
    mLocalTimeToSampleTransform.a_to_b_numer = sampleRate;
    mLocalTimeToSampleTransform.a_to_b_denom = mLocalTimeFreq;
    LinearTransform::reduce(&mLocalTimeToSampleTransform.a_to_b_numer,
                            &mLocalTimeToSampleTransform.a_to_b_denom);

    mMediaTimeToSampleTransform.a_zero = 0;
    mMediaTimeToSampleTransform.b_zero = 0;
    mMediaTimeToSampleTransform.a_to_b_numer = sampleRate;
    mMediaTimeToSampleTransform.a_to_b_denom = 1000000;
    LinearTransform::reduce(&mMediaTimeToSampleTransform.a_to_b_numer,
                            &mMediaTimeToSampleTransform.a_to_b_denom);
}

AudioFlinger::PlaybackThread::TimedTrack::~TimedTrack() {
    mClient->releaseTimedTrack();
    delete [] mTimedSilenceBuffer;
}

status_t AudioFlinger::PlaybackThread::TimedTrack::allocateTimedBuffer(
    size_t size, sp<IMemory>* buffer) {

    Mutex::Autolock _l(mTimedBufferQueueLock);

    trimTimedBufferQueue_l();

    // lazily initialize the shared memory heap for timed buffers
    if (mTimedMemoryDealer == NULL) {
        const int kTimedBufferHeapSize = 512 << 10;

        mTimedMemoryDealer = new MemoryDealer(kTimedBufferHeapSize,
                                              "AudioFlingerTimed");
        if (mTimedMemoryDealer == NULL)
            return NO_MEMORY;
    }

    sp<IMemory> newBuffer = mTimedMemoryDealer->allocate(size);
    if (newBuffer == NULL) {
        newBuffer = mTimedMemoryDealer->allocate(size);
        if (newBuffer == NULL)
            return NO_MEMORY;
    }

    *buffer = newBuffer;
    return NO_ERROR;
}

// caller must hold mTimedBufferQueueLock
void AudioFlinger::PlaybackThread::TimedTrack::trimTimedBufferQueue_l() {
    int64_t mediaTimeNow;
    {
        Mutex::Autolock mttLock(mMediaTimeTransformLock);
        if (!mMediaTimeTransformValid)
            return;

        int64_t targetTimeNow;
        status_t res = (mMediaTimeTransformTarget == TimedAudioTrack::COMMON_TIME)
            ? mCCHelper.getCommonTime(&targetTimeNow)
            : mCCHelper.getLocalTime(&targetTimeNow);

        if (OK != res)
            return;

        if (!mMediaTimeTransform.doReverseTransform(targetTimeNow,
                                                    &mediaTimeNow)) {
            return;
        }
    }

    size_t trimEnd;
    for (trimEnd = 0; trimEnd < mTimedBufferQueue.size(); trimEnd++) {
        int64_t bufEnd;

        if ((trimEnd + 1) < mTimedBufferQueue.size()) {
            // We have a next buffer.  Just use its PTS as the PTS of the frame
            // following the last frame in this buffer.  If the stream is sparse
            // (ie, there are deliberate gaps left in the stream which should be
            // filled with silence by the TimedAudioTrack), then this can result
            // in one extra buffer being left un-trimmed when it could have
            // been.  In general, this is not typical, and we would rather
            // optimized away the TS calculation below for the more common case
            // where PTSes are contiguous.
            bufEnd = mTimedBufferQueue[trimEnd + 1].pts();
        } else {
            // We have no next buffer.  Compute the PTS of the frame following
            // the last frame in this buffer by computing the duration of of
            // this frame in media time units and adding it to the PTS of the
            // buffer.
            int64_t frameCount = mTimedBufferQueue[trimEnd].buffer()->size()
                               / mCblk->frameSize;

            if (!mMediaTimeToSampleTransform.doReverseTransform(frameCount,
                                                                &bufEnd)) {
                ALOGE("Failed to convert frame count of %lld to media time"
                      " duration" " (scale factor %d/%u) in %s",
                      frameCount,
                      mMediaTimeToSampleTransform.a_to_b_numer,
                      mMediaTimeToSampleTransform.a_to_b_denom,
                      __PRETTY_FUNCTION__);
                break;
            }
            bufEnd += mTimedBufferQueue[trimEnd].pts();
        }

        if (bufEnd > mediaTimeNow)
            break;

        // Is the buffer we want to use in the middle of a mix operation right
        // now?  If so, don't actually trim it.  Just wait for the releaseBuffer
        // from the mixer which should be coming back shortly.
        if (!trimEnd && mQueueHeadInFlight) {
            mTrimQueueHeadOnRelease = true;
        }
    }

    size_t trimStart = mTrimQueueHeadOnRelease ? 1 : 0;
    if (trimStart < trimEnd) {
        // Update the bookkeeping for framesReady()
        for (size_t i = trimStart; i < trimEnd; ++i) {
            updateFramesPendingAfterTrim_l(mTimedBufferQueue[i], "trim");
        }

        // Now actually remove the buffers from the queue.
        mTimedBufferQueue.removeItemsAt(trimStart, trimEnd);
    }
}

void AudioFlinger::PlaybackThread::TimedTrack::trimTimedBufferQueueHead_l(
        const char* logTag) {
    ALOG_ASSERT(mTimedBufferQueue.size() > 0,
                "%s called (reason \"%s\"), but timed buffer queue has no"
                " elements to trim.", __FUNCTION__, logTag);

    updateFramesPendingAfterTrim_l(mTimedBufferQueue[0], logTag);
    mTimedBufferQueue.removeAt(0);
}

void AudioFlinger::PlaybackThread::TimedTrack::updateFramesPendingAfterTrim_l(
        const TimedBuffer& buf,
        const char* logTag) {
    uint32_t bufBytes        = buf.buffer()->size();
    uint32_t consumedAlready = buf.position();

    ALOG_ASSERT(consumedAlready <= bufBytes,
                "Bad bookkeeping while updating frames pending.  Timed buffer is"
                " only %u bytes long, but claims to have consumed %u"
                " bytes.  (update reason: \"%s\")",
                bufBytes, consumedAlready, logTag);

    uint32_t bufFrames = (bufBytes - consumedAlready) / mCblk->frameSize;
    ALOG_ASSERT(mFramesPendingInQueue >= bufFrames,
                "Bad bookkeeping while updating frames pending.  Should have at"
                " least %u queued frames, but we think we have only %u.  (update"
                " reason: \"%s\")",
                bufFrames, mFramesPendingInQueue, logTag);

    mFramesPendingInQueue -= bufFrames;
}

status_t AudioFlinger::PlaybackThread::TimedTrack::queueTimedBuffer(
    const sp<IMemory>& buffer, int64_t pts) {

    {
        Mutex::Autolock mttLock(mMediaTimeTransformLock);
        if (!mMediaTimeTransformValid)
            return INVALID_OPERATION;
    }

    Mutex::Autolock _l(mTimedBufferQueueLock);

    uint32_t bufFrames = buffer->size() / mCblk->frameSize;
    mFramesPendingInQueue += bufFrames;
    mTimedBufferQueue.add(TimedBuffer(buffer, pts));

    return NO_ERROR;
}

status_t AudioFlinger::PlaybackThread::TimedTrack::setMediaTimeTransform(
    const LinearTransform& xform, TimedAudioTrack::TargetTimeline target) {

    ALOGVV("setMediaTimeTransform az=%lld bz=%lld n=%d d=%u tgt=%d",
           xform.a_zero, xform.b_zero, xform.a_to_b_numer, xform.a_to_b_denom,
           target);

    if (!(target == TimedAudioTrack::LOCAL_TIME ||
          target == TimedAudioTrack::COMMON_TIME)) {
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mMediaTimeTransformLock);
    mMediaTimeTransform = xform;
    mMediaTimeTransformTarget = target;
    mMediaTimeTransformValid = true;

    return NO_ERROR;
}

#define min(a, b) ((a) < (b) ? (a) : (b))

// implementation of getNextBuffer for tracks whose buffers have timestamps
status_t AudioFlinger::PlaybackThread::TimedTrack::getNextBuffer(
    AudioBufferProvider::Buffer* buffer, int64_t pts)
{
    if (pts == AudioBufferProvider::kInvalidPTS) {
        buffer->raw = NULL;
        buffer->frameCount = 0;
        mTimedAudioOutputOnTime = false;
        return INVALID_OPERATION;
    }

    Mutex::Autolock _l(mTimedBufferQueueLock);

    ALOG_ASSERT(!mQueueHeadInFlight,
                "getNextBuffer called without releaseBuffer!");

    while (true) {

        // if we have no timed buffers, then fail
        if (mTimedBufferQueue.isEmpty()) {
            buffer->raw = NULL;
            buffer->frameCount = 0;
            return NOT_ENOUGH_DATA;
        }

        TimedBuffer& head = mTimedBufferQueue.editItemAt(0);

        // calculate the PTS of the head of the timed buffer queue expressed in
        // local time
        int64_t headLocalPTS;
        {
            Mutex::Autolock mttLock(mMediaTimeTransformLock);

            ALOG_ASSERT(mMediaTimeTransformValid, "media time transform invalid");

            if (mMediaTimeTransform.a_to_b_denom == 0) {
                // the transform represents a pause, so yield silence
                timedYieldSilence_l(buffer->frameCount, buffer);
                return NO_ERROR;
            }

            int64_t transformedPTS;
            if (!mMediaTimeTransform.doForwardTransform(head.pts(),
                                                        &transformedPTS)) {
                // the transform failed.  this shouldn't happen, but if it does
                // then just drop this buffer
                ALOGW("timedGetNextBuffer transform failed");
                buffer->raw = NULL;
                buffer->frameCount = 0;
                trimTimedBufferQueueHead_l("getNextBuffer; no transform");
                return NO_ERROR;
            }

            if (mMediaTimeTransformTarget == TimedAudioTrack::COMMON_TIME) {
                if (OK != mCCHelper.commonTimeToLocalTime(transformedPTS,
                                                          &headLocalPTS)) {
                    buffer->raw = NULL;
                    buffer->frameCount = 0;
                    return INVALID_OPERATION;
                }
            } else {
                headLocalPTS = transformedPTS;
            }
        }

        // adjust the head buffer's PTS to reflect the portion of the head buffer
        // that has already been consumed
        int64_t effectivePTS = headLocalPTS +
                ((head.position() / mCblk->frameSize) * mLocalTimeFreq / sampleRate());

        // Calculate the delta in samples between the head of the input buffer
        // queue and the start of the next output buffer that will be written.
        // If the transformation fails because of over or underflow, it means
        // that the sample's position in the output stream is so far out of
        // whack that it should just be dropped.
        int64_t sampleDelta;
        if (llabs(effectivePTS - pts) >= (static_cast<int64_t>(1) << 31)) {
            ALOGV("*** head buffer is too far from PTS: dropped buffer");
            trimTimedBufferQueueHead_l("getNextBuffer, buf pts too far from"
                                       " mix");
            continue;
        }
        if (!mLocalTimeToSampleTransform.doForwardTransform(
                (effectivePTS - pts) << 32, &sampleDelta)) {
            ALOGV("*** too late during sample rate transform: dropped buffer");
            trimTimedBufferQueueHead_l("getNextBuffer, bad local to sample");
            continue;
        }

        ALOGVV("*** getNextBuffer head.pts=%lld head.pos=%d pts=%lld"
               " sampleDelta=[%d.%08x]",
               head.pts(), head.position(), pts,
               static_cast<int32_t>((sampleDelta >= 0 ? 0 : 1)
                   + (sampleDelta >> 32)),
               static_cast<uint32_t>(sampleDelta & 0xFFFFFFFF));

        // if the delta between the ideal placement for the next input sample and
        // the current output position is within this threshold, then we will
        // concatenate the next input samples to the previous output
        const int64_t kSampleContinuityThreshold =
                (static_cast<int64_t>(sampleRate()) << 32) / 250;

        // if this is the first buffer of audio that we're emitting from this track
        // then it should be almost exactly on time.
        const int64_t kSampleStartupThreshold = 1LL << 32;

        if ((mTimedAudioOutputOnTime && llabs(sampleDelta) <= kSampleContinuityThreshold) ||
           (!mTimedAudioOutputOnTime && llabs(sampleDelta) <= kSampleStartupThreshold)) {
            // the next input is close enough to being on time, so concatenate it
            // with the last output
            timedYieldSamples_l(buffer);

            ALOGVV("*** on time: head.pos=%d frameCount=%u",
                    head.position(), buffer->frameCount);
            return NO_ERROR;
        }

        // Looks like our output is not on time.  Reset our on timed status.
        // Next time we mix samples from our input queue, then should be within
        // the StartupThreshold.
        mTimedAudioOutputOnTime = false;
        if (sampleDelta > 0) {
            // the gap between the current output position and the proper start of
            // the next input sample is too big, so fill it with silence
            uint32_t framesUntilNextInput = (sampleDelta + 0x80000000) >> 32;

            timedYieldSilence_l(framesUntilNextInput, buffer);
            ALOGV("*** silence: frameCount=%u", buffer->frameCount);
            return NO_ERROR;
        } else {
            // the next input sample is late
            uint32_t lateFrames = static_cast<uint32_t>(-((sampleDelta + 0x80000000) >> 32));
            size_t onTimeSamplePosition =
                    head.position() + lateFrames * mCblk->frameSize;

            if (onTimeSamplePosition > head.buffer()->size()) {
                // all the remaining samples in the head are too late, so
                // drop it and move on
                ALOGV("*** too late: dropped buffer");
                trimTimedBufferQueueHead_l("getNextBuffer, dropped late buffer");
                continue;
            } else {
                // skip over the late samples
                head.setPosition(onTimeSamplePosition);

                // yield the available samples
                timedYieldSamples_l(buffer);

                ALOGV("*** late: head.pos=%d frameCount=%u", head.position(), buffer->frameCount);
                return NO_ERROR;
            }
        }
    }
}

// Yield samples from the timed buffer queue head up to the given output
// buffer's capacity.
//
// Caller must hold mTimedBufferQueueLock
void AudioFlinger::PlaybackThread::TimedTrack::timedYieldSamples_l(
    AudioBufferProvider::Buffer* buffer) {

    const TimedBuffer& head = mTimedBufferQueue[0];

    buffer->raw = (static_cast<uint8_t*>(head.buffer()->pointer()) +
                   head.position());

    uint32_t framesLeftInHead = ((head.buffer()->size() - head.position()) /
                                 mCblk->frameSize);
    size_t framesRequested = buffer->frameCount;
    buffer->frameCount = min(framesLeftInHead, framesRequested);

    mQueueHeadInFlight = true;
    mTimedAudioOutputOnTime = true;
}

// Yield samples of silence up to the given output buffer's capacity
//
// Caller must hold mTimedBufferQueueLock
void AudioFlinger::PlaybackThread::TimedTrack::timedYieldSilence_l(
    uint32_t numFrames, AudioBufferProvider::Buffer* buffer) {

    // lazily allocate a buffer filled with silence
    if (mTimedSilenceBufferSize < numFrames * mCblk->frameSize) {
        delete [] mTimedSilenceBuffer;
        mTimedSilenceBufferSize = numFrames * mCblk->frameSize;
        mTimedSilenceBuffer = new uint8_t[mTimedSilenceBufferSize];
        memset(mTimedSilenceBuffer, 0, mTimedSilenceBufferSize);
    }

    buffer->raw = mTimedSilenceBuffer;
    size_t framesRequested = buffer->frameCount;
    buffer->frameCount = min(numFrames, framesRequested);

    mTimedAudioOutputOnTime = false;
}

// AudioBufferProvider interface
void AudioFlinger::PlaybackThread::TimedTrack::releaseBuffer(
    AudioBufferProvider::Buffer* buffer) {

    Mutex::Autolock _l(mTimedBufferQueueLock);

    // If the buffer which was just released is part of the buffer at the head
    // of the queue, be sure to update the amt of the buffer which has been
    // consumed.  If the buffer being returned is not part of the head of the
    // queue, its either because the buffer is part of the silence buffer, or
    // because the head of the timed queue was trimmed after the mixer called
    // getNextBuffer but before the mixer called releaseBuffer.
    if (buffer->raw == mTimedSilenceBuffer) {
        ALOG_ASSERT(!mQueueHeadInFlight,
                    "Queue head in flight during release of silence buffer!");
        goto done;
    }

    ALOG_ASSERT(mQueueHeadInFlight,
                "TimedTrack::releaseBuffer of non-silence buffer, but no queue"
                " head in flight.");

    if (mTimedBufferQueue.size()) {
        TimedBuffer& head = mTimedBufferQueue.editItemAt(0);

        void* start = head.buffer()->pointer();
        void* end   = reinterpret_cast<void*>(
                        reinterpret_cast<uint8_t*>(head.buffer()->pointer())
                        + head.buffer()->size());

        ALOG_ASSERT((buffer->raw >= start) && (buffer->raw < end),
                    "released buffer not within the head of the timed buffer"
                    " queue; qHead = [%p, %p], released buffer = %p",
                    start, end, buffer->raw);

        head.setPosition(head.position() +
                (buffer->frameCount * mCblk->frameSize));
        mQueueHeadInFlight = false;

        ALOG_ASSERT(mFramesPendingInQueue >= buffer->frameCount,
                    "Bad bookkeeping during releaseBuffer!  Should have at"
                    " least %u queued frames, but we think we have only %u",
                    buffer->frameCount, mFramesPendingInQueue);

        mFramesPendingInQueue -= buffer->frameCount;

        if ((static_cast<size_t>(head.position()) >= head.buffer()->size())
            || mTrimQueueHeadOnRelease) {
            trimTimedBufferQueueHead_l("releaseBuffer");
            mTrimQueueHeadOnRelease = false;
        }
    } else {
        LOG_FATAL("TimedTrack::releaseBuffer of non-silence buffer with no"
                  " buffers in the timed buffer queue");
    }

done:
    buffer->raw = 0;
    buffer->frameCount = 0;
}

size_t AudioFlinger::PlaybackThread::TimedTrack::framesReady() const {
    Mutex::Autolock _l(mTimedBufferQueueLock);
    return mFramesPendingInQueue;
}

AudioFlinger::PlaybackThread::TimedTrack::TimedBuffer::TimedBuffer()
        : mPTS(0), mPosition(0) {}

AudioFlinger::PlaybackThread::TimedTrack::TimedBuffer::TimedBuffer(
    const sp<IMemory>& buffer, int64_t pts)
        : mBuffer(buffer), mPTS(pts), mPosition(0) {}

// ----------------------------------------------------------------------------

// RecordTrack constructor must be called with AudioFlinger::mLock held
AudioFlinger::RecordThread::RecordTrack::RecordTrack(
            RecordThread *thread,
            const sp<Client>& client,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            int frameCount,
#ifdef QCOM_ENHANCED_AUDIO
            uint32_t flags,
#endif
            int sessionId)
    :   TrackBase(thread, client, sampleRate, format,channelMask,frameCount,
#ifdef QCOM_ENHANCED_AUDIO
                  ((audio_source_t)((int16_t)flags) == AUDIO_SOURCE_VOICE_COMMUNICATION) ?
                  ((flags & 0xffff0000)| 0x1) : ((flags & 0xffff0000)),
#endif
                  0 /*sharedBuffer*/, sessionId),
        mOverflow(false)
{
    uint8_t channelCount = popcount(channelMask);
    if (mCblk != NULL) {
#ifdef QCOM_ENHANCED_AUDIO
        ALOGV("RecordTrack constructor, size %d flags %d", (int)mBufferEnd - (int)mBuffer,flags);
        if ((audio_source_t)((int16_t)flags) == AUDIO_SOURCE_VOICE_COMMUNICATION) {
             mCblk->frameSize = mChannelCount * sizeof(int16_t);
        } else {
            if (format == AUDIO_FORMAT_AMR_NB) {
                mCblk->frameSize = channelCount * 32;
            } else if (format == AUDIO_FORMAT_EVRC) {
                mCblk->frameSize = channelCount * 23;
            } else if (format == AUDIO_FORMAT_QCELP) {
                mCblk->frameSize = channelCount * 35;
            } else if (format == AUDIO_FORMAT_AAC) {
                mCblk->frameSize = 2048;
            } else if (format == AUDIO_FORMAT_PCM_16_BIT) {
                mCblk->frameSize = mChannelCount * sizeof(int16_t);
            } else if (format == AUDIO_FORMAT_PCM_8_BIT) {
                mCblk->frameSize = mChannelCount * sizeof(int8_t);
            } else if (format == AUDIO_FORMAT_AMR_WB) {
                mCblk->frameSize = channelCount * 61;
            } else {
                mCblk->frameSize = sizeof(int8_t);
            }
        }
#else
        ALOGV("RecordTrack constructor, size %d", (int)mBufferEnd - (int)mBuffer);
        if (format == AUDIO_FORMAT_PCM_16_BIT) {
            mCblk->frameSize = mChannelCount * sizeof(int16_t);
        } else if (format == AUDIO_FORMAT_PCM_8_BIT) {
            mCblk->frameSize = mChannelCount * sizeof(int8_t);
        } else {
            mCblk->frameSize = sizeof(int8_t);
        }
#endif
    }
}

AudioFlinger::RecordThread::RecordTrack::~RecordTrack()
{
    ALOGV("%s", __func__);
}

// AudioBufferProvider interface
status_t AudioFlinger::RecordThread::RecordTrack::getNextBuffer(AudioBufferProvider::Buffer* buffer, int64_t pts)
{
    audio_track_cblk_t* cblk = this->cblk();
    uint32_t framesAvail;
    uint32_t framesReq = buffer->frameCount;

    // Check if last stepServer failed, try to step now
    if (mStepServerFailed) {
        if (!step()) goto getNextBuffer_exit;
        ALOGV("stepServer recovered");
        mStepServerFailed = false;
    }

    framesAvail = cblk->framesAvailable_l();

    if (CC_LIKELY(framesAvail)) {
        uint32_t s = cblk->server;
        uint32_t bufferEnd = cblk->serverBase + cblk->frameCount;

        if (framesReq > framesAvail) {
            framesReq = framesAvail;
        }
        if (framesReq > bufferEnd - s) {
            framesReq = bufferEnd - s;
        }

        buffer->raw = getBuffer(s, framesReq);
        buffer->frameCount = framesReq;
        return NO_ERROR;
    }

getNextBuffer_exit:
    buffer->raw = NULL;
    buffer->frameCount = 0;
    return NOT_ENOUGH_DATA;
}

status_t AudioFlinger::RecordThread::RecordTrack::start(AudioSystem::sync_event_t event,
                                                        int triggerSession)
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        RecordThread *recordThread = (RecordThread *)thread.get();
        return recordThread->start(this, event, triggerSession);
    } else {
        return BAD_VALUE;
    }
}

void AudioFlinger::RecordThread::RecordTrack::stop()
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread != 0) {
        RecordThread *recordThread = (RecordThread *)thread.get();
        recordThread->mLock.lock();
        bool doStop = recordThread->stop_l(this);
        if (doStop) {
            TrackBase::reset();
            // Force overrun condition to avoid false overrun callback until first data is
            // read from buffer
            android_atomic_or(CBLK_UNDERRUN_ON, &mCblk->flags);
        }
        recordThread->mLock.unlock();
        if (doStop) {
            AudioSystem::stopInput(recordThread->id());
        }
    }
}

/*static*/ void AudioFlinger::RecordThread::RecordTrack::appendDumpHeader(String8& result)
{
    result.append("   Clien Fmt Chn mask   Session Buf  S SRate  Serv     User   FrameCount\n");
}

void AudioFlinger::RecordThread::RecordTrack::dump(char* buffer, size_t size)
{
    snprintf(buffer, size, "   %05d %03u 0x%08x %05d   %04u %01d %05u  %08x %08x %05d\n",
            (mClient == 0) ? getpid_cached : mClient->pid(),
            mFormat,
            mChannelMask,
            mSessionId,
            mFrameCount,
            mState,
            mCblk->sampleRate,
            mCblk->server,
            mCblk->user,
            mCblk->frameCount);
}


// ----------------------------------------------------------------------------

AudioFlinger::PlaybackThread::OutputTrack::OutputTrack(
            PlaybackThread *playbackThread,
            DuplicatingThread *sourceThread,
            uint32_t sampleRate,
            audio_format_t format,
            audio_channel_mask_t channelMask,
            int frameCount)
    :   Track(playbackThread, NULL, AUDIO_STREAM_CNT, sampleRate, format, channelMask, frameCount,
                NULL, 0, IAudioFlinger::TRACK_DEFAULT),
    mActive(false), mSourceThread(sourceThread)
{

    if (mCblk != NULL) {
        mCblk->flags |= CBLK_DIRECTION_OUT;
        mCblk->buffers = (char*)mCblk + sizeof(audio_track_cblk_t);
        mOutBuffer.frameCount = 0;
        playbackThread->mTracks.add(this);
        ALOGV("OutputTrack constructor mCblk %p, mBuffer %p, mCblk->buffers %p, " \
                "mCblk->frameCount %d, mCblk->sampleRate %d, mChannelMask 0x%08x mBufferEnd %p",
                mCblk, mBuffer, mCblk->buffers,
                mCblk->frameCount, mCblk->sampleRate, mChannelMask, mBufferEnd);
    } else {
        ALOGW("Error creating output track on thread %p", playbackThread);
    }
}

AudioFlinger::PlaybackThread::OutputTrack::~OutputTrack()
{
    clearBufferQueue();
}

status_t AudioFlinger::PlaybackThread::OutputTrack::start(AudioSystem::sync_event_t event,
                                                          int triggerSession)
{
    status_t status = Track::start(event, triggerSession);
    if (status != NO_ERROR) {
        return status;
    }

    mActive = true;
    mRetryCount = 127;
    return status;
}

void AudioFlinger::PlaybackThread::OutputTrack::stop()
{
    Track::stop();
    clearBufferQueue();
    mOutBuffer.frameCount = 0;
    mActive = false;
}

bool AudioFlinger::PlaybackThread::OutputTrack::write(int16_t* data, uint32_t frames)
{
    Buffer *pInBuffer;
    Buffer inBuffer;
    uint32_t channelCount = mChannelCount;
    bool outputBufferFull = false;
    inBuffer.frameCount = frames;
    inBuffer.i16 = data;

    uint32_t waitTimeLeftMs = mSourceThread->waitTimeMs();

    if (!mActive && frames != 0) {
        start();
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            MixerThread *mixerThread = (MixerThread *)thread.get();
            if (mCblk->frameCount > frames){
                if (mBufferQueue.size() < kMaxOverFlowBuffers) {
                    uint32_t startFrames = (mCblk->frameCount - frames);
                    pInBuffer = new Buffer;
                    pInBuffer->mBuffer = new int16_t[startFrames * channelCount];
                    pInBuffer->frameCount = startFrames;
                    pInBuffer->i16 = pInBuffer->mBuffer;
                    memset(pInBuffer->raw, 0, startFrames * channelCount * sizeof(int16_t));
                    mBufferQueue.add(pInBuffer);
                } else {
                    ALOGW ("OutputTrack::write() %p no more buffers in queue", this);
                }
            }
        }
    }

    while (waitTimeLeftMs) {
        // First write pending buffers, then new data
        if (mBufferQueue.size()) {
            pInBuffer = mBufferQueue.itemAt(0);
        } else {
            pInBuffer = &inBuffer;
        }

        if (pInBuffer->frameCount == 0) {
            break;
        }

        if (mOutBuffer.frameCount == 0) {
            mOutBuffer.frameCount = pInBuffer->frameCount;
            nsecs_t startTime = systemTime();
            if (obtainBuffer(&mOutBuffer, waitTimeLeftMs) == (status_t)NO_MORE_BUFFERS) {
                ALOGV ("OutputTrack::write() %p thread %p no more output buffers", this, mThread.unsafe_get());
                outputBufferFull = true;
                break;
            }
            uint32_t waitTimeMs = (uint32_t)ns2ms(systemTime() - startTime);
            if (waitTimeLeftMs >= waitTimeMs) {
                waitTimeLeftMs -= waitTimeMs;
            } else {
                waitTimeLeftMs = 0;
            }
        }

        uint32_t outFrames = pInBuffer->frameCount > mOutBuffer.frameCount ? mOutBuffer.frameCount : pInBuffer->frameCount;
        memcpy(mOutBuffer.raw, pInBuffer->raw, outFrames * channelCount * sizeof(int16_t));
        mCblk->stepUser(outFrames);
        pInBuffer->frameCount -= outFrames;
        pInBuffer->i16 += outFrames * channelCount;
        mOutBuffer.frameCount -= outFrames;
        mOutBuffer.i16 += outFrames * channelCount;

        if (pInBuffer->frameCount == 0) {
            if (mBufferQueue.size()) {
                mBufferQueue.removeAt(0);
                delete [] pInBuffer->mBuffer;
                delete pInBuffer;
                ALOGV("OutputTrack::write() %p thread %p released overflow buffer %d", this, mThread.unsafe_get(), mBufferQueue.size());
            } else {
                break;
            }
        }
    }

    // If we could not write all frames, allocate a buffer and queue it for next time.
    if (inBuffer.frameCount) {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0 && !thread->standby()) {
            if (mBufferQueue.size() < kMaxOverFlowBuffers) {
                pInBuffer = new Buffer;
                pInBuffer->mBuffer = new int16_t[inBuffer.frameCount * channelCount];
                pInBuffer->frameCount = inBuffer.frameCount;
                pInBuffer->i16 = pInBuffer->mBuffer;
                memcpy(pInBuffer->raw, inBuffer.raw, inBuffer.frameCount * channelCount * sizeof(int16_t));
                mBufferQueue.add(pInBuffer);
                ALOGV("OutputTrack::write() %p thread %p adding overflow buffer %d", this, mThread.unsafe_get(), mBufferQueue.size());
            } else {
                ALOGW("OutputTrack::write() %p thread %p no more overflow buffers", mThread.unsafe_get(), this);
            }
        }
    }

    // Calling write() with a 0 length buffer, means that no more data will be written:
    // If no more buffers are pending, fill output track buffer to make sure it is started
    // by output mixer.
    if (frames == 0 && mBufferQueue.size() == 0) {
        if (mCblk->user < mCblk->frameCount) {
            frames = mCblk->frameCount - mCblk->user;
            pInBuffer = new Buffer;
            pInBuffer->mBuffer = new int16_t[frames * channelCount];
            pInBuffer->frameCount = frames;
            pInBuffer->i16 = pInBuffer->mBuffer;
            memset(pInBuffer->raw, 0, frames * channelCount * sizeof(int16_t));
            mBufferQueue.add(pInBuffer);
        } else if (mActive) {
            stop();
        }
    }

    return outputBufferFull;
}

status_t AudioFlinger::PlaybackThread::OutputTrack::obtainBuffer(AudioBufferProvider::Buffer* buffer, uint32_t waitTimeMs)
{
    int active;
    status_t result;
    audio_track_cblk_t* cblk = mCblk;
    uint32_t framesReq = buffer->frameCount;

//    ALOGV("OutputTrack::obtainBuffer user %d, server %d", cblk->user, cblk->server);
    buffer->frameCount  = 0;

    uint32_t framesAvail = cblk->framesAvailable();


    if (framesAvail == 0) {
        Mutex::Autolock _l(cblk->lock);
        goto start_loop_here;
        while (framesAvail == 0) {
            active = mActive;
            if (CC_UNLIKELY(!active)) {
                ALOGV("Not active and NO_MORE_BUFFERS");
                return NO_MORE_BUFFERS;
            }
            result = cblk->cv.waitRelative(cblk->lock, milliseconds(waitTimeMs));
            if (result != NO_ERROR) {
                return NO_MORE_BUFFERS;
            }
            // read the server count again
        start_loop_here:
            framesAvail = cblk->framesAvailable_l();
        }
    }

//    if (framesAvail < framesReq) {
//        return NO_MORE_BUFFERS;
//    }

    if (framesReq > framesAvail) {
        framesReq = framesAvail;
    }

    uint32_t u = cblk->user;
    uint32_t bufferEnd = cblk->userBase + cblk->frameCount;

    if (framesReq > bufferEnd - u) {
        framesReq = bufferEnd - u;
    }

    buffer->frameCount  = framesReq;
    buffer->raw         = (void *)cblk->buffer(u);
    return NO_ERROR;
}


void AudioFlinger::PlaybackThread::OutputTrack::clearBufferQueue()
{
    size_t size = mBufferQueue.size();

    for (size_t i = 0; i < size; i++) {
        Buffer *pBuffer = mBufferQueue.itemAt(i);
        delete [] pBuffer->mBuffer;
        delete pBuffer;
    }
    mBufferQueue.clear();
}

// ----------------------------------------------------------------------------

AudioFlinger::Client::Client(const sp<AudioFlinger>& audioFlinger, pid_t pid)
    :   RefBase(),
        mAudioFlinger(audioFlinger),
        // FIXME should be a "k" constant not hard-coded, in .h or ro. property, see 4 lines below
        mMemoryDealer(new MemoryDealer(1024*1024, "AudioFlinger::Client")),
        mPid(pid),
        mTimedTrackCount(0)
{
    // 1 MB of address space is good for 32 tracks, 8 buffers each, 4 KB/buffer
}

// Client destructor must be called with AudioFlinger::mLock held
AudioFlinger::Client::~Client()
{
    mAudioFlinger->removeClient_l(mPid);
}

sp<MemoryDealer> AudioFlinger::Client::heap() const
{
    return mMemoryDealer;
}

// Reserve one of the limited slots for a timed audio track associated
// with this client
bool AudioFlinger::Client::reserveTimedTrack()
{
    const int kMaxTimedTracksPerClient = 4;

    Mutex::Autolock _l(mTimedTrackLock);

    if (mTimedTrackCount >= kMaxTimedTracksPerClient) {
        ALOGW("can not create timed track - pid %d has exceeded the limit",
             mPid);
        return false;
    }

    mTimedTrackCount++;
    return true;
}

// Release a slot for a timed audio track
void AudioFlinger::Client::releaseTimedTrack()
{
    Mutex::Autolock _l(mTimedTrackLock);
    mTimedTrackCount--;
}

// ----------------------------------------------------------------------------

AudioFlinger::NotificationClient::NotificationClient(const sp<AudioFlinger>& audioFlinger,
                                                     const sp<IAudioFlingerClient>& client,
                                                     sp<IBinder> binder)
    : mAudioFlinger(audioFlinger), mBinder(binder), mAudioFlingerClient(client)
{
}

AudioFlinger::NotificationClient::~NotificationClient()
{
}

void AudioFlinger::NotificationClient::binderDied(const wp<IBinder>& who)
{
    sp<NotificationClient> keep(this);
    mAudioFlinger->removeNotificationClient(mBinder);
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
        createEffectThread();

        mAudioFlingerClient = new AudioFlingerDirectTrackClient(this);
        mAudioFlinger->registerClient(mAudioFlingerClient);

        allocateBufPool();
#ifdef SRS_PROCESSING
    } else if (mFlag & AUDIO_OUTPUT_FLAG_TUNNEL) {
        ALOGV("create effects thread for TUNNEL");
        createEffectThread();
        mAudioFlingerClient = new AudioFlingerDirectTrackClient(this);
        mAudioFlinger->registerClient(mAudioFlingerClient);
#endif
    }
    outputDesc->mVolumeScale = 1.0;
    mDeathRecipient = new PMDeathRecipient(this);
    acquireWakeLock();
}

AudioFlinger::DirectAudioTrack::~DirectAudioTrack() {
#ifdef SRS_PROCESSING
    ALOGD("SRS_Processing - DirectAudioTrack - OutNotify_Init: %p TID %d\n", this, gettid());
    POSTPRO_PATCH_ICS_OUTPROC_DIRECT_EXIT(this, gettid());
#endif
    if (mFlag & AUDIO_OUTPUT_FLAG_LPA) {
        requestAndWaitForEffectsThreadExit();
        mAudioFlinger->deregisterClient(mAudioFlingerClient);
        mAudioFlinger->deleteEffectSession();
        deallocateBufPool();
#ifdef SRS_PROCESSING
    } else if (mFlag & AUDIO_OUTPUT_FLAG_TUNNEL) {
        requestAndWaitForEffectsThreadExit();
        mAudioFlinger->deregisterClient(mAudioFlingerClient);
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
    mOutputDesc->mVolumeLeft = left;
    mOutputDesc->mVolumeRight = right;
    mOutputDesc->stream->set_volume(mOutputDesc->stream,
                                left * mOutputDesc->mVolumeScale,
                                right* mOutputDesc->mVolumeScale);
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

void *AudioFlinger::DirectAudioTrack::EffectsThreadWrapper(void *me) {
    static_cast<DirectAudioTrack *>(me)->EffectsThreadEntry();
    return NULL;
}

void AudioFlinger::DirectAudioTrack::EffectsThreadEntry() {
    while(1) {
        mEffectLock.lock();
        if (!mEffectConfigChanged && !mKillEffectsThread) {
            mEffectCv.wait(mEffectLock);
        }

        if(mKillEffectsThread) {
            mEffectLock.unlock();
            break;
        }

        if (mEffectConfigChanged) {
            mEffectConfigChanged = false;
            if (mFlag & AUDIO_OUTPUT_FLAG_LPA) {
                for ( List<BufferInfo>::iterator it = mEffectsPool.begin();
                      it != mEffectsPool.end(); it++) {
                    ALOGV("ete: calling applyEffectsOn buff %x",it->localBuf);
                    bool isEffectsApplied = mAudioFlinger->applyEffectsOn(
                                    static_cast<void *>(this),
                                    (int16_t *)it->localBuf,
                                    (int16_t *)mEffectsThreadScratchBuffer,
                                    it->bytesToWrite,
                                    false);
                    if (isEffectsApplied == true){
                        ALOGV("ete:dsp updated for local buf %x",it->localBuf);
                        memcpy(it->dspBuf, mEffectsThreadScratchBuffer, it->bytesToWrite);
                    }
                    else
                        ALOGV("ete:dsp updated for local buf %x SKIPPED",it->localBuf);

                    if (mEffectConfigChanged) {
                        ALOGE("ete:effects changed, abort effects application");
                        break;
                    }
            }
#ifdef SRS_PROCESSING
            } else if (mFlag & AUDIO_OUTPUT_FLAG_TUNNEL) {
                ALOGV("applying effects for TUNNEL");
                char buffer[2];
                    //dummy buffer to ensure the SRS processing takes place
                    // The API mandates Sample rate and channel mode. Hence
                    // defaulted the sample rate channel mode to 48000 and 2 respectively
                POSTPRO_PATCH_ICS_OUTPROC_DIRECT_SAMPLES(static_cast<void *>(this),
                                                         AUDIO_FORMAT_PCM_16_BIT,
                                                        (int16_t*)buffer, 2, 48000, 2);
#endif
            }
        }
        mEffectLock.unlock();
    }
    ALOGV("Effects thread is dead");
    mEffectsThreadAlive = false;
}

void AudioFlinger::DirectAudioTrack::requestAndWaitForEffectsThreadExit() {
    if (!mEffectsThreadAlive)
        return;
    mKillEffectsThread = true;
    mEffectCv.signal();
    pthread_join(mEffectsThread,NULL);
    ALOGV("effects thread killed");
}

void AudioFlinger::DirectAudioTrack::createEffectThread() {
    //Create the effects thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    mEffectsThreadAlive = true;
    ALOGV("Creating Effects Thread");
    pthread_create(&mEffectsThread, &attr, EffectsThreadWrapper, this);
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

AudioFlinger::TrackHandle::TrackHandle(const sp<AudioFlinger::PlaybackThread::Track>& track)
    : BnAudioTrack(),
      mTrack(track)
{
}

AudioFlinger::TrackHandle::~TrackHandle() {
    // just stop the track on deletion, associated resources
    // will be freed from the main thread once all pending buffers have
    // been played. Unless it's not in the active track list, in which
    // case we free everything now...
    mTrack->destroy();
}

sp<IMemory> AudioFlinger::TrackHandle::getCblk() const {
    return mTrack->getCblk();
}

status_t AudioFlinger::TrackHandle::start() {
    return mTrack->start();
}

void AudioFlinger::TrackHandle::stop() {
    mTrack->stop();
}

void AudioFlinger::TrackHandle::flush() {
    mTrack->flush();
}

void AudioFlinger::TrackHandle::mute(bool e) {
    mTrack->mute(e);
}

void AudioFlinger::TrackHandle::pause() {
    mTrack->pause();
}

status_t AudioFlinger::TrackHandle::attachAuxEffect(int EffectId)
{
    return mTrack->attachAuxEffect(EffectId);
}

status_t AudioFlinger::TrackHandle::allocateTimedBuffer(size_t size,
                                                         sp<IMemory>* buffer) {
    if (!mTrack->isTimedTrack())
        return INVALID_OPERATION;

    PlaybackThread::TimedTrack* tt =
            reinterpret_cast<PlaybackThread::TimedTrack*>(mTrack.get());
    return tt->allocateTimedBuffer(size, buffer);
}

status_t AudioFlinger::TrackHandle::queueTimedBuffer(const sp<IMemory>& buffer,
                                                     int64_t pts) {
    if (!mTrack->isTimedTrack())
        return INVALID_OPERATION;

    PlaybackThread::TimedTrack* tt =
            reinterpret_cast<PlaybackThread::TimedTrack*>(mTrack.get());
    return tt->queueTimedBuffer(buffer, pts);
}

status_t AudioFlinger::TrackHandle::setMediaTimeTransform(
    const LinearTransform& xform, int target) {

    if (!mTrack->isTimedTrack())
        return INVALID_OPERATION;

    PlaybackThread::TimedTrack* tt =
            reinterpret_cast<PlaybackThread::TimedTrack*>(mTrack.get());
    return tt->setMediaTimeTransform(
        xform, static_cast<TimedAudioTrack::TargetTimeline>(target));
}

status_t AudioFlinger::TrackHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioTrack::onTransact(code, data, reply, flags);
}

// ----------------------------------------------------------------------------

sp<IAudioRecord> AudioFlinger::openRecord(
        pid_t pid,
        audio_io_handle_t input,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        int frameCount,
        IAudioFlinger::track_flags_t flags,
        pid_t tid,
        int *sessionId,
        status_t *status)
{
    sp<RecordThread::RecordTrack> recordTrack;
    sp<RecordHandle> recordHandle;
    sp<Client> client;
#ifdef QCOM_ENHANCED_AUDIO
    size_t inputBufferSize = 0;
    uint32_t channelCount = popcount(channelMask);
#endif
    status_t lStatus;
    RecordThread *thread;
    size_t inFrameCount;
    int lSessionId;

    // check calling permissions
    if (!recordingAllowed()) {
        lStatus = PERMISSION_DENIED;
        goto Exit;
    }

#ifdef QCOM_ENHANCED_AUDIO
    // Check that audio input stream accepts requested audio parameters
    inputBufferSize = getInputBufferSize(sampleRate, format, channelCount);
    if (inputBufferSize == 0) {
        lStatus = BAD_VALUE;
        ALOGE("Bad audio input parameters: sampling rate %u, format %d, channels %d",  sampleRate, format, channelCount);
        goto Exit;
    }
#endif
    // add client to list
    { // scope for mLock
        Mutex::Autolock _l(mLock);
        thread = checkRecordThread_l(input);
        if (thread == NULL) {
            lStatus = BAD_VALUE;
            goto Exit;
        }

        client = registerPid_l(pid);

        // If no audio session id is provided, create one here
        if (sessionId != NULL && *sessionId != AUDIO_SESSION_OUTPUT_MIX) {
            lSessionId = *sessionId;
        } else {
            lSessionId = nextUniqueId();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
        }
#ifdef QCOM_ENHANCED_AUDIO
        // frameCount must be a multiple of input buffer size
        // Change for Codec type
        uint8_t channelCount = popcount(channelMask);
        if ((audio_source_t)((int16_t)flags) == AUDIO_SOURCE_VOICE_COMMUNICATION) {
             inFrameCount = inputBufferSize/channelCount/sizeof(short);
        } else {
            if ((format == AUDIO_FORMAT_PCM_16_BIT) ||
                (format == AUDIO_FORMAT_PCM_8_BIT))
            {
              inFrameCount = inputBufferSize/channelCount/sizeof(short);
            }
            else if (format == AUDIO_FORMAT_AMR_NB)
            {
              inFrameCount = inputBufferSize/channelCount/32;
            }
            else if (format == AUDIO_FORMAT_EVRC)
            {
              inFrameCount = inputBufferSize/channelCount/23;
            }
            else if (format == AUDIO_FORMAT_QCELP)
            {
              inFrameCount = inputBufferSize/channelCount/35;
            }
            else if (format == AUDIO_FORMAT_AAC)
            {
              inFrameCount = inputBufferSize/2048;
            }
            else if (format == AUDIO_FORMAT_AMR_WB)
            {
              inFrameCount = inputBufferSize/channelCount/61;
            }
        }
        frameCount = ((frameCount - 1)/inFrameCount + 1) * inFrameCount;
#endif
        // create new record track. The record track uses one track in mHardwareMixerThread by convention.
        recordTrack = thread->createRecordTrack_l(client, sampleRate, format, channelMask,
                                                  frameCount, lSessionId, flags, tid, &lStatus);
    }
    if (lStatus != NO_ERROR) {
        // remove local strong reference to Client before deleting the RecordTrack so that the Client
        // destructor is called by the TrackBase destructor with mLock held
        client.clear();
        recordTrack.clear();
        goto Exit;
    }

    // return to handle to client
    recordHandle = new RecordHandle(recordTrack);
    lStatus = NO_ERROR;

Exit:
    if (status) {
        *status = lStatus;
    }
    return recordHandle;
}

// ----------------------------------------------------------------------------

AudioFlinger::RecordHandle::RecordHandle(const sp<AudioFlinger::RecordThread::RecordTrack>& recordTrack)
    : BnAudioRecord(),
    mRecordTrack(recordTrack)
{
}

AudioFlinger::RecordHandle::~RecordHandle() {
    stop_nonvirtual();
    mRecordTrack->destroy();
}

sp<IMemory> AudioFlinger::RecordHandle::getCblk() const {
    return mRecordTrack->getCblk();
}

status_t AudioFlinger::RecordHandle::start(int /*AudioSystem::sync_event_t*/ event, int triggerSession) {
    ALOGV("RecordHandle::start()");
    return mRecordTrack->start((AudioSystem::sync_event_t)event, triggerSession);
}

void AudioFlinger::RecordHandle::stop() {
    stop_nonvirtual();
}

void AudioFlinger::RecordHandle::stop_nonvirtual() {
    ALOGV("RecordHandle::stop()");
    mRecordTrack->stop();
}

status_t AudioFlinger::RecordHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioRecord::onTransact(code, data, reply, flags);
}

// ----------------------------------------------------------------------------

AudioFlinger::RecordThread::RecordThread(const sp<AudioFlinger>& audioFlinger,
                                         AudioStreamIn *input,
                                         uint32_t sampleRate,
                                         audio_channel_mask_t channelMask,
                                         audio_io_handle_t id,
                                         audio_devices_t device) :
    ThreadBase(audioFlinger, id, AUDIO_DEVICE_NONE, device, RECORD),
    mInput(input), mResampler(NULL), mRsmpOutBuffer(NULL), mRsmpInBuffer(NULL),
    // mRsmpInIndex and mInputBytes set by readInputParameters()
    mReqChannelCount(getInputChannelCount(channelMask)),
    mReqSampleRate(sampleRate)
    // mBytesRead is only meaningful while active, and so is cleared in start()
    // (but might be better to also clear here for dump?)
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

                if (exitPending()) break;

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
                            int8_t *dst = buffer.i8 + (buffer.frameCount - framesOut) * mActiveTrack->mCblk->frameSize;
                            if (framesIn > framesOut)
                                framesIn = framesOut;
                            mRsmpInIndex += framesIn;
                            framesOut -= framesIn;
                            if ((int)mChannelCount == mReqChannelCount ||
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
#ifdef QCOM_ENHANCED_AUDIO
                            if (((int) framesOut != mFrameCount) &&
                                ((mFormat != AUDIO_FORMAT_PCM_16_BIT)&&
                                  ((audio_source_t)mInputSource != AUDIO_SOURCE_VOICE_COMMUNICATION))) {
                                mBytesRead = mInput->stream->read(mInput->stream, buffer.raw,
                                    buffer.frameCount * mFrameSize);
                                ALOGV("IR mBytesRead = %d",mBytesRead);
                                if(mBytesRead >= 0 ){
                                    buffer.frameCount = mBytesRead/mFrameSize;
                                }
                                framesOut = 0;
                            } else 
#endif
                            if (framesOut == mFrameCount &&
#ifdef QCOM_ENHANCED_AUDIO
                                ((audio_source_t)mInputSource != AUDIO_SOURCE_VOICE_COMMUNICATION) &&
#endif
                                ((int)mChannelCount == mReqChannelCount || mFormat != AUDIO_FORMAT_PCM_16_BIT)) {
                                mBytesRead = mInput->stream->read(mInput->stream, buffer.raw, mInputBytes);
#ifdef QCOM_ENHANCED_AUDIO
                                if( mBytesRead >= 0 ){
                                      buffer.frameCount = mBytesRead/mFrameSize;
                                }
#endif
                                framesOut = 0;
                            } else {
                                mBytesRead = mInput->stream->read(mInput->stream, mRsmpInBuffer, mInputBytes);
                                mRsmpInIndex = 0;
                            }
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
                        }
                    }
                } else {
                    // resampling

                    memset(mRsmpOutBuffer, 0, framesOut * 2 * sizeof(int32_t));
                    // alter output frame count as if we were expecting stereo samples
                    if (mChannelCount == 1 && mReqChannelCount == 1) {
                        framesOut >>= 1;
                    }
                    mResampler->resample(mRsmpOutBuffer, framesOut, this /* AudioBufferProvider* */);
                    // ditherAndClamp() works as long as all buffers returned by mActiveTrack->getNextBuffer()
                    // are 32 bit aligned which should be always true.
                    if (mChannelCount == 2 && mReqChannelCount == 1) {
                        ditherAndClamp(mRsmpOutBuffer, mRsmpOutBuffer, framesOut);
                        // the resampler always outputs stereo samples: do post stereo to mono conversion
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
        int frameCount,
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
                      format, channelMask, frameCount,
#ifdef QCOM_ENHANCED_AUDIO
                      flags,
#endif
                      sessionId);

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

void AudioFlinger::RecordThread::RecordTrack::destroy()
{
    // see comments at AudioFlinger::PlaybackThread::Track::destroy()
    sp<RecordTrack> keep(this);
    {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            if (mState == ACTIVE || mState == RESUMING) {
                AudioSystem::stopInput(thread->id());
            }
            AudioSystem::releaseInput(thread->id());
            Mutex::Autolock _l(thread->mLock);
            RecordThread *recordThread = (RecordThread *) thread.get();
            recordThread->destroyTrack_l(this);
        }
    }
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
        snprintf(buffer, SIZE, "Out channel count: %d\n", mReqChannelCount);
        result.append(buffer);
        snprintf(buffer, SIZE, "Out sample rate: %d\n", mReqSampleRate);
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
        int reqSamplingRate = mReqSampleRate;
        int reqChannelCount = mReqChannelCount;

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
                // disable AEC and NS if the device is a BT SCO headset supporting those pre processings
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
            status = mInput->stream->common.set_parameters(&mInput->stream->common, keyValuePair.string());
            if (status == INVALID_OPERATION) {
                inputStandBy();
                status = mInput->stream->common.set_parameters(&mInput->stream->common,
                        keyValuePair.string());
            }
            if (reconfig) {
                if (status == BAD_VALUE &&
                    reqFormat == mInput->stream->common.get_format(&mInput->stream->common) &&
                    reqFormat == AUDIO_FORMAT_PCM_16_BIT &&
                    ((int)mInput->stream->common.get_sample_rate(&mInput->stream->common) <= (2 * reqSamplingRate)) &&
                    getInputChannelCount(mInput->stream->common.get_channels(&mInput->stream->common)) <= FCC_2 &&
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

        // optmization: if mono to mono, alter input frame count as if we were inputing stereo samples
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


// ----------------------------------------------------------------------------

audio_module_handle_t AudioFlinger::loadHwModule(const char *name)
{
    if (!settingsAllowed()) {
        return 0;
    }
    Mutex::Autolock _l(mLock);
    return loadHwModule_l(name);
}

// loadHwModule_l() must be called with AudioFlinger::mLock held
audio_module_handle_t AudioFlinger::loadHwModule_l(const char *name)
{
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        if (strncmp(mAudioHwDevs.valueAt(i)->moduleName(), name, strlen(name)) == 0) {
            ALOGW("loadHwModule() module %s already loaded", name);
            return mAudioHwDevs.keyAt(i);
        }
    }

    audio_hw_device_t *dev;

    int rc = load_audio_interface(name, &dev);
    if (rc) {
        ALOGI("loadHwModule() error %d loading module %s ", rc, name);
        return 0;
    }

    mHardwareStatus = AUDIO_HW_INIT;
    rc = dev->init_check(dev);
    mHardwareStatus = AUDIO_HW_IDLE;
    if (rc) {
        ALOGI("loadHwModule() init check error %d for module %s ", rc, name);
        return 0;
    }

    // Check and cache this HAL's level of support for master mute and master
    // volume.  If this is the first HAL opened, and it supports the get
    // methods, use the initial values provided by the HAL as the current
    // master mute and volume settings.

    AudioHwDevice::Flags flags = static_cast<AudioHwDevice::Flags>(0);
    {  // scope for auto-lock pattern
        AutoMutex lock(mHardwareLock);

#if !defined(ICS_AUDIO_BLOB) && !defined(MR0_AUDIO_BLOB)
        if (0 == mAudioHwDevs.size()) {
            mHardwareStatus = AUDIO_HW_GET_MASTER_VOLUME;
            if (NULL != dev->get_master_volume) {
                float mv;
                if (OK == dev->get_master_volume(dev, &mv)) {
                    mMasterVolume = mv;
                }
            }

            mHardwareStatus = AUDIO_HW_GET_MASTER_MUTE;
            if (NULL != dev->get_master_mute) {
                bool mm;
                if (OK == dev->get_master_mute(dev, &mm)) {
                    mMasterMute = mm;
                }
            }
        }
#endif

        mHardwareStatus = AUDIO_HW_SET_MASTER_VOLUME;
        if ((NULL != dev->set_master_volume) &&
            (OK == dev->set_master_volume(dev, mMasterVolume))) {
            flags = static_cast<AudioHwDevice::Flags>(flags |
                    AudioHwDevice::AHWD_CAN_SET_MASTER_VOLUME);
        }

#if !defined(ICS_AUDIO_BLOB) && !defined(MR0_AUDIO_BLOB)
        mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
        if ((NULL != dev->set_master_mute) &&
            (OK == dev->set_master_mute(dev, mMasterMute))) {
            flags = static_cast<AudioHwDevice::Flags>(flags |
                    AudioHwDevice::AHWD_CAN_SET_MASTER_MUTE);
        }
#endif

        mHardwareStatus = AUDIO_HW_IDLE;
    }

    audio_module_handle_t handle = nextUniqueId();
    mAudioHwDevs.add(handle, new AudioHwDevice(name, dev, flags));

    ALOGI("loadHwModule() Loaded %s audio interface from %s (%s) handle %d",
          name, dev->common.module->name, dev->common.module->id, handle);

    return handle;

}

// ----------------------------------------------------------------------------

int32_t AudioFlinger::getPrimaryOutputSamplingRate()
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = primaryPlaybackThread_l();
    return thread != NULL ? thread->sampleRate() : 0;
}

int32_t AudioFlinger::getPrimaryOutputFrameCount()
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = primaryPlaybackThread_l();
    return thread != NULL ? thread->frameCountHAL() : 0;
}

// ----------------------------------------------------------------------------

audio_io_handle_t AudioFlinger::openOutput(audio_module_handle_t module,
                                           audio_devices_t *pDevices,
                                           uint32_t *pSamplingRate,
                                           audio_format_t *pFormat,
                                           audio_channel_mask_t *pChannelMask,
                                           uint32_t *pLatencyMs,
                                           audio_output_flags_t flags)
{
    status_t status;
    PlaybackThread *thread = NULL;
    struct audio_config config = {
        sample_rate: pSamplingRate ? *pSamplingRate : 0,
        channel_mask: pChannelMask ? *pChannelMask : 0,
        format: pFormat ? *pFormat : AUDIO_FORMAT_DEFAULT,
    };
    audio_stream_out_t *outStream = NULL;
    AudioHwDevice *outHwDev;

    ALOGV("openOutput(), module %d Device %x, SamplingRate %d, Format %d, Channels %x, flags %x",
              module,
              (pDevices != NULL) ? *pDevices : 0,
              config.sample_rate,
              config.format,
              config.channel_mask,
              flags);

    if (pDevices == NULL || *pDevices == 0) {
        return 0;
    }

    Mutex::Autolock _l(mLock);

    outHwDev = findSuitableHwDev_l(module, *pDevices);
    if (outHwDev == NULL)
        return 0;

    audio_hw_device_t *hwDevHal = outHwDev->hwDevice();
    audio_io_handle_t id = nextUniqueId();

    mHardwareStatus = AUDIO_HW_OUTPUT_OPEN;

#ifndef ICS_AUDIO_BLOB
    status = hwDevHal->open_output_stream(hwDevHal,
                                          id,
                                          *pDevices,
                                          (audio_output_flags_t)flags,
                                          &config,
                                          &outStream);
#else
    status = hwDevHal->open_output_stream(hwDevHal,
                                          *pDevices,
                                          (int *)&config.format,
                                          &config.channel_mask,
                                          &config.sample_rate,
                                          &outStream);
    uint32_t newflags = flags | AUDIO_OUTPUT_FLAG_PRIMARY;
    flags = (audio_output_flags_t)newflags;
#endif

    mHardwareStatus = AUDIO_HW_IDLE;
    ALOGV("openOutput() openOutputStream returned output %p, SamplingRate %d, Format %d, Channels %x, status %d",
            outStream,
            config.sample_rate,
            config.format,
            config.channel_mask,
            status);

    if (status == NO_ERROR && outStream != NULL) {
        AudioStreamOut *output = new AudioStreamOut(outHwDev, outStream);
#ifdef QCOM_HARDWARE
        if (flags & AUDIO_OUTPUT_FLAG_LPA || flags & AUDIO_OUTPUT_FLAG_TUNNEL ) {
            AudioSessionDescriptor *desc = new AudioSessionDescriptor(hwDevHal, outStream, flags);
            desc->mActive = true;
            //TODO: no stream type
            //desc->mStreamType = streamType;
            desc->mVolumeLeft = 1.0;
            desc->mVolumeRight = 1.0;
            desc->device = *pDevices;
            mDirectAudioTracks.add(id, desc);
        } else
#endif
        if ((flags & AUDIO_OUTPUT_FLAG_DIRECT) ||
            (config.format != AUDIO_FORMAT_PCM_16_BIT) ||
            (config.channel_mask != AUDIO_CHANNEL_OUT_STEREO)) {
            thread = new DirectOutputThread(this, output, id, *pDevices);
            ALOGV("openOutput() created direct output: ID %d thread %p", id, thread);
        } else {
            thread = new MixerThread(this, output, id, *pDevices);
            ALOGV("openOutput() created mixer output: ID %d thread %p", id, thread);
        }
#ifdef QCOM_HARDWARE
        if (thread != NULL)
#endif
            mPlaybackThreads.add(id, thread);

#ifdef QCOM_HARDWARE
        // if the device is a A2DP, then this is an A2DP Output
        if ( true == audio_is_a2dp_device((audio_devices_t) *pDevices) )
        {
            mA2DPHandle = id;
            ALOGV("A2DP device activated. The handle is set to %d", mA2DPHandle);
        }
#endif

        if (pSamplingRate != NULL) *pSamplingRate = config.sample_rate;
        if (pFormat != NULL) *pFormat = config.format;
        if (pChannelMask != NULL) *pChannelMask = config.channel_mask;
#ifdef QCOM_HARDWARE
        if (thread != NULL) {
#endif
            if (pLatencyMs != NULL) *pLatencyMs = thread->latency();
            // notify client processes of the new output creation
            thread->audioConfigChanged_l(AudioSystem::OUTPUT_OPENED);
#ifdef QCOM_HARDWARE
        }
        else {
            *pLatencyMs = 0;
            if ((flags & AUDIO_OUTPUT_FLAG_LPA) || (flags & AUDIO_OUTPUT_FLAG_TUNNEL)) {
                AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(id);
                *pLatencyMs = desc->stream->get_latency(desc->stream);
            }
        }
#endif
        // the first primary output opened designates the primary hw device
        if ((mPrimaryHardwareDev == NULL) && (flags & AUDIO_OUTPUT_FLAG_PRIMARY)) {
            ALOGI("Using module %d has the primary audio interface", module);
            mPrimaryHardwareDev = outHwDev;

#ifdef SRS_PROCESSING
            SRS_Processing::RawDataSet(NULL, "qdsp hook", &mPrimaryHardwareDev,
                sizeof(&mPrimaryHardwareDev));
#endif
            AutoMutex lock(mHardwareLock);
            mHardwareStatus = AUDIO_HW_SET_MODE;
            hwDevHal->set_mode(hwDevHal, mMode);
            mHardwareStatus = AUDIO_HW_IDLE;
        }
        return id;
    }

    return 0;
}

audio_io_handle_t AudioFlinger::openDuplicateOutput(audio_io_handle_t output1,
        audio_io_handle_t output2)
{
    Mutex::Autolock _l(mLock);
    MixerThread *thread1 = checkMixerThread_l(output1);
    MixerThread *thread2 = checkMixerThread_l(output2);

    if (thread1 == NULL || thread2 == NULL) {
        ALOGW("openDuplicateOutput() wrong output mixer type for output %d or %d", output1, output2);
        return 0;
    }

    audio_io_handle_t id = nextUniqueId();
    DuplicatingThread *thread = new DuplicatingThread(this, thread1, id);
    thread->addOutputTrack(thread2);
    mPlaybackThreads.add(id, thread);
    // notify client processes of the new output creation
    thread->audioConfigChanged_l(AudioSystem::OUTPUT_OPENED);
    return id;
}

status_t AudioFlinger::closeOutput(audio_io_handle_t output)
{
    return closeOutput_nonvirtual(output);
}

status_t AudioFlinger::closeOutput_nonvirtual(audio_io_handle_t output)
{
    // keep strong reference on the playback thread so that
    // it is not destroyed while exit() is executed
#ifdef QCOM_HARDWARE
    AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(output);
    if (desc) {
        ALOGV("Closing DirectTrack output %d", output);
        desc->mActive = false;
        desc->stream->common.standby(&desc->stream->common);
        desc->hwDev->close_output_stream(desc->hwDev, desc->stream);
        desc->trackRefPtr = NULL;
        mDirectAudioTracks.removeItem(output);
        audioConfigChanged_l(AudioSystem::OUTPUT_CLOSED, output, NULL);
        delete desc;
        return NO_ERROR;
    }
#endif

    sp<PlaybackThread> thread;
    {
        Mutex::Autolock _l(mLock);
        thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            return BAD_VALUE;
        }

        ALOGV("closeOutput() %d", output);

        if (thread->type() == ThreadBase::MIXER) {
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                if (mPlaybackThreads.valueAt(i)->type() == ThreadBase::DUPLICATING) {
                    DuplicatingThread *dupThread = (DuplicatingThread *)mPlaybackThreads.valueAt(i).get();
                    dupThread->removeOutputTrack((MixerThread *)thread.get());
                }
            }
        }
        audioConfigChanged_l(AudioSystem::OUTPUT_CLOSED, output, NULL);
        mPlaybackThreads.removeItem(output);
#ifdef QCOM_HARDWARE
        if (mA2DPHandle == output)
        {
            mA2DPHandle = -1;
            ALOGV("A2DP OutputClosed Notifying Client");
            audioConfigChanged_l(AudioSystem::A2DP_OUTPUT_STATE, mA2DPHandle, &mA2DPHandle);
        }
#endif
    }
    thread->exit();
    // The thread entity (active unit of execution) is no longer running here,
    // but the ThreadBase container still exists.

    if (thread->type() != ThreadBase::DUPLICATING) {
        AudioStreamOut *out = thread->clearOutput();
        ALOG_ASSERT(out != NULL, "out shouldn't be NULL");
        // from now on thread->mOutput is NULL
        out->hwDev()->close_output_stream(out->hwDev(), out->stream);
        delete out;
    }
    return NO_ERROR;
}

status_t AudioFlinger::suspendOutput(audio_io_handle_t output)
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);

    if (thread == NULL) {
        return BAD_VALUE;
    }

    ALOGV("suspendOutput() %d", output);
    thread->suspend();

    return NO_ERROR;
}

status_t AudioFlinger::restoreOutput(audio_io_handle_t output)
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = checkPlaybackThread_l(output);

    if (thread == NULL) {
        return BAD_VALUE;
    }

    ALOGV("restoreOutput() %d", output);

    thread->restore();

    return NO_ERROR;
}

audio_io_handle_t AudioFlinger::openInput(audio_module_handle_t module,
                                          audio_devices_t *pDevices,
                                          uint32_t *pSamplingRate,
                                          audio_format_t *pFormat,
                                          audio_channel_mask_t *pChannelMask)
{
    status_t status;
    RecordThread *thread = NULL;
    struct audio_config config = {
        sample_rate: pSamplingRate ? *pSamplingRate : 0,
        channel_mask: pChannelMask ? *pChannelMask : 0,
        format: pFormat ? *pFormat : AUDIO_FORMAT_DEFAULT,
    };
    uint32_t reqSamplingRate = config.sample_rate;
    audio_format_t reqFormat = config.format;
    audio_channel_mask_t reqChannels = config.channel_mask;
    audio_stream_in_t *inStream = NULL;
    AudioHwDevice *inHwDev;

    if (pDevices == NULL || *pDevices == 0) {
        return 0;
    }

    Mutex::Autolock _l(mLock);

    inHwDev = findSuitableHwDev_l(module, *pDevices);
    if (inHwDev == NULL)
        return 0;

    audio_hw_device_t *inHwHal = inHwDev->hwDevice();
    audio_io_handle_t id = nextUniqueId();

#ifndef ICS_AUDIO_BLOB
    status = inHwHal->open_input_stream(inHwHal, id, *pDevices, &config,
                                        &inStream);
#else
    status = inHwHal->open_input_stream(inHwHal, *pDevices, 
                                        (int *)&config.format, 
                                        &config.channel_mask,
                                        &config.sample_rate, (audio_in_acoustics_t)0,
                                        &inStream);
#endif
    ALOGV("openInput() openInputStream returned input %p, SamplingRate %d, Format %d, Channels %x, status %d",
            inStream,
            config.sample_rate,
            config.format,
            config.channel_mask,
            status);

    // If the input could not be opened with the requested parameters and we can handle the conversion internally,
    // try to open again with the proposed parameters. The AudioFlinger can resample the input and do mono to stereo
    // or stereo to mono conversions on 16 bit PCM inputs.
    if (status == BAD_VALUE &&
        reqFormat == config.format && config.format == AUDIO_FORMAT_PCM_16_BIT &&
        (config.sample_rate <= 2 * reqSamplingRate) &&
        (getInputChannelCount(config.channel_mask) <= FCC_2) && (getInputChannelCount(reqChannels) <= FCC_2)) {
        ALOGV("openInput() reopening with proposed sampling rate and channel mask");
        inStream = NULL;
#ifndef ICS_AUDIO_BLOB
        status = inHwHal->open_input_stream(inHwHal, id, *pDevices, &config, &inStream);
#else
        status = inHwHal->open_input_stream(inHwHal, *pDevices, 
                                        (int *)&config.format, 
                                        &config.channel_mask,
                                        &config.sample_rate, (audio_in_acoustics_t)0,
                                        &inStream);
#endif
    }

    if (status == NO_ERROR && inStream != NULL) {
        AudioStreamIn *input = new AudioStreamIn(inHwDev, inStream);

        // Start record thread
        // RecorThread require both input and output device indication to forward to audio
        // pre processing modules
        audio_devices_t device = (*pDevices) | primaryOutputDevice_l();
        thread = new RecordThread(this,
                                  input,
                                  reqSamplingRate,
                                  reqChannels,
                                  id,
                                  device);
        mRecordThreads.add(id, thread);
        ALOGV("openInput() created record thread: ID %d thread %p", id, thread);
        if (pSamplingRate != NULL) *pSamplingRate = reqSamplingRate;
        if (pFormat != NULL) *pFormat = config.format;
        if (pChannelMask != NULL) *pChannelMask = reqChannels;

        // notify client processes of the new input creation
        thread->audioConfigChanged_l(AudioSystem::INPUT_OPENED);
        return id;
    }

    return 0;
}

status_t AudioFlinger::closeInput(audio_io_handle_t input)
{
    return closeInput_nonvirtual(input);
}

status_t AudioFlinger::closeInput_nonvirtual(audio_io_handle_t input)
{
    // keep strong reference on the record thread so that
    // it is not destroyed while exit() is executed
    sp<RecordThread> thread;
    {
        Mutex::Autolock _l(mLock);
        thread = checkRecordThread_l(input);
        if (thread == 0) {
            return BAD_VALUE;
        }

        ALOGV("closeInput() %d", input);
        audioConfigChanged_l(AudioSystem::INPUT_CLOSED, input, NULL);
        mRecordThreads.removeItem(input);
    }
    thread->exit();
    // The thread entity (active unit of execution) is no longer running here,
    // but the ThreadBase container still exists.

    AudioStreamIn *in = thread->clearInput();
    ALOG_ASSERT(in != NULL, "in shouldn't be NULL");
    // from now on thread->mInput is NULL
    in->hwDev()->close_input_stream(in->hwDev(), in->stream);
    delete in;

    return NO_ERROR;
}

status_t AudioFlinger::setStreamOutput(audio_stream_type_t stream, audio_io_handle_t output)
{
    Mutex::Autolock _l(mLock);
    ALOGV("setStreamOutput() stream %d to output %d", stream, output);

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
        thread->invalidateTracks(stream);
    }
#ifdef QCOM_HARDWARE
    if ( mA2DPHandle == output ) {
        ALOGV("A2DP Activated and hence notifying the client");
        audioConfigChanged_l(AudioSystem::A2DP_OUTPUT_STATE, mA2DPHandle, &output);
    }
#endif

    return NO_ERROR;
}


int AudioFlinger::newAudioSessionId()
{
    return nextUniqueId();
}

void AudioFlinger::acquireAudioSessionId(int audioSession)
{
    Mutex::Autolock _l(mLock);
    pid_t caller = IPCThreadState::self()->getCallingPid();
    ALOGV("acquiring %d from %d", audioSession, caller);
    size_t num = mAudioSessionRefs.size();
    for (size_t i = 0; i< num; i++) {
        AudioSessionRef *ref = mAudioSessionRefs.editItemAt(i);
        if (ref->mSessionid == audioSession && ref->mPid == caller) {
            ref->mCnt++;
            ALOGV(" incremented refcount to %d", ref->mCnt);
            return;
        }
    }
    mAudioSessionRefs.push(new AudioSessionRef(audioSession, caller));
    ALOGV(" added new entry for %d", audioSession);
}

void AudioFlinger::releaseAudioSessionId(int audioSession)
{
    Mutex::Autolock _l(mLock);
    pid_t caller = IPCThreadState::self()->getCallingPid();
    ALOGV("releasing %d from %d", audioSession, caller);
    size_t num = mAudioSessionRefs.size();
    for (size_t i = 0; i< num; i++) {
        AudioSessionRef *ref = mAudioSessionRefs.itemAt(i);
        if (ref->mSessionid == audioSession && ref->mPid == caller) {
            ref->mCnt--;
            ALOGV(" decremented refcount to %d", ref->mCnt);
            if (ref->mCnt == 0) {
                mAudioSessionRefs.removeAt(i);
                delete ref;
                purgeStaleEffects_l();
            }
            return;
        }
    }
    ALOGW("session id %d not found for pid %d", audioSession, caller);
}

void AudioFlinger::purgeStaleEffects_l() {

    ALOGV("purging stale effects");

    Vector< sp<EffectChain> > chains;

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<PlaybackThread> t = mPlaybackThreads.valueAt(i);
        for (size_t j = 0; j < t->mEffectChains.size(); j++) {
            sp<EffectChain> ec = t->mEffectChains[j];
            if (ec->sessionId() > AUDIO_SESSION_OUTPUT_MIX) {
                chains.push(ec);
            }
        }
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        sp<RecordThread> t = mRecordThreads.valueAt(i);
        for (size_t j = 0; j < t->mEffectChains.size(); j++) {
            sp<EffectChain> ec = t->mEffectChains[j];
            chains.push(ec);
        }
    }

    for (size_t i = 0; i < chains.size(); i++) {
        sp<EffectChain> ec = chains[i];
        int sessionid = ec->sessionId();
        sp<ThreadBase> t = ec->mThread.promote();
        if (t == 0) {
            continue;
        }
        size_t numsessionrefs = mAudioSessionRefs.size();
        bool found = false;
        for (size_t k = 0; k < numsessionrefs; k++) {
            AudioSessionRef *ref = mAudioSessionRefs.itemAt(k);
            if (ref->mSessionid == sessionid) {
                ALOGV(" session %d still exists for %d with %d refs",
                    sessionid, ref->mPid, ref->mCnt);
                found = true;
                break;
            }
        }
        if (!found) {
            Mutex::Autolock _l (t->mLock);
            // remove all effects from the chain
            while (ec->mEffects.size()) {
                sp<EffectModule> effect = ec->mEffects[0];
                effect->unPin();
                t->removeEffect_l(effect);
                if (effect->purgeHandles()) {
                    t->checkSuspendOnEffectEnabled_l(effect, false, effect->sessionId());
                }
                AudioSystem::unregisterEffect(effect->id());
            }
        }
    }
    return;
}

// checkPlaybackThread_l() must be called with AudioFlinger::mLock held
AudioFlinger::PlaybackThread *AudioFlinger::checkPlaybackThread_l(audio_io_handle_t output) const
{
    return mPlaybackThreads.valueFor(output).get();
}

// checkMixerThread_l() must be called with AudioFlinger::mLock held
AudioFlinger::MixerThread *AudioFlinger::checkMixerThread_l(audio_io_handle_t output) const
{
    PlaybackThread *thread = checkPlaybackThread_l(output);
    return thread != NULL && thread->type() != ThreadBase::DIRECT ? (MixerThread *) thread : NULL;
}

// checkRecordThread_l() must be called with AudioFlinger::mLock held
AudioFlinger::RecordThread *AudioFlinger::checkRecordThread_l(audio_io_handle_t input) const
{
    return mRecordThreads.valueFor(input).get();
}

uint32_t AudioFlinger::nextUniqueId()
{
    return android_atomic_inc(&mNextUniqueId);
}

AudioFlinger::PlaybackThread *AudioFlinger::primaryPlaybackThread_l() const
{
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
        AudioStreamOut *output = thread->getOutput();
        if (output != NULL && output->audioHwDev == mPrimaryHardwareDev) {
            return thread;
        }
    }
    return NULL;
}

audio_devices_t AudioFlinger::primaryOutputDevice_l() const
{
    PlaybackThread *thread = primaryPlaybackThread_l();

    if (thread == NULL) {
        return 0;
    }

    return thread->outDevice();
}

sp<AudioFlinger::SyncEvent> AudioFlinger::createSyncEvent(AudioSystem::sync_event_t type,
                                    int triggerSession,
                                    int listenerSession,
                                    sync_event_callback_t callBack,
                                    void *cookie)
{
    Mutex::Autolock _l(mLock);

    sp<SyncEvent> event = new SyncEvent(type, triggerSession, listenerSession, callBack, cookie);
    status_t playStatus = NAME_NOT_FOUND;
    status_t recStatus = NAME_NOT_FOUND;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        playStatus = mPlaybackThreads.valueAt(i)->setSyncEvent(event);
        if (playStatus == NO_ERROR) {
            return event;
        }
    }
    for (size_t i = 0; i < mRecordThreads.size(); i++) {
        recStatus = mRecordThreads.valueAt(i)->setSyncEvent(event);
        if (recStatus == NO_ERROR) {
            return event;
        }
    }
    if (playStatus == NAME_NOT_FOUND || recStatus == NAME_NOT_FOUND) {
        mPendingSyncEvents.add(event);
    } else {
        ALOGV("createSyncEvent() invalid event %d", event->type());
        event.clear();
    }
    return event;
}

// ----------------------------------------------------------------------------
//  Effect management
// ----------------------------------------------------------------------------


status_t AudioFlinger::queryNumberEffects(uint32_t *numEffects) const
{
    Mutex::Autolock _l(mLock);
    return EffectQueryNumberEffects(numEffects);
}

status_t AudioFlinger::queryEffect(uint32_t index, effect_descriptor_t *descriptor) const
{
    Mutex::Autolock _l(mLock);
    return EffectQueryEffect(index, descriptor);
}

status_t AudioFlinger::getEffectDescriptor(const effect_uuid_t *pUuid,
        effect_descriptor_t *descriptor) const
{
    Mutex::Autolock _l(mLock);
    return EffectGetDescriptor(pUuid, descriptor);
}


sp<IEffect> AudioFlinger::createEffect(pid_t pid,
        effect_descriptor_t *pDesc,
        const sp<IEffectClient>& effectClient,
        int32_t priority,
        audio_io_handle_t io,
        int sessionId,
        status_t *status,
        int *id,
        int *enabled)
{
    status_t lStatus = NO_ERROR;
    sp<EffectHandle> handle;
    effect_descriptor_t desc;

    ALOGV("createEffect pid %d, effectClient %p, priority %d, sessionId %d, io %d",
            pid, effectClient.get(), priority, sessionId, io);

    if (pDesc == NULL) {
        lStatus = BAD_VALUE;
        goto Exit;
    }

    // check audio settings permission for global effects
    if (sessionId == AUDIO_SESSION_OUTPUT_MIX && !settingsAllowed()) {
        lStatus = PERMISSION_DENIED;
        goto Exit;
    }

    // Session AUDIO_SESSION_OUTPUT_STAGE is reserved for output stage effects
    // that can only be created by audio policy manager (running in same process)
    if (sessionId == AUDIO_SESSION_OUTPUT_STAGE && getpid_cached != pid) {
        lStatus = PERMISSION_DENIED;
        goto Exit;
    }

    if (io == 0) {
        if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
            // output must be specified by AudioPolicyManager when using session
            // AUDIO_SESSION_OUTPUT_STAGE
            lStatus = BAD_VALUE;
            goto Exit;
        } else if (sessionId == AUDIO_SESSION_OUTPUT_MIX) {
            // if the output returned by getOutputForEffect() is removed before we lock the
            // mutex below, the call to checkPlaybackThread_l(io) below will detect it
            // and we will exit safely
            io = AudioSystem::getOutputForEffect(&desc);
        }
    }

    {
        Mutex::Autolock _l(mLock);


        if (!EffectIsNullUuid(&pDesc->uuid)) {
            // if uuid is specified, request effect descriptor
            lStatus = EffectGetDescriptor(&pDesc->uuid, &desc);
            if (lStatus < 0) {
                ALOGW("createEffect() error %d from EffectGetDescriptor", lStatus);
                goto Exit;
            }
        } else {
            // if uuid is not specified, look for an available implementation
            // of the required type in effect factory
            if (EffectIsNullUuid(&pDesc->type)) {
                ALOGW("createEffect() no effect type");
                lStatus = BAD_VALUE;
                goto Exit;
            }
            uint32_t numEffects = 0;
            effect_descriptor_t d;
            d.flags = 0; // prevent compiler warning
            bool found = false;

            lStatus = EffectQueryNumberEffects(&numEffects);
            if (lStatus < 0) {
                ALOGW("createEffect() error %d from EffectQueryNumberEffects", lStatus);
                goto Exit;
            }
            for (uint32_t i = 0; i < numEffects; i++) {
                lStatus = EffectQueryEffect(i, &desc);
                if (lStatus < 0) {
                    ALOGW("createEffect() error %d from EffectQueryEffect", lStatus);
                    continue;
                }
                if (memcmp(&desc.type, &pDesc->type, sizeof(effect_uuid_t)) == 0) {
                    // If matching type found save effect descriptor. If the session is
                    // 0 and the effect is not auxiliary, continue enumeration in case
                    // an auxiliary version of this effect type is available
                    found = true;
                    d = desc;
                    if (sessionId != AUDIO_SESSION_OUTPUT_MIX ||
                            (desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
                        break;
                    }
                }
            }
            if (!found) {
                lStatus = BAD_VALUE;
                ALOGW("createEffect() effect not found");
                goto Exit;
            }
            // For same effect type, chose auxiliary version over insert version if
            // connect to output mix (Compliance to OpenSL ES)
            if (sessionId == AUDIO_SESSION_OUTPUT_MIX &&
                    (d.flags & EFFECT_FLAG_TYPE_MASK) != EFFECT_FLAG_TYPE_AUXILIARY) {
                desc = d;
            }
        }

        // Do not allow auxiliary effects on a session different from 0 (output mix)
        if (sessionId != AUDIO_SESSION_OUTPUT_MIX &&
             (desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
            lStatus = INVALID_OPERATION;
            goto Exit;
        }

        // check recording permission for visualizer
        if ((memcmp(&desc.type, SL_IID_VISUALIZATION, sizeof(effect_uuid_t)) == 0) &&
            !recordingAllowed()) {
            lStatus = PERMISSION_DENIED;
            goto Exit;
        }

        // return effect descriptor
        *pDesc = desc;

        // If output is not specified try to find a matching audio session ID in one of the
        // output threads.
        // If output is 0 here, sessionId is neither SESSION_OUTPUT_STAGE nor SESSION_OUTPUT_MIX
        // because of code checking output when entering the function.
        // Note: io is never 0 when creating an effect on an input
        if (io == 0) {
            // look for the thread where the specified audio session is present
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                if (mPlaybackThreads.valueAt(i)->hasAudioSession(sessionId) != 0) {
                    io = mPlaybackThreads.keyAt(i);
                    break;
                }
            }
            if (io == 0) {
                for (size_t i = 0; i < mRecordThreads.size(); i++) {
                    if (mRecordThreads.valueAt(i)->hasAudioSession(sessionId) != 0) {
                        io = mRecordThreads.keyAt(i);
                        break;
                    }
                }
            }
            // If no output thread contains the requested session ID, default to
            // first output. The effect chain will be moved to the correct output
            // thread when a track with the same session ID is created
            if (io == 0 && mPlaybackThreads.size()) {
                io = mPlaybackThreads.keyAt(0);
            }
            ALOGV("createEffect() got io %d for effect %s", io, desc.name);
        }
        ThreadBase *thread = checkRecordThread_l(io);
        if (thread == NULL) {
            thread = checkPlaybackThread_l(io);
            if (thread == NULL) {
                ALOGE("createEffect() unknown output thread");
                lStatus = BAD_VALUE;
                goto Exit;
            }
        }

        sp<Client> client = registerPid_l(pid);

        // create effect on selected output thread
        handle = thread->createEffect_l(client, effectClient, priority, sessionId,
                &desc, enabled, &lStatus);
        if (handle != 0 && id != NULL) {
            *id = handle->id();
        }
    }

Exit:
    if (status != NULL) {
        *status = lStatus;
    }
    return handle;
}

status_t AudioFlinger::moveEffects(int sessionId, audio_io_handle_t srcOutput,
        audio_io_handle_t dstOutput)
{
    ALOGV("moveEffects() session %d, srcOutput %d, dstOutput %d",
            sessionId, srcOutput, dstOutput);
    Mutex::Autolock _l(mLock);
    if (srcOutput == dstOutput) {
        ALOGW("moveEffects() same dst and src outputs %d", dstOutput);
        return NO_ERROR;
    }
    PlaybackThread *srcThread = checkPlaybackThread_l(srcOutput);
    if (srcThread == NULL) {
        ALOGW("moveEffects() bad srcOutput %d", srcOutput);
        return BAD_VALUE;
    }
    PlaybackThread *dstThread = checkPlaybackThread_l(dstOutput);
    if (dstThread == NULL) {
        ALOGW("moveEffects() bad dstOutput %d", dstOutput);
        return BAD_VALUE;
    }

    Mutex::Autolock _dl(dstThread->mLock);
    Mutex::Autolock _sl(srcThread->mLock);
    moveEffectChain_l(sessionId, srcThread, dstThread, false);

    return NO_ERROR;
}

// moveEffectChain_l must be called with both srcThread and dstThread mLocks held
status_t AudioFlinger::moveEffectChain_l(int sessionId,
                                   AudioFlinger::PlaybackThread *srcThread,
                                   AudioFlinger::PlaybackThread *dstThread,
                                   bool reRegister)
{
    ALOGV("moveEffectChain_l() session %d from thread %p to thread %p",
            sessionId, srcThread, dstThread);

    sp<EffectChain> chain = srcThread->getEffectChain_l(sessionId);
    if (chain == 0) {
        ALOGW("moveEffectChain_l() effect chain for session %d not on source thread %p",
                sessionId, srcThread);
        return INVALID_OPERATION;
    }

    // remove chain first. This is useful only if reconfiguring effect chain on same output thread,
    // so that a new chain is created with correct parameters when first effect is added. This is
    // otherwise unnecessary as removeEffect_l() will remove the chain when last effect is
    // removed.
    srcThread->removeEffectChain_l(chain);

    // transfer all effects one by one so that new effect chain is created on new thread with
    // correct buffer sizes and audio parameters and effect engines reconfigured accordingly
    audio_io_handle_t dstOutput = dstThread->id();
    sp<EffectChain> dstChain;
    uint32_t strategy = 0; // prevent compiler warning
    sp<EffectModule> effect = chain->getEffectFromId_l(0);
    while (effect != 0) {
        srcThread->removeEffect_l(effect);
        dstThread->addEffect_l(effect);
        // removeEffect_l() has stopped the effect if it was active so it must be restarted
        if (effect->state() == EffectModule::ACTIVE ||
                effect->state() == EffectModule::STOPPING) {
            effect->start();
        }
        // if the move request is not received from audio policy manager, the effect must be
        // re-registered with the new strategy and output
        if (dstChain == 0) {
            dstChain = effect->chain().promote();
            if (dstChain == 0) {
                ALOGW("moveEffectChain_l() cannot get chain from effect %p", effect.get());
                srcThread->addEffect_l(effect);
                return NO_INIT;
            }
            strategy = dstChain->strategy();
        }
        if (reRegister) {
            AudioSystem::unregisterEffect(effect->id());
            AudioSystem::registerEffect(&effect->desc(),
                                        dstOutput,
                                        strategy,
                                        sessionId,
                                        effect->id());
        }
        effect = chain->getEffectFromId_l(0);
    }

    return NO_ERROR;
}


// PlaybackThread::createEffect_l() must be called with AudioFlinger::mLock held
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

            effect->setDevice(mOutDevice);
            effect->setDevice(mInDevice);
            effect->setMode(mAudioFlinger->getMode());
            effect->setAudioSource(mAudioSource);
#ifdef QCOM_HARDWARE
            if(chain == mAudioFlinger->mLPAEffectChain) {
                effect->setLPAFlag(true);
            }
#endif
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
#ifdef QCOM_HARDWARE
    mAudioFlinger->mAllChainsLocked = true;
#endif
    for (size_t i = 0; i < mEffectChains.size(); i++) {
#ifdef QCOM_HARDWARE
        if (mEffectChains[i] != mAudioFlinger->mLPAEffectChain) {
#endif
            mEffectChains[i]->lock();
#ifdef QCOM_HARDWARE
        } else {
            mAudioFlinger-> mAllChainsLocked = false;
        }
#endif
    }
}

void AudioFlinger::ThreadBase::unlockEffectChains(
        const Vector< sp<AudioFlinger::EffectChain> >& effectChains)
{
    for (size_t i = 0; i < effectChains.size(); i++) {
#ifdef QCOM_HARDWARE
        if (mAudioFlinger-> mAllChainsLocked || mEffectChains[i] != mAudioFlinger->mLPAEffectChain)
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
                ALOGV("addEffectChain_l() track->setMainBuffer track %p buffer %p", track.get(), buffer);
                track->setMainBuffer(buffer);
                chain->incTrackCnt();
            }
        }

        // indicate all active tracks in the chain
        for (size_t i = 0 ; i < mActiveTracks.size() ; ++i) {
            sp<Track> track = mActiveTracks[i].promote();
            if (track == 0) continue;
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
        if (mEffectChains[i]->sessionId() < session) break;
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
                if (track == 0) continue;
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
//  EffectModule implementation
// ----------------------------------------------------------------------------

#undef LOG_TAG
#define LOG_TAG "AudioFlinger::EffectModule"

AudioFlinger::EffectModule::EffectModule(ThreadBase *thread,
                                        const wp<AudioFlinger::EffectChain>& chain,
                                        effect_descriptor_t *desc,
                                        int id,
                                        int sessionId)
    : mPinned(sessionId > AUDIO_SESSION_OUTPUT_MIX),
      mThread(thread), mChain(chain), mId(id), mSessionId(sessionId),
      mDescriptor(*desc),
      // mConfig is set by configure() and not used before then
      mEffectInterface(NULL),
      mStatus(NO_INIT), mState(IDLE),
      // mMaxDisableWaitCnt is set by configure() and not used before then
      // mDisableWaitCnt is set by process() and updateState() and not used before then
      mSuspended(false)
#ifdef QCOM_HARDWARE
      ,mIsForLPA(false)
#endif
{
    ALOGV("Constructor %p", this);
    int lStatus;

    // create effect engine from effect factory
    mStatus = EffectCreate(&desc->uuid, sessionId, thread->id(), &mEffectInterface);

    if (mStatus != NO_ERROR) {
        return;
    }
    lStatus = init();
    if (lStatus < 0) {
        mStatus = lStatus;
        goto Error;
    }

    ALOGV("Constructor success name %s, Interface %p", mDescriptor.name, mEffectInterface);
    return;
Error:
    EffectRelease(mEffectInterface);
    mEffectInterface = NULL;
    ALOGV("Constructor Error %d", mStatus);
}

AudioFlinger::EffectModule::~EffectModule()
{
    ALOGV("Destructor %p", this);
    if (mEffectInterface != NULL) {
        if ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC ||
                (mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_POST_PROC) {
            sp<ThreadBase> thread = mThread.promote();
            if (thread != 0) {
                audio_stream_t *stream = thread->stream();
                if (stream != NULL) {
                    stream->remove_audio_effect(stream, mEffectInterface);
                }
            }
        }
        // release effect engine
        EffectRelease(mEffectInterface);
    }
}

status_t AudioFlinger::EffectModule::addHandle(EffectHandle *handle)
{
    status_t status;

    Mutex::Autolock _l(mLock);
    int priority = handle->priority();
    size_t size = mHandles.size();
    EffectHandle *controlHandle = NULL;
    size_t i;
    for (i = 0; i < size; i++) {
        EffectHandle *h = mHandles[i];
        if (h == NULL || h->destroyed_l()) continue;
        // first non destroyed handle is considered in control
        if (controlHandle == NULL)
            controlHandle = h;
        if (h->priority() <= priority) break;
    }
    // if inserted in first place, move effect control from previous owner to this handle
    if (i == 0) {
        bool enabled = false;
        if (controlHandle != NULL) {
            enabled = controlHandle->enabled();
            controlHandle->setControl(false/*hasControl*/, true /*signal*/, enabled /*enabled*/);
        }
        handle->setControl(true /*hasControl*/, false /*signal*/, enabled /*enabled*/);
        status = NO_ERROR;
    } else {
        status = ALREADY_EXISTS;
    }
    ALOGV("addHandle() %p added handle %p in position %d", this, handle, i);
    mHandles.insertAt(handle, i);
    return status;
}

size_t AudioFlinger::EffectModule::removeHandle(EffectHandle *handle)
{
    Mutex::Autolock _l(mLock);
    size_t size = mHandles.size();
    size_t i;
    for (i = 0; i < size; i++) {
        if (mHandles[i] == handle) break;
    }
    if (i == size) {
        return size;
    }
    ALOGV("removeHandle() %p removed handle %p in position %d", this, handle, i);

    mHandles.removeAt(i);
    // if removed from first place, move effect control from this handle to next in line
    if (i == 0) {
        EffectHandle *h = controlHandle_l();
        if (h != NULL) {
            h->setControl(true /*hasControl*/, true /*signal*/ , handle->enabled() /*enabled*/);
        }
    }

    // Prevent calls to process() and other functions on effect interface from now on.
    // The effect engine will be released by the destructor when the last strong reference on
    // this object is released which can happen after next process is called.
    if (mHandles.size() == 0 && !mPinned) {
        mState = DESTROYED;
    }

    return mHandles.size();
}

// must be called with EffectModule::mLock held
AudioFlinger::EffectHandle *AudioFlinger::EffectModule::controlHandle_l()
{
    // the first valid handle in the list has control over the module
    for (size_t i = 0; i < mHandles.size(); i++) {
        EffectHandle *h = mHandles[i];
        if (h != NULL && !h->destroyed_l()) {
            return h;
        }
    }

    return NULL;
}

size_t AudioFlinger::EffectModule::disconnect(EffectHandle *handle, bool unpinIfLast)
{
#ifdef QCOM_HARDWARE
    setEnabled(false);
#endif
    ALOGV("disconnect() %p handle %p", this, handle);
    // keep a strong reference on this EffectModule to avoid calling the
    // destructor before we exit
    sp<EffectModule> keep(this);
    {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            thread->disconnectEffect(keep, handle, unpinIfLast);
        }
    }
    return mHandles.size();
}

void AudioFlinger::EffectModule::updateState() {
    Mutex::Autolock _l(mLock);

    switch (mState) {
    case RESTART:
        reset_l();
        // FALL THROUGH

    case STARTING:
        // clear auxiliary effect input buffer for next accumulation
        if ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
            memset(mConfig.inputCfg.buffer.raw,
                   0,
                   mConfig.inputCfg.buffer.frameCount*sizeof(int32_t));
        }
        start_l();
        mState = ACTIVE;
        break;
    case STOPPING:
        stop_l();
        mDisableWaitCnt = mMaxDisableWaitCnt;
        mState = STOPPED;
        break;
    case STOPPED:
        // mDisableWaitCnt is forced to 1 by process() when the engine indicates the end of the
        // turn off sequence.
        if (--mDisableWaitCnt == 0) {
            reset_l();
            mState = IDLE;
        }
        break;
    default: //IDLE , ACTIVE, DESTROYED
        break;
    }
}

void AudioFlinger::EffectModule::process()
{
    Mutex::Autolock _l(mLock);

    if (mState == DESTROYED || mEffectInterface == NULL ||
            mConfig.inputCfg.buffer.raw == NULL ||
            mConfig.outputCfg.buffer.raw == NULL) {
        return;
    }

    if (isProcessEnabled()) {
        // do 32 bit to 16 bit conversion for auxiliary effect input buffer
        if ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
            ditherAndClamp(mConfig.inputCfg.buffer.s32,
                                        mConfig.inputCfg.buffer.s32,
                                        mConfig.inputCfg.buffer.frameCount/2);
        }

        // do the actual processing in the effect engine
        int ret = (*mEffectInterface)->process(mEffectInterface,
                                               &mConfig.inputCfg.buffer,
                                               &mConfig.outputCfg.buffer);

        // force transition to IDLE state when engine is ready
        if (mState == STOPPED && ret == -ENODATA) {
            mDisableWaitCnt = 1;
        }

        // clear auxiliary effect input buffer for next accumulation
        if ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
            memset(mConfig.inputCfg.buffer.raw, 0,
                   mConfig.inputCfg.buffer.frameCount*sizeof(int32_t));
        }
    } else if ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_INSERT &&
                mConfig.inputCfg.buffer.raw != mConfig.outputCfg.buffer.raw) {
        // If an insert effect is idle and input buffer is different from output buffer,
        // accumulate input onto output
        sp<EffectChain> chain = mChain.promote();
        if (chain != 0 && chain->activeTrackCnt() != 0) {
            size_t frameCnt = mConfig.inputCfg.buffer.frameCount * 2;  //always stereo here
            int16_t *in = mConfig.inputCfg.buffer.s16;
            int16_t *out = mConfig.outputCfg.buffer.s16;
            for (size_t i = 0; i < frameCnt; i++) {
                out[i] = clamp16((int32_t)out[i] + (int32_t)in[i]);
            }
        }
    }
}

void AudioFlinger::EffectModule::reset_l()
{
    if (mEffectInterface == NULL) {
        return;
    }
    (*mEffectInterface)->command(mEffectInterface, EFFECT_CMD_RESET, 0, NULL, 0, NULL);
}

#ifndef QCOM_HARDWARE
status_t AudioFlinger::EffectModule::configure()
{
#else
status_t AudioFlinger::EffectModule::configure(bool isForLPA, int sampleRate, int channelCount, int frameCount)
{
    uint32_t channels;

    // Acquire lock here to make sure that any other thread does not delete
    // the effect handle and release the effect module.
    Mutex::Autolock _l(mLock);
#endif

    if (mEffectInterface == NULL) {
        return NO_INIT;
    }

    sp<ThreadBase> thread = mThread.promote();
    if (thread == 0) {
        return DEAD_OBJECT;
    }

    // TODO: handle configuration of effects replacing track process
    audio_channel_mask_t channelMask = thread->channelMask();
#ifdef QCOM_HARDWARE
    mIsForLPA = isForLPA;
    if(isForLPA) {
        if (channelCount == 1) {
            channels = AUDIO_CHANNEL_OUT_MONO;
        } else {
            channels = AUDIO_CHANNEL_OUT_STEREO;
        }
//        ALOGV("%s: LPA ON - channels %d", __func__, channels);
    } else {
        if (thread->channelCount() == 1) {
            channels = AUDIO_CHANNEL_OUT_MONO;
        } else {
            channels = AUDIO_CHANNEL_OUT_STEREO;
        }
    }
#endif

    if ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
        mConfig.inputCfg.channels = AUDIO_CHANNEL_OUT_MONO;
    } else {
        mConfig.inputCfg.channels = channelMask;
    }
    mConfig.outputCfg.channels = channelMask;
    mConfig.inputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    mConfig.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
#ifdef QCOM_HARDWARE
    if(isForLPA){
        mConfig.inputCfg.samplingRate = sampleRate;
        ALOGV("%s: LPA ON - sampleRate %d", __func__, sampleRate);
    } else
#endif
        mConfig.inputCfg.samplingRate = thread->sampleRate();
    mConfig.outputCfg.samplingRate = mConfig.inputCfg.samplingRate;
    mConfig.inputCfg.bufferProvider.cookie = NULL;
    mConfig.inputCfg.bufferProvider.getBuffer = NULL;
    mConfig.inputCfg.bufferProvider.releaseBuffer = NULL;
    mConfig.outputCfg.bufferProvider.cookie = NULL;
    mConfig.outputCfg.bufferProvider.getBuffer = NULL;
    mConfig.outputCfg.bufferProvider.releaseBuffer = NULL;
    mConfig.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    // Insert effect:
    // - in session AUDIO_SESSION_OUTPUT_MIX or AUDIO_SESSION_OUTPUT_STAGE,
    // always overwrites output buffer: input buffer == output buffer
    // - in other sessions:
    //      last effect in the chain accumulates in output buffer: input buffer != output buffer
    //      other effect: overwrites output buffer: input buffer == output buffer
    // Auxiliary effect:
    //      accumulates in output buffer: input buffer != output buffer
    // Therefore: accumulate <=> input buffer != output buffer
    if (mConfig.inputCfg.buffer.raw != mConfig.outputCfg.buffer.raw) {
        mConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_ACCUMULATE;
    } else {
        mConfig.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    }
    mConfig.inputCfg.mask = EFFECT_CONFIG_ALL;
    mConfig.outputCfg.mask = EFFECT_CONFIG_ALL;
#ifdef QCOM_HARDWARE
    if(isForLPA) {
        mConfig.inputCfg.buffer.frameCount = frameCount;
        ALOGV("%s: LPA ON - frameCount %d", __func__, frameCount);
    } else
#endif
        mConfig.inputCfg.buffer.frameCount = thread->frameCount();
    mConfig.outputCfg.buffer.frameCount = mConfig.inputCfg.buffer.frameCount;

//    ALOGV("configure() %p thread %p buffer %p framecount %d",
//            this, thread.get(), mConfig.inputCfg.buffer.raw, mConfig.inputCfg.buffer.frameCount);

    status_t cmdStatus;
    uint32_t size = sizeof(int);
    status_t status = (*mEffectInterface)->command(mEffectInterface,
                                                   EFFECT_CMD_SET_CONFIG,
                                                   sizeof(effect_config_t),
                                                   &mConfig,
                                                   &size,
                                                   &cmdStatus);
    if (status == 0) {
        status = cmdStatus;
    }

    if (status == 0 &&
            (memcmp(&mDescriptor.type, SL_IID_VISUALIZATION, sizeof(effect_uuid_t)) == 0)) {
        uint32_t buf32[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
        effect_param_t *p = (effect_param_t *)buf32;

        p->psize = sizeof(uint32_t);
        p->vsize = sizeof(uint32_t);
        size = sizeof(int);
        *(int32_t *)p->data = VISUALIZER_PARAM_LATENCY;

        uint32_t latency = 0;
        PlaybackThread *pbt = thread->mAudioFlinger->checkPlaybackThread_l(thread->mId);
        if (pbt != NULL) {
            latency = pbt->latency_l();
        }

        *((int32_t *)p->data + 1)= latency;
        (*mEffectInterface)->command(mEffectInterface,
                                     EFFECT_CMD_SET_PARAM,
                                     sizeof(effect_param_t) + 8,
                                     &buf32,
                                     &size,
                                     &cmdStatus);
    }

    mMaxDisableWaitCnt = (MAX_DISABLE_TIME_MS * mConfig.outputCfg.samplingRate) /
            (1000 * mConfig.outputCfg.buffer.frameCount);

    return status;
}

status_t AudioFlinger::EffectModule::init()
{
    Mutex::Autolock _l(mLock);
    if (mEffectInterface == NULL) {
        return NO_INIT;
    }
    status_t cmdStatus;
    uint32_t size = sizeof(status_t);
    status_t status = (*mEffectInterface)->command(mEffectInterface,
                                                   EFFECT_CMD_INIT,
                                                   0,
                                                   NULL,
                                                   &size,
                                                   &cmdStatus);
    if (status == 0) {
        status = cmdStatus;
    }
    return status;
}

status_t AudioFlinger::EffectModule::start()
{
    Mutex::Autolock _l(mLock);
    return start_l();
}

status_t AudioFlinger::EffectModule::start_l()
{
    if (mEffectInterface == NULL) {
        return NO_INIT;
    }
    status_t cmdStatus;
    uint32_t size = sizeof(status_t);
    status_t status = (*mEffectInterface)->command(mEffectInterface,
                                                   EFFECT_CMD_ENABLE,
                                                   0,
                                                   NULL,
                                                   &size,
                                                   &cmdStatus);
    if (status == 0) {
        status = cmdStatus;
    }
    if (status == 0 &&
            ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC ||
             (mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_POST_PROC)) {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            audio_stream_t *stream = thread->stream();
            if (stream != NULL) {
                stream->add_audio_effect(stream, mEffectInterface);
            }
        }
    }
    return status;
}

status_t AudioFlinger::EffectModule::stop()
{
    Mutex::Autolock _l(mLock);
    return stop_l();
}

status_t AudioFlinger::EffectModule::stop_l()
{
    if (mEffectInterface == NULL) {
        return NO_INIT;
    }
    status_t cmdStatus;
    uint32_t size = sizeof(status_t);
    status_t status = (*mEffectInterface)->command(mEffectInterface,
                                                   EFFECT_CMD_DISABLE,
                                                   0,
                                                   NULL,
                                                   &size,
                                                   &cmdStatus);
    if (status == 0) {
        status = cmdStatus;
    }
    if (status == 0 &&
            ((mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_PRE_PROC ||
             (mDescriptor.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_POST_PROC)) {
        sp<ThreadBase> thread = mThread.promote();
        if (thread != 0) {
            audio_stream_t *stream = thread->stream();
            if (stream != NULL) {
                stream->remove_audio_effect(stream, mEffectInterface);
            }
        }
    }
    return status;
}

status_t AudioFlinger::EffectModule::command(uint32_t cmdCode,
                                             uint32_t cmdSize,
                                             void *pCmdData,
                                             uint32_t *replySize,
                                             void *pReplyData)
{
    Mutex::Autolock _l(mLock);
//    ALOGV("command(), cmdCode: %d, mEffectInterface: %p", cmdCode, mEffectInterface);

    if (mState == DESTROYED || mEffectInterface == NULL) {
        return NO_INIT;
    }
    status_t status = (*mEffectInterface)->command(mEffectInterface,
                                                   cmdCode,
                                                   cmdSize,
                                                   pCmdData,
                                                   replySize,
                                                   pReplyData);
    if (cmdCode != EFFECT_CMD_GET_PARAM && status == NO_ERROR) {
        uint32_t size = (replySize == NULL) ? 0 : *replySize;
        for (size_t i = 1; i < mHandles.size(); i++) {
            EffectHandle *h = mHandles[i];
            if (h != NULL && !h->destroyed_l()) {
                h->commandExecuted(cmdCode, cmdSize, pCmdData, size, pReplyData);
            }
        }
    }
    return status;
}

status_t AudioFlinger::EffectModule::setEnabled(bool enabled)
{
    Mutex::Autolock _l(mLock);
    return setEnabled_l(enabled);
}

// must be called with EffectModule::mLock held
status_t AudioFlinger::EffectModule::setEnabled_l(bool enabled)
{
#ifdef QCOM_HARDWARE
    bool effectStateChanged = false;
#endif
    ALOGV("setEnabled %p enabled %d", this, enabled);

    if (enabled != isEnabled()) {
#ifdef QCOM_HARDWARE
        effectStateChanged = true;
#endif
        status_t status = AudioSystem::setEffectEnabled(mId, enabled);
        if (enabled && status != NO_ERROR) {
            return status;
        }

        switch (mState) {
        // going from disabled to enabled
        case IDLE:
            mState = STARTING;
            break;
        case STOPPED:
            mState = RESTART;
            break;
        case STOPPING:
            mState = ACTIVE;
            break;

        // going from enabled to disabled
        case RESTART:
            mState = STOPPED;
            break;
        case STARTING:
            mState = IDLE;
            break;
        case ACTIVE:
            mState = STOPPING;
            break;
        case DESTROYED:
            return NO_ERROR; // simply ignore as we are being destroyed
        }
        for (size_t i = 1; i < mHandles.size(); i++) {
            EffectHandle *h = mHandles[i];
            if (h != NULL && !h->destroyed_l()) {
                h->setEnabled(enabled);
            }
        }
    }
#ifdef QCOM_HARDWARE
    /*
       Send notification event to LPA Player when an effect for
       LPA output is enabled or disabled.
    */
    if (effectStateChanged && mIsForLPA) {
        sp<ThreadBase> thread = mThread.promote();
        thread->effectConfigChanged();
    }
#endif
    return NO_ERROR;
}

bool AudioFlinger::EffectModule::isEnabled() const
{
    switch (mState) {
    case RESTART:
    case STARTING:
    case ACTIVE:
        return true;
    case IDLE:
    case STOPPING:
    case STOPPED:
    case DESTROYED:
    default:
        return false;
    }
}

bool AudioFlinger::EffectModule::isProcessEnabled() const
{
    switch (mState) {
    case RESTART:
    case ACTIVE:
    case STOPPING:
    case STOPPED:
        return true;
    case IDLE:
    case STARTING:
    case DESTROYED:
    default:
        return false;
    }
}

status_t AudioFlinger::EffectModule::setVolume(uint32_t *left, uint32_t *right, bool controller)
{
    Mutex::Autolock _l(mLock);
    status_t status = NO_ERROR;

    // Send volume indication if EFFECT_FLAG_VOLUME_IND is set and read back altered volume
    // if controller flag is set (Note that controller == TRUE => EFFECT_FLAG_VOLUME_CTRL set)
    if (isProcessEnabled() &&
            ((mDescriptor.flags & EFFECT_FLAG_VOLUME_MASK) == EFFECT_FLAG_VOLUME_CTRL ||
            (mDescriptor.flags & EFFECT_FLAG_VOLUME_MASK) == EFFECT_FLAG_VOLUME_IND)) {
        status_t cmdStatus;
        uint32_t volume[2];
        uint32_t *pVolume = NULL;
        uint32_t size = sizeof(volume);
        volume[0] = *left;
        volume[1] = *right;
        if (controller) {
            pVolume = volume;
        }
        status = (*mEffectInterface)->command(mEffectInterface,
                                              EFFECT_CMD_SET_VOLUME,
                                              size,
                                              volume,
                                              &size,
                                              pVolume);
        if (controller && status == NO_ERROR && size == sizeof(volume)) {
            *left = volume[0];
            *right = volume[1];
        }
    }
    return status;
}

status_t AudioFlinger::EffectModule::setDevice(audio_devices_t device)
{
    if (device == AUDIO_DEVICE_NONE) {
        return NO_ERROR;
    }

    Mutex::Autolock _l(mLock);
    status_t status = NO_ERROR;
    if (device && (mDescriptor.flags & EFFECT_FLAG_DEVICE_MASK) == EFFECT_FLAG_DEVICE_IND) {
        status_t cmdStatus;
        uint32_t size = sizeof(status_t);
        uint32_t cmd = audio_is_output_devices(device) ? EFFECT_CMD_SET_DEVICE :
                            EFFECT_CMD_SET_INPUT_DEVICE;
        status = (*mEffectInterface)->command(mEffectInterface,
                                              cmd,
                                              sizeof(uint32_t),
                                              &device,
                                              &size,
                                              &cmdStatus);
    }
    return status;
}

status_t AudioFlinger::EffectModule::setMode(audio_mode_t mode)
{
    Mutex::Autolock _l(mLock);
    status_t status = NO_ERROR;
    if ((mDescriptor.flags & EFFECT_FLAG_AUDIO_MODE_MASK) == EFFECT_FLAG_AUDIO_MODE_IND) {
        status_t cmdStatus;
        uint32_t size = sizeof(status_t);
        status = (*mEffectInterface)->command(mEffectInterface,
                                              EFFECT_CMD_SET_AUDIO_MODE,
                                              sizeof(audio_mode_t),
                                              &mode,
                                              &size,
                                              &cmdStatus);
        if (status == NO_ERROR) {
            status = cmdStatus;
        }
    }
    return status;
}

status_t AudioFlinger::EffectModule::setAudioSource(audio_source_t source)
{
    Mutex::Autolock _l(mLock);
    status_t status = NO_ERROR;
    if ((mDescriptor.flags & EFFECT_FLAG_AUDIO_SOURCE_MASK) == EFFECT_FLAG_AUDIO_SOURCE_IND) {
        uint32_t size = 0;
        status = (*mEffectInterface)->command(mEffectInterface,
                                              EFFECT_CMD_SET_AUDIO_SOURCE,
                                              sizeof(audio_source_t),
                                              &source,
                                              &size,
                                              NULL);
    }
    return status;
}

void AudioFlinger::EffectModule::setSuspended(bool suspended)
{
    Mutex::Autolock _l(mLock);
    mSuspended = suspended;
}

bool AudioFlinger::EffectModule::suspended() const
{
    Mutex::Autolock _l(mLock);
    return mSuspended;
}

bool AudioFlinger::EffectModule::purgeHandles()
{
    bool enabled = false;
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mHandles.size(); i++) {
        EffectHandle *handle = mHandles[i];
        if (handle != NULL && !handle->destroyed_l()) {
            handle->effect().clear();
            if (handle->hasControl()) {
                enabled = handle->enabled();
            }
        }
    }
    return enabled;
}

void AudioFlinger::EffectModule::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "\tEffect ID %d:\n", mId);
    result.append(buffer);

    bool locked = tryLock(mLock);
    // failed to lock - AudioFlinger is probably deadlocked
    if (!locked) {
        result.append("\t\tCould not lock Fx mutex:\n");
    }

    result.append("\t\tSession Status State Engine:\n");
    snprintf(buffer, SIZE, "\t\t%05d   %03d    %03d   0x%08x\n",
            mSessionId, mStatus, mState, (uint32_t)mEffectInterface);
    result.append(buffer);

    result.append("\t\tDescriptor:\n");
    snprintf(buffer, SIZE, "\t\t- UUID: %08X-%04X-%04X-%04X-%02X%02X%02X%02X%02X%02X\n",
            mDescriptor.uuid.timeLow, mDescriptor.uuid.timeMid, mDescriptor.uuid.timeHiAndVersion,
            mDescriptor.uuid.clockSeq, mDescriptor.uuid.node[0], mDescriptor.uuid.node[1],mDescriptor.uuid.node[2],
            mDescriptor.uuid.node[3],mDescriptor.uuid.node[4],mDescriptor.uuid.node[5]);
    result.append(buffer);
    snprintf(buffer, SIZE, "\t\t- TYPE: %08X-%04X-%04X-%04X-%02X%02X%02X%02X%02X%02X\n",
                mDescriptor.type.timeLow, mDescriptor.type.timeMid, mDescriptor.type.timeHiAndVersion,
                mDescriptor.type.clockSeq, mDescriptor.type.node[0], mDescriptor.type.node[1],mDescriptor.type.node[2],
                mDescriptor.type.node[3],mDescriptor.type.node[4],mDescriptor.type.node[5]);
    result.append(buffer);
    snprintf(buffer, SIZE, "\t\t- apiVersion: %08X\n\t\t- flags: %08X\n",
            mDescriptor.apiVersion,
            mDescriptor.flags);
    result.append(buffer);
    snprintf(buffer, SIZE, "\t\t- name: %s\n",
            mDescriptor.name);
    result.append(buffer);
    snprintf(buffer, SIZE, "\t\t- implementor: %s\n",
            mDescriptor.implementor);
    result.append(buffer);

    result.append("\t\t- Input configuration:\n");
    result.append("\t\t\tBuffer     Frames  Smp rate Channels Format\n");
    snprintf(buffer, SIZE, "\t\t\t0x%08x %05d   %05d    %08x %d\n",
            (uint32_t)mConfig.inputCfg.buffer.raw,
            mConfig.inputCfg.buffer.frameCount,
            mConfig.inputCfg.samplingRate,
            mConfig.inputCfg.channels,
            mConfig.inputCfg.format);
    result.append(buffer);

    result.append("\t\t- Output configuration:\n");
    result.append("\t\t\tBuffer     Frames  Smp rate Channels Format\n");
    snprintf(buffer, SIZE, "\t\t\t0x%08x %05d   %05d    %08x %d\n",
            (uint32_t)mConfig.outputCfg.buffer.raw,
            mConfig.outputCfg.buffer.frameCount,
            mConfig.outputCfg.samplingRate,
            mConfig.outputCfg.channels,
            mConfig.outputCfg.format);
    result.append(buffer);

    snprintf(buffer, SIZE, "\t\t%d Clients:\n", mHandles.size());
    result.append(buffer);
    result.append("\t\t\tPid   Priority Ctrl Locked client server\n");
    for (size_t i = 0; i < mHandles.size(); ++i) {
        EffectHandle *handle = mHandles[i];
        if (handle != NULL && !handle->destroyed_l()) {
            handle->dump(buffer, SIZE);
            result.append(buffer);
        }
    }

    result.append("\n");

    write(fd, result.string(), result.length());

    if (locked) {
        mLock.unlock();
    }
}

// ----------------------------------------------------------------------------
//  EffectHandle implementation
// ----------------------------------------------------------------------------

#undef LOG_TAG
#define LOG_TAG "AudioFlinger::EffectHandle"

AudioFlinger::EffectHandle::EffectHandle(const sp<EffectModule>& effect,
                                        const sp<AudioFlinger::Client>& client,
                                        const sp<IEffectClient>& effectClient,
                                        int32_t priority)
    : BnEffect(),
    mEffect(effect), mEffectClient(effectClient), mClient(client), mCblk(NULL),
    mPriority(priority), mHasControl(false), mEnabled(false), mDestroyed(false)
{
    ALOGV("constructor %p", this);

    if (client == 0) {
        return;
    }
    int bufOffset = ((sizeof(effect_param_cblk_t) - 1) / sizeof(int) + 1) * sizeof(int);
    mCblkMemory = client->heap()->allocate(EFFECT_PARAM_BUFFER_SIZE + bufOffset);
    if (mCblkMemory != 0) {
        mCblk = static_cast<effect_param_cblk_t *>(mCblkMemory->pointer());

        if (mCblk != NULL) {
            new(mCblk) effect_param_cblk_t();
            mBuffer = (uint8_t *)mCblk + bufOffset;
        }
    } else {
        ALOGE("not enough memory for Effect size=%u", EFFECT_PARAM_BUFFER_SIZE + sizeof(effect_param_cblk_t));
        return;
    }
}

AudioFlinger::EffectHandle::~EffectHandle()
{
    ALOGV("Destructor %p", this);

    if (mEffect == 0) {
        mDestroyed = true;
        return;
    }
    mEffect->lock();
    mDestroyed = true;
    mEffect->unlock();
    disconnect(false);
}

status_t AudioFlinger::EffectHandle::enable()
{
    ALOGV("enable %p", this);
    if (!mHasControl) return INVALID_OPERATION;
    if (mEffect == 0) return DEAD_OBJECT;

    if (mEnabled) {
        return NO_ERROR;
    }

    mEnabled = true;

    sp<ThreadBase> thread = mEffect->thread().promote();
    if (thread != 0) {
        thread->checkSuspendOnEffectEnabled(mEffect, true, mEffect->sessionId());
    }

    // checkSuspendOnEffectEnabled() can suspend this same effect when enabled
    if (mEffect->suspended()) {
        return NO_ERROR;
    }

    status_t status = mEffect->setEnabled(true);
    if (status != NO_ERROR) {
        if (thread != 0) {
            thread->checkSuspendOnEffectEnabled(mEffect, false, mEffect->sessionId());
        }
        mEnabled = false;
    }
    return status;
}

status_t AudioFlinger::EffectHandle::disable()
{
    ALOGV("disable %p", this);
    if (!mHasControl) return INVALID_OPERATION;
    if (mEffect == 0) return DEAD_OBJECT;

    if (!mEnabled) {
        return NO_ERROR;
    }
    mEnabled = false;

    if (mEffect->suspended()) {
        return NO_ERROR;
    }

    status_t status = mEffect->setEnabled(false);

    sp<ThreadBase> thread = mEffect->thread().promote();
    if (thread != 0) {
        thread->checkSuspendOnEffectEnabled(mEffect, false, mEffect->sessionId());
    }

    return status;
}

void AudioFlinger::EffectHandle::disconnect()
{
    disconnect(true);
}

void AudioFlinger::EffectHandle::disconnect(bool unpinIfLast)
{
    ALOGV("disconnect(%s)", unpinIfLast ? "true" : "false");
    if (mEffect == 0) {
        return;
    }
    // restore suspended effects if the disconnected handle was enabled and the last one.
    if ((mEffect->disconnect(this, unpinIfLast) == 0) && mEnabled) {
        sp<ThreadBase> thread = mEffect->thread().promote();
        if (thread != 0) {
            thread->checkSuspendOnEffectEnabled(mEffect, false, mEffect->sessionId());
        }
    }

    // release sp on module => module destructor can be called now
    mEffect.clear();
    if (mClient != 0) {
        if (mCblk != NULL) {
            // unlike ~TrackBase(), mCblk is never a local new, so don't delete
            mCblk->~effect_param_cblk_t();   // destroy our shared-structure.
        }
        mCblkMemory.clear();    // free the shared memory before releasing the heap it belongs to
        // Client destructor must run with AudioFlinger mutex locked
        Mutex::Autolock _l(mClient->audioFlinger()->mLock);
        mClient.clear();
    }
}

status_t AudioFlinger::EffectHandle::command(uint32_t cmdCode,
                                             uint32_t cmdSize,
                                             void *pCmdData,
                                             uint32_t *replySize,
                                             void *pReplyData)
{
//    ALOGV("command(), cmdCode: %d, mHasControl: %d, mEffect: %p",
//              cmdCode, mHasControl, (mEffect == 0) ? 0 : mEffect.get());

    // only get parameter command is permitted for applications not controlling the effect
    if (!mHasControl && cmdCode != EFFECT_CMD_GET_PARAM) {
        return INVALID_OPERATION;
    }
    if (mEffect == 0) return DEAD_OBJECT;
    if (mClient == 0) return INVALID_OPERATION;

    // handle commands that are not forwarded transparently to effect engine
    if (cmdCode == EFFECT_CMD_SET_PARAM_COMMIT) {
        // No need to trylock() here as this function is executed in the binder thread serving a particular client process:
        // no risk to block the whole media server process or mixer threads is we are stuck here
        Mutex::Autolock _l(mCblk->lock);
        if (mCblk->clientIndex > EFFECT_PARAM_BUFFER_SIZE ||
            mCblk->serverIndex > EFFECT_PARAM_BUFFER_SIZE) {
            mCblk->serverIndex = 0;
            mCblk->clientIndex = 0;
            return BAD_VALUE;
        }
        status_t status = NO_ERROR;
        while (mCblk->serverIndex < mCblk->clientIndex) {
            int reply;
            uint32_t rsize = sizeof(int);
            int *p = (int *)(mBuffer + mCblk->serverIndex);
            int size = *p++;
            if (((uint8_t *)p + size) > mBuffer + mCblk->clientIndex) {
                ALOGW("command(): invalid parameter block size");
                break;
            }
            effect_param_t *param = (effect_param_t *)p;
            if (param->psize == 0 || param->vsize == 0) {
                ALOGW("command(): null parameter or value size");
                mCblk->serverIndex += size;
                continue;
            }
            uint32_t psize = sizeof(effect_param_t) +
                             ((param->psize - 1) / sizeof(int) + 1) * sizeof(int) +
                             param->vsize;
            status_t ret = mEffect->command(EFFECT_CMD_SET_PARAM,
                                            psize,
                                            p,
                                            &rsize,
                                            &reply);
            // stop at first error encountered
            if (ret != NO_ERROR) {
                status = ret;
                *(int *)pReplyData = reply;
                break;
            } else if (reply != NO_ERROR) {
                *(int *)pReplyData = reply;
                break;
            }
            mCblk->serverIndex += size;
        }
        mCblk->serverIndex = 0;
        mCblk->clientIndex = 0;
        return status;
    } else if (cmdCode == EFFECT_CMD_ENABLE) {
        *(int *)pReplyData = NO_ERROR;
        return enable();
    } else if (cmdCode == EFFECT_CMD_DISABLE) {
        *(int *)pReplyData = NO_ERROR;
        return disable();
    }

#ifdef QCOM_HARDWARE
    ALOGV("EffectHandle::command: isOnLPA %d", mEffect->isOnLPA());
    if(mEffect->isOnLPA() &&
       ((cmdCode == EFFECT_CMD_SET_PARAM) || (cmdCode == EFFECT_CMD_SET_PARAM_DEFERRED) ||
        (cmdCode == EFFECT_CMD_SET_PARAM_COMMIT) || (cmdCode == EFFECT_CMD_SET_DEVICE) ||
        (cmdCode == EFFECT_CMD_SET_VOLUME) || (cmdCode == EFFECT_CMD_SET_AUDIO_MODE)) ) {
        // Notify Direct track for the change in Effect module
        // TODO: check if it is required to send mLPAHandle
        ALOGV("Notifying Direct Track for the change in effect config %d", cmdCode);
        mClient->audioFlinger()->audioConfigChanged_l(AudioSystem::EFFECT_CONFIG_CHANGED, 0, NULL);
    }
#endif
    return mEffect->command(cmdCode, cmdSize, pCmdData, replySize, pReplyData);
}

void AudioFlinger::EffectHandle::setControl(bool hasControl, bool signal, bool enabled)
{
    ALOGV("setControl %p control %d", this, hasControl);

    mHasControl = hasControl;
    mEnabled = enabled;

    if (signal && mEffectClient != 0) {
        mEffectClient->controlStatusChanged(hasControl);
    }
}

void AudioFlinger::EffectHandle::commandExecuted(uint32_t cmdCode,
                                                 uint32_t cmdSize,
                                                 void *pCmdData,
                                                 uint32_t replySize,
                                                 void *pReplyData)
{
    if (mEffectClient != 0) {
        mEffectClient->commandExecuted(cmdCode, cmdSize, pCmdData, replySize, pReplyData);
    }
}



void AudioFlinger::EffectHandle::setEnabled(bool enabled)
{
    if (mEffectClient != 0) {
        mEffectClient->enableStatusChanged(enabled);
    }
}

status_t AudioFlinger::EffectHandle::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnEffect::onTransact(code, data, reply, flags);
}


void AudioFlinger::EffectHandle::dump(char* buffer, size_t size)
{
    bool locked = mCblk != NULL && tryLock(mCblk->lock);

    snprintf(buffer, size, "\t\t\t%05d %05d    %01u    %01u      %05u  %05u\n",
            (mClient == 0) ? getpid_cached : mClient->pid(),
            mPriority,
            mHasControl,
            !locked,
            mCblk ? mCblk->clientIndex : 0,
            mCblk ? mCblk->serverIndex : 0
            );

    if (locked) {
        mCblk->lock.unlock();
    }
}

#undef LOG_TAG
#define LOG_TAG "AudioFlinger::EffectChain"

AudioFlinger::EffectChain::EffectChain(ThreadBase *thread,
                                        int sessionId)
    : mThread(thread), mSessionId(sessionId), mActiveTrackCnt(0), mTrackCnt(0), mTailBufferCount(0),
      mOwnInBuffer(false), mVolumeCtrlIdx(-1), mLeftVolume(UINT_MAX), mRightVolume(UINT_MAX),
      mNewLeftVolume(UINT_MAX), mNewRightVolume(UINT_MAX)
#ifdef QCOM_HARDWARE
      ,mIsForLPATrack(false)
#endif
{
    mStrategy = AudioSystem::getStrategyForStream(AUDIO_STREAM_MUSIC);
    if (thread == NULL) {
        return;
    }
    mMaxTailBuffers = ((kProcessTailDurationMs * thread->sampleRate()) / 1000) /
                                    thread->frameCount();
}

AudioFlinger::EffectChain::~EffectChain()
{
    if (mOwnInBuffer) {
        delete mInBuffer;
    }

}

// getEffectFromDesc_l() must be called with ThreadBase::mLock held
sp<AudioFlinger::EffectModule> AudioFlinger::EffectChain::getEffectFromDesc_l(effect_descriptor_t *descriptor)
{
    size_t size = mEffects.size();

    for (size_t i = 0; i < size; i++) {
        if (memcmp(&mEffects[i]->desc().uuid, &descriptor->uuid, sizeof(effect_uuid_t)) == 0) {
            return mEffects[i];
        }
    }
    return 0;
}

// getEffectFromId_l() must be called with ThreadBase::mLock held
sp<AudioFlinger::EffectModule> AudioFlinger::EffectChain::getEffectFromId_l(int id)
{
    size_t size = mEffects.size();

    for (size_t i = 0; i < size; i++) {
        // by convention, return first effect if id provided is 0 (0 is never a valid id)
        if (id == 0 || mEffects[i]->id() == id) {
            return mEffects[i];
        }
    }
    return 0;
}

#ifdef QCOM_HARDWARE
sp<AudioFlinger::EffectModule> AudioFlinger::EffectChain::getEffectFromIndex_l(int idx)
{
    sp<EffectModule> effect = NULL;
    if(idx < 0 || idx >= mEffects.size()) {
        ALOGE("EffectChain::getEffectFromIndex_l: invalid index %d", idx);
    }
    if(mEffects.size() > 0){
        effect = mEffects[idx];
    }
    return effect;
}
#endif

// getEffectFromType_l() must be called with ThreadBase::mLock held
sp<AudioFlinger::EffectModule> AudioFlinger::EffectChain::getEffectFromType_l(
        const effect_uuid_t *type)
{
    size_t size = mEffects.size();

    for (size_t i = 0; i < size; i++) {
        if (memcmp(&mEffects[i]->desc().type, type, sizeof(effect_uuid_t)) == 0) {
            return mEffects[i];
        }
    }
    return 0;
}

void AudioFlinger::EffectChain::clearInputBuffer()
{
    Mutex::Autolock _l(mLock);
    sp<ThreadBase> thread = mThread.promote();
    if (thread == 0) {
        ALOGW("clearInputBuffer(): cannot promote mixer thread");
        return;
    }
    clearInputBuffer_l(thread);
}

// Must be called with EffectChain::mLock locked
void AudioFlinger::EffectChain::clearInputBuffer_l(sp<ThreadBase> thread)
{
    size_t numSamples = thread->frameCount() * thread->channelCount();
    memset(mInBuffer, 0, numSamples * sizeof(int16_t));

}

// Must be called with EffectChain::mLock locked
void AudioFlinger::EffectChain::process_l()
{
    sp<ThreadBase> thread = mThread.promote();
    if (thread == 0) {
        ALOGW("process_l(): cannot promote mixer thread");
        return;
    }
    bool isGlobalSession = (mSessionId == AUDIO_SESSION_OUTPUT_MIX) ||
            (mSessionId == AUDIO_SESSION_OUTPUT_STAGE);
    // always process effects unless no more tracks are on the session and the effect tail
    // has been rendered
    bool doProcess = true;
    if (!isGlobalSession) {
        bool tracksOnSession = (trackCnt() != 0);

        if (!tracksOnSession && mTailBufferCount == 0) {
            doProcess = false;
        }

        if (activeTrackCnt() == 0) {
            // if no track is active and the effect tail has not been rendered,
            // the input buffer must be cleared here as the mixer process will not do it
            if (tracksOnSession || mTailBufferCount > 0) {
                clearInputBuffer_l(thread);
                if (mTailBufferCount > 0) {
                    mTailBufferCount--;
                }
            }
        }
    }

    size_t size = mEffects.size();
#ifdef QCOM_HARDWARE
    if (doProcess || isForLPATrack()) {
#else
    if (doProcess) {
#endif
        for (size_t i = 0; i < size; i++) {
            mEffects[i]->process();
        }
    }
    for (size_t i = 0; i < size; i++) {
        mEffects[i]->updateState();
    }
}

// addEffect_l() must be called with PlaybackThread::mLock held
status_t AudioFlinger::EffectChain::addEffect_l(const sp<EffectModule>& effect)
{
    effect_descriptor_t desc = effect->desc();
    uint32_t insertPref = desc.flags & EFFECT_FLAG_INSERT_MASK;

    Mutex::Autolock _l(mLock);
    effect->setChain(this);
    sp<ThreadBase> thread = mThread.promote();
    if (thread == 0) {
        return NO_INIT;
    }
    effect->setThread(thread);

    if ((desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) {
        // Auxiliary effects are inserted at the beginning of mEffects vector as
        // they are processed first and accumulated in chain input buffer
        mEffects.insertAt(effect, 0);

        // the input buffer for auxiliary effect contains mono samples in
        // 32 bit format. This is to avoid saturation in AudoMixer
        // accumulation stage. Saturation is done in EffectModule::process() before
        // calling the process in effect engine
        size_t numSamples = thread->frameCount();
        int32_t *buffer = new int32_t[numSamples];
        memset(buffer, 0, numSamples * sizeof(int32_t));
        effect->setInBuffer((int16_t *)buffer);
        // auxiliary effects output samples to chain input buffer for further processing
        // by insert effects
        effect->setOutBuffer(mInBuffer);
    } else {
        // Insert effects are inserted at the end of mEffects vector as they are processed
        //  after track and auxiliary effects.
        // Insert effect order as a function of indicated preference:
        //  if EFFECT_FLAG_INSERT_EXCLUSIVE, insert in first position or reject if
        //  another effect is present
        //  else if EFFECT_FLAG_INSERT_FIRST, insert in first position or after the
        //  last effect claiming first position
        //  else if EFFECT_FLAG_INSERT_LAST, insert in last position or before the
        //  first effect claiming last position
        //  else if EFFECT_FLAG_INSERT_ANY insert after first or before last
        // Reject insertion if an effect with EFFECT_FLAG_INSERT_EXCLUSIVE is
        // already present

        size_t size = mEffects.size();
        size_t idx_insert = size;
        ssize_t idx_insert_first = -1;
        ssize_t idx_insert_last = -1;

        for (size_t i = 0; i < size; i++) {
            effect_descriptor_t d = mEffects[i]->desc();
            uint32_t iMode = d.flags & EFFECT_FLAG_TYPE_MASK;
            uint32_t iPref = d.flags & EFFECT_FLAG_INSERT_MASK;
            if (iMode == EFFECT_FLAG_TYPE_INSERT) {
                // check invalid effect chaining combinations
                if (insertPref == EFFECT_FLAG_INSERT_EXCLUSIVE ||
                    iPref == EFFECT_FLAG_INSERT_EXCLUSIVE) {
                    ALOGW("addEffect_l() could not insert effect %s: exclusive conflict with %s", desc.name, d.name);
                    return INVALID_OPERATION;
                }
                // remember position of first insert effect and by default
                // select this as insert position for new effect
                if (idx_insert == size) {
                    idx_insert = i;
                }
                // remember position of last insert effect claiming
                // first position
                if (iPref == EFFECT_FLAG_INSERT_FIRST) {
                    idx_insert_first = i;
                }
                // remember position of first insert effect claiming
                // last position
                if (iPref == EFFECT_FLAG_INSERT_LAST &&
                    idx_insert_last == -1) {
                    idx_insert_last = i;
                }
            }
        }

        // modify idx_insert from first position if needed
        if (insertPref == EFFECT_FLAG_INSERT_LAST) {
            if (idx_insert_last != -1) {
                idx_insert = idx_insert_last;
            } else {
                idx_insert = size;
            }
        } else {
            if (idx_insert_first != -1) {
                idx_insert = idx_insert_first + 1;
            }
        }

        // always read samples from chain input buffer
        effect->setInBuffer(mInBuffer);

        // if last effect in the chain, output samples to chain
        // output buffer, otherwise to chain input buffer
        if (idx_insert == size) {
            if (idx_insert != 0) {
                mEffects[idx_insert-1]->setOutBuffer(mInBuffer);
                mEffects[idx_insert-1]->configure();
            }
            effect->setOutBuffer(mOutBuffer);
        } else {
            effect->setOutBuffer(mInBuffer);
        }
        mEffects.insertAt(effect, idx_insert);

        ALOGV("addEffect_l() effect %p, added in chain %p at rank %d", effect.get(), this, idx_insert);
    }
    effect->configure();
    return NO_ERROR;
}

// removeEffect_l() must be called with PlaybackThread::mLock held
size_t AudioFlinger::EffectChain::removeEffect_l(const sp<EffectModule>& effect)
{
    Mutex::Autolock _l(mLock);
    size_t size = mEffects.size();
    uint32_t type = effect->desc().flags & EFFECT_FLAG_TYPE_MASK;

    for (size_t i = 0; i < size; i++) {
        if (effect == mEffects[i]) {
            // calling stop here will remove pre-processing effect from the audio HAL.
            // This is safe as we hold the EffectChain mutex which guarantees that we are not in
            // the middle of a read from audio HAL
            if (mEffects[i]->state() == EffectModule::ACTIVE ||
                    mEffects[i]->state() == EffectModule::STOPPING) {
                mEffects[i]->stop();
            }
            if (type == EFFECT_FLAG_TYPE_AUXILIARY) {
                delete[] effect->inBuffer();
            } else {
                if (i == size - 1 && i != 0) {
                    mEffects[i - 1]->setOutBuffer(mOutBuffer);
                    mEffects[i - 1]->configure();
                }
            }
            mEffects.removeAt(i);
            ALOGV("removeEffect_l() effect %p, removed from chain %p at rank %d", effect.get(), this, i);
            break;
        }
    }

    return mEffects.size();
}

// setDevice_l() must be called with PlaybackThread::mLock held
void AudioFlinger::EffectChain::setDevice_l(audio_devices_t device)
{
    size_t size = mEffects.size();
    for (size_t i = 0; i < size; i++) {
        mEffects[i]->setDevice(device);
    }
}

// setMode_l() must be called with PlaybackThread::mLock held
void AudioFlinger::EffectChain::setMode_l(audio_mode_t mode)
{
    size_t size = mEffects.size();
    for (size_t i = 0; i < size; i++) {
        mEffects[i]->setMode(mode);
    }
}

// setAudioSource_l() must be called with PlaybackThread::mLock held
void AudioFlinger::EffectChain::setAudioSource_l(audio_source_t source)
{
    size_t size = mEffects.size();
    for (size_t i = 0; i < size; i++) {
        mEffects[i]->setAudioSource(source);
    }
}

// setVolume_l() must be called with PlaybackThread::mLock held
bool AudioFlinger::EffectChain::setVolume_l(uint32_t *left, uint32_t *right)
{
    uint32_t newLeft = *left;
    uint32_t newRight = *right;
    bool hasControl = false;
    int ctrlIdx = -1;
    size_t size = mEffects.size();

    // first update volume controller
    for (size_t i = size; i > 0; i--) {
        if (mEffects[i - 1]->isProcessEnabled() &&
            (mEffects[i - 1]->desc().flags & EFFECT_FLAG_VOLUME_MASK) == EFFECT_FLAG_VOLUME_CTRL) {
            ctrlIdx = i - 1;
            hasControl = true;
            break;
        }
    }

    if (ctrlIdx == mVolumeCtrlIdx && *left == mLeftVolume && *right == mRightVolume) {
        if (hasControl) {
            *left = mNewLeftVolume;
            *right = mNewRightVolume;
        }
        return hasControl;
    }

    mVolumeCtrlIdx = ctrlIdx;
    mLeftVolume = newLeft;
    mRightVolume = newRight;

    // second get volume update from volume controller
    if (ctrlIdx >= 0) {
        mEffects[ctrlIdx]->setVolume(&newLeft, &newRight, true);
        mNewLeftVolume = newLeft;
        mNewRightVolume = newRight;
    }
    // then indicate volume to all other effects in chain.
    // Pass altered volume to effects before volume controller
    // and requested volume to effects after controller
    uint32_t lVol = newLeft;
    uint32_t rVol = newRight;

    for (size_t i = 0; i < size; i++) {
        if ((int)i == ctrlIdx) continue;
        // this also works for ctrlIdx == -1 when there is no volume controller
        if ((int)i > ctrlIdx) {
            lVol = *left;
            rVol = *right;
        }
        mEffects[i]->setVolume(&lVol, &rVol, false);
    }
    *left = newLeft;
    *right = newRight;

    return hasControl;
}

void AudioFlinger::EffectChain::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "Effects for session %d:\n", mSessionId);
    result.append(buffer);

    bool locked = tryLock(mLock);
    // failed to lock - AudioFlinger is probably deadlocked
    if (!locked) {
        result.append("\tCould not lock mutex:\n");
    }

    result.append("\tNum fx In buffer   Out buffer   Active tracks:\n");
    snprintf(buffer, SIZE, "\t%02d     0x%08x  0x%08x   %d\n",
            mEffects.size(),
            (uint32_t)mInBuffer,
            (uint32_t)mOutBuffer,
            mActiveTrackCnt);
    result.append(buffer);
    write(fd, result.string(), result.size());

    for (size_t i = 0; i < mEffects.size(); ++i) {
        sp<EffectModule> effect = mEffects[i];
        if (effect != 0) {
            effect->dump(fd, args);
        }
    }

    if (locked) {
        mLock.unlock();
    }
}

// must be called with ThreadBase::mLock held
void AudioFlinger::EffectChain::setEffectSuspended_l(
        const effect_uuid_t *type, bool suspend)
{
    sp<SuspendedEffectDesc> desc;
    // use effect type UUID timelow as key as there is no real risk of identical
    // timeLow fields among effect type UUIDs.
    ssize_t index = mSuspendedEffects.indexOfKey(type->timeLow);
    if (suspend) {
        if (index >= 0) {
            desc = mSuspendedEffects.valueAt(index);
        } else {
            desc = new SuspendedEffectDesc();
            desc->mType = *type;
            mSuspendedEffects.add(type->timeLow, desc);
            ALOGV("setEffectSuspended_l() add entry for %08x", type->timeLow);
        }
        if (desc->mRefCount++ == 0) {
            sp<EffectModule> effect = getEffectIfEnabled(type);
            if (effect != 0) {
                desc->mEffect = effect;
                effect->setSuspended(true);
                effect->setEnabled(false);
            }
        }
    } else {
        if (index < 0) {
            return;
        }
        desc = mSuspendedEffects.valueAt(index);
        if (desc->mRefCount <= 0) {
            ALOGW("setEffectSuspended_l() restore refcount should not be 0 %d", desc->mRefCount);
            desc->mRefCount = 1;
        }
        if (--desc->mRefCount == 0) {
            ALOGV("setEffectSuspended_l() remove entry for %08x", mSuspendedEffects.keyAt(index));
            if (desc->mEffect != 0) {
                sp<EffectModule> effect = desc->mEffect.promote();
                if (effect != 0) {
                    effect->setSuspended(false);
                    effect->lock();
                    EffectHandle *handle = effect->controlHandle_l();
                    if (handle != NULL && !handle->destroyed_l()) {
                        effect->setEnabled_l(handle->enabled());
                    }
                    effect->unlock();
                }
                desc->mEffect.clear();
            }
            mSuspendedEffects.removeItemsAt(index);
        }
    }
}

// must be called with ThreadBase::mLock held
void AudioFlinger::EffectChain::setEffectSuspendedAll_l(bool suspend)
{
    sp<SuspendedEffectDesc> desc;

    ssize_t index = mSuspendedEffects.indexOfKey((int)kKeyForSuspendAll);
    if (suspend) {
        if (index >= 0) {
            desc = mSuspendedEffects.valueAt(index);
        } else {
            desc = new SuspendedEffectDesc();
            mSuspendedEffects.add((int)kKeyForSuspendAll, desc);
            ALOGV("setEffectSuspendedAll_l() add entry for 0");
        }
        if (desc->mRefCount++ == 0) {
            Vector< sp<EffectModule> > effects;
            getSuspendEligibleEffects(effects);
            for (size_t i = 0; i < effects.size(); i++) {
                setEffectSuspended_l(&effects[i]->desc().type, true);
            }
        }
    } else {
        if (index < 0) {
            return;
        }
        desc = mSuspendedEffects.valueAt(index);
        if (desc->mRefCount <= 0) {
            ALOGW("setEffectSuspendedAll_l() restore refcount should not be 0 %d", desc->mRefCount);
            desc->mRefCount = 1;
        }
        if (--desc->mRefCount == 0) {
            Vector<const effect_uuid_t *> types;
            for (size_t i = 0; i < mSuspendedEffects.size(); i++) {
                if (mSuspendedEffects.keyAt(i) == (int)kKeyForSuspendAll) {
                    continue;
                }
                types.add(&mSuspendedEffects.valueAt(i)->mType);
            }
            for (size_t i = 0; i < types.size(); i++) {
                setEffectSuspended_l(types[i], false);
            }
            ALOGV("setEffectSuspendedAll_l() remove entry for %08x", mSuspendedEffects.keyAt(index));
            mSuspendedEffects.removeItem((int)kKeyForSuspendAll);
        }
    }
}


// The volume effect is used for automated tests only
#ifndef OPENSL_ES_H_
static const effect_uuid_t SL_IID_VOLUME_ = { 0x09e8ede0, 0xddde, 0x11db, 0xb4f6,
                                            { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const effect_uuid_t * const SL_IID_VOLUME = &SL_IID_VOLUME_;
#endif //OPENSL_ES_H_

bool AudioFlinger::EffectChain::isEffectEligibleForSuspend(const effect_descriptor_t& desc)
{
    // auxiliary effects and visualizer are never suspended on output mix
    if ((mSessionId == AUDIO_SESSION_OUTPUT_MIX) &&
        (((desc.flags & EFFECT_FLAG_TYPE_MASK) == EFFECT_FLAG_TYPE_AUXILIARY) ||
         (memcmp(&desc.type, SL_IID_VISUALIZATION, sizeof(effect_uuid_t)) == 0) ||
         (memcmp(&desc.type, SL_IID_VOLUME, sizeof(effect_uuid_t)) == 0))) {
        return false;
    }
    return true;
}

void AudioFlinger::EffectChain::getSuspendEligibleEffects(Vector< sp<AudioFlinger::EffectModule> > &effects)
{
    effects.clear();
    for (size_t i = 0; i < mEffects.size(); i++) {
        if (isEffectEligibleForSuspend(mEffects[i]->desc())) {
            effects.add(mEffects[i]);
        }
    }
}

sp<AudioFlinger::EffectModule> AudioFlinger::EffectChain::getEffectIfEnabled(
                                                            const effect_uuid_t *type)
{
    sp<EffectModule> effect = getEffectFromType_l(type);
    return effect != 0 && effect->isEnabled() ? effect : 0;
}

void AudioFlinger::EffectChain::checkSuspendOnEffectEnabled(const sp<EffectModule>& effect,
                                                            bool enabled)
{
    ssize_t index = mSuspendedEffects.indexOfKey(effect->desc().type.timeLow);
    if (enabled) {
        if (index < 0) {
            // if the effect is not suspend check if all effects are suspended
            index = mSuspendedEffects.indexOfKey((int)kKeyForSuspendAll);
            if (index < 0) {
                return;
            }
            if (!isEffectEligibleForSuspend(effect->desc())) {
                return;
            }
            setEffectSuspended_l(&effect->desc().type, enabled);
            index = mSuspendedEffects.indexOfKey(effect->desc().type.timeLow);
            if (index < 0) {
                ALOGW("checkSuspendOnEffectEnabled() Fx should be suspended here!");
                return;
            }
        }
        ALOGV("checkSuspendOnEffectEnabled() enable suspending fx %08x",
            effect->desc().type.timeLow);
        sp<SuspendedEffectDesc> desc = mSuspendedEffects.valueAt(index);
        // if effect is requested to suspended but was not yet enabled, supend it now.
        if (desc->mEffect == 0) {
            desc->mEffect = effect;
            effect->setEnabled(false);
            effect->setSuspended(true);
        }
    } else {
        if (index < 0) {
            return;
        }
        ALOGV("checkSuspendOnEffectEnabled() disable restoring fx %08x",
            effect->desc().type.timeLow);
        sp<SuspendedEffectDesc> desc = mSuspendedEffects.valueAt(index);
        desc->mEffect.clear();
        effect->setSuspended(false);
    }
}

#undef LOG_TAG
#define LOG_TAG "AudioFlinger"

// ----------------------------------------------------------------------------

status_t AudioFlinger::onTransact(
        uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioFlinger::onTransact(code, data, reply, flags);
}

}; // namespace android
