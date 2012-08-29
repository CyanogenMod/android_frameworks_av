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

#include <binder/IInterface.h>
#include <media/hardware/HDCPAPI.h>
#include <media/stagefright/foundation/ABase.h>

namespace android {

struct IHDCPObserver : public IInterface {
    DECLARE_META_INTERFACE(HDCPObserver);

    virtual void notify(
            int msg, int ext1, int ext2, const Parcel *obj) = 0;

private:
    DISALLOW_EVIL_CONSTRUCTORS(IHDCPObserver);
};

struct IHDCP : public IInterface {
    DECLARE_META_INTERFACE(HDCP);

    // Called to specify the observer that receives asynchronous notifications
    // from the HDCP implementation to signal completion/failure of asynchronous
    // operations (such as initialization) or out of band events.
    virtual status_t setObserver(const sp<IHDCPObserver> &observer) = 0;

    // Request to setup an HDCP session with the specified host listening
    // on the specified port.
    virtual status_t initAsync(const char *host, unsigned port) = 0;

    // Request to shutdown the active HDCP session.
    virtual status_t shutdownAsync() = 0;

    // Encrypt a data according to the HDCP spec. The data is to be
    // encrypted in-place, only size bytes of data should be read/write,
    // even if the size is not a multiple of 128 bit (16 bytes).
    // This operation is to be synchronous, i.e. this call does not return
    // until outData contains size bytes of encrypted data.
    // streamCTR will be assigned by the caller (to 0 for the first PES stream,
    // 1 for the second and so on)
    // inputCTR will be maintained by the callee for each PES stream.
    virtual status_t encrypt(
            const void *inData, size_t size, uint32_t streamCTR,
            uint64_t *outInputCTR, void *outData) = 0;

private:
    DISALLOW_EVIL_CONSTRUCTORS(IHDCP);
};

struct BnHDCPObserver : public BnInterface<IHDCPObserver> {
    virtual status_t onTransact(
            uint32_t code, const Parcel &data, Parcel *reply,
            uint32_t flags = 0);
};

struct BnHDCP : public BnInterface<IHDCP> {
    virtual status_t onTransact(
            uint32_t code, const Parcel &data, Parcel *reply,
            uint32_t flags = 0);
};

}  // namespace android


