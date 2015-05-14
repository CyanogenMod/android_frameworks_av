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

#ifndef CLEARKEY_CRYPTO_PLUGIN_H_
#define CLEARKEY_CRYPTO_PLUGIN_H_

#include <media/hardware/CryptoAPI.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include "ClearKeyTypes.h"
#include "Session.h"
#include "Utils.h"

namespace clearkeydrm {

class CryptoPlugin : public android::CryptoPlugin {
public:
    CryptoPlugin(const android::sp<Session>& session) : mSession(session) {}
    virtual ~CryptoPlugin() {}

    virtual bool requiresSecureDecoderComponent(const char* mime) const {
        UNUSED(mime);
        return false;
    }

    virtual ssize_t decrypt(
            bool secure, const KeyId keyId, const Iv iv,
            Mode mode, const void* srcPtr,
            const SubSample* subSamples, size_t numSubSamples,
            void* dstPtr, android::AString* errorDetailMsg);

private:
    DISALLOW_EVIL_CONSTRUCTORS(CryptoPlugin);

    android::sp<Session> mSession;
};

} // namespace clearkeydrm

#endif // CLEARKEY_CRYPTO_PLUGIN_H_
