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


#include <AudioPolicyManagerInterface.h>
#include <AudioPolicyPluginInterface.h>
#include "Collection.h"

namespace android
{
class AudioPolicyManagerObserver;

namespace audio_policy
{

class ParameterManagerWrapper;
class VolumeProfile;

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

        virtual android::status_t initCheck()
        {
            return mPolicyEngine->initCheck();
        }
        virtual void setObserver(AudioPolicyManagerObserver *observer)
        {
            mPolicyEngine->setObserver(observer);
        }
        virtual audio_devices_t getDeviceForInputSource(audio_source_t inputSource) const
        {
            return mPolicyEngine->getPropertyForKey<audio_devices_t, audio_source_t>(inputSource);
        }
        virtual audio_devices_t getDeviceForStrategy(routing_strategy stategy) const;
        virtual routing_strategy getStrategyForStream(audio_stream_type_t stream)
        {
            return mPolicyEngine->getPropertyForKey<routing_strategy, audio_stream_type_t>(stream);
        }
        virtual routing_strategy getStrategyForUsage(audio_usage_t usage);
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
        virtual android::status_t setDeviceConnectionState(const sp<DeviceDescriptor> devDesc,
                                                           audio_policy_dev_state_t state)
        {
            return mPolicyEngine->setDeviceConnectionState(devDesc, state);
        }

    private:
        Engine *mPolicyEngine;
    } mManagerInterface;

    class PluginInterfaceImpl : public AudioPolicyPluginInterface
    {
    public:
        PluginInterfaceImpl(Engine *policyEngine)
            : mPolicyEngine(policyEngine) {}

        virtual status_t addStrategy(const std::string &name, routing_strategy strategy)
        {
            return mPolicyEngine->add<routing_strategy>(name, strategy);
        }
        virtual status_t addStream(const std::string &name, audio_stream_type_t stream)
        {
            return mPolicyEngine->add<audio_stream_type_t>(name, stream);
        }
        virtual status_t addUsage(const std::string &name, audio_usage_t usage)
        {
            return mPolicyEngine->add<audio_usage_t>(name, usage);
        }
        virtual status_t addInputSource(const std::string &name, audio_source_t source)
        {
            return mPolicyEngine->add<audio_source_t>(name, source);
        }
        virtual bool setDeviceForStrategy(const routing_strategy &strategy, audio_devices_t devices)
        {
            return mPolicyEngine->setPropertyForKey<audio_devices_t, routing_strategy>(devices,
                                                                                       strategy);
        }
        virtual bool setStrategyForStream(const audio_stream_type_t &stream,
                                          routing_strategy strategy)
        {
            return mPolicyEngine->setPropertyForKey<routing_strategy, audio_stream_type_t>(strategy,
                                                                                           stream);
        }
        virtual bool setVolumeProfileForStream(const audio_stream_type_t &stream,
                                               const audio_stream_type_t &volumeProfile);

        virtual bool setStrategyForUsage(const audio_usage_t &usage, routing_strategy strategy)
        {
            return mPolicyEngine->setPropertyForKey<routing_strategy, audio_usage_t>(strategy,
                                                                                     usage);
        }
        virtual bool setDeviceForInputSource(const audio_source_t &inputSource,
                                             audio_devices_t device)
        {
            return mPolicyEngine->setPropertyForKey<audio_devices_t, audio_source_t>(device,
                                                                                     inputSource);
        }

    private:
        Engine *mPolicyEngine;
    } mPluginInterface;

private:
    /* Copy facilities are put private to disable copy. */
    Engine(const Engine &object);
    Engine &operator=(const Engine &object);

    void setObserver(AudioPolicyManagerObserver *observer);

    bool setVolumeProfileForStream(const audio_stream_type_t &stream,
                                   device_category deviceCategory,
                                   const VolumeCurvePoints &points);

    status_t initCheck();
    status_t setPhoneState(audio_mode_t mode);
    audio_mode_t getPhoneState() const;
    status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);
    audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage) const;
    status_t setDeviceConnectionState(const sp<DeviceDescriptor> devDesc,
                                      audio_policy_dev_state_t state);
    StrategyCollection mStrategyCollection; /**< Strategies indexed by their enum id. */
    StreamCollection mStreamCollection; /**< Streams indexed by their enum id.  */
    UsageCollection mUsageCollection; /**< Usages indexed by their enum id. */
    InputSourceCollection mInputSourceCollection; /**< Input sources indexed by their enum id. */

    template <typename Key>
    status_t add(const std::string &name, const Key &key);

    template <typename Key>
    Element<Key> *getFromCollection(const Key &key) const;

    template <typename Key>
    const Collection<Key> &getCollection() const;

    template <typename Key>
    Collection<Key> &getCollection();

    template <typename Property, typename Key>
    Property getPropertyForKey(Key key) const;

    template <typename Property, typename Key>
    bool setPropertyForKey(const Property &property, const Key &key);

    /**
     * Policy Parameter Manager hidden through a wrapper.
     */
    ParameterManagerWrapper *mPolicyParameterMgr;

    AudioPolicyManagerObserver *mApmObserver;
};

}; // namespace audio_policy

}; // namespace android

