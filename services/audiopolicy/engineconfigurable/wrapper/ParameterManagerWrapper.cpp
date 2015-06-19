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

#define LOG_TAG "APM::AudioPolicyEngine/PFWWrapper"

#include "ParameterManagerWrapper.h"
#include "audio_policy_criteria_conf.h"
#include <ParameterMgrPlatformConnector.h>
#include <SelectionCriterionTypeInterface.h>
#include <SelectionCriterionInterface.h>
#include <convert.h>
#include <algorithm>
#include <cutils/config_utils.h>
#include <cutils/misc.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <stdint.h>
#include <cmath>
#include <utils/Log.h>

using std::string;
using std::map;
using std::vector;

/// PFW related definitions
// Logger
class ParameterMgrPlatformConnectorLogger : public CParameterMgrPlatformConnector::ILogger
{
public:
    ParameterMgrPlatformConnectorLogger() {}

    virtual void log(bool isWarning, const string &log)
    {
        const static string format("policy-parameter-manager: ");

        if (isWarning) {
            ALOGW("%s %s", format.c_str(), log.c_str());
        } else {
            ALOGD("%s %s", format.c_str(), log.c_str());
        }
    }
};

namespace android
{

using utilities::convertTo;

namespace audio_policy
{
const char *const ParameterManagerWrapper::mPolicyPfwDefaultConfFileName =
    "/etc/parameter-framework/ParameterFrameworkConfigurationPolicy.xml";

template <>
struct ParameterManagerWrapper::parameterManagerElementSupported<ISelectionCriterionInterface> {};
template <>
struct ParameterManagerWrapper::parameterManagerElementSupported<ISelectionCriterionTypeInterface> {};

ParameterManagerWrapper::ParameterManagerWrapper()
    : mPfwConnectorLogger(new ParameterMgrPlatformConnectorLogger)
{
    // Connector
    mPfwConnector = new CParameterMgrPlatformConnector(mPolicyPfwDefaultConfFileName);

    // Logger
    mPfwConnector->setLogger(mPfwConnectorLogger);

    // Load criteria file
    if ((loadAudioPolicyCriteriaConfig(gAudioPolicyCriteriaVendorConfFilePath) != NO_ERROR) &&
        (loadAudioPolicyCriteriaConfig(gAudioPolicyCriteriaConfFilePath) != NO_ERROR)) {
        ALOGE("%s: Neither vendor conf file (%s) nor system conf file (%s) could be found",
              __FUNCTION__, gAudioPolicyCriteriaVendorConfFilePath,
              gAudioPolicyCriteriaConfFilePath);
    }
    ALOGD("%s: ParameterManagerWrapper instantiated!", __FUNCTION__);
}

ParameterManagerWrapper::~ParameterManagerWrapper()
{
    // Unset logger
    mPfwConnector->setLogger(NULL);
    // Remove logger
    delete mPfwConnectorLogger;
    // Remove connector
    delete mPfwConnector;
}

status_t ParameterManagerWrapper::start()
{
    ALOGD("%s: in", __FUNCTION__);
    /// Start PFW
    std::string error;
    if (!mPfwConnector->start(error)) {
        ALOGE("%s: Policy PFW start error: %s", __FUNCTION__, error.c_str());
        return NO_INIT;
    }
    ALOGD("%s: Policy PFW successfully started!", __FUNCTION__);
    return NO_ERROR;
}


void ParameterManagerWrapper::addCriterionType(const string &typeName, bool isInclusive)
{
    ALOG_ASSERT(mPolicyCriterionTypes.find(typeName) == mPolicyCriterionTypes.end(),
                      "CriterionType " << typeName << " already added");
    ALOGD("%s: Adding new criterionType %s", __FUNCTION__, typeName.c_str());

    mPolicyCriterionTypes[typeName] = mPfwConnector->createSelectionCriterionType(isInclusive);
}

void ParameterManagerWrapper::addCriterionTypeValuePair(
    const string &typeName,
    uint32_t numericValue,
    const string &literalValue)
{
    ALOG_ASSERT(mPolicyCriterionTypes.find(typeName) != mPolicyCriterionTypes.end(),
                      "CriterionType " << typeName.c_str() << "not found");
    ALOGV("%s: Adding new value pair (%d,%s) for criterionType %s", __FUNCTION__,
          numericValue, literalValue.c_str(), typeName.c_str());
    ISelectionCriterionTypeInterface *criterionType = mPolicyCriterionTypes[typeName];
    criterionType->addValuePair(numericValue, literalValue.c_str());
}

void ParameterManagerWrapper::loadCriterionType(cnode *root, bool isInclusive)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    cnode *node;
    for (node = root->first_child; node != NULL; node = node->next) {

        ALOG_ASSERT(node != NULL, "error in parsing file");
        const char *typeName = node->name;
        char *valueNames = strndup(node->value, strlen(node->value));

        addCriterionType(typeName, isInclusive);

        uint32_t index = 0;
        char *ctx;
        char *valueName = strtok_r(valueNames, ",", &ctx);
        while (valueName != NULL) {
            if (strlen(valueName) != 0) {

                // Conf file may use or not pair, if no pair, use incremental index, else
                // use provided index.
                if (strchr(valueName, ':') != NULL) {

                    char *first = strtok(valueName, ":");
                    char *second = strtok(NULL, ":");
                    ALOG_ASSERT((first != NULL) && (strlen(first) != 0) &&
                                      (second != NULL) && (strlen(second) != 0),
                                      "invalid value pair");

                    if (!convertTo<string, uint32_t>(first, index)) {
                        ALOGE("%s: Invalid index(%s) found", __FUNCTION__, first);
                    }
                    addCriterionTypeValuePair(typeName, index, second);
                } else {

                    uint32_t pfwIndex = isInclusive ? 1 << index : index;
                    addCriterionTypeValuePair(typeName, pfwIndex, valueName);
                    index += 1;
                }
            }
            valueName = strtok_r(NULL, ",", &ctx);
        }
        free(valueNames);
    }
}

void ParameterManagerWrapper::loadInclusiveCriterionType(cnode *root)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    cnode *node = config_find(root, gInclusiveCriterionTypeTag.c_str());
    if (node == NULL) {
        return;
    }
    loadCriterionType(node, true);
}

void ParameterManagerWrapper::loadExclusiveCriterionType(cnode *root)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    cnode *node = config_find(root, gExclusiveCriterionTypeTag.c_str());
    if (node == NULL) {
        return;
    }
    loadCriterionType(node, false);
}

void ParameterManagerWrapper::parseChildren(cnode *root, string &defaultValue, string &type)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    cnode *node;
    for (node = root->first_child; node != NULL; node = node->next) {
        ALOG_ASSERT(node != NULL, "error in parsing file");

        if (string(node->name) == gDefaultTag) {
            defaultValue = node->value;
        } else if (string(node->name) == gTypeTag) {
            type = node->value;
        } else {
             ALOGE("%s: Unrecognized %s %s node", __FUNCTION__, node->name, node->value);
        }
    }
}

template <typename T>
T *ParameterManagerWrapper::getElement(const string &name, std::map<string, T *> &elementsMap)
{
    parameterManagerElementSupported<T>();
    typename std::map<string, T *>::iterator it = elementsMap.find(name);
    ALOG_ASSERT(it != elementsMap.end(), "Element " << name << " not found");
    return it->second;
}

template <typename T>
const T *ParameterManagerWrapper::getElement(const string &name, const std::map<string, T *> &elementsMap) const
{
    parameterManagerElementSupported<T>();
    typename std::map<string, T *>::const_iterator it = elementsMap.find(name);
    ALOG_ASSERT(it != elementsMap.end(), "Element " << name << " not found");
    return it->second;
}

void ParameterManagerWrapper::loadCriteria(cnode *root)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    cnode *node = config_find(root, gCriterionTag.c_str());

    if (node == NULL) {
        ALOGW("%s: no inclusive criteria found", __FUNCTION__);
        return;
    }
    for (node = node->first_child; node != NULL; node = node->next) {
        loadCriterion(node);
    }
}

void ParameterManagerWrapper::addCriterion(const string &name, const string &typeName,
                              const string &defaultLiteralValue)
{
    ALOG_ASSERT(mPolicyCriteria.find(criterionName) == mPolicyCriteria.end(),
                "Route Criterion " << criterionName << " already added");

    ISelectionCriterionTypeInterface *criterionType =
            getElement<ISelectionCriterionTypeInterface>(typeName, mPolicyCriterionTypes);

    ISelectionCriterionInterface *criterion =
            mPfwConnector->createSelectionCriterion(name, criterionType);

    mPolicyCriteria[name] = criterion;
    int numericalValue = 0;
    if (!criterionType->getNumericalValue(defaultLiteralValue.c_str(),  numericalValue)) {
        ALOGE("%s; trying to apply invalid default literal value (%s)", __FUNCTION__,
              defaultLiteralValue.c_str());
    }
    criterion->setCriterionState(numericalValue);
}

void ParameterManagerWrapper::loadCriterion(cnode *root)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    const char *criterionName = root->name;

    ALOG_ASSERT(mPolicyCriteria.find(criterionName) == mPolicyCriteria.end(),
                      "Criterion " << criterionName << " already added");

    string paramKeyName = "";
    string path = "";
    string typeName = "";
    string defaultValue = "";

    parseChildren(root, defaultValue, typeName);

    addCriterion(criterionName, typeName, defaultValue);
}

void ParameterManagerWrapper::loadConfig(cnode *root)
{
    ALOG_ASSERT(root != NULL, "error in parsing file");
    cnode *node = config_find(root, gPolicyConfTag.c_str());
    if (node == NULL) {
        ALOGW("%s: Could not find node for pfw", __FUNCTION__);
        return;
    }
    ALOGD("%s: Loading conf for pfw", __FUNCTION__);
    loadInclusiveCriterionType(node);
    loadExclusiveCriterionType(node);
    loadCriteria(node);
}


status_t ParameterManagerWrapper::loadAudioPolicyCriteriaConfig(const char *path)
{
    ALOG_ASSERT(path != NULL, "error in parsing file: empty path");
    cnode *root;
    char *data;
    ALOGD("%s", __FUNCTION__);
    data = (char *)load_file(path, NULL);
    if (data == NULL) {
        return -ENODEV;
    }
    root = config_node("", "");
    ALOG_ASSERT(root != NULL, "Unable to allocate a configuration node");
    config_load(root, data);

    loadConfig(root);

    config_free(root);
    free(root);
    free(data);
    ALOGD("%s: loaded", __FUNCTION__);
    return NO_ERROR;
}

bool ParameterManagerWrapper::isStarted()
{
    return mPfwConnector && mPfwConnector->isStarted();
}

status_t ParameterManagerWrapper::setPhoneState(audio_mode_t mode)
{
    ISelectionCriterionInterface *criterion = mPolicyCriteria[gPhoneStateCriterionTag];
    if (!isValueValidForCriterion(criterion, static_cast<int>(mode))) {
        return BAD_VALUE;
    }
    criterion->setCriterionState((int)(mode));
    applyPlatformConfiguration();
    return NO_ERROR;
}

audio_mode_t ParameterManagerWrapper::getPhoneState() const
{
    const ISelectionCriterionInterface *criterion =
            getElement<ISelectionCriterionInterface>(gPhoneStateCriterionTag, mPolicyCriteria);
    return static_cast<audio_mode_t>(criterion->getCriterionState());
}

status_t ParameterManagerWrapper::setForceUse(audio_policy_force_use_t usage,
                                              audio_policy_forced_cfg_t config)
{
    // @todo: return an error on a unsupported value
    if (usage > AUDIO_POLICY_FORCE_USE_CNT) {
        return BAD_VALUE;
    }

    ISelectionCriterionInterface *criterion = mPolicyCriteria[gForceUseCriterionTag[usage]];
    if (!isValueValidForCriterion(criterion, static_cast<int>(config))) {
        return BAD_VALUE;
    }
    criterion->setCriterionState((int)config);
    applyPlatformConfiguration();
    return NO_ERROR;
}

audio_policy_forced_cfg_t ParameterManagerWrapper::getForceUse(audio_policy_force_use_t usage) const
{
    // @todo: return an error on a unsupported value
    if (usage > AUDIO_POLICY_FORCE_USE_CNT) {
        return AUDIO_POLICY_FORCE_NONE;
    }
    const ISelectionCriterionInterface *criterion =
            getElement<ISelectionCriterionInterface>(gForceUseCriterionTag[usage], mPolicyCriteria);
    return static_cast<audio_policy_forced_cfg_t>(criterion->getCriterionState());
}

bool ParameterManagerWrapper::isValueValidForCriterion(ISelectionCriterionInterface *criterion,
                                                       int valueToCheck)
{
    const ISelectionCriterionTypeInterface *interface = criterion->getCriterionType();
    string literalValue;
    return interface->getLiteralValue(valueToCheck, literalValue);
}

status_t ParameterManagerWrapper::setDeviceConnectionState(audio_devices_t devices,
                                                           audio_policy_dev_state_t state,
                                                           const char */*deviceAddres*/)
{
    ISelectionCriterionInterface *criterion = NULL;

    if (audio_is_output_devices(devices)) {
        criterion = mPolicyCriteria[gOutputDeviceCriterionTag];
    } else if (devices & AUDIO_DEVICE_BIT_IN) {
        criterion = mPolicyCriteria[gInputDeviceCriterionTag];
    } else {
        return BAD_TYPE;
    }
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for devices", __FUNCTION__);
        return DEAD_OBJECT;
    }

    int32_t previousDevices = criterion->getCriterionState();
    switch (state)
    {
    case AUDIO_POLICY_DEVICE_STATE_AVAILABLE:
        criterion->setCriterionState(previousDevices |= devices);
        break;

    case AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE:
        if (devices & AUDIO_DEVICE_BIT_IN) {
            devices &= ~AUDIO_DEVICE_BIT_IN;
        }
        criterion->setCriterionState(previousDevices &= ~devices);
        break;

    default:
        return BAD_VALUE;
    }
    applyPlatformConfiguration();
    return NO_ERROR;
}

void ParameterManagerWrapper::applyPlatformConfiguration()
{
    mPfwConnector->applyConfigurations();
}

} // namespace audio_policy
} // namespace android
