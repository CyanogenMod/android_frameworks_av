/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "MediaUtils"
#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <cutils/properties.h>
#include <sys/resource.h>
#include <unistd.h>

#include "MediaUtils.h"

namespace android {

void limitProcessMemory(
    const char *property,
    size_t numberOfBytes,
    size_t percentageOfTotalMem) {

    long pageSize = sysconf(_SC_PAGESIZE);
    long numPages = sysconf(_SC_PHYS_PAGES);
    size_t maxMem = SIZE_MAX;

    if (pageSize > 0 && numPages > 0) {
        if (size_t(numPages) < SIZE_MAX / size_t(pageSize)) {
            maxMem = size_t(numPages) * size_t(pageSize);
        }
        ALOGV("physMem: %zu", maxMem);
        if (percentageOfTotalMem > 100) {
            ALOGW("requested %zu%% of total memory, using 100%%", percentageOfTotalMem);
            percentageOfTotalMem = 100;
        }
        maxMem = maxMem / 100 * percentageOfTotalMem;
        if (numberOfBytes < maxMem) {
            maxMem = numberOfBytes;
        }
        ALOGV("requested limit: %zu", maxMem);
    } else {
        ALOGW("couldn't determine total RAM");
    }

    int64_t propVal = property_get_int64(property, maxMem);
    if (propVal > 0 && uint64_t(propVal) <= SIZE_MAX) {
        maxMem = propVal;
    }
    ALOGV("actual limit: %zu", maxMem);

    struct rlimit limit;
    getrlimit(RLIMIT_AS, &limit);
    ALOGV("original limits: %lld/%lld", (long long)limit.rlim_cur, (long long)limit.rlim_max);
    limit.rlim_cur = maxMem;
    setrlimit(RLIMIT_AS, &limit);
    limit.rlim_cur = -1;
    limit.rlim_max = -1;
    getrlimit(RLIMIT_AS, &limit);
    ALOGV("new limits: %lld/%lld", (long long)limit.rlim_cur, (long long)limit.rlim_max);

}

} // namespace android
