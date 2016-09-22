/*
 * Copyright (C) 2012 The Android Open Source Project
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
#define LOG_TAG "ICrypto"
#include <utils/Log.h>

#include <binder/Parcel.h>
#include <binder/IMemory.h>
#include <media/ICrypto.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>

namespace android {

enum {
    INIT_CHECK = IBinder::FIRST_CALL_TRANSACTION,
    IS_CRYPTO_SUPPORTED,
    CREATE_PLUGIN,
    DESTROY_PLUGIN,
    REQUIRES_SECURE_COMPONENT,
    DECRYPT,
    NOTIFY_RESOLUTION,
    SET_MEDIADRM_SESSION,
};

struct BpCrypto : public BpInterface<ICrypto> {
    BpCrypto(const sp<IBinder> &impl)
        : BpInterface<ICrypto>(impl) {
    }

    virtual status_t initCheck() const {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        remote()->transact(INIT_CHECK, data, &reply);

        return reply.readInt32();
    }

    virtual bool isCryptoSchemeSupported(const uint8_t uuid[16]) {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        data.write(uuid, 16);
        remote()->transact(IS_CRYPTO_SUPPORTED, data, &reply);

        return reply.readInt32() != 0;
    }

    virtual status_t createPlugin(
            const uint8_t uuid[16], const void *opaqueData, size_t opaqueSize) {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        data.write(uuid, 16);
        data.writeInt32(opaqueSize);

        if (opaqueSize > 0) {
            data.write(opaqueData, opaqueSize);
        }

        remote()->transact(CREATE_PLUGIN, data, &reply);

        return reply.readInt32();
    }

    virtual status_t destroyPlugin() {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        remote()->transact(DESTROY_PLUGIN, data, &reply);

        return reply.readInt32();
    }

    virtual bool requiresSecureDecoderComponent(
            const char *mime) const {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        data.writeCString(mime);
        remote()->transact(REQUIRES_SECURE_COMPONENT, data, &reply);

        return reply.readInt32() != 0;
    }

    virtual ssize_t decrypt(
            DestinationType dstType,
            const uint8_t key[16],
            const uint8_t iv[16],
            CryptoPlugin::Mode mode, const CryptoPlugin::Pattern &pattern,
            const sp<IMemory> &sharedBuffer, size_t offset,
            const CryptoPlugin::SubSample *subSamples, size_t numSubSamples,
            void *dstPtr,
            AString *errorDetailMsg) {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        data.writeInt32((int32_t)dstType);
        data.writeInt32(mode);
        data.writeInt32(pattern.mEncryptBlocks);
        data.writeInt32(pattern.mSkipBlocks);

        static const uint8_t kDummy[16] = { 0 };

        if (key == NULL) {
            key = kDummy;
        }

        if (iv == NULL) {
            iv = kDummy;
        }

        data.write(key, 16);
        data.write(iv, 16);

        size_t totalSize = 0;
        for (size_t i = 0; i < numSubSamples; ++i) {
            totalSize += subSamples[i].mNumBytesOfEncryptedData;
            totalSize += subSamples[i].mNumBytesOfClearData;
        }

        data.writeInt32(totalSize);
        data.writeStrongBinder(IInterface::asBinder(sharedBuffer));
        data.writeInt32(offset);

        data.writeInt32(numSubSamples);
        data.write(subSamples, sizeof(CryptoPlugin::SubSample) * numSubSamples);

        if (dstType == kDestinationTypeNativeHandle) {
            data.writeNativeHandle(static_cast<native_handle_t *>(dstPtr));
        } else if (dstType == kDestinationTypeOpaqueHandle) {
            data.writeInt64(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dstPtr)));
        } else {
            dstType = kDestinationTypeVmPointer;
        }

        remote()->transact(DECRYPT, data, &reply);

        ssize_t result = reply.readInt32();

        if (isCryptoError(result)) {
            errorDetailMsg->setTo(reply.readCString());
        } else if (dstType == kDestinationTypeVmPointer) {
            // For the non-secure case, copy the decrypted
            // data from shared memory to its final destination
            memcpy(dstPtr, sharedBuffer->pointer(), result);
        }

        return result;
    }

    virtual void notifyResolution(
        uint32_t width, uint32_t height) {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());
        data.writeInt32(width);
        data.writeInt32(height);
        remote()->transact(NOTIFY_RESOLUTION, data, &reply);
    }

    virtual status_t setMediaDrmSession(const Vector<uint8_t> &sessionId) {
        Parcel data, reply;
        data.writeInterfaceToken(ICrypto::getInterfaceDescriptor());

        writeVector(data, sessionId);
        remote()->transact(SET_MEDIADRM_SESSION, data, &reply);

        return reply.readInt32();
    }

private:
    void readVector(Parcel &reply, Vector<uint8_t> &vector) const {
        uint32_t size = reply.readInt32();
        vector.insertAt((size_t)0, size);
        reply.read(vector.editArray(), size);
    }

    void writeVector(Parcel &data, Vector<uint8_t> const &vector) const {
        data.writeInt32(vector.size());
        data.write(vector.array(), vector.size());
    }

    DISALLOW_EVIL_CONSTRUCTORS(BpCrypto);
};

IMPLEMENT_META_INTERFACE(Crypto, "android.hardware.ICrypto");

////////////////////////////////////////////////////////////////////////////////

void BnCrypto::readVector(const Parcel &data, Vector<uint8_t> &vector) const {
    uint32_t size = data.readInt32();
    vector.insertAt((size_t)0, size);
    data.read(vector.editArray(), size);
}

void BnCrypto::writeVector(Parcel *reply, Vector<uint8_t> const &vector) const {
    reply->writeInt32(vector.size());
    reply->write(vector.array(), vector.size());
}

status_t BnCrypto::onTransact(
    uint32_t code, const Parcel &data, Parcel *reply, uint32_t flags) {
    switch (code) {
        case INIT_CHECK:
        {
            CHECK_INTERFACE(ICrypto, data, reply);
            reply->writeInt32(initCheck());

            return OK;
        }

        case IS_CRYPTO_SUPPORTED:
        {
            CHECK_INTERFACE(ICrypto, data, reply);
            uint8_t uuid[16];
            data.read(uuid, sizeof(uuid));
            reply->writeInt32(isCryptoSchemeSupported(uuid));

            return OK;
        }

        case CREATE_PLUGIN:
        {
            CHECK_INTERFACE(ICrypto, data, reply);

            uint8_t uuid[16];
            data.read(uuid, sizeof(uuid));

            size_t opaqueSize = data.readInt32();
            void *opaqueData = NULL;

            if (opaqueSize > 0) {
                opaqueData = malloc(opaqueSize);
                data.read(opaqueData, opaqueSize);
            }

            reply->writeInt32(createPlugin(uuid, opaqueData, opaqueSize));

            if (opaqueData != NULL) {
                free(opaqueData);
                opaqueData = NULL;
            }

            return OK;
        }

        case DESTROY_PLUGIN:
        {
            CHECK_INTERFACE(ICrypto, data, reply);
            reply->writeInt32(destroyPlugin());

            return OK;
        }

        case REQUIRES_SECURE_COMPONENT:
        {
            CHECK_INTERFACE(ICrypto, data, reply);

            const char *mime = data.readCString();
            if (mime == NULL) {
                reply->writeInt32(BAD_VALUE);
            } else {
                reply->writeInt32(requiresSecureDecoderComponent(mime));
            }

            return OK;
        }

        case DECRYPT:
        {
            CHECK_INTERFACE(ICrypto, data, reply);

            DestinationType dstType = (DestinationType)data.readInt32();
            CryptoPlugin::Mode mode = (CryptoPlugin::Mode)data.readInt32();
            CryptoPlugin::Pattern pattern;
            pattern.mEncryptBlocks = data.readInt32();
            pattern.mSkipBlocks = data.readInt32();

            uint8_t key[16];
            data.read(key, sizeof(key));

            uint8_t iv[16];
            data.read(iv, sizeof(iv));

            size_t totalSize = data.readInt32();
            sp<IMemory> sharedBuffer =
                interface_cast<IMemory>(data.readStrongBinder());
            if (sharedBuffer == NULL) {
                reply->writeInt32(BAD_VALUE);
                return OK;
            }
            int32_t offset = data.readInt32();

            int32_t numSubSamples = data.readInt32();

            CryptoPlugin::SubSample *subSamples =
                new CryptoPlugin::SubSample[numSubSamples];

            data.read(
                    subSamples,
                    sizeof(CryptoPlugin::SubSample) * numSubSamples);

            native_handle_t *nativeHandle = NULL;
            void *secureBufferId = NULL, *dstPtr;
            if (dstType == kDestinationTypeNativeHandle) {
                nativeHandle = data.readNativeHandle();
                dstPtr = static_cast<void *>(nativeHandle);
            } else if (dstType == kDestinationTypeOpaqueHandle) {
                secureBufferId = reinterpret_cast<void *>(static_cast<uintptr_t>(data.readInt64()));
                dstPtr = secureBufferId;
            } else {
                dstType = kDestinationTypeVmPointer;
                dstPtr = malloc(totalSize);
            }

            AString errorDetailMsg;
            ssize_t result;

            size_t sumSubsampleSizes = 0;
            bool overflow = false;
            for (int32_t i = 0; i < numSubSamples; ++i) {
                CryptoPlugin::SubSample &ss = subSamples[i];
                if (sumSubsampleSizes <= SIZE_MAX - ss.mNumBytesOfEncryptedData) {
                    sumSubsampleSizes += ss.mNumBytesOfEncryptedData;
                } else {
                    overflow = true;
                }
                if (sumSubsampleSizes <= SIZE_MAX - ss.mNumBytesOfClearData) {
                    sumSubsampleSizes += ss.mNumBytesOfClearData;
                } else {
                    overflow = true;
                }
            }

            if (overflow || sumSubsampleSizes != totalSize) {
                result = -EINVAL;
            } else if (totalSize > sharedBuffer->size()) {
                result = -EINVAL;
            } else if ((size_t)offset > sharedBuffer->size() - totalSize) {
                result = -EINVAL;
            } else {
                result = decrypt(
                    dstType,
                    key,
                    iv,
                    mode, pattern,
                    sharedBuffer, offset,
                    subSamples, numSubSamples,
                    dstPtr,
                    &errorDetailMsg);
            }

            reply->writeInt32(result);

            if (isCryptoError(result)) {
                reply->writeCString(errorDetailMsg.c_str());
            }

            if (dstType == kDestinationTypeVmPointer) {
                if (result >= 0) {
                    CHECK_LE(result, static_cast<ssize_t>(totalSize));
                    // For the non-secure case, pass the decrypted
                    // data back via the shared buffer rather than
                    // copying it separately over binder to avoid
                    // binder's 1MB limit.
                    memcpy(sharedBuffer->pointer(), dstPtr, totalSize);
                }
                free(dstPtr);
                dstPtr = NULL;
            } else if (dstType == kDestinationTypeNativeHandle) {
                int err;
                if ((err = native_handle_close(nativeHandle)) < 0) {
                    ALOGW("secure buffer native_handle_close failed: %d", err);
                }
                if ((err = native_handle_delete(nativeHandle)) < 0) {
                    ALOGW("secure buffer native_handle_delete failed: %d", err);
                }
            }

            delete[] subSamples;
            subSamples = NULL;

            return OK;
        }

        case NOTIFY_RESOLUTION:
        {
            CHECK_INTERFACE(ICrypto, data, reply);

            int32_t width = data.readInt32();
            int32_t height = data.readInt32();
            notifyResolution(width, height);

            return OK;
        }

        case SET_MEDIADRM_SESSION:
        {
            CHECK_INTERFACE(IDrm, data, reply);
            Vector<uint8_t> sessionId;
            readVector(data, sessionId);
            reply->writeInt32(setMediaDrmSession(sessionId));
            return OK;
        }

        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}  // namespace android
