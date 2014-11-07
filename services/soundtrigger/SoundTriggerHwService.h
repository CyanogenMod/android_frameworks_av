/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_HARDWARE_SOUNDTRIGGER_HAL_SERVICE_H
#define ANDROID_HARDWARE_SOUNDTRIGGER_HAL_SERVICE_H

#include <utils/Vector.h>
//#include <binder/AppOpsManager.h>
#include <binder/MemoryDealer.h>
#include <binder/BinderService.h>
#include <binder/IAppOpsCallback.h>
#include <soundtrigger/ISoundTriggerHwService.h>
#include <soundtrigger/ISoundTrigger.h>
#include <soundtrigger/ISoundTriggerClient.h>
#include <system/sound_trigger.h>
#include <hardware/sound_trigger.h>

namespace android {

class MemoryHeapBase;

class SoundTriggerHwService :
    public BinderService<SoundTriggerHwService>,
    public BnSoundTriggerHwService
{
    friend class BinderService<SoundTriggerHwService>;
public:
    class Module;

    static char const* getServiceName() { return "media.sound_trigger_hw"; }

                        SoundTriggerHwService();
    virtual             ~SoundTriggerHwService();

    // ISoundTriggerHwService
    virtual status_t listModules(struct sound_trigger_module_descriptor *modules,
                                 uint32_t *numModules);

    virtual status_t attach(const sound_trigger_module_handle_t handle,
                            const sp<ISoundTriggerClient>& client,
                            sp<ISoundTrigger>& module);

    virtual status_t setCaptureState(bool active);

    virtual status_t    onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);

    virtual status_t    dump(int fd, const Vector<String16>& args);

    class Model : public RefBase {
     public:

        enum {
            STATE_IDLE,
            STATE_ACTIVE
        };

        Model(sound_model_handle_t handle, audio_session_t session, audio_io_handle_t ioHandle,
              audio_devices_t device, sound_trigger_sound_model_type_t type);
        ~Model() {}

        sound_model_handle_t    mHandle;
        int                     mState;
        audio_session_t         mCaptureSession;
        audio_io_handle_t       mCaptureIOHandle;
        audio_devices_t         mCaptureDevice;
        sound_trigger_sound_model_type_t mType;
        struct sound_trigger_recognition_config mConfig;
    };

    class CallbackEvent : public RefBase {
    public:
        typedef enum {
            TYPE_RECOGNITION,
            TYPE_SOUNDMODEL,
            TYPE_SERVICE_STATE,
        } event_type;
        CallbackEvent(event_type type, sp<IMemory> memory, wp<Module> module);

        virtual             ~CallbackEvent();

        event_type mType;
        sp<IMemory> mMemory;
        wp<Module> mModule;
    };

    class Module : public virtual RefBase,
                   public BnSoundTrigger,
                   public IBinder::DeathRecipient     {
    public:

       Module(const sp<SoundTriggerHwService>& service,
              sound_trigger_hw_device* hwDevice,
              sound_trigger_module_descriptor descriptor,
              const sp<ISoundTriggerClient>& client);

       virtual ~Module();

       virtual void detach();

       virtual status_t loadSoundModel(const sp<IMemory>& modelMemory,
                                       sound_model_handle_t *handle);

       virtual status_t unloadSoundModel(sound_model_handle_t handle);

       virtual status_t startRecognition(sound_model_handle_t handle,
                                         const sp<IMemory>& dataMemory);
       virtual status_t stopRecognition(sound_model_handle_t handle);

       virtual status_t dump(int fd, const Vector<String16>& args);


       sound_trigger_hw_device *hwDevice() const { return mHwDevice; }
       struct sound_trigger_module_descriptor descriptor() { return mDescriptor; }
       void setClient(sp<ISoundTriggerClient> client) { mClient = client; }
       void clearClient() { mClient.clear(); }
       sp<ISoundTriggerClient> client() const { return mClient; }
       wp<SoundTriggerHwService> service() const { return mService; }

       void onCallbackEvent(const sp<CallbackEvent>& event);

       sp<Model> getModel(sound_model_handle_t handle);

       void setCaptureState_l(bool active);

       // IBinder::DeathRecipient implementation
       virtual void        binderDied(const wp<IBinder> &who);

    private:

        Mutex                                  mLock;
        wp<SoundTriggerHwService>              mService;
        struct sound_trigger_hw_device*        mHwDevice;
        struct sound_trigger_module_descriptor mDescriptor;
        sp<ISoundTriggerClient>                mClient;
        DefaultKeyedVector< sound_model_handle_t, sp<Model> >     mModels;
        sound_trigger_service_state_t          mServiceState;
    }; // class Module

    class CallbackThread : public Thread {
    public:

        CallbackThread(const wp<SoundTriggerHwService>& service);

        virtual             ~CallbackThread();

        // Thread virtuals
        virtual bool        threadLoop();

        // RefBase
        virtual void        onFirstRef();

                void        exit();
                void        sendCallbackEvent(const sp<CallbackEvent>& event);

    private:
        wp<SoundTriggerHwService>   mService;
        Condition                   mCallbackCond;
        Mutex                       mCallbackLock;
        Vector< sp<CallbackEvent> > mEventQueue;
    };

           void detachModule(sp<Module> module);

    static void recognitionCallback(struct sound_trigger_recognition_event *event, void *cookie);
           sp<IMemory> prepareRecognitionEvent_l(struct sound_trigger_recognition_event *event);
           void sendRecognitionEvent(struct sound_trigger_recognition_event *event, Module *module);

    static void soundModelCallback(struct sound_trigger_model_event *event, void *cookie);
           sp<IMemory> prepareSoundModelEvent_l(struct sound_trigger_model_event *event);
           void sendSoundModelEvent(struct sound_trigger_model_event *event, Module *module);

           sp<IMemory> prepareServiceStateEvent_l(sound_trigger_service_state_t state);
           void sendServiceStateEvent_l(sound_trigger_service_state_t state, Module *module);

           void sendCallbackEvent_l(const sp<CallbackEvent>& event);
           void onCallbackEvent(const sp<CallbackEvent>& event);

private:

    virtual void onFirstRef();

    Mutex               mServiceLock;
    volatile int32_t    mNextUniqueId;
    DefaultKeyedVector< sound_trigger_module_handle_t, sp<Module> >     mModules;
    sp<CallbackThread>  mCallbackThread;
    sp<MemoryDealer>    mMemoryDealer;
    bool                mCaptureState;
};

} // namespace android

#endif // ANDROID_HARDWARE_SOUNDTRIGGER_HAL_SERVICE_H
