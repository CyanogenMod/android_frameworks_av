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

#ifndef CLEARKEY_DRM_PLUGIN_H_
#define CLEARKEY_DRM_PLUGIN_H_

#include <media/drm/DrmAPI.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/MediaErrors.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/List.h>
#include <utils/String8.h>
#include <utils/Vector.h>

#include "SessionLibrary.h"
#include "Utils.h"

namespace clearkeydrm {

using android::KeyedVector;
using android::List;
using android::status_t;
using android::String8;
using android::Vector;

class DrmPlugin : public android::DrmPlugin {
public:
    DrmPlugin(SessionLibrary* sessionLibrary)
            : mSessionLibrary(sessionLibrary) {}
    virtual ~DrmPlugin() {}

    virtual status_t openSession(Vector<uint8_t>& sessionId);

    virtual status_t closeSession(const Vector<uint8_t>& sessionId);

    virtual status_t getKeyRequest(
            const Vector<uint8_t>& scope,
            const Vector<uint8_t>& initData,
            const String8& initDataType,
            KeyType keyType,
            const KeyedVector<String8, String8>& optionalParameters,
            Vector<uint8_t>& request,
            String8& defaultUrl);

    virtual status_t provideKeyResponse(
            const Vector<uint8_t>& scope,
            const Vector<uint8_t>& response,
            Vector<uint8_t>& keySetId);

    virtual status_t removeKeys(const Vector<uint8_t>& sessionId) {
        UNUSED(sessionId);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t restoreKeys(
            const Vector<uint8_t>& sessionId,
            const Vector<uint8_t>& keySetId) {
        UNUSED(sessionId);
        UNUSED(keySetId);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t queryKeyStatus(
            const Vector<uint8_t>& sessionId,
            KeyedVector<String8, String8>& infoMap) const {
        UNUSED(sessionId);
        UNUSED(infoMap);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t getProvisionRequest(
            const String8& cert_type,
            const String8& cert_authority,
            Vector<uint8_t>& request,
            String8& defaultUrl) {
        UNUSED(cert_type);
        UNUSED(cert_authority);
        UNUSED(request);
        UNUSED(defaultUrl);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t provideProvisionResponse(
            const Vector<uint8_t>& response,
            Vector<uint8_t>& certificate,
            Vector<uint8_t>& wrappedKey) {
        UNUSED(response);
        UNUSED(certificate);
        UNUSED(wrappedKey);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t unprovisionDevice() {
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t getSecureStops(List<Vector<uint8_t> >& secureStops) {
        UNUSED(secureStops);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t releaseSecureStops(const Vector<uint8_t>& ssRelease) {
        UNUSED(ssRelease);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t getPropertyString(
            const String8& name, String8& value) const;

    virtual status_t getPropertyByteArray(
            const String8& name, Vector<uint8_t>& value) const {
        UNUSED(name);
        UNUSED(value);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t setPropertyString(
            const String8& name, const String8& value) {
        UNUSED(name);
        UNUSED(value);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t setPropertyByteArray(
            const String8& name, const Vector<uint8_t>& value) {
        UNUSED(name);
        UNUSED(value);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t setCipherAlgorithm(
            const Vector<uint8_t>& sessionId, const String8& algorithm) {
        UNUSED(sessionId);
        UNUSED(algorithm);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t setMacAlgorithm(
            const Vector<uint8_t>& sessionId, const String8& algorithm) {
        UNUSED(sessionId);
        UNUSED(algorithm);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t encrypt(
            const Vector<uint8_t>& sessionId,
            const Vector<uint8_t>& keyId,
            const Vector<uint8_t>& input,
            const Vector<uint8_t>& iv,
            Vector<uint8_t>& output) {
        UNUSED(sessionId);
        UNUSED(keyId);
        UNUSED(input);
        UNUSED(iv);
        UNUSED(output);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t decrypt(
            const Vector<uint8_t>& sessionId,
            const Vector<uint8_t>& keyId,
            const Vector<uint8_t>& input,
            const Vector<uint8_t>& iv,
            Vector<uint8_t>& output) {
        UNUSED(sessionId);
        UNUSED(keyId);
        UNUSED(input);
        UNUSED(iv);
        UNUSED(output);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t sign(
            const Vector<uint8_t>& sessionId,
            const Vector<uint8_t>& keyId,
            const Vector<uint8_t>& message,
            Vector<uint8_t>& signature) {
        UNUSED(sessionId);
        UNUSED(keyId);
        UNUSED(message);
        UNUSED(signature);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t verify(
            const Vector<uint8_t>& sessionId,
            const Vector<uint8_t>& keyId,
            const Vector<uint8_t>& message,
            const Vector<uint8_t>& signature, bool& match) {
        UNUSED(sessionId);
        UNUSED(keyId);
        UNUSED(message);
        UNUSED(signature);
        UNUSED(match);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

    virtual status_t signRSA(
            const Vector<uint8_t>& sessionId,
            const String8& algorithm,
            const Vector<uint8_t>& message,
            const Vector<uint8_t>& wrappedKey,
            Vector<uint8_t>& signature) {
        UNUSED(sessionId);
        UNUSED(algorithm);
        UNUSED(message);
        UNUSED(wrappedKey);
        UNUSED(signature);
        return android::ERROR_DRM_CANNOT_HANDLE;
    }

private:
    DISALLOW_EVIL_CONSTRUCTORS(DrmPlugin);

    SessionLibrary* mSessionLibrary;
};

} // namespace clearkeydrm

#endif // CLEARKEY_DRM_PLUGIN_H_
