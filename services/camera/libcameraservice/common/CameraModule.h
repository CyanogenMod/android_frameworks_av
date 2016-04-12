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

#ifndef ANDROID_SERVERS_CAMERA_CAMERAMODULE_H
#define ANDROID_SERVERS_CAMERA_CAMERAMODULE_H

#include <hardware/camera.h>
#include <camera/CameraMetadata.h>
#include <utils/Mutex.h>
#include <utils/KeyedVector.h>

namespace android {
/**
 * A wrapper class for HAL camera module.
 *
 * This class wraps camera_module_t returned from HAL to provide a wrapped
 * get_camera_info implementation which CameraService generates some
 * camera characteristics keys defined in newer HAL version on an older HAL.
 */
class CameraModule {
public:
    CameraModule(camera_module_t *module);
    virtual ~CameraModule();

    // Must be called after construction
    // Returns OK on success, NO_INIT on failure
    int init();

    int getCameraInfo(int cameraId, struct camera_info *info);
    int getNumberOfCameras(void);
    int open(const char* id, struct hw_device_t** device);
    int openLegacy(const char* id, uint32_t halVersion, struct hw_device_t** device);
    int setCallbacks(const camera_module_callbacks_t *callbacks);
    bool isVendorTagDefined();
    void getVendorTagOps(vendor_tag_ops_t* ops);
    int setTorchMode(const char* camera_id, bool enable);
    uint16_t getModuleApiVersion();
    const char* getModuleName();
    uint16_t getHalApiVersion();
    const char* getModuleAuthor();
    // Only used by CameraModuleFixture native test. Do NOT use elsewhere.
    void *getDso();

private:
    // Derive camera characteristics keys defined after HAL device version
    static void deriveCameraCharacteristicsKeys(uint32_t deviceVersion, CameraMetadata &chars);
    // Helper function to append available[request|result|chars]Keys
    static void appendAvailableKeys(CameraMetadata &chars,
            int32_t keyTag, const Vector<int32_t>& appendKeys);
    status_t filterOpenErrorCode(status_t err);
    camera_module_t *mModule;
    KeyedVector<int, camera_info> mCameraInfoMap;
    Mutex mCameraInfoLock;
};

} // namespace android

#endif
