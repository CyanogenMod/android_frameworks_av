/*
** Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
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
**
** This file was modified by Dolby Laboratories, Inc. The portions of the
** code that are surrounded by "DOLBY..." are copyrighted and
** licensed separately, as follows:
**
**  (C) 2011-2014 Dolby Laboratories, Inc.
** This file was modified by DTS, Inc. The portions of the
** code that are surrounded by "DTS..." are copyrighted and
** licensed separately, as follows:
**
**  (C) 2013 DTS, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**    http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License
*/


#define LOG_TAG "AudioFlinger"
//#define LOG_NDEBUG 0

#include "Configuration.h"
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include <binder/Parcel.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include <utils/Atomic.h>

#include <cutils/bitops.h>
#include <cutils/properties.h>

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

#include <common_time/cc_helper.h>

#include <media/IMediaLogService.h>

#include <media/nbaio/Pipe.h>
#include <media/nbaio/PipeReader.h>
#include <media/AudioParameter.h>
#include <private/android_filesystem_config.h>
#ifdef SRS_PROCESSING
#include "postpro_patch.h"
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

#ifdef DOLBY_DAP
#include "EffectDapController_impl.h"
#endif // DOLBY_END
namespace android {

static const char kDeadlockedString[] = "AudioFlinger may be deadlocked\n";
static const char kHardwareLockedString[] = "Hardware lock is taken\n";
static const char kClientLockedString[] = "Client lock is taken\n";


nsecs_t AudioFlinger::mStandbyTimeInNsecs = kDefaultStandbyTimeInNsecs;

uint32_t AudioFlinger::mScreenState;
#ifdef HW_ACC_HPX
bool AudioFlinger::mIsHPXOn = false;
#endif

#ifdef TEE_SINK
bool AudioFlinger::mTeeSinkInputEnabled = false;
bool AudioFlinger::mTeeSinkOutputEnabled = false;
bool AudioFlinger::mTeeSinkTrackEnabled = false;

size_t AudioFlinger::mTeeSinkInputFrames = kTeeSinkInputFramesDefault;
size_t AudioFlinger::mTeeSinkOutputFrames = kTeeSinkOutputFramesDefault;
size_t AudioFlinger::mTeeSinkTrackFrames = kTeeSinkTrackFramesDefault;
#endif

// In order to avoid invalidating offloaded tracks each time a Visualizer is turned on and off
// we define a minimum time during which a global effect is considered enabled.
static const nsecs_t kMinGlobalEffectEnabletimeNs = seconds(7200);

// ----------------------------------------------------------------------------

const char *formatToString(audio_format_t format) {
    switch (format & AUDIO_FORMAT_MAIN_MASK) {
    case AUDIO_FORMAT_PCM:
        switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT: return "pcm16";
        case AUDIO_FORMAT_PCM_8_BIT: return "pcm8";
        case AUDIO_FORMAT_PCM_32_BIT: return "pcm32";
        case AUDIO_FORMAT_PCM_8_24_BIT: return "pcm8.24";
        case AUDIO_FORMAT_PCM_FLOAT: return "pcmfloat";
        case AUDIO_FORMAT_PCM_24_BIT_PACKED: return "pcm24";
        default:
            break;
        }
        break;
    case AUDIO_FORMAT_MP3: return "mp3";
    case AUDIO_FORMAT_AMR_NB: return "amr-nb";
    case AUDIO_FORMAT_AMR_WB: return "amr-wb";
    case AUDIO_FORMAT_AAC: return "aac";
    case AUDIO_FORMAT_HE_AAC_V1: return "he-aac-v1";
    case AUDIO_FORMAT_HE_AAC_V2: return "he-aac-v2";
    case AUDIO_FORMAT_VORBIS: return "vorbis";
    case AUDIO_FORMAT_OPUS: return "opus";
    case AUDIO_FORMAT_AC3: return "ac-3";
    case AUDIO_FORMAT_E_AC3: return "e-ac-3";
    case AUDIO_FORMAT_PCM_OFFLOAD:
        switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT_OFFLOAD: return "pcm-16bit-offload";
        case AUDIO_FORMAT_PCM_24_BIT_OFFLOAD: return "pcm-24bit-offload";
        default:
            break;
        }
        break;
    default:
        break;
    }
    return "unknown";
}

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
    if ((*dev)->common.version < AUDIO_DEVICE_API_VERSION_MIN) {
        ALOGE("%s wrong audio hw device version %04x", __func__, (*dev)->common.version);
        rc = BAD_VALUE;
        goto out;
    }
    return 0;

out:
    *dev = NULL;
    return rc;
}

// ----------------------------------------------------------------------------

AudioFlinger::AudioFlinger()
    : BnAudioFlinger(),
      mPrimaryHardwareDev(NULL),
      mAudioHwDevs(NULL),
      mHardwareStatus(AUDIO_HW_IDLE),
      mMasterVolume(1.0f),
      mMasterMute(false),
      mNextUniqueId(1),
      mMode(AUDIO_MODE_INVALID),
      mBtNrecIsOff(false),
      mIsLowRamDevice(true),
      mIsDeviceTypeKnown(false),
      mGlobalEffectEnableTime(0),
#ifdef QCOM_DIRECTTRACK
      mPrimaryOutputSampleRate(0),
      mAllChainsLocked(false)
#else
      mPrimaryOutputSampleRate(0)
#endif
{
    getpid_cached = getpid();
    char value[PROPERTY_VALUE_MAX];
    bool doLog = (property_get("ro.test_harness", value, "0") > 0) && (atoi(value) == 1);
    if (doLog) {
        mLogMemoryDealer = new MemoryDealer(kLogMemorySize, "LogWriters", MemoryHeapBase::READ_ONLY);
    }

#ifdef TEE_SINK
    (void) property_get("ro.debuggable", value, "0");
    int debuggable = atoi(value);
    int teeEnabled = 0;
    if (debuggable) {
        (void) property_get("af.tee", value, "0");
        teeEnabled = atoi(value);
    }
    // FIXME symbolic constants here
    if (teeEnabled & 1) {
        mTeeSinkInputEnabled = true;
    }
    if (teeEnabled & 2) {
        mTeeSinkOutputEnabled = true;
    }
    if (teeEnabled & 4) {
        mTeeSinkTrackEnabled = true;
    }
#endif
#ifdef DOLBY_DAP
    EffectDapController::mInstance = new EffectDapController(this);
#endif // DOLBY_END
}

void AudioFlinger::onFirstRef()
{
    int rc = 0;

    Mutex::Autolock _l(mLock);

    /* TODO: move all this work into an Init() function */
#ifdef QCOM_DIRECTTRACK
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

    mPatchPanel = new PatchPanel(this);

    mMode = AUDIO_MODE_NORMAL;
}

AudioFlinger::~AudioFlinger()
{
#ifdef DOLBY_DAP
    delete EffectDapController::mInstance;
#endif // DOLBY_END
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

    // Tell media.log service about any old writers that still need to be unregistered
    sp<IBinder> binder = defaultServiceManager()->getService(String16("media.log"));
    if (binder != 0) {
        sp<IMediaLogService> mediaLogService(interface_cast<IMediaLogService>(binder));
        for (size_t count = mUnregisteredWriters.size(); count > 0; count--) {
            sp<IMemory> iMemory(mUnregisteredWriters.top()->getIMemory());
            mUnregisteredWriters.pop();
            mediaLogService->unregisterWriter(iMemory);
        }
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

void AudioFlinger::dumpClients(int fd, const Vector<String16>& args __unused)
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

    result.append("Notification Clients:\n");
    for (size_t i = 0; i < mNotificationClients.size(); ++i) {
        snprintf(buffer, SIZE, "  pid: %d\n", mNotificationClients.keyAt(i));
        result.append(buffer);
    }

    result.append("Global session refs:\n");
    result.append("  session   pid count\n");
    for (size_t i = 0; i < mAudioSessionRefs.size(); i++) {
        AudioSessionRef *r = mAudioSessionRefs[i];
        snprintf(buffer, SIZE, "  %7d %5d %5d\n", r->mSessionid, r->mPid, r->mCnt);
        result.append(buffer);
    }
    write(fd, result.string(), result.size());
}


void AudioFlinger::dumpInternals(int fd, const Vector<String16>& args __unused)
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

void AudioFlinger::dumpPermissionDenial(int fd, const Vector<String16>& args __unused)
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

bool AudioFlinger::dumpTryLock(Mutex& mutex)
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
        bool hardwareLocked = dumpTryLock(mHardwareLock);
        if (!hardwareLocked) {
            String8 result(kHardwareLockedString);
            write(fd, result.string(), result.size());
        } else {
            mHardwareLock.unlock();
        }

        bool locked = dumpTryLock(mLock);

        // failed to lock - AudioFlinger is probably deadlocked
        if (!locked) {
            String8 result(kDeadlockedString);
            write(fd, result.string(), result.size());
        }

        bool clientLocked = dumpTryLock(mClientLock);
        if (!clientLocked) {
            String8 result(kClientLockedString);
            write(fd, result.string(), result.size());
        }
        dumpClients(fd, args);
        if (clientLocked) {
            mClientLock.unlock();
        }

        dumpInternals(fd, args);

        // dump playback threads
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->dump(fd, args);
        }

        // dump record threads
        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            mRecordThreads.valueAt(i)->dump(fd, args);
        }

        // dump orphan effect chains
        if (mOrphanEffectChains.size() != 0) {
            write(fd, "  Orphan Effect Chains\n", strlen("  Orphan Effect Chains\n"));
            for (size_t i = 0; i < mOrphanEffectChains.size(); i++) {
                mOrphanEffectChains.valueAt(i)->dump(fd, args);
            }
        }
        // dump all hardware devs
        for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
            audio_hw_device_t *dev = mAudioHwDevs.valueAt(i)->hwDevice();
            dev->dump(dev, fd);
        }

#ifdef TEE_SINK
        // dump the serially shared record tee sink
        if (mRecordTeeSource != 0) {
            dumpTee(fd, mRecordTeeSource);
        }
#endif

        if (locked) {
            mLock.unlock();
        }

        // append a copy of media.log here by forwarding fd to it, but don't attempt
        // to lookup the service if it's not running, as it will block for a second
        if (mLogMemoryDealer != 0) {
            sp<IBinder> binder = defaultServiceManager()->getService(String16("media.log"));
            if (binder != 0) {
                dprintf(fd, "\nmedia.log:\n");
                Vector<String16> args;
                binder->dump(fd, args);
            }
        }
    }
    return NO_ERROR;
}

sp<AudioFlinger::Client> AudioFlinger::registerPid(pid_t pid)
{
    Mutex::Autolock _cl(mClientLock);
    // If pid is already in the mClients wp<> map, then use that entry
    // (for which promote() is always != 0), otherwise create a new entry and Client.
    sp<Client> client = mClients.valueFor(pid).promote();
    if (client == 0) {
        client = new Client(this, pid);
        mClients.add(pid, client);
    }

    return client;
}

sp<NBLog::Writer> AudioFlinger::newWriter_l(size_t size, const char *name)
{
    // If there is no memory allocated for logs, return a dummy writer that does nothing
    if (mLogMemoryDealer == 0) {
        return new NBLog::Writer();
    }
    sp<IBinder> binder = defaultServiceManager()->getService(String16("media.log"));
    // Similarly if we can't contact the media.log service, also return a dummy writer
    if (binder == 0) {
        return new NBLog::Writer();
    }
    sp<IMediaLogService> mediaLogService(interface_cast<IMediaLogService>(binder));
    sp<IMemory> shared = mLogMemoryDealer->allocate(NBLog::Timeline::sharedSize(size));
    // If allocation fails, consult the vector of previously unregistered writers
    // and garbage-collect one or more them until an allocation succeeds
    if (shared == 0) {
        Mutex::Autolock _l(mUnregisteredWritersLock);
        for (size_t count = mUnregisteredWriters.size(); count > 0; count--) {
            {
                // Pick the oldest stale writer to garbage-collect
                sp<IMemory> iMemory(mUnregisteredWriters[0]->getIMemory());
                mUnregisteredWriters.removeAt(0);
                mediaLogService->unregisterWriter(iMemory);
                // Now the media.log remote reference to IMemory is gone.  When our last local
                // reference to IMemory also drops to zero at end of this block,
                // the IMemory destructor will deallocate the region from mLogMemoryDealer.
            }
            // Re-attempt the allocation
            shared = mLogMemoryDealer->allocate(NBLog::Timeline::sharedSize(size));
            if (shared != 0) {
                goto success;
            }
        }
        // Even after garbage-collecting all old writers, there is still not enough memory,
        // so return a dummy writer
        return new NBLog::Writer();
    }
success:
    mediaLogService->registerWriter(shared, size, name);
    return new NBLog::Writer(size, shared);
}

void AudioFlinger::unregisterWriter(const sp<NBLog::Writer>& writer)
{
    if (writer == 0) {
        return;
    }
    sp<IMemory> iMemory(writer->getIMemory());
    if (iMemory == 0) {
        return;
    }
    // Rather than removing the writer immediately, append it to a queue of old writers to
    // be garbage-collected later.  This allows us to continue to view old logs for a while.
    Mutex::Autolock _l(mUnregisteredWritersLock);
    mUnregisteredWriters.push(writer);
}

// IAudioFlinger interface


sp<IAudioTrack> AudioFlinger::createTrack(
        audio_stream_type_t streamType,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *frameCount,
        IAudioFlinger::track_flags_t *flags,
        const sp<IMemory>& sharedBuffer,
        audio_io_handle_t output,
        pid_t tid,
        int *sessionId,
        int clientUid,
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

    // further sample rate checks are performed by createTrack_l() depending on the thread type
    if (sampleRate == 0) {
        ALOGE("createTrack() invalid sample rate %u", sampleRate);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    // further channel mask checks are performed by createTrack_l() depending on the thread type
    if (!audio_is_output_channel(channelMask)) {
        ALOGE("createTrack() invalid channel mask %#x", channelMask);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    // further format checks are performed by createTrack_l() depending on the thread type
    if (!audio_is_valid_format(format)) {
        ALOGE("createTrack() invalid format %#x", format);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    if (sharedBuffer != 0 && sharedBuffer->pointer() == NULL) {
        ALOGE("createTrack() sharedBuffer is non-0 but has NULL pointer()");
        lStatus = BAD_VALUE;
        goto Exit;
    }

    {
        Mutex::Autolock _l(mLock);
        PlaybackThread *thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            ALOGE("no playback thread found for output handle %d", output);
            lStatus = BAD_VALUE;
            goto Exit;
        }

        pid_t pid = IPCThreadState::self()->getCallingPid();
        client = registerPid(pid);

        PlaybackThread *effectThread = NULL;
        if (sessionId != NULL && *sessionId != AUDIO_SESSION_ALLOCATE) {
            lSessionId = *sessionId;
            // check if an effect chain with the same session ID is present on another
            // output thread and move it here.
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                sp<PlaybackThread> t = mPlaybackThreads.valueAt(i);
                if (mPlaybackThreads.keyAt(i) != output) {
                    uint32_t sessions = t->hasAudioSession(lSessionId);
                    if (sessions & PlaybackThread::EFFECT_SESSION) {
                        effectThread = t.get();
                        break;
                    }
                }
            }
        } else {
            // if no audio session id is provided, create one here
            lSessionId = nextUniqueId();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
        }
        ALOGV("createTrack() lSessionId: %d", lSessionId);

        track = thread->createTrack_l(client, streamType, sampleRate, format,
                channelMask, frameCount, sharedBuffer, lSessionId, flags, tid, clientUid, &lStatus);
        LOG_ALWAYS_FATAL_IF((lStatus == NO_ERROR) && (track == 0));
        // we don't abort yet if lStatus != NO_ERROR; there is still work to be done regardless

        // move effect chain to this output thread if an effect on same session was waiting
        // for a track to be created
        if (lStatus == NO_ERROR && effectThread != NULL) {
            // no risk of deadlock because AudioFlinger::mLock is held
            Mutex::Autolock _dl(thread->mLock);
            Mutex::Autolock _sl(effectThread->mLock);
            moveEffectChain_l(lSessionId, effectThread, thread, true);
        }

        // Look for sync events awaiting for a session to be used.
        for (size_t i = 0; i < mPendingSyncEvents.size(); i++) {
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

        setAudioHwSyncForSession_l(thread, (audio_session_t)lSessionId);
    }

    if (lStatus != NO_ERROR) {
        // remove local strong reference to Client before deleting the Track so that the
        // Client destructor is called by the TrackBase destructor with mClientLock held
        // Don't hold mClientLock when releasing the reference on the track as the
        // destructor will acquire it.
        {
            Mutex::Autolock _cl(mClientLock);
            client.clear();
        }
        track.clear();
        goto Exit;
    }

    // return handle to client
    trackHandle = new TrackHandle(track);

Exit:
    *status = lStatus;
    return trackHandle;
}

#ifdef QCOM_DIRECTTRACK
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
#endif

uint32_t AudioFlinger::sampleRate(audio_io_handle_t output) const
{
    Mutex::Autolock _l(mLock);
#ifdef QCOM_DIRECTTRACK
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
#ifdef QCOM_DIRECTTRACK
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
        ALOGW("latency(): no playback thread found for output handle %d", output);
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
    mHardwareStatus = AUDIO_HW_SET_MIC_MUTE;
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        audio_hw_device_t *dev = mAudioHwDevs.valueAt(i)->hwDevice();
        status_t result = dev->set_mic_mute(dev, state);
        if (result != NO_ERROR) {
            ret = result;
        }
    }
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
    for (size_t i = 0; i < mAudioHwDevs.size(); i++) {
        AutoMutex lock(mHardwareLock);
        AudioHwDevice *dev = mAudioHwDevs.valueAt(i);

        mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
        if (dev->canSetMasterMute()) {
            dev->hwDevice()->set_master_mute(dev->hwDevice(), muted);
        }
        mHardwareStatus = AUDIO_HW_IDLE;
    }

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

status_t AudioFlinger::checkStreamType(audio_stream_type_t stream) const
{
    if (uint32_t(stream) >= AUDIO_STREAM_CNT) {
        ALOGW("setStreamVolume() invalid stream %d", stream);
        return BAD_VALUE;
    }
    pid_t caller = IPCThreadState::self()->getCallingPid();
    if (uint32_t(stream) >= AUDIO_STREAM_PUBLIC_CNT && caller != getpid_cached) {
        ALOGW("setStreamVolume() pid %d cannot use internal stream type %d", caller, stream);
        return PERMISSION_DENIED;
    }

    return NO_ERROR;
}

status_t AudioFlinger::setStreamVolume(audio_stream_type_t stream, float value,
        audio_io_handle_t output)
{
    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return status;
    }
    ALOG_ASSERT(stream != AUDIO_STREAM_PATCH, "attempt to change AUDIO_STREAM_PATCH volume");

    AutoMutex lock(mLock);
#ifdef QCOM_DIRECTTRACK
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
    if (output != AUDIO_IO_HANDLE_NONE) {
        thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
#ifdef QCOM_DIRECTTRACK
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

    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return status;
    }
    ALOG_ASSERT(stream != AUDIO_STREAM_PATCH, "attempt to mute AUDIO_STREAM_PATCH");

    if (uint32_t(stream) == AUDIO_STREAM_ENFORCED_AUDIBLE) {
        ALOGE("setStreamMute() invalid stream %d", stream);
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mStreamTypes[stream].mute = muted;
    for (size_t i = 0; i < mPlaybackThreads.size(); i++)
        mPlaybackThreads.valueAt(i)->setStreamMute(stream, muted);

    return NO_ERROR;
}

float AudioFlinger::streamVolume(audio_stream_type_t stream, audio_io_handle_t output) const
{
    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return 0.0f;
    }

    AutoMutex lock(mLock);
    float volume;
    if (output != AUDIO_IO_HANDLE_NONE) {
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
    status_t status = checkStreamType(stream);
    if (status != NO_ERROR) {
        return true;
    }

    AutoMutex lock(mLock);
    return streamMute_l(stream);
}

status_t AudioFlinger::setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs)
{
    ALOGV("setParameters(): io %d, keyvalue %s, calling pid %d",
            ioHandle, keyValuePairs.string(), IPCThreadState::self()->getCallingPid());

    // check calling permissions
    if (!settingsAllowed()) {
        return PERMISSION_DENIED;
    }

    // AUDIO_IO_HANDLE_NONE means the parameters are global to the audio hardware interface
    if (ioHandle == AUDIO_IO_HANDLE_NONE) {
        Mutex::Autolock _l(mLock);
        status_t final_result = NO_ERROR;

#ifdef SRS_PROCESSING
        POSTPRO_PATCH_PARAMS_SET(keyValuePairs);
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
            thread->setPostPro();
        }
#endif
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

        // invalidate all tracks of type MUSIC. This is handled in the player as a teardown
        // event and can be used for fallback and retry.
        AudioParameter param = AudioParameter(keyValuePairs);
        String8 value, key;
        int i = 0;

        key = String8("SND_CARD_STATUS");
        if (param.get(key, value) == NO_ERROR) {
            ALOGV("Set keySoundCardStatus:%s", value.string());
            if ((value.find("OFFLINE", 0) != -1) ) {
                ALOGV("OFFLINE detected - call InvalidateTracks()");
                for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                    PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
                    thread->onFatalError();
                }
           } else if ((value.find("OFFLINE", 0) != -1) ) {
                ALOGV("ONLINE detected - what should I do?");
           }
        }
#ifdef QCOM_DIRECTTRACK
        key = String8(AudioParameter::keyADSPStatus);
        if (param.get(key, value) == NO_ERROR) {
            ALOGV("Set keyADSPStatus:%s", value.string());
            if (value == "ONLINE" || value == "OFFLINE") {
               if (!mDirectAudioTracks.isEmpty()) {
                   for (size_t i=0; i < mDirectAudioTracks.size(); i++) {
                       mDirectAudioTracks.valueAt(i)->stream->common.set_parameters(
                          &mDirectAudioTracks.valueAt(i)->stream->common, keyValuePairs.string());
                   }
               }
           }
        }
#endif

        // disable AEC and NS if the device is a BT SCO headset supporting those pre processings
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
            if (isOff != (AudioFlinger::mScreenState & 1)) {
                AudioFlinger::mScreenState = ((AudioFlinger::mScreenState & ~1) + 2) | isOff;
            }
        }
#ifdef HW_ACC_HPX
        if (param.get(String8("HPX"), value) == NO_ERROR)
            AudioFlinger::mIsHPXOn = !(value == "OFF");
#endif
        return final_result;
    }

#ifdef QCOM_DIRECTTRACK
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
                mDirectDevice = device;
#ifdef SRS_PROCESSING
                ALOGV("setParameters:: routing change to device %d", device);
                POSTPRO_PATCH_ICS_OUTPROC_MIX_ROUTE(desc->trackRefPtr, param, device);
                if(desc->flag & AUDIO_OUTPUT_FLAG_TUNNEL)
                    audioConfigChanged(AudioSystem::EFFECT_CONFIG_CHANGED, 0, NULL);
#endif
                if(mLPAEffectChain != NULL){
                    mLPAEffectChain->setDevice_l(device);
                    audioConfigChanged(AudioSystem::EFFECT_CONFIG_CHANGED, 0, NULL);
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
            if ((param.getInt(String8(AudioParameter::keyRouting), value) == NO_ERROR) &&
                    (value != 0)) {
                for (size_t i = 0; i < mRecordThreads.size(); i++) {
                    mRecordThreads.valueAt(i)->setParameters(keyValuePairs);
                }
            }
        }
    }
    if (thread != 0) {
        return thread->setParameters(keyValuePairs);
    }
    return BAD_VALUE;
}

String8 AudioFlinger::getParameters(audio_io_handle_t ioHandle, const String8& keys) const
{
    ALOGVV("getParameters() io %d, keys %s, calling pid %d",
            ioHandle, keys.string(), IPCThreadState::self()->getCallingPid());

    Mutex::Autolock _l(mLock);

    if (ioHandle == AUDIO_IO_HANDLE_NONE) {
        String8 out_s8;

#ifdef SRS_PROCESSING
        POSTPRO_PATCH_PARAMS_GET(keys, out_s8);
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
    audio_config_t config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = sampleRate;
    config.channel_mask = channelMask;
    config.format = format;

    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
    size_t size = dev->get_input_buffer_size(dev, &config);
    mHardwareStatus = AUDIO_HW_IDLE;
    return size;
}

uint32_t AudioFlinger::getInputFramesLost(audio_io_handle_t ioHandle) const
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

void AudioFlinger::registerClient(const sp<IAudioFlingerClient>& client)
{
    Mutex::Autolock _l(mLock);
    if (client == 0) {
        return;
    }
    bool clientAdded = false;
    {
        Mutex::Autolock _cl(mClientLock);

        pid_t pid = IPCThreadState::self()->getCallingPid();
        if (mNotificationClients.indexOfKey(pid) < 0) {
            sp<NotificationClient> notificationClient = new NotificationClient(this,
                                                                                client,
                                                                                pid);
            ALOGV("registerClient() client %p, pid %d", notificationClient.get(), pid);

            mNotificationClients.add(pid, notificationClient);

            sp<IBinder> binder = client->asBinder();
            binder->linkToDeath(notificationClient);
            clientAdded = true;
        }
    }

    // mClientLock should not be held here because ThreadBase::sendIoConfigEvent() will lock the
    // ThreadBase mutex and the locking order is ThreadBase::mLock then AudioFlinger::mClientLock.
    if (clientAdded) {
        // the config change is always sent from playback or record threads to avoid deadlock
        // with AudioSystem::gLock
        for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
            mPlaybackThreads.valueAt(i)->sendIoConfigEvent(AudioSystem::OUTPUT_OPENED);
        }

        for (size_t i = 0; i < mRecordThreads.size(); i++) {
            mRecordThreads.valueAt(i)->sendIoConfigEvent(AudioSystem::INPUT_OPENED);
        }
    }
}

#ifdef QCOM_DIRECTTRACK
status_t AudioFlinger::deregisterClient(const sp<IAudioFlingerClient>& client)
{
    ALOGV("deregisterClient() %p, tid %d, calling tid %d", client.get(), gettid(), IPCThreadState::self()->getCallingPid());
    Mutex::Autolock _l(mLock);

    pid_t pid = IPCThreadState::self()->getCallingPid();
    int index = mNotificationClients.indexOfKey(pid);
    if (index >= 0) {
        mNotificationClients.removeItemsAt(index);
        return true;
    }

    return false;
}
#endif

void AudioFlinger::removeNotificationClient(pid_t pid)
{
    Mutex::Autolock _l(mLock);
    {
        Mutex::Autolock _cl(mClientLock);
        mNotificationClients.removeItem(pid);
    }

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

void AudioFlinger::audioConfigChanged(int event, audio_io_handle_t ioHandle, const void *param2)
{
    Mutex::Autolock _l(mClientLock);
#ifdef QCOM_DIRECTTRACK
    ALOGV("AudioFlinger::audioConfigChanged_l: event %d", event);
    if (event == AudioSystem::EFFECT_CONFIG_CHANGED) {
        mIsEffectConfigChanged = true;
    }

    if (!mNotificationClients.isEmpty()){
#endif
        size_t size = mNotificationClients.size();
        for (size_t i = 0; i < size; i++) {
            mNotificationClients.valueAt(i)->audioFlingerClient()->ioConfigChanged(event, ioHandle,
                                                                                   param2);
        }
#ifdef QCOM_DIRECTTRACK
    }
    if ((!mDirectAudioTracks.isEmpty())&& (event == AudioSystem::EFFECT_CONFIG_CHANGED)){
        size_t dsize = mDirectAudioTracks.size();
        for(size_t i = 0; i < dsize; i++) {
            AudioSessionDescriptor *desc = mDirectAudioTracks.valueAt(i);
            if(desc && ((DirectAudioTrack*)desc->trackRefPtr)) {
                ALOGV("signalling directAudioTrack ");
                ((DirectAudioTrack*)desc->trackRefPtr)->signalEffect();
            } else{
                ALOGV("not found track to signal directAudioTrack ");
            }
        }

    }
#endif
}
// removeClient_l() must be called with AudioFlinger::mClientLock held
void AudioFlinger::removeClient_l(pid_t pid)
{
    ALOGV("removeClient_l() pid %d, calling pid %d", pid,
            IPCThreadState::self()->getCallingPid());
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



void AudioFlinger::PlaybackThread::setPostPro()
{
    Mutex::Autolock _l(mLock);
    if (mType == OFFLOAD)
        broadcast_l();
}
// ----------------------------------------------------------------------------

AudioFlinger::Client::Client(const sp<AudioFlinger>& audioFlinger, pid_t pid)
    :   RefBase(),
        mAudioFlinger(audioFlinger),
        // FIXME should be a "k" constant not hard-coded, in .h or ro. property, see 4 lines below
        mMemoryDealer(new MemoryDealer(2048*1024, "AudioFlinger::Client")), //2MB
        mPid(pid),
        mTimedTrackCount(0)
{
    // 1 MB of address space is good for 32 tracks, 8 buffers each, 4 KB/buffer
}

// Client destructor must be called with AudioFlinger::mClientLock held
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
                                                     pid_t pid)
    : mAudioFlinger(audioFlinger), mPid(pid), mAudioFlingerClient(client)
{
}

AudioFlinger::NotificationClient::~NotificationClient()
{
}

void AudioFlinger::NotificationClient::binderDied(const wp<IBinder>& who __unused)
{
    sp<NotificationClient> keep(this);
    mAudioFlinger->removeNotificationClient(mPid);
}


// ----------------------------------------------------------------------------

static bool deviceRequiresCaptureAudioOutputPermission(audio_devices_t inDevice) {
    return audio_is_remote_submix_device(inDevice);
}

sp<IAudioRecord> AudioFlinger::openRecord(
        audio_io_handle_t input,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t *frameCount,
        IAudioFlinger::track_flags_t *flags,
        pid_t tid,
        int *sessionId,
        size_t *notificationFrames,
        sp<IMemory>& cblk,
        sp<IMemory>& buffers,
        status_t *status)
{
    sp<RecordThread::RecordTrack> recordTrack;
    sp<RecordHandle> recordHandle;
    sp<Client> client;
    status_t lStatus;
    int lSessionId;

    cblk.clear();
    buffers.clear();

    // check calling permissions
    if (!recordingAllowed()) {
        ALOGE("openRecord() permission denied: recording not allowed");
        lStatus = PERMISSION_DENIED;
        goto Exit;
    }

    // further sample rate checks are performed by createRecordTrack_l()
    if (sampleRate == 0) {
        ALOGE("openRecord() invalid sample rate %u", sampleRate);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    // we don't yet support anything other than 16-bit PCM and compress formats
    if (format != AUDIO_FORMAT_PCM_16_BIT &&
            !audio_is_compress_voip_format(format) &&
            !audio_is_compress_capture_format(format)) {
        ALOGE("openRecord() invalid format %#x", format);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    // further channel mask checks are performed by createRecordTrack_l()
    if (!audio_is_input_channel(channelMask)) {
        ALOGE("openRecord() invalid channel mask %#x", channelMask);
        lStatus = BAD_VALUE;
        goto Exit;
    }

    {
        Mutex::Autolock _l(mLock);
        RecordThread *thread = checkRecordThread_l(input);
        if (thread == NULL) {
            ALOGE("openRecord() checkRecordThread_l failed");
            lStatus = BAD_VALUE;
            goto Exit;
        }

        pid_t pid = IPCThreadState::self()->getCallingPid();
        client = registerPid(pid);

        if (sessionId != NULL && *sessionId != AUDIO_SESSION_ALLOCATE) {
            lSessionId = *sessionId;
        } else {
            // if no audio session id is provided, create one here
            lSessionId = nextUniqueId();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
        }
        ALOGV("openRecord() lSessionId: %d input %d", lSessionId, input);

        // TODO: the uid should be passed in as a parameter to openRecord
        recordTrack = thread->createRecordTrack_l(client, sampleRate, format, channelMask,
                                                  frameCount, lSessionId, notificationFrames,
                                                  IPCThreadState::self()->getCallingUid(),
                                                  flags, tid, &lStatus);
        LOG_ALWAYS_FATAL_IF((lStatus == NO_ERROR) && (recordTrack == 0));

        if (lStatus == NO_ERROR) {
            // Check if one effect chain was awaiting for an AudioRecord to be created on this
            // session and move it to this thread.
            sp<EffectChain> chain = getOrphanEffectChain_l((audio_session_t)lSessionId);
            if (chain != 0) {
                Mutex::Autolock _l(thread->mLock);
                thread->addEffectChain_l(chain);
            }
        }
    }

    if (lStatus != NO_ERROR) {
        // remove local strong reference to Client before deleting the RecordTrack so that the
        // Client destructor is called by the TrackBase destructor with mClientLock held
        // Don't hold mClientLock when releasing the reference on the track as the
        // destructor will acquire it.
        {
            Mutex::Autolock _cl(mClientLock);
            client.clear();
        }
        recordTrack.clear();
        goto Exit;
    }

    cblk = recordTrack->getCblk();
    buffers = recordTrack->getBuffers();

    // return handle to client
    recordHandle = new RecordHandle(recordTrack);

Exit:
    *status = lStatus;
    return recordHandle;
}



// ----------------------------------------------------------------------------

audio_module_handle_t AudioFlinger::loadHwModule(const char *name)
{
    if (name == NULL) {
        return 0;
    }
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

        mHardwareStatus = AUDIO_HW_SET_MASTER_VOLUME;
        if ((NULL != dev->set_master_volume) &&
            (OK == dev->set_master_volume(dev, mMasterVolume))) {
            flags = static_cast<AudioHwDevice::Flags>(flags |
                    AudioHwDevice::AHWD_CAN_SET_MASTER_VOLUME);
        }

        mHardwareStatus = AUDIO_HW_SET_MASTER_MUTE;
        if ((NULL != dev->set_master_mute) &&
            (OK == dev->set_master_mute(dev, mMasterMute))) {
            flags = static_cast<AudioHwDevice::Flags>(flags |
                    AudioHwDevice::AHWD_CAN_SET_MASTER_MUTE);
        }

        mHardwareStatus = AUDIO_HW_IDLE;
    }

    audio_module_handle_t handle = nextUniqueId();
    mAudioHwDevs.add(handle, new AudioHwDevice(handle, name, dev, flags));

    ALOGI("loadHwModule() Loaded %s audio interface from %s (%s) handle %d",
          name, dev->common.module->name, dev->common.module->id, handle);

    return handle;

}

// ----------------------------------------------------------------------------

uint32_t AudioFlinger::getPrimaryOutputSamplingRate()
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = primaryPlaybackThread_l();
    return thread != NULL ? thread->sampleRate() : 0;
}

size_t AudioFlinger::getPrimaryOutputFrameCount()
{
    Mutex::Autolock _l(mLock);
    PlaybackThread *thread = primaryPlaybackThread_l();
    return thread != NULL ? thread->frameCountHAL() : 0;
}

// ----------------------------------------------------------------------------

status_t AudioFlinger::setLowRamDevice(bool isLowRamDevice)
{
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid != AID_SYSTEM) {
        return PERMISSION_DENIED;
    }
    Mutex::Autolock _l(mLock);
    if (mIsDeviceTypeKnown) {
        return INVALID_OPERATION;
    }
    mIsLowRamDevice = isLowRamDevice;
    mIsDeviceTypeKnown = true;
    return NO_ERROR;
}

audio_hw_sync_t AudioFlinger::getAudioHwSyncForSession(audio_session_t sessionId)
{
    Mutex::Autolock _l(mLock);

    ssize_t index = mHwAvSyncIds.indexOfKey(sessionId);
    if (index >= 0) {
        ALOGV("getAudioHwSyncForSession found ID %d for session %d",
              mHwAvSyncIds.valueAt(index), sessionId);
        return mHwAvSyncIds.valueAt(index);
    }

    audio_hw_device_t *dev = mPrimaryHardwareDev->hwDevice();
    if (dev == NULL) {
        return AUDIO_HW_SYNC_INVALID;
    }
    char *reply = dev->get_parameters(dev, AUDIO_PARAMETER_HW_AV_SYNC);
    AudioParameter param = AudioParameter(String8(reply));
    free(reply);

    int value;
    if (param.getInt(String8(AUDIO_PARAMETER_HW_AV_SYNC), value) != NO_ERROR) {
        ALOGW("getAudioHwSyncForSession error getting sync for session %d", sessionId);
        return AUDIO_HW_SYNC_INVALID;
    }

    // allow only one session for a given HW A/V sync ID.
    for (size_t i = 0; i < mHwAvSyncIds.size(); i++) {
        if (mHwAvSyncIds.valueAt(i) == (audio_hw_sync_t)value) {
            ALOGV("getAudioHwSyncForSession removing ID %d for session %d",
                  value, mHwAvSyncIds.keyAt(i));
            mHwAvSyncIds.removeItemsAt(i);
            break;
        }
    }

    mHwAvSyncIds.add(sessionId, value);

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<PlaybackThread> thread = mPlaybackThreads.valueAt(i);
        uint32_t sessions = thread->hasAudioSession(sessionId);
        if (sessions & PlaybackThread::TRACK_SESSION) {
            AudioParameter param = AudioParameter();
            param.addInt(String8(AUDIO_PARAMETER_STREAM_HW_AV_SYNC), value);
            thread->setParameters(param.toString());
            break;
        }
    }

    ALOGV("getAudioHwSyncForSession adding ID %d for session %d", value, sessionId);
    return (audio_hw_sync_t)value;
}

// setAudioHwSyncForSession_l() must be called with AudioFlinger::mLock held
void AudioFlinger::setAudioHwSyncForSession_l(PlaybackThread *thread, audio_session_t sessionId)
{
    ssize_t index = mHwAvSyncIds.indexOfKey(sessionId);
    if (index >= 0) {
        audio_hw_sync_t syncId = mHwAvSyncIds.valueAt(index);
        ALOGV("setAudioHwSyncForSession_l found ID %d for session %d", syncId, sessionId);
        AudioParameter param = AudioParameter();
        param.addInt(String8(AUDIO_PARAMETER_STREAM_HW_AV_SYNC), syncId);
        thread->setParameters(param.toString());
    }
}


// ----------------------------------------------------------------------------


sp<AudioFlinger::PlaybackThread> AudioFlinger::openOutput_l(audio_module_handle_t module,
                                                            audio_io_handle_t *output,
                                                            audio_config_t *config,
                                                            audio_devices_t devices,
                                                            const String8& address,
                                                            audio_output_flags_t flags)
{
    AudioHwDevice *outHwDev = findSuitableHwDev_l(module, devices);
    if (outHwDev == NULL) {
        return 0;
    }

    audio_hw_device_t *hwDevHal = outHwDev->hwDevice();
    if (*output == AUDIO_IO_HANDLE_NONE) {
        *output = nextUniqueId();
    }

    mHardwareStatus = AUDIO_HW_OUTPUT_OPEN;

    audio_stream_out_t *outStream = NULL;

    // FOR TESTING ONLY:
    // This if statement allows overriding the audio policy settings
    // and forcing a specific format or channel mask to the HAL/Sink device for testing.
    if (!(flags & (AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD | AUDIO_OUTPUT_FLAG_DIRECT))) {
        // Check only for Normal Mixing mode
        if (kEnableExtendedPrecision) {
            // Specify format (uncomment one below to choose)
            //config->format = AUDIO_FORMAT_PCM_FLOAT;
            //config->format = AUDIO_FORMAT_PCM_24_BIT_PACKED;
            //config->format = AUDIO_FORMAT_PCM_32_BIT;
            //config->format = AUDIO_FORMAT_PCM_8_24_BIT;
            // ALOGV("openOutput_l() upgrading format to %#08x", config->format);
        }
        if (kEnableExtendedChannels) {
            // Specify channel mask (uncomment one below to choose)
            //config->channel_mask = audio_channel_out_mask_from_count(4);  // for USB 4ch
            //config->channel_mask = audio_channel_mask_from_representation_and_bits(
            //        AUDIO_CHANNEL_REPRESENTATION_INDEX, (1 << 4) - 1);  // another 4ch example
        }
    }

    status_t status = hwDevHal->open_output_stream(hwDevHal,
                                                   *output,
                                                   devices,
                                                   flags,
                                                   config,
                                                   &outStream,
                                                   address.string());

#ifdef QCOM_DIRECTTRACK
    /* if (flags & AUDIO_OUTPUT_FLAG_LPA || flags & AUDIO_OUTPUT_FLAG_TUNNEL ) {
        AudioSessionDescriptor *desc = new AudioSessionDescriptor(hwDevHal, outStream, flags);
        desc->mActive = true;
          //TODO: no stream type
            //desc->mStreamType = streamType;
        desc->mVolumeLeft = 1.0;
        desc->mVolumeRight = 1.0;
        desc->device = devices;
        mDirectAudioTracks.add(*output, desc);
    }*/
#endif
    mHardwareStatus = AUDIO_HW_IDLE;
    ALOGV("openOutput_l() openOutputStream returned output %p, sampleRate %d, Format %#x, "
            "channelMask %#x, status %d",
            outStream,
            config->sample_rate,
            config->format,
            config->channel_mask,
            status);

    if (status == NO_ERROR && outStream != NULL) {
        ALOGV("openOutput_l() Creating AudioStreamOut");
        AudioStreamOut *outputStream = new AudioStreamOut(outHwDev, outStream, flags);
        ALOGV("openOutput_l() Created AudioStreamOut");
        PlaybackThread *thread=NULL;
        if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
            thread = new OffloadThread(this, outputStream, *output, devices);
            ALOGV("openOutput_l() created offload output: ID %d thread %p", *output, thread);
#ifdef QCOM_DIRECTTRACK
        }  else if (flags & AUDIO_OUTPUT_FLAG_LPA || flags & AUDIO_OUTPUT_FLAG_TUNNEL ) {
            ALOGE("openOutput_l() created LPA/Tunnel output");
            AudioSessionDescriptor *desc = new AudioSessionDescriptor(hwDevHal, outStream, flags);
            desc->mActive = true;
          //TODO: no stream type
            //desc->mStreamType = streamType;
            desc->mVolumeLeft = 1.0;
            desc->mVolumeRight = 1.0;
            desc->device = devices;
            mDirectAudioTracks.add(*output, desc);
#endif
        }  else if ((flags & AUDIO_OUTPUT_FLAG_DIRECT)
                || !isValidPcmSinkFormat(config->format)
                || !isValidPcmSinkChannelMask(config->channel_mask)) {
            thread = new DirectOutputThread(this, outputStream, *output, devices);
            ALOGV("openOutput_l() created direct output: ID %d thread %p", *output, thread);
        }  else {
            thread = new MixerThread(this, outputStream, *output, devices);
            ALOGV("openOutput_l() created mixer output: ID %d thread %p", *output, thread);
        }
        if (thread != NULL) {
            mPlaybackThreads.add(*output, thread);
        }

        return thread;
    }
    return 0;
}

status_t AudioFlinger::openOutput(audio_module_handle_t module,
                                  audio_io_handle_t *output,
                                  audio_config_t *config,
                                  audio_devices_t *devices,
                                  const String8& address,
                                  uint32_t *latencyMs,
                                  audio_output_flags_t flags)
{
    ALOGV("openOutput(), module %d Device %x, SamplingRate %d, Format %#08x, Channels %x, flags %x",
              module,
              (devices != NULL) ? *devices : 0,
              config->sample_rate,
              config->format,
              config->channel_mask,
              flags);

    if (*devices == AUDIO_DEVICE_NONE) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mLock);

    sp<PlaybackThread> thread = openOutput_l(module, output, config, *devices, address, flags);
    if (thread != 0) {
        *latencyMs = thread->latency();

        // notify client processes of the new output creation
        thread->audioConfigChanged(AudioSystem::OUTPUT_OPENED);

        // the first primary output opened designates the primary hw device
        if ((mPrimaryHardwareDev == NULL) && (flags & AUDIO_OUTPUT_FLAG_PRIMARY)) {
            ALOGI("Using module %d has the primary audio interface", module);
            mPrimaryHardwareDev = thread->getOutput()->audioHwDev;

            AutoMutex lock(mHardwareLock);
            mHardwareStatus = AUDIO_HW_SET_MODE;
            mPrimaryHardwareDev->hwDevice()->set_mode(mPrimaryHardwareDev->hwDevice(), mMode);
            mHardwareStatus = AUDIO_HW_IDLE;

            mPrimaryOutputSampleRate = config->sample_rate;
        }
        return NO_ERROR;
#ifdef QCOM_DIRECTTRACK
    } else {
          *latencyMs = 0;
          if ((flags & AUDIO_OUTPUT_FLAG_LPA) || (flags & AUDIO_OUTPUT_FLAG_TUNNEL)) {
              AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(*output);
              if(desc != NULL) {
                 *latencyMs = desc->stream->get_latency(desc->stream);
                 return NO_ERROR;
              } else {
                 return NO_INIT;
              }
          }
#endif
    }
    return NO_INIT;
}

audio_io_handle_t AudioFlinger::openDuplicateOutput(audio_io_handle_t output1,
        audio_io_handle_t output2)
{
    Mutex::Autolock _l(mLock);
    MixerThread *thread1 = checkMixerThread_l(output1);
    MixerThread *thread2 = checkMixerThread_l(output2);

    if (thread1 == NULL || thread2 == NULL) {
        ALOGW("openDuplicateOutput() wrong output mixer type for output %d or %d", output1,
                output2);
        return AUDIO_IO_HANDLE_NONE;
    }

    audio_io_handle_t id = nextUniqueId();
    DuplicatingThread *thread = new DuplicatingThread(this, thread1, id);
    thread->addOutputTrack(thread2);
    mPlaybackThreads.add(id, thread);
    // notify client processes of the new output creation
    thread->audioConfigChanged(AudioSystem::OUTPUT_OPENED);
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
    sp<PlaybackThread> thread;
    {
        Mutex::Autolock _l(mLock);
#ifdef QCOM_DIRECTTRACK
        AudioSessionDescriptor *desc = mDirectAudioTracks.valueFor(output);
        if (desc) {
            ALOGV("Closing DirectTrack output %d", output);
            desc->mActive = false;
            desc->stream->common.standby(&desc->stream->common);
            desc->hwDev->close_output_stream(desc->hwDev, desc->stream);
            desc->trackRefPtr = NULL;
            desc->stream = NULL;
            mDirectAudioTracks.removeItem(output);
            audioConfigChanged(AudioSystem::OUTPUT_CLOSED, output, NULL);
            delete desc;
            return NO_ERROR;
        }
#endif

        thread = checkPlaybackThread_l(output);
        if (thread == NULL) {
            return BAD_VALUE;
        }

        ALOGV("closeOutput() %d", output);

        if (thread->type() == ThreadBase::MIXER) {
            for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
                if (mPlaybackThreads.valueAt(i)->type() == ThreadBase::DUPLICATING) {
                    DuplicatingThread *dupThread =
                            (DuplicatingThread *)mPlaybackThreads.valueAt(i).get();
                    dupThread->removeOutputTrack((MixerThread *)thread.get());

                }
            }
        }


        mPlaybackThreads.removeItem(output);
        // save all effects to the default thread
        if (mPlaybackThreads.size()) {
            PlaybackThread *dstThread = checkPlaybackThread_l(mPlaybackThreads.keyAt(0));
            if (dstThread != NULL) {
                // audioflinger lock is held here so the acquisition order of thread locks does not
                // matter
                Mutex::Autolock _dl(dstThread->mLock);
                Mutex::Autolock _sl(thread->mLock);
                Vector< sp<EffectChain> > effectChains = thread->getEffectChains_l();
                for (size_t i = 0; i < effectChains.size(); i ++) {
                    moveEffectChain_l(effectChains[i]->sessionId(), thread.get(), dstThread, true);
                }
            }
        }
        audioConfigChanged(AudioSystem::OUTPUT_CLOSED, output, NULL);
    }
    thread->exit();
    // The thread entity (active unit of execution) is no longer running here,
    // but the ThreadBase container still exists.

    if (thread->type() != ThreadBase::DUPLICATING) {
        closeOutputFinish(thread);
    }

    return NO_ERROR;
}

void AudioFlinger::closeOutputFinish(sp<PlaybackThread> thread)
{
    AudioStreamOut *out = thread->clearOutput();
    ALOG_ASSERT(out != NULL, "out shouldn't be NULL");
    // from now on thread->mOutput is NULL
    out->hwDev()->close_output_stream(out->hwDev(), out->stream);
    delete out;
}

void AudioFlinger::closeOutputInternal_l(sp<PlaybackThread> thread)
{
    mPlaybackThreads.removeItem(thread->mId);
    thread->exit();
    closeOutputFinish(thread);
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

status_t AudioFlinger::openInput(audio_module_handle_t module,
                                          audio_io_handle_t *input,
                                          audio_config_t *config,
                                          audio_devices_t *device,
                                          const String8& address,
                                          audio_source_t source,
                                          audio_input_flags_t flags)
{
    Mutex::Autolock _l(mLock);

    if (*device == AUDIO_DEVICE_NONE) {
        return BAD_VALUE;
    }

    sp<RecordThread> thread = openInput_l(module, input, config, *device, address, source, flags);

    if (thread != 0) {
        // notify client processes of the new input creation
        thread->audioConfigChanged(AudioSystem::INPUT_OPENED);
        return NO_ERROR;
    }
    return NO_INIT;
}

sp<AudioFlinger::RecordThread> AudioFlinger::openInput_l(audio_module_handle_t module,
                                                         audio_io_handle_t *input,
                                                         audio_config_t *config,
                                                         audio_devices_t device,
                                                         const String8& address,
                                                         audio_source_t source,
                                                         audio_input_flags_t flags)
{
    AudioHwDevice *inHwDev = findSuitableHwDev_l(module, device);
    if (inHwDev == NULL) {
        *input = AUDIO_IO_HANDLE_NONE;
        return 0;
    }

    if (*input == AUDIO_IO_HANDLE_NONE) {
        *input = nextUniqueId();
    }

    audio_config_t halconfig = *config;
    audio_hw_device_t *inHwHal = inHwDev->hwDevice();
    audio_stream_in_t *inStream = NULL;
    status_t status = inHwHal->open_input_stream(inHwHal, *input, device, &halconfig,
                                        &inStream, flags, address.string(), source);
    ALOGV("openInput_l() openInputStream returned input %p, SamplingRate %d"
           ", Format %#x, Channels %x, flags %#x, status %d addr %s",
            inStream,
            halconfig.sample_rate,
            halconfig.format,
            halconfig.channel_mask,
            flags,
            status, address.string());

    // If the input could not be opened with the requested parameters and we can handle the
    // conversion internally, try to open again with the proposed parameters. The AudioFlinger can
    // resample the input and do mono to stereo or stereo to mono conversions on 16 bit PCM inputs.
    if (status == BAD_VALUE &&
            config->format == halconfig.format && halconfig.format == AUDIO_FORMAT_PCM_16_BIT &&
#ifdef SPEEX_RESAMPLER
            !AudioResampler::checkRate(config->sample_rate, halconfig.sample_rate) &&
#else
            (halconfig.sample_rate <= 2 * config->sample_rate) &&
#endif
            (audio_channel_count_from_in_mask(halconfig.channel_mask) <= FCC_2) &&
            (audio_channel_count_from_in_mask(config->channel_mask) <= FCC_2)) {
        // FIXME describe the change proposed by HAL (save old values so we can log them here)
        ALOGV("openInput_l() reopening with proposed sampling rate and channel mask");
        inStream = NULL;
        status = inHwHal->open_input_stream(inHwHal, *input, device, &halconfig,
                                            &inStream, flags, address.string(), source);
        // FIXME log this new status; HAL should not propose any further changes
    }

    if (status == NO_ERROR && inStream != NULL) {

#ifdef TEE_SINK
        // Try to re-use most recently used Pipe to archive a copy of input for dumpsys,
        // or (re-)create if current Pipe is idle and does not match the new format
        sp<NBAIO_Sink> teeSink;
        enum {
            TEE_SINK_NO,    // don't copy input
            TEE_SINK_NEW,   // copy input using a new pipe
            TEE_SINK_OLD,   // copy input using an existing pipe
        } kind;
        NBAIO_Format format = Format_from_SR_C(halconfig.sample_rate,
                audio_channel_count_from_in_mask(halconfig.channel_mask), halconfig.format);
        if (!mTeeSinkInputEnabled) {
            kind = TEE_SINK_NO;
        } else if (!Format_isValid(format)) {
            kind = TEE_SINK_NO;
        } else if (mRecordTeeSink == 0) {
            kind = TEE_SINK_NEW;
        } else if (mRecordTeeSink->getStrongCount() != 1) {
            kind = TEE_SINK_NO;
        } else if (Format_isEqual(format, mRecordTeeSink->format())) {
            kind = TEE_SINK_OLD;
        } else {
            kind = TEE_SINK_NEW;
        }
        switch (kind) {
        case TEE_SINK_NEW: {
            Pipe *pipe = new Pipe(mTeeSinkInputFrames, format);
            size_t numCounterOffers = 0;
            const NBAIO_Format offers[1] = {format};
            ssize_t index = pipe->negotiate(offers, 1, NULL, numCounterOffers);
            ALOG_ASSERT(index == 0);
            PipeReader *pipeReader = new PipeReader(*pipe);
            numCounterOffers = 0;
            index = pipeReader->negotiate(offers, 1, NULL, numCounterOffers);
            ALOG_ASSERT(index == 0);
            mRecordTeeSink = pipe;
            mRecordTeeSource = pipeReader;
            teeSink = pipe;
            }
            break;
        case TEE_SINK_OLD:
            teeSink = mRecordTeeSink;
            break;
        case TEE_SINK_NO:
        default:
            break;
        }
#endif

        AudioStreamIn *inputStream = new AudioStreamIn(inHwDev, inStream);

        // Start record thread
        // RecordThread requires both input and output device indication to forward to audio
        // pre processing modules
        sp<RecordThread> thread = new RecordThread(this,
                                  inputStream,
                                  *input,
                                  primaryOutputDevice_l(),
                                  device
#ifdef TEE_SINK
                                  , teeSink
#endif
                                  );
        mRecordThreads.add(*input, thread);
        ALOGV("openInput_l() created record thread: ID %d thread %p", *input, thread.get());
        return thread;
    }

    *input = AUDIO_IO_HANDLE_NONE;
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

        // If we still have effect chains, it means that a client still holds a handle
        // on at least one effect. We must either move the chain to an existing thread with the
        // same session ID or put it aside in case a new record thread is opened for a
        // new capture on the same session
        sp<EffectChain> chain;
        {
            Mutex::Autolock _sl(thread->mLock);
            Vector< sp<EffectChain> > effectChains = thread->getEffectChains_l();
            // Note: maximum one chain per record thread
            if (effectChains.size() != 0) {
                chain = effectChains[0];
            }
        }
        if (chain != 0) {
            // first check if a record thread is already opened with a client on the same session.
            // This should only happen in case of overlap between one thread tear down and the
            // creation of its replacement
            size_t i;
            for (i = 0; i < mRecordThreads.size(); i++) {
                sp<RecordThread> t = mRecordThreads.valueAt(i);
                if (t == thread) {
                    continue;
                }
                if (t->hasAudioSession(chain->sessionId()) != 0) {
                    Mutex::Autolock _l(t->mLock);
                    ALOGV("closeInput() found thread %d for effect session %d",
                          t->id(), chain->sessionId());
                    t->addEffectChain_l(chain);
                    break;
                }
            }
            // put the chain aside if we could not find a record thread with the same session id.
            if (i == mRecordThreads.size()) {
                putOrphanEffectChain_l(chain);
            }
        }
        audioConfigChanged(AudioSystem::INPUT_CLOSED, input, NULL);
        mRecordThreads.removeItem(input);
    }
    // FIXME: calling thread->exit() without mLock held should not be needed anymore now that
    // we have a different lock for notification client
    closeInputFinish(thread);
    return NO_ERROR;
}

void AudioFlinger::closeInputFinish(sp<RecordThread> thread)
{
    thread->exit();
    AudioStreamIn *in = thread->clearInput();
    ALOG_ASSERT(in != NULL, "in shouldn't be NULL");
    // from now on thread->mInput is NULL
    in->hwDev()->close_input_stream(in->hwDev(), in->stream);
    delete in;
}

void AudioFlinger::closeInputInternal_l(sp<RecordThread> thread)
{
    mRecordThreads.removeItem(thread->mId);
    closeInputFinish(thread);
}

status_t AudioFlinger::invalidateStream(audio_stream_type_t stream)
{
    Mutex::Autolock _l(mLock);
    ALOGV("invalidateStream() stream %d", stream);

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        PlaybackThread *thread = mPlaybackThreads.valueAt(i).get();
        thread->invalidateTracks(stream);
    }

    return NO_ERROR;
}


audio_unique_id_t AudioFlinger::newAudioUniqueId()
{
    return nextUniqueId();
}

void AudioFlinger::acquireAudioSessionId(int audioSession, pid_t pid)
{
    Mutex::Autolock _l(mLock);
    pid_t caller = IPCThreadState::self()->getCallingPid();
    ALOGV("acquiring %d from %d, for %d", audioSession, caller, pid);
    if (pid != -1 && (caller == getpid_cached)) {
        caller = pid;
    }

    {
        Mutex::Autolock _cl(mClientLock);
        // Ignore requests received from processes not known as notification client. The request
        // is likely proxied by mediaserver (e.g CameraService) and releaseAudioSessionId() can be
        // called from a different pid leaving a stale session reference.  Also we don't know how
        // to clear this reference if the client process dies.
        if (mNotificationClients.indexOfKey(caller) < 0) {
            ALOGW("acquireAudioSessionId() unknown client %d for session %d", caller, audioSession);
            return;
        }
    }

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

void AudioFlinger::releaseAudioSessionId(int audioSession, pid_t pid)
{
    Mutex::Autolock _l(mLock);
    pid_t caller = IPCThreadState::self()->getCallingPid();
    ALOGV("releasing %d from %d for %d", audioSession, caller, pid);
    if (pid != -1 && (caller == getpid_cached)) {
        caller = pid;
    }
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
    // If the caller is mediaserver it is likely that the session being released was acquired
    // on behalf of a process not in notification clients and we ignore the warning.
    ALOGW_IF(caller != getpid_cached, "session id %d not found for pid %d", audioSession, caller);
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
            Mutex::Autolock _l(t->mLock);
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
    return (uint32_t) android_atomic_inc(&mNextUniqueId);
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
                                    wp<RefBase> cookie)
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


sp<IEffect> AudioFlinger::createEffect(
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

    pid_t pid = IPCThreadState::self()->getCallingPid();
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

    {
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
        if (io == AUDIO_IO_HANDLE_NONE && sessionId == AUDIO_SESSION_OUTPUT_MIX) {
            // if the output returned by getOutputForEffect() is removed before we lock the
            // mutex below, the call to checkPlaybackThread_l(io) below will detect it
            // and we will exit safely
            io = AudioSystem::getOutputForEffect(&desc);
            ALOGV("createEffect got output %d", io);
        }

        Mutex::Autolock _l(mLock);

        // If output is not specified try to find a matching audio session ID in one of the
        // output threads.
        // If output is 0 here, sessionId is neither SESSION_OUTPUT_STAGE nor SESSION_OUTPUT_MIX
        // because of code checking output when entering the function.
        // Note: io is never 0 when creating an effect on an input
        if (io == AUDIO_IO_HANDLE_NONE) {
            if (sessionId == AUDIO_SESSION_OUTPUT_STAGE) {
                // output must be specified by AudioPolicyManager when using session
                // AUDIO_SESSION_OUTPUT_STAGE
                lStatus = BAD_VALUE;
                goto Exit;
            }
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
            if (io == AUDIO_IO_HANDLE_NONE && mPlaybackThreads.size() > 0) {
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
        } else {
            // Check if one effect chain was awaiting for an effect to be created on this
            // session and used it instead of creating a new one.
            sp<EffectChain> chain = getOrphanEffectChain_l((audio_session_t)sessionId);
            if (chain != 0) {
                Mutex::Autolock _l(thread->mLock);
                thread->addEffectChain_l(chain);
            }
        }

        sp<Client> client = registerPid(pid);

        // create effect on selected output thread
        handle = thread->createEffect_l(client, effectClient, priority, sessionId,
                &desc, enabled, &lStatus);
        if (handle != 0 && id != NULL) {
            *id = handle->id();
        }
        if (handle == 0) {
            // remove local strong reference to Client with mClientLock held
            Mutex::Autolock _cl(mClientLock);
            client.clear();
        }
    }

Exit:
    *status = lStatus;
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
#ifdef DOLBY_DAP_MOVE_EFFECT
    if (sessionId == DOLBY_MOVE_EFFECT_SIGNAL) {
        return EffectDapController::instance()->moveEffect(AUDIO_SESSION_OUTPUT_MIX, srcThread, dstThread);
    }
#endif // DOLBY_END
    return moveEffectChain_l(sessionId, srcThread, dstThread, false);
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

    // Check whether the destination thread has a channel count of FCC_2, which is
    // currently required for (most) effects. Prevent moving the effect chain here rather
    // than disabling the addEffect_l() call in dstThread below.
    if ((dstThread->type() == ThreadBase::MIXER || dstThread->type() == ThreadBase::DUPLICATING) &&
            dstThread->mChannelCount != FCC_2) {
        ALOGW("moveEffectChain_l() effect chain failed because"
                " destination thread %p channel count(%u) != %u",
                dstThread, dstThread->mChannelCount, FCC_2);
        return INVALID_OPERATION;
    }

    // remove chain first. This is useful only if reconfiguring effect chain on same output thread,
    // so that a new chain is created with correct parameters when first effect is added. This is
    // otherwise unnecessary as removeEffect_l() will remove the chain when last effect is
    // removed.
    srcThread->removeEffectChain_l(chain);

    // transfer all effects one by one so that new effect chain is created on new thread with
    // correct buffer sizes and audio parameters and effect engines reconfigured accordingly
    sp<EffectChain> dstChain;
    uint32_t strategy = 0; // prevent compiler warning
    sp<EffectModule> effect = chain->getEffectFromId_l(0);
    Vector< sp<EffectModule> > removed;
    status_t status = NO_ERROR;
    while (effect != 0) {
        srcThread->removeEffect_l(effect);
        removed.add(effect);
        status = dstThread->addEffect_l(effect);
        if (status != NO_ERROR) {
            break;
        }
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
                status = NO_INIT;
                break;
            }
            strategy = dstChain->strategy();
        }
        if (reRegister) {
            AudioSystem::unregisterEffect(effect->id());
            AudioSystem::registerEffect(&effect->desc(),
                                        dstThread->id(),
                                        strategy,
                                        sessionId,
                                        effect->id());
            AudioSystem::setEffectEnabled(effect->id(), effect->isEnabled());
        }
        effect = chain->getEffectFromId_l(0);
    }

    if (status != NO_ERROR) {
        for (size_t i = 0; i < removed.size(); i++) {
            srcThread->addEffect_l(removed[i]);
#ifdef DOLBY_DAP_MOVE_EFFECT
            // removeEffect_l() has stopped the effect if it was active so it must be restarted
            if (effect->state() == EffectModule::ACTIVE ||
                    effect->state() == EffectModule::STOPPING) {
                effect->start();
            }
#endif // DOLBY_END
            if (dstChain != 0 && reRegister) {
                AudioSystem::unregisterEffect(removed[i]->id());
                AudioSystem::registerEffect(&removed[i]->desc(),
                                            srcThread->id(),
                                            strategy,
                                            sessionId,
                                            removed[i]->id());
                AudioSystem::setEffectEnabled(effect->id(), effect->isEnabled());
            }
        }
    }

    return status;
}

bool AudioFlinger::isNonOffloadableGlobalEffectEnabled_l()
{
    if (mGlobalEffectEnableTime != 0 &&
            ((systemTime() - mGlobalEffectEnableTime) < kMinGlobalEffectEnabletimeNs)) {
        return true;
    }

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<EffectChain> ec =
                mPlaybackThreads.valueAt(i)->getEffectChain_l(AUDIO_SESSION_OUTPUT_MIX);
        if (ec != 0 && ec->isNonOffloadableEnabled()) {
            return true;
        }
    }
    return false;
}

void AudioFlinger::onNonOffloadableGlobalEffectEnable()
{
    Mutex::Autolock _l(mLock);

    mGlobalEffectEnableTime = systemTime();

    for (size_t i = 0; i < mPlaybackThreads.size(); i++) {
        sp<PlaybackThread> t = mPlaybackThreads.valueAt(i);
        if (t->mType == ThreadBase::OFFLOAD) {
            t->invalidateTracks(AUDIO_STREAM_MUSIC);
        }
    }

}

status_t AudioFlinger::putOrphanEffectChain_l(const sp<AudioFlinger::EffectChain>& chain)
{
    audio_session_t session = (audio_session_t)chain->sessionId();
    ssize_t index = mOrphanEffectChains.indexOfKey(session);
    ALOGV("putOrphanEffectChain_l session %d index %d", session, index);
    if (index >= 0) {
        ALOGW("putOrphanEffectChain_l chain for session %d already present", session);
        return ALREADY_EXISTS;
    }
    mOrphanEffectChains.add(session, chain);
    return NO_ERROR;
}

sp<AudioFlinger::EffectChain> AudioFlinger::getOrphanEffectChain_l(audio_session_t session)
{
    sp<EffectChain> chain;
    ssize_t index = mOrphanEffectChains.indexOfKey(session);
    ALOGV("getOrphanEffectChain_l session %d index %d", session, index);
    if (index >= 0) {
        chain = mOrphanEffectChains.valueAt(index);
        mOrphanEffectChains.removeItemsAt(index);
    }
    return chain;
}

bool AudioFlinger::updateOrphanEffectChains(const sp<AudioFlinger::EffectModule>& effect)
{
    Mutex::Autolock _l(mLock);
    audio_session_t session = (audio_session_t)effect->sessionId();
    ssize_t index = mOrphanEffectChains.indexOfKey(session);
    ALOGV("updateOrphanEffectChains session %d index %d", session, index);
    if (index >= 0) {
        sp<EffectChain> chain = mOrphanEffectChains.valueAt(index);
        if (chain->removeEffect_l(effect) == 0) {
            ALOGV("updateOrphanEffectChains removing effect chain at index %d", index);
            mOrphanEffectChains.removeItemsAt(index);
        }
        return true;
    }
    return false;
}


struct Entry {
#define MAX_NAME 32     // %Y%m%d%H%M%S_%d.wav
    char mName[MAX_NAME];
};

int comparEntry(const void *p1, const void *p2)
{
    return strcmp(((const Entry *) p1)->mName, ((const Entry *) p2)->mName);
}

#ifdef TEE_SINK
void AudioFlinger::dumpTee(int fd, const sp<NBAIO_Source>& source, audio_io_handle_t id)
{
    NBAIO_Source *teeSource = source.get();
    if (teeSource != NULL) {
        // .wav rotation
        // There is a benign race condition if 2 threads call this simultaneously.
        // They would both traverse the directory, but the result would simply be
        // failures at unlink() which are ignored.  It's also unlikely since
        // normally dumpsys is only done by bugreport or from the command line.
        char teePath[32+256];
        strcpy(teePath, "/data/misc/media");
        size_t teePathLen = strlen(teePath);
        DIR *dir = opendir(teePath);
        teePath[teePathLen++] = '/';
        if (dir != NULL) {
#define MAX_SORT 20 // number of entries to sort
#define MAX_KEEP 10 // number of entries to keep
            struct Entry entries[MAX_SORT];
            size_t entryCount = 0;
            while (entryCount < MAX_SORT) {
                struct dirent de;
                struct dirent *result = NULL;
                int rc = readdir_r(dir, &de, &result);
                if (rc != 0) {
                    ALOGW("readdir_r failed %d", rc);
                    break;
                }
                if (result == NULL) {
                    break;
                }
                if (result != &de) {
                    ALOGW("readdir_r returned unexpected result %p != %p", result, &de);
                    break;
                }
                // ignore non .wav file entries
                size_t nameLen = strlen(de.d_name);
                if (nameLen <= 4 || nameLen >= MAX_NAME ||
                        strcmp(&de.d_name[nameLen - 4], ".wav")) {
                    continue;
                }
                strcpy(entries[entryCount++].mName, de.d_name);
            }
            (void) closedir(dir);
            if (entryCount > MAX_KEEP) {
                qsort(entries, entryCount, sizeof(Entry), comparEntry);
                for (size_t i = 0; i < entryCount - MAX_KEEP; ++i) {
                    strcpy(&teePath[teePathLen], entries[i].mName);
                    (void) unlink(teePath);
                }
            }
        } else {
            if (fd >= 0) {
                dprintf(fd, "unable to rotate tees in %s: %s\n", teePath, strerror(errno));
            }
        }
        char teeTime[16];
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        strftime(teeTime, sizeof(teeTime), "%Y%m%d%H%M%S", &tm);
        snprintf(&teePath[teePathLen], sizeof(teePath) - teePathLen, "%s_%d.wav", teeTime, id);
        // if 2 dumpsys are done within 1 second, and rotation didn't work, then discard 2nd
        int teeFd = open(teePath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (teeFd >= 0) {
            // FIXME use libsndfile
            char wavHeader[44];
            memcpy(wavHeader,
                "RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\2\0\104\254\0\0\0\0\0\0\4\0\20\0data\0\0\0\0",
                sizeof(wavHeader));
            NBAIO_Format format = teeSource->format();
            unsigned channelCount = Format_channelCount(format);
            uint32_t sampleRate = Format_sampleRate(format);
            size_t frameSize = Format_frameSize(format);
            wavHeader[22] = channelCount;       // number of channels
            wavHeader[24] = sampleRate;         // sample rate
            wavHeader[25] = sampleRate >> 8;
            wavHeader[32] = frameSize;          // block alignment
            wavHeader[33] = frameSize >> 8;
            write(teeFd, wavHeader, sizeof(wavHeader));
            size_t total = 0;
            bool firstRead = true;
#define TEE_SINK_READ 1024                      // frames per I/O operation
            void *buffer = malloc(TEE_SINK_READ * frameSize);
            for (;;) {
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
                write(teeFd, buffer, actual * frameSize);
                total += actual;
            }
            free(buffer);
            lseek(teeFd, (off_t) 4, SEEK_SET);
            uint32_t temp = 44 + total * frameSize - 8;
            // FIXME not big-endian safe
            write(teeFd, &temp, sizeof(temp));
            lseek(teeFd, (off_t) 40, SEEK_SET);
            temp =  total * frameSize;
            // FIXME not big-endian safe
            write(teeFd, &temp, sizeof(temp));
            close(teeFd);
            if (fd >= 0) {
                dprintf(fd, "tee copied to %s\n", teePath);
            }
        } else {
            if (fd >= 0) {
                dprintf(fd, "unable to create tee %s: %s\n", teePath, strerror(errno));
            }
        }
    }
}
#endif

status_t AudioFlinger::onTransact(
        uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioFlinger::onTransact(code, data, reply, flags);
}

}; // namespace android
