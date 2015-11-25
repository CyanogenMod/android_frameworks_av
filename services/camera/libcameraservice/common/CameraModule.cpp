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
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Trace.h>

#include "CameraModule.h"

namespace android {

void CameraModule::deriveCameraCharacteristicsKeys(
        uint32_t deviceVersion, CameraMetadata &chars) {
    ATRACE_CALL();

    // Keys added in HAL3.3
    if (deviceVersion < CAMERA_DEVICE_API_VERSION_3_3) {
        const size_t NUM_DERIVED_KEYS_HAL3_3 = 5;
        Vector<uint8_t> controlModes;
        uint8_t data = ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE;
        chars.update(ANDROID_CONTROL_AE_LOCK_AVAILABLE, &data, /*count*/1);
        data = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE;
        chars.update(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &data, /*count*/1);
        controlModes.push(ANDROID_CONTROL_MODE_AUTO);
        camera_metadata_entry entry = chars.find(ANDROID_CONTROL_AVAILABLE_SCENE_MODES);
        if (entry.count > 1 || entry.data.u8[0] != ANDROID_CONTROL_SCENE_MODE_DISABLED) {
            controlModes.push(ANDROID_CONTROL_MODE_USE_SCENE_MODE);
        }

        // Only advertise CONTROL_OFF mode if 3A manual controls are supported.
        bool isManualAeSupported = false;
        bool isManualAfSupported = false;
        bool isManualAwbSupported = false;
        entry = chars.find(ANDROID_CONTROL_AE_AVAILABLE_MODES);
        if (entry.count > 0) {
            for (size_t i = 0; i < entry.count; i++) {
                if (entry.data.u8[i] == ANDROID_CONTROL_AE_MODE_OFF) {
                    isManualAeSupported = true;
                    break;
                }
            }
        }
        entry = chars.find(ANDROID_CONTROL_AF_AVAILABLE_MODES);
        if (entry.count > 0) {
            for (size_t i = 0; i < entry.count; i++) {
                if (entry.data.u8[i] == ANDROID_CONTROL_AF_MODE_OFF) {
                    isManualAfSupported = true;
                    break;
                }
            }
        }
        entry = chars.find(ANDROID_CONTROL_AWB_AVAILABLE_MODES);
        if (entry.count > 0) {
            for (size_t i = 0; i < entry.count; i++) {
                if (entry.data.u8[i] == ANDROID_CONTROL_AWB_MODE_OFF) {
                    isManualAwbSupported = true;
                    break;
                }
            }
        }
        if (isManualAeSupported && isManualAfSupported && isManualAwbSupported) {
            controlModes.push(ANDROID_CONTROL_MODE_OFF);
        }

        chars.update(ANDROID_CONTROL_AVAILABLE_MODES, controlModes);

        entry = chars.find(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS);
        // HAL3.2 devices passing existing CTS test should all support all LSC modes and LSC map
        bool lensShadingModeSupported = false;
        if (entry.count > 0) {
            for (size_t i = 0; i < entry.count; i++) {
                if (entry.data.i32[i] == ANDROID_SHADING_MODE) {
                    lensShadingModeSupported = true;
                    break;
                }
            }
        }
        Vector<uint8_t> lscModes;
        Vector<uint8_t> lscMapModes;
        lscModes.push(ANDROID_SHADING_MODE_FAST);
        lscModes.push(ANDROID_SHADING_MODE_HIGH_QUALITY);
        lscMapModes.push(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF);
        if (lensShadingModeSupported) {
            lscModes.push(ANDROID_SHADING_MODE_OFF);
            lscMapModes.push(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_ON);
        }
        chars.update(ANDROID_SHADING_AVAILABLE_MODES, lscModes);
        chars.update(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES, lscMapModes);

        entry = chars.find(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
        Vector<int32_t> availableCharsKeys;
        availableCharsKeys.setCapacity(entry.count + NUM_DERIVED_KEYS_HAL3_3);
        for (size_t i = 0; i < entry.count; i++) {
            availableCharsKeys.push(entry.data.i32[i]);
        }
        availableCharsKeys.push(ANDROID_CONTROL_AE_LOCK_AVAILABLE);
        availableCharsKeys.push(ANDROID_CONTROL_AWB_LOCK_AVAILABLE);
        availableCharsKeys.push(ANDROID_CONTROL_AVAILABLE_MODES);
        availableCharsKeys.push(ANDROID_SHADING_AVAILABLE_MODES);
        availableCharsKeys.push(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES);
        chars.update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, availableCharsKeys);

        // Need update android.control.availableHighSpeedVideoConfigurations since HAL3.3
        // adds batch size to this array.
        entry = chars.find(ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS);
        if (entry.count > 0) {
            Vector<int32_t> highSpeedConfig;
            for (size_t i = 0; i < entry.count; i += 4) {
                highSpeedConfig.add(entry.data.i32[i]); // width
                highSpeedConfig.add(entry.data.i32[i + 1]); // height
                highSpeedConfig.add(entry.data.i32[i + 2]); // fps_min
                highSpeedConfig.add(entry.data.i32[i + 3]); // fps_max
                highSpeedConfig.add(1); // batchSize_max. default to 1 for HAL3.2
            }
            chars.update(ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS,
                    highSpeedConfig);
        }
    }

    // Always add a default for the pre-correction active array if the vendor chooses to omit this
    camera_metadata_entry entry = chars.find(ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE);
    if (entry.count == 0) {
        Vector<int32_t> preCorrectionArray;
        entry = chars.find(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        preCorrectionArray.appendArray(entry.data.i32, entry.count);
        chars.update(ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, preCorrectionArray);
    }

    return;
}

CameraModule::CameraModule(camera_module_t *module) {
    if (module == NULL) {
        ALOGE("%s: camera hardware module must not be null", __FUNCTION__);
        assert(0);
    }
    mModule = module;
}

CameraModule::~CameraModule()
{
    while (mCameraInfoMap.size() > 0) {
        camera_info cameraInfo = mCameraInfoMap.editValueAt(0);
        if (cameraInfo.static_camera_characteristics != NULL) {
            free_camera_metadata(
                    const_cast<camera_metadata_t*>(cameraInfo.static_camera_characteristics));
        }
        mCameraInfoMap.removeItemsAt(0);
    }
}

int CameraModule::init() {
    ATRACE_CALL();
    int res = OK;
    if (getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_4 &&
            mModule->init != NULL) {
        ATRACE_BEGIN("camera_module->init");
        res = mModule->init();
        ATRACE_END();
    }
    mCameraInfoMap.setCapacity(getNumberOfCameras());
    return res;
}

int CameraModule::getCameraInfo(int cameraId, struct camera_info *info) {
    ATRACE_CALL();
    Mutex::Autolock lock(mCameraInfoLock);
    if (cameraId < 0) {
        ALOGE("%s: Invalid camera ID %d", __FUNCTION__, cameraId);
        return -EINVAL;
    }

    // Only override static_camera_characteristics for API2 devices
    int apiVersion = mModule->common.module_api_version;
    if (apiVersion < CAMERA_MODULE_API_VERSION_2_0) {
        int ret;
        ATRACE_BEGIN("camera_module->get_camera_info");
        ret = mModule->get_camera_info(cameraId, info);
        ATRACE_END();
        return ret;
    }

    ssize_t index = mCameraInfoMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        // Get camera info from raw module and cache it
        camera_info rawInfo, cameraInfo;
        ATRACE_BEGIN("camera_module->get_camera_info");
        int ret = mModule->get_camera_info(cameraId, &rawInfo);
        ATRACE_END();
        if (ret != 0) {
            return ret;
        }
        int deviceVersion = rawInfo.device_version;
        if (deviceVersion < CAMERA_DEVICE_API_VERSION_3_0) {
            // static_camera_characteristics is invalid
            *info = rawInfo;
            return ret;
        }
        CameraMetadata m;
        m = rawInfo.static_camera_characteristics;
        deriveCameraCharacteristicsKeys(rawInfo.device_version, m);
        cameraInfo = rawInfo;
        cameraInfo.static_camera_characteristics = m.release();
        index = mCameraInfoMap.add(cameraId, cameraInfo);
    }

    assert(index != NAME_NOT_FOUND);
    // return the cached camera info
    *info = mCameraInfoMap[index];
    return OK;
}

int CameraModule::open(const char* id, struct hw_device_t** device) {
    int res;
    ATRACE_BEGIN("camera_module->open");
    res = filterOpenErrorCode(mModule->common.methods->open(&mModule->common, id, device));
    ATRACE_END();
    return res;
}

int CameraModule::openLegacy(
        const char* id, uint32_t halVersion, struct hw_device_t** device) {
    int res;
    ATRACE_BEGIN("camera_module->open_legacy");
    res = mModule->open_legacy(&mModule->common, id, halVersion, device);
    ATRACE_END();
    return res;
}

int CameraModule::getNumberOfCameras() {
    int numCameras;
    ATRACE_BEGIN("camera_module->get_number_of_cameras");
    numCameras = mModule->get_number_of_cameras();
    ATRACE_END();
    return numCameras;
}

int CameraModule::setCallbacks(const camera_module_callbacks_t *callbacks) {
    int res;
    ATRACE_BEGIN("camera_module->set_callbacks");
    res = mModule->set_callbacks(callbacks);
    ATRACE_END();
    return res;
}

bool CameraModule::isVendorTagDefined() {
    return mModule->get_vendor_tag_ops != NULL;
}

void CameraModule::getVendorTagOps(vendor_tag_ops_t* ops) {
    if (mModule->get_vendor_tag_ops) {
        ATRACE_BEGIN("camera_module->get_vendor_tag_ops");
        mModule->get_vendor_tag_ops(ops);
        ATRACE_END();
    }
}

int CameraModule::setTorchMode(const char* camera_id, bool enable) {
    int res;
    ATRACE_BEGIN("camera_module->set_torch_mode");
    res = mModule->set_torch_mode(camera_id, enable);
    ATRACE_END();
    return res;
}

status_t CameraModule::filterOpenErrorCode(status_t err) {
    switch(err) {
        case NO_ERROR:
        case -EBUSY:
        case -EINVAL:
        case -EUSERS:
            return err;
        default:
            break;
    }
    return -ENODEV;
}

uint16_t CameraModule::getModuleApiVersion() {
    return mModule->common.module_api_version;
}

const char* CameraModule::getModuleName() {
    return mModule->common.name;
}

uint16_t CameraModule::getHalApiVersion() {
    return mModule->common.hal_api_version;
}

const char* CameraModule::getModuleAuthor() {
    return mModule->common.author;
}

void* CameraModule::getDso() {
    return mModule->common.dso;
}

}; // namespace android
