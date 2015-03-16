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

//#define LOG_NDEBUG 0
#define LOG_TAG "ProcessInfo"
#include <utils/Log.h>

#include <media/stagefright/ProcessInfo.h>

#include <binder/IProcessInfoService.h>
#include <binder/IServiceManager.h>

namespace android {

ProcessInfo::ProcessInfo() {}

bool ProcessInfo::getPriority(int pid, int* priority) {
    sp<IBinder> binder = defaultServiceManager()->getService(String16("processinfo"));
    sp<IProcessInfoService> service = interface_cast<IProcessInfoService>(binder);

    size_t length = 1;
    int32_t states;
    status_t err = service->getProcessStatesFromPids(length, &pid, &states);
    if (err != OK) {
        ALOGE("getProcessStatesFromPids failed");
        return false;
    }
    ALOGV("pid %d states %d", pid, states);
    if (states < 0) {
        return false;
    }

    // Use process state as the priority. Lower the value, higher the priority.
    *priority = states;
    return true;
}

ProcessInfo::~ProcessInfo() {}

}  // namespace android
