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

#include <img_utils/TiffEntryImpl.h>
#include <img_utils/TiffIfd.h>

#include <utils/Vector.h>

namespace android {
namespace img_utils {

template<>
uint32_t TiffEntryImpl<TiffIfd>::getSize() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < mCount; ++i) {
        total += mData[i].getSize();
    }
    return total;
}

template<>
status_t TiffEntryImpl<TiffIfd>::writeData(uint32_t offset, EndianOutput* out) const {
    status_t ret = OK;
    for (uint32_t i = 0; i < mCount; ++i) {
        BAIL_ON_FAIL(mData[i].writeData(offset, out), ret);
    }
    return ret;
}

} /*namespace img_utils*/
} /*namespace android*/
