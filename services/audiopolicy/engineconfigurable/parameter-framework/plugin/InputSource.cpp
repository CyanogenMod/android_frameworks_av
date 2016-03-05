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

#include "InputSource.h"
#include "PolicyMappingKeys.h"
#include "PolicySubsystem.h"

using std::string;

InputSource::InputSource(const string &mappingValue,
                         CInstanceConfigurableElement *instanceConfigurableElement,
                         const CMappingContext &context, core::log::Logger &logger)
    : CFormattedSubsystemObject(instanceConfigurableElement,
                                logger,
                                mappingValue,
                                MappingKeyAmend1,
                                (MappingKeyAmendEnd - MappingKeyAmend1 + 1),
                                context),
      mPolicySubsystem(static_cast<const PolicySubsystem *>(
                           instanceConfigurableElement->getBelongingSubsystem())),
      mPolicyPluginInterface(mPolicySubsystem->getPolicyPluginInterface())
{
    mId = static_cast<audio_source_t>(context.getItemAsInteger(MappingKeyIdentifier));
    // Declares the strategy to audio policy engine
    mPolicyPluginInterface->addInputSource(getFormattedMappingValue(), mId);
}

bool InputSource::sendToHW(string & /*error*/)
{
    uint32_t applicableInputDevice;
    blackboardRead(&applicableInputDevice, sizeof(applicableInputDevice));
    return mPolicyPluginInterface->setDeviceForInputSource(mId, applicableInputDevice);
}
