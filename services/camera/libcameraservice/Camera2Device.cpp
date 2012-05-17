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

#define LOG_TAG "Camera2Device"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include "Camera2Device.h"

namespace android {

Camera2Device::Camera2Device(const char *name):
        mName(name),
        mDevice(NULL)
{

}

Camera2Device::~Camera2Device()
{
    if (mDevice) {
        status_t res;
        res = mDevice->common.close(&mDevice->common);
        if (res != OK) {
            ALOGE("Could not close camera2 %s: %s (%d)",
                    mName, strerror(-res), res);
        }
    }
}

status_t Camera2Device::initialize(hw_module_t *module)
{
    status_t res;
    res = module->methods->open(module, mName,
            reinterpret_cast<hw_device_t**>(&mDevice));

    if (res != OK) {
        ALOGE("Could not open camera %s: %s (%d)", mName, strerror(-res), res);
        return res;
    }

    if (mDevice->common.version != CAMERA_DEVICE_API_VERSION_2_0) {
        ALOGE("Could not open camera %s: "
                "Camera device is not version 2.0, reports %x instead",
                mName, mDevice->common.version);
        return BAD_VALUE;
    }

    return OK;
}


}; // namespace android
