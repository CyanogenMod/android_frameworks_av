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

#include "VolumeProfile.h"
#include "PolicyMappingKeys.h"
#include "PolicySubsystem.h"
#include "ParameterBlockType.h"
#include <Volume.h>
#include <math.h>

using std::string;

VolumeProfile::VolumeProfile(const string &mappingValue,
                             CInstanceConfigurableElement *instanceConfigurableElement,
                             const CMappingContext &context)
    : CFormattedSubsystemObject(instanceConfigurableElement,
                                mappingValue,
                                MappingKeyAmend1,
                                (MappingKeyAmendEnd - MappingKeyAmend1 + 1),
                                context),
      mPolicySubsystem(static_cast<const PolicySubsystem *>(
                           instanceConfigurableElement->getBelongingSubsystem())),
      mPolicyPluginInterface(mPolicySubsystem->getPolicyPluginInterface())
{
    uint32_t categoryKey = context.getItemAsInteger(MappingKeyCategory);
    if (categoryKey >= Volume::DEVICE_CATEGORY_CNT) {
        mCategory = Volume::DEVICE_CATEGORY_SPEAKER;
    } else {
        mCategory = static_cast<Volume::device_category>(categoryKey);
    }
    mId = static_cast<audio_stream_type_t>(context.getItemAsInteger(MappingKeyIdentifier));

    // (no exception support, defer the error)
    if (instanceConfigurableElement->getType() != CInstanceConfigurableElement::EParameterBlock) {
        return;
    }
    // Get actual element type
    const CParameterBlockType *parameterType = static_cast<const CParameterBlockType *>(
                instanceConfigurableElement->getTypeElement());
    mPoints = parameterType->getArrayLength();
}

bool VolumeProfile::receiveFromHW(string & /*error*/)
{
    return true;
}

bool VolumeProfile::sendToHW(string & /*error*/)
{
    Point points[mPoints];
    blackboardRead(&points, sizeof(Point) * mPoints);

    VolumeCurvePoints pointsVector;
    for (size_t i = 0; i < mPoints; i++) {
        VolumeCurvePoint curvePoint;
        curvePoint.mIndex = points[i].index;
        curvePoint.mDBAttenuation = static_cast<float>(points[i].dbAttenuation) /
                (1UL << gFractional);
        pointsVector.push_back(curvePoint);
    }
    return mPolicyPluginInterface->setVolumeProfileForStream(mId, mCategory, pointsVector);
}
