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
#include <hardware/hardware.h>
#include <media/AudioSystem.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
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
      mNextUniqueId(1),
      mMemoryDealer(new MemoryDealer(1024 * 1024, "SoundTriggerHwService")),
      mCaptureState(false)
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
              SOUND_TRIGGER_HARDWARE_MODULE_ID, HW_MODULE_PREFIX, strerror(-rc));
        return;
    }
    rc = sound_trigger_hw_device_open(mod, &dev);
    if (rc != 0) {
        ALOGE("couldn't open sound trigger hw device in %s.%s (%s)",
              SOUND_TRIGGER_HARDWARE_MODULE_ID, HW_MODULE_PREFIX, strerror(-rc));
        return;
    }
    if (dev->common.version < SOUND_TRIGGER_DEVICE_API_VERSION_1_0 ||
        dev->common.version > SOUND_TRIGGER_DEVICE_API_VERSION_CURRENT) {
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
    IInterface::asBinder(client)->linkToDeath(module);
    moduleInterface = module;

    module->setCaptureState_l(mCaptureState);

    return NO_ERROR;
}

status_t SoundTriggerHwService::setCaptureState(bool active)
{
    ALOGV("setCaptureState %d", active);
    AutoMutex lock(mServiceLock);
    mCaptureState = active;
    for (size_t i = 0; i < mModules.size(); i++) {
        mModules.valueAt(i)->setCaptureState_l(active);
    }
    return NO_ERROR;
}


void SoundTriggerHwService::detachModule(sp<Module> module)
{
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
    sp<SoundTriggerHwService> service = module->service().promote();
    if (service == 0) {
        return;
    }

    service->sendRecognitionEvent(event, module);
}

sp<IMemory> SoundTriggerHwService::prepareRecognitionEvent_l(
                                                    struct sound_trigger_recognition_event *event)
{
    sp<IMemory> eventMemory;

    //sanitize event
    switch (event->type) {
    case SOUND_MODEL_TYPE_KEYPHRASE:
        ALOGW_IF(event->data_size != 0 && event->data_offset !=
                    sizeof(struct sound_trigger_phrase_recognition_event),
                    "prepareRecognitionEvent_l(): invalid data offset %u for keyphrase event type",
                    event->data_offset);
        event->data_offset = sizeof(struct sound_trigger_phrase_recognition_event);
        break;
    case SOUND_MODEL_TYPE_GENERIC:
        ALOGW_IF(event->data_size != 0 && event->data_offset !=
                    sizeof(struct sound_trigger_generic_recognition_event),
                    "prepareRecognitionEvent_l(): invalid data offset %u for generic event type",
                    event->data_offset);
        event->data_offset = sizeof(struct sound_trigger_generic_recognition_event);
        break;
    case SOUND_MODEL_TYPE_UNKNOWN:
        ALOGW_IF(event->data_size != 0 && event->data_offset !=
                    sizeof(struct sound_trigger_recognition_event),
                    "prepareRecognitionEvent_l(): invalid data offset %u for unknown event type",
                    event->data_offset);
        event->data_offset = sizeof(struct sound_trigger_recognition_event);
        break;
    default:
        return eventMemory;
    }

    size_t size = event->data_offset + event->data_size;
    eventMemory = mMemoryDealer->allocate(size);
    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        eventMemory.clear();
        return eventMemory;
    }
    memcpy(eventMemory->pointer(), event, size);

    return eventMemory;
}

void SoundTriggerHwService::sendRecognitionEvent(struct sound_trigger_recognition_event *event,
                                                 Module *module)
 {
     AutoMutex lock(mServiceLock);
     if (module == NULL) {
         return;
     }
    if (event-> type == SOUND_MODEL_TYPE_KEYPHRASE && event->data_size != 0
        && event->data_offset != sizeof(struct sound_trigger_phrase_recognition_event)) {
        // set some defaults for the phrase if the recognition event won't be parsed properly
        // TODO: read defaults from the config

        struct sound_trigger_phrase_recognition_event newEvent;
        memset(&newEvent, 0, sizeof(struct sound_trigger_phrase_recognition_event));

        sp<Model> model = module->getModel(event->model);

        newEvent.num_phrases = 1;
        newEvent.phrase_extras[0].id = 100;
        newEvent.phrase_extras[0].recognition_modes = RECOGNITION_MODE_VOICE_TRIGGER;
        newEvent.phrase_extras[0].confidence_level = 100;
        newEvent.phrase_extras[0].num_levels = 1;
        newEvent.phrase_extras[0].levels[0].level = 100;
        newEvent.phrase_extras[0].levels[0].user_id = 100;
        newEvent.common.status = event->status;
        newEvent.common.type = event->type;
        newEvent.common.model = event->model;
        newEvent.common.capture_available = event->capture_available;
        newEvent.common.capture_session = event->capture_session;
        newEvent.common.capture_delay_ms = event->capture_delay_ms;
        newEvent.common.capture_preamble_ms = event->capture_preamble_ms;
        newEvent.common.trigger_in_data = event->trigger_in_data;
        newEvent.common.audio_config = event->audio_config;
        newEvent.common.data_size = event->data_size;
        newEvent.common.data_offset = sizeof(struct sound_trigger_phrase_recognition_event);

         event = &newEvent.common;
     }
     sp<IMemory> eventMemory = prepareRecognitionEvent_l(event);
     if (eventMemory == 0) {
         return;
     }
     sp<Module> strongModule;
     for (size_t i = 0; i < mModules.size(); i++) {
         if (mModules.valueAt(i).get() == module) {
             strongModule = mModules.valueAt(i);
             break;
         }
     }
     if (strongModule == 0) {
         return;
     }

     sendCallbackEvent_l(new CallbackEvent(CallbackEvent::TYPE_RECOGNITION,
                                                  eventMemory, strongModule));
}

// static
void SoundTriggerHwService::soundModelCallback(struct sound_trigger_model_event *event,
                                               void *cookie)
{
    Module *module = (Module *)cookie;
    if (module == NULL) {
        return;
    }
    sp<SoundTriggerHwService> service = module->service().promote();
    if (service == 0) {
        return;
    }

    service->sendSoundModelEvent(event, module);
}

sp<IMemory> SoundTriggerHwService::prepareSoundModelEvent_l(struct sound_trigger_model_event *event)
{
    sp<IMemory> eventMemory;

    size_t size = event->data_offset + event->data_size;
    eventMemory = mMemoryDealer->allocate(size);
    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        eventMemory.clear();
        return eventMemory;
    }
    memcpy(eventMemory->pointer(), event, size);

    return eventMemory;
}

void SoundTriggerHwService::sendSoundModelEvent(struct sound_trigger_model_event *event,
                                                Module *module)
{
    AutoMutex lock(mServiceLock);
    sp<IMemory> eventMemory = prepareSoundModelEvent_l(event);
    if (eventMemory == 0) {
        return;
    }
    sp<Module> strongModule;
    for (size_t i = 0; i < mModules.size(); i++) {
        if (mModules.valueAt(i).get() == module) {
            strongModule = mModules.valueAt(i);
            break;
        }
    }
    if (strongModule == 0) {
        return;
    }
    sendCallbackEvent_l(new CallbackEvent(CallbackEvent::TYPE_SOUNDMODEL,
                                                 eventMemory, strongModule));
}


sp<IMemory> SoundTriggerHwService::prepareServiceStateEvent_l(sound_trigger_service_state_t state)
{
    sp<IMemory> eventMemory;

    size_t size = sizeof(sound_trigger_service_state_t);
    eventMemory = mMemoryDealer->allocate(size);
    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        eventMemory.clear();
        return eventMemory;
    }
    *((sound_trigger_service_state_t *)eventMemory->pointer()) = state;
    return eventMemory;
}

// call with mServiceLock held
void SoundTriggerHwService::sendServiceStateEvent_l(sound_trigger_service_state_t state,
                                                  Module *module)
{
    sp<IMemory> eventMemory = prepareServiceStateEvent_l(state);
    if (eventMemory == 0) {
        return;
    }
    sp<Module> strongModule;
    for (size_t i = 0; i < mModules.size(); i++) {
        if (mModules.valueAt(i).get() == module) {
            strongModule = mModules.valueAt(i);
            break;
        }
    }
    if (strongModule == 0) {
        return;
    }
    sendCallbackEvent_l(new CallbackEvent(CallbackEvent::TYPE_SERVICE_STATE,
                                                 eventMemory, strongModule));
}

// call with mServiceLock held
void SoundTriggerHwService::sendCallbackEvent_l(const sp<CallbackEvent>& event)
{
    mCallbackThread->sendCallbackEvent(event);
}

void SoundTriggerHwService::onCallbackEvent(const sp<CallbackEvent>& event)
{
    ALOGV("onCallbackEvent");
    sp<Module> module;
    {
        AutoMutex lock(mServiceLock);
        module = event->mModule.promote();
        if (module == 0) {
            return;
        }
    }
    module->onCallbackEvent(event);
    {
        AutoMutex lock(mServiceLock);
        // clear now to execute with mServiceLock locked
        event->mMemory.clear();
    }
}

#undef LOG_TAG
#define LOG_TAG "SoundTriggerHwService::CallbackThread"

SoundTriggerHwService::CallbackThread::CallbackThread(const wp<SoundTriggerHwService>& service)
    : mService(service)
{
}

SoundTriggerHwService::CallbackThread::~CallbackThread()
{
    while (!mEventQueue.isEmpty()) {
        mEventQueue[0]->mMemory.clear();
        mEventQueue.removeAt(0);
    }
}

void SoundTriggerHwService::CallbackThread::onFirstRef()
{
    run("soundTrigger cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}

bool SoundTriggerHwService::CallbackThread::threadLoop()
{
    while (!exitPending()) {
        sp<CallbackEvent> event;
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
            service->onCallbackEvent(event);
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

void SoundTriggerHwService::CallbackThread::sendCallbackEvent(
                        const sp<SoundTriggerHwService::CallbackEvent>& event)
{
    AutoMutex lock(mCallbackLock);
    mEventQueue.add(event);
    mCallbackCond.signal();
}

SoundTriggerHwService::CallbackEvent::CallbackEvent(event_type type, sp<IMemory> memory,
                                                    wp<Module> module)
    : mType(type), mMemory(memory), mModule(module)
{
}

SoundTriggerHwService::CallbackEvent::~CallbackEvent()
{
}


#undef LOG_TAG
#define LOG_TAG "SoundTriggerHwService::Module"

SoundTriggerHwService::Module::Module(const sp<SoundTriggerHwService>& service,
                                      sound_trigger_hw_device* hwDevice,
                                      sound_trigger_module_descriptor descriptor,
                                      const sp<ISoundTriggerClient>& client)
 : mService(service), mHwDevice(hwDevice), mDescriptor(descriptor),
   mClient(client), mServiceState(SOUND_TRIGGER_STATE_NO_INIT)
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
            }
            mHwDevice->unload_sound_model(mHwDevice, model->mHandle);
        }
        mModels.clear();
    }
    if (mClient != 0) {
        IInterface::asBinder(mClient)->unlinkToDeath(this);
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

    size_t structSize;
    if (sound_model->type == SOUND_MODEL_TYPE_KEYPHRASE) {
        structSize = sizeof(struct sound_trigger_phrase_sound_model);
    } else {
        structSize = sizeof(struct sound_trigger_sound_model);
    }

    if (sound_model->data_offset < structSize ||
           sound_model->data_size > (UINT_MAX - sound_model->data_offset) ||
           modelMemory->size() < sound_model->data_offset ||
           sound_model->data_size > (modelMemory->size() - sound_model->data_offset)) {
        android_errorWriteLog(0x534e4554, "30148546");
        ALOGE("loadSoundModel() data_size is too big");
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);

    if (mModels.size() >= mDescriptor.properties.max_sound_models) {
        ALOGW("loadSoundModel(): Not loading, max number of models (%d) would be exceeded",
              mDescriptor.properties.max_sound_models);
        return INVALID_OPERATION;
    }

    status_t status = mHwDevice->load_sound_model(mHwDevice, sound_model,
                                                  SoundTriggerHwService::soundModelCallback,
                                                  this, handle);

    if (status != NO_ERROR) {
        return status;
    }
    audio_session_t session;
    audio_io_handle_t ioHandle;
    audio_devices_t device;

    status = AudioSystem::acquireSoundTriggerSession(&session, &ioHandle, &device);
    if (status != NO_ERROR) {
        return status;
    }

    sp<Model> model = new Model(*handle, session, ioHandle, device, sound_model->type);
    mModels.replaceValueFor(*handle, model);

    return status;
}

status_t SoundTriggerHwService::Module::unloadSoundModel(sound_model_handle_t handle)
{
    ALOGV("unloadSoundModel() model handle %d", handle);
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    AutoMutex lock(mLock);
    return unloadSoundModel_l(handle);
}

status_t SoundTriggerHwService::Module::unloadSoundModel_l(sound_model_handle_t handle)
{
    ssize_t index = mModels.indexOfKey(handle);
    if (index < 0) {
        return BAD_VALUE;
    }
    sp<Model> model = mModels.valueAt(index);
    mModels.removeItem(handle);
    if (model->mState == Model::STATE_ACTIVE) {
        mHwDevice->stop_recognition(mHwDevice, model->mHandle);
        model->mState = Model::STATE_IDLE;
    }
    AudioSystem::releaseSoundTriggerSession(model->mCaptureSession);
    return mHwDevice->unload_sound_model(mHwDevice, handle);
}

status_t SoundTriggerHwService::Module::startRecognition(sound_model_handle_t handle,
                                 const sp<IMemory>& dataMemory)
{
    ALOGV("startRecognition() model handle %d", handle);
    if (!captureHotwordAllowed()) {
        return PERMISSION_DENIED;
    }

    if (dataMemory == 0 || dataMemory->pointer() == NULL) {
        ALOGE("startRecognition() dataMemory is 0 or has NULL pointer()");
        return BAD_VALUE;

    }

    struct sound_trigger_recognition_config *config =
            (struct sound_trigger_recognition_config *)dataMemory->pointer();

    if (config->data_offset < sizeof(struct sound_trigger_recognition_config) ||
            config->data_size > (UINT_MAX - config->data_offset) ||
            dataMemory->size() < config->data_offset ||
            config->data_size > (dataMemory->size() - config->data_offset)) {
        ALOGE("startRecognition() data_size is too big");
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    if (mServiceState == SOUND_TRIGGER_STATE_DISABLED) {
        return INVALID_OPERATION;
    }
    sp<Model> model = getModel(handle);
    if (model == 0) {
        return BAD_VALUE;
    }

    if (model->mState == Model::STATE_ACTIVE) {
        return INVALID_OPERATION;
    }


    //TODO: get capture handle and device from audio policy service
    config->capture_handle = model->mCaptureIOHandle;
    config->capture_device = model->mCaptureDevice;
    status_t status = mHwDevice->start_recognition(mHwDevice, handle, config,
                                        SoundTriggerHwService::recognitionCallback,
                                        this);

    if (status == NO_ERROR) {
        model->mState = Model::STATE_ACTIVE;
        model->mConfig = *config;
    }

    return status;
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
    model->mState = Model::STATE_IDLE;
    return NO_ERROR;
}


void SoundTriggerHwService::Module::onCallbackEvent(const sp<CallbackEvent>& event)
{
    ALOGV("onCallbackEvent type %d", event->mType);

    sp<IMemory> eventMemory = event->mMemory;

    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        return;
    }
    if (mClient == 0) {
        ALOGI("%s mClient == 0", __func__);
        return;
    }

    switch (event->mType) {
    case CallbackEvent::TYPE_RECOGNITION: {
        struct sound_trigger_recognition_event *recognitionEvent =
                (struct sound_trigger_recognition_event *)eventMemory->pointer();
        sp<ISoundTriggerClient> client;
        {
            AutoMutex lock(mLock);
            sp<Model> model = getModel(recognitionEvent->model);
            if (model == 0) {
                ALOGW("%s model == 0", __func__);
                return;
            }
            if (model->mState != Model::STATE_ACTIVE) {
                ALOGV("onCallbackEvent model->mState %d != Model::STATE_ACTIVE", model->mState);
                return;
            }

            recognitionEvent->capture_session = model->mCaptureSession;
            model->mState = Model::STATE_IDLE;
            client = mClient;
        }
        if (client != 0) {
            client->onRecognitionEvent(eventMemory);
        }
    } break;
    case CallbackEvent::TYPE_SOUNDMODEL: {
        struct sound_trigger_model_event *soundmodelEvent =
                (struct sound_trigger_model_event *)eventMemory->pointer();
        sp<ISoundTriggerClient> client;
        {
            AutoMutex lock(mLock);
            sp<Model> model = getModel(soundmodelEvent->model);
            if (model == 0) {
                ALOGW("%s model == 0", __func__);
                return;
            }
            client = mClient;
        }
        if (client != 0) {
            client->onSoundModelEvent(eventMemory);
        }
    } break;
    case CallbackEvent::TYPE_SERVICE_STATE: {
        sp<ISoundTriggerClient> client;
        {
            AutoMutex lock(mLock);
            client = mClient;
        }
        if (client != 0) {
            client->onServiceStateChange(eventMemory);
        }
    } break;
    default:
        LOG_ALWAYS_FATAL("onCallbackEvent unknown event type %d", event->mType);
    }
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

// Called with mServiceLock held
void SoundTriggerHwService::Module::setCaptureState_l(bool active)
{
    ALOGV("Module::setCaptureState_l %d", active);
    sp<SoundTriggerHwService> service;
    sound_trigger_service_state_t state;

    Vector< sp<IMemory> > events;
    {
        AutoMutex lock(mLock);
        state = (active && !mDescriptor.properties.concurrent_capture) ?
                                        SOUND_TRIGGER_STATE_DISABLED : SOUND_TRIGGER_STATE_ENABLED;

        if (state == mServiceState) {
            return;
        }

        mServiceState = state;

        service = mService.promote();
        if (service == 0) {
            return;
        }

        if (state == SOUND_TRIGGER_STATE_ENABLED) {
            goto exit;
        }

        const bool supports_stop_all =
            (mHwDevice->common.version >= SOUND_TRIGGER_DEVICE_API_VERSION_1_1 &&
             mHwDevice->stop_all_recognitions);

        if (supports_stop_all) {
            mHwDevice->stop_all_recognitions(mHwDevice);
        }

        for (size_t i = 0; i < mModels.size(); i++) {
            sp<Model> model = mModels.valueAt(i);
            if (model->mState == Model::STATE_ACTIVE) {
                if (!supports_stop_all) {
                    mHwDevice->stop_recognition(mHwDevice, model->mHandle);
                }
                // keep model in ACTIVE state so that event is processed by onCallbackEvent()
                if (model->mType == SOUND_MODEL_TYPE_KEYPHRASE) {
                    struct sound_trigger_phrase_recognition_event event;
                    memset(&event, 0, sizeof(struct sound_trigger_phrase_recognition_event));
                    event.num_phrases = model->mConfig.num_phrases;
                    for (size_t i = 0; i < event.num_phrases; i++) {
                        event.phrase_extras[i] = model->mConfig.phrases[i];
                    }
                    event.common.status = RECOGNITION_STATUS_ABORT;
                    event.common.type = model->mType;
                    event.common.model = model->mHandle;
                    event.common.data_size = 0;
                    sp<IMemory> eventMemory = service->prepareRecognitionEvent_l(&event.common);
                    if (eventMemory != 0) {
                        events.add(eventMemory);
                    }
                } else if (model->mType == SOUND_MODEL_TYPE_GENERIC) {
                    struct sound_trigger_generic_recognition_event event;
                    memset(&event, 0, sizeof(struct sound_trigger_generic_recognition_event));
                    event.common.status = RECOGNITION_STATUS_ABORT;
                    event.common.type = model->mType;
                    event.common.model = model->mHandle;
                    event.common.data_size = 0;
                    sp<IMemory> eventMemory = service->prepareRecognitionEvent_l(&event.common);
                    if (eventMemory != 0) {
                        events.add(eventMemory);
                    }
                } else if (model->mType == SOUND_MODEL_TYPE_UNKNOWN) {
                    struct sound_trigger_phrase_recognition_event event;
                    memset(&event, 0, sizeof(struct sound_trigger_phrase_recognition_event));
                    event.common.status = RECOGNITION_STATUS_ABORT;
                    event.common.type = model->mType;
                    event.common.model = model->mHandle;
                    event.common.data_size = 0;
                    sp<IMemory> eventMemory = service->prepareRecognitionEvent_l(&event.common);
                    if (eventMemory != 0) {
                        events.add(eventMemory);
                    }
                } else {
                    goto exit;
                }
            }
        }
    }

    for (size_t i = 0; i < events.size(); i++) {
        service->sendCallbackEvent_l(new CallbackEvent(CallbackEvent::TYPE_RECOGNITION, events[i],
                                                     this));
    }

exit:
    service->sendServiceStateEvent_l(state, this);
}


SoundTriggerHwService::Model::Model(sound_model_handle_t handle, audio_session_t session,
                                    audio_io_handle_t ioHandle, audio_devices_t device,
                                    sound_trigger_sound_model_type_t type) :
    mHandle(handle), mState(STATE_IDLE), mCaptureSession(session),
    mCaptureIOHandle(ioHandle), mCaptureDevice(device), mType(type)
{

}

status_t SoundTriggerHwService::Module::dump(int fd __unused,
                                             const Vector<String16>& args __unused) {
    String8 result;
    return NO_ERROR;
}

}; // namespace android
