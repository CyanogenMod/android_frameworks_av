/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_IRADIO_SERVICE_H
#define ANDROID_HARDWARE_IRADIO_SERVICE_H

#include <utils/RefBase.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <system/radio.h>

namespace android {

class IRadio;
class IRadioClient;

class IRadioService : public IInterface
{
public:

    DECLARE_META_INTERFACE(RadioService);

    virtual status_t listModules(struct radio_properties *properties,
                                 uint32_t *numModules) = 0;

    virtual status_t attach(const radio_handle_t handle,
                            const sp<IRadioClient>& client,
                            const struct radio_band_config *config,
                            bool withAudio,
                            sp<IRadio>& radio) = 0;
};

// ----------------------------------------------------------------------------

class BnRadioService: public BnInterface<IRadioService>
{
public:
    virtual status_t    onTransact( uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0);
};

}; // namespace android

#endif //ANDROID_HARDWARE_IRADIO_SERVICE_H
