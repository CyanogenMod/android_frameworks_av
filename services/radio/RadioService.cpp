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

#define LOG_TAG "RadioService"
//#define LOG_NDEBUG 0

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>

#include <system/audio.h>
#include <system/audio_policy.h>
#include <system/radio.h>
#include <system/radio_metadata.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <hardware/hardware.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <hardware/radio.h>
#include <media/AudioSystem.h>
#include "RadioService.h"
#include "RadioRegions.h"

namespace android {

static const char kRadioTunerAudioDeviceName[] = "Radio tuner source";

RadioService::RadioService()
    : BnRadioService(), mNextUniqueId(1)
{
    ALOGI("%s", __FUNCTION__);
}

void RadioService::onFirstRef()
{
    const hw_module_t *mod;
    int rc;
    struct radio_hw_device *dev;

    ALOGI("%s", __FUNCTION__);

    rc = hw_get_module_by_class(RADIO_HARDWARE_MODULE_ID, RADIO_HARDWARE_MODULE_ID_FM, &mod);
    if (rc != 0) {
        ALOGE("couldn't load radio module %s.%s (%s)",
              RADIO_HARDWARE_MODULE_ID, "primary", strerror(-rc));
        return;
    }
    rc = radio_hw_device_open(mod, &dev);
    if (rc != 0) {
        ALOGE("couldn't open radio hw device in %s.%s (%s)",
              RADIO_HARDWARE_MODULE_ID, "primary", strerror(-rc));
        return;
    }
    if (dev->common.version != RADIO_DEVICE_API_VERSION_CURRENT) {
        ALOGE("wrong radio hw device version %04x", dev->common.version);
        return;
    }

    struct radio_hal_properties halProperties;
    rc = dev->get_properties(dev, &halProperties);
    if (rc != 0) {
        ALOGE("could not read implementation properties");
        return;
    }

    radio_properties_t properties;
    properties.handle =
            (radio_handle_t)android_atomic_inc(&mNextUniqueId);

    ALOGI("loaded default module %s, handle %d", properties.product, properties.handle);

    convertProperties(&properties, &halProperties);
    sp<Module> module = new Module(dev, properties);
    mModules.add(properties.handle, module);
}

RadioService::~RadioService()
{
    for (size_t i = 0; i < mModules.size(); i++) {
        radio_hw_device_close(mModules.valueAt(i)->hwDevice());
    }
}

status_t RadioService::listModules(struct radio_properties *properties,
                             uint32_t *numModules)
{
    ALOGV("listModules");

    AutoMutex lock(mServiceLock);
    if (numModules == NULL || (*numModules != 0 && properties == NULL)) {
        return BAD_VALUE;
    }
    size_t maxModules = *numModules;
    *numModules = mModules.size();
    for (size_t i = 0; i < mModules.size() && i < maxModules; i++) {
        properties[i] = mModules.valueAt(i)->properties();
    }
    return NO_ERROR;
}

status_t RadioService::attach(radio_handle_t handle,
                        const sp<IRadioClient>& client,
                        const struct radio_band_config *config,
                        bool withAudio,
                        sp<IRadio>& radio)
{
    ALOGV("%s %d config %p withAudio %d", __FUNCTION__, handle, config, withAudio);

    AutoMutex lock(mServiceLock);
    radio.clear();
    if (client == 0) {
        return BAD_VALUE;
    }
    ssize_t index = mModules.indexOfKey(handle);
    if (index < 0) {
        return BAD_VALUE;
    }
    sp<Module> module = mModules.valueAt(index);

    if (config == NULL) {
        config = module->getDefaultConfig();
        if (config == NULL) {
            return INVALID_OPERATION;
        }
    }
    ALOGV("%s region %d type %d", __FUNCTION__, config->region, config->band.type);

    radio = module->addClient(client, config, withAudio);

    if (radio == 0) {
        return NO_INIT;
    }
    return NO_ERROR;
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

status_t RadioService::dump(int fd, const Vector<String16>& args __unused) {
    String8 result;
    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        result.appendFormat("Permission Denial: can't dump RadioService");
        write(fd, result.string(), result.size());
    } else {
        bool locked = tryLock(mServiceLock);
        // failed to lock - RadioService is probably deadlocked
        if (!locked) {
            result.append("RadioService may be deadlocked\n");
            write(fd, result.string(), result.size());
        }

        if (locked) mServiceLock.unlock();
    }
    return NO_ERROR;
}

status_t RadioService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    return BnRadioService::onTransact(code, data, reply, flags);
}


// static
void RadioService::callback(radio_hal_event_t *halEvent, void *cookie)
{
    CallbackThread *callbackThread = (CallbackThread *)cookie;
    if (callbackThread == NULL) {
        return;
    }
    callbackThread->sendEvent(halEvent);
}

/* static */
void RadioService::convertProperties(radio_properties_t *properties,
                                     const radio_hal_properties_t *halProperties)
{
    memset(properties, 0, sizeof(struct radio_properties));
    properties->class_id = halProperties->class_id;
    strlcpy(properties->implementor, halProperties->implementor,
            RADIO_STRING_LEN_MAX);
    strlcpy(properties->product, halProperties->product,
            RADIO_STRING_LEN_MAX);
    strlcpy(properties->version, halProperties->version,
            RADIO_STRING_LEN_MAX);
    strlcpy(properties->serial, halProperties->serial,
            RADIO_STRING_LEN_MAX);
    properties->num_tuners = halProperties->num_tuners;
    properties->num_audio_sources = halProperties->num_audio_sources;
    properties->supports_capture = halProperties->supports_capture;

    for (size_t i = 0; i < ARRAY_SIZE(sKnownRegionConfigs); i++) {
        const radio_hal_band_config_t *band = &sKnownRegionConfigs[i].band;
        size_t j;
        for (j = 0; j < halProperties->num_bands; j++) {
            const radio_hal_band_config_t *halBand = &halProperties->bands[j];
            size_t k;
            if (band->type != halBand->type) continue;
            if (band->lower_limit < halBand->lower_limit) continue;
            if (band->upper_limit > halBand->upper_limit) continue;
            for (k = 0; k < halBand->num_spacings; k++) {
                if (band->spacings[0] == halBand->spacings[k]) break;
            }
            if (k == halBand->num_spacings) continue;
            if (band->type == RADIO_BAND_AM) break;
            if ((band->fm.deemphasis & halBand->fm.deemphasis) == 0) continue;
            if (halBand->fm.rds == 0) break;
            if ((band->fm.rds & halBand->fm.rds) != 0) break;
        }
        if (j == halProperties->num_bands) continue;

        ALOGI("convertProperties() Adding band type %d region %d",
              sKnownRegionConfigs[i].band.type , sKnownRegionConfigs[i].region);

        memcpy(&properties->bands[properties->num_bands++],
               &sKnownRegionConfigs[i],
               sizeof(radio_band_config_t));
    }
}

#undef LOG_TAG
#define LOG_TAG "RadioService::CallbackThread"

RadioService::CallbackThread::CallbackThread(const wp<ModuleClient>& moduleClient)
    : mModuleClient(moduleClient), mMemoryDealer(new MemoryDealer(1024 * 1024, "RadioService"))
{
}

RadioService::CallbackThread::~CallbackThread()
{
    mEventQueue.clear();
}

void RadioService::CallbackThread::onFirstRef()
{
    run("RadioService cbk", ANDROID_PRIORITY_URGENT_AUDIO);
}

bool RadioService::CallbackThread::threadLoop()
{
    while (!exitPending()) {
        sp<IMemory> eventMemory;
        sp<ModuleClient> moduleClient;
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
            eventMemory = mEventQueue[0];
            mEventQueue.removeAt(0);
            moduleClient = mModuleClient.promote();
        }
        if (moduleClient != 0) {
            moduleClient->onCallbackEvent(eventMemory);
            eventMemory.clear();
        }
    }
    return false;
}

void RadioService::CallbackThread::exit()
{
    Mutex::Autolock _l(mCallbackLock);
    requestExit();
    mCallbackCond.broadcast();
}

sp<IMemory> RadioService::CallbackThread::prepareEvent(radio_hal_event_t *halEvent)
{
    sp<IMemory> eventMemory;

    size_t headerSize =
            (sizeof(struct radio_event) + sizeof(unsigned int) - 1) /sizeof(unsigned int);
    size_t metadataSize = 0;
    switch (halEvent->type) {
    case RADIO_EVENT_TUNED:
    case RADIO_EVENT_AF_SWITCH:
        if (radio_metadata_check(halEvent->info.metadata) == 0) {
            metadataSize = radio_metadata_get_size(halEvent->info.metadata);
        }
        break;
    case RADIO_EVENT_METADATA:
        if (radio_metadata_check(halEvent->metadata) != 0) {
            return eventMemory;
        }
        metadataSize = radio_metadata_get_size(halEvent->metadata);
        break;
    default:
        break;
    }
    size_t size = headerSize + metadataSize;
    eventMemory = mMemoryDealer->allocate(size);
    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        eventMemory.clear();
        return eventMemory;
    }
    struct radio_event *event = (struct radio_event *)eventMemory->pointer();
    event->type = halEvent->type;
    event->status = halEvent->status;

    switch (event->type) {
    case RADIO_EVENT_CONFIG:
        event->config.band = halEvent->config;
        break;
    case RADIO_EVENT_TUNED:
    case RADIO_EVENT_AF_SWITCH:
        event->info = halEvent->info;
        if (metadataSize != 0) {
            memcpy((char *)event + headerSize, halEvent->info.metadata, metadataSize);
            // replace meta data pointer by offset while in shared memory so that receiving side
            // can restore the pointer in destination process.
            event->info.metadata = (radio_metadata_t *)headerSize;
        }
        break;
    case RADIO_EVENT_TA:
    case RADIO_EVENT_EA:
    case RADIO_EVENT_ANTENNA:
    case RADIO_EVENT_CONTROL:
        event->on = halEvent->on;
        break;
    case RADIO_EVENT_METADATA:
        memcpy((char *)event + headerSize, halEvent->metadata, metadataSize);
        // replace meta data pointer by offset while in shared memory so that receiving side
        // can restore the pointer in destination process.
        event->metadata = (radio_metadata_t *)headerSize;
        break;
    case RADIO_EVENT_HW_FAILURE:
    default:
        break;
    }

    return eventMemory;
}

void RadioService::CallbackThread::sendEvent(radio_hal_event_t *event)
 {
     sp<IMemory> eventMemory = prepareEvent(event);
     if (eventMemory == 0) {
         return;
     }

     AutoMutex lock(mCallbackLock);
     mEventQueue.add(eventMemory);
     mCallbackCond.signal();
     ALOGV("%s DONE", __FUNCTION__);
}


#undef LOG_TAG
#define LOG_TAG "RadioService::Module"

RadioService::Module::Module(radio_hw_device* hwDevice, radio_properties properties)
 : mHwDevice(hwDevice), mProperties(properties), mMute(true)
{
}

RadioService::Module::~Module() {
    mModuleClients.clear();
}

status_t RadioService::Module::dump(int fd __unused, const Vector<String16>& args __unused) {
    String8 result;
    return NO_ERROR;
}

sp<RadioService::ModuleClient> RadioService::Module::addClient(const sp<IRadioClient>& client,
                                    const struct radio_band_config *config,
                                    bool audio)
{
    ALOGV("addClient() %p config %p product %s", this, config, mProperties.product);
    AutoMutex lock(mLock);
    sp<ModuleClient> moduleClient;
    int ret;

    for (size_t i = 0; i < mModuleClients.size(); i++) {
        if (mModuleClients[i]->client() == client) {
            // client already connected: reject
            return moduleClient;
        }
    }
    moduleClient = new ModuleClient(this, client, config, audio);

    struct radio_hal_band_config halConfig;
    halConfig = config->band;

    // Tuner preemption logic:
    // There is a limited amount of tuners and a limited amount of radio audio sources per module.
    // The minimum is one tuner and one audio source.
    // The numbers of tuners and sources are indicated in the module properties.
    // NOTE: current framework implementation only supports one radio audio source.
    // It is possible to open more than one tuner at a time but only one tuner can be connected
    // to the radio audio source (AUDIO_DEVICE_IN_FM_TUNER).
    // The base rule is that a newly connected tuner always wins, i.e. always gets a tuner
    // and can use the audio source if requested.
    // If another client is preempted, it is notified by a callback with RADIO_EVENT_CONTROL
    // indicating loss of control.
    // - If the newly connected client requests the audio source (audio == true):
    //    - if an audio source is available
    //          no problem
    //    - if not:
    //          the oldest client in the list using audio is preempted.
    // - If the newly connected client does not request the audio source (audio == false):
    //    - if a tuner is available
    //          no problem
    //    - if not:
    //          The oldest client not using audio is preempted first and if none is found the
    //          the oldest client using audio is preempted.
    // Each time a tuner using the audio source is opened or closed, the audio policy manager is
    // notified of the connection or disconnection of AUDIO_DEVICE_IN_FM_TUNER.

    sp<ModuleClient> oldestTuner;
    sp<ModuleClient> oldestAudio;
    size_t allocatedTuners = 0;
    size_t allocatedAudio = 0;
    for (size_t i = 0; i < mModuleClients.size(); i++) {
        if (mModuleClients[i]->getTuner() != NULL) {
            if (mModuleClients[i]->audio()) {
                if (oldestAudio == 0) {
                    oldestAudio = mModuleClients[i];
                }
                allocatedAudio++;
            } else {
                if (oldestTuner == 0) {
                    oldestTuner = mModuleClients[i];
                }
                allocatedTuners++;
            }
        }
    }

    const struct radio_tuner *halTuner;
    sp<ModuleClient> preemtedClient;
    if (audio) {
        if (allocatedAudio >= mProperties.num_audio_sources) {
            ALOG_ASSERT(oldestAudio != 0, "addClient() allocatedAudio/oldestAudio mismatch");
            preemtedClient = oldestAudio;
        }
    } else {
        if (allocatedAudio + allocatedTuners >= mProperties.num_tuners) {
            if (allocatedTuners != 0) {
                ALOG_ASSERT(oldestTuner != 0, "addClient() allocatedTuners/oldestTuner mismatch");
                preemtedClient = oldestTuner;
            } else {
                ALOG_ASSERT(oldestAudio != 0, "addClient() allocatedAudio/oldestAudio mismatch");
                preemtedClient = oldestAudio;
            }
        }
    }
    if (preemtedClient != 0) {
        halTuner = preemtedClient->getTuner();
        preemtedClient->setTuner(NULL);
        mHwDevice->close_tuner(mHwDevice, halTuner);
        if (preemtedClient->audio()) {
            notifyDeviceConnection(false, "");
        }
    }

    ret = mHwDevice->open_tuner(mHwDevice, &halConfig, audio,
                                RadioService::callback, moduleClient->callbackThread().get(),
                                &halTuner);
    if (ret == 0) {
        ALOGV("addClient() setTuner %p", halTuner);
        moduleClient->setTuner(halTuner);
        mModuleClients.add(moduleClient);
        if (audio) {
            notifyDeviceConnection(true, "");
        }
        ALOGV("addClient() DONE moduleClient %p", moduleClient.get());
    } else {
        ALOGW("%s open_tuner failed with error %d", __FUNCTION__, ret);
        moduleClient.clear();
    }

    return moduleClient;
}

void RadioService::Module::removeClient(const sp<ModuleClient>& moduleClient) {
    ALOGV("removeClient()");
    AutoMutex lock(mLock);
    int ret;
    ssize_t index = -1;

    for (size_t i = 0; i < mModuleClients.size(); i++) {
        if (mModuleClients[i] == moduleClient) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        return;
    }

    mModuleClients.removeAt(index);
    const struct radio_tuner *halTuner = moduleClient->getTuner();
    if (halTuner == NULL) {
        return;
    }

    mHwDevice->close_tuner(mHwDevice, halTuner);
    if (moduleClient->audio()) {
        notifyDeviceConnection(false, "");
    }

    mMute = true;

    if (mModuleClients.isEmpty()) {
        return;
    }

    // Tuner reallocation logic:
    // When a client is removed and was controlling a tuner, this tuner will be allocated to a
    // previously preempted client. This client will be notified by a callback with
    // RADIO_EVENT_CONTROL indicating gain of control.
    // - If a preempted client is waiting for an audio source and one becomes available:
    //    Allocate the tuner to the most recently added client waiting for an audio source
    // - If not:
    //    Allocate the tuner to the most recently added client.
    // Each time a tuner using the audio source is opened or closed, the audio policy manager is
    // notified of the connection or disconnection of AUDIO_DEVICE_IN_FM_TUNER.

    sp<ModuleClient> youngestClient;
    sp<ModuleClient> youngestClientAudio;
    size_t allocatedTuners = 0;
    size_t allocatedAudio = 0;
    for (ssize_t i = mModuleClients.size() - 1; i >= 0; i--) {
        if (mModuleClients[i]->getTuner() == NULL) {
            if (mModuleClients[i]->audio()) {
                if (youngestClientAudio == 0) {
                    youngestClientAudio = mModuleClients[i];
                }
            } else {
                if (youngestClient == 0) {
                    youngestClient = mModuleClients[i];
                }
            }
        } else {
            if (mModuleClients[i]->audio()) {
                allocatedAudio++;
            } else {
                allocatedTuners++;
            }
        }
    }

    ALOG_ASSERT(allocatedTuners + allocatedAudio < mProperties.num_tuners,
                "removeClient() removed client but no tuner available");

    ALOG_ASSERT(!moduleClient->audio() || allocatedAudio < mProperties.num_audio_sources,
                "removeClient() removed audio client but no tuner with audio available");

    if (allocatedAudio < mProperties.num_audio_sources && youngestClientAudio != 0) {
        youngestClient = youngestClientAudio;
    }

    ALOG_ASSERT(youngestClient != 0, "removeClient() removed client no candidate found for tuner");

    struct radio_hal_band_config halConfig = youngestClient->halConfig();
    ret = mHwDevice->open_tuner(mHwDevice, &halConfig, youngestClient->audio(),
                                RadioService::callback, moduleClient->callbackThread().get(),
                                &halTuner);

    if (ret == 0) {
        youngestClient->setTuner(halTuner);
        if (youngestClient->audio()) {
            notifyDeviceConnection(true, "");
        }
    }
}

status_t RadioService::Module::setMute(bool mute)
{
    Mutex::Autolock _l(mLock);
    if (mute != mMute) {
        mMute = mute;
        //TODO notifify audio policy manager of media activity on radio audio device
    }
    return NO_ERROR;
}

status_t RadioService::Module::getMute(bool *mute)
{
    Mutex::Autolock _l(mLock);
    *mute = mMute;
    return NO_ERROR;
}


const struct radio_band_config *RadioService::Module::getDefaultConfig() const
{
    if (mProperties.num_bands == 0) {
        return NULL;
    }
    return &mProperties.bands[0];
}

void RadioService::Module::notifyDeviceConnection(bool connected,
                                                  const char *address) {
    int64_t token = IPCThreadState::self()->clearCallingIdentity();
    AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_IN_FM_TUNER,
                                          connected ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE :
                                                  AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
                                          address, kRadioTunerAudioDeviceName);
    IPCThreadState::self()->restoreCallingIdentity(token);
}

#undef LOG_TAG
#define LOG_TAG "RadioService::ModuleClient"

RadioService::ModuleClient::ModuleClient(const sp<Module>& module,
                                         const sp<IRadioClient>& client,
                                         const struct radio_band_config *config,
                                         bool audio)
 : mModule(module), mClient(client), mConfig(*config), mAudio(audio), mTuner(NULL)
{
}

void RadioService::ModuleClient::onFirstRef()
{
    mCallbackThread = new CallbackThread(this);
    IInterface::asBinder(mClient)->linkToDeath(this);
}

RadioService::ModuleClient::~ModuleClient() {
    if (mClient != 0) {
        IInterface::asBinder(mClient)->unlinkToDeath(this);
        mClient.clear();
    }
    if (mCallbackThread != 0) {
        mCallbackThread->exit();
    }
}

status_t RadioService::ModuleClient::dump(int fd __unused,
                                             const Vector<String16>& args __unused) {
    String8 result;
    return NO_ERROR;
}

void RadioService::ModuleClient::detach() {
    ALOGV("%s", __FUNCTION__);
    sp<ModuleClient> strongMe = this;
    {
        AutoMutex lock(mLock);
        if (mClient != 0) {
            IInterface::asBinder(mClient)->unlinkToDeath(this);
            mClient.clear();
        }
    }
    sp<Module> module = mModule.promote();
    if (module == 0) {
        return;
    }
    module->removeClient(this);
}

radio_hal_band_config_t RadioService::ModuleClient::halConfig() const
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    return mConfig.band;
}

const struct radio_tuner *RadioService::ModuleClient::getTuner() const
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    return mTuner;
}

void RadioService::ModuleClient::setTuner(const struct radio_tuner *tuner)
{
    ALOGV("%s %p", __FUNCTION__, this);

    AutoMutex lock(mLock);
    mTuner = tuner;
    ALOGV("%s locked", __FUNCTION__);

    radio_hal_event_t event;
    event.type = RADIO_EVENT_CONTROL;
    event.status = 0;
    event.on = mTuner != NULL;
    mCallbackThread->sendEvent(&event);
    ALOGV("%s DONE", __FUNCTION__);

}

status_t RadioService::ModuleClient::setConfiguration(const struct radio_band_config *config)
{
    AutoMutex lock(mLock);
    status_t status = NO_ERROR;
    ALOGV("%s locked", __FUNCTION__);

    if (mTuner != NULL) {
        struct radio_hal_band_config halConfig;
        halConfig = config->band;
        status = (status_t)mTuner->set_configuration(mTuner, &halConfig);
        if (status == NO_ERROR) {
            mConfig = *config;
        }
    } else {
        mConfig = *config;
        status == INVALID_OPERATION;
    }

    return status;
}

status_t RadioService::ModuleClient::getConfiguration(struct radio_band_config *config)
{
    AutoMutex lock(mLock);
    status_t status = NO_ERROR;
    ALOGV("%s locked", __FUNCTION__);

    if (mTuner != NULL) {
        struct radio_hal_band_config halConfig;
        status = (status_t)mTuner->get_configuration(mTuner, &halConfig);
        if (status == NO_ERROR) {
            mConfig.band = halConfig;
        }
    }
    *config = mConfig;

    return status;
}

status_t RadioService::ModuleClient::setMute(bool mute)
{
    sp<Module> module;
    {
        Mutex::Autolock _l(mLock);
        ALOGV("%s locked", __FUNCTION__);
        if (mTuner == NULL || !mAudio) {
            return INVALID_OPERATION;
        }
        module = mModule.promote();
        if (module == 0) {
            return NO_INIT;
        }
    }
    module->setMute(mute);
    return NO_ERROR;
}

status_t RadioService::ModuleClient::getMute(bool *mute)
{
    sp<Module> module;
    {
        Mutex::Autolock _l(mLock);
        ALOGV("%s locked", __FUNCTION__);
        module = mModule.promote();
        if (module == 0) {
            return NO_INIT;
        }
    }
    return module->getMute(mute);
}

status_t RadioService::ModuleClient::scan(radio_direction_t direction, bool skipSubChannel)
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    status_t status;
    if (mTuner != NULL) {
        status = (status_t)mTuner->scan(mTuner, direction, skipSubChannel);
    } else {
        status = INVALID_OPERATION;
    }
    return status;
}

status_t RadioService::ModuleClient::step(radio_direction_t direction, bool skipSubChannel)
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    status_t status;
    if (mTuner != NULL) {
        status = (status_t)mTuner->step(mTuner, direction, skipSubChannel);
    } else {
        status = INVALID_OPERATION;
    }
    return status;
}

status_t RadioService::ModuleClient::tune(unsigned int channel, unsigned int subChannel)
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    status_t status;
    if (mTuner != NULL) {
        status = (status_t)mTuner->tune(mTuner, channel, subChannel);
    } else {
        status = INVALID_OPERATION;
    }
    return status;
}

status_t RadioService::ModuleClient::cancel()
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    status_t status;
    if (mTuner != NULL) {
        status = (status_t)mTuner->cancel(mTuner);
    } else {
        status = INVALID_OPERATION;
    }
    return status;
}

status_t RadioService::ModuleClient::getProgramInformation(struct radio_program_info *info)
{
    AutoMutex lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    status_t status;
    if (mTuner != NULL) {
        status = (status_t)mTuner->get_program_information(mTuner, info);
    } else {
        status = INVALID_OPERATION;
    }
    return status;
}

status_t RadioService::ModuleClient::hasControl(bool *hasControl)
{
    Mutex::Autolock lock(mLock);
    ALOGV("%s locked", __FUNCTION__);
    *hasControl = mTuner != NULL;
    return NO_ERROR;
}

void RadioService::ModuleClient::onCallbackEvent(const sp<IMemory>& eventMemory)
{
    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        return;
    }

    sp<IRadioClient> client;
    {
        AutoMutex lock(mLock);
        ALOGV("%s locked", __FUNCTION__);
        radio_event_t *event = (radio_event_t *)eventMemory->pointer();
        switch (event->type) {
        case RADIO_EVENT_CONFIG:
            mConfig.band = event->config.band;
            event->config.region = mConfig.region;
            break;
        default:
            break;
        }

        client = mClient;
    }
    if (client != 0) {
        client->onEvent(eventMemory);
    }
}


void RadioService::ModuleClient::binderDied(
    const wp<IBinder> &who __unused) {
    ALOGW("client binder died for client %p", this);
    detach();
}

}; // namespace android
