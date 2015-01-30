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

#define LOG_TAG "CameraModule"
//#define LOG_NDEBUG 0

#include "CameraModule.h"

namespace android {

void CameraModule::deriveCameraCharacteristicsKeys(
        uint32_t deviceVersion, CameraMetadata &chars) {
    // HAL1 devices should not reach here
    if (deviceVersion < CAMERA_DEVICE_API_VERSION_2_0) {
        ALOGV("%s: Cannot derive keys for HAL version < 2.0");
        return;
    }

    // Keys added in HAL3.3
    if (deviceVersion < CAMERA_DEVICE_API_VERSION_3_3) {
        Vector<uint8_t> controlModes;
        uint8_t data = ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE;
        chars.update(ANDROID_CONTROL_AE_LOCK_AVAILABLE, &data, /*count*/1);
        data = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE;
        chars.update(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &data, /*count*/1);
        controlModes.push(ANDROID_CONTROL_MODE_OFF);
        controlModes.push(ANDROID_CONTROL_MODE_AUTO);
        camera_metadata_entry entry = chars.find(ANDROID_CONTROL_AVAILABLE_SCENE_MODES);
        if (entry.count > 1 || entry.data.u8[0] != ANDROID_CONTROL_SCENE_MODE_DISABLED) {
            controlModes.push(ANDROID_CONTROL_MODE_USE_SCENE_MODE);
        }
        chars.update(ANDROID_CONTROL_AVAILABLE_MODES, controlModes);
    }
    return;
}

CameraModule::CameraModule(camera_module_t *module) {
    if (module == NULL) {
        ALOGE("%s: camera hardware module must not be null", __FUNCTION__);
        assert(0);
    }

    mModule = module;
    for (int i = 0; i < MAX_CAMERAS_PER_MODULE; i++) {
        mCameraInfoCached[i] = false;
    }
}

int CameraModule::getCameraInfo(int cameraId, struct camera_info *info) {
    Mutex::Autolock lock(mCameraInfoLock);
    if (cameraId < 0 || cameraId >= MAX_CAMERAS_PER_MODULE) {
        ALOGE("%s: Invalid camera ID %d", __FUNCTION__, cameraId);
        return -EINVAL;
    }

    camera_info &wrappedInfo = mCameraInfo[cameraId];
    if (!mCameraInfoCached[cameraId]) {
        camera_info rawInfo;
        int ret = mModule->get_camera_info(cameraId, &rawInfo);
        if (ret != 0) {
            return ret;
        }
        CameraMetadata &m = mCameraCharacteristics[cameraId];
        m = rawInfo.static_camera_characteristics;
        int deviceVersion;
        int apiVersion = mModule->common.module_api_version;
        if (apiVersion >= CAMERA_MODULE_API_VERSION_2_0) {
            deviceVersion = rawInfo.device_version;
        } else {
            deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;
        }
        deriveCameraCharacteristicsKeys(deviceVersion, m);
        wrappedInfo = rawInfo;
        wrappedInfo.static_camera_characteristics = m.getAndLock();
        mCameraInfoCached[cameraId] = true;
    }
    *info = wrappedInfo;
    return 0;
}

int CameraModule::open(const char* id, struct hw_device_t** device) {
    return mModule->common.methods->open(&mModule->common, id, device);
}

int CameraModule::openLegacy(
        const char* id, uint32_t halVersion, struct hw_device_t** device) {
    return mModule->open_legacy(&mModule->common, id, halVersion, device);
}

const hw_module_t* CameraModule::getRawModule() {
    return &mModule->common;
}

int CameraModule::getNumberOfCameras() {
    return mModule->get_number_of_cameras();
}

int CameraModule::setCallbacks(const camera_module_callbacks_t *callbacks) {
    return mModule->set_callbacks(callbacks);
}

bool CameraModule::isVendorTagDefined() {
    return mModule->get_vendor_tag_ops != NULL;
}

void CameraModule::getVendorTagOps(vendor_tag_ops_t* ops) {
    if (mModule->get_vendor_tag_ops) {
        mModule->get_vendor_tag_ops(ops);
    }
}

int CameraModule::setTorchMode(const char* camera_id, bool enable) {
    return mModule->set_torch_mode(camera_id, enable);
}

}; // namespace android

