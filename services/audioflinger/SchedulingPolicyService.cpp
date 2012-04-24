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

#include <binder/IServiceManager.h>
#include <utils/Mutex.h>
#include "ISchedulingPolicyService.h"
#include "SchedulingPolicyService.h"

namespace android {

static sp<ISchedulingPolicyService> sSchedulingPolicyService;
static const String16 _scheduling_policy("scheduling_policy");
static Mutex sMutex;

int requestPriority(pid_t pid, pid_t tid, int32_t prio)
{
    // FIXME merge duplicated code related to service lookup, caching, and error recovery
    sp<ISchedulingPolicyService> sps;
    for (;;) {
        sMutex.lock();
        sps = sSchedulingPolicyService;
        sMutex.unlock();
        if (sps != 0) {
            break;
        }
        sp<IBinder> binder = defaultServiceManager()->checkService(_scheduling_policy);
        if (binder != 0) {
            sps = interface_cast<ISchedulingPolicyService>(binder);
            sMutex.lock();
            sSchedulingPolicyService = sps;
            sMutex.unlock();
            break;
        }
        sleep(1);
    }
    return sps->requestPriority(pid, tid, prio);
}

}   // namespace android
