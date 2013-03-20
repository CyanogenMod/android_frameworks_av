/*
 * Copyright (C) 2013 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "IDrm"
#include <utils/Log.h>

#include <binder/Parcel.h>
#include <media/IDrm.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>

namespace android {

enum {
    INIT_CHECK = IBinder::FIRST_CALL_TRANSACTION,
    IS_CRYPTO_SUPPORTED,
    CREATE_PLUGIN,
    DESTROY_PLUGIN,
    OPEN_SESSION,
    CLOSE_SESSION,
    GET_LICENSE_REQUEST,
    PROVIDE_LICENSE_RESPONSE,
    REMOVE_LICENSE,
    QUERY_LICENSE_STATUS,
    GET_PROVISION_REQUEST,
    PROVIDE_PROVISION_RESPONSE,
    GET_SECURE_STOPS,
    RELEASE_SECURE_STOPS,
    GET_PROPERTY_STRING,
    GET_PROPERTY_BYTE_ARRAY,
    SET_PROPERTY_STRING,
    SET_PROPERTY_BYTE_ARRAY
};

struct BpDrm : public BpInterface<IDrm> {
    BpDrm(const sp<IBinder> &impl)
        : BpInterface<IDrm>(impl) {
    }

    virtual status_t initCheck() const {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());
        remote()->transact(INIT_CHECK, data, &reply);

        return reply.readInt32();
    }

    virtual bool isCryptoSchemeSupported(const uint8_t uuid[16]) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());
        data.write(uuid, 16);
        remote()->transact(IS_CRYPTO_SUPPORTED, data, &reply);

        return reply.readInt32() != 0;
    }

    virtual status_t createPlugin(const uint8_t uuid[16]) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());
        data.write(uuid, 16);

        remote()->transact(CREATE_PLUGIN, data, &reply);

        return reply.readInt32();
    }

    virtual status_t destroyPlugin() {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());
        remote()->transact(DESTROY_PLUGIN, data, &reply);

        return reply.readInt32();
    }

    virtual status_t openSession(Vector<uint8_t> &sessionId) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        remote()->transact(OPEN_SESSION, data, &reply);
        uint32_t size = reply.readInt32();
        sessionId.insertAt((size_t)0, size);
        reply.read(sessionId.editArray(), size);

        return reply.readInt32();
    }

    virtual status_t closeSession(Vector<uint8_t> const &sessionId) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(sessionId.size());
        data.write(sessionId.array(), sessionId.size());
        remote()->transact(CLOSE_SESSION, data, &reply);

        return reply.readInt32();
    }

    virtual status_t
        getLicenseRequest(Vector<uint8_t> const &sessionId,
                          Vector<uint8_t> const &initData,
                          String8 const &mimeType, DrmPlugin::LicenseType licenseType,
                          KeyedVector<String8, String8> const &optionalParameters,
                          Vector<uint8_t> &request, String8 &defaultUrl) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(sessionId.size());
        data.write(sessionId.array(), sessionId.size());

        data.writeInt32(initData.size());
        data.write(initData.array(), initData.size());

        data.writeString8(mimeType);
        data.writeInt32((uint32_t)licenseType);

        data.writeInt32(optionalParameters.size());
        for (size_t i = 0; i < optionalParameters.size(); ++i) {
            data.writeString8(optionalParameters.keyAt(i));
            data.writeString8(optionalParameters.valueAt(i));
        }
        remote()->transact(GET_LICENSE_REQUEST, data, &reply);

        uint32_t len = reply.readInt32();
        request.insertAt((size_t)0, len);
        reply.read(request.editArray(), len);
        defaultUrl = reply.readString8();

        return reply.readInt32();
    }

    virtual status_t provideLicenseResponse(Vector<uint8_t> const &sessionId,
                                            Vector<uint8_t> const &response) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(sessionId.size());
        data.write(sessionId.array(), sessionId.size());
        data.writeInt32(response.size());
        data.write(response.array(), response.size());
        remote()->transact(PROVIDE_LICENSE_RESPONSE, data, &reply);

        return reply.readInt32();
    }

    virtual status_t removeLicense(Vector<uint8_t> const &sessionId) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(sessionId.size());
        data.write(sessionId.array(), sessionId.size());
        remote()->transact(REMOVE_LICENSE, data, &reply);

        return reply.readInt32();
    }

    virtual status_t queryLicenseStatus(Vector<uint8_t> const &sessionId,
                                        KeyedVector<String8, String8> &infoMap) const {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(sessionId.size());
        data.write(sessionId.array(), sessionId.size());

        remote()->transact(QUERY_LICENSE_STATUS, data, &reply);

        infoMap.clear();
        size_t count = reply.readInt32();
        for (size_t i = 0; i < count; i++) {
            String8 key = reply.readString8();
            String8 value = reply.readString8();
            infoMap.add(key, value);
        }
        return reply.readInt32();
    }

    virtual status_t getProvisionRequest(Vector<uint8_t> &request,
                                         String8 &defaultUrl) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        remote()->transact(GET_PROVISION_REQUEST, data, &reply);

        uint32_t len = reply.readInt32();
        request.insertAt((size_t)0, len);
        reply.read(request.editArray(), len);
        defaultUrl = reply.readString8();

        return reply.readInt32();
    }

    virtual status_t provideProvisionResponse(Vector<uint8_t> const &response) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(response.size());
        data.write(response.array(), response.size());
        remote()->transact(PROVIDE_PROVISION_RESPONSE, data, &reply);

        return reply.readInt32();
    }

    virtual status_t getSecureStops(List<Vector<uint8_t> > &secureStops) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        remote()->transact(GET_SECURE_STOPS, data, &reply);

        secureStops.clear();
        uint32_t count = reply.readInt32();
        for (size_t i = 0; i < count; i++) {
            Vector<uint8_t> secureStop;
            uint32_t len = reply.readInt32();
            secureStop.insertAt((size_t)0, len);
            reply.read(secureStop.editArray(), len);
            secureStops.push_back(secureStop);
        }
        return reply.readInt32();
    }

    virtual status_t releaseSecureStops(Vector<uint8_t> const &ssRelease) {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeInt32(ssRelease.size());
        data.write(ssRelease.array(), ssRelease.size());
        remote()->transact(RELEASE_SECURE_STOPS, data, &reply);

        return reply.readInt32();
    }

    virtual status_t getPropertyString(String8 const &name, String8 &value) const {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeString8(name);
        remote()->transact(GET_PROPERTY_STRING, data, &reply);

        value = reply.readString8();
        return reply.readInt32();
    }

    virtual status_t getPropertyByteArray(String8 const &name, Vector<uint8_t> &value) const {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeString8(name);
        remote()->transact(GET_PROPERTY_BYTE_ARRAY, data, &reply);

        uint32_t len = reply.readInt32();
        value.insertAt((size_t)0, len);
        reply.read(value.editArray(), len);

        return reply.readInt32();
    }

    virtual status_t setPropertyString(String8 const &name, String8 const &value) const {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeString8(name);
        data.writeString8(value);
        remote()->transact(SET_PROPERTY_STRING, data, &reply);

        return reply.readInt32();
    }

    virtual status_t setPropertyByteArray(String8 const &name,
                                          Vector<uint8_t> const &value) const {
        Parcel data, reply;
        data.writeInterfaceToken(IDrm::getInterfaceDescriptor());

        data.writeString8(name);
        data.writeInt32(value.size());
        data.write(value.array(), value.size());
        remote()->transact(SET_PROPERTY_BYTE_ARRAY, data, &reply);

        return reply.readInt32();
    }


private:
    DISALLOW_EVIL_CONSTRUCTORS(BpDrm);
};

IMPLEMENT_META_INTERFACE(Drm, "android.drm.IDrm");

////////////////////////////////////////////////////////////////////////////////

status_t BnDrm::onTransact(
    uint32_t code, const Parcel &data, Parcel *reply, uint32_t flags) {
    switch (code) {
        case INIT_CHECK:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            reply->writeInt32(initCheck());
            return OK;
        }

        case IS_CRYPTO_SUPPORTED:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            uint8_t uuid[16];
            data.read(uuid, sizeof(uuid));
            reply->writeInt32(isCryptoSchemeSupported(uuid));
            return OK;
        }

        case CREATE_PLUGIN:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            uint8_t uuid[16];
            data.read(uuid, sizeof(uuid));
            reply->writeInt32(createPlugin(uuid));
            return OK;
        }

        case DESTROY_PLUGIN:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            reply->writeInt32(destroyPlugin());
            return OK;
        }

        case OPEN_SESSION:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            status_t result = openSession(sessionId);
            reply->writeInt32(sessionId.size());
            reply->write(sessionId.array(), sessionId.size());
            reply->writeInt32(result);
            return OK;
        }

        case CLOSE_SESSION:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            uint32_t size = data.readInt32();
            sessionId.insertAt((size_t)0, size);
            data.read(sessionId.editArray(), size);
            reply->writeInt32(closeSession(sessionId));
            return OK;
        }

        case GET_LICENSE_REQUEST:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            uint32_t size = data.readInt32();
            sessionId.insertAt((size_t)0, size);
            data.read(sessionId.editArray(), size);

            Vector<uint8_t> initData;
            size = data.readInt32();
            initData.insertAt((size_t)0, size);
            data.read(initData.editArray(), size);

            String8 mimeType = data.readString8();
            DrmPlugin::LicenseType licenseType = (DrmPlugin::LicenseType)data.readInt32();

            KeyedVector<String8, String8> optionalParameters;
            uint32_t count = data.readInt32();
            for (size_t i = 0; i < count; ++i) {
                String8 key, value;
                key = data.readString8();
                value = data.readString8();
                optionalParameters.add(key, value);
            }

            Vector<uint8_t> request;
            String8 defaultUrl;

            status_t result = getLicenseRequest(sessionId, initData,
                                                mimeType, licenseType,
                                                optionalParameters,
                                                request, defaultUrl);
            reply->writeInt32(request.size());
            reply->write(request.array(), request.size());
            reply->writeString8(defaultUrl);
            reply->writeInt32(result);
            return OK;
        }

        case PROVIDE_LICENSE_RESPONSE:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            uint32_t size = data.readInt32();
            sessionId.insertAt((size_t)0, size);
            data.read(sessionId.editArray(), size);
            Vector<uint8_t> response;
            size = data.readInt32();
            response.insertAt((size_t)0, size);
            data.read(response.editArray(), size);

            reply->writeInt32(provideLicenseResponse(sessionId, response));
            return OK;
        }

        case REMOVE_LICENSE:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            uint32_t size = data.readInt32();
            sessionId.insertAt((size_t)0, size);
            data.read(sessionId.editArray(), size);
            reply->writeInt32(removeLicense(sessionId));
            return OK;
        }

        case QUERY_LICENSE_STATUS:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            uint32_t size = data.readInt32();
            sessionId.insertAt((size_t)0, size);
            data.read(sessionId.editArray(), size);
            KeyedVector<String8, String8> infoMap;

            status_t result = queryLicenseStatus(sessionId, infoMap);

            size_t count = infoMap.size();
            reply->writeInt32(count);
            for (size_t i = 0; i < count; ++i) {
                reply->writeString8(infoMap.keyAt(i));
                reply->writeString8(infoMap.valueAt(i));
            }
            reply->writeInt32(result);
            return OK;
        }

        case GET_PROVISION_REQUEST:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> request;
            String8 defaultUrl;
            status_t result = getProvisionRequest(request, defaultUrl);
            reply->writeInt32(request.size());
            reply->write(request.array(), request.size());
            reply->writeString8(defaultUrl);
            reply->writeInt32(result);
            return OK;
        }

        case PROVIDE_PROVISION_RESPONSE:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> response;
            uint32_t size = data.readInt32();
            response.insertAt((size_t)0, size);
            data.read(response.editArray(), size);
            reply->writeInt32(provideProvisionResponse(response));

            return OK;
        }

        case GET_SECURE_STOPS:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            List<Vector<uint8_t> > secureStops;
            status_t result = getSecureStops(secureStops);
            size_t count = secureStops.size();
            reply->writeInt32(count);
            List<Vector<uint8_t> >::iterator iter = secureStops.begin();
            while(iter != secureStops.end()) {
                size_t size = iter->size();
                reply->writeInt32(size);
                reply->write(iter->array(), iter->size());
            }
            reply->writeInt32(result);
            return OK;
        }

        case RELEASE_SECURE_STOPS:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> ssRelease;
            uint32_t size = data.readInt32();
            ssRelease.insertAt((size_t)0, size);
            data.read(ssRelease.editArray(), size);
            reply->writeInt32(releaseSecureStops(ssRelease));
            return OK;
        }

        case GET_PROPERTY_STRING:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            String8 name = data.readString8();
            String8 value;
            status_t result = getPropertyString(name, value);
            reply->writeString8(value);
            reply->writeInt32(result);
            return OK;
        }

        case GET_PROPERTY_BYTE_ARRAY:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            String8 name = data.readString8();
            Vector<uint8_t> value;
            status_t result = getPropertyByteArray(name, value);
            reply->writeInt32(value.size());
            reply->write(value.array(), value.size());
            reply->writeInt32(result);
            return OK;
        }

        case SET_PROPERTY_STRING:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            String8 name = data.readString8();
            String8 value = data.readString8();
            reply->writeInt32(setPropertyString(name, value));
            return OK;
        }

        case SET_PROPERTY_BYTE_ARRAY:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            String8 name = data.readString8();
            Vector<uint8_t> value;
            size_t count = data.readInt32();
            value.insertAt((size_t)0, count);
            data.read(value.editArray(), count);
            reply->writeInt32(setPropertyByteArray(name, value));
            return OK;
        }

        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}  // namespace android

