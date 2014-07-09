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

#ifndef ANDROID_AUDIO_FLINGER_H
#define ANDROID_AUDIO_FLINGER_H

#include "Configuration.h"
#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#include <common_time/cc_helper.h>

#include <cutils/compiler.h>

#include <media/IAudioFlinger.h>
#include <media/IAudioFlingerClient.h>
#include <media/IAudioTrack.h>
#include <media/IAudioRecord.h>
#include <media/AudioSystem.h>
#include <media/AudioTrack.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/threads.h>
#include <utils/SortedVector.h>
#include <utils/TypeHelpers.h>
#include <utils/Vector.h>

#include <binder/BinderService.h>
#include <binder/MemoryDealer.h>

#include <system/audio.h>
#include <hardware/audio.h>
#include <hardware/audio_policy.h>

#include <media/AudioBufferProvider.h>
#include <media/ExtendedAudioBufferProvider.h>

#include "FastCapture.h"
#include "FastMixer.h"
#include <media/nbaio/NBAIO.h>
#include "AudioWatchdog.h"

#include <powermanager/IPowerManager.h>

#include <media/nbaio/NBLog.h>
#include <private/media/AudioTrackShared.h>

namespace android {

struct audio_track_cblk_t;
struct effect_param_cblk_t;
class AudioMixer;
class AudioBuffer;
class AudioResampler;
class FastMixer;
class ServerProxy;

// ----------------------------------------------------------------------------

// AudioFlinger has a hard-coded upper limit of 2 channels for capture and playback.
// There is support for > 2 channel tracks down-mixed to 2 channel output via a down-mix effect.
// Adding full support for > 2 channel capture or playback would require more than simply changing
// this #define.  There is an independent hard-coded upper limit in AudioMixer;
// removing that AudioMixer limit would be necessary but insufficient to support > 2 channels.
// The macro FCC_2 highlights some (but not all) places where there is are 2-channel assumptions.
// Search also for "2", "left", "right", "[0]", "[1]", ">> 16", "<< 16", etc.
#define FCC_2 2     // FCC_2 = Fixed Channel Count 2

static const nsecs_t kDefaultStandbyTimeInNsecs = seconds(3);

#define INCLUDING_FROM_AUDIOFLINGER_H

class AudioFlinger :
    public BinderService<AudioFlinger>,
    public BnAudioFlinger
{
    friend class BinderService<AudioFlinger>;   // for AudioFlinger()
public:
    static const char* getServiceName() ANDROID_API { return "media.audio_flinger"; }

    virtual     status_t    dump(int fd, const Vector<String16>& args);

    // IAudioFlinger interface, in binder opcode order
    virtual sp<IAudioTrack> createTrack(
                                audio_stream_type_t streamType,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t *pFrameCount,
                                IAudioFlinger::track_flags_t *flags,
                                const sp<IMemory>& sharedBuffer,
                                audio_io_handle_t output,
                                pid_t tid,
                                int *sessionId,
                                int clientUid,
                                status_t *status /*non-NULL*/);

    virtual sp<IAudioRecord> openRecord(
                                audio_io_handle_t input,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t *pFrameCount,
                                IAudioFlinger::track_flags_t *flags,
                                pid_t tid,
                                int *sessionId,
                                size_t *notificationFrames,
                                sp<IMemory>& cblk,
                                sp<IMemory>& buffers,
                                status_t *status /*non-NULL*/);

    virtual     uint32_t    sampleRate(audio_io_handle_t output) const;
    virtual     audio_format_t format(audio_io_handle_t output) const;
    virtual     size_t      frameCount(audio_io_handle_t output) const;
    virtual     uint32_t    latency(audio_io_handle_t output) const;

    virtual     status_t    setMasterVolume(float value);
    virtual     status_t    setMasterMute(bool muted);

    virtual     float       masterVolume() const;
    virtual     bool        masterMute() const;

    virtual     status_t    setStreamVolume(audio_stream_type_t stream, float value,
                                            audio_io_handle_t output);
    virtual     status_t    setStreamMute(audio_stream_type_t stream, bool muted);

    virtual     float       streamVolume(audio_stream_type_t stream,
                                         audio_io_handle_t output) const;
    virtual     bool        streamMute(audio_stream_type_t stream) const;

    virtual     status_t    setMode(audio_mode_t mode);

    virtual     status_t    setMicMute(bool state);
    virtual     bool        getMicMute() const;

    virtual     status_t    setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs);
    virtual     String8     getParameters(audio_io_handle_t ioHandle, const String8& keys) const;

    virtual     void        registerClient(const sp<IAudioFlingerClient>& client);

    virtual     size_t      getInputBufferSize(uint32_t sampleRate, audio_format_t format,
                                               audio_channel_mask_t channelMask) const;

    virtual audio_io_handle_t openOutput(audio_module_handle_t module,
                                         audio_devices_t *pDevices,
                                         uint32_t *pSamplingRate,
                                         audio_format_t *pFormat,
                                         audio_channel_mask_t *pChannelMask,
                                         uint32_t *pLatencyMs,
                                         audio_output_flags_t flags,
                                         const audio_offload_info_t *offloadInfo);

    virtual audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1,
                                                  audio_io_handle_t output2);

    virtual status_t closeOutput(audio_io_handle_t output);

    virtual status_t suspendOutput(audio_io_handle_t output);

    virtual status_t restoreOutput(audio_io_handle_t output);

    virtual audio_io_handle_t openInput(audio_module_handle_t module,
                                        audio_devices_t *pDevices,
                                        uint32_t *pSamplingRate,
                                        audio_format_t *pFormat,
                                        audio_channel_mask_t *pChannelMask);

    virtual status_t closeInput(audio_io_handle_t input);

    virtual status_t invalidateStream(audio_stream_type_t stream);

    virtual status_t setVoiceVolume(float volume);

    virtual status_t getRenderPosition(uint32_t *halFrames, uint32_t *dspFrames,
                                       audio_io_handle_t output) const;

    virtual uint32_t getInputFramesLost(audio_io_handle_t ioHandle) const;

    virtual int newAudioSessionId();

    virtual void acquireAudioSessionId(int audioSession, pid_t pid);

    virtual void releaseAudioSessionId(int audioSession, pid_t pid);

    virtual status_t queryNumberEffects(uint32_t *numEffects) const;

    virtual status_t queryEffect(uint32_t index, effect_descriptor_t *descriptor) const;

    virtual status_t getEffectDescriptor(const effect_uuid_t *pUuid,
                                         effect_descriptor_t *descriptor) const;

    virtual sp<IEffect> createEffect(
                        effect_descriptor_t *pDesc,
                        const sp<IEffectClient>& effectClient,
                        int32_t priority,
                        audio_io_handle_t io,
                        int sessionId,
                        status_t *status /*non-NULL*/,
                        int *id,
                        int *enabled);

    virtual status_t moveEffects(int sessionId, audio_io_handle_t srcOutput,
                        audio_io_handle_t dstOutput);

    virtual audio_module_handle_t loadHwModule(const char *name);

    virtual uint32_t getPrimaryOutputSamplingRate();
    virtual size_t getPrimaryOutputFrameCount();

    virtual status_t setLowRamDevice(bool isLowRamDevice);

    /* List available audio ports and their attributes */
    virtual status_t listAudioPorts(unsigned int *num_ports,
                                    struct audio_port *ports);

    /* Get attributes for a given audio port */
    virtual status_t getAudioPort(struct audio_port *port);

    /* Create an audio patch between several source and sink ports */
    virtual status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle);

    /* Release an audio patch */
    virtual status_t releaseAudioPatch(audio_patch_handle_t handle);

    /* List existing audio patches */
    virtual status_t listAudioPatches(unsigned int *num_patches,
                                      struct audio_patch *patches);

    /* Set audio port configuration */
    virtual status_t setAudioPortConfig(const struct audio_port_config *config);

    virtual     status_t    onTransact(
                                uint32_t code,
                                const Parcel& data,
                                Parcel* reply,
                                uint32_t flags);

    // end of IAudioFlinger interface

    sp<NBLog::Writer>   newWriter_l(size_t size, const char *name);
    void                unregisterWriter(const sp<NBLog::Writer>& writer);
private:
    static const size_t kLogMemorySize = 40 * 1024;
    sp<MemoryDealer>    mLogMemoryDealer;   // == 0 when NBLog is disabled
    // When a log writer is unregistered, it is done lazily so that media.log can continue to see it
    // for as long as possible.  The memory is only freed when it is needed for another log writer.
    Vector< sp<NBLog::Writer> > mUnregisteredWriters;
    Mutex               mUnregisteredWritersLock;
public:

    class SyncEvent;

    typedef void (*sync_event_callback_t)(const wp<SyncEvent>& event) ;

    class SyncEvent : public RefBase {
    public:
        SyncEvent(AudioSystem::sync_event_t type,
                  int triggerSession,
                  int listenerSession,
                  sync_event_callback_t callBack,
                  wp<RefBase> cookie)
        : mType(type), mTriggerSession(triggerSession), mListenerSession(listenerSession),
          mCallback(callBack), mCookie(cookie)
        {}

        virtual ~SyncEvent() {}

        void trigger() { Mutex::Autolock _l(mLock); if (mCallback) mCallback(this); }
        bool isCancelled() const { Mutex::Autolock _l(mLock); return (mCallback == NULL); }
        void cancel() { Mutex::Autolock _l(mLock); mCallback = NULL; }
        AudioSystem::sync_event_t type() const { return mType; }
        int triggerSession() const { return mTriggerSession; }
        int listenerSession() const { return mListenerSession; }
        wp<RefBase> cookie() const { return mCookie; }

    private:
          const AudioSystem::sync_event_t mType;
          const int mTriggerSession;
          const int mListenerSession;
          sync_event_callback_t mCallback;
          const wp<RefBase> mCookie;
          mutable Mutex mLock;
    };

    sp<SyncEvent> createSyncEvent(AudioSystem::sync_event_t type,
                                        int triggerSession,
                                        int listenerSession,
                                        sync_event_callback_t callBack,
                                        wp<RefBase> cookie);

private:
    class AudioHwDevice;    // fwd declaration for findSuitableHwDev_l

               audio_mode_t getMode() const { return mMode; }

                bool        btNrecIsOff() const { return mBtNrecIsOff; }

                            AudioFlinger() ANDROID_API;
    virtual                 ~AudioFlinger();

    // call in any IAudioFlinger method that accesses mPrimaryHardwareDev
    status_t                initCheck() const { return mPrimaryHardwareDev == NULL ?
                                                        NO_INIT : NO_ERROR; }

    // RefBase
    virtual     void        onFirstRef();

    AudioHwDevice*          findSuitableHwDev_l(audio_module_handle_t module,
                                                audio_devices_t devices);
    void                    purgeStaleEffects_l();

    // Set kEnableExtendedPrecision to true to use extended precision in MixerThread
    static const bool kEnableExtendedPrecision = true;

    // Returns true if format is permitted for the PCM sink in the MixerThread
    static inline bool isValidPcmSinkFormat(audio_format_t format) {
        switch (format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            return true;
        case AUDIO_FORMAT_PCM_FLOAT:
        case AUDIO_FORMAT_PCM_24_BIT_PACKED:
        case AUDIO_FORMAT_PCM_32_BIT:
        case AUDIO_FORMAT_PCM_8_24_BIT:
            return kEnableExtendedPrecision;
        default:
            return false;
        }
    }

    // standby delay for MIXER and DUPLICATING playback threads is read from property
    // ro.audio.flinger_standbytime_ms or defaults to kDefaultStandbyTimeInNsecs
    static nsecs_t          mStandbyTimeInNsecs;

    // incremented by 2 when screen state changes, bit 0 == 1 means "off"
    // AudioFlinger::setParameters() updates, other threads read w/o lock
    static uint32_t         mScreenState;

    // Internal dump utilities.
    static const int kDumpLockRetries = 50;
    static const int kDumpLockSleepUs = 20000;
    static bool dumpTryLock(Mutex& mutex);
    void dumpPermissionDenial(int fd, const Vector<String16>& args);
    void dumpClients(int fd, const Vector<String16>& args);
    void dumpInternals(int fd, const Vector<String16>& args);

    // --- Client ---
    class Client : public RefBase {
    public:
                            Client(const sp<AudioFlinger>& audioFlinger, pid_t pid);
        virtual             ~Client();
        sp<MemoryDealer>    heap() const;
        pid_t               pid() const { return mPid; }
        sp<AudioFlinger>    audioFlinger() const { return mAudioFlinger; }

        bool reserveTimedTrack();
        void releaseTimedTrack();

    private:
                            Client(const Client&);
                            Client& operator = (const Client&);
        const sp<AudioFlinger> mAudioFlinger;
        const sp<MemoryDealer> mMemoryDealer;
        const pid_t         mPid;

        Mutex               mTimedTrackLock;
        int                 mTimedTrackCount;
    };

    // --- Notification Client ---
    class NotificationClient : public IBinder::DeathRecipient {
    public:
                            NotificationClient(const sp<AudioFlinger>& audioFlinger,
                                                const sp<IAudioFlingerClient>& client,
                                                pid_t pid);
        virtual             ~NotificationClient();

                sp<IAudioFlingerClient> audioFlingerClient() const { return mAudioFlingerClient; }

                // IBinder::DeathRecipient
                virtual     void        binderDied(const wp<IBinder>& who);

    private:
                            NotificationClient(const NotificationClient&);
                            NotificationClient& operator = (const NotificationClient&);

        const sp<AudioFlinger>  mAudioFlinger;
        const pid_t             mPid;
        const sp<IAudioFlingerClient> mAudioFlingerClient;
    };

    class TrackHandle;
    class RecordHandle;
    class RecordThread;
    class PlaybackThread;
    class MixerThread;
    class DirectOutputThread;
    class OffloadThread;
    class DuplicatingThread;
    class AsyncCallbackThread;
    class Track;
    class RecordTrack;
    class EffectModule;
    class EffectHandle;
    class EffectChain;
    struct AudioStreamOut;
    struct AudioStreamIn;

    struct  stream_type_t {
        stream_type_t()
            :   volume(1.0f),
                mute(false)
        {
        }
        float       volume;
        bool        mute;
    };

    // --- PlaybackThread ---

#include "Threads.h"

#include "Effects.h"

#include "PatchPanel.h"

    // server side of the client's IAudioTrack
    class TrackHandle : public android::BnAudioTrack {
    public:
                            TrackHandle(const sp<PlaybackThread::Track>& track);
        virtual             ~TrackHandle();
        virtual sp<IMemory> getCblk() const;
        virtual status_t    start();
        virtual void        stop();
        virtual void        flush();
        virtual void        pause();
        virtual status_t    attachAuxEffect(int effectId);
        virtual status_t    allocateTimedBuffer(size_t size,
                                                sp<IMemory>* buffer);
        virtual status_t    queueTimedBuffer(const sp<IMemory>& buffer,
                                             int64_t pts);
        virtual status_t    setMediaTimeTransform(const LinearTransform& xform,
                                                  int target);
        virtual status_t    setParameters(const String8& keyValuePairs);
        virtual status_t    getTimestamp(AudioTimestamp& timestamp);
        virtual void        signal(); // signal playback thread for a change in control block

        virtual status_t onTransact(
            uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags);

    private:
        const sp<PlaybackThread::Track> mTrack;
    };

    // server side of the client's IAudioRecord
    class RecordHandle : public android::BnAudioRecord {
    public:
        RecordHandle(const sp<RecordThread::RecordTrack>& recordTrack);
        virtual             ~RecordHandle();
        virtual status_t    start(int /*AudioSystem::sync_event_t*/ event, int triggerSession);
        virtual void        stop();
        virtual status_t onTransact(
            uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags);
    private:
        const sp<RecordThread::RecordTrack> mRecordTrack;

        // for use from destructor
        void                stop_nonvirtual();
    };


              PlaybackThread *checkPlaybackThread_l(audio_io_handle_t output) const;
              MixerThread *checkMixerThread_l(audio_io_handle_t output) const;
              RecordThread *checkRecordThread_l(audio_io_handle_t input) const;
              // no range check, AudioFlinger::mLock held
              bool streamMute_l(audio_stream_type_t stream) const
                                { return mStreamTypes[stream].mute; }
              // no range check, doesn't check per-thread stream volume, AudioFlinger::mLock held
              float streamVolume_l(audio_stream_type_t stream) const
                                { return mStreamTypes[stream].volume; }
              void audioConfigChanged(int event, audio_io_handle_t ioHandle, const void *param2);

              // Allocate an audio_io_handle_t, session ID, effect ID, or audio_module_handle_t.
              // They all share the same ID space, but the namespaces are actually independent
              // because there are separate KeyedVectors for each kind of ID.
              // The return value is uint32_t, but is cast to signed for some IDs.
              // FIXME This API does not handle rollover to zero (for unsigned IDs),
              //       or from positive to negative (for signed IDs).
              //       Thus it may fail by returning an ID of the wrong sign,
              //       or by returning a non-unique ID.
              uint32_t nextUniqueId();

              status_t moveEffectChain_l(int sessionId,
                                     PlaybackThread *srcThread,
                                     PlaybackThread *dstThread,
                                     bool reRegister);
              // return thread associated with primary hardware device, or NULL
              PlaybackThread *primaryPlaybackThread_l() const;
              audio_devices_t primaryOutputDevice_l() const;

              sp<PlaybackThread> getEffectThread_l(int sessionId, int EffectId);


                void        removeClient_l(pid_t pid);
                void        removeNotificationClient(pid_t pid);
                bool isNonOffloadableGlobalEffectEnabled_l();
                void onNonOffloadableGlobalEffectEnable();

    class AudioHwDevice {
    public:
        enum Flags {
            AHWD_CAN_SET_MASTER_VOLUME  = 0x1,
            AHWD_CAN_SET_MASTER_MUTE    = 0x2,
        };

        AudioHwDevice(const char *moduleName,
                      audio_hw_device_t *hwDevice,
                      Flags flags)
            : mModuleName(strdup(moduleName))
            , mHwDevice(hwDevice)
            , mFlags(flags) { }
        /*virtual*/ ~AudioHwDevice() { free((void *)mModuleName); }

        bool canSetMasterVolume() const {
            return (0 != (mFlags & AHWD_CAN_SET_MASTER_VOLUME));
        }

        bool canSetMasterMute() const {
            return (0 != (mFlags & AHWD_CAN_SET_MASTER_MUTE));
        }

        const char *moduleName() const { return mModuleName; }
        audio_hw_device_t *hwDevice() const { return mHwDevice; }
        uint32_t version() const { return mHwDevice->common.version; }

    private:
        const char * const mModuleName;
        audio_hw_device_t * const mHwDevice;
        const Flags mFlags;
    };

    // AudioStreamOut and AudioStreamIn are immutable, so their fields are const.
    // For emphasis, we could also make all pointers to them be "const *",
    // but that would clutter the code unnecessarily.

    struct AudioStreamOut {
        AudioHwDevice* const audioHwDev;
        audio_stream_out_t* const stream;
        const audio_output_flags_t flags;

        audio_hw_device_t* hwDev() const { return audioHwDev->hwDevice(); }

        AudioStreamOut(AudioHwDevice *dev, audio_stream_out_t *out, audio_output_flags_t flags) :
            audioHwDev(dev), stream(out), flags(flags) {}
    };

    struct AudioStreamIn {
        AudioHwDevice* const audioHwDev;
        audio_stream_in_t* const stream;

        audio_hw_device_t* hwDev() const { return audioHwDev->hwDevice(); }

        AudioStreamIn(AudioHwDevice *dev, audio_stream_in_t *in) :
            audioHwDev(dev), stream(in) {}
    };

    // for mAudioSessionRefs only
    struct AudioSessionRef {
        AudioSessionRef(int sessionid, pid_t pid) :
            mSessionid(sessionid), mPid(pid), mCnt(1) {}
        const int   mSessionid;
        const pid_t mPid;
        int         mCnt;
    };

    mutable     Mutex                               mLock;
                // protects mClients and mNotificationClients.
                // must be locked after mLock and ThreadBase::mLock if both must be locked
                // avoids acquiring AudioFlinger::mLock from inside thread loop.
    mutable     Mutex                               mClientLock;
                // protected by mClientLock
                DefaultKeyedVector< pid_t, wp<Client> >     mClients;   // see ~Client()

                mutable     Mutex                   mHardwareLock;
                // NOTE: If both mLock and mHardwareLock mutexes must be held,
                // always take mLock before mHardwareLock

                // These two fields are immutable after onFirstRef(), so no lock needed to access
                AudioHwDevice*                      mPrimaryHardwareDev; // mAudioHwDevs[0] or NULL
                DefaultKeyedVector<audio_module_handle_t, AudioHwDevice*>  mAudioHwDevs;

    // for dump, indicates which hardware operation is currently in progress (but not stream ops)
    enum hardware_call_state {
        AUDIO_HW_IDLE = 0,              // no operation in progress
        AUDIO_HW_INIT,                  // init_check
        AUDIO_HW_OUTPUT_OPEN,           // open_output_stream
        AUDIO_HW_OUTPUT_CLOSE,          // unused
        AUDIO_HW_INPUT_OPEN,            // unused
        AUDIO_HW_INPUT_CLOSE,           // unused
        AUDIO_HW_STANDBY,               // unused
        AUDIO_HW_SET_MASTER_VOLUME,     // set_master_volume
        AUDIO_HW_GET_ROUTING,           // unused
        AUDIO_HW_SET_ROUTING,           // unused
        AUDIO_HW_GET_MODE,              // unused
        AUDIO_HW_SET_MODE,              // set_mode
        AUDIO_HW_GET_MIC_MUTE,          // get_mic_mute
        AUDIO_HW_SET_MIC_MUTE,          // set_mic_mute
        AUDIO_HW_SET_VOICE_VOLUME,      // set_voice_volume
        AUDIO_HW_SET_PARAMETER,         // set_parameters
        AUDIO_HW_GET_INPUT_BUFFER_SIZE, // get_input_buffer_size
        AUDIO_HW_GET_MASTER_VOLUME,     // get_master_volume
        AUDIO_HW_GET_PARAMETER,         // get_parameters
        AUDIO_HW_SET_MASTER_MUTE,       // set_master_mute
        AUDIO_HW_GET_MASTER_MUTE,       // get_master_mute
    };

    mutable     hardware_call_state                 mHardwareStatus;    // for dump only


                DefaultKeyedVector< audio_io_handle_t, sp<PlaybackThread> >  mPlaybackThreads;
                stream_type_t                       mStreamTypes[AUDIO_STREAM_CNT];

                // member variables below are protected by mLock
                float                               mMasterVolume;
                bool                                mMasterMute;
                // end of variables protected by mLock

                DefaultKeyedVector< audio_io_handle_t, sp<RecordThread> >    mRecordThreads;

                // protected by mClientLock
                DefaultKeyedVector< pid_t, sp<NotificationClient> >    mNotificationClients;

                volatile int32_t                    mNextUniqueId;  // updated by android_atomic_inc
                // nextUniqueId() returns uint32_t, but this is declared int32_t
                // because the atomic operations require an int32_t

                audio_mode_t                        mMode;
                bool                                mBtNrecIsOff;

                // protected by mLock
                Vector<AudioSessionRef*> mAudioSessionRefs;

                float       masterVolume_l() const;
                bool        masterMute_l() const;
                audio_module_handle_t loadHwModule_l(const char *name);

                Vector < sp<SyncEvent> > mPendingSyncEvents; // sync events awaiting for a session
                                                             // to be created

private:
    sp<Client>  registerPid(pid_t pid);    // always returns non-0

    // for use from destructor
    status_t    closeOutput_nonvirtual(audio_io_handle_t output);
    status_t    closeInput_nonvirtual(audio_io_handle_t input);

#ifdef TEE_SINK
    // all record threads serially share a common tee sink, which is re-created on format change
    sp<NBAIO_Sink>   mRecordTeeSink;
    sp<NBAIO_Source> mRecordTeeSource;
#endif

public:

#ifdef TEE_SINK
    // tee sink, if enabled by property, allows dumpsys to write most recent audio to .wav file
    static void dumpTee(int fd, const sp<NBAIO_Source>& source, audio_io_handle_t id = 0);

    // whether tee sink is enabled by property
    static bool mTeeSinkInputEnabled;
    static bool mTeeSinkOutputEnabled;
    static bool mTeeSinkTrackEnabled;

    // runtime configured size of each tee sink pipe, in frames
    static size_t mTeeSinkInputFrames;
    static size_t mTeeSinkOutputFrames;
    static size_t mTeeSinkTrackFrames;

    // compile-time default size of tee sink pipes, in frames
    // 0x200000 stereo 16-bit PCM frames = 47.5 seconds at 44.1 kHz, 8 megabytes
    static const size_t kTeeSinkInputFramesDefault = 0x200000;
    static const size_t kTeeSinkOutputFramesDefault = 0x200000;
    static const size_t kTeeSinkTrackFramesDefault = 0x200000;
#endif

    // This method reads from a variable without mLock, but the variable is updated under mLock.  So
    // we might read a stale value, or a value that's inconsistent with respect to other variables.
    // In this case, it's safe because the return value isn't used for making an important decision.
    // The reason we don't want to take mLock is because it could block the caller for a long time.
    bool    isLowRamDevice() const { return mIsLowRamDevice; }

private:
    bool    mIsLowRamDevice;
    bool    mIsDeviceTypeKnown;
    nsecs_t mGlobalEffectEnableTime;  // when a global effect was last enabled

    sp<PatchPanel> mPatchPanel;

    uint32_t    mPrimaryOutputSampleRate;   // sample rate of the primary output, or zero if none
                                            // protected by mHardwareLock
};

#undef INCLUDING_FROM_AUDIOFLINGER_H

const char *formatToString(audio_format_t format);

// ----------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_AUDIO_FLINGER_H
