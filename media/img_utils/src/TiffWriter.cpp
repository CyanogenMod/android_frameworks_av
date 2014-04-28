/*
 * Copyright 2014 The Android Open Source Project
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

#include <img_utils/TiffIfd.h>
#include <img_utils/TiffHelpers.h>
#include <img_utils/TiffWriter.h>
#include <img_utils/TagDefinitions.h>

#include <assert.h>

namespace android {
namespace img_utils {

KeyedVector<uint16_t, const TagDefinition_t*> TiffWriter::buildTagMap(
            const TagDefinition_t* definitions, size_t length) {
    KeyedVector<uint16_t, const TagDefinition_t*> map;
    for(size_t i = 0; i < length; ++i) {
        map.add(definitions[i].tagId, definitions + i);
    }
    return map;
}

#define COMPARE(op) \
bool Orderable::operator op (const Orderable& orderable) const { \
    return getComparableValue() op orderable.getComparableValue(); \
}

#define ARRAY_SIZE(array) \
    (sizeof(array) / sizeof(array[0]))

KeyedVector<uint16_t, const TagDefinition_t*> TiffWriter::sTagMaps[] = {
    buildTagMap(TIFF_EP_TAG_DEFINITIONS, ARRAY_SIZE(TIFF_EP_TAG_DEFINITIONS)),
    buildTagMap(DNG_TAG_DEFINITIONS, ARRAY_SIZE(DNG_TAG_DEFINITIONS)),
    buildTagMap(EXIF_2_3_TAG_DEFINITIONS, ARRAY_SIZE(EXIF_2_3_TAG_DEFINITIONS)),
    buildTagMap(TIFF_6_TAG_DEFINITIONS, ARRAY_SIZE(TIFF_6_TAG_DEFINITIONS))
};

TiffWriter::TiffWriter() : mTagMaps(sTagMaps), mNumTagMaps(DEFAULT_NUM_TAG_MAPS) {}

TiffWriter::TiffWriter(KeyedVector<uint16_t, const TagDefinition_t*>* enabledDefinitions,
        size_t length) : mTagMaps(enabledDefinitions), mNumTagMaps(length) {}

TiffWriter::~TiffWriter() {}

status_t TiffWriter::write(Output* out, Endianness end) {
    status_t ret = OK;
    EndianOutput endOut(out, end);

    if (mIfd == NULL) {
        ALOGE("%s: Tiff header is empty.", __FUNCTION__);
        return BAD_VALUE;
    }
    BAIL_ON_FAIL(writeFileHeader(endOut), ret);

    uint32_t offset = FILE_HEADER_SIZE;
    sp<TiffIfd> ifd = mIfd;
    while(ifd != NULL) {
        BAIL_ON_FAIL(ifd->writeData(offset, &endOut), ret);
        offset += ifd->getSize();
        ifd = ifd->getNextIfd();
    }
    return ret;
}


const TagDefinition_t* TiffWriter::lookupDefinition(uint16_t tag) const {
    const TagDefinition_t* definition = NULL;
    for (size_t i = 0; i < mNumTagMaps; ++i) {
        ssize_t index = mTagMaps[i].indexOfKey(tag);
        if (index >= 0) {
            definition = mTagMaps[i][index];
            break;
        }
    }

    if (definition == NULL) {
        ALOGE("%s: No definition exists for tag with id %x.", __FUNCTION__, tag);
    }
    return definition;
}

sp<TiffEntry> TiffWriter::getEntry(uint16_t tag, uint32_t ifd) const {
    ssize_t index = mNamedIfds.indexOfKey(ifd);
    if (index < 0) {
        ALOGE("%s: No IFD %d set for this writer.", __FUNCTION__, ifd);
        return NULL;
    }
    return mNamedIfds[index]->getEntry(tag);
}


// TODO: Fix this to handle IFD position in chain/sub-IFD tree
status_t TiffWriter::addEntry(const sp<TiffEntry>& entry) {
    uint16_t tag = entry->getTag();

    const TagDefinition_t* definition = lookupDefinition(tag);

    if (definition == NULL) {
        return BAD_INDEX;
    }
    uint32_t ifdId = 0;  // TODO: all in IFD0 for now.

    ssize_t index = mNamedIfds.indexOfKey(ifdId);

    // Add a new IFD if necessary
    if (index < 0) {
        sp<TiffIfd> ifdEntry = new TiffIfd(ifdId);
        if (mIfd == NULL) {
            mIfd = ifdEntry;
        }
        index = mNamedIfds.add(ifdId, ifdEntry);
        assert(index >= 0);
    }

    sp<TiffIfd> selectedIfd  = mNamedIfds[index];
    return selectedIfd->addEntry(entry);
}

status_t TiffWriter::uncheckedAddIfd(const sp<TiffIfd>& ifd) {
    mNamedIfds.add(ifd->getId(), ifd);
    sp<TiffIfd> last = findLastIfd();
    if (last == NULL) {
        mIfd = ifd;
    } else {
        last->setNextIfd(ifd);
    }
    last = ifd->getNextIfd();
    while (last != NULL) {
        mNamedIfds.add(last->getId(), last);
        last = last->getNextIfd();
    }
    return OK;
}

status_t TiffWriter::addIfd(uint32_t ifd) {
    ssize_t index = mNamedIfds.indexOfKey(ifd);
    if (index >= 0) {
        ALOGE("%s: Ifd with ID 0x%x already exists.", __FUNCTION__, ifd);
        return BAD_VALUE;
    }
    sp<TiffIfd> newIfd = new TiffIfd(ifd);
    if (mIfd == NULL) {
        mIfd = newIfd;
    } else {
        sp<TiffIfd> last = findLastIfd();
        last->setNextIfd(newIfd);
    }
    mNamedIfds.add(ifd, newIfd);
    return OK;
}

TagType TiffWriter::getDefaultType(uint16_t tag) const {
    const TagDefinition_t* definition = lookupDefinition(tag);
    if (definition == NULL) {
        ALOGE("%s: Could not find definition for tag %x", __FUNCTION__, tag);
        return UNKNOWN_TAGTYPE;
    }
    return definition->defaultType;
}

uint32_t TiffWriter::getDefaultCount(uint16_t tag) const {
    const TagDefinition_t* definition = lookupDefinition(tag);
    if (definition == NULL) {
        ALOGE("%s: Could not find definition for tag %x", __FUNCTION__, tag);
        return 0;
    }
    return definition->fixedCount;
}

bool TiffWriter::checkIfDefined(uint16_t tag) const {
    return lookupDefinition(tag) != NULL;
}

sp<TiffIfd> TiffWriter::findLastIfd() {
    sp<TiffIfd> ifd = mIfd;
    while(ifd != NULL) {
        sp<TiffIfd> nextIfd = ifd->getNextIfd();
        if (nextIfd == NULL) {
            break;
        }
        ifd = nextIfd;
    }
    return ifd;
}

status_t TiffWriter::writeFileHeader(EndianOutput& out) {
    status_t ret = OK;
    uint16_t endMarker = (out.getEndianness() == BIG) ? BIG_ENDIAN_MARKER : LITTLE_ENDIAN_MARKER;
    BAIL_ON_FAIL(out.write(&endMarker, 0, 1), ret);

    uint16_t tiffMarker = TIFF_FILE_MARKER;
    BAIL_ON_FAIL(out.write(&tiffMarker, 0, 1), ret);

    uint32_t offsetMarker = FILE_HEADER_SIZE;
    BAIL_ON_FAIL(out.write(&offsetMarker, 0, 1), ret);
    return ret;
}

uint32_t TiffWriter::getTotalSize() const {
    uint32_t totalSize = FILE_HEADER_SIZE;
    sp<TiffIfd> ifd = mIfd;
    while(ifd != NULL) {
        totalSize += ifd->getSize();
        ifd = ifd->getNextIfd();
    }
    return totalSize;
}

void TiffWriter::log() const {
    ALOGI("%s: TiffWriter:", __FUNCTION__);
    sp<TiffIfd> ifd = mIfd;
    while(ifd != NULL) {
        ifd->log();
        ifd = ifd->getNextIfd();
    }
}

} /*namespace img_utils*/
} /*namespace android*/
