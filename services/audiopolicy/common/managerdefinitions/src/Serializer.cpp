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

#define LOG_TAG "APM::Serializer"
//#define LOG_NDEBUG 0

#include "Serializer.h"
#include <convert/convert.h>
#include "TypeConverter.h"
#include <libxml/parser.h>
#include <libxml/xinclude.h>
#include <string>
#include <sstream>
#include <istream>

using std::string;

namespace android {

string getXmlAttribute(const xmlNode *cur, const char *attribute)
{
    xmlChar *xmlValue = xmlGetProp(cur, (const xmlChar*)attribute);
    if (xmlValue == NULL) {
        return "";
    }
    string value((const char*)xmlValue);
    xmlFree(xmlValue);
    return value;
}

using utilities::convertTo;

const char *const PolicySerializer::rootName = "audioPolicyConfiguration";
const char *const PolicySerializer::versionAttribute = "version";
const uint32_t PolicySerializer::gMajor = 1;
const uint32_t PolicySerializer::gMinor = 0;
static const char *const gReferenceElementName = "reference";
static const char *const gReferenceAttributeName = "name";

static void getReference(const _xmlNode *root, const _xmlNode *&refNode, const string &refName)
{
    const _xmlNode *cur = root->xmlChildrenNode;
    while (cur != NULL) {
        if ((!xmlStrcmp(cur->name, (const xmlChar *)gReferenceElementName))) {
            string name = getXmlAttribute(cur, gReferenceAttributeName);
              if (refName == name) {
                  refNode = cur;
                  return;
              }
        }
        cur = cur->next;
    }
    return;
}

template <class Trait>
static status_t deserializeCollection(_xmlDoc *doc, const _xmlNode *cur,
                                      typename Trait::Collection &collection,
                                      typename Trait::PtrSerializingCtx serializingContext)
{
    const xmlNode *root = cur->xmlChildrenNode;
    while (root != NULL) {
        if (xmlStrcmp(root->name, (const xmlChar *)Trait::collectionTag) &&
                xmlStrcmp(root->name, (const xmlChar *)Trait::tag)) {
            root = root->next;
            continue;
        }
        const xmlNode *child = root;
        if (!xmlStrcmp(child->name, (const xmlChar *)Trait::collectionTag)) {
            child = child->xmlChildrenNode;
        }
        while (child != NULL) {
            if (!xmlStrcmp(child->name, (const xmlChar *)Trait::tag)) {
                typename Trait::PtrElement element;
                status_t status = Trait::deserialize(doc, child, element, serializingContext);
                if (status != NO_ERROR) {
                    return status;
                }
                if (collection.add(element) < 0) {
                    ALOGE("%s: could not add element to %s collection", __FUNCTION__,
                          Trait::collectionTag);
                }
            }
            child = child->next;
        }
        if (!xmlStrcmp(root->name, (const xmlChar *)Trait::tag)) {
            return NO_ERROR;
        }
        root = root->next;
    }
    return NO_ERROR;
}

const char *const AudioGainTraits::tag = "gain";
const char *const AudioGainTraits::collectionTag = "gains";

const char AudioGainTraits::Attributes::mode[] = "mode";
const char AudioGainTraits::Attributes::channelMask[] = "channel_mask";
const char AudioGainTraits::Attributes::minValueMB[] = "minValueMB";
const char AudioGainTraits::Attributes::maxValueMB[] = "maxValueMB";
const char AudioGainTraits::Attributes::defaultValueMB[] = "defaultValueMB";
const char AudioGainTraits::Attributes::stepValueMB[] = "stepValueMB";
const char AudioGainTraits::Attributes::minRampMs[] = "minRampMs";
const char AudioGainTraits::Attributes::maxRampMs[] = "maxRampMs";

status_t AudioGainTraits::deserialize(_xmlDoc */*doc*/, const _xmlNode *root, PtrElement &gain,
                                      PtrSerializingCtx /*serializingContext*/)
{
    static uint32_t index = 0;
    gain = new Element(index++, true);

    string mode = getXmlAttribute(root, Attributes::mode);
    if (!mode.empty()) {
        gain->setMode(GainModeConverter::maskFromString(mode));
    }

    string channelsLiteral = getXmlAttribute(root, Attributes::channelMask);
    if (!channelsLiteral.empty()) {
        gain->setChannelMask(channelMaskFromString(channelsLiteral));
    }

    string minValueMBLiteral = getXmlAttribute(root, Attributes::minValueMB);
    uint32_t minValueMB;
    if (!minValueMBLiteral.empty() && convertTo(minValueMBLiteral, minValueMB)) {
        gain->setMinValueInMb(minValueMB);
    }

    string maxValueMBLiteral = getXmlAttribute(root, Attributes::maxValueMB);
    uint32_t maxValueMB;
    if (!maxValueMBLiteral.empty() && convertTo(maxValueMBLiteral, maxValueMB)) {
        gain->setMaxValueInMb(maxValueMB);
    }

    string defaultValueMBLiteral = getXmlAttribute(root, Attributes::defaultValueMB);
    uint32_t defaultValueMB;
    if (!defaultValueMBLiteral.empty() && convertTo(defaultValueMBLiteral, defaultValueMB)) {
        gain->setDefaultValueInMb(defaultValueMB);
    }

    string stepValueMBLiteral = getXmlAttribute(root, Attributes::stepValueMB);
    uint32_t stepValueMB;
    if (!stepValueMBLiteral.empty() && convertTo(stepValueMBLiteral, stepValueMB)) {
        gain->setStepValueInMb(stepValueMB);
    }

    string minRampMsLiteral = getXmlAttribute(root, Attributes::minRampMs);
    uint32_t minRampMs;
    if (!minRampMsLiteral.empty() && convertTo(minRampMsLiteral, minRampMs)) {
        gain->setMinRampInMs(minRampMs);
    }

    string maxRampMsLiteral = getXmlAttribute(root, Attributes::maxRampMs);
    uint32_t maxRampMs;
    if (!maxRampMsLiteral.empty() && convertTo(maxRampMsLiteral, maxRampMs)) {
        gain->setMaxRampInMs(maxRampMs);
    }
    ALOGV("%s: adding new gain mode %08x channel mask %08x min mB %d max mB %d", __FUNCTION__,
          gain->getMode(), gain->getChannelMask(), gain->getMinValueInMb(),
          gain->getMaxValueInMb());

    if (gain->getMode() == 0) {
        return BAD_VALUE;
    }
    return NO_ERROR;
}

const char *const AudioProfileTraits::collectionTag = "profiles";
const char *const AudioProfileTraits::tag = "profile";

const char AudioProfileTraits::Attributes::name[] = "name";
const char AudioProfileTraits::Attributes::samplingRates[] = "samplingRates";
const char AudioProfileTraits::Attributes::format[] = "format";
const char AudioProfileTraits::Attributes::channelMasks[] = "channelMasks";

status_t AudioProfileTraits::deserialize(_xmlDoc */*doc*/, const _xmlNode *root, PtrElement &profile,
                                         PtrSerializingCtx /*serializingContext*/)
{
    string samplingRates = getXmlAttribute(root, Attributes::samplingRates);
    string format = getXmlAttribute(root, Attributes::format);
    string channels = getXmlAttribute(root, Attributes::channelMasks);

    profile = new Element(formatFromString(format), channelMasksFromString(channels, ","),
                          samplingRatesFromString(samplingRates, ","));

    profile->setDynamicFormat(profile->getFormat() == gDynamicFormat);
    profile->setDynamicChannels(profile->getChannels().isEmpty());
    profile->setDynamicRate(profile->getSampleRates().isEmpty());

    return NO_ERROR;
}


const char *const MixPortTraits::collectionTag = "mixPorts";
const char *const MixPortTraits::tag = "mixPort";

const char MixPortTraits::Attributes::name[] = "name";
const char MixPortTraits::Attributes::role[] = "role";
const char MixPortTraits::Attributes::flags[] = "flags";

status_t MixPortTraits::deserialize(_xmlDoc *doc, const _xmlNode *child, PtrElement &mixPort,
                                    PtrSerializingCtx /*serializingContext*/)
{
    string name = getXmlAttribute(child, Attributes::name);
    if (name.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::name);
        return BAD_VALUE;
    }
    ALOGV("%s: %s %s=%s", __FUNCTION__, tag, Attributes::name, name.c_str());
    string role = getXmlAttribute(child, Attributes::role);
    if (role.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::role);
        return BAD_VALUE;
    }
    ALOGV("%s: Role=%s", __FUNCTION__, role.c_str());
    audio_port_role_t portRole = role == "source" ? AUDIO_PORT_ROLE_SOURCE : AUDIO_PORT_ROLE_SINK;

    mixPort = new Element(String8(name.c_str()), portRole);

    AudioProfileTraits::Collection profiles;
    deserializeCollection<AudioProfileTraits>(doc, child, profiles, NULL);
    if (profiles.isEmpty()) {
        sp <AudioProfile> dynamicProfile = new AudioProfile(gDynamicFormat,
                                                            ChannelsVector(), SampleRateVector());
        dynamicProfile->setDynamicFormat(true);
        dynamicProfile->setDynamicChannels(true);
        dynamicProfile->setDynamicRate(true);
        profiles.add(dynamicProfile);
    }
    mixPort->setAudioProfiles(profiles);

    string flags = getXmlAttribute(child, Attributes::flags);
    if (!flags.empty()) {
        // Source role
        if (portRole == AUDIO_PORT_ROLE_SOURCE) {
            mixPort->setFlags(OutputFlagConverter::maskFromString(flags));
        } else {
            // Sink role
            mixPort->setFlags(InputFlagConverter::maskFromString(flags));
        }
    }
    // Deserialize children
    AudioGainTraits::Collection gains;
    deserializeCollection<AudioGainTraits>(doc, child, gains, NULL);
    mixPort->setGains(gains);

    return NO_ERROR;
}

const char *const DevicePortTraits::tag = "devicePort";
const char *const DevicePortTraits::collectionTag = "devicePorts";

const char DevicePortTraits::Attributes::tagName[] = "tagName";
const char DevicePortTraits::Attributes::type[] = "type";
const char DevicePortTraits::Attributes::role[] = "role";
const char DevicePortTraits::Attributes::address[] = "address";
const char DevicePortTraits::Attributes::roleSource[] = "source";

status_t DevicePortTraits::deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &deviceDesc,
                                       PtrSerializingCtx /*serializingContext*/)
{
    string name = getXmlAttribute(root, Attributes::tagName);
    if (name.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::tagName);
        return BAD_VALUE;
    }
    ALOGV("%s: %s %s=%s", __FUNCTION__, tag, Attributes::tagName, name.c_str());
    string typeName = getXmlAttribute(root, Attributes::type);
    if (typeName.empty()) {
        ALOGE("%s: no type for %s", __FUNCTION__, name.c_str());
        return BAD_VALUE;
    }
    ALOGV("%s: %s %s=%s", __FUNCTION__, tag, Attributes::type, typeName.c_str());
    string role = getXmlAttribute(root, Attributes::role);
    if (role.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::role);
        return BAD_VALUE;
    }
    ALOGV("%s: %s %s=%s", __FUNCTION__, tag, Attributes::role, role.c_str());
    audio_port_role_t portRole = (role == Attributes::roleSource) ?
                AUDIO_PORT_ROLE_SOURCE : AUDIO_PORT_ROLE_SINK;

    audio_devices_t type = AUDIO_DEVICE_NONE;
    if (!DeviceConverter::fromString(typeName, type) ||
            (!audio_is_input_device(type) && portRole == AUDIO_PORT_ROLE_SOURCE) ||
            (!audio_is_output_devices(type) && portRole == AUDIO_PORT_ROLE_SINK)) {
        ALOGW("%s: bad type %08x", __FUNCTION__, type);
        return BAD_VALUE;
    }
    deviceDesc = new Element(type, String8(name.c_str()));

    string address = getXmlAttribute(root, Attributes::address);
    if (!address.empty()) {
        ALOGV("%s: address=%s for %s", __FUNCTION__, address.c_str(), name.c_str());
        deviceDesc->mAddress = String8(address.c_str());
    }

    AudioProfileTraits::Collection profiles;
    deserializeCollection<AudioProfileTraits>(doc, root, profiles, NULL);
    if (profiles.isEmpty()) {
        sp <AudioProfile> dynamicProfile = new AudioProfile(gDynamicFormat,
                                                            ChannelsVector(), SampleRateVector());
        dynamicProfile->setDynamicFormat(true);
        dynamicProfile->setDynamicChannels(true);
        dynamicProfile->setDynamicRate(true);
        profiles.add(dynamicProfile);
    }
    deviceDesc->setAudioProfiles(profiles);

    // Deserialize AudioGain children
    deserializeCollection<AudioGainTraits>(doc, root, deviceDesc->mGains, NULL);
    ALOGV("%s: adding device tag %s type %08x address %s", __FUNCTION__,
          deviceDesc->getName().string(), type, deviceDesc->mAddress.string());
    return NO_ERROR;
}

const char *const RouteTraits::tag = "route";
const char *const RouteTraits::collectionTag = "routes";

const char RouteTraits::Attributes::type[] = "type";
const char RouteTraits::Attributes::typeMix[] = "mix";
const char RouteTraits::Attributes::sink[] = "sink";
const char RouteTraits::Attributes::sources[] = "sources";


status_t RouteTraits::deserialize(_xmlDoc */*doc*/, const _xmlNode *root, PtrElement &element,
                                  PtrSerializingCtx ctx)
{
    string type = getXmlAttribute(root, Attributes::type);
    if (type.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::type);
        return BAD_VALUE;
    }
    audio_route_type_t routeType = (type == Attributes::typeMix) ?
                AUDIO_ROUTE_MIX : AUDIO_ROUTE_MUX;

    ALOGV("%s: %s %s=%s", __FUNCTION__, tag, Attributes::type, type.c_str());
    element = new Element(routeType);

    string sinkAttr = getXmlAttribute(root, Attributes::sink);
    if (sinkAttr.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::sink);
        return BAD_VALUE;
    }
    // Convert Sink name to port pointer
    sp<AudioPort> sink = ctx->findPortByTagName(String8(sinkAttr.c_str()));
    if (sink == NULL) {
        ALOGE("%s: no sink found with name=%s", __FUNCTION__, sinkAttr.c_str());
        return BAD_VALUE;
    }
    element->setSink(sink);

    string sourcesAttr = getXmlAttribute(root, Attributes::sources);
    if (sourcesAttr.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::sources);
        return BAD_VALUE;
    }
    // Tokenize and Convert Sources name to port pointer
    AudioPortVector sources;
    char *sourcesLiteral = strndup(sourcesAttr.c_str(), strlen(sourcesAttr.c_str()));
    char *devTag = strtok(sourcesLiteral, ",");
    while (devTag != NULL) {
        if (strlen(devTag) != 0) {
            sp<AudioPort> source = ctx->findPortByTagName(String8(devTag));
            if (source == NULL) {
                ALOGE("%s: no source found with name=%s", __FUNCTION__, devTag);
                return BAD_VALUE;
            }
            sources.add(source);
        }
        devTag = strtok(NULL, ",");
    }
    free(sourcesLiteral);

    sink->addRoute(element);
    for (size_t i = 0; i < sources.size(); i++) {
        sp<AudioPort> source = sources.itemAt(i);
        source->addRoute(element);
    }
    element->setSources(sources);
    return NO_ERROR;
}

const char *const ModuleTraits::childAttachedDevicesTag = "attachedDevices";
const char *const ModuleTraits::childAttachedDeviceTag = "item";
const char *const ModuleTraits::childDefaultOutputDeviceTag = "defaultOutputDevice";

const char *const ModuleTraits::tag = "module";
const char *const ModuleTraits::collectionTag = "modules";

const char ModuleTraits::Attributes::name[] = "name";
const char ModuleTraits::Attributes::version[] = "halVersion";

status_t ModuleTraits::deserialize(xmlDocPtr doc, const xmlNode *root, PtrElement &module,
                                   PtrSerializingCtx ctx)
{
    string name = getXmlAttribute(root, Attributes::name);
    if (name.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::name);
        return BAD_VALUE;
    }
    uint32_t version = AUDIO_DEVICE_API_VERSION_MIN;
    string versionLiteral = getXmlAttribute(root, Attributes::version);
    if (!versionLiteral.empty()) {
        uint32_t major, minor;
        sscanf(versionLiteral.c_str(), "%u.%u", &major, &minor);
        version = HARDWARE_DEVICE_API_VERSION(major, minor);
        ALOGV("%s: mHalVersion = %04x major %u minor %u",  __FUNCTION__,
              version, major, minor);
    }

    ALOGV("%s: %s %s=%s", __FUNCTION__, tag, Attributes::name, name.c_str());

    module = new Element(name.c_str(), version);

    // Deserialize childrens: Audio Mix Port, Audio Device Ports (Source/Sink), Audio Routes
    MixPortTraits::Collection mixPorts;
    deserializeCollection<MixPortTraits>(doc, root, mixPorts, NULL);
    module->setProfiles(mixPorts);

    DevicePortTraits::Collection devicePorts;
    deserializeCollection<DevicePortTraits>(doc, root, devicePorts, NULL);
    module->setDeclaredDevices(devicePorts);

    RouteTraits::Collection routes;
    deserializeCollection<RouteTraits>(doc, root, routes, module.get());
    module->setRoutes(routes);

    const xmlNode *children = root->xmlChildrenNode;
    while (children != NULL) {
        if (!xmlStrcmp(children->name, (const xmlChar *)childAttachedDevicesTag)) {
            ALOGV("%s: %s %s found", __FUNCTION__, tag, childAttachedDevicesTag);
            const xmlNode *child = children->xmlChildrenNode;
            while (child != NULL) {
                if (!xmlStrcmp(child->name, (const xmlChar *)childAttachedDeviceTag)) {
                    xmlChar *attachedDevice = xmlNodeListGetString(doc, child->xmlChildrenNode, 1);
                    if (attachedDevice != NULL) {
                        ALOGV("%s: %s %s=%s", __FUNCTION__, tag, childAttachedDeviceTag,
                              (const char*)attachedDevice);
                        sp<DeviceDescriptor> device =
                                module->getDeclaredDevices().getDeviceFromTagName(String8((const char*)attachedDevice));
                        ctx->addAvailableDevice(device);
                        xmlFree(attachedDevice);
                    }
                }
                child = child->next;
            }
        }
        if (!xmlStrcmp(children->name, (const xmlChar *)childDefaultOutputDeviceTag)) {
            xmlChar *defaultOutputDevice = xmlNodeListGetString(doc, children->xmlChildrenNode, 1);;
            if (defaultOutputDevice != NULL) {
                ALOGV("%s: %s %s=%s", __FUNCTION__, tag, childDefaultOutputDeviceTag,
                      (const char*)defaultOutputDevice);
                sp<DeviceDescriptor> device =
                        module->getDeclaredDevices().getDeviceFromTagName(String8((const char*)defaultOutputDevice));
                if (device != 0 && ctx->getDefaultOutputDevice() == 0) {
                    ctx->setDefaultOutputDevice(device);
                    ALOGV("%s: default is %08x", __FUNCTION__, ctx->getDefaultOutputDevice()->type());
                }
                xmlFree(defaultOutputDevice);
            }
        }
        children = children->next;
    }
    return NO_ERROR;
}

const char *const GlobalConfigTraits::tag = "globalConfiguration";

const char GlobalConfigTraits::Attributes::speakerDrcEnabled[] = "speaker_drc_enabled";


status_t GlobalConfigTraits::deserialize(const xmlNode *cur, AudioPolicyConfig &config)
{
    const xmlNode *root = cur->xmlChildrenNode;
    while (root != NULL) {
        if (!xmlStrcmp(root->name, (const xmlChar *)tag)) {
            string speakerDrcEnabled =
                    getXmlAttribute(root, Attributes::speakerDrcEnabled);
            bool isSpeakerDrcEnabled;
            if (!speakerDrcEnabled.empty() &&
                    convertTo<string, bool>(speakerDrcEnabled, isSpeakerDrcEnabled)) {
                config.setSpeakerDrcEnabled(isSpeakerDrcEnabled);
            }
            return NO_ERROR;
        }
        root = root->next;
    }
    return NO_ERROR;
}


const char *const VolumeTraits::tag = "volume";
const char *const VolumeTraits::collectionTag = "volumes";
const char *const VolumeTraits::volumePointTag = "point";

const char VolumeTraits::Attributes::stream[] = "stream";
const char VolumeTraits::Attributes::deviceCategory[] = "deviceCategory";
const char VolumeTraits::Attributes::reference[] = "ref";

status_t VolumeTraits::deserialize(_xmlDoc *doc, const _xmlNode *root, PtrElement &element,
                                   PtrSerializingCtx /*serializingContext*/)
{
    string streamTypeLiteral = getXmlAttribute(root, Attributes::stream);
    if (streamTypeLiteral.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::stream);
        return BAD_VALUE;
    }
    audio_stream_type_t streamType;
    if (!StreamTypeConverter::fromString(streamTypeLiteral, streamType)) {
        ALOGE("%s: Invalid %s", __FUNCTION__, Attributes::stream);
        return BAD_VALUE;
    }
    string deviceCategoryLiteral = getXmlAttribute(root, Attributes::deviceCategory);
    if (deviceCategoryLiteral.empty()) {
        ALOGE("%s: No %s found", __FUNCTION__, Attributes::deviceCategory);
        return BAD_VALUE;
    }
    device_category deviceCategory;
    if (!DeviceCategoryConverter::fromString(deviceCategoryLiteral, deviceCategory)) {
        ALOGE("%s: Invalid %s=%s", __FUNCTION__, Attributes::deviceCategory,
              deviceCategoryLiteral.c_str());
        return BAD_VALUE;
    }

    string referenceName = getXmlAttribute(root, Attributes::reference);
    const _xmlNode *ref = NULL;
    if (!referenceName.empty()) {
        getReference(root->parent, ref, referenceName);
        if (ref == NULL) {
            ALOGE("%s: No reference Ptr found for %s", __FUNCTION__, referenceName.c_str());
            return BAD_VALUE;
        }
    }

    element = new Element(deviceCategory, streamType);

    const xmlNode *child = referenceName.empty() ? root->xmlChildrenNode : ref->xmlChildrenNode;
    while (child != NULL) {
        if (!xmlStrcmp(child->name, (const xmlChar *)volumePointTag)) {
            xmlChar *pointDefinition = xmlNodeListGetString(doc, child->xmlChildrenNode, 1);;
            if (pointDefinition == NULL) {
                return BAD_VALUE;
            }
            ALOGV("%s: %s=%s", __FUNCTION__, tag, (const char*)pointDefinition);
            Vector<int32_t> point;
            collectionFromString<DefaultTraits<int32_t> >((const char*)pointDefinition, point, ",");
            if (point.size() != 2) {
                ALOGE("%s: Invalid %s: %s", __FUNCTION__, volumePointTag,
                      (const char*)pointDefinition);
                return BAD_VALUE;
            }
            element->add(CurvePoint(point[0], point[1]));
            xmlFree(pointDefinition);
        }
        child = child->next;
    }
    return NO_ERROR;
}

PolicySerializer::PolicySerializer() : mRootElementName(rootName)
{
    std::ostringstream oss;
    oss << gMajor << "." << gMinor;
    mVersion = oss.str();
    ALOGV("%s: Version=%s Root=%s", __FUNCTION__, mVersion.c_str(), mRootElementName.c_str());
}

status_t PolicySerializer::deserialize(const char *configFile, AudioPolicyConfig &config)
{
    xmlDocPtr doc;
    doc = xmlParseFile(configFile);
    if (doc == NULL) {
        ALOGE("%s: Could not parse %s document.", __FUNCTION__, configFile);
        return BAD_VALUE;
    }
    xmlNodePtr cur = xmlDocGetRootElement(doc);
    if (cur == NULL) {
        ALOGE("%s: Could not parse %s document: empty.", __FUNCTION__, configFile);
        xmlFreeDoc(doc);
        return BAD_VALUE;
    }
    if (xmlXIncludeProcess(doc) < 0) {
         ALOGE("%s: libxml failed to resolve XIncludes on %s document.", __FUNCTION__, configFile);
    }

    if (xmlStrcmp(cur->name, (const xmlChar *) mRootElementName.c_str()))  {
        ALOGE("%s: No %s root element found in xml data %s.", __FUNCTION__, mRootElementName.c_str(),
              (const char *)cur->name);
        xmlFreeDoc(doc);
        return BAD_VALUE;
    }

    string version = getXmlAttribute(cur, versionAttribute);
    if (version.empty()) {
        ALOGE("%s: No version found in root node %s", __FUNCTION__, mRootElementName.c_str());
        return BAD_VALUE;
    }
    if (version != mVersion) {
        ALOGE("%s: Version does not match; expect %s got %s", __FUNCTION__, mVersion.c_str(),
              version.c_str());
        return BAD_VALUE;
    }
    // Lets deserialize children
    // Modules
    ModuleTraits::Collection modules;
    deserializeCollection<ModuleTraits>(doc, cur, modules, &config);
    config.setHwModules(modules);

    // deserialize volume section
    VolumeTraits::Collection volumes;
    deserializeCollection<VolumeTraits>(doc, cur, volumes, &config);
    config.setVolumes(volumes);

    // Global Configuration
    GlobalConfigTraits::deserialize(cur, config);

    xmlFreeDoc(doc);
    return android::OK;
}

}; // namespace android
