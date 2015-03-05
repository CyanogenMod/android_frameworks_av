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

#define LOG_TAG "IRadio"
#include <utils/Log.h>
#include <utils/Errors.h>
#include <binder/IMemory.h>
#include <radio/IRadio.h>
#include <radio/IRadioService.h>
#include <radio/IRadioClient.h>
#include <system/radio.h>
#include <system/radio_metadata.h>

namespace android {

enum {
    DETACH = IBinder::FIRST_CALL_TRANSACTION,
    SET_CONFIGURATION,
    GET_CONFIGURATION,
    SET_MUTE,
    GET_MUTE,
    SCAN,
    STEP,
    TUNE,
    CANCEL,
    GET_PROGRAM_INFORMATION,
    HAS_CONTROL
};

class BpRadio: public BpInterface<IRadio>
{
public:
    BpRadio(const sp<IBinder>& impl)
        : BpInterface<IRadio>(impl)
    {
    }

    void detach()
    {
        ALOGV("detach");
        Parcel data, reply;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        remote()->transact(DETACH, data, &reply);
    }

    virtual status_t setConfiguration(const struct radio_band_config *config)
    {
        Parcel data, reply;
        if (config == NULL) {
            return BAD_VALUE;
        }
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        data.write(config, sizeof(struct radio_band_config));
        status_t status = remote()->transact(SET_CONFIGURATION, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t getConfiguration(struct radio_band_config *config)
    {
        Parcel data, reply;
        if (config == NULL) {
            return BAD_VALUE;
        }
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        status_t status = remote()->transact(GET_CONFIGURATION, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            if (status == NO_ERROR) {
                reply.read(config, sizeof(struct radio_band_config));
            }
        }
        return status;
    }

    virtual status_t setMute(bool mute)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        data.writeInt32(mute ? 1 : 0);
        status_t status = remote()->transact(SET_MUTE, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t getMute(bool *mute)
    {
        Parcel data, reply;
        if (mute == NULL) {
            return BAD_VALUE;
        }
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        status_t status = remote()->transact(GET_MUTE, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            if (status == NO_ERROR) {
                int muteread = reply.readInt32();
                *mute = muteread != 0;
            }
        }
        return status;
    }

    virtual status_t scan(radio_direction_t direction, bool skipSubChannel)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        data.writeInt32(direction);
        data.writeInt32(skipSubChannel ? 1 : 0);
        status_t status = remote()->transact(SCAN, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t step(radio_direction_t direction, bool skipSubChannel)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        data.writeInt32(direction);
        data.writeInt32(skipSubChannel ? 1 : 0);
        status_t status = remote()->transact(STEP, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t tune(unsigned int channel, unsigned int subChannel)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        data.writeInt32(channel);
        data.writeInt32(subChannel);
        status_t status = remote()->transact(TUNE, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t cancel()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        status_t status = remote()->transact(CANCEL, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t getProgramInformation(struct radio_program_info *info)
    {
        Parcel data, reply;
        if (info == NULL) {
            return BAD_VALUE;
        }
        radio_metadata_t *metadata = info->metadata;
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        status_t status = remote()->transact(GET_PROGRAM_INFORMATION, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            if (status == NO_ERROR) {
                reply.read(info, sizeof(struct radio_program_info));
                info->metadata = metadata;
                if (metadata == NULL) {
                    return status;
                }
                size_t size = (size_t)reply.readInt32();
                if (size == 0) {
                    return status;
                }
                metadata =
                    (radio_metadata_t *)calloc(size / sizeof(unsigned int), sizeof(unsigned int));
                if (metadata == NULL) {
                    return NO_MEMORY;
                }
                reply.read(metadata, size);
                status = radio_metadata_add_metadata(&info->metadata, metadata);
                free(metadata);
            }
        }
        return status;
    }

    virtual status_t hasControl(bool *hasControl)
    {
        Parcel data, reply;
        if (hasControl == NULL) {
            return BAD_VALUE;
        }
        data.writeInterfaceToken(IRadio::getInterfaceDescriptor());
        status_t status = remote()->transact(HAS_CONTROL, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            if (status == NO_ERROR) {
                *hasControl = reply.readInt32() != 0;
            }
        }
        return status;
    }
};

IMPLEMENT_META_INTERFACE(Radio, "android.hardware.IRadio");

// ----------------------------------------------------------------------

status_t BnRadio::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case DETACH: {
            ALOGV("DETACH");
            CHECK_INTERFACE(IRadio, data, reply);
            detach();
            return NO_ERROR;
        } break;
        case SET_CONFIGURATION: {
            CHECK_INTERFACE(IRadio, data, reply);
            struct radio_band_config config;
            data.read(&config, sizeof(struct radio_band_config));
            status_t status = setConfiguration(&config);
            reply->writeInt32(status);
            return NO_ERROR;
        }
        case GET_CONFIGURATION: {
            CHECK_INTERFACE(IRadio, data, reply);
            struct radio_band_config config;
            status_t status = getConfiguration(&config);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->write(&config, sizeof(struct radio_band_config));
            }
            return NO_ERROR;
        }
        case SET_MUTE: {
            CHECK_INTERFACE(IRadio, data, reply);
            bool mute = data.readInt32() != 0;
            status_t status = setMute(mute);
            reply->writeInt32(status);
            return NO_ERROR;
        }
        case GET_MUTE: {
            CHECK_INTERFACE(IRadio, data, reply);
            bool mute;
            status_t status = getMute(&mute);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->writeInt32(mute ? 1 : 0);
            }
            return NO_ERROR;
        }
        case SCAN: {
            CHECK_INTERFACE(IRadio, data, reply);
            radio_direction_t direction = (radio_direction_t)data.readInt32();
            bool skipSubChannel = data.readInt32() == 1;
            status_t status = scan(direction, skipSubChannel);
            reply->writeInt32(status);
            return NO_ERROR;
        }
        case STEP: {
            CHECK_INTERFACE(IRadio, data, reply);
            radio_direction_t direction = (radio_direction_t)data.readInt32();
            bool skipSubChannel = data.readInt32() == 1;
            status_t status = step(direction, skipSubChannel);
            reply->writeInt32(status);
            return NO_ERROR;
        }
        case TUNE: {
            CHECK_INTERFACE(IRadio, data, reply);
            unsigned int channel = (unsigned int)data.readInt32();
            unsigned int subChannel = (unsigned int)data.readInt32();
            status_t status = tune(channel, subChannel);
            reply->writeInt32(status);
            return NO_ERROR;
        }
        case CANCEL: {
            CHECK_INTERFACE(IRadio, data, reply);
            status_t status = cancel();
            reply->writeInt32(status);
            return NO_ERROR;
        }
        case GET_PROGRAM_INFORMATION: {
            CHECK_INTERFACE(IRadio, data, reply);
            struct radio_program_info info;

            status_t status = radio_metadata_allocate(&info.metadata, 0, 0);
            if (status != NO_ERROR) {
                return status;
            }
            status = getProgramInformation(&info);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->write(&info, sizeof(struct radio_program_info));
                int count = radio_metadata_get_count(info.metadata);
                if (count > 0) {
                    size_t size = radio_metadata_get_size(info.metadata);
                    reply->writeInt32(size);
                    reply->write(info.metadata, size);
                } else {
                    reply->writeInt32(0);
                }
            }
            radio_metadata_deallocate(info.metadata);
            return NO_ERROR;
        }
        case HAS_CONTROL: {
            CHECK_INTERFACE(IRadio, data, reply);
            bool control;
            status_t status = hasControl(&control);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->writeInt32(control ? 1 : 0);
            }
            return NO_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
