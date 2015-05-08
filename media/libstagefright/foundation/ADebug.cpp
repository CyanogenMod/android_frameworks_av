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

#include <errno.h>
#include <stdlib.h>
#include <ctype.h>

#define LOG_TAG "ADebug"
#include <cutils/atomic.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <cutils/properties.h>

#include <ADebug.h>
#include <AStringUtils.h>
#include <AUtils.h>

namespace android {

//static
ADebug::Level ADebug::GetDebugLevelFromString(
        const char *name, const char *value, ADebug::Level def) {
    // split on ,
    const char *next = value, *current;
    const unsigned long maxLevel = (unsigned long)kDebugMax;
    while (next != NULL) {
        current = next;
        next = strchr(current, ',');
        if (next != NULL) {
            ++next;  // pass ,
        }

        while (isspace(*current)) {
            ++current;
        }
        // check for :
        char *colon = strchr(current, ':');

        // get level
        char *end;
        errno = 0;  // strtoul does not clear errno, but it can be set for any return value
        unsigned long level = strtoul(current, &end, 10);
        while (isspace(*end)) {
            ++end;
        }
        if (errno != 0 || end == current || (end != colon && *end != '\0' && end != next)) {
            // invalid level - skip
            continue;
        }
        if (colon != NULL) {
            // check if pattern matches
            do {  // skip colon and spaces
                ++colon;
            } while (isspace(*colon));
            size_t globLen = (next == NULL ? strlen(colon) : (next - 1 - colon));
            while (globLen > 0 && isspace(colon[globLen - 1])) {
                --globLen;  // trim glob
            }

            if (!AStringUtils::MatchesGlob(
                    colon, globLen, name, strlen(name), true /* ignoreCase */)) {
                continue;
            }
        }

        // update debug level
        def = (Level)min(level, maxLevel);
    }
    return def;
}

//static
ADebug::Level ADebug::GetDebugLevelFromProperty(
        const char *name, const char *propertyName, ADebug::Level def) {
    char value[PROPERTY_VALUE_MAX];
    if (property_get(propertyName, value, NULL)) {
        return GetDebugLevelFromString(name, value, def);
    }
    return def;
}

//static
char *ADebug::GetDebugName(const char *name) {
    char *debugName = strdup(name);
    const char *terms[] = { "omx", "video", "audio" };
    for (size_t i = 0; i < NELEM(terms) && debugName != NULL; i++) {
        const char *term = terms[i];
        const size_t len = strlen(term);
        char *match = strcasestr(debugName, term);
        if (match != NULL && (match == debugName || match[-1] == '.'
                || match[len] == '.' || match[len] == '\0')) {
            char *src = match + len;
            if (match == debugName || match[-1] == '.') {
                src += (*src == '.');  // remove trailing or double .
            }
            memmove(match, src, debugName + strlen(debugName) - src + 1);
        }
    }

    return debugName;
}

//static
bool ADebug::getExperimentFlag(
        bool allow, const char *name, uint64_t modulo,
        uint64_t limit, uint64_t plus, uint64_t timeDivisor) {
    static volatile int32_t haveSerial = 0;
    static uint64_t serialNum;
    if (!android_atomic_acquire_load(&haveSerial)) {
        // calculate initial counter value based on serial number
        static char serial[PROPERTY_VALUE_MAX];
        property_get("ro.serialno", serial, "0");
        uint64_t num = 0; // it is okay for this number to overflow
        for (size_t i = 0; i < NELEM(serial) && serial[i] != '\0'; ++i) {
            const char &c = serial[i];
            // try to use most letters of serialno
            if (isdigit(c)) {
                num = num * 10 + (c - '0');
            } else if (islower(c)) {
                num = num * 26 + (c - 'a');
            } else if (isupper(c)) {
                num = num * 26 + (c - 'A');
            } else {
                num = num * 256 + c;
            }
        }
        ALOGI("got serial");
        serialNum = num;
        android_atomic_release_store(1, &haveSerial);
    }
    ALOGI("serial: %llu, time: %llu", (long long)serialNum, (long long)time(NULL));
    // MINOR: use modulo for counter and time, so that their sum does not
    // roll over, and mess up the correlation between related experiments.
    // e.g. keep (a mod 2N) = 0 impl (a mod N) = 0
    time_t counter = (time(NULL) / timeDivisor) % modulo + plus + serialNum % modulo;
    bool enable = allow && (counter % modulo < limit);
    ALOGI("experiment '%s': %s", name, enable ? "ENABLED" : "disabled");
    return enable;
}

}  // namespace android

