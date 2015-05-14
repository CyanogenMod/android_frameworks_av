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

#include <img_utils/FileInput.h>

#include <utils/Log.h>

namespace android {
namespace img_utils {

FileInput::FileInput(String8 path) : mFp(NULL), mPath(path), mOpen(false) {}

FileInput::~FileInput() {
    if (mOpen) {
        ALOGE("%s: FileInput destroyed without calling close!", __FUNCTION__);
        close();
    }

}

status_t FileInput::open() {
    if (mOpen) {
        ALOGW("%s: Open called when file %s already open.", __FUNCTION__, mPath.string());
        return OK;
    }
    mFp = ::fopen(mPath, "rb");
    if (!mFp) {
        ALOGE("%s: Could not open file %s", __FUNCTION__, mPath.string());
        return BAD_VALUE;
    }
    mOpen = true;
    return OK;
}

ssize_t FileInput::read(uint8_t* buf, size_t offset, size_t count) {
    if (!mOpen) {
        ALOGE("%s: Could not read file %s, file not open.", __FUNCTION__, mPath.string());
        return BAD_VALUE;
    }

    size_t bytesRead = ::fread(buf + offset, sizeof(uint8_t), count, mFp);
    int error = ::ferror(mFp);
    if (error != 0) {
        ALOGE("%s: Error %d occurred while reading file %s.", __FUNCTION__, error, mPath.string());
        return BAD_VALUE;
    }

    // End of file reached
    if (::feof(mFp) != 0 && bytesRead == 0) {
        return NOT_ENOUGH_DATA;
    }

    return bytesRead;
}

status_t FileInput::close() {
    if(!mOpen) {
        ALOGW("%s: Close called when file %s already close.", __FUNCTION__, mPath.string());
        return OK;
    }

    status_t ret = OK;
    if(::fclose(mFp) != 0) {
        ALOGE("%s: Failed to close file %s.", __FUNCTION__, mPath.string());
        ret = BAD_VALUE;
    }
    mOpen = false;
    return OK;
}

} /*namespace img_utils*/
} /*namespace android*/
