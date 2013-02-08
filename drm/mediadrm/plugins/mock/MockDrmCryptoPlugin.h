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

#include <utils/Mutex.h>

#include "drm/DrmAPI.h"
#include "hardware/CryptoAPI.h"

extern "C" {
      android::DrmFactory *createDrmFactory();
      android::CryptoFactory *createCryptoFactory();
}

namespace android {

    class MockDrmFactory : public DrmFactory {
    public:
        MockDrmFactory() {}
        virtual ~MockDrmFactory() {}

        bool isCryptoSchemeSupported(const uint8_t uuid[16]);
        status_t createDrmPlugin(const uint8_t uuid[16], DrmPlugin **plugin);
    };

    class MockCryptoFactory : public CryptoFactory {
    public:
        MockCryptoFactory() {}
        virtual ~MockCryptoFactory() {}

        bool isCryptoSchemeSupported(const uint8_t uuid[16]) const;
        status_t createPlugin(
            const uint8_t uuid[16], const void *data, size_t size,
            CryptoPlugin **plugin);
    };



    class MockDrmPlugin : public DrmPlugin {
    public:
        MockDrmPlugin() {}
        virtual ~MockDrmPlugin() {}

        // from DrmPlugin
        status_t openSession(Vector<uint8_t> &sessionId);
        status_t closeSession(Vector<uint8_t> const &sessionId);

        status_t
            getLicenseRequest(Vector<uint8_t> const &sessionId,
                              Vector<uint8_t> const &initData,
                              String8 const &mimeType, LicenseType licenseType,
                              KeyedVector<String8, String8> const &optionalParameters,
                              Vector<uint8_t> &request, String8 &defaultUrl);

        status_t provideLicenseResponse(Vector<uint8_t> const &sessionId,
                                                Vector<uint8_t> const &response);

        status_t removeLicense(Vector<uint8_t> const &sessionId);

        status_t
            queryLicenseStatus(Vector<uint8_t> const &sessionId,
                               KeyedVector<String8, String8> &infoMap) const;

        status_t getProvisionRequest(Vector<uint8_t> &request,
                                             String8 &defaultUrl);

        status_t provideProvisionResponse(Vector<uint8_t> const &response);

        status_t getSecureStops(List<Vector<uint8_t> > &secureStops);
        status_t releaseSecureStops(Vector<uint8_t> const &ssRelease);

        status_t getPropertyString(String8 const &name, String8 &value ) const;
        status_t getPropertyByteArray(String8 const &name,
                                              Vector<uint8_t> &value ) const;

        status_t setPropertyString(String8 const &name,
                                   String8 const &value );
        status_t setPropertyByteArray(String8 const &name,
                                      Vector<uint8_t> const &value );

    private:
        String8 vectorToString(Vector<uint8_t> const &vector) const;
        String8 arrayToString(uint8_t const *array, size_t len) const;
        String8 stringMapToString(KeyedVector<String8, String8> map) const;

        SortedVector<Vector<uint8_t> > mSessions;

        static const ssize_t kNotFound = -1;
        ssize_t findSession(Vector<uint8_t> const &sessionId) const;

        Mutex mLock;
        KeyedVector<String8, String8> mStringProperties;
        KeyedVector<String8, Vector<uint8_t> > mByteArrayProperties;
    };


    class MockCryptoPlugin : public CryptoPlugin {

        bool requiresSecureDecoderComponent(const char *mime) const;

        ssize_t decrypt(bool secure,
            const uint8_t key[16], const uint8_t iv[16],
            Mode mode, const void *srcPtr,
            const SubSample *subSamples, size_t numSubSamples,
            void *dstPtr, AString *errorDetailMsg);
    private:
        String8 subSamplesToString(CryptoPlugin::SubSample const *subSamples, size_t numSubSamples) const;
        String8 arrayToString(uint8_t const *array, size_t len) const;
    };
};
