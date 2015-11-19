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

#include "AudioPolicyConfig.h"
#include <utils/StrongPointer.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <string>
#include <sstream>
#include <fstream>

struct _xmlNode;
struct _xmlDoc;

namespace android {

struct AudioGainTraits
{
    static const char *const tag;
    static const char *const collectionTag;

    struct Attributes
    {
        static const char mode[]; /**< gain modes supported, e.g. AUDIO_GAIN_MODE_CHANNELS. */
        /** controlled channels, needed if mode AUDIO_GAIN_MODE_CHANNELS. */
        static const char channelMask[];
        static const char minValueMB[]; /**< min value in millibel. */
        static const char maxValueMB[]; /**< max value in millibel. */
        static const char defaultValueMB[]; /**< default value in millibel. */
        static const char stepValueMB[]; /**< step value in millibel. */
        static const char minRampMs[]; /**< needed if mode AUDIO_GAIN_MODE_RAMP. */
        static const char maxRampMs[]; /**< .needed if mode AUDIO_GAIN_MODE_RAMP */
    };

    typedef AudioGain Element;
    typedef sp<Element> PtrElement;
    typedef AudioGainCollection Collection;
    typedef void *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx serializingContext);

    // Gain has no child
};

// A profile section contains a name,  one audio format and the list of supported sampling rates
// and channel masks for this format
struct AudioProfileTraits
{
    static const char *const tag;
    static const char *const collectionTag;

    struct Attributes
    {
        static const char name[];
        static const char samplingRates[];
        static const char format[];
        static const char channelMasks[];
    };

    typedef AudioProfile Element;
    typedef sp<AudioProfile> PtrElement;
    typedef AudioProfileVector Collection;
    typedef void *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx serializingContext);
};

struct MixPortTraits
{
    static const char *const tag;
    static const char *const collectionTag;

    struct Attributes
    {
        static const char name[];
        static const char role[];
        static const char flags[];
    };

    typedef IOProfile Element;
    typedef sp<Element> PtrElement;
    typedef IOProfileCollection Collection;
    typedef void *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx serializingContext);

    // Children are: GainTraits
};

struct DevicePortTraits
{
    static const char *const tag;
    static const char *const collectionTag;

    struct Attributes
    {
        static const char tagName[]; /**<  <device tag name>: any string without space. */
        static const char type[]; /**< <device type>. */
        static const char role[]; /**< <device role: sink or source>. */
        static const char roleSource[]; /**< <attribute role source value>. */
        static const char address[]; /**< optional: device address, char string less than 64. */
    };
    typedef DeviceDescriptor Element;
    typedef sp<DeviceDescriptor> PtrElement;
    typedef DeviceVector Collection;
    typedef void *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx serializingContext);
    // Children are: GainTraits (optionnal)
};

struct RouteTraits
{
    static const char *const tag;
    static const char *const collectionTag;

    struct Attributes
    {
        static const char type[]; /**< <route type>: mix or mux. */
        static const char typeMix[]; /**< type attribute mix value. */
        static const char sink[]; /**< <sink: involved in this route>. */
        static const char sources[]; /**< sources: all source that can be involved in this route. */
    };
    typedef AudioRoute Element;
    typedef sp<AudioRoute> PtrElement;
    typedef AudioRouteVector Collection;
    typedef HwModule *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx ctx);
};

struct ModuleTraits
{
    static const char *const tag;
    static const char *const collectionTag;

    static const char *const childAttachedDevicesTag;
    static const char *const childAttachedDeviceTag;
    static const char *const childDefaultOutputDeviceTag;

    struct Attributes
    {
        static const char name[];
        static const char version[];
    };

    typedef HwModule Element;
    typedef sp<Element> PtrElement;
    typedef HwModuleCollection Collection;
    typedef AudioPolicyConfig *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx serializingContext);

    // Children are: mixPortTraits, devicePortTraits and routeTraits
    // Need to call deserialize on each child
};

struct GlobalConfigTraits
{
    static const char *const tag;

    struct Attributes
    {
        static const char speakerDrcEnabled[];
    };

    static status_t deserialize(const _xmlNode *root, AudioPolicyConfig &config);
};

struct VolumeTraits
{
    static const char *const tag;
    static const char *const collectionTag;
    static const char *const volumePointTag;

    struct Attributes
    {
        static const char stream[];
        static const char deviceCategory[];
    };

    typedef VolumeCurve Element;
    typedef sp<VolumeCurve> PtrElement;
    typedef VolumeCurveCollection Collection;
    typedef void *PtrSerializingCtx;

    static status_t deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                PtrSerializingCtx serializingContext);

    // No Child
};

class PolicySerializer
{
private:
    static const char *const rootName;

    static const char *const versionAttribute;
    static const uint32_t gMajor; /**< the major number of the policy xml format version. */
    static const uint32_t gMinor; /**< the minor number of the policy xml format version. */

public:
    PolicySerializer();
    status_t deserialize(const char *str, AudioPolicyConfig &config);

private:
    typedef AudioPolicyConfig Element;

    std::string mRootElementName;
    std::string mVersion;

    // Children are: ModulesTraits, VolumeTraits
};

}; // namespace android
