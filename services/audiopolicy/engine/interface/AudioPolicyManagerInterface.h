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

#pragma once

#include <AudioPolicyManagerObserver.h>
#include <RoutingStrategy.h>
#include <Volume.h>
#include <HwModule.h>
#include <DeviceDescriptor.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <utils/Errors.h>
#include <utils/Vector.h>

namespace android {

/**
 * This interface is dedicated to the policy manager that a Policy Engine shall implement.
 */
class AudioPolicyManagerInterface
{
public:
    /**
     * Checks if the engine was correctly initialized.
     *
     * @return NO_ERROR if initialization has been done correctly, error code otherwise..
     */
    virtual status_t initCheck() = 0;

    /**
     * Sets the Manager observer that allows the engine to retrieve information on collection
     * of devices, streams, HwModules, ...
     *
     * @param[in] observer handle on the manager.
     */
    virtual void setObserver(AudioPolicyManagerObserver *observer) = 0;

    /**
     * Get the input device selected for a given input source.
     *
     * @param[in] inputSource to get the selected input device associated to
     *
     * @return selected input device for the given input source, may be none if error.
     */
    virtual audio_devices_t getDeviceForInputSource(audio_source_t inputSource) const = 0;

    /**
     * Get the output device associated to a given strategy.
     *
     * @param[in] stream type for which the selected ouput device is requested.
     *
     * @return selected ouput device for the given strategy, may be none if error.
     */
    virtual audio_devices_t getDeviceForStrategy(routing_strategy stategy) const = 0;

    /**
     * Get the strategy selected for a given stream type.
     *
     * @param[in] stream: for which the selected strategy followed by is requested.
     *
     * @return strategy to be followed.
     */
    virtual routing_strategy getStrategyForStream(audio_stream_type_t stream) = 0;

    /**
     * Get the strategy selected for a given usage.
     *
     * @param[in] usage to get the selected strategy followed by.
     *
     * @return strategy to be followed.
     */
    virtual routing_strategy getStrategyForUsage(audio_usage_t usage) = 0;

    /**
     * Set the Telephony Mode.
     *
     * @param[in] mode: Android Phone state (normal, ringtone, csv, in communication)
     *
     * @return NO_ERROR if Telephony Mode set correctly, error code otherwise.
     */
    virtual status_t setPhoneState(audio_mode_t mode) = 0;

    /**
     * Get the telephony Mode
     *
     * @return the current telephony mode
     */
    virtual audio_mode_t getPhoneState() const = 0;

    /**
     * Set Force Use config for a given usage.
     *
     * @param[in] usage for which a configuration shall be forced.
     * @param[in] config wished to be forced for the given usage.
     *
     * @return NO_ERROR if the Force Use config was set correctly, error code otherwise (e.g. config
     * not allowed a given usage...)
     */
    virtual status_t setForceUse(audio_policy_force_use_t usage,
                                 audio_policy_forced_cfg_t config) = 0;

    /**
     * Get Force Use config for a given usage.
     *
     * @param[in] usage for which a configuration shall be forced.
     *
     * @return config wished to be forced for the given usage.
     */
    virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage) const = 0;

    /**
     * Set the connection state of device(s).
     *
     * @param[in] devDesc for which the state has changed.
     * @param[in] state of availability of this(these) device(s).
     *
     * @return NO_ERROR if devices criterion updated correctly, error code otherwise.
     */
    virtual status_t setDeviceConnectionState(const android::sp<android::DeviceDescriptor> devDesc,
                                              audio_policy_dev_state_t state) = 0;

protected:
    virtual ~AudioPolicyManagerInterface() {}
};

}; // namespace android
