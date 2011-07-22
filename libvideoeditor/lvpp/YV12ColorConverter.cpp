/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <YV12ColorConverter.h>
#include <cutils/log.h>
#include <dlfcn.h>

YV12ColorConverter::YV12ColorConverter() {
    // Open the shared library
    mHandle = dlopen("libyv12colorconvert.so", RTLD_NOW);

    if (mHandle == NULL) {
        LOGW("YV12ColorConverter: cannot load libyv12colorconvert.so");
        return;
    }

    // Find the entry point
    void (*getYV12ColorConverter)(YV12ColorConverter *converter) =
        (void (*)(YV12ColorConverter*)) dlsym(mHandle, "getYV12ColorConverter");

    if (getYV12ColorConverter == NULL) {
        LOGW("YV12ColorConverter: cannot load getYV12ColorConverter");
        dlclose(mHandle);
        mHandle = NULL;
        return;
    }

    // Fill the function pointers.
    getYV12ColorConverter(this);

    LOGI("YV12ColorConverter: libyv12colorconvert.so loaded");
}

bool YV12ColorConverter::isLoaded() {
    return mHandle != NULL;
}

YV12ColorConverter::~YV12ColorConverter() {
    if (mHandle) {
        dlclose(mHandle);
    }
}
