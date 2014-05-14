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

#define LOG_NDEBUG 0
#define LOG_TAG "NdkMediaDrm"

#include "NdkMediaDrm.h"

#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <gui/Surface.h>

#include <media/IDrm.h>
#include <media/IDrmClient.h>
#include <media/stagefright/MediaErrors.h>
#include <binder/IServiceManager.h>
#include <media/IMediaPlayerService.h>
#include <ndk/NdkMediaCrypto.h>


using namespace android;

typedef Vector<uint8_t> idvec_t;

struct AMediaDrm {
    sp<IDrm> mDrm;
    sp<IDrmClient> mDrmClient;
    AMediaDrmEventListener mListener;
    List<idvec_t> mIds;
    KeyedVector<String8, String8> mQueryResults;
    Vector<uint8_t> mKeyRequest;
    Vector<uint8_t> mProvisionRequest;
    String8 mProvisionUrl;
    String8 mPropertyString;
    Vector<uint8_t> mPropertyByteArray;
    List<Vector<uint8_t> > mSecureStops;
};

extern "C" {

static mediadrm_status_t translateStatus(status_t status) {
    mediadrm_status_t result = MEDIADRM_UNKNOWN_ERROR;
    switch (status) {
        case OK:
            result = MEDIADRM_OK;
            break;
        case android::ERROR_DRM_NOT_PROVISIONED:
            result = MEDIADRM_NOT_PROVISIONED_ERROR;
            break;
        case android::ERROR_DRM_RESOURCE_BUSY:
            result = MEDIADRM_RESOURCE_BUSY_ERROR;
            break;
        case android::ERROR_DRM_DEVICE_REVOKED:
            result = MEDIADRM_DEVICE_REVOKED_ERROR;
            break;
        case android::ERROR_DRM_CANNOT_HANDLE:
            result = MEDIADRM_INVALID_PARAMETER_ERROR;
            break;
        case android::ERROR_DRM_TAMPER_DETECTED:
            result = MEDIADRM_TAMPER_DETECTED_ERROR;
            break;
        case android::ERROR_DRM_SESSION_NOT_OPENED:
            result = MEDIADRM_SESSION_NOT_OPENED_ERROR;
            break;
        case android::ERROR_DRM_NO_LICENSE:
            result = MEDIADRM_NEED_KEY_ERROR;
            break;
        case android::ERROR_DRM_LICENSE_EXPIRED:
            result = MEDIADRM_LICENSE_EXPIRED_ERROR;
            break;
        default:
            result = MEDIADRM_UNKNOWN_ERROR;
            break;
    }
    return result;
}

static sp<IDrm> CreateDrm() {
    sp<IServiceManager> sm = defaultServiceManager();

    sp<IBinder> binder =
        sm->getService(String16("media.player"));

    sp<IMediaPlayerService> service =
        interface_cast<IMediaPlayerService>(binder);

    if (service == NULL) {
        return NULL;
    }

    sp<IDrm> drm = service->makeDrm();

    if (drm == NULL || (drm->initCheck() != OK && drm->initCheck() != NO_INIT)) {
        return NULL;
    }

    return drm;
}


static sp<IDrm> CreateDrmFromUUID(const AMediaUUID uuid) {
    sp<IDrm> drm = CreateDrm();

    if (drm == NULL) {
        return NULL;
    }

    status_t err = drm->createPlugin(uuid);

    if (err != OK) {
        return NULL;
    }

    return drm;
}

EXPORT
bool AMediaDrm_isCryptoSchemeSupported(const AMediaUUID uuid, const char *mimeType) {
    sp<IDrm> drm = CreateDrm();

    if (drm == NULL) {
        return false;
    }

    String8 mimeStr = mimeType ? String8(mimeType) : String8("");
    return drm->isCryptoSchemeSupported(uuid, mimeStr);
}

EXPORT
AMediaDrm* AMediaDrm_createByUUID(const AMediaUUID uuid) {
    AMediaDrm *mObj = new AMediaDrm();
    mObj->mDrm = CreateDrmFromUUID(uuid);
    return mObj;
}

EXPORT
void AMediaDrm_release(AMediaDrm *mObj) {
    if (mObj->mDrm != NULL) {
        mObj->mDrm->setListener(NULL);
        mObj->mDrm->destroyPlugin();
        mObj->mDrm.clear();
    }
    delete mObj;
}

#if 0
void AMediaDrm_setOnEventListener(AMediaDrm *mObj, AMediaDrmEventListener listener) {
    mObj->mListener = listener;
}
#endif


static bool findId(AMediaDrm *mObj, const AMediaDrmByteArray &id, List<idvec_t>::iterator &iter) {
    iter = mObj->mIds.begin();
    while (iter != mObj->mIds.end()) {
        if (iter->array() == id.ptr && iter->size() == id.length) {
            return true;
        }
    }
    return false;
}

EXPORT
mediadrm_status_t AMediaDrm_openSession(AMediaDrm *mObj, AMediaDrmSessionId &sessionId) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    Vector<uint8_t> session;
    status_t status = mObj->mDrm->openSession(session);
    if (status == OK) {
        mObj->mIds.push_front(session);
        List<idvec_t>::iterator iter = mObj->mIds.begin();
        sessionId.ptr = iter->array();
        sessionId.length = iter->size();
    }
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_closeSession(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }

    List<idvec_t>::iterator iter;
    if (!findId(mObj, sessionId, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }
    mObj->mDrm->closeSession(*iter);
    mObj->mIds.erase(iter);
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_getKeyRequest(AMediaDrm *mObj, const AMediaDrmScope &scope,
        const uint8_t *init, size_t initSize, const char *mimeType, AMediaDrmKeyType keyType,
        const AMediaDrmKeyValue *optionalParameters, size_t numOptionalParameters,
        const uint8_t *&keyRequest, size_t &keyRequestSize) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    if (!mimeType) {
        return MEDIADRM_INVALID_PARAMETER_ERROR;
    }

    List<idvec_t>::iterator iter;
    if (!findId(mObj, scope, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }

    Vector<uint8_t> mdInit;
    mdInit.appendArray(init, initSize);
    DrmPlugin::KeyType mdKeyType;
    switch (keyType) {
        case KEY_TYPE_STREAMING:
            mdKeyType = DrmPlugin::kKeyType_Streaming;
            break;
        case KEY_TYPE_OFFLINE:
            mdKeyType = DrmPlugin::kKeyType_Offline;
            break;
        case KEY_TYPE_RELEASE:
            mdKeyType = DrmPlugin::kKeyType_Release;
            break;
        default:
            return MEDIADRM_INVALID_PARAMETER_ERROR;
    }
    KeyedVector<String8, String8> mdOptionalParameters;
    for (size_t i = 0; i < numOptionalParameters; i++) {
        mdOptionalParameters.add(String8(optionalParameters[i].mKey),
                String8(optionalParameters[i].mValue));
    }
    String8 defaultUrl;
    status_t status = mObj->mDrm->getKeyRequest(*iter, mdInit, String8(mimeType),
            mdKeyType, mdOptionalParameters, mObj->mKeyRequest, defaultUrl);
    if (status != OK) {
        return translateStatus(status);
    } else {
        keyRequest = mObj->mKeyRequest.array();
        keyRequestSize = mObj->mKeyRequest.size();
    }
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_provideKeyResponse(AMediaDrm *mObj, const AMediaDrmScope &scope,
        const uint8_t *response, size_t responseSize, AMediaDrmKeySetId &keySetId) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    if (!response || !responseSize) {
        return MEDIADRM_INVALID_PARAMETER_ERROR;
    }

    List<idvec_t>::iterator iter;
    if (!findId(mObj, scope, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }
    Vector<uint8_t> mdResponse;
    mdResponse.appendArray(response, responseSize);

    Vector<uint8_t> mdKeySetId;
    status_t status = mObj->mDrm->provideKeyResponse(*iter, mdResponse, mdKeySetId);
    if (status == OK) {
        mObj->mIds.push_front(mdKeySetId);
        List<idvec_t>::iterator iter = mObj->mIds.begin();
        keySetId.ptr = iter->array();
        keySetId.length = iter->size();
    } else {
        keySetId.ptr = NULL;
        keySetId.length = 0;
    }
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_restoreKeys(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId,
        const AMediaDrmKeySetId &keySetId) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    List<idvec_t>::iterator iter;
    if (!findId(mObj, sessionId, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }
    Vector<uint8_t> keySet;
    keySet.appendArray(keySetId.ptr, keySetId.length);
    return translateStatus(mObj->mDrm->restoreKeys(*iter, keySet));
}

EXPORT
mediadrm_status_t AMediaDrm_removeKeys(AMediaDrm *mObj, const AMediaDrmSessionId &keySetId) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    List<idvec_t>::iterator iter;
    status_t status;
    if (!findId(mObj, keySetId, iter)) {
        Vector<uint8_t> keySet;
        keySet.appendArray(keySetId.ptr, keySetId.length);
        status = mObj->mDrm->removeKeys(keySet);
    } else {
        status = mObj->mDrm->removeKeys(*iter);
        mObj->mIds.erase(iter);
    }
    return translateStatus(status);
}

EXPORT
mediadrm_status_t AMediaDrm_queryKeyStatus(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId,
        AMediaDrmKeyValue *keyValuePairs, size_t &numPairs) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    List<idvec_t>::iterator iter;
    if (!findId(mObj, sessionId, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }

    status_t status = mObj->mDrm->queryKeyStatus(*iter, mObj->mQueryResults);
    if (status != OK) {
        numPairs = 0;
        return translateStatus(status);
    }

    if (mObj->mQueryResults.size() > numPairs) {
        numPairs = mObj->mQueryResults.size();
        return MEDIADRM_SHORT_BUFFER;
    }

    for (size_t i = 0; i < mObj->mQueryResults.size(); i++) {
        keyValuePairs[i].mKey = mObj->mQueryResults.keyAt(i).string();
        keyValuePairs[i].mValue = mObj->mQueryResults.keyAt(i).string();
    }
    numPairs = mObj->mQueryResults.size();
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_getProvisionRequest(AMediaDrm *mObj, const uint8_t *&provisionRequest,
        size_t &provisionRequestSize, const char *&serverUrl) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    if (!provisionRequestSize || !serverUrl) {
        return MEDIADRM_INVALID_PARAMETER_ERROR;
    }

    status_t status = mObj->mDrm->getProvisionRequest(String8(""), String8(""),
            mObj->mProvisionRequest, mObj->mProvisionUrl);
    if (status != OK) {
        return translateStatus(status);
    } else {
        provisionRequest = mObj->mProvisionRequest.array();
        provisionRequestSize = mObj->mProvisionRequest.size();
        serverUrl = mObj->mProvisionUrl.string();
    }
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_provideProvisionResponse(AMediaDrm *mObj,
        const uint8_t *response, size_t responseSize) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    if (!response || !responseSize) {
        return MEDIADRM_INVALID_PARAMETER_ERROR;
    }

    Vector<uint8_t> mdResponse;
    mdResponse.appendArray(response, responseSize);

    Vector<uint8_t> unused;
    return translateStatus(mObj->mDrm->provideProvisionResponse(mdResponse, unused, unused));
}

EXPORT
mediadrm_status_t AMediaDrm_getSecureStops(AMediaDrm *mObj,
        AMediaDrmSecureStop *secureStops, size_t &numSecureStops) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    status_t status = mObj->mDrm->getSecureStops(mObj->mSecureStops);
    if (status != OK) {
        numSecureStops = 0;
        return translateStatus(status);
    }
    if (numSecureStops < mObj->mSecureStops.size()) {
        return MEDIADRM_SHORT_BUFFER;
    }
    List<Vector<uint8_t> >::iterator iter = mObj->mSecureStops.begin();
    size_t i = 0;
    while (iter != mObj->mSecureStops.end()) {
        secureStops[i].ptr = iter->array();
        secureStops[i].length = iter->size();
        ++iter;
        ++i;
    }
    numSecureStops = mObj->mSecureStops.size();
    return MEDIADRM_OK;
}

EXPORT
mediadrm_status_t AMediaDrm_releaseSecureStops(AMediaDrm *mObj,
        const AMediaDrmSecureStop &ssRelease) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }

    Vector<uint8_t> release;
    release.appendArray(ssRelease.ptr, ssRelease.length);
    return translateStatus(mObj->mDrm->releaseSecureStops(release));
}


EXPORT
mediadrm_status_t AMediaDrm_getPropertyString(AMediaDrm *mObj, const char *propertyName,
        const char *&propertyValue) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }

    status_t status = mObj->mDrm->getPropertyString(String8(propertyName),
            mObj->mPropertyString);

    if (status == OK) {
        propertyValue = mObj->mPropertyString.string();
    } else {
        propertyValue = NULL;
    }
    return translateStatus(status);
}

EXPORT
mediadrm_status_t AMediaDrm_getPropertyByteArray(AMediaDrm *mObj,
        const char *propertyName, AMediaDrmByteArray &propertyValue) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }

    status_t status = mObj->mDrm->getPropertyByteArray(String8(propertyName),
            mObj->mPropertyByteArray);

    if (status == OK) {
        propertyValue.ptr = mObj->mPropertyByteArray.array();
        propertyValue.length = mObj->mPropertyByteArray.size();
    } else {
        propertyValue.ptr = NULL;
        propertyValue.length = 0;
    }
    return translateStatus(status);
}

EXPORT
mediadrm_status_t AMediaDrm_setPropertyString(AMediaDrm *mObj,
        const char *propertyName, const char *value) {
    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }

    return translateStatus(mObj->mDrm->setPropertyString(String8(propertyName),
                    String8(value)));
}

EXPORT
mediadrm_status_t AMediaDrm_setPropertyByteArray(AMediaDrm *mObj,
        const char *propertyName, const uint8_t *value, size_t valueSize) {

    Vector<uint8_t> byteArray;
    byteArray.appendArray(value, valueSize);

    return translateStatus(mObj->mDrm->getPropertyByteArray(String8(propertyName),
                    byteArray));
}


static mediadrm_status_t encrypt_decrypt_common(AMediaDrm *mObj,
        const AMediaDrmSessionId &sessionId,
        const char *cipherAlgorithm, uint8_t *keyId, uint8_t *iv,
        const uint8_t *input, uint8_t *output, size_t dataSize, bool encrypt) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    List<idvec_t>::iterator iter;
    if (!findId(mObj, sessionId, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }

    status_t status = mObj->mDrm->setCipherAlgorithm(*iter, String8(cipherAlgorithm));
    if (status != OK) {
        return translateStatus(status);
    }

    Vector<uint8_t> keyIdVec;
    const size_t kKeyIdSize = 16;
    keyIdVec.appendArray(keyId, kKeyIdSize);

    Vector<uint8_t> inputVec;
    inputVec.appendArray(input, dataSize);

    Vector<uint8_t> ivVec;
    const size_t kIvSize = 16;
    ivVec.appendArray(iv, kIvSize);

    Vector<uint8_t> outputVec;
    if (encrypt) {
        status_t status = mObj->mDrm->encrypt(*iter, keyIdVec, inputVec, ivVec, outputVec);
    } else {
        status_t status = mObj->mDrm->decrypt(*iter, keyIdVec, inputVec, ivVec, outputVec);
    }
    if (status == OK) {
        memcpy(output, outputVec.array(), outputVec.size());
    }
    return translateStatus(status);
}

EXPORT
mediadrm_status_t AMediaDrm_encrypt(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId,
        const char *cipherAlgorithm, uint8_t *keyId, uint8_t *iv,
        const uint8_t *input, uint8_t *output, size_t dataSize) {
    return encrypt_decrypt_common(mObj, sessionId, cipherAlgorithm, keyId, iv,
            input, output, dataSize, true);
}

EXPORT
mediadrm_status_t AMediaDrm_decrypt(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId,
        const char *cipherAlgorithm, uint8_t *keyId, uint8_t *iv,
        const uint8_t *input, uint8_t *output, size_t dataSize) {
    return encrypt_decrypt_common(mObj, sessionId, cipherAlgorithm, keyId, iv,
            input, output, dataSize, false);
}

EXPORT
mediadrm_status_t AMediaDrm_sign(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId,
        const char *macAlgorithm, uint8_t *keyId, uint8_t *message, size_t messageSize,
        uint8_t *signature, size_t *signatureSize) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    List<idvec_t>::iterator iter;
    if (!findId(mObj, sessionId, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }

    status_t status = mObj->mDrm->setMacAlgorithm(*iter, String8(macAlgorithm));
    if (status != OK) {
        return translateStatus(status);
    }

    Vector<uint8_t> keyIdVec;
    const size_t kKeyIdSize = 16;
    keyIdVec.appendArray(keyId, kKeyIdSize);

    Vector<uint8_t> messageVec;
    messageVec.appendArray(message, messageSize);

    Vector<uint8_t> signatureVec;
    status = mObj->mDrm->sign(*iter, keyIdVec, messageVec, signatureVec);
    if (signatureVec.size() > *signatureSize) {
        return MEDIADRM_SHORT_BUFFER;
    }
    if (status == OK) {
        memcpy(signature, signatureVec.array(), signatureVec.size());
    }
    return translateStatus(status);
}

EXPORT
mediadrm_status_t AMediaDrm_verify(AMediaDrm *mObj, const AMediaDrmSessionId &sessionId,
        const char *macAlgorithm, uint8_t *keyId, const uint8_t *message, size_t messageSize,
        const uint8_t *signature, size_t signatureSize) {

    if (!mObj || mObj->mDrm == NULL) {
        return MEDIADRM_INVALID_OBJECT_ERROR;
    }
    List<idvec_t>::iterator iter;
    if (!findId(mObj, sessionId, iter)) {
        return MEDIADRM_SESSION_NOT_OPENED_ERROR;
    }

    status_t status = mObj->mDrm->setMacAlgorithm(*iter, String8(macAlgorithm));
    if (status != OK) {
        return translateStatus(status);
    }

    Vector<uint8_t> keyIdVec;
    const size_t kKeyIdSize = 16;
    keyIdVec.appendArray(keyId, kKeyIdSize);

    Vector<uint8_t> messageVec;
    messageVec.appendArray(message, messageSize);

    Vector<uint8_t> signatureVec;
    signatureVec.appendArray(signature, signatureSize);

    bool match;
    status = mObj->mDrm->verify(*iter, keyIdVec, messageVec, signatureVec, match);
    if (status == OK) {
        return match ? MEDIADRM_OK : MEDIADRM_VERIFY_FAILED;
    }
    return translateStatus(status);
}

} // extern "C"

