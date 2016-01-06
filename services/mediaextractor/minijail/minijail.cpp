/*
**
** Copyright 2015, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <cutils/log.h>
#include <libminijail.h>

#include "minijail.h"

namespace android {

/* Must match location in Android.mk */
static const char kSeccompFilePath[] = "/system/etc/seccomp_policy/mediaextractor-seccomp.policy";

int MiniJail()
{
    /* no seccomp policy for this architecture */
    if (access(kSeccompFilePath, R_OK) == -1) {
        ALOGW("No seccomp filter defined for this architecture.");
        return 0;
    }

    struct minijail *jail = minijail_new();
    if (jail == NULL) {
        ALOGW("Failed to create minijail.");
        return -1;
    }

    minijail_no_new_privs(jail);
    minijail_log_seccomp_filter_failures(jail);
    minijail_use_seccomp_filter(jail);
    minijail_parse_seccomp_filters(jail, kSeccompFilePath);
    minijail_enter(jail);
    minijail_destroy(jail);
    return 0;
}
}
