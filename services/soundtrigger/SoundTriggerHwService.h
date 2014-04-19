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

    virtual status_t    onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);

    virtual status_t    dump(int fd, const Vector<String16>& args);

    class Model : public RefBase {
     public:

        enum {
            STATE_IDLE,
            STATE_ACTIVE
        };

        Model(sound_model_handle_t handle);
        ~Model() {}

        sp<IMemory> allocateMemory(size_t size);
        void deallocateMemory();

        sound_model_handle_t    mHandle;
        int                     mState;
        audio_io_handle_t       mInputHandle;
        audio_session_t         mCaptureSession;
        sp<MemoryDealer>        mMemoryDealer;
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
       sp<ISoundTriggerClient> client() { return mClient; }

       void sendRecognitionEvent(struct sound_trigger_recognition_event *event);
       void onRecognitionEvent(sp<IMemory> eventMemory);

       sp<Model> getModel(sound_model_handle_t handle);

       // IBinder::DeathRecipient implementation
       virtual void        binderDied(const wp<IBinder> &who);

    private:
        Mutex                                  mLock;
        wp<SoundTriggerHwService>              mService;
        struct sound_trigger_hw_device*        mHwDevice;
        struct sound_trigger_module_descriptor mDescriptor;
        sp<ISoundTriggerClient>                mClient;
        DefaultKeyedVector< sound_model_handle_t, sp<Model> >     mModels;
    }; // class Module

    class RecognitionEvent : public RefBase {
    public:

        RecognitionEvent(sp<IMemory> eventMemory, wp<Module> module);

        virtual             ~RecognitionEvent();

        sp<IMemory> mEventMemory;
        wp<Module> mModule;
    };

    class CallbackThread : public Thread {
    public:

        CallbackThread(const wp<SoundTriggerHwService>& service);

        virtual             ~CallbackThread();

        // Thread virtuals
        virtual bool        threadLoop();

        // RefBase
        virtual void        onFirstRef();

                void        exit();
                void        sendRecognitionEvent(const sp<RecognitionEvent>& event);

    private:
        wp<SoundTriggerHwService>   mService;
        Condition                   mCallbackCond;
        Mutex                       mCallbackLock;
        Vector< sp<RecognitionEvent> > mEventQueue;
    };

    void detachModule(sp<Module> module);

    static void recognitionCallback(struct sound_trigger_recognition_event *event, void *cookie);
    void sendRecognitionEvent(const sp<RecognitionEvent>& event);
    void onRecognitionEvent(const sp<RecognitionEvent>& event);

    static void soundModelCallback(struct sound_trigger_model_event *event, void *cookie);

private:

    virtual void onFirstRef();

    Mutex               mServiceLock;
    volatile int32_t    mNextUniqueId;
    DefaultKeyedVector< sound_trigger_module_handle_t, sp<Module> >     mModules;
    sp<CallbackThread>  mCallbackThread;
};

} // namespace android

#endif // ANDROID_HARDWARE_SOUNDTRIGGER_HAL_SERVICE_H
