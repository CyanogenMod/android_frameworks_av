/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "Camera2-Metadata"
#include <utils/Log.h>
#include <utils/Errors.h>

#include "CameraMetadata.h"

namespace android {

namespace camera2 {
CameraMetadata::CameraMetadata() :
        mBuffer(NULL) {
}

CameraMetadata::CameraMetadata(size_t entryCapacity, size_t dataCapacity)
{
    mBuffer = allocate_camera_metadata(entryCapacity, dataCapacity);
}

CameraMetadata::CameraMetadata(const CameraMetadata &other) {
    mBuffer = clone_camera_metadata(other.mBuffer);
}

CameraMetadata &CameraMetadata::operator=(const CameraMetadata &other) {
    return operator=(other.mBuffer);
}

CameraMetadata &CameraMetadata::operator=(const camera_metadata_t *buffer) {
    if (CC_LIKELY(buffer != mBuffer)) {
        camera_metadata_t *newBuffer = clone_camera_metadata(buffer);
        clear();
        mBuffer = newBuffer;
    }
    return *this;
}

CameraMetadata::~CameraMetadata() {
    clear();
}

camera_metadata_t* CameraMetadata::release() {
    camera_metadata_t *released = mBuffer;
    mBuffer = NULL;
    return released;
}

void CameraMetadata::clear() {
    if (mBuffer) {
        free_camera_metadata(mBuffer);
        mBuffer = NULL;
    }
}

void CameraMetadata::acquire(camera_metadata_t *buffer) {
    clear();
    mBuffer = buffer;
}

void CameraMetadata::acquire(CameraMetadata &other) {
    acquire(other.release());
}

status_t CameraMetadata::append(const CameraMetadata &other) {
    return append_camera_metadata(mBuffer, other.mBuffer);
}

size_t CameraMetadata::entryCount() const {
    return (mBuffer == NULL) ? 0 :
            get_camera_metadata_entry_count(mBuffer);
}

bool CameraMetadata::isEmpty() const {
    return entryCount() == 0;
}

status_t CameraMetadata::sort() {
    return sort_camera_metadata(mBuffer);
}

status_t CameraMetadata::checkType(uint32_t tag, uint8_t expectedType) {
    int tagType = get_camera_metadata_tag_type(tag);
    if ( CC_UNLIKELY(tagType == -1)) {
        ALOGE("Update metadata entry: Unknown tag %d", tag);
        return INVALID_OPERATION;
    }
    if ( CC_UNLIKELY(tagType != expectedType) ) {
        ALOGE("Mismatched tag type when updating entry %s (%d) of type %s; "
                "got type %s data instead ",
                get_camera_metadata_tag_name(tag), tag,
                camera_metadata_type_names[tagType],
                camera_metadata_type_names[expectedType]);
        return INVALID_OPERATION;
    }
    return OK;
}

status_t CameraMetadata::update(uint32_t tag,
        const int32_t *data, size_t data_count) {
    status_t res;
    if ( (res = checkType(tag, TYPE_INT32)) != OK) {
        return res;
    }
    return update(tag, (const void*)data, data_count);
}

status_t CameraMetadata::update(uint32_t tag,
        const uint8_t *data, size_t data_count) {
    status_t res;
    if ( (res = checkType(tag, TYPE_BYTE)) != OK) {
        return res;
    }
    return update(tag, (const void*)data, data_count);
}

status_t CameraMetadata::update(uint32_t tag,
        const float *data, size_t data_count) {
    status_t res;
    if ( (res = checkType(tag, TYPE_FLOAT)) != OK) {
        return res;
    }
    return update(tag, (const void*)data, data_count);
}

status_t CameraMetadata::update(uint32_t tag,
        const int64_t *data, size_t data_count) {
    status_t res;
    if ( (res = checkType(tag, TYPE_INT64)) != OK) {
        return res;
    }
    return update(tag, (const void*)data, data_count);
}

status_t CameraMetadata::update(uint32_t tag,
        const double *data, size_t data_count) {
    status_t res;
    if ( (res = checkType(tag, TYPE_DOUBLE)) != OK) {
        return res;
    }
    return update(tag, (const void*)data, data_count);
}

status_t CameraMetadata::update(uint32_t tag,
        const camera_metadata_rational_t *data, size_t data_count) {
    status_t res;
    if ( (res = checkType(tag, TYPE_RATIONAL)) != OK) {
        return res;
    }
    return update(tag, (const void*)data, data_count);
}

status_t CameraMetadata::update(uint32_t tag,
        const String8 &string) {
    status_t res;
    if ( (res = checkType(tag, TYPE_BYTE)) != OK) {
        return res;
    }
    return update(tag, (const void*)string.string(), string.size());
}

status_t CameraMetadata::update(uint32_t tag, const void *data,
        size_t data_count) {
    status_t res;
    int type = get_camera_metadata_tag_type(tag);
    if (type == -1) {
        ALOGE("%s: Tag %d not found", __FUNCTION__, tag);
        return BAD_VALUE;
    }
    size_t data_size = calculate_camera_metadata_entry_data_size(type,
            data_count);

    res = resizeIfNeeded(1, data_size);

    if (res == OK) {
        camera_metadata_entry_t entry;
        res = find_camera_metadata_entry(mBuffer, tag, &entry);
        if (res == NAME_NOT_FOUND) {
            res = add_camera_metadata_entry(mBuffer,
                    tag, data, data_count);
        } else if (res == OK) {
            res = update_camera_metadata_entry(mBuffer,
                    entry.index, data, data_count, NULL);
        }
    }

    if (res != OK) {
        ALOGE("%s: Unable to update metadata entry %s.%s (%x): %s (%d)",
                __FUNCTION__, get_camera_metadata_section_name(tag),
                get_camera_metadata_tag_name(tag), tag, strerror(-res), res);
    }
    return res;
}

camera_metadata_entry_t CameraMetadata::find(uint32_t tag) {
    status_t res;
    camera_metadata_entry entry;
    res = find_camera_metadata_entry(mBuffer, tag, &entry);
    if (CC_UNLIKELY( res != OK )) {
        entry.count = 0;
        entry.data.u8 = NULL;
    }
    return entry;
}

camera_metadata_ro_entry_t CameraMetadata::find(uint32_t tag) const {
    status_t res;
    camera_metadata_ro_entry entry;
    res = find_camera_metadata_ro_entry(mBuffer, tag, &entry);
    if (CC_UNLIKELY( res != OK )) {
        entry.count = 0;
        entry.data.u8 = NULL;
    }
    return entry;
}

status_t CameraMetadata::erase(uint32_t tag) {
    camera_metadata_entry_t entry;
    status_t res;
    res = find_camera_metadata_entry(mBuffer, tag, &entry);
    if (res == NAME_NOT_FOUND) {
        return OK;
    } else if (res != OK) {
        ALOGE("%s: Error looking for entry %s.%s (%x): %s %d",
                __FUNCTION__,
                get_camera_metadata_section_name(tag),
                get_camera_metadata_tag_name(tag), tag, strerror(-res), res);
        return res;
    }
    res = delete_camera_metadata_entry(mBuffer, entry.index);
    if (res != OK) {
        ALOGE("%s: Error deleting entry %s.%s (%x): %s %d",
                __FUNCTION__,
                get_camera_metadata_section_name(tag),
                get_camera_metadata_tag_name(tag), tag, strerror(-res), res);
    }
    return res;
}

void CameraMetadata::dump(int fd, int verbosity, int indentation) const {
    dump_indented_camera_metadata(mBuffer, fd, verbosity, indentation);
}

status_t CameraMetadata::resizeIfNeeded(size_t extraEntries, size_t extraData) {
    if (mBuffer == NULL) {
        mBuffer = allocate_camera_metadata(extraEntries * 2, extraData * 2);
        if (mBuffer == NULL) {
            ALOGE("%s: Can't allocate larger metadata buffer", __FUNCTION__);
            return NO_MEMORY;
        }
    } else {
        size_t currentEntryCount = get_camera_metadata_entry_count(mBuffer);
        size_t currentEntryCap = get_camera_metadata_entry_capacity(mBuffer);
        size_t newEntryCount = currentEntryCount +
                extraEntries;
        newEntryCount = (newEntryCount > currentEntryCap) ?
                newEntryCount * 2 : currentEntryCap;

        size_t currentDataCount = get_camera_metadata_data_count(mBuffer);
        size_t currentDataCap = get_camera_metadata_data_capacity(mBuffer);
        size_t newDataCount = currentDataCount +
                extraData;
        newDataCount = (newDataCount > currentDataCap) ?
                newDataCount * 2 : currentDataCap;

        if (newEntryCount > currentEntryCap ||
                newDataCount > currentDataCap) {
            camera_metadata_t *oldBuffer = mBuffer;
            mBuffer = allocate_camera_metadata(newEntryCount,
                    newDataCount);
            if (mBuffer == NULL) {
                ALOGE("%s: Can't allocate larger metadata buffer", __FUNCTION__);
                return NO_MEMORY;
            }
            append_camera_metadata(mBuffer, oldBuffer);
            free_camera_metadata(oldBuffer);
        }
    }
    return OK;
}

}; // namespace camera2
}; // namespace android
