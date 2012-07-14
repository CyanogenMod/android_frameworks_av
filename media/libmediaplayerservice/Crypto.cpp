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
#define LOG_TAG "Crypto"
#include <utils/Log.h>

#include "Crypto.h"

#include <media/hardware/CryptoAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>

#include <dlfcn.h>

namespace android {

Crypto::Crypto()
    : mInitCheck(NO_INIT),
      mLibHandle(NULL),
      mFactory(NULL),
      mPlugin(NULL) {
    mInitCheck = init();
}

Crypto::~Crypto() {
    delete mPlugin;
    mPlugin = NULL;

    delete mFactory;
    mFactory = NULL;

    if (mLibHandle != NULL) {
        dlclose(mLibHandle);
        mLibHandle = NULL;
    }
}

status_t Crypto::initCheck() const {
    return mInitCheck;
}

status_t Crypto::init() {
    mLibHandle = dlopen("libdrmdecrypt.so", RTLD_NOW);

    if (mLibHandle == NULL) {
        ALOGE("Unable to locate libdrmdecrypt.so");

        return ERROR_UNSUPPORTED;
    }

    typedef CryptoFactory *(*CreateCryptoFactoryFunc)();
    CreateCryptoFactoryFunc createCryptoFactory =
        (CreateCryptoFactoryFunc)dlsym(mLibHandle, "createCryptoFactory");

    if (createCryptoFactory == NULL
            || ((mFactory = createCryptoFactory()) == NULL)) {
        if (createCryptoFactory == NULL) {
            ALOGE("Unable to find symbol 'createCryptoFactory'.");
        } else {
            ALOGE("createCryptoFactory() failed.");
        }

        dlclose(mLibHandle);
        mLibHandle = NULL;

        return ERROR_UNSUPPORTED;
    }

    return OK;
}

bool Crypto::isCryptoSchemeSupported(const uint8_t uuid[16]) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return false;
    }

    return mFactory->isCryptoSchemeSupported(uuid);
}

status_t Crypto::createPlugin(
        const uint8_t uuid[16], const void *data, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin != NULL) {
        return -EINVAL;
    }

    return mFactory->createPlugin(uuid, data, size, &mPlugin);
}

status_t Crypto::destroyPlugin() {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    delete mPlugin;
    mPlugin = NULL;

    return OK;
}

bool Crypto::requiresSecureDecoderComponent(const char *mime) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->requiresSecureDecoderComponent(mime);
}

ssize_t Crypto::decrypt(
        bool secure,
        const uint8_t key[16],
        const uint8_t iv[16],
        CryptoPlugin::Mode mode,
        const void *srcPtr,
        const CryptoPlugin::SubSample *subSamples, size_t numSubSamples,
        void *dstPtr,
        AString *errorDetailMsg) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    if (mPlugin == NULL) {
        return -EINVAL;
    }

    return mPlugin->decrypt(
            secure, key, iv, mode, srcPtr, subSamples, numSubSamples, dstPtr,
            errorDetailMsg);
}

}  // namespace android
