/*
**
** Copyright 2015, The Android Open Source Project
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

#define LOG_TAG "BpRadioService"
//
#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Errors.h>

#include <stdint.h>
#include <sys/types.h>
#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>

#include <radio/IRadioService.h>
#include <radio/IRadio.h>
#include <radio/IRadioClient.h>

namespace android {

enum {
    LIST_MODULES = IBinder::FIRST_CALL_TRANSACTION,
    ATTACH,
};

#define MAX_ITEMS_PER_LIST 1024

class BpRadioService: public BpInterface<IRadioService>
{
public:
    BpRadioService(const sp<IBinder>& impl)
        : BpInterface<IRadioService>(impl)
    {
    }

    virtual status_t listModules(struct radio_properties *properties,
                                 uint32_t *numModules)
    {
        if (numModules == NULL || (*numModules != 0 && properties == NULL)) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IRadioService::getInterfaceDescriptor());
        unsigned int numModulesReq = (properties == NULL) ? 0 : *numModules;
        data.writeInt32(numModulesReq);
        status_t status = remote()->transact(LIST_MODULES, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            *numModules = (unsigned int)reply.readInt32();
        }
        ALOGV("listModules() status %d got *numModules %d", status, *numModules);
        if (status == NO_ERROR) {
            if (numModulesReq > *numModules) {
                numModulesReq = *numModules;
            }
            if (numModulesReq > 0) {
                reply.read(properties, numModulesReq * sizeof(struct radio_properties));
            }
        }
        return status;
    }

    virtual status_t attach(radio_handle_t handle,
                            const sp<IRadioClient>& client,
                            const struct radio_band_config *config,
                            bool withAudio,
                            sp<IRadio>& radio)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadioService::getInterfaceDescriptor());
        data.writeInt32(handle);
        data.writeStrongBinder(IInterface::asBinder(client));
        ALOGV("attach() config %p withAudio %d region %d type %d", config, withAudio, config->region, config->band.type);
        if (config == NULL) {
            data.writeInt32(0);
        } else {
            data.writeInt32(1);
            data.write(config, sizeof(struct radio_band_config));
        }
        data.writeInt32(withAudio ? 1 : 0);
        status_t status = remote()->transact(ATTACH, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = reply.readInt32();
        if (reply.readInt32() != 0) {
            radio = interface_cast<IRadio>(reply.readStrongBinder());
        }
        return status;
    }
};

IMPLEMENT_META_INTERFACE(RadioService, "android.hardware.IRadioService");

// ----------------------------------------------------------------------

status_t BnRadioService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case LIST_MODULES: {
            CHECK_INTERFACE(IRadioService, data, reply);
            unsigned int numModulesReq = data.readInt32();
            if (numModulesReq > MAX_ITEMS_PER_LIST) {
                numModulesReq = MAX_ITEMS_PER_LIST;
            }
            unsigned int numModules = numModulesReq;
            struct radio_properties *properties =
                    (struct radio_properties *)calloc(numModulesReq,
                                                      sizeof(struct radio_properties));
            if (properties == NULL) {
                reply->writeInt32(NO_MEMORY);
                reply->writeInt32(0);
                return NO_ERROR;
            }

            status_t status = listModules(properties, &numModules);
            reply->writeInt32(status);
            reply->writeInt32(numModules);
            ALOGV("LIST_MODULES status %d got numModules %d", status, numModules);

            if (status == NO_ERROR) {
                if (numModulesReq > numModules) {
                    numModulesReq = numModules;
                }
                reply->write(properties,
                             numModulesReq * sizeof(struct radio_properties));
            }
            free(properties);
            return NO_ERROR;
        } break;

        case ATTACH: {
            CHECK_INTERFACE(IRadioService, data, reply);
            radio_handle_t handle = data.readInt32();
            sp<IRadioClient> client =
                    interface_cast<IRadioClient>(data.readStrongBinder());
            struct radio_band_config config;
            struct radio_band_config *configPtr = NULL;
            if (data.readInt32() != 0) {
                data.read(&config, sizeof(struct radio_band_config));
                configPtr = &config;
            }
            bool withAudio = data.readInt32() != 0;
            ALOGV("ATTACH configPtr %p withAudio %d", configPtr, withAudio);
            sp<IRadio> radio;
            status_t status = attach(handle, client, configPtr, withAudio, radio);
            reply->writeInt32(status);
            if (radio != 0) {
                reply->writeInt32(1);
                reply->writeStrongBinder(IInterface::asBinder(radio));
            } else {
                reply->writeInt32(0);
            }
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
