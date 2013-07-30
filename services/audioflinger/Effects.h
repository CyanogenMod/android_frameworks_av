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

#ifndef INCLUDING_FROM_AUDIOFLINGER_H
    #error This header file should only be included from AudioFlinger.h
#endif

//--- Audio Effect Management

// EffectModule and EffectChain classes both have their own mutex to protect
// state changes or resource modifications. Always respect the following order
// if multiple mutexes must be acquired to avoid cross deadlock:
// AudioFlinger -> ThreadBase -> EffectChain -> EffectModule

// The EffectModule class is a wrapper object controlling the effect engine implementation
// in the effect library. It prevents concurrent calls to process() and command() functions
// from different client threads. It keeps a list of EffectHandle objects corresponding
// to all client applications using this effect and notifies applications of effect state,
// control or parameter changes. It manages the activation state machine to send appropriate
// reset, enable, disable commands to effect engine and provide volume
// ramping when effects are activated/deactivated.
// When controlling an auxiliary effect, the EffectModule also provides an input buffer used by
// the attached track(s) to accumulate their auxiliary channel.
class EffectModule : public RefBase {
public:
    EffectModule(ThreadBase *thread,
                    const wp<AudioFlinger::EffectChain>& chain,
                    effect_descriptor_t *desc,
                    int id,
                    int sessionId);
    virtual ~EffectModule();

    enum effect_state {
        IDLE,
        RESTART,
        STARTING,
        ACTIVE,
        STOPPING,
        STOPPED,
        DESTROYED
    };

    int         id() const { return mId; }
    void process();
    void updateState();
    status_t command(uint32_t cmdCode,
                     uint32_t cmdSize,
                     void *pCmdData,
                     uint32_t *replySize,
                     void *pReplyData);

    void reset_l();
#ifdef QCOM_HARDWARE
    status_t configure(bool isForLPA = false,
                       int sampleRate = 0,
                       int channelCount = 0,
                       int frameCount = 0);
#else
    status_t configure();
#endif
    status_t init();
    effect_state state() const {
        return mState;
    }
    uint32_t status() {
        return mStatus;
    }
    int sessionId() const {
        return mSessionId;
    }
    status_t    setEnabled(bool enabled);
    status_t    setEnabled_l(bool enabled);
    bool isEnabled() const;
    bool isProcessEnabled() const;

    void        setInBuffer(int16_t *buffer) { mConfig.inputCfg.buffer.s16 = buffer; }
    int16_t     *inBuffer() { return mConfig.inputCfg.buffer.s16; }
    void        setOutBuffer(int16_t *buffer) { mConfig.outputCfg.buffer.s16 = buffer; }
    int16_t     *outBuffer() { return mConfig.outputCfg.buffer.s16; }
    void        setChain(const wp<EffectChain>& chain) { mChain = chain; }
    void        setThread(const wp<ThreadBase>& thread) { mThread = thread; }
    const wp<ThreadBase>& thread() { return mThread; }

    status_t addHandle(EffectHandle *handle);
    size_t disconnect(EffectHandle *handle, bool unpinIfLast);
    size_t removeHandle(EffectHandle *handle);

    const effect_descriptor_t& desc() const { return mDescriptor; }
    wp<EffectChain>&     chain() { return mChain; }

    status_t         setDevice(audio_devices_t device);
    status_t         setVolume(uint32_t *left, uint32_t *right, bool controller);
    status_t         setMode(audio_mode_t mode);
    status_t         setAudioSource(audio_source_t source);
    status_t         start();
    status_t         stop();
    void             setSuspended(bool suspended);
    bool             suspended() const;

    EffectHandle*    controlHandle_l();

    bool             isPinned() const { return mPinned; }
    void             unPin() { mPinned = false; }
    bool             purgeHandles();
    void             lock() { mLock.lock(); }
    void             unlock() { mLock.unlock(); }
#ifdef QCOM_HARDWARE
    bool             isOnLPA() { return mIsForLPA;}
    void             setLPAFlag(bool isForLPA) {mIsForLPA = isForLPA; }
#endif
    void             dump(int fd, const Vector<String16>& args);

protected:
    friend class AudioFlinger;      // for mHandles
    bool                mPinned;

    // Maximum time allocated to effect engines to complete the turn off sequence
    static const uint32_t MAX_DISABLE_TIME_MS = 10000;

    EffectModule(const EffectModule&);
    EffectModule& operator = (const EffectModule&);

    status_t start_l();
    status_t stop_l();

mutable Mutex               mLock;      // mutex for process, commands and handles list protection
    wp<ThreadBase>      mThread;    // parent thread
    wp<EffectChain>     mChain;     // parent effect chain
    const int           mId;        // this instance unique ID
    const int           mSessionId; // audio session ID
    const effect_descriptor_t mDescriptor;// effect descriptor received from effect engine
    effect_config_t     mConfig;    // input and output audio configuration
    effect_handle_t  mEffectInterface; // Effect module C API
    status_t            mStatus;    // initialization status
    effect_state        mState;     // current activation state
    Vector<EffectHandle *> mHandles;    // list of client handles
                // First handle in mHandles has highest priority and controls the effect module
    uint32_t mMaxDisableWaitCnt;    // maximum grace period before forcing an effect off after
                                    // sending disable command.
    uint32_t mDisableWaitCnt;       // current process() calls count during disable period.
    bool     mSuspended;            // effect is suspended: temporarily disabled by framework
#ifdef QCOM_HARDWARE
    bool     mIsForLPA;
#endif
};

// The EffectHandle class implements the IEffect interface. It provides resources
// to receive parameter updates, keeps track of effect control
// ownership and state and has a pointer to the EffectModule object it is controlling.
// There is one EffectHandle object for each application controlling (or using)
// an effect module.
// The EffectHandle is obtained by calling AudioFlinger::createEffect().
class EffectHandle: public android::BnEffect {
public:

    EffectHandle(const sp<EffectModule>& effect,
            const sp<AudioFlinger::Client>& client,
            const sp<IEffectClient>& effectClient,
            int32_t priority);
    virtual ~EffectHandle();

    // IEffect
    virtual status_t enable();
    virtual status_t disable();
    virtual status_t command(uint32_t cmdCode,
                             uint32_t cmdSize,
                             void *pCmdData,
                             uint32_t *replySize,
                             void *pReplyData);
    virtual void disconnect();
private:
            void disconnect(bool unpinIfLast);
public:
    virtual sp<IMemory> getCblk() const { return mCblkMemory; }
    virtual status_t onTransact(uint32_t code, const Parcel& data,
            Parcel* reply, uint32_t flags);


    // Give or take control of effect module
    // - hasControl: true if control is given, false if removed
    // - signal: true client app should be signaled of change, false otherwise
    // - enabled: state of the effect when control is passed
    void setControl(bool hasControl, bool signal, bool enabled);
    void commandExecuted(uint32_t cmdCode,
                         uint32_t cmdSize,
                         void *pCmdData,
                         uint32_t replySize,
                         void *pReplyData);
    void setEnabled(bool enabled);
    bool enabled() const { return mEnabled; }

    // Getters
    int id() const { return mEffect->id(); }
    int priority() const { return mPriority; }
    bool hasControl() const { return mHasControl; }
    sp<EffectModule> effect() const { return mEffect; }
    // destroyed_l() must be called with the associated EffectModule mLock held
    bool destroyed_l() const { return mDestroyed; }

    void dump(char* buffer, size_t size);

protected:
    friend class AudioFlinger;          // for mEffect, mHasControl, mEnabled
    EffectHandle(const EffectHandle&);
    EffectHandle& operator =(const EffectHandle&);

    sp<EffectModule> mEffect;           // pointer to controlled EffectModule
    sp<IEffectClient> mEffectClient;    // callback interface for client notifications
    /*const*/ sp<Client> mClient;       // client for shared memory allocation, see disconnect()
    sp<IMemory>         mCblkMemory;    // shared memory for control block
    effect_param_cblk_t* mCblk;         // control block for deferred parameter setting via
                                        // shared memory
    uint8_t*            mBuffer;        // pointer to parameter area in shared memory
    int mPriority;                      // client application priority to control the effect
    bool mHasControl;                   // true if this handle is controlling the effect
    bool mEnabled;                      // cached enable state: needed when the effect is
                                        // restored after being suspended
    bool mDestroyed;                    // Set to true by destructor. Access with EffectModule
                                        // mLock held
};

// the EffectChain class represents a group of effects associated to one audio session.
// There can be any number of EffectChain objects per output mixer thread (PlaybackThread).
// The EffecChain with session ID 0 contains global effects applied to the output mix.
// Effects in this chain can be insert or auxiliary. Effects in other chains (attached to
// tracks) are insert only. The EffectChain maintains an ordered list of effect module, the
// order corresponding in the effect process order. When attached to a track (session ID != 0),
// it also provide it's own input buffer used by the track as accumulation buffer.
class EffectChain : public RefBase {
public:
    EffectChain(const wp<ThreadBase>& wThread, int sessionId);
    EffectChain(ThreadBase *thread, int sessionId);
    virtual ~EffectChain();

    // special key used for an entry in mSuspendedEffects keyed vector
    // corresponding to a suspend all request.
    static const int        kKeyForSuspendAll = 0;

    // minimum duration during which we force calling effect process when last track on
    // a session is stopped or removed to allow effect tail to be rendered
    static const int        kProcessTailDurationMs = 1000;

    void process_l();

    void lock() {
        mLock.lock();
    }
    void unlock() {
        mLock.unlock();
    }

    status_t addEffect_l(const sp<EffectModule>& handle);
    size_t removeEffect_l(const sp<EffectModule>& handle);
#ifdef QCOM_HARDWARE
    size_t getNumEffects() { return mEffects.size(); }
#endif

    int sessionId() const { return mSessionId; }
    void setSessionId(int sessionId) { mSessionId = sessionId; }

    sp<EffectModule> getEffectFromDesc_l(effect_descriptor_t *descriptor);
    sp<EffectModule> getEffectFromId_l(int id);
#ifdef QCOM_HARDWARE
    sp<EffectModule> getEffectFromIndex_l(int idx);
#endif
    sp<EffectModule> getEffectFromType_l(const effect_uuid_t *type);
    bool setVolume_l(uint32_t *left, uint32_t *right);
    void setDevice_l(audio_devices_t device);
    void setMode_l(audio_mode_t mode);
    void setAudioSource_l(audio_source_t source);

    void setInBuffer(int16_t *buffer, bool ownsBuffer = false) {
        mInBuffer = buffer;
        mOwnInBuffer = ownsBuffer;
    }
    int16_t *inBuffer() const {
        return mInBuffer;
    }
    void setOutBuffer(int16_t *buffer) {
        mOutBuffer = buffer;
    }
    int16_t *outBuffer() const {
        return mOutBuffer;
    }

    void incTrackCnt() { android_atomic_inc(&mTrackCnt); }
    void decTrackCnt() { android_atomic_dec(&mTrackCnt); }
    int32_t trackCnt() const { return android_atomic_acquire_load(&mTrackCnt); }

    void incActiveTrackCnt() { android_atomic_inc(&mActiveTrackCnt);
                               mTailBufferCount = mMaxTailBuffers; }
    void decActiveTrackCnt() { android_atomic_dec(&mActiveTrackCnt); }
    int32_t activeTrackCnt() const { return android_atomic_acquire_load(&mActiveTrackCnt); }

    uint32_t strategy() const { return mStrategy; }
    void setStrategy(uint32_t strategy)
            { mStrategy = strategy; }

    // suspend effect of the given type
    void setEffectSuspended_l(const effect_uuid_t *type,
                              bool suspend);
    // suspend all eligible effects
    void setEffectSuspendedAll_l(bool suspend);
    // check if effects should be suspend or restored when a given effect is enable or disabled
    void checkSuspendOnEffectEnabled(const sp<EffectModule>& effect,
                                          bool enabled);

    void clearInputBuffer();

    void dump(int fd, const Vector<String16>& args);
#ifdef QCOM_HARDWARE
    bool isForLPATrack() {return mIsForLPATrack; }
    void setLPAFlag(bool flag) {mIsForLPATrack = flag;}
#endif

protected:
    friend class AudioFlinger;  // for mThread, mEffects
    EffectChain(const EffectChain&);
    EffectChain& operator =(const EffectChain&);

    class SuspendedEffectDesc : public RefBase {
    public:
        SuspendedEffectDesc() : mRefCount(0) {}

        int mRefCount;
        effect_uuid_t mType;
        wp<EffectModule> mEffect;
    };

    // get a list of effect modules to suspend when an effect of the type
    // passed is enabled.
    void                       getSuspendEligibleEffects(Vector< sp<EffectModule> > &effects);

    // get an effect module if it is currently enable
    sp<EffectModule> getEffectIfEnabled(const effect_uuid_t *type);
    // true if the effect whose descriptor is passed can be suspended
    // OEMs can modify the rules implemented in this method to exclude specific effect
    // types or implementations from the suspend/restore mechanism.
    bool isEffectEligibleForSuspend(const effect_descriptor_t& desc);

    void clearInputBuffer_l(sp<ThreadBase> thread);

    wp<ThreadBase> mThread;     // parent mixer thread
    Mutex mLock;                // mutex protecting effect list
    Vector< sp<EffectModule> > mEffects; // list of effect modules
    int mSessionId;             // audio session ID
    int16_t *mInBuffer;         // chain input buffer
    int16_t *mOutBuffer;        // chain output buffer

    // 'volatile' here means these are accessed with atomic operations instead of mutex
    volatile int32_t mActiveTrackCnt;    // number of active tracks connected
    volatile int32_t mTrackCnt;          // number of tracks connected

    int32_t mTailBufferCount;   // current effect tail buffer count
    int32_t mMaxTailBuffers;    // maximum effect tail buffers
    bool mOwnInBuffer;          // true if the chain owns its input buffer
    int mVolumeCtrlIdx;         // index of insert effect having control over volume
    uint32_t mLeftVolume;       // previous volume on left channel
    uint32_t mRightVolume;      // previous volume on right channel
    uint32_t mNewLeftVolume;       // new volume on left channel
    uint32_t mNewRightVolume;      // new volume on right channel
    uint32_t mStrategy; // strategy for this effect chain
#ifdef QCOM_HARDWARE
    bool     mIsForLPATrack;
#endif
    // mSuspendedEffects lists all effects currently suspended in the chain.
    // Use effect type UUID timelow field as key. There is no real risk of identical
    // timeLow fields among effect type UUIDs.
    // Updated by updateSuspendedSessions_l() only.
    KeyedVector< int, sp<SuspendedEffectDesc> > mSuspendedEffects;
};
