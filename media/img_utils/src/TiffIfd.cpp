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

#include <utils/Log.h>

namespace android {
namespace img_utils {

TiffIfd::TiffIfd(uint32_t ifdId)
        : mNextIfd(), mIfdId(ifdId) {}

TiffIfd::~TiffIfd() {}

status_t TiffIfd::addEntry(const sp<TiffEntry>& entry) {
    size_t size = mEntries.size();
    if (size >= MAX_IFD_ENTRIES) {
        ALOGW("%s: Failed to add entry for tag 0x%x to IFD %u, too many entries in IFD!",
                __FUNCTION__, entry->getTag(), mIfdId);
        return BAD_INDEX;
    }

    if (mEntries.add(entry) < 0) {
        ALOGW("%s: Failed to add entry for tag 0x%x to ifd %u.", __FUNCTION__, entry->getTag(),
                mIfdId);
        return BAD_INDEX;
    }
    return OK;
}

sp<TiffEntry> TiffIfd::getEntry(uint16_t tag) const {
    ssize_t index = mEntries.indexOfTag(tag);
    if (index < 0) {
        ALOGW("%s: No entry for tag 0x%x in ifd %u.", __FUNCTION__, tag, mIfdId);
        return NULL;
    }
    return mEntries[index];
}

void TiffIfd::setNextIfd(const sp<TiffIfd>& ifd) {
    mNextIfd = ifd;
}

sp<TiffIfd> TiffIfd::getNextIfd() const {
    return mNextIfd;
}

uint32_t TiffIfd::checkAndGetOffset(uint32_t offset) const {
    size_t size = mEntries.size();

    if (size > MAX_IFD_ENTRIES) {
        ALOGW("%s: Could not calculate IFD offsets, IFD %u contains too many entries.",
                __FUNCTION__, mIfdId);
        return BAD_OFFSET;
    }

    if (size <= 0) {
        ALOGW("%s: Could not calculate IFD offsets, IFD %u contains no entries.", __FUNCTION__,
                mIfdId);
        return BAD_OFFSET;
    }

    if (offset == BAD_OFFSET) {
        ALOGW("%s: Could not calculate IFD offsets, IFD %u had a bad initial offset.",
                __FUNCTION__, mIfdId);
        return BAD_OFFSET;
    }

    uint32_t ifdSize = calculateIfdSize(size);
    WORD_ALIGN(ifdSize);
    return offset + ifdSize;
}

status_t TiffIfd::writeData(uint32_t offset, /*out*/EndianOutput* out) const {
    assert((offset % TIFF_WORD_SIZE) == 0);
    status_t ret = OK;

    ALOGV("%s: IFD %u written to offset %u", __FUNCTION__, mIfdId, offset );
    uint32_t valueOffset = checkAndGetOffset(offset);
    if (valueOffset == 0) {
        return BAD_VALUE;
    }

    size_t size = mEntries.size();

    // Writer IFD header (2 bytes, number of entries).
    uint16_t header = static_cast<uint16_t>(size);
    BAIL_ON_FAIL(out->write(&header, 0, 1), ret);

    // Write tag entries
    for (size_t i = 0; i < size; ++i) {
        BAIL_ON_FAIL(mEntries[i]->writeTagInfo(valueOffset, out), ret);
        valueOffset += mEntries[i]->getSize();
    }

    // Writer IFD footer (4 bytes, offset to next IFD).
    uint32_t footer = (mNextIfd != NULL) ? offset + getSize() : 0;
    BAIL_ON_FAIL(out->write(&footer, 0, 1), ret);

    assert(out->getCurrentOffset() == offset + calculateIfdSize(size));

    // Write zeroes till word aligned
    ZERO_TILL_WORD(out, calculateIfdSize(size), ret);

    // Write values for each tag entry
    for (size_t i = 0; i < size; ++i) {
        size_t last = out->getCurrentOffset();
        // Only write values that are too large to fit in the 12-byte TIFF entry
        if (mEntries[i]->getSize() > OFFSET_SIZE) {
            BAIL_ON_FAIL(mEntries[i]->writeData(out->getCurrentOffset(), out), ret);
        }
        size_t next = out->getCurrentOffset();
        size_t diff = (next - last);
        size_t actual = mEntries[i]->getSize();
        if (diff != actual) {
            ALOGW("Sizes do not match for tag %x. Expected %zu, received %zu",
                    mEntries[i]->getTag(), actual, diff);
        }
    }

    assert(out->getCurrentOffset() == offset + getSize());

    return ret;
}

size_t TiffIfd::getSize() const {
    size_t size = mEntries.size();
    uint32_t total = calculateIfdSize(size);
    WORD_ALIGN(total);
    for (size_t i = 0; i < size; ++i) {
        total += mEntries[i]->getSize();
    }
    return total;
}

uint32_t TiffIfd::getId() const {
    return mIfdId;
}

uint32_t TiffIfd::getComparableValue() const {
    return mIfdId;
}

String8 TiffIfd::toString() const {
    size_t s = mEntries.size();
    String8 output;
    output.appendFormat("[ifd: %x, num_entries: %zu, entries:\n", getId(), s);
    for(size_t i = 0; i < mEntries.size(); ++i) {
        output.append("\t");
        output.append(mEntries[i]->toString());
        output.append("\n");
    }
    output.append(", next_ifd: %x]", ((mNextIfd != NULL) ? mNextIfd->getId() : 0));
    return output;
}

void TiffIfd::log() const {
    size_t s = mEntries.size();
    ALOGI("[ifd: %x, num_entries: %zu, entries:\n", getId(), s);
    for(size_t i = 0; i < s; ++i) {
        ALOGI("\t%s", mEntries[i]->toString().string());
    }
    ALOGI(", next_ifd: %x]", ((mNextIfd != NULL) ? mNextIfd->getId() : 0));
}

} /*namespace img_utils*/
} /*namespace android*/
