/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "IcuUtils.h"

#include "unicode/putil.h"
#include "unicode/uclean.h"
#include "unicode/utypes.h"
#include "utils/Log.h"

#include <stdlib.h>

void initializeIcuOrDie() {
    const char* systemPathPrefix = getenv("ANDROID_ROOT");
    LOG_ALWAYS_FATAL_IF(systemPathPrefix == NULL, "ANDROID_ROOT environment variable not set");

    char buf[256];
    const int num_written = snprintf(buf, sizeof(buf), "%s/usr/icu/", systemPathPrefix);
    LOG_ALWAYS_FATAL_IF((num_written < 0 || static_cast<size_t>(num_written) >= sizeof(buf)),
            "Unable to construct ICU path.");

    u_setDataDirectory(buf);
    UErrorCode status = U_ZERO_ERROR;

    // u_setDataDirectory doesn't try doing anything with the directory we gave
    // it, so we'll have to call u_init to make sure it was successful.
    u_init(&status);
    LOG_ALWAYS_FATAL_IF(!U_SUCCESS(status), "Failed to initialize ICU %s", u_errorName(status));
}
