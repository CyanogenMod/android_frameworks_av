/*
 * Copyright 2015 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "CallbackDataSource"
#include <utils/Log.h>

#include "include/CallbackDataSource.h"

#include <binder/IMemory.h>
#include <media/IDataSource.h>
#include <media/stagefright/foundation/ADebug.h>

#include <algorithm>

namespace android {

CallbackDataSource::CallbackDataSource(
    const sp<IDataSource>& binderDataSource)
    : mIDataSource(binderDataSource) {
    // Set up the buffer to read into.
    mMemory = mIDataSource->getIMemory();
}

CallbackDataSource::~CallbackDataSource() {
    ALOGV("~CallbackDataSource");
    mIDataSource->close();
}

status_t CallbackDataSource::initCheck() const {
    if (mMemory == NULL) {
        return UNKNOWN_ERROR;
    }
    return OK;
}

ssize_t CallbackDataSource::readAt(off64_t offset, void* data, size_t size) {
    if (mMemory == NULL) {
        return -1;
    }

    // IDataSource can only read up to mMemory->size() bytes at a time, but this
    // method should be able to read any number of bytes, so read in a loop.
    size_t totalNumRead = 0;
    size_t numLeft = size;
    const size_t bufferSize = mMemory->size();

    while (numLeft > 0) {
        size_t numToRead = std::min(numLeft, bufferSize);
        ssize_t numRead =
            mIDataSource->readAt(offset + totalNumRead, numToRead);
        // A negative return value represents an error. Pass it on.
        if (numRead < 0) {
            return numRead;
        }
        // A zero return value signals EOS. Return the bytes read so far.
        if (numRead == 0) {
            return totalNumRead;
        }
        // Sanity check.
        CHECK((size_t)numRead <= numToRead && numRead >= 0 &&
              numRead <= bufferSize);
        memcpy(((uint8_t*)data) + totalNumRead, mMemory->pointer(), numRead);
        numLeft -= numRead;
        totalNumRead += numRead;
    }

    return totalNumRead;
}

status_t CallbackDataSource::getSize(off64_t *size) {
    status_t err = mIDataSource->getSize(size);
    if (err != OK) {
        return err;
    }
    if (*size < 0) {
        // IDataSource will set size to -1 to indicate unknown size, but
        // DataSource returns ERROR_UNSUPPORTED for that.
        return ERROR_UNSUPPORTED;
    }
    return OK;
}

} // namespace android
