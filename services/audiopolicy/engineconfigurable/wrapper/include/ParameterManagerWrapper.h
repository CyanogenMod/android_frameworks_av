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

#include <system/audio.h>
#include <system/audio_policy.h>
#include <utils/Errors.h>
#include <utils/RWLock.h>
#include <list>
#include <map>
#include <string>
#include <vector>

class CParameterMgrPlatformConnector;
class ISelectionCriterionInterface;
class ISelectionCriterionTypeInterface;
struct cnode;

class ParameterMgrPlatformConnectorLogger;

namespace android
{
namespace audio_policy
{

class ParameterManagerWrapper
{
private:
    typedef std::pair<int, const char *> CriterionTypeValuePair;

    typedef std::map<std::string, ISelectionCriterionInterface *> CriterionCollection;
    typedef std::map<std::string, ISelectionCriterionTypeInterface *> CriterionTypeCollection;
    typedef CriterionCollection::iterator CriterionMapIterator;
    typedef CriterionCollection::const_iterator CriterionMapConstIterator;
    typedef CriterionTypeCollection::iterator CriterionTypeMapIterator;
    typedef CriterionTypeCollection::const_iterator CriteriaTypeMapConstIterator;

public:
    ParameterManagerWrapper();
    ~ParameterManagerWrapper();

    /**
     * Starts the platform state service.
     * It starts the parameter framework policy instance.
     *
     * @return NO_ERROR if success, error code otherwise.
     */
    status_t start();

    /**
     * The following API wrap policy action to criteria
     */

    /**
     * Checks if the platform state was correctly started (ie the policy parameter manager
     * has been instantiated and started correctly).
     *
     * @todo: map on initCheck?
     *
     * @return true if platform state is started correctly, false otherwise.
     */
    bool isStarted();

    /**
     * Set Telephony Mode.
     * It will set the telephony mode criterion accordingly and apply the configuration in order
     * to select the right configuration on domains depending on this mode criterion.
     *
     * @param[in] mode: Android Phone state (normal, ringtone, csv, in communication)
     *
     * @return NO_ERROR if criterion set correctly, error code otherwise.
     */
    status_t setPhoneState(audio_mode_t mode);

    audio_mode_t getPhoneState() const;

    /**
     * Set Force Use config for a given usage.
     * It will set the corresponding policy parameter framework criterion.
     *
     * @param[in] usage for which a configuration shall be forced.
     * @param[in] config wished to be forced for the given shall.
     *
     * @return NO_ERROR if the criterion was set correctly, error code otherwise (e.g. config not
     * allowed a given usage...)
     */
    status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);

    audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage) const;

    /**
     * Set the available input devices i.e. set the associated policy parameter framework criterion
     *
     * @param[in] inputDevices mask of available input devices.
     *
     * @return NO_ERROR if devices criterion updated correctly, error code otherwise.
     */
    status_t setAvailableInputDevices(audio_devices_t inputDevices);

    /**
     * Set the available output devices i.e. set the associated policy parameter framework criterion
     *
     * @param[in] outputDevices mask of available output devices.
     *
     * @return NO_ERROR if devices criterion updated correctly, error code otherwise.
     */
    status_t setAvailableOutputDevices(audio_devices_t outputDevices);

private:
    /**
     * Apply the configuration of the platform on the policy parameter manager.
     * Once all the criteria have been set, the client of the platform state must call
     * this function in order to have the route PFW taking into account these criteria.
     *
     * OPENS: shall we expose this?
     *      - Yes if atomic set operation.
     *          In this case, abstract it behind the "STAGE AND COMMIT" pattern
     *      - no if need to set more than one before triggering an apply configuration.
     */
    void applyPlatformConfiguration();

    /**
     * Load the criterion configuration file.
     *
     * @param[in] path Criterion conf file path.
     *
     * @return NO_ERROR is parsing successful, error code otherwise.
     */
    status_t loadAudioPolicyCriteriaConfig(const char *path);

    /**
     * Add a criterion type to AudioPolicyPfw.
     *
     * @param[in] typeName of the PFW criterion type.
     * @param[in] isInclusive attribute of the criterion type.
     */
    void addCriterionType(const std::string &typeName, bool isInclusive);

    /**
     * Add a criterion type value pair to AudioPolicyPfw.
     *
     * @param[in] typeName criterion type name to which this value pair is added to.
     * @param[in] numeric part of the value pair.
     * @param[in] literal part of the value pair.
     */
    void addCriterionTypeValuePair(const std::string &typeName, uint32_t numeric,
                                   const std::string &literal);

    /**
     * Add a criterion to AudioPolicyPfw.
     *
     * @param[in] name of the PFW criterion.
     * @param[in] typeName criterion type name to which this criterion is associated to.
     * @param[in] defaultLiteralValue of the PFW criterion.
     */
    void addCriterion(const std::string &name,
                      const std::string &typeName,
                      const std::string &defaultLiteralValue);
    /**
     * Parse and load the inclusive criterion type from configuration file.
     *
     * @param[in] root node of the configuration file.
     */
    void loadInclusiveCriterionType(cnode *root);

    /**
     * Parse and load the exclusive criterion type from configuration file.
     *
     * @param[in] root node of the configuration file.
     */
    void loadExclusiveCriterionType(cnode *root);

    /**
     * Parse and load the criteria from configuration file.
     *
     * @param[in] root node of the configuration file.
     */
    void loadCriteria(cnode *root);

    /**
     * Parse and load a criterion from configuration file.
     *
     * @param[in] root node of the configuration file.
     */
    void loadCriterion(cnode *root);

    /**
     * Parse and load the criterion types from configuration file.
     *
     * @param[in] root node of the configuration file
     * @param[in] isInclusive true if inclusive, false is exclusive.
     */
    void loadCriterionType(cnode *root, bool isInclusive);

    /**
     * Load the configuration file.
     *
     * @param[in] root node of the configuration file.
     */
    void loadConfig(cnode *root);

    /**
     * Parse and load the chidren node from a given root node.
     *
     * @param[in] root node of the configuration file
     * @param[out] defaultValue of the parameter manager element to retrieve.
     * @param[out] type of the parameter manager element to retrieve.
    */
    void parseChildren(cnode *root, std::string &defaultValue, std::string &type);

    /**
     * Retrieve an element from a map by its name.
     *
     * @tparam T type of element to search.
     * @param[in] name name of the element to find.
     * @param[in] elementsMap maps of elements to search into.
     *
     * @return valid pointer on element if found, NULL otherwise.
     */
    template <typename T>
    T *getElement(const std::string &name, std::map<std::string, T *> &elementsMap);

    /**
     * Retrieve an element from a map by its name. Const version.
     *
     * @tparam T type of element to search.
     * @param[in] name name of the element to find.
     * @param[in] elementsMap maps of elements to search into.
     *
     * @return valid pointer on element if found, NULL otherwise.
     */
    template <typename T>
    const T *getElement(const std::string &name,
                        const std::map<std::string, T *> &elementsMap) const;

    /**
     * set the value of a component state.
     *
     * @param[in] value new value to set to the component state.
     * @param[in] stateName of the component state.
     */
    void setValue(int value, const std::string &stateName);

    /**
     * get the value of a component state.
     *
     * @param[in] name of the component state.
     *
     * @return value of the component state
     */
    int getValue(const std::string &stateName) const;

    bool isValueValidForCriterion(ISelectionCriterionInterface *criterion, int valueToCheck);

    CriterionTypeCollection mPolicyCriterionTypes; /**< Policy Criterion Type map. */
    CriterionCollection mPolicyCriteria; /**< Policy Criterion Map. */

    CParameterMgrPlatformConnector *mPfwConnector; /**< Policy Parameter Manager connector. */
    ParameterMgrPlatformConnectorLogger *mPfwConnectorLogger; /**< Policy PFW logger. */


    /**
     * provide a compile time error if no specialization is provided for a given type.
     *
     * @tparam T: type of the parameter manager element. Supported one are:
     *                      - Criterion
     *                      - CriterionType.
     */
    template <typename T>
    struct parameterManagerElementSupported;

    static const char *const mPolicyPfwDefaultConfFileName; /**< Default Policy PFW top file name.*/
};

} // namespace audio_policy
} // namespace android
