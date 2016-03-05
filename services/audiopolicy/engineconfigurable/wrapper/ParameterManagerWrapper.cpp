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
//#define LOG_NDEBUG 0

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

    virtual void info(const string &log)
    {
        ALOGD("policy-parameter-manager: %s", log.c_str());
    }
    virtual void warning(const string &log)
    {
        ALOGW("policy-parameter-manager: %s", log.c_str());
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
                      "CriterionType %s already added", typeName.c_str());
    ALOGD("%s: Adding new criterionType %s", __FUNCTION__, typeName.c_str());

    mPolicyCriterionTypes[typeName] = mPfwConnector->createSelectionCriterionType(isInclusive);
}

void ParameterManagerWrapper::addCriterionTypeValuePair(
    const string &typeName,
    uint32_t numericValue,
    const string &literalValue)
{
    ALOG_ASSERT(mPolicyCriterionTypes.find(typeName) != mPolicyCriterionTypes.end(),
                      "CriterionType %s not found", typeName.c_str());
    ALOGV("%s: Adding new value pair (%d,%s) for criterionType %s", __FUNCTION__,
          numericValue, literalValue.c_str(), typeName.c_str());
    ISelectionCriterionTypeInterface *criterionType = mPolicyCriterionTypes[typeName];
    std::string error;
    criterionType->addValuePair(numericValue, literalValue, error);
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
    ALOG_ASSERT(it != elementsMap.end(), "Element %s not found", name.c_str());
    return it != elementsMap.end() ? it->second : NULL;
}

template <typename T>
const T *ParameterManagerWrapper::getElement(const string &name, const std::map<string, T *> &elementsMap) const
{
    parameterManagerElementSupported<T>();
    typename std::map<string, T *>::const_iterator it = elementsMap.find(name);
    ALOG_ASSERT(it != elementsMap.end(), "Element %s not found", name.c_str());
    return it != elementsMap.end() ? it->second : NULL;
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
    ALOG_ASSERT(mPolicyCriteria.find(name) == mPolicyCriteria.end(),
                "Route Criterion %s already added", name.c_str());

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
                      "Criterion %s already added", criterionName);

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
    ISelectionCriterionInterface *criterion =
            getElement<ISelectionCriterionInterface>(gPhoneStateCriterionTag, mPolicyCriteria);
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for %s", __FUNCTION__, gPhoneStateCriterionTag.c_str());
        return BAD_VALUE;
    }
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
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for %s", __FUNCTION__, gPhoneStateCriterionTag.c_str());
        return AUDIO_MODE_NORMAL;
    }
    return static_cast<audio_mode_t>(criterion->getCriterionState());
}

status_t ParameterManagerWrapper::setForceUse(audio_policy_force_use_t usage,
                                              audio_policy_forced_cfg_t config)
{
    // @todo: return an error on a unsupported value
    if (usage > AUDIO_POLICY_FORCE_USE_CNT) {
        return BAD_VALUE;
    }

    ISelectionCriterionInterface *criterion =
            getElement<ISelectionCriterionInterface>(gForceUseCriterionTag[usage], mPolicyCriteria);
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for %s", __FUNCTION__, gForceUseCriterionTag[usage].c_str());
        return BAD_VALUE;
    }
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
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for %s", __FUNCTION__, gForceUseCriterionTag[usage].c_str());
        return AUDIO_POLICY_FORCE_NONE;
    }
    return static_cast<audio_policy_forced_cfg_t>(criterion->getCriterionState());
}

bool ParameterManagerWrapper::isValueValidForCriterion(ISelectionCriterionInterface *criterion,
                                                       int valueToCheck)
{
    const ISelectionCriterionTypeInterface *interface = criterion->getCriterionType();
    string literalValue;
    return interface->getLiteralValue(valueToCheck, literalValue);
}

status_t ParameterManagerWrapper::setAvailableInputDevices(audio_devices_t inputDevices)
{
    ISelectionCriterionInterface *criterion =
            getElement<ISelectionCriterionInterface>(gInputDeviceCriterionTag, mPolicyCriteria);
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for %s", __FUNCTION__, gInputDeviceCriterionTag.c_str());
        return DEAD_OBJECT;
    }
    criterion->setCriterionState(inputDevices & ~AUDIO_DEVICE_BIT_IN);
    applyPlatformConfiguration();
    return NO_ERROR;
}

status_t ParameterManagerWrapper::setAvailableOutputDevices(audio_devices_t outputDevices)
{
    ISelectionCriterionInterface *criterion =
            getElement<ISelectionCriterionInterface>(gOutputDeviceCriterionTag, mPolicyCriteria);
    if (criterion == NULL) {
        ALOGE("%s: no criterion found for %s", __FUNCTION__, gOutputDeviceCriterionTag.c_str());
        return DEAD_OBJECT;
    }
    criterion->setCriterionState(outputDevices);
    applyPlatformConfiguration();
    return NO_ERROR;
}

void ParameterManagerWrapper::applyPlatformConfiguration()
{
    mPfwConnector->applyConfigurations();
}

} // namespace audio_policy
} // namespace android
