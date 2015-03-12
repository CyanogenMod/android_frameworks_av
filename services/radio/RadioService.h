/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_RADIO_SERVICE_H
#define ANDROID_HARDWARE_RADIO_SERVICE_H

#include <utils/Vector.h>
//#include <binder/AppOpsManager.h>
#include <binder/MemoryDealer.h>
#include <binder/BinderService.h>
#include <binder/IAppOpsCallback.h>
#include <radio/IRadioService.h>
#include <radio/IRadio.h>
#include <radio/IRadioClient.h>
#include <system/radio.h>
#include <hardware/radio.h>

namespace android {

class MemoryHeapBase;

class RadioService :
    public BinderService<RadioService>,
    public BnRadioService
{
    friend class BinderService<RadioService>;

public:
    class ModuleClient;
    class Module;

    static char const* getServiceName() { return "media.radio"; }

                        RadioService();
    virtual             ~RadioService();

    // IRadioService
    virtual status_t listModules(struct radio_properties *properties,
                                 uint32_t *numModules);

    virtual status_t attach(radio_handle_t handle,
                            const sp<IRadioClient>& client,
                            const struct radio_band_config *config,
                            bool withAudio,
                            sp<IRadio>& radio);

    virtual status_t    onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);

    virtual status_t    dump(int fd, const Vector<String16>& args);


    class Module : public virtual RefBase {
    public:

       Module(radio_hw_device* hwDevice,
              struct radio_properties properties);

       virtual ~Module();

               sp<ModuleClient> addClient(const sp<IRadioClient>& client,
                                  const struct radio_band_config *config,
                                  bool audio);

               void removeClient(const sp<ModuleClient>& moduleClient);

               status_t setMute(bool mute);

               status_t getMute(bool *mute);

       virtual status_t dump(int fd, const Vector<String16>& args);

       const struct radio_hw_device *hwDevice() const { return mHwDevice; }
       const struct radio_properties properties() const { return mProperties; }
       const struct radio_band_config *getDefaultConfig() const ;

    private:

       void notifyDeviceConnection(bool connected, const char *address);

        Mutex                         mLock;          // protects  mModuleClients
        const struct radio_hw_device  *mHwDevice;     // HAL hardware device
        const struct radio_properties mProperties;    // cached hardware module properties
        Vector< sp<ModuleClient> >    mModuleClients; // list of attached clients
        bool                          mMute;          // radio audio source state
                                                      // when unmuted, audio is routed to the
                                                      // output device selected for media use case.
    }; // class Module

    class CallbackThread : public Thread {
    public:

        CallbackThread(const wp<ModuleClient>& moduleClient);

        virtual ~CallbackThread();


        // Thread virtuals
        virtual bool threadLoop();

        // RefBase
        virtual void onFirstRef();

                void exit();

                void sendEvent(radio_hal_event_t *halEvent);
                sp<IMemory> prepareEvent(radio_hal_event_t *halEvent);

    private:
        wp<ModuleClient>      mModuleClient;    // client module the thread belongs to
        Condition             mCallbackCond;    // condition signaled when a new event is posted
        Mutex                 mCallbackLock;    // protects mEventQueue
        Vector< sp<IMemory> > mEventQueue;      // pending callback events
        sp<MemoryDealer>      mMemoryDealer;    // shared memory for callback event
    }; // class CallbackThread

    class ModuleClient : public BnRadio,
                   public IBinder::DeathRecipient {
    public:

       ModuleClient(const sp<Module>& module,
              const sp<IRadioClient>& client,
              const struct radio_band_config *config,
              bool audio);

       virtual ~ModuleClient();

       // IRadio
       virtual void detach();

       virtual status_t setConfiguration(const struct radio_band_config *config);

       virtual status_t getConfiguration(struct radio_band_config *config);

       virtual status_t setMute(bool mute);

       virtual status_t getMute(bool *mute);

       virtual status_t scan(radio_direction_t direction, bool skipSubChannel);

       virtual status_t step(radio_direction_t direction, bool skipSubChannel);

       virtual status_t tune(unsigned int channel, unsigned int subChannel);

       virtual status_t cancel();

       virtual status_t getProgramInformation(struct radio_program_info *info);

       virtual status_t hasControl(bool *hasControl);

       virtual status_t dump(int fd, const Vector<String16>& args);

               sp<IRadioClient> client() const { return mClient; }
               wp<Module> module() const { return mModule; }
               radio_hal_band_config_t halConfig() const;
               sp<CallbackThread> callbackThread() const { return mCallbackThread; }
               void setTuner(const struct radio_tuner *tuner);
               const struct radio_tuner *getTuner() const;
               bool audio() const { return mAudio; }

               void onCallbackEvent(const sp<IMemory>& event);

       virtual void onFirstRef();


       // IBinder::DeathRecipient implementation
       virtual void        binderDied(const wp<IBinder> &who);

    private:

        mutable Mutex               mLock;           // protects mClient, mConfig and mTuner
        wp<Module>                  mModule;         // The module this client is attached to
        sp<IRadioClient>            mClient;         // event callback binder interface
        radio_band_config_t         mConfig;         // current band configuration
        sp<CallbackThread>          mCallbackThread; // event callback thread
        const bool                  mAudio;
        const struct radio_tuner    *mTuner;        // HAL tuner interface. NULL indicates that
                                                    // this client does not have control on any
                                                    // tuner
    }; // class ModuleClient


    static void callback(radio_hal_event_t *halEvent, void *cookie);

private:

    virtual void onFirstRef();

    static void convertProperties(radio_properties_t *properties,
                                  const radio_hal_properties_t *halProperties);
    Mutex               mServiceLock;   // protects mModules
    volatile int32_t    mNextUniqueId;  // for module ID allocation
    DefaultKeyedVector< radio_handle_t, sp<Module> > mModules;
};

} // namespace android

#endif // ANDROID_HARDWARE_RADIO_SERVICE_H
