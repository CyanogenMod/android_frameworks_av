/*
 * Copyright (C) 2014 The Android Open Source Project
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

#define LOG_TAG "SoundTriggerHwService"
//#define LOG_NDEBUG 0

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>

#include <system/sound_trigger.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <hardware/hardware.h>
#include <hardware/sound_trigger.h>
#include <ServiceUtilities.h>
#include "SoundTriggerHwService.h"

namespace android {

#ifdef SOUND_TRIGGER_USE_STUB_MODULE
#define HW_MODULE_PREFIX "stub"
#else
#define HW_MODULE_PREFIX "primary"
#endif

SoundTriggerHwService::SoundTriggerHwService()
    : BnSoundTriggerHwService(),
      mNextUniqueId(1)
{
}

void SoundTriggerHwService::onFirstRef()
{
    const hw_module_t *mod;
    int rc;
    sound_trigger_hw_device *dev;

    rc = hw_get_module_by_class(SOUND_TRIGGER_HARDWARE_MODULE_ID, HW_MODULE_PREFIX, &mod);
    if (rc != 0) {
        ALOGE("couldn't load sound trigger module %s.%s (%s)",
              SOUND_TRIGGER_HARDWARE_MODULE_ID, "primary", strerror(-rc));
        return;
    }
    rc = sound_trigger_hw_device_open(mod, &dev);
    if (rc != 0) {
        ALOGE("couldn't open sound trigger hw device in %s.%s (%s)",
              SOUND_TRIGGER_HARDWARE_MODULE_ID, "primary", strerror(-rc));
        return;
    }
    if (dev->common.version != SOUND_TRIGGER_DEVICE_API_VERSION_CURRENT) {
        ALOGE("wrong sound trigger hw device version %04x", dev->common.version);
        return;
    }

    sound_trigger_module_descriptor descriptor;
    rc = dev->get_properties(dev, &descriptor.properties);
    if (rc != 0) {
        ALOGE("could not read implementation properties");
        return;
    }
    descriptor.handle =
            (sound_trigger_module_handle_t)android_atomic_inc(&mNextUniqueId);
    ALOGI("loaded default module %s, handle %d", descriptor.properties.description,
                                                 descriptor.handle);

    sp<ISoundTriggerClient> client;
    sp<Module> module = new Module(this, dev, descriptor, client);
    mModules.add(descriptor.handle, module);
    mCallbackThread = new CallbackThread(this);
}

SoundTriggerHwService::~SoundTriggerHwService()
{
    if (mCallbackThread != 0) {
        mCallbackThread->exit();
    }
    for (size_t i = 0; i < mModules.size(); i++) {
        sound_trigger_hw_device_close(mModules.valueAt(i)->hwDevice());
    }
}

status_t SoundTriggerHwService::listModules(struct sound_trigger_module_descriptor *modules,
                             uint32_t *numModules)
{
    ALOGV("listModules");
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mServiceLock);
    if (numModules == NULL || (*numModules != 0 && modules == NULL)) {
        return BAD_VALUE;
    }
    size_t maxModules = *numModules;
    *numModules = mModules.size();
    for (size_t i = 0; i < mModules.size() && i < maxModules; i++) {
        modules[i] = mModules.valueAt(i)->descriptor();
    }
    return NO_ERROR;
}

status_t SoundTriggerHwService::attach(const sound_trigger_module_handle_t handle,
                        const sp<ISoundTriggerClient>& client,
                        sp<ISoundTrigger>& moduleInterface)
{
    ALOGV("attach module %d", handle);
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mServiceLock);
    moduleInterface.clear();
    if (client == 0) {
        return BAD_VALUE;
    }
    ssize_t index = mModules.indexOfKey(handle);
    if (index < 0) {
        return BAD_VALUE;
    }
    sp<Module> module = mModules.valueAt(index);

    module->setClient(client);
    client->asBinder()->linkToDeath(module);
    moduleInterface = module;

    return NO_ERROR;
}

void SoundTriggerHwService::detachModule(sp<Module> module) {
    ALOGV("detachModule");
    AutoMutex lock(mServiceLock);
    module->clearClient();
}

static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 60000;

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}

status_t SoundTriggerHwService::dump(int fd, const Vector<String16>& args __unused) {
    String8 result;
    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        result.appendFormat("Permission Denial: can't dump SoundTriggerHwService");
        write(fd, result.string(), result.size());
    } else {
        bool locked = tryLock(mServiceLock);
        // failed to lock - SoundTriggerHwService is probably deadlocked
        if (!locked) {
            result.append("SoundTriggerHwService may be deadlocked\n");
            write(fd, result.string(), result.size());
        }

        if (locked) mServiceLock.unlock();
    }
    return NO_ERROR;
}

status_t SoundTriggerHwService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    return BnSoundTriggerHwService::onTransact(code, data, reply, flags);
}


// static
void SoundTriggerHwService::recognitionCallback(struct sound_trigger_recognition_event *event,
                                                void *cookie)
{
    Module *module = (Module *)cookie;
    if (module == NULL) {
        return;
    }
    module->sendRecognitionEvent(event);
}


void SoundTriggerHwService::sendRecognitionEvent(const sp<RecognitionEvent>& event)
{
    mCallbackThread->sendRecognitionEvent(event);
}

void SoundTriggerHwService::onRecognitionEvent(const sp<RecognitionEvent>& event)
{
    ALOGV("onRecognitionEvent");
    sp<Module> module;
    {
        AutoMutex lock(mServiceLock);
        module = event->mModule.promote();
        if (module == 0) {
            return;
        }
    }
    module->onRecognitionEvent(event->mEventMemory);
}

// static
void SoundTriggerHwService::soundModelCallback(struct sound_trigger_model_event *event __unused,
                                               void *cookie)
{
    Module *module = (Module *)cookie;

}

#undef LOG_TAG
#define LOG_TAG "SoundTriggerHwService::CallbackThread"

SoundTriggerHwService::CallbackThread::CallbackThread(const wp<SoundTriggerHwService>& service)
    : mService(service)
{
}

SoundTriggerHwService::CallbackThread::~CallbackThread()
{
    mEventQueue.clear();
}

void SoundTriggerHwService::CallbackThread::onFirstRef()
{
    run("soundTrigger cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}

bool SoundTriggerHwService::CallbackThread::threadLoop()
{
    while (!exitPending()) {
        sp<RecognitionEvent> event;
        sp<SoundTriggerHwService> service;
        {
            Mutex::Autolock _l(mCallbackLock);
            while (mEventQueue.isEmpty() && !exitPending()) {
                ALOGV("CallbackThread::threadLoop() sleep");
                mCallbackCond.wait(mCallbackLock);
                ALOGV("CallbackThread::threadLoop() wake up");
            }
            if (exitPending()) {
                break;
            }
            event = mEventQueue[0];
            mEventQueue.removeAt(0);
            service = mService.promote();
        }
        if (service != 0) {
            service->onRecognitionEvent(event);
        }
    }
    return false;
}

void SoundTriggerHwService::CallbackThread::exit()
{
    Mutex::Autolock _l(mCallbackLock);
    requestExit();
    mCallbackCond.broadcast();
}

void SoundTriggerHwService::CallbackThread::sendRecognitionEvent(
                        const sp<SoundTriggerHwService::RecognitionEvent>& event)
{
    AutoMutex lock(mCallbackLock);
    mEventQueue.add(event);
    mCallbackCond.signal();
}

SoundTriggerHwService::RecognitionEvent::RecognitionEvent(
                                            sp<IMemory> eventMemory,
                                            wp<Module> module)
    : mEventMemory(eventMemory), mModule(module)
{
}

SoundTriggerHwService::RecognitionEvent::~RecognitionEvent()
{
}

#undef LOG_TAG
#define LOG_TAG "SoundTriggerHwService::Module"

SoundTriggerHwService::Module::Module(const sp<SoundTriggerHwService>& service,
                                      sound_trigger_hw_device* hwDevice,
                                      sound_trigger_module_descriptor descriptor,
                                      const sp<ISoundTriggerClient>& client)
 : mService(service), mHwDevice(hwDevice), mDescriptor(descriptor),
   mClient(client)
{
}

SoundTriggerHwService::Module::~Module() {
}

void SoundTriggerHwService::Module::detach() {
    ALOGV("detach()");
    if (!captureHotwordAllowed()) {
        return;
    }
    {
        AutoMutex lock(mLock);
        for (size_t i = 0; i < mModels.size(); i++) {
            sp<Model> model = mModels.valueAt(i);
            ALOGV("detach() unloading model %d", model->mHandle);
            if (model->mState == Model::STATE_ACTIVE) {
                mHwDevice->stop_recognition(mHwDevice, model->mHandle);
                model->deallocateMemory();
            }
            mHwDevice->unload_sound_model(mHwDevice, model->mHandle);
        }
        mModels.clear();
    }
    if (mClient != 0) {
        mClient->asBinder()->unlinkToDeath(this);
    }
    sp<SoundTriggerHwService> service = mService.promote();
    if (service == 0) {
        return;
    }
    service->detachModule(this);
}

status_t SoundTriggerHwService::Module::loadSoundModel(const sp<IMemory>& modelMemory,
                                sound_model_handle_t *handle)
{
    ALOGV("loadSoundModel() handle");
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    if (modelMemory == 0 || modelMemory->pointer() == NULL) {
        ALOGE("loadSoundModel() modelMemory is 0 or has NULL pointer()");
        return BAD_VALUE;
    }
    struct sound_trigger_sound_model *sound_model =
            (struct sound_trigger_sound_model *)modelMemory->pointer();

    AutoMutex lock(mLock);
    status_t status = mHwDevice->load_sound_model(mHwDevice,
                                                  sound_model,
                                                  SoundTriggerHwService::soundModelCallback,
                                                  this,
                                                  handle);
    if (status == NO_ERROR) {
        mModels.replaceValueFor(*handle, new Model(*handle));
    }

    return status;
}

status_t SoundTriggerHwService::Module::unloadSoundModel(sound_model_handle_t handle)
{
    ALOGV("unloadSoundModel() model handle %d", handle);
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mLock);
    ssize_t index = mModels.indexOfKey(handle);
    if (index < 0) {
        return BAD_VALUE;
    }
    sp<Model> model = mModels.valueAt(index);
    mModels.removeItem(handle);
    if (model->mState == Model::STATE_ACTIVE) {
        mHwDevice->stop_recognition(mHwDevice, model->mHandle);
        model->deallocateMemory();
    }
    return mHwDevice->unload_sound_model(mHwDevice, handle);
}

status_t SoundTriggerHwService::Module::startRecognition(sound_model_handle_t handle,
                                 const sp<IMemory>& dataMemory)
{
    ALOGV("startRecognition() model handle %d", handle);
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    if (dataMemory != 0 && dataMemory->pointer() == NULL) {
        ALOGE("startRecognition() dataMemory is non-0 but has NULL pointer()");
        return BAD_VALUE;

    }
    AutoMutex lock(mLock);
    sp<Model> model = getModel(handle);
    if (model == 0) {
        return BAD_VALUE;
    }
    if ((dataMemory == 0) ||
            (dataMemory->size() < sizeof(struct sound_trigger_recognition_config))) {
        return BAD_VALUE;
    }

    if (model->mState == Model::STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    model->mState = Model::STATE_ACTIVE;

    struct sound_trigger_recognition_config *config =
            (struct sound_trigger_recognition_config *)dataMemory->pointer();

    //TODO: get capture handle and device from audio policy service
    config->capture_handle = AUDIO_IO_HANDLE_NONE;
    config->capture_device = AUDIO_DEVICE_NONE;
    return mHwDevice->start_recognition(mHwDevice, handle, config,
                                        SoundTriggerHwService::recognitionCallback,
                                        this);
}

status_t SoundTriggerHwService::Module::stopRecognition(sound_model_handle_t handle)
{
    ALOGV("stopRecognition() model handle %d", handle);
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mLock);
    sp<Model> model = getModel(handle);
    if (model == 0) {
        return BAD_VALUE;
    }

    if (model->mState != Model::STATE_ACTIVE) {
        return INVALID_OPERATION;
    }
    mHwDevice->stop_recognition(mHwDevice, handle);
    model->deallocateMemory();
    model->mState = Model::STATE_IDLE;
    return NO_ERROR;
}

void SoundTriggerHwService::Module::sendRecognitionEvent(
                                                    struct sound_trigger_recognition_event *event)
{
    sp<SoundTriggerHwService> service;
    sp<IMemory> eventMemory;
    ALOGV("sendRecognitionEvent for model %d", event->model);
    {
        AutoMutex lock(mLock);
        sp<Model> model = getModel(event->model);
        if (model == 0) {
            return;
        }
        if (model->mState != Model::STATE_ACTIVE) {
            ALOGV("sendRecognitionEvent model->mState %d != Model::STATE_ACTIVE", model->mState);
            return;
        }
        if (mClient == 0) {
            return;
        }
        service = mService.promote();
        if (service == 0) {
            return;
        }

        //sanitize event
        switch (event->type) {
        case SOUND_MODEL_TYPE_KEYPHRASE:
            ALOGW_IF(event->data_offset !=
                        sizeof(struct sound_trigger_phrase_recognition_event),
                        "sendRecognitionEvent(): invalid data offset %u for keyphrase event type",
                        event->data_offset);
            event->data_offset = sizeof(struct sound_trigger_phrase_recognition_event);
            break;
        case SOUND_MODEL_TYPE_UNKNOWN:
            ALOGW_IF(event->data_offset !=
                        sizeof(struct sound_trigger_recognition_event),
                        "sendRecognitionEvent(): invalid data offset %u for unknown event type",
                        event->data_offset);
            event->data_offset = sizeof(struct sound_trigger_recognition_event);
            break;
        default:
                return;
        }

        size_t size = event->data_offset + event->data_size;
        eventMemory = model->allocateMemory(size);
        if (eventMemory == 0 || eventMemory->pointer() == NULL) {
            return;
        }
        memcpy(eventMemory->pointer(), event, size);
    }
    service->sendRecognitionEvent(new RecognitionEvent(eventMemory, this));
}

void SoundTriggerHwService::Module::onRecognitionEvent(sp<IMemory> eventMemory)
{
    ALOGV("Module::onRecognitionEvent");

    AutoMutex lock(mLock);

    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        return;
    }
    struct sound_trigger_recognition_event *event =
            (struct sound_trigger_recognition_event *)eventMemory->pointer();

    sp<Model> model = getModel(event->model);
    if (model == 0) {
        ALOGI("%s model == 0", __func__);
        return;
    }
    if (model->mState != Model::STATE_ACTIVE) {
        ALOGV("onRecognitionEvent model->mState %d != Model::STATE_ACTIVE", model->mState);
        return;
    }
    if (mClient == 0) {
        ALOGI("%s mClient == 0", __func__);
        return;
    }
    mClient->onRecognitionEvent(eventMemory);
    model->mState = Model::STATE_IDLE;
    model->deallocateMemory();
}

sp<SoundTriggerHwService::Model> SoundTriggerHwService::Module::getModel(
        sound_model_handle_t handle)
{
    sp<Model> model;
    ssize_t index = mModels.indexOfKey(handle);
    if (index >= 0) {
        model = mModels.valueAt(index);
    }
    return model;
}

void SoundTriggerHwService::Module::binderDied(
    const wp<IBinder> &who __unused) {
    ALOGW("client binder died for module %d", mDescriptor.handle);
    detach();
}


SoundTriggerHwService::Model::Model(sound_model_handle_t handle) :
    mHandle(handle), mState(STATE_IDLE), mInputHandle(AUDIO_IO_HANDLE_NONE),
    mCaptureSession(AUDIO_SESSION_ALLOCATE),
    mMemoryDealer(new MemoryDealer(sizeof(struct sound_trigger_recognition_event),
                                   "SoundTriggerHwService::Event"))
{

}


sp<IMemory> SoundTriggerHwService::Model::allocateMemory(size_t size)
{
    sp<IMemory> memory;
    if (mMemoryDealer->getMemoryHeap()->getSize() < size) {
        mMemoryDealer = new MemoryDealer(size, "SoundTriggerHwService::Event");
    }
    memory = mMemoryDealer->allocate(size);
    return memory;
}

void SoundTriggerHwService::Model::deallocateMemory()
{
    mMemoryDealer->deallocate(0);
}

status_t SoundTriggerHwService::Module::dump(int fd __unused,
                                             const Vector<String16>& args __unused) {
    String8 result;
    return NO_ERROR;
}

}; // namespace android
