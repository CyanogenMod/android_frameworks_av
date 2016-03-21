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


#include "AudioPolicyManagerInterface.h"
#include <AudioGain.h>
#include <policy.h>

namespace android
{

class AudioPolicyManagerObserver;

namespace audio_policy
{

class Engine
{
public:
    Engine();
    virtual ~Engine();

    template <class RequestedInterface>
    RequestedInterface *queryInterface();

private:
    /// Interface members
    class ManagerInterfaceImpl : public AudioPolicyManagerInterface
    {
    public:
        ManagerInterfaceImpl(Engine *policyEngine)
            : mPolicyEngine(policyEngine) {}

        virtual void setObserver(AudioPolicyManagerObserver *observer)
        {
            mPolicyEngine->setObserver(observer);
        }
        virtual status_t initCheck()
        {
            return mPolicyEngine->initCheck();
        }
        virtual audio_devices_t getDeviceForInputSource(audio_source_t inputSource) const
        {
            return mPolicyEngine->getDeviceForInputSource(inputSource);
        }
        virtual audio_devices_t getDeviceForStrategy(routing_strategy strategy) const
        {
            return mPolicyEngine->getDeviceForStrategy(strategy);
        }
        virtual routing_strategy getStrategyForStream(audio_stream_type_t stream)
        {
            return mPolicyEngine->getStrategyForStream(stream);
        }
        virtual routing_strategy getStrategyForUsage(audio_usage_t usage)
        {
            return mPolicyEngine->getStrategyForUsage(usage);
        }
        virtual status_t setPhoneState(audio_mode_t mode)
        {
            return mPolicyEngine->setPhoneState(mode);
        }
        virtual audio_mode_t getPhoneState() const
        {
            return mPolicyEngine->getPhoneState();
        }
        virtual status_t setForceUse(audio_policy_force_use_t usage,
                                     audio_policy_forced_cfg_t config)
        {
            return mPolicyEngine->setForceUse(usage, config);
        }
        virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage) const
        {
            return mPolicyEngine->getForceUse(usage);
        }
        virtual status_t setDeviceConnectionState(const sp<DeviceDescriptor> /*devDesc*/,
                                                  audio_policy_dev_state_t /*state*/)
        {
            return NO_ERROR;
        }
    private:
        Engine *mPolicyEngine;
    } mManagerInterface;

private:
    /* Copy facilities are put private to disable copy. */
    Engine(const Engine &object);
    Engine &operator=(const Engine &object);

    void setObserver(AudioPolicyManagerObserver *observer);

    status_t initCheck();

    inline bool isInCall() const
    {
        return is_state_in_call(mPhoneState);
    }

    status_t setPhoneState(audio_mode_t mode);
    audio_mode_t getPhoneState() const
    {
        return mPhoneState;
    }
    status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);
    audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage) const
    {
        return mForceUse[usage];
    }
    status_t setDefaultDevice(audio_devices_t device);

    routing_strategy getStrategyForStream(audio_stream_type_t stream);
    routing_strategy getStrategyForUsage(audio_usage_t usage);
    audio_devices_t getDeviceForStrategy(routing_strategy strategy) const;
    audio_devices_t getDeviceForStrategyInt(routing_strategy strategy,
                                            DeviceVector availableOutputDevices,
                                            DeviceVector availableInputDevices,
                                            const SwAudioOutputCollection &outputs) const;
    audio_devices_t getDeviceForInputSource(audio_source_t inputSource) const;
    audio_mode_t mPhoneState;  /**< current phone state. */

    /** current forced use configuration. */
    audio_policy_forced_cfg_t mForceUse[AUDIO_POLICY_FORCE_USE_CNT];

    AudioPolicyManagerObserver *mApmObserver;
};
} // namespace audio_policy
} // namespace android

