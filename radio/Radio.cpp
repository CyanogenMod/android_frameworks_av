/*
**
** Copyright (C) 2015, The Android Open Source Project
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

#define LOG_TAG "Radio"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/threads.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/IMemory.h>

#include <radio/Radio.h>
#include <radio/IRadio.h>
#include <radio/IRadioService.h>
#include <radio/IRadioClient.h>
#include <radio/RadioCallback.h>

namespace android {

namespace {
    sp<IRadioService>          gRadioService;
    const int                  kRadioServicePollDelay = 500000; // 0.5s
    const char*                kRadioServiceName      = "media.radio";
    Mutex                      gLock;

    class DeathNotifier : public IBinder::DeathRecipient
    {
    public:
        DeathNotifier() {
        }

        virtual void binderDied(const wp<IBinder>& who __unused) {
            ALOGV("binderDied");
            Mutex::Autolock _l(gLock);
            gRadioService.clear();
            ALOGW("Radio service died!");
        }
    };

    sp<DeathNotifier>         gDeathNotifier;
}; // namespace anonymous

const sp<IRadioService> Radio::getRadioService()
{
    Mutex::Autolock _l(gLock);
    if (gRadioService.get() == 0) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder;
        do {
            binder = sm->getService(String16(kRadioServiceName));
            if (binder != 0) {
                break;
            }
            ALOGW("RadioService not published, waiting...");
            usleep(kRadioServicePollDelay);
        } while(true);
        if (gDeathNotifier == NULL) {
            gDeathNotifier = new DeathNotifier();
        }
        binder->linkToDeath(gDeathNotifier);
        gRadioService = interface_cast<IRadioService>(binder);
    }
    ALOGE_IF(gRadioService == 0, "no RadioService!?");
    return gRadioService;
}

// Static methods
status_t Radio::listModules(struct radio_properties *properties,
                            uint32_t *numModules)
{
    ALOGV("listModules()");
    const sp<IRadioService> service = getRadioService();
    if (service == 0) {
        return NO_INIT;
    }
    return service->listModules(properties, numModules);
}

sp<Radio> Radio::attach(radio_handle_t handle,
                        const struct radio_band_config *config,
                        bool withAudio,
                        const sp<RadioCallback>& callback)
{
    ALOGV("attach()");
    sp<Radio> radio;
    const sp<IRadioService> service = getRadioService();
    if (service == 0) {
        return radio;
    }
    radio = new Radio(handle, callback);
    status_t status = service->attach(handle, radio, config, withAudio, radio->mIRadio);

    if (status == NO_ERROR && radio->mIRadio != 0) {
        IInterface::asBinder(radio->mIRadio)->linkToDeath(radio);
    } else {
        ALOGW("Error %d connecting to radio service", status);
        radio.clear();
    }
    return radio;
}



// Radio
Radio::Radio(radio_handle_t handle, const sp<RadioCallback>& callback)
    : mHandle(handle), mCallback(callback)
{
}

Radio::~Radio()
{
    if (mIRadio != 0) {
        mIRadio->detach();
    }
}


void Radio::detach() {
    ALOGV("detach()");
    Mutex::Autolock _l(mLock);
    mCallback.clear();
    if (mIRadio != 0) {
        mIRadio->detach();
        IInterface::asBinder(mIRadio)->unlinkToDeath(this);
        mIRadio = 0;
    }
}

status_t Radio::setConfiguration(const struct radio_band_config *config)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->setConfiguration(config);
}

status_t Radio::getConfiguration(struct radio_band_config *config)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->getConfiguration(config);
}

status_t Radio::setMute(bool mute)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->setMute(mute);
}

status_t Radio::getMute(bool *mute)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->getMute(mute);
}

status_t Radio::scan(radio_direction_t direction, bool skipSubchannel)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->scan(direction, skipSubchannel);
}

status_t Radio::step(radio_direction_t direction, bool skipSubchannel)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->step(direction, skipSubchannel);
}

status_t Radio::tune(unsigned int channel, unsigned int subChannel)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->tune(channel, subChannel);
}

status_t Radio::cancel()
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->cancel();
}

status_t Radio::getProgramInformation(struct radio_program_info *info)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->getProgramInformation(info);
}

status_t Radio::hasControl(bool *hasControl)
{
    Mutex::Autolock _l(mLock);
    if (mIRadio == 0) {
        return NO_INIT;
    }
    return mIRadio->hasControl(hasControl);
}


// BpRadioClient
void Radio::onEvent(const sp<IMemory>& eventMemory)
{
    Mutex::Autolock _l(mLock);
    if (eventMemory == 0 || eventMemory->pointer() == NULL) {
        return;
    }

    struct radio_event *event = (struct radio_event *)eventMemory->pointer();
    // restore local metadata pointer from offset
    switch (event->type) {
    case RADIO_EVENT_TUNED:
    case RADIO_EVENT_AF_SWITCH:
        if (event->info.metadata != NULL) {
            event->info.metadata =
                    (radio_metadata_t *)((char *)event + (size_t)event->info.metadata);
        }
        break;
    case RADIO_EVENT_METADATA:
        if (event->metadata != NULL) {
            event->metadata =
                    (radio_metadata_t *)((char *)event + (size_t)event->metadata);
        }
        break;
    default:
        break;
    }

    if (mCallback != 0) {
        mCallback->onEvent(event);
    }
}


//IBinder::DeathRecipient
void Radio::binderDied(const wp<IBinder>& who __unused) {
    Mutex::Autolock _l(mLock);
    ALOGW("Radio server binder Died ");
    mIRadio = 0;
    struct radio_event event;
    memset(&event, 0, sizeof(struct radio_event));
    event.type = RADIO_EVENT_SERVER_DIED;
    event.status = DEAD_OBJECT;
    if (mCallback != 0) {
        mCallback->onEvent(&event);
    }
}

}; // namespace android
