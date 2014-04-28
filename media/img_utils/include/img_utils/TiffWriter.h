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

#ifndef IMG_UTILS_TIFF_WRITER_H
#define IMG_UTILS_TIFF_WRITER_H

#include <img_utils/EndianUtils.h>
#include <img_utils/TiffEntryImpl.h>
#include <img_utils/TagDefinitions.h>

#include <utils/Log.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>
#include <utils/KeyedVector.h>
#include <utils/Vector.h>

#include <cutils/compiler.h>
#include <stdint.h>

namespace android {
namespace img_utils {

class TiffEntry;
class TiffIfd;
class Output;

/**
 * This class holds a collection of TIFF IFDs that can be written as a
 * complete DNG file header.
 *
 * This maps to the TIFF header structure that is logically composed of:
 * - An 8-byte file header containing an endianness indicator, the TIFF
 *   file marker, and the offset to the first IFD.
 * - A list of TIFF IFD structures.
 */
class ANDROID_API TiffWriter : public LightRefBase<TiffWriter> {
    public:

        /**
         * Constructs a TiffWriter with the default tag mappings. This enables
         * all of the tags defined in TagDefinitions.h, and uses the following
         * mapping precedence to resolve collisions:
         * (highest precedence) TIFF/EP > DNG > EXIF 2.3 > TIFF 6.0
         */
        TiffWriter();

        /**
         * Constructs a TiffWriter with the given tag mappings.  The mapping
         * precedence will be in the order that the definition maps are given,
         * where the lower index map gets precedence.
         *
         * This can be used with user-defined definitions, or definitions form
         * TagDefinitions.h
         *
         * The enabledDefinitions mapping object is owned by the caller, and must
         * stay alive for the lifespan of the constructed TiffWriter object.
         */
        TiffWriter(KeyedVector<uint16_t, const TagDefinition_t*>* enabledDefinitions,
                size_t length);

        virtual ~TiffWriter();

        /**
         * Write a TIFF header containing each IFD set.  This will recursively
         * write all SubIFDs and tags.
         *
         * Returns OK on success, or a negative error code on failure.
         */
        virtual status_t write(Output* out, Endianness end = LITTLE);

        /**
         * Get the total size in bytes of the TIFF header.  This includes all
         * IFDs, tags, and values set for this TiffWriter.
         */
        virtual uint32_t getTotalSize() const;

        /**
         * Add the given entry to its default IFD.  If that IFD does not
         * exist, it will be created.
         */
        virtual status_t addEntry(const sp<TiffEntry>& entry);

        /**
         * Build an entry for a known tag.  This tag must be one of the tags
         * defined in one of the definition vectors this TIFF writer was constructed
         * with. The count and type are validated. If this succeeds, the resulting
         * entry will be placed in the outEntry pointer.
         *
         * Returns OK on success, or a negative error code on failure. Valid
         * error codes for this method are:
         * - BAD_INDEX - The given tag doesn't exist.
         * - BAD_VALUE - The given count doesn't match the required count for
         *               this tag.
         * - BAD_TYPE  - The type of the given data isn't compatible with the
         *               type required for this tag.
         */
        template<typename T>
        status_t buildEntry(uint16_t tag, uint32_t count, const T* data,
                  /*out*/sp<TiffEntry>* outEntry) const;

         /**
         * Build an entry for a known tag and add it to the IFD with the given ID.
         * This tag must be defined in one of the definition vectors this TIFF writer
         * was constructed with. The count and type are validated. If this succeeds,
         * the resulting entry will be placed in the outEntry pointer.
         *
         * Returns OK on success, or a negative error code on failure. Valid
         * error codes for this method are:
         * - BAD_INDEX - The given tag doesn't exist.
         * - BAD_VALUE - The given count doesn't match the required count for
         *               this tag.
         * - BAD_TYPE  - The type of the given data isn't compatible with the
         *               type required for this tag.
         * - NAME_NOT_FOUND - No ifd exists with the given ID.
         */
        template<typename T>
        status_t addEntry(uint16_t tag, uint32_t count, const T* data, uint32_t ifd);

        /**
         * Return the TIFF entry with the given tag ID in the IFD with the given ID,
         * or an empty pointer if none exists.
         */
        virtual sp<TiffEntry> getEntry(uint16_t tag, uint32_t ifd) const;

        /**
         * Add the given IFD to the end of the top-level IFD chain. No
         * validation is done.
         *
         * Returns OK on success, or a negative error code on failure.
         */
        virtual status_t uncheckedAddIfd(const sp<TiffIfd>& ifd);

        /**
         * Create an empty IFD with the given ID and add it to the end of the
         * list of IFDs.
         */
        virtual status_t addIfd(uint32_t ifd);

        /**
         * Build an entry.  No validation is done.
         *
         * WARNING: Using this method can result in creating poorly formatted
         * TIFF files.
         *
         * Returns a TiffEntry with the given tag, type, count, endianness,
         * and data.
         */
        template<typename T>
        static sp<TiffEntry> uncheckedBuildEntry(uint16_t tag, TagType type,
                  uint32_t count, Endianness end, const T* data);

        /**
         * Utility function to build atag-to-definition mapping from a given
         * array of tag definitions.
         */
        static KeyedVector<uint16_t, const TagDefinition_t*> buildTagMap(
                  const TagDefinition_t* definitions, size_t length);

        /**
         * Returns the default type for the given tag ID.
         */
        virtual TagType getDefaultType(uint16_t tag) const;

        /**
         * Returns the default count for a given tag ID, or 0 if this
         * tag normally has a variable count.
         */
        virtual uint32_t getDefaultCount(uint16_t tag) const;

        /**
         * Returns true if a definition exist for the given tag ID.
         */
        virtual bool checkIfDefined(uint16_t tag) const;

        /**
         * Print the currently configured IFDs and entries to logcat.
         */
        virtual void log() const;

    protected:
        enum {
            DEFAULT_NUM_TAG_MAPS = 4,
        };

        sp<TiffIfd> findLastIfd();
        status_t writeFileHeader(EndianOutput& out);
        const TagDefinition_t* lookupDefinition(uint16_t tag) const;
        status_t calculateOffsets();

        sp<TiffIfd> mIfd;
        KeyedVector<uint32_t, sp<TiffIfd> > mNamedIfds;
        KeyedVector<uint16_t, const TagDefinition_t*>* mTagMaps;
        size_t mNumTagMaps;

        static KeyedVector<uint16_t, const TagDefinition_t*> sTagMaps[];
};

template<typename T>
status_t TiffWriter::buildEntry(uint16_t tag, uint32_t count, const T* data,
                  /*out*/sp<TiffEntry>* outEntry) const {
    const TagDefinition_t* definition = lookupDefinition(tag);

    if (definition == NULL) {
        ALOGE("%s: No such tag exists for id %x.", __FUNCTION__, tag);
        return BAD_INDEX;
    }

    uint32_t fixedCount = definition->fixedCount;
    if (fixedCount > 0 && fixedCount != count) {
        ALOGE("%s: Invalid count %d for tag %x (expects %d).", __FUNCTION__, count, tag,
                fixedCount);
        return BAD_VALUE;
    }

    TagType fixedType = definition->defaultType;
    if (TiffEntry::forceValidType(fixedType, data) == NULL) {
        ALOGE("%s: Invalid type used for tag value for tag %x.", __FUNCTION__, tag);
        return BAD_TYPE;
    }

    *outEntry = new TiffEntryImpl<T>(tag, fixedType, count,
        definition->fixedEndian, data);

    return OK;
}

template<typename T>
status_t TiffWriter::addEntry(uint16_t tag, uint32_t count, const T* data, uint32_t ifd) {
    sp<TiffEntry> outEntry;
    status_t ret = buildEntry<T>(tag, count, data, &outEntry);
    if (ret != OK) {
        ALOGE("%s: Could not build entry for tag %x.", __FUNCTION__, tag);
        return ret;
    }
    ssize_t index = mNamedIfds.indexOfKey(ifd);
    if (index < 0) {
        ALOGE("%s: No IFD %d set for this writer.", __FUNCTION__, ifd);
        return NAME_NOT_FOUND;
    }
    return mNamedIfds[index]->addEntry(outEntry);
}

template<typename T>
sp<TiffEntry> TiffWriter::uncheckedBuildEntry(uint16_t tag, TagType type, uint32_t count,
        Endianness end, const T* data) {
    TiffEntryImpl<T>* entry = new TiffEntryImpl<T>(tag, type, count, end, data);
    return sp<TiffEntry>(entry);
}

} /*namespace img_utils*/
} /*namespace android*/


#endif /*IMG_UTILS_TIFF_WRITER_H*/
