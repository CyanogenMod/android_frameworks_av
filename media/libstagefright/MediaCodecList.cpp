/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaCodecList"
#include <utils/Log.h>

#include <media/stagefright/MediaCodecList.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <utils/threads.h>

#include <libexpat/expat.h>

namespace android {

static Mutex sInitMutex;

// static
MediaCodecList *MediaCodecList::sCodecList;

// static
const MediaCodecList *MediaCodecList::getInstance() {
    Mutex::Autolock autoLock(sInitMutex);

    if (sCodecList == NULL) {
        sCodecList = new MediaCodecList;
    }

    return sCodecList->initCheck() == OK ? sCodecList : NULL;
}

MediaCodecList::MediaCodecList()
    : mInitCheck(NO_INIT) {
    parseTopLevelXMLFile("/etc/media_codecs.xml");
}

void MediaCodecList::parseTopLevelXMLFile(const char *codecs_xml) {
    // get href_base
    char *href_base_end = strrchr(codecs_xml, '/');
    if (href_base_end != NULL) {
        mHrefBase = AString(codecs_xml, href_base_end - codecs_xml + 1);
    }

    mInitCheck = OK;
    mCurrentSection = SECTION_TOPLEVEL;
    mDepth = 0;

    parseXMLFile(codecs_xml);

    if (mInitCheck != OK) {
        mCodecInfos.clear();
        mCodecQuirks.clear();
        return;
    }

    for (size_t i = mCodecInfos.size(); i-- > 0;) {
        CodecInfo *info = &mCodecInfos.editItemAt(i);

        if (info->mTypes == 0) {
            // No types supported by this component???
            ALOGW("Component %s does not support any type of media?",
                  info->mName.c_str());

            mCodecInfos.removeAt(i);
#if LOG_NDEBUG == 0
        } else {
            for (size_t type_ix = 0; type_ix < mTypes.size(); ++type_ix) {
                uint32_t typeMask = 1ul << mTypes.valueAt(type_ix);
                if (info->mTypes & typeMask) {
                    AString mime = mTypes.keyAt(type_ix);
                    uint32_t bit = mTypes.valueAt(type_ix);

                    ALOGV("%s codec info for %s: %s", info->mName.c_str(), mime.c_str(),
                            info->mCaps.editValueFor(bit)->debugString().c_str());
                }
            }
#endif
        }
    }

#if 0
    for (size_t i = 0; i < mCodecInfos.size(); ++i) {
        const CodecInfo &info = mCodecInfos.itemAt(i);

        AString line = info.mName;
        line.append(" supports ");
        for (size_t j = 0; j < mTypes.size(); ++j) {
            uint32_t value = mTypes.valueAt(j);

            if (info.mTypes & (1ul << value)) {
                line.append(mTypes.keyAt(j));
                line.append(" ");
            }
        }

        ALOGI("%s", line.c_str());
    }
#endif
}

MediaCodecList::~MediaCodecList() {
}

status_t MediaCodecList::initCheck() const {
    return mInitCheck;
}

void MediaCodecList::parseXMLFile(const char *path) {
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        ALOGW("unable to open media codecs configuration xml file: %s", path);
        mInitCheck = NAME_NOT_FOUND;
        return;
    }

    XML_Parser parser = ::XML_ParserCreate(NULL);
    CHECK(parser != NULL);

    ::XML_SetUserData(parser, this);
    ::XML_SetElementHandler(
            parser, StartElementHandlerWrapper, EndElementHandlerWrapper);

    const int BUFF_SIZE = 512;
    while (mInitCheck == OK) {
        void *buff = ::XML_GetBuffer(parser, BUFF_SIZE);
        if (buff == NULL) {
            ALOGE("failed in call to XML_GetBuffer()");
            mInitCheck = UNKNOWN_ERROR;
            break;
        }

        int bytes_read = ::fread(buff, 1, BUFF_SIZE, file);
        if (bytes_read < 0) {
            ALOGE("failed in call to read");
            mInitCheck = ERROR_IO;
            break;
        }

        XML_Status status = ::XML_ParseBuffer(parser, bytes_read, bytes_read == 0);
        if (status != XML_STATUS_OK) {
            ALOGE("malformed (%s)", ::XML_ErrorString(::XML_GetErrorCode(parser)));
            mInitCheck = ERROR_MALFORMED;
            break;
        }

        if (bytes_read == 0) {
            break;
        }
    }

    ::XML_ParserFree(parser);

    fclose(file);
    file = NULL;
}

// static
void MediaCodecList::StartElementHandlerWrapper(
        void *me, const char *name, const char **attrs) {
    static_cast<MediaCodecList *>(me)->startElementHandler(name, attrs);
}

// static
void MediaCodecList::EndElementHandlerWrapper(void *me, const char *name) {
    static_cast<MediaCodecList *>(me)->endElementHandler(name);
}

status_t MediaCodecList::includeXMLFile(const char **attrs) {
    const char *href = NULL;
    size_t i = 0;
    while (attrs[i] != NULL) {
        if (!strcmp(attrs[i], "href")) {
            if (attrs[i + 1] == NULL) {
                return -EINVAL;
            }
            href = attrs[i + 1];
            ++i;
        } else {
            return -EINVAL;
        }
        ++i;
    }

    // For security reasons and for simplicity, file names can only contain
    // [a-zA-Z0-9_.] and must start with  media_codecs_ and end with .xml
    for (i = 0; href[i] != '\0'; i++) {
        if (href[i] == '.' || href[i] == '_' ||
                (href[i] >= '0' && href[i] <= '9') ||
                (href[i] >= 'A' && href[i] <= 'Z') ||
                (href[i] >= 'a' && href[i] <= 'z')) {
            continue;
        }
        ALOGE("invalid include file name: %s", href);
        return -EINVAL;
    }

    AString filename = href;
    if (!filename.startsWith("media_codecs_") ||
        !filename.endsWith(".xml")) {
        ALOGE("invalid include file name: %s", href);
        return -EINVAL;
    }
    filename.insert(mHrefBase, 0);

    parseXMLFile(filename.c_str());
    return mInitCheck;
}

void MediaCodecList::startElementHandler(
        const char *name, const char **attrs) {
    if (mInitCheck != OK) {
        return;
    }

    bool inType = true;

    if (!strcmp(name, "Include")) {
        mInitCheck = includeXMLFile(attrs);
        if (mInitCheck == OK) {
            mPastSections.push(mCurrentSection);
            mCurrentSection = SECTION_INCLUDE;
        }
        ++mDepth;
        return;
    }

    switch (mCurrentSection) {
        case SECTION_TOPLEVEL:
        {
            if (!strcmp(name, "Decoders")) {
                mCurrentSection = SECTION_DECODERS;
            } else if (!strcmp(name, "Encoders")) {
                mCurrentSection = SECTION_ENCODERS;
            }
            break;
        }

        case SECTION_DECODERS:
        {
            if (!strcmp(name, "MediaCodec")) {
                mInitCheck =
                    addMediaCodecFromAttributes(false /* encoder */, attrs);

                mCurrentSection = SECTION_DECODER;
            }
            break;
        }

        case SECTION_ENCODERS:
        {
            if (!strcmp(name, "MediaCodec")) {
                mInitCheck =
                    addMediaCodecFromAttributes(true /* encoder */, attrs);

                mCurrentSection = SECTION_ENCODER;
            }
            break;
        }

        case SECTION_DECODER:
        case SECTION_ENCODER:
        {
            if (!strcmp(name, "Quirk")) {
                mInitCheck = addQuirk(attrs);
            } else if (!strcmp(name, "Type")) {
                mInitCheck = addTypeFromAttributes(attrs);
                mCurrentSection =
                    (mCurrentSection == SECTION_DECODER
                            ? SECTION_DECODER_TYPE : SECTION_ENCODER_TYPE);
            }
        }
        inType = false;
        // fall through

        case SECTION_DECODER_TYPE:
        case SECTION_ENCODER_TYPE:
        {
            CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
            // ignore limits and features specified outside of type
            bool outside = !inType && info->mSoleType == 0;
            if (outside && (!strcmp(name, "Limit") || !strcmp(name, "Feature"))) {
                ALOGW("ignoring %s specified outside of a Type", name);
            } else if (!strcmp(name, "Limit")) {
                mInitCheck = addLimit(attrs);
            } else if (!strcmp(name, "Feature")) {
                mInitCheck = addFeature(attrs);
            }
            break;
        }

        default:
            break;
    }

    ++mDepth;
}

void MediaCodecList::endElementHandler(const char *name) {
    if (mInitCheck != OK) {
        return;
    }

    switch (mCurrentSection) {
        case SECTION_DECODERS:
        {
            if (!strcmp(name, "Decoders")) {
                mCurrentSection = SECTION_TOPLEVEL;
            }
            break;
        }

        case SECTION_ENCODERS:
        {
            if (!strcmp(name, "Encoders")) {
                mCurrentSection = SECTION_TOPLEVEL;
            }
            break;
        }

        case SECTION_DECODER_TYPE:
        case SECTION_ENCODER_TYPE:
        {
            if (!strcmp(name, "Type")) {
                mCurrentSection =
                    (mCurrentSection == SECTION_DECODER_TYPE
                            ? SECTION_DECODER : SECTION_ENCODER);

                CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
                info->mCurrentCaps = NULL;
            }
            break;
        }

        case SECTION_DECODER:
        {
            if (!strcmp(name, "MediaCodec")) {
                mCurrentSection = SECTION_DECODERS;

                CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
                info->mCurrentCaps = NULL;
            }
            break;
        }

        case SECTION_ENCODER:
        {
            if (!strcmp(name, "MediaCodec")) {
                mCurrentSection = SECTION_ENCODERS;

                CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
                info->mCurrentCaps = NULL;
            }
            break;
        }

        case SECTION_INCLUDE:
        {
            if (!strcmp(name, "Include") && mPastSections.size() > 0) {
                mCurrentSection = mPastSections.top();
                mPastSections.pop();
            }
            break;
        }

        default:
            break;
    }

    --mDepth;
}

status_t MediaCodecList::addMediaCodecFromAttributes(
        bool encoder, const char **attrs) {
    const char *name = NULL;
    const char *type = NULL;

    size_t i = 0;
    while (attrs[i] != NULL) {
        if (!strcmp(attrs[i], "name")) {
            if (attrs[i + 1] == NULL) {
                return -EINVAL;
            }
            name = attrs[i + 1];
            ++i;
        } else if (!strcmp(attrs[i], "type")) {
            if (attrs[i + 1] == NULL) {
                return -EINVAL;
            }
            type = attrs[i + 1];
            ++i;
        } else {
            return -EINVAL;
        }

        ++i;
    }

    if (name == NULL) {
        return -EINVAL;
    }

    addMediaCodec(encoder, name, type);

    return OK;
}

void MediaCodecList::addMediaCodec(
        bool encoder, const char *name, const char *type) {
    mCodecInfos.push();
    CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    info->mName = name;
    info->mIsEncoder = encoder;
    info->mSoleType = 0;
    info->mTypes = 0;
    info->mQuirks = 0;
    info->mCurrentCaps = NULL;

    if (type != NULL) {
        addType(type);
        // if type was specified in attributes, we do not allow
        // subsequent types
        info->mSoleType = info->mTypes;
    }
}

status_t MediaCodecList::addQuirk(const char **attrs) {
    const char *name = NULL;

    size_t i = 0;
    while (attrs[i] != NULL) {
        if (!strcmp(attrs[i], "name")) {
            if (attrs[i + 1] == NULL) {
                return -EINVAL;
            }
            name = attrs[i + 1];
            ++i;
        } else {
            return -EINVAL;
        }

        ++i;
    }

    if (name == NULL) {
        return -EINVAL;
    }

    uint32_t bit;
    ssize_t index = mCodecQuirks.indexOfKey(name);
    if (index < 0) {
        bit = mCodecQuirks.size();

        if (bit == 32) {
            ALOGW("Too many distinct quirk names in configuration.");
            return OK;
        }

        mCodecQuirks.add(name, bit);
    } else {
        bit = mCodecQuirks.valueAt(index);
    }

    CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    info->mQuirks |= 1ul << bit;

    return OK;
}

status_t MediaCodecList::addTypeFromAttributes(const char **attrs) {
    const char *name = NULL;

    CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    if (info->mSoleType != 0) {
        ALOGE("Codec '%s' already had its type specified", info->mName.c_str());
        return -EINVAL;
    }

    size_t i = 0;
    while (attrs[i] != NULL) {
        if (!strcmp(attrs[i], "name")) {
            if (attrs[i + 1] == NULL) {
                return -EINVAL;
            }
            name = attrs[i + 1];
            ++i;
        } else {
            return -EINVAL;
        }

        ++i;
    }

    if (name == NULL) {
        return -EINVAL;
    }

    addType(name);

    return OK;
}

void MediaCodecList::addType(const char *name) {
    uint32_t bit;
    ssize_t index = mTypes.indexOfKey(name);
    if (index < 0) {
        bit = mTypes.size();

        if (bit == 32) {
            ALOGW("Too many distinct type names in configuration.");
            return;
        }

        mTypes.add(name, bit);
    } else {
        bit = mTypes.valueAt(index);
    }

    CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    info->mTypes |= 1ul << bit;
    if (info->mCaps.indexOfKey(bit) < 0) {
        AMessage *msg = new AMessage();
        info->mCaps.add(bit, msg);
    }
    info->mCurrentCaps = info->mCaps.editValueFor(bit);
}

ssize_t MediaCodecList::findCodecByType(
        const char *type, bool encoder, size_t startIndex) const {
    ssize_t typeIndex = mTypes.indexOfKey(type);

    if (typeIndex < 0) {
        return -ENOENT;
    }

    uint32_t typeMask = 1ul << mTypes.valueAt(typeIndex);

    while (startIndex < mCodecInfos.size()) {
        const CodecInfo &info = mCodecInfos.itemAt(startIndex);

        if (info.mIsEncoder == encoder && (info.mTypes & typeMask)) {
            return startIndex;
        }

        ++startIndex;
    }

    return -ENOENT;
}

static status_t limitFoundMissingAttr(AString name, const char *attr, bool found = true) {
    ALOGE("limit '%s' with %s'%s' attribute", name.c_str(),
            (found ? "" : "no "), attr);
    return -EINVAL;
}

static status_t limitError(AString name, const char *msg) {
    ALOGE("limit '%s' %s", name.c_str(), msg);
    return -EINVAL;
}

static status_t limitInvalidAttr(AString name, const char *attr, AString value) {
    ALOGE("limit '%s' with invalid '%s' attribute (%s)", name.c_str(),
            attr, value.c_str());
    return -EINVAL;
}

status_t MediaCodecList::addLimit(const char **attrs) {
    sp<AMessage> msg = new AMessage();

    size_t i = 0;
    while (attrs[i] != NULL) {
        if (attrs[i + 1] == NULL) {
            return -EINVAL;
        }

        // attributes with values
        if (!strcmp(attrs[i], "name")
                || !strcmp(attrs[i], "default")
                || !strcmp(attrs[i], "in")
                || !strcmp(attrs[i], "max")
                || !strcmp(attrs[i], "min")
                || !strcmp(attrs[i], "range")
                || !strcmp(attrs[i], "ranges")
                || !strcmp(attrs[i], "scale")
                || !strcmp(attrs[i], "value")) {
            msg->setString(attrs[i], attrs[i + 1]);
            ++i;
        } else {
            return -EINVAL;
        }
        ++i;
    }

    AString name;
    if (!msg->findString("name", &name)) {
        ALOGE("limit with no 'name' attribute");
        return -EINVAL;
    }

    CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);

    // size, blocks, bitrate, frame-rate, blocks-per-second, aspect-ratio: range
    // quality: range + default + [scale]
    // complexity: range + default
    bool found;
    if (name == "aspect-ratio" || name == "bitrate" || name == "block-count"
            || name == "blocks-per-second" || name == "complexity"
            || name == "frame-rate" || name == "quality" || name == "size") {
        AString min, max;
        if (msg->findString("min", &min) && msg->findString("max", &max)) {
            min.append("-");
            min.append(max);
            if (msg->contains("range") || msg->contains("value")) {
                return limitError(name, "has 'min' and 'max' as well as 'range' or "
                        "'value' attributes");
            }
            msg->setString("range", min);
        } else if (msg->contains("min") || msg->contains("max")) {
            return limitError(name, "has only 'min' or 'max' attribute");
        } else if (msg->findString("value", &max)) {
            min = max;
            min.append("-");
            min.append(max);
            if (msg->contains("range")) {
                return limitError(name, "has both 'range' and 'value' attributes");
            }
            msg->setString("range", min);
        }

        AString range, scale = "linear", def, in_;
        if (!msg->findString("range", &range)) {
            return limitError(name, "with no 'range', 'value' or 'min'/'max' attributes");
        }

        if ((name == "quality" || name == "complexity") ^
                (found = msg->findString("default", &def))) {
            return limitFoundMissingAttr(name, "default", found);
        }
        if (name != "quality" && msg->findString("scale", &scale)) {
            return limitFoundMissingAttr(name, "scale");
        }
        if ((name == "aspect-ratio") ^ (found = msg->findString("in", &in_))) {
            return limitFoundMissingAttr(name, "in", found);
        }

        if (name == "aspect-ratio") {
            if (!(in_ == "pixels") && !(in_ == "blocks")) {
                return limitInvalidAttr(name, "in", in_);
            }
            in_.erase(5, 1); // (pixel|block)-aspect-ratio
            in_.append("-");
            in_.append(name);
            name = in_;
        }
        if (name == "quality") {
            info->mCurrentCaps->setString("quality-scale", scale);
        }
        if (name == "quality" || name == "complexity") {
            AString tag = name;
            tag.append("-default");
            info->mCurrentCaps->setString(tag.c_str(), def);
        }
        AString tag = name;
        tag.append("-range");
        info->mCurrentCaps->setString(tag.c_str(), range);
    } else {
        AString max, value, ranges;
        if (msg->contains("default")) {
            return limitFoundMissingAttr(name, "default");
        } else if (msg->contains("in")) {
            return limitFoundMissingAttr(name, "in");
        } else if ((name == "channel-count") ^
                (found = msg->findString("max", &max))) {
            return limitFoundMissingAttr(name, "max", found);
        } else if (msg->contains("min")) {
            return limitFoundMissingAttr(name, "min");
        } else if (msg->contains("range")) {
            return limitFoundMissingAttr(name, "range");
        } else if ((name == "sample-rate") ^
                (found = msg->findString("ranges", &ranges))) {
            return limitFoundMissingAttr(name, "ranges", found);
        } else if (msg->contains("scale")) {
            return limitFoundMissingAttr(name, "scale");
        } else if ((name == "alignment" || name == "block-size") ^
                (found = msg->findString("value", &value))) {
            return limitFoundMissingAttr(name, "value", found);
        }

        if (max.size()) {
            AString tag = "max-";
            tag.append(name);
            info->mCurrentCaps->setString(tag.c_str(), max);
        } else if (value.size()) {
            info->mCurrentCaps->setString(name.c_str(), value);
        } else if (ranges.size()) {
            AString tag = name;
            tag.append("-ranges");
            info->mCurrentCaps->setString(tag.c_str(), ranges);
        } else {
            ALOGW("Ignoring unrecognized limit '%s'", name.c_str());
        }
    }
    return OK;
}

static bool parseBoolean(const char *s) {
    if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcasecmp(s, "y")) {
        return true;
    }
    char *end;
    unsigned long res = strtoul(s, &end, 10);
    return *s != '\0' && *end == '\0' && res > 0;
}

status_t MediaCodecList::addFeature(const char **attrs) {
    size_t i = 0;
    const char *name = NULL;
    int32_t optional = -1;
    int32_t required = -1;

    while (attrs[i] != NULL) {
        if (attrs[i + 1] == NULL) {
            return -EINVAL;
        }

        // attributes with values
        if (!strcmp(attrs[i], "name")) {
            name = attrs[i + 1];
            ++i;
        } else if (!strcmp(attrs[i], "optional") || !strcmp(attrs[i], "required")) {
            int value = (int)parseBoolean(attrs[i + 1]);
            if (!strcmp(attrs[i], "optional")) {
                optional = value;
            } else {
                required = value;
            }
            ++i;
        } else {
            return -EINVAL;
        }
        ++i;
    }
    if (name == NULL) {
        ALOGE("feature with no 'name' attribute");
        return -EINVAL;
    }

    if (optional == required && optional != -1) {
        ALOGE("feature '%s' is both/neither optional and required", name);
        return -EINVAL;
    }

    CodecInfo *info = &mCodecInfos.editItemAt(mCodecInfos.size() - 1);
    AString tag = "feature-";
    tag.append(name);
    info->mCurrentCaps->setInt32(tag.c_str(), (required == 1) || (optional == 0));
    return OK;
}

ssize_t MediaCodecList::findCodecByName(const char *name) const {
    for (size_t i = 0; i < mCodecInfos.size(); ++i) {
        const CodecInfo &info = mCodecInfos.itemAt(i);

        if (info.mName == name) {
            return i;
        }
    }

    return -ENOENT;
}

size_t MediaCodecList::countCodecs() const {
    return mCodecInfos.size();
}

const char *MediaCodecList::getCodecName(size_t index) const {
    if (index >= mCodecInfos.size()) {
        return NULL;
    }

    const CodecInfo &info = mCodecInfos.itemAt(index);
    return info.mName.c_str();
}

bool MediaCodecList::isEncoder(size_t index) const {
    if (index >= mCodecInfos.size()) {
        return NULL;
    }

    const CodecInfo &info = mCodecInfos.itemAt(index);
    return info.mIsEncoder;
}

bool MediaCodecList::codecHasQuirk(
        size_t index, const char *quirkName) const {
    if (index >= mCodecInfos.size()) {
        return NULL;
    }

    const CodecInfo &info = mCodecInfos.itemAt(index);

    if (info.mQuirks != 0) {
        ssize_t index = mCodecQuirks.indexOfKey(quirkName);
        if (index >= 0 && info.mQuirks & (1ul << mCodecQuirks.valueAt(index))) {
            return true;
        }
    }

    return false;
}

status_t MediaCodecList::getSupportedTypes(
        size_t index, Vector<AString> *types) const {
    types->clear();

    if (index >= mCodecInfos.size()) {
        return -ERANGE;
    }

    const CodecInfo &info = mCodecInfos.itemAt(index);

    for (size_t i = 0; i < mTypes.size(); ++i) {
        uint32_t typeMask = 1ul << mTypes.valueAt(i);

        if (info.mTypes & typeMask) {
            types->push(mTypes.keyAt(i));
        }
    }

    return OK;
}

status_t MediaCodecList::getCodecCapabilities(
        size_t index, const char *type,
        Vector<ProfileLevel> *profileLevels,
        Vector<uint32_t> *colorFormats,
        uint32_t *flags,
        sp<AMessage> *capabilities) const {
    profileLevels->clear();
    colorFormats->clear();

    if (index >= mCodecInfos.size()) {
        return -ERANGE;
    }

    const CodecInfo &info = mCodecInfos.itemAt(index);

    ssize_t typeIndex = mTypes.indexOfKey(type);
    if (typeIndex < 0) {
        return -EINVAL;
    }

    OMXClient client;
    status_t err = client.connect();
    if (err != OK) {
        return err;
    }

    CodecCapabilities caps;
    err = QueryCodec(
            client.interface(),
            info.mName.c_str(), type, info.mIsEncoder, &caps);

    if (err != OK) {
        return err;
    }

    for (size_t i = 0; i < caps.mProfileLevels.size(); ++i) {
        const CodecProfileLevel &src = caps.mProfileLevels.itemAt(i);

        ProfileLevel profileLevel;
        profileLevel.mProfile = src.mProfile;
        profileLevel.mLevel = src.mLevel;
        profileLevels->push(profileLevel);
    }

    for (size_t i = 0; i < caps.mColorFormats.size(); ++i) {
        colorFormats->push(caps.mColorFormats.itemAt(i));
    }

    *flags = caps.mFlags;

    // TODO this check will be removed once JNI side is merged
    if (capabilities != NULL) {
        *capabilities = info.mCaps.valueFor(typeIndex);
    }

    return OK;
}

}  // namespace android
