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

// for property functions below
#define __STDINT_LIMITS
#include <stdint.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>


#define LOG_TAG "MediaUtils"
#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <cutils/properties.h>
#include <sys/resource.h>
#include <unistd.h>

#include "MediaUtils.h"

namespace android {

// following property functions borrowed from lmp properties.c
static intmax_t property_get_imax(const char *key, intmax_t lower_bound, intmax_t upper_bound,
        intmax_t default_value) {
    if (!key) {
        return default_value;
    }

    intmax_t result = default_value;
    char buf[PROPERTY_VALUE_MAX] = {'\0',};
    char *end = NULL;

    int len = property_get(key, buf, "");
    if (len > 0) {
        int tmp = errno;
        errno = 0;

        // Infer base automatically
        result = strtoimax(buf, &end, /*base*/0);
        if ((result == INTMAX_MIN || result == INTMAX_MAX) && errno == ERANGE) {
            // Over or underflow
            result = default_value;
            ALOGV("%s(%s,%" PRIdMAX ") - overflow", __FUNCTION__, key, default_value);
        } else if (result < lower_bound || result > upper_bound) {
            // Out of range of requested bounds
            result = default_value;
            ALOGV("%s(%s,%" PRIdMAX ") - out of range", __FUNCTION__, key, default_value);
        } else if (end == buf) {
            // Numeric conversion failed
            result = default_value;
            ALOGV("%s(%s,%" PRIdMAX ") - numeric conversion failed",
                    __FUNCTION__, key, default_value);
        }

        errno = tmp;
    }

    return result;
}

static int64_t property_get_int64(const char *key, int64_t default_value) {
    return (int64_t)property_get_imax(key, INT64_MIN, INT64_MAX, default_value);
}

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
