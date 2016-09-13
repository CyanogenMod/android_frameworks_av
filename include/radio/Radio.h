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

#ifndef ANDROID_HARDWARE_RADIO_H
#define ANDROID_HARDWARE_RADIO_H

#include <binder/IBinder.h>
#include <utils/threads.h>
#include <radio/RadioCallback.h>
#include <radio/IRadio.h>
#include <radio/IRadioService.h>
#include <radio/IRadioClient.h>
#include <system/radio.h>

namespace android {

class MemoryDealer;

class Radio : public BnRadioClient,
                        public IBinder::DeathRecipient
{
public:

    virtual ~Radio();

    static  status_t listModules(struct radio_properties *properties,
                                 uint32_t *numModules);
    static  sp<Radio> attach(radio_handle_t handle,
                             const struct radio_band_config *config,
                             bool withAudio,
                             const sp<RadioCallback>& callback);


            void detach();

            status_t setConfiguration(const struct radio_band_config *config);

            status_t getConfiguration(struct radio_band_config *config);

            status_t setMute(bool mute);

            status_t getMute(bool *mute);

            status_t step(radio_direction_t direction, bool skipSubChannel);

            status_t scan(radio_direction_t direction, bool skipSubChannel);

            status_t tune(unsigned int channel, unsigned int subChannel);

            status_t cancel();

            status_t getProgramInformation(struct radio_program_info *info);

            status_t hasControl(bool *hasControl);

            // BpRadioClient
            virtual void onEvent(const sp<IMemory>& eventMemory);

            //IBinder::DeathRecipient
            virtual void binderDied(const wp<IBinder>& who);

private:
            Radio(radio_handle_t handle,
                            const sp<RadioCallback>&);
            static const sp<IRadioService> getRadioService();

            Mutex                   mLock;
            sp<IRadio>              mIRadio;
            const radio_handle_t    mHandle;
            sp<RadioCallback>       mCallback;
};

}; // namespace android

#endif //ANDROID_HARDWARE_RADIO_H
