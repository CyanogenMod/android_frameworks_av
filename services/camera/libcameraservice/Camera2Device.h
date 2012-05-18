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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2DEVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERA2DEVICE_H

#include <utils/RefBase.h>
#include <utils/Errors.h>
#include "hardware/camera2.h"

namespace android {

class Camera2Device : public virtual RefBase {
  public:
    Camera2Device(const char *name);

    ~Camera2Device();

    status_t initialize(hw_module_t *module);
  private:

    const char *mName;
    camera2_device_t *mDevice;

};

}; // namespace android

#endif
