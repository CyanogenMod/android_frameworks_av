/*
 * Copyright (C) 2008 The Android Open Source Project
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

#define LOG_TAG "CameraService"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <algorithm>
#include <climits>
#include <stdio.h>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/types.h>
#include <inttypes.h>
#include <pthread.h>

#include <android/hardware/ICamera.h>
#include <android/hardware/ICameraClient.h>

#include <binder/AppOpsManager.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <binder/ProcessInfoService.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <gui/Surface.h>
#include <hardware/hardware.h>
#include <memunreachable/memunreachable.h>
#include <media/AudioSystem.h>
#include <media/IMediaHTTPService.h>
#include <media/mediaplayer.h>
#include <mediautils/BatteryNotifier.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/String16.h>
#include <utils/Trace.h>
#include <private/android_filesystem_config.h>
#include <system/camera_vendor_tags.h>
#include <system/camera_metadata.h>
#include <system/camera.h>

#include "CameraService.h"
#include "api1/CameraClient.h"
#include "api1/Camera2Client.h"
#include "api2/CameraDeviceClient.h"
#include "utils/CameraTraces.h"

namespace {
    const char* kPermissionServiceName = "permission";
}; // namespace anonymous

namespace android {

using binder::Status;
using namespace hardware;

// ----------------------------------------------------------------------------
// Logging support -- this is for debugging only
// Use "adb shell dumpsys media.camera -v 1" to change it.
volatile int32_t gLogLevel = 0;

#define LOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define LOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

static void setLogLevel(int level) {
    android_atomic_write(level, &gLogLevel);
}

// Convenience methods for constructing binder::Status objects for error returns

#define STATUS_ERROR(errorCode, errorString) \
    binder::Status::fromServiceSpecificError(errorCode, \
            String8::format("%s:%d: %s", __FUNCTION__, __LINE__, errorString))

#define STATUS_ERROR_FMT(errorCode, errorString, ...) \
    binder::Status::fromServiceSpecificError(errorCode, \
            String8::format("%s:%d: " errorString, __FUNCTION__, __LINE__, \
                    __VA_ARGS__))

// ----------------------------------------------------------------------------

extern "C" {
static void camera_device_status_change(
        const struct camera_module_callbacks* callbacks,
        int camera_id,
        int new_status) {
    sp<CameraService> cs = const_cast<CameraService*>(
            static_cast<const CameraService*>(callbacks));

    cs->onDeviceStatusChanged(static_cast<camera_device_status_t>(camera_id),
            static_cast<camera_device_status_t>(new_status));
}

static void torch_mode_status_change(
        const struct camera_module_callbacks* callbacks,
        const char* camera_id,
        int new_status) {
    if (!callbacks || !camera_id) {
        ALOGE("%s invalid parameters. callbacks %p, camera_id %p", __FUNCTION__,
                callbacks, camera_id);
    }
    sp<CameraService> cs = const_cast<CameraService*>(
                                static_cast<const CameraService*>(callbacks));

    int32_t status;
    switch (new_status) {
        case TORCH_MODE_STATUS_NOT_AVAILABLE:
            status = ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE;
            break;
        case TORCH_MODE_STATUS_AVAILABLE_OFF:
            status = ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF;
            break;
        case TORCH_MODE_STATUS_AVAILABLE_ON:
            status = ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON;
            break;
        default:
            ALOGE("Unknown torch status %d", new_status);
            return;
    }

    cs->onTorchStatusChanged(
        String8(camera_id),
        status);
}
} // extern "C"

// ----------------------------------------------------------------------------

// This is ugly and only safe if we never re-create the CameraService, but
// should be ok for now.
static CameraService *gCameraService;

CameraService::CameraService() :
        mEventLog(DEFAULT_EVENT_LOG_LENGTH),
        mNumberOfCameras(0), mNumberOfNormalCameras(0),
        mSoundRef(0), mModule(nullptr) {
    ALOGI("CameraService started (pid=%d)", getpid());
    gCameraService = this;

    this->camera_device_status_change = android::camera_device_status_change;
    this->torch_mode_status_change = android::torch_mode_status_change;

    mServiceLockWrapper = std::make_shared<WaitableMutexWrapper>(&mServiceLock);
}

void CameraService::onFirstRef()
{
    ALOGI("CameraService process starting");

    BnCameraService::onFirstRef();

    // Update battery life tracking if service is restarting
    BatteryNotifier& notifier(BatteryNotifier::getInstance());
    notifier.noteResetCamera();
    notifier.noteResetFlashlight();

    camera_module_t *rawModule;
    int err = hw_get_module(CAMERA_HARDWARE_MODULE_ID,
            (const hw_module_t **)&rawModule);
    if (err < 0) {
        ALOGE("Could not load camera HAL module: %d (%s)", err, strerror(-err));
        logServiceError("Could not load camera HAL module", err);
        return;
    }

    mModule = new CameraModule(rawModule);
    err = mModule->init();
    if (err != OK) {
        ALOGE("Could not initialize camera HAL module: %d (%s)", err,
            strerror(-err));
        logServiceError("Could not initialize camera HAL module", err);

        delete mModule;
        mModule = nullptr;
        return;
    }
    ALOGI("Loaded \"%s\" camera module", mModule->getModuleName());

    mNumberOfCameras = mModule->getNumberOfCameras();
    mNumberOfNormalCameras = mNumberOfCameras;

    // Setup vendor tags before we call get_camera_info the first time
    // because HAL might need to setup static vendor keys in get_camera_info
    VendorTagDescriptor::clearGlobalVendorTagDescriptor();
    if (mModule->getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_2) {
        setUpVendorTags();
    }

    mFlashlight = new CameraFlashlight(*mModule, *this);
    status_t res = mFlashlight->findFlashUnits();
    if (res) {
        // impossible because we haven't open any camera devices.
        ALOGE("Failed to find flash units.");
    }

    int latestStrangeCameraId = INT_MAX;
    for (int i = 0; i < mNumberOfCameras; i++) {
        String8 cameraId = String8::format("%d", i);

        // Get camera info

        struct camera_info info;
        bool haveInfo = true;
        status_t rc = mModule->getCameraInfo(i, &info);
        if (rc != NO_ERROR) {
            ALOGE("%s: Received error loading camera info for device %d, cost and"
                    " conflicting devices fields set to defaults for this device.",
                    __FUNCTION__, i);
            haveInfo = false;
        }

        // Check for backwards-compatibility support
        if (haveInfo) {
            if (checkCameraCapabilities(i, info, &latestStrangeCameraId) != OK) {
                delete mModule;
                mModule = nullptr;
                return;
            }
        }

        // Defaults to use for cost and conflicting devices
        int cost = 100;
        char** conflicting_devices = nullptr;
        size_t conflicting_devices_length = 0;

        // If using post-2.4 module version, query the cost + conflicting devices from the HAL
        if (mModule->getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_4 && haveInfo) {
            cost = info.resource_cost;
            conflicting_devices = info.conflicting_devices;
            conflicting_devices_length = info.conflicting_devices_length;
        }

        std::set<String8> conflicting;
        for (size_t i = 0; i < conflicting_devices_length; i++) {
            conflicting.emplace(String8(conflicting_devices[i]));
        }

        // Initialize state for each camera device
        {
            Mutex::Autolock lock(mCameraStatesLock);
            mCameraStates.emplace(cameraId, std::make_shared<CameraState>(cameraId, cost,
                    conflicting));
        }

        if (mFlashlight->hasFlashUnit(cameraId)) {
            mTorchStatusMap.add(cameraId,
                    ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF);
        }
    }

    if (mModule->getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_1) {
        mModule->setCallbacks(this);
    }

    CameraService::pingCameraServiceProxy();
}

sp<ICameraServiceProxy> CameraService::getCameraServiceProxy() {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.camera.proxy"));
    if (binder == nullptr) {
        return nullptr;
    }
    sp<ICameraServiceProxy> proxyBinder = interface_cast<ICameraServiceProxy>(binder);
    return proxyBinder;
}

void CameraService::pingCameraServiceProxy() {
    sp<ICameraServiceProxy> proxyBinder = getCameraServiceProxy();
    if (proxyBinder == nullptr) return;
    proxyBinder->pingForUserUpdate();
}

CameraService::~CameraService() {
    if (mModule) {
        delete mModule;
        mModule = nullptr;
    }
    VendorTagDescriptor::clearGlobalVendorTagDescriptor();
    gCameraService = nullptr;
}

void CameraService::onDeviceStatusChanged(camera_device_status_t  cameraId,
        camera_device_status_t newStatus) {
    ALOGI("%s: Status changed for cameraId=%d, newStatus=%d", __FUNCTION__,
          cameraId, newStatus);

    String8 id = String8::format("%d", cameraId);
    std::shared_ptr<CameraState> state = getCameraState(id);

    if (state == nullptr) {
        ALOGE("%s: Bad camera ID %d", __FUNCTION__, cameraId);
        return;
    }

    int32_t oldStatus = state->getStatus();

    if (oldStatus == static_cast<int32_t>(newStatus)) {
        ALOGE("%s: State transition to the same status %#x not allowed", __FUNCTION__, newStatus);
        return;
    }

    if (newStatus == CAMERA_DEVICE_STATUS_NOT_PRESENT) {
        logDeviceRemoved(id, String8::format("Device status changed from %d to %d", oldStatus,
                newStatus));
        sp<BasicClient> clientToDisconnect;
        {
            // Don't do this in updateStatus to avoid deadlock over mServiceLock
            Mutex::Autolock lock(mServiceLock);

            // Set the device status to NOT_PRESENT, clients will no longer be able to connect
            // to this device until the status changes
            updateStatus(ICameraServiceListener::STATUS_NOT_PRESENT, id);

            // Remove cached shim parameters
            state->setShimParams(CameraParameters());

            // Remove the client from the list of active clients
            clientToDisconnect = removeClientLocked(id);

            // Notify the client of disconnection
            clientToDisconnect->notifyError(
                    hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                    CaptureResultExtras{});
        }

        ALOGI("%s: Client for camera ID %s evicted due to device status change from HAL",
                __FUNCTION__, id.string());

        // Disconnect client
        if (clientToDisconnect.get() != nullptr) {
            // Ensure not in binder RPC so client disconnect PID checks work correctly
            LOG_ALWAYS_FATAL_IF(getCallingPid() != getpid(),
                    "onDeviceStatusChanged must be called from the camera service process!");
            clientToDisconnect->disconnect();
        }

    } else {
        if (oldStatus == ICameraServiceListener::STATUS_NOT_PRESENT) {
            logDeviceAdded(id, String8::format("Device status changed from %d to %d", oldStatus,
                    newStatus));
        }
        updateStatus(static_cast<int32_t>(newStatus), id);
    }

}

void CameraService::onTorchStatusChanged(const String8& cameraId,
        int32_t newStatus) {
    Mutex::Autolock al(mTorchStatusMutex);
    onTorchStatusChangedLocked(cameraId, newStatus);
}

void CameraService::onTorchStatusChangedLocked(const String8& cameraId,
        int32_t newStatus) {
    ALOGI("%s: Torch status changed for cameraId=%s, newStatus=%d",
            __FUNCTION__, cameraId.string(), newStatus);

    int32_t status;
    status_t res = getTorchStatusLocked(cameraId, &status);
    if (res) {
        ALOGE("%s: cannot get torch status of camera %s: %s (%d)",
                __FUNCTION__, cameraId.string(), strerror(-res), res);
        return;
    }
    if (status == newStatus) {
        return;
    }

    res = setTorchStatusLocked(cameraId, newStatus);
    if (res) {
        ALOGE("%s: Failed to set the torch status to %d: %s (%d)", __FUNCTION__,
                (uint32_t)newStatus, strerror(-res), res);
        return;
    }

    {
        // Update battery life logging for flashlight
        Mutex::Autolock al(mTorchUidMapMutex);
        auto iter = mTorchUidMap.find(cameraId);
        if (iter != mTorchUidMap.end()) {
            int oldUid = iter->second.second;
            int newUid = iter->second.first;
            BatteryNotifier& notifier(BatteryNotifier::getInstance());
            if (oldUid != newUid) {
                // If the UID has changed, log the status and update current UID in mTorchUidMap
                if (status == ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON) {
                    notifier.noteFlashlightOff(cameraId, oldUid);
                }
                if (newStatus == ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON) {
                    notifier.noteFlashlightOn(cameraId, newUid);
                }
                iter->second.second = newUid;
            } else {
                // If the UID has not changed, log the status
                if (newStatus == ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON) {
                    notifier.noteFlashlightOn(cameraId, oldUid);
                } else {
                    notifier.noteFlashlightOff(cameraId, oldUid);
                }
            }
        }
    }

    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto& i : mListenerList) {
            i->onTorchStatusChanged(newStatus, String16{cameraId});
        }
    }
}

Status CameraService::getNumberOfCameras(int32_t type, int32_t* numCameras) {
    ATRACE_CALL();
    switch (type) {
        case CAMERA_TYPE_BACKWARD_COMPATIBLE:
            *numCameras = mNumberOfNormalCameras;
            break;
        case CAMERA_TYPE_ALL:
            *numCameras = mNumberOfCameras;
            break;
        default:
            ALOGW("%s: Unknown camera type %d",
                    __FUNCTION__, type);
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Unknown camera type %d", type);
    }
    return Status::ok();
}

Status CameraService::getCameraInfo(int cameraId,
        CameraInfo* cameraInfo) {
    ATRACE_CALL();
    if (!mModule) {
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");
    }

    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "CameraId is not valid");
    }

    struct camera_info info;
    Status rc = filterGetInfoErrorCode(
        mModule->getCameraInfo(cameraId, &info));

    if (rc.isOk()) {
        cameraInfo->facing = info.facing;
        cameraInfo->orientation = info.orientation;
        // CameraInfo is for android.hardware.Camera which does not
        // support external camera facing. The closest approximation would be
        // front camera.
        if (cameraInfo->orientation == CAMERA_FACING_EXTERNAL) {
            cameraInfo->orientation = CAMERA_FACING_FRONT;
        }
    }
    return rc;
}

int CameraService::cameraIdToInt(const String8& cameraId) {
    errno = 0;
    size_t pos = 0;
    int ret = stoi(std::string{cameraId.string()}, &pos);
    if (errno != 0 || pos != cameraId.size()) {
        return -1;
    }
    return ret;
}

Status CameraService::generateShimMetadata(int cameraId, /*out*/CameraMetadata* cameraInfo) {
    ATRACE_CALL();

    Status ret = Status::ok();

    struct CameraInfo info;
    if (!(ret = getCameraInfo(cameraId, &info)).isOk()) {
        return ret;
    }

    CameraMetadata shimInfo;
    int32_t orientation = static_cast<int32_t>(info.orientation);
    status_t rc;
    if ((rc = shimInfo.update(ANDROID_SENSOR_ORIENTATION, &orientation, 1)) != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error updating metadata: %d (%s)", rc, strerror(-rc));
    }

    uint8_t facing = (info.facing == CAMERA_FACING_FRONT) ?
            ANDROID_LENS_FACING_FRONT : ANDROID_LENS_FACING_BACK;
    if ((rc = shimInfo.update(ANDROID_LENS_FACING, &facing, 1)) != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error updating metadata: %d (%s)", rc, strerror(-rc));
    }

    CameraParameters shimParams;
    if (!(ret = getLegacyParametersLazy(cameraId, /*out*/&shimParams)).isOk()) {
        // Error logged by callee
        return ret;
    }

    Vector<Size> sizes;
    Vector<Size> jpegSizes;
    Vector<int32_t> formats;
    {
        shimParams.getSupportedPreviewSizes(/*out*/sizes);
        shimParams.getSupportedPreviewFormats(/*out*/formats);
        shimParams.getSupportedPictureSizes(/*out*/jpegSizes);
    }

    // Always include IMPLEMENTATION_DEFINED
    formats.add(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);

    const size_t INTS_PER_CONFIG = 4;

    // Build available stream configurations metadata
    size_t streamConfigSize = (sizes.size() * formats.size() + jpegSizes.size()) * INTS_PER_CONFIG;

    Vector<int32_t> streamConfigs;
    streamConfigs.setCapacity(streamConfigSize);

    for (size_t i = 0; i < formats.size(); ++i) {
        for (size_t j = 0; j < sizes.size(); ++j) {
            streamConfigs.add(formats[i]);
            streamConfigs.add(sizes[j].width);
            streamConfigs.add(sizes[j].height);
            streamConfigs.add(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
        }
    }

    for (size_t i = 0; i < jpegSizes.size(); ++i) {
        streamConfigs.add(HAL_PIXEL_FORMAT_BLOB);
        streamConfigs.add(jpegSizes[i].width);
        streamConfigs.add(jpegSizes[i].height);
        streamConfigs.add(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
    }

    if ((rc = shimInfo.update(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            streamConfigs.array(), streamConfigSize)) != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error updating metadata: %d (%s)", rc, strerror(-rc));
    }

    int64_t fakeMinFrames[0];
    // TODO: Fixme, don't fake min frame durations.
    if ((rc = shimInfo.update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
            fakeMinFrames, 0)) != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error updating metadata: %d (%s)", rc, strerror(-rc));
    }

    int64_t fakeStalls[0];
    // TODO: Fixme, don't fake stall durations.
    if ((rc = shimInfo.update(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
            fakeStalls, 0)) != OK) {
        return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                "Error updating metadata: %d (%s)", rc, strerror(-rc));
    }

    *cameraInfo = shimInfo;
    return ret;
}

Status CameraService::getCameraCharacteristics(int cameraId,
                                                CameraMetadata* cameraInfo) {
    ATRACE_CALL();
    if (!cameraInfo) {
        ALOGE("%s: cameraInfo is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "cameraInfo is NULL");
    }

    if (!mModule) {
        ALOGE("%s: camera hardware module doesn't exist", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED,
                "Camera subsystem is not available");;
    }

    if (cameraId < 0 || cameraId >= mNumberOfCameras) {
        ALOGE("%s: Invalid camera id: %d", __FUNCTION__, cameraId);
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Invalid camera id: %d", cameraId);
    }

    int facing;
    Status ret;
    if (mModule->getModuleApiVersion() < CAMERA_MODULE_API_VERSION_2_0 ||
            getDeviceVersion(cameraId, &facing) < CAMERA_DEVICE_API_VERSION_3_0) {
        /**
         * Backwards compatibility mode for old HALs:
         * - Convert CameraInfo into static CameraMetadata properties.
         * - Retrieve cached CameraParameters for this camera.  If none exist,
         *   attempt to open CameraClient and retrieve the CameraParameters.
         * - Convert cached CameraParameters into static CameraMetadata
         *   properties.
         */
        ALOGI("%s: Switching to HAL1 shim implementation...", __FUNCTION__);

        ret = generateShimMetadata(cameraId, cameraInfo);
    } else {
        /**
         * Normal HAL 2.1+ codepath.
         */
        struct camera_info info;
        ret = filterGetInfoErrorCode(mModule->getCameraInfo(cameraId, &info));
        if (ret.isOk()) {
            *cameraInfo = info.static_camera_characteristics;
        }
    }

    return ret;
}

int CameraService::getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

int CameraService::getCallingUid() {
    return IPCThreadState::self()->getCallingUid();
}

String8 CameraService::getFormattedCurrentTime() {
    time_t now = time(nullptr);
    char formattedTime[64];
    strftime(formattedTime, sizeof(formattedTime), "%m-%d %H:%M:%S", localtime(&now));
    return String8(formattedTime);
}

int CameraService::getCameraPriorityFromProcState(int procState) {
    // Find the priority for the camera usage based on the process state.  Higher priority clients
    // win for evictions.
    if (procState < 0) {
        ALOGE("%s: Received invalid process state %d from ActivityManagerService!", __FUNCTION__,
                procState);
        return -1;
    }
    // Treat sleeping TOP processes the same as regular TOP processes, for
    // access priority.  This is important for lock-screen camera launch scenarios
    if (procState == PROCESS_STATE_TOP_SLEEPING) {
        procState = PROCESS_STATE_TOP;
    }
    return INT_MAX - procState;
}

Status CameraService::getCameraVendorTagDescriptor(
        /*out*/
        hardware::camera2::params::VendorTagDescriptor* desc) {
    ATRACE_CALL();
    if (!mModule) {
        ALOGE("%s: camera hardware module doesn't exist", __FUNCTION__);
        return STATUS_ERROR(ERROR_DISCONNECTED, "Camera subsystem not available");
    }
    sp<VendorTagDescriptor> globalDescriptor = VendorTagDescriptor::getGlobalVendorTagDescriptor();
    if (globalDescriptor != nullptr) {
        *desc = *(globalDescriptor.get());
    }
    return Status::ok();
}

int CameraService::getDeviceVersion(int cameraId, int* facing) {
    ATRACE_CALL();
    struct camera_info info;
    if (mModule->getCameraInfo(cameraId, &info) != OK) {
        return -1;
    }

    int deviceVersion;
    if (mModule->getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_0) {
        deviceVersion = info.device_version;
    } else {
        deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;
    }

    if (facing) {
        *facing = info.facing;
    }

    return deviceVersion;
}

Status CameraService::filterGetInfoErrorCode(status_t err) {
    switch(err) {
        case NO_ERROR:
            return Status::ok();
        case -EINVAL:
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                    "CameraId is not valid for HAL module");
        case -ENODEV:
            return STATUS_ERROR(ERROR_DISCONNECTED,
                    "Camera device not available");
        default:
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Camera HAL encountered error %d: %s",
                    err, strerror(-err));
    }
}

bool CameraService::setUpVendorTags() {
    ATRACE_CALL();
    vendor_tag_ops_t vOps = vendor_tag_ops_t();

    // Check if vendor operations have been implemented
    if (!mModule->isVendorTagDefined()) {
        ALOGI("%s: No vendor tags defined for this device.", __FUNCTION__);
        return false;
    }

    mModule->getVendorTagOps(&vOps);

    // Ensure all vendor operations are present
    if (vOps.get_tag_count == NULL || vOps.get_all_tags == NULL ||
            vOps.get_section_name == NULL || vOps.get_tag_name == NULL ||
            vOps.get_tag_type == NULL) {
        ALOGE("%s: Vendor tag operations not fully defined. Ignoring definitions."
               , __FUNCTION__);
        return false;
    }

    // Read all vendor tag definitions into a descriptor
    sp<VendorTagDescriptor> desc;
    status_t res;
    if ((res = VendorTagDescriptor::createDescriptorFromOps(&vOps, /*out*/desc))
            != OK) {
        ALOGE("%s: Could not generate descriptor from vendor tag operations,"
              "received error %s (%d). Camera clients will not be able to use"
              "vendor tags", __FUNCTION__, strerror(res), res);
        return false;
    }

    // Set the global descriptor to use with camera metadata
    VendorTagDescriptor::setAsGlobalVendorTagDescriptor(desc);
    return true;
}

Status CameraService::makeClient(const sp<CameraService>& cameraService,
        const sp<IInterface>& cameraCb, const String16& packageName, int cameraId,
        int facing, int clientPid, uid_t clientUid, int servicePid, bool legacyMode,
        int halVersion, int deviceVersion, apiLevel effectiveApiLevel,
        /*out*/sp<BasicClient>* client) {

    if (halVersion < 0 || halVersion == deviceVersion) {
        // Default path: HAL version is unspecified by caller, create CameraClient
        // based on device version reported by the HAL.
        switch(deviceVersion) {
          case CAMERA_DEVICE_API_VERSION_1_0:
            if (effectiveApiLevel == API_1) {  // Camera1 API route
                sp<ICameraClient> tmp = static_cast<ICameraClient*>(cameraCb.get());
                *client = new CameraClient(cameraService, tmp, packageName, cameraId, facing,
                        clientPid, clientUid, getpid(), legacyMode);
            } else { // Camera2 API route
                ALOGW("Camera using old HAL version: %d", deviceVersion);
                return STATUS_ERROR_FMT(ERROR_DEPRECATED_HAL,
                        "Camera device \"%d\" HAL version %d does not support camera2 API",
                        cameraId, deviceVersion);
            }
            break;
          case CAMERA_DEVICE_API_VERSION_3_0:
          case CAMERA_DEVICE_API_VERSION_3_1:
          case CAMERA_DEVICE_API_VERSION_3_2:
          case CAMERA_DEVICE_API_VERSION_3_3:
          case CAMERA_DEVICE_API_VERSION_3_4:
            if (effectiveApiLevel == API_1) { // Camera1 API route
                sp<ICameraClient> tmp = static_cast<ICameraClient*>(cameraCb.get());
                *client = new Camera2Client(cameraService, tmp, packageName, cameraId, facing,
                        clientPid, clientUid, servicePid, legacyMode);
            } else { // Camera2 API route
                sp<hardware::camera2::ICameraDeviceCallbacks> tmp =
                        static_cast<hardware::camera2::ICameraDeviceCallbacks*>(cameraCb.get());
                *client = new CameraDeviceClient(cameraService, tmp, packageName, cameraId,
                        facing, clientPid, clientUid, servicePid);
            }
            break;
          default:
            // Should not be reachable
            ALOGE("Unknown camera device HAL version: %d", deviceVersion);
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Camera device \"%d\" has unknown HAL version %d",
                    cameraId, deviceVersion);
        }
    } else {
        // A particular HAL version is requested by caller. Create CameraClient
        // based on the requested HAL version.
        if (deviceVersion > CAMERA_DEVICE_API_VERSION_1_0 &&
            halVersion == CAMERA_DEVICE_API_VERSION_1_0) {
            // Only support higher HAL version device opened as HAL1.0 device.
            sp<ICameraClient> tmp = static_cast<ICameraClient*>(cameraCb.get());
            *client = new CameraClient(cameraService, tmp, packageName, cameraId, facing,
                    clientPid, clientUid, servicePid, legacyMode);
        } else {
            // Other combinations (e.g. HAL3.x open as HAL2.x) are not supported yet.
            ALOGE("Invalid camera HAL version %x: HAL %x device can only be"
                    " opened as HAL %x device", halVersion, deviceVersion,
                    CAMERA_DEVICE_API_VERSION_1_0);
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Camera device \"%d\" (HAL version %d) cannot be opened as HAL version %d",
                    cameraId, deviceVersion, halVersion);
        }
    }
    return Status::ok();
}

String8 CameraService::toString(std::set<userid_t> intSet) {
    String8 s("");
    bool first = true;
    for (userid_t i : intSet) {
        if (first) {
            s.appendFormat("%d", i);
            first = false;
        } else {
            s.appendFormat(", %d", i);
        }
    }
    return s;
}

Status CameraService::initializeShimMetadata(int cameraId) {
    int uid = getCallingUid();

    String16 internalPackageName("cameraserver");
    String8 id = String8::format("%d", cameraId);
    Status ret = Status::ok();
    sp<Client> tmp = nullptr;
    if (!(ret = connectHelper<ICameraClient,Client>(
            sp<ICameraClient>{nullptr}, id, static_cast<int>(CAMERA_HAL_API_VERSION_UNSPECIFIED),
            internalPackageName, uid, USE_CALLING_PID,
            API_1, /*legacyMode*/ false, /*shimUpdateOnly*/ true,
            /*out*/ tmp)
            ).isOk()) {
        ALOGE("%s: Error initializing shim metadata: %s", __FUNCTION__, ret.toString8().string());
    }
    return ret;
}

Status CameraService::getLegacyParametersLazy(int cameraId,
        /*out*/
        CameraParameters* parameters) {

    ALOGV("%s: for cameraId: %d", __FUNCTION__, cameraId);

    Status ret = Status::ok();

    if (parameters == NULL) {
        ALOGE("%s: parameters must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Parameters must not be null");
    }

    String8 id = String8::format("%d", cameraId);

    // Check if we already have parameters
    {
        // Scope for service lock
        Mutex::Autolock lock(mServiceLock);
        auto cameraState = getCameraState(id);
        if (cameraState == nullptr) {
            ALOGE("%s: Invalid camera ID: %s", __FUNCTION__, id.string());
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Invalid camera ID: %s", id.string());
        }
        CameraParameters p = cameraState->getShimParams();
        if (!p.isEmpty()) {
            *parameters = p;
            return ret;
        }
    }

    int64_t token = IPCThreadState::self()->clearCallingIdentity();
    ret = initializeShimMetadata(cameraId);
    IPCThreadState::self()->restoreCallingIdentity(token);
    if (!ret.isOk()) {
        // Error already logged by callee
        return ret;
    }

    // Check for parameters again
    {
        // Scope for service lock
        Mutex::Autolock lock(mServiceLock);
        auto cameraState = getCameraState(id);
        if (cameraState == nullptr) {
            ALOGE("%s: Invalid camera ID: %s", __FUNCTION__, id.string());
            return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                    "Invalid camera ID: %s", id.string());
        }
        CameraParameters p = cameraState->getShimParams();
        if (!p.isEmpty()) {
            *parameters = p;
            return ret;
        }
    }

    ALOGE("%s: Parameters were not initialized, or were empty.  Device may not be present.",
            __FUNCTION__);
    return STATUS_ERROR(ERROR_INVALID_OPERATION, "Unable to initialize legacy parameters");
}

// Can camera service trust the caller based on the calling UID?
static bool isTrustedCallingUid(uid_t uid) {
    switch (uid) {
        case AID_MEDIA:        // mediaserver
        case AID_CAMERASERVER: // cameraserver
        case AID_RADIO:        // telephony
            return true;
        default:
            return false;
    }
}

Status CameraService::validateConnectLocked(const String8& cameraId,
        const String8& clientName8, /*inout*/int& clientUid, /*inout*/int& clientPid,
        /*out*/int& originalClientPid) const {

    int callingPid = getCallingPid();
    int callingUid = getCallingUid();

    // Check if we can trust clientUid
    if (clientUid == USE_CALLING_UID) {
        clientUid = callingUid;
    } else if (!isTrustedCallingUid(callingUid)) {
        ALOGE("CameraService::connect X (calling PID %d, calling UID %d) rejected "
                "(don't trust clientUid %d)", callingPid, callingUid, clientUid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Untrusted caller (calling PID %d, UID %d) trying to "
                "forward camera access to camera %s for client %s (PID %d, UID %d)",
                callingPid, callingUid, cameraId.string(),
                clientName8.string(), clientUid, clientPid);
    }

    // Check if we can trust clientPid
    if (clientPid == USE_CALLING_PID) {
        clientPid = callingPid;
    } else if (!isTrustedCallingUid(callingUid)) {
        ALOGE("CameraService::connect X (calling PID %d, calling UID %d) rejected "
                "(don't trust clientPid %d)", callingPid, callingUid, clientPid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Untrusted caller (calling PID %d, UID %d) trying to "
                "forward camera access to camera %s for client %s (PID %d, UID %d)",
                callingPid, callingUid, cameraId.string(),
                clientName8.string(), clientUid, clientPid);
    }

    // If it's not calling from cameraserver, check the permission.
    if (callingPid != getpid() &&
            !checkPermission(String16("android.permission.CAMERA"), clientPid, clientUid)) {
        ALOGE("Permission Denial: can't use the camera pid=%d, uid=%d", clientPid, clientUid);
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Caller \"%s\" (PID %d, UID %d) cannot open camera \"%s\" without camera permission",
                clientName8.string(), clientUid, clientPid, cameraId.string());
    }

    // Only use passed in clientPid to check permission. Use calling PID as the client PID that's
    // connected to camera service directly.
    originalClientPid = clientPid;
    clientPid = callingPid;

    if (!mModule) {
        ALOGE("CameraService::connect X (PID %d) rejected (camera HAL module not loaded)",
                callingPid);
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                "No camera HAL module available to open camera device \"%s\"", cameraId.string());
    }

    if (getCameraState(cameraId) == nullptr) {
        ALOGE("CameraService::connect X (PID %d) rejected (invalid camera ID %s)", callingPid,
                cameraId.string());
        return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                "No camera device with ID \"%s\" available", cameraId.string());
    }

    userid_t clientUserId = multiuser_get_user_id(clientUid);

    // Only allow clients who are being used by the current foreground device user, unless calling
    // from our own process.
    if (callingPid != getpid() && (mAllowedUsers.find(clientUserId) == mAllowedUsers.end())) {
        ALOGE("CameraService::connect X (PID %d) rejected (cannot connect from "
                "device user %d, currently allowed device users: %s)", callingPid, clientUserId,
                toString(mAllowedUsers).string());
        return STATUS_ERROR_FMT(ERROR_PERMISSION_DENIED,
                "Callers from device user %d are not currently allowed to connect to camera \"%s\"",
                clientUserId, cameraId.string());
    }

    status_t err = checkIfDeviceIsUsable(cameraId);
    if (err != NO_ERROR) {
        switch(err) {
            case -ENODEV:
            case -EBUSY:
                return STATUS_ERROR_FMT(ERROR_DISCONNECTED,
                        "No camera device with ID \"%s\" currently available", cameraId.string());
            default:
                return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                        "Unknown error connecting to ID \"%s\"", cameraId.string());
        }
    }
    return Status::ok();
}

status_t CameraService::checkIfDeviceIsUsable(const String8& cameraId) const {
    auto cameraState = getCameraState(cameraId);
    int callingPid = getCallingPid();
    if (cameraState == nullptr) {
        ALOGE("CameraService::connect X (PID %d) rejected (invalid camera ID %s)", callingPid,
                cameraId.string());
        return -ENODEV;
    }

    int32_t currentStatus = cameraState->getStatus();
    if (currentStatus == ICameraServiceListener::STATUS_NOT_PRESENT) {
        ALOGE("CameraService::connect X (PID %d) rejected (camera %s is not connected)",
                callingPid, cameraId.string());
        return -ENODEV;
    } else if (currentStatus == ICameraServiceListener::STATUS_ENUMERATING) {
        ALOGE("CameraService::connect X (PID %d) rejected, (camera %s is initializing)",
                callingPid, cameraId.string());
        return -EBUSY;
    }

    return NO_ERROR;
}

void CameraService::finishConnectLocked(const sp<BasicClient>& client,
        const CameraService::DescriptorPtr& desc) {

    // Make a descriptor for the incoming client
    auto clientDescriptor = CameraService::CameraClientManager::makeClientDescriptor(client, desc);
    auto evicted = mActiveClientManager.addAndEvict(clientDescriptor);

    logConnected(desc->getKey(), static_cast<int>(desc->getOwnerId()),
            String8(client->getPackageName()));

    if (evicted.size() > 0) {
        // This should never happen - clients should already have been removed in disconnect
        for (auto& i : evicted) {
            ALOGE("%s: Invalid state: Client for camera %s was not removed in disconnect",
                    __FUNCTION__, i->getKey().string());
        }

        LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, clients not evicted properly",
                __FUNCTION__);
    }

    // And register a death notification for the client callback. Do
    // this last to avoid Binder policy where a nested Binder
    // transaction might be pre-empted to service the client death
    // notification if the client process dies before linkToDeath is
    // invoked.
    sp<IBinder> remoteCallback = client->getRemote();
    if (remoteCallback != nullptr) {
        remoteCallback->linkToDeath(this);
    }
}

status_t CameraService::handleEvictionsLocked(const String8& cameraId, int clientPid,
        apiLevel effectiveApiLevel, const sp<IBinder>& remoteCallback, const String8& packageName,
        /*out*/
        sp<BasicClient>* client,
        std::shared_ptr<resource_policy::ClientDescriptor<String8, sp<BasicClient>>>* partial) {
    ATRACE_CALL();
    status_t ret = NO_ERROR;
    std::vector<DescriptorPtr> evictedClients;
    DescriptorPtr clientDescriptor;
    {
        if (effectiveApiLevel == API_1) {
            // If we are using API1, any existing client for this camera ID with the same remote
            // should be returned rather than evicted to allow MediaRecorder to work properly.

            auto current = mActiveClientManager.get(cameraId);
            if (current != nullptr) {
                auto clientSp = current->getValue();
                if (clientSp.get() != nullptr) { // should never be needed
                    if (!clientSp->canCastToApiClient(effectiveApiLevel)) {
                        ALOGW("CameraService connect called from same client, but with a different"
                                " API level, evicting prior client...");
                    } else if (clientSp->getRemote() == remoteCallback) {
                        ALOGI("CameraService::connect X (PID %d) (second call from same"
                                " app binder, returning the same client)", clientPid);
                        *client = clientSp;
                        return NO_ERROR;
                    }
                }
            }
        }

        // Get current active client PIDs
        std::vector<int> ownerPids(mActiveClientManager.getAllOwners());
        ownerPids.push_back(clientPid);

        // Use the value +PROCESS_STATE_NONEXISTENT, to avoid taking
        // address of PROCESS_STATE_NONEXISTENT as a reference argument
        // for the vector constructor. PROCESS_STATE_NONEXISTENT does
        // not have an out-of-class definition.
        std::vector<int> priorities(ownerPids.size(), +PROCESS_STATE_NONEXISTENT);

        // Get priorites of all active PIDs
        ProcessInfoService::getProcessStatesFromPids(ownerPids.size(), &ownerPids[0],
                /*out*/&priorities[0]);

        // Update all active clients' priorities
        std::map<int,int> pidToPriorityMap;
        for (size_t i = 0; i < ownerPids.size() - 1; i++) {
            pidToPriorityMap.emplace(ownerPids[i], getCameraPriorityFromProcState(priorities[i]));
        }
        mActiveClientManager.updatePriorities(pidToPriorityMap);

        // Get state for the given cameraId
        auto state = getCameraState(cameraId);
        if (state == nullptr) {
            ALOGE("CameraService::connect X (PID %d) rejected (no camera device with ID %s)",
                clientPid, cameraId.string());
            // Should never get here because validateConnectLocked should have errored out
            return BAD_VALUE;
        }

        // Make descriptor for incoming client
        clientDescriptor = CameraClientManager::makeClientDescriptor(cameraId,
                sp<BasicClient>{nullptr}, static_cast<int32_t>(state->getCost()),
                state->getConflicting(),
                getCameraPriorityFromProcState(priorities[priorities.size() - 1]), clientPid);

        // Find clients that would be evicted
        auto evicted = mActiveClientManager.wouldEvict(clientDescriptor);

        // If the incoming client was 'evicted,' higher priority clients have the camera in the
        // background, so we cannot do evictions
        if (std::find(evicted.begin(), evicted.end(), clientDescriptor) != evicted.end()) {
            ALOGE("CameraService::connect X (PID %d) rejected (existing client(s) with higher"
                    " priority).", clientPid);

            sp<BasicClient> clientSp = clientDescriptor->getValue();
            String8 curTime = getFormattedCurrentTime();
            auto incompatibleClients =
                    mActiveClientManager.getIncompatibleClients(clientDescriptor);

            String8 msg = String8::format("%s : DENIED connect device %s client for package %s "
                    "(PID %d, priority %d) due to eviction policy", curTime.string(),
                    cameraId.string(), packageName.string(), clientPid,
                    getCameraPriorityFromProcState(priorities[priorities.size() - 1]));

            for (auto& i : incompatibleClients) {
                msg.appendFormat("\n   - Blocked by existing device %s client for package %s"
                        "(PID %" PRId32 ", priority %" PRId32 ")", i->getKey().string(),
                        String8{i->getValue()->getPackageName()}.string(), i->getOwnerId(),
                        i->getPriority());
                ALOGE("   Conflicts with: Device %s, client package %s (PID %"
                        PRId32 ", priority %" PRId32 ")", i->getKey().string(),
                        String8{i->getValue()->getPackageName()}.string(), i->getOwnerId(),
                        i->getPriority());
            }

            // Log the client's attempt
            Mutex::Autolock l(mLogLock);
            mEventLog.add(msg);

            return -EBUSY;
        }

        for (auto& i : evicted) {
            sp<BasicClient> clientSp = i->getValue();
            if (clientSp.get() == nullptr) {
                ALOGE("%s: Invalid state: Null client in active client list.", __FUNCTION__);

                // TODO: Remove this
                LOG_ALWAYS_FATAL("%s: Invalid state for CameraService, null client in active list",
                        __FUNCTION__);
                mActiveClientManager.remove(i);
                continue;
            }

            ALOGE("CameraService::connect evicting conflicting client for camera ID %s",
                    i->getKey().string());
            evictedClients.push_back(i);

            // Log the clients evicted
            logEvent(String8::format("EVICT device %s client held by package %s (PID"
                    " %" PRId32 ", priority %" PRId32 ")\n   - Evicted by device %s client for"
                    " package %s (PID %d, priority %" PRId32 ")",
                    i->getKey().string(), String8{clientSp->getPackageName()}.string(),
                    i->getOwnerId(), i->getPriority(), cameraId.string(),
                    packageName.string(), clientPid,
                    getCameraPriorityFromProcState(priorities[priorities.size() - 1])));

            // Notify the client of disconnection
            clientSp->notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                    CaptureResultExtras());
        }
    }

    // Do not hold mServiceLock while disconnecting clients, but retain the condition blocking
    // other clients from connecting in mServiceLockWrapper if held
    mServiceLock.unlock();

    // Clear caller identity temporarily so client disconnect PID checks work correctly
    int64_t token = IPCThreadState::self()->clearCallingIdentity();

    // Destroy evicted clients
    for (auto& i : evictedClients) {
        // Disconnect is blocking, and should only have returned when HAL has cleaned up
        i->getValue()->disconnect(); // Clients will remove themselves from the active client list
    }

    IPCThreadState::self()->restoreCallingIdentity(token);

    for (const auto& i : evictedClients) {
        ALOGV("%s: Waiting for disconnect to complete for client for device %s (PID %" PRId32 ")",
                __FUNCTION__, i->getKey().string(), i->getOwnerId());
        ret = mActiveClientManager.waitUntilRemoved(i, DEFAULT_DISCONNECT_TIMEOUT_NS);
        if (ret == TIMED_OUT) {
            ALOGE("%s: Timed out waiting for client for device %s to disconnect, "
                    "current clients:\n%s", __FUNCTION__, i->getKey().string(),
                    mActiveClientManager.toString().string());
            return -EBUSY;
        }
        if (ret != NO_ERROR) {
            ALOGE("%s: Received error waiting for client for device %s to disconnect: %s (%d), "
                    "current clients:\n%s", __FUNCTION__, i->getKey().string(), strerror(-ret),
                    ret, mActiveClientManager.toString().string());
            return ret;
        }
    }

    evictedClients.clear();

    // Once clients have been disconnected, relock
    mServiceLock.lock();

    // Check again if the device was unplugged or something while we weren't holding mServiceLock
    if ((ret = checkIfDeviceIsUsable(cameraId)) != NO_ERROR) {
        return ret;
    }

    *partial = clientDescriptor;
    return NO_ERROR;
}

Status CameraService::connect(
        const sp<ICameraClient>& cameraClient,
        int cameraId,
        const String16& clientPackageName,
        int clientUid,
        int clientPid,
        /*out*/
        sp<ICamera>* device) {

    ATRACE_CALL();
    Status ret = Status::ok();
    String8 id = String8::format("%d", cameraId);
    sp<Client> client = nullptr;
    ret = connectHelper<ICameraClient,Client>(cameraClient, id,
            CAMERA_HAL_API_VERSION_UNSPECIFIED, clientPackageName, clientUid, clientPid, API_1,
            /*legacyMode*/ false, /*shimUpdateOnly*/ false,
            /*out*/client);

    if(!ret.isOk()) {
        logRejected(id, getCallingPid(), String8(clientPackageName),
                ret.toString8());
        return ret;
    }

    *device = client;
    return ret;
}

Status CameraService::connectLegacy(
        const sp<ICameraClient>& cameraClient,
        int cameraId, int halVersion,
        const String16& clientPackageName,
        int clientUid,
        /*out*/
        sp<ICamera>* device) {

    ATRACE_CALL();
    String8 id = String8::format("%d", cameraId);
    int apiVersion = mModule->getModuleApiVersion();
    if (halVersion != CAMERA_HAL_API_VERSION_UNSPECIFIED &&
            apiVersion < CAMERA_MODULE_API_VERSION_2_3) {
        /*
         * Either the HAL version is unspecified in which case this just creates
         * a camera client selected by the latest device version, or
         * it's a particular version in which case the HAL must supported
         * the open_legacy call
         */
        String8 msg = String8::format("Camera HAL module version %x too old for connectLegacy!",
                apiVersion);
        ALOGE("%s: %s",
                __FUNCTION__, msg.string());
        logRejected(id, getCallingPid(), String8(clientPackageName),
                msg);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    Status ret = Status::ok();
    sp<Client> client = nullptr;
    ret = connectHelper<ICameraClient,Client>(cameraClient, id, halVersion,
            clientPackageName, clientUid, USE_CALLING_PID, API_1,
            /*legacyMode*/ true, /*shimUpdateOnly*/ false,
            /*out*/client);

    if(!ret.isOk()) {
        logRejected(id, getCallingPid(), String8(clientPackageName),
                ret.toString8());
        return ret;
    }

    *device = client;
    return ret;
}

Status CameraService::connectDevice(
        const sp<hardware::camera2::ICameraDeviceCallbacks>& cameraCb,
        int cameraId,
        const String16& clientPackageName,
        int clientUid,
        /*out*/
        sp<hardware::camera2::ICameraDeviceUser>* device) {

    ATRACE_CALL();
    Status ret = Status::ok();
    String8 id = String8::format("%d", cameraId);
    sp<CameraDeviceClient> client = nullptr;
    ret = connectHelper<hardware::camera2::ICameraDeviceCallbacks,CameraDeviceClient>(cameraCb, id,
            CAMERA_HAL_API_VERSION_UNSPECIFIED, clientPackageName,
            clientUid, USE_CALLING_PID, API_2,
            /*legacyMode*/ false, /*shimUpdateOnly*/ false,
            /*out*/client);

    if(!ret.isOk()) {
        logRejected(id, getCallingPid(), String8(clientPackageName),
                ret.toString8());
        return ret;
    }

    *device = client;
    return ret;
}

Status CameraService::setTorchMode(const String16& cameraId, bool enabled,
        const sp<IBinder>& clientBinder) {

    ATRACE_CALL();
    if (enabled && clientBinder == nullptr) {
        ALOGE("%s: torch client binder is NULL", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT,
                "Torch client Binder is null");
    }

    String8 id = String8(cameraId.string());
    int uid = getCallingUid();

    // verify id is valid.
    auto state = getCameraState(id);
    if (state == nullptr) {
        ALOGE("%s: camera id is invalid %s", __FUNCTION__, id.string());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", id.string());
    }

    int32_t cameraStatus = state->getStatus();
    if (cameraStatus != ICameraServiceListener::STATUS_PRESENT &&
            cameraStatus != ICameraServiceListener::STATUS_NOT_AVAILABLE) {
        ALOGE("%s: camera id is invalid %s", __FUNCTION__, id.string());
        return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                "Camera ID \"%s\" is a not valid camera ID", id.string());
    }

    {
        Mutex::Autolock al(mTorchStatusMutex);
        int32_t status;
        status_t err = getTorchStatusLocked(id, &status);
        if (err != OK) {
            if (err == NAME_NOT_FOUND) {
                return STATUS_ERROR_FMT(ERROR_ILLEGAL_ARGUMENT,
                        "Camera \"%s\" does not have a flash unit", id.string());
            }
            ALOGE("%s: getting current torch status failed for camera %s",
                    __FUNCTION__, id.string());
            return STATUS_ERROR_FMT(ERROR_INVALID_OPERATION,
                    "Error updating torch status for camera \"%s\": %s (%d)", id.string(),
                    strerror(-err), err);
        }

        if (status == ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE) {
            if (cameraStatus == ICameraServiceListener::STATUS_NOT_AVAILABLE) {
                ALOGE("%s: torch mode of camera %s is not available because "
                        "camera is in use", __FUNCTION__, id.string());
                return STATUS_ERROR_FMT(ERROR_CAMERA_IN_USE,
                        "Torch for camera \"%s\" is not available due to an existing camera user",
                        id.string());
            } else {
                ALOGE("%s: torch mode of camera %s is not available due to "
                        "insufficient resources", __FUNCTION__, id.string());
                return STATUS_ERROR_FMT(ERROR_MAX_CAMERAS_IN_USE,
                        "Torch for camera \"%s\" is not available due to insufficient resources",
                        id.string());
            }
        }
    }

    {
        // Update UID map - this is used in the torch status changed callbacks, so must be done
        // before setTorchMode
        Mutex::Autolock al(mTorchUidMapMutex);
        if (mTorchUidMap.find(id) == mTorchUidMap.end()) {
            mTorchUidMap[id].first = uid;
            mTorchUidMap[id].second = uid;
        } else {
            // Set the pending UID
            mTorchUidMap[id].first = uid;
        }
    }

    status_t err = mFlashlight->setTorchMode(id, enabled);

    if (err != OK) {
        int32_t errorCode;
        String8 msg;
        switch (err) {
            case -ENOSYS:
                msg = String8::format("Camera \"%s\" has no flashlight",
                    id.string());
                errorCode = ERROR_ILLEGAL_ARGUMENT;
                break;
            default:
                msg = String8::format(
                    "Setting torch mode of camera \"%s\" to %d failed: %s (%d)",
                    id.string(), enabled, strerror(-err), err);
                errorCode = ERROR_INVALID_OPERATION;
        }
        ALOGE("%s: %s", __FUNCTION__, msg.string());
        return STATUS_ERROR(errorCode, msg.string());
    }

    {
        // update the link to client's death
        Mutex::Autolock al(mTorchClientMapMutex);
        ssize_t index = mTorchClientMap.indexOfKey(id);
        if (enabled) {
            if (index == NAME_NOT_FOUND) {
                mTorchClientMap.add(id, clientBinder);
            } else {
                mTorchClientMap.valueAt(index)->unlinkToDeath(this);
                mTorchClientMap.replaceValueAt(index, clientBinder);
            }
            clientBinder->linkToDeath(this);
        } else if (index != NAME_NOT_FOUND) {
            mTorchClientMap.valueAt(index)->unlinkToDeath(this);
        }
    }

    return Status::ok();
}

Status CameraService::notifySystemEvent(int32_t eventId,
        const std::vector<int32_t>& args) {
    ATRACE_CALL();

    switch(eventId) {
        case ICameraService::EVENT_USER_SWITCHED: {
            doUserSwitch(/*newUserIds*/ args);
            break;
        }
        case ICameraService::EVENT_NONE:
        default: {
            ALOGW("%s: Received invalid system event from system_server: %d", __FUNCTION__,
                    eventId);
            break;
        }
    }
    return Status::ok();
}

Status CameraService::addListener(const sp<ICameraServiceListener>& listener) {
    ATRACE_CALL();

    ALOGV("%s: Add listener %p", __FUNCTION__, listener.get());

    if (listener == nullptr) {
        ALOGE("%s: Listener must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Null listener given to addListener");
    }

    Mutex::Autolock lock(mServiceLock);

    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto& it : mListenerList) {
            if (IInterface::asBinder(it) == IInterface::asBinder(listener)) {
                ALOGW("%s: Tried to add listener %p which was already subscribed",
                      __FUNCTION__, listener.get());
                return STATUS_ERROR(ERROR_ALREADY_EXISTS, "Listener already registered");
            }
        }

        mListenerList.push_back(listener);
    }


    /* Immediately signal current status to this listener only */
    {
        Mutex::Autolock lock(mCameraStatesLock);
        for (auto& i : mCameraStates) {
            // TODO: Update binder to use String16 for camera IDs and remove;
            int id = cameraIdToInt(i.first);
            if (id == -1) continue;

            listener->onStatusChanged(i.second->getStatus(), id);
        }
    }

    /* Immediately signal current torch status to this listener only */
    {
        Mutex::Autolock al(mTorchStatusMutex);
        for (size_t i = 0; i < mTorchStatusMap.size(); i++ ) {
            String16 id = String16(mTorchStatusMap.keyAt(i).string());
            listener->onTorchStatusChanged(mTorchStatusMap.valueAt(i), id);
        }
    }

    return Status::ok();
}

Status CameraService::removeListener(const sp<ICameraServiceListener>& listener) {
    ATRACE_CALL();

    ALOGV("%s: Remove listener %p", __FUNCTION__, listener.get());

    if (listener == 0) {
        ALOGE("%s: Listener must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Null listener given to removeListener");
    }

    Mutex::Autolock lock(mServiceLock);

    {
        Mutex::Autolock lock(mStatusListenerLock);
        for (auto it = mListenerList.begin(); it != mListenerList.end(); it++) {
            if (IInterface::asBinder(*it) == IInterface::asBinder(listener)) {
                mListenerList.erase(it);
                return Status::ok();
            }
        }
    }

    ALOGW("%s: Tried to remove a listener %p which was not subscribed",
          __FUNCTION__, listener.get());

    return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Unregistered listener given to removeListener");
}

Status CameraService::getLegacyParameters(int cameraId, /*out*/String16* parameters) {

    ATRACE_CALL();
    ALOGV("%s: for camera ID = %d", __FUNCTION__, cameraId);

    if (parameters == NULL) {
        ALOGE("%s: parameters must not be null", __FUNCTION__);
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Parameters must not be null");
    }

    Status ret = Status::ok();

    CameraParameters shimParams;
    if (!(ret = getLegacyParametersLazy(cameraId, /*out*/&shimParams)).isOk()) {
        // Error logged by caller
        return ret;
    }

    String8 shimParamsString8 = shimParams.flatten();
    String16 shimParamsString16 = String16(shimParamsString8);

    *parameters = shimParamsString16;

    return ret;
}

Status CameraService::supportsCameraApi(int cameraId, int apiVersion, bool *isSupported) {
    ATRACE_CALL();

    ALOGV("%s: for camera ID = %d", __FUNCTION__, cameraId);

    switch (apiVersion) {
        case API_VERSION_1:
        case API_VERSION_2:
            break;
        default:
            String8 msg = String8::format("Unknown API version %d", apiVersion);
            ALOGE("%s: %s", __FUNCTION__, msg.string());
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.string());
    }

    int facing = -1;
    int deviceVersion = getDeviceVersion(cameraId, &facing);

    switch(deviceVersion) {
        case CAMERA_DEVICE_API_VERSION_1_0:
        case CAMERA_DEVICE_API_VERSION_3_0:
        case CAMERA_DEVICE_API_VERSION_3_1:
            if (apiVersion == API_VERSION_2) {
                ALOGV("%s: Camera id %d uses HAL version %d <3.2, doesn't support api2 without shim",
                        __FUNCTION__, cameraId, deviceVersion);
                *isSupported = false;
            } else { // if (apiVersion == API_VERSION_1) {
                ALOGV("%s: Camera id %d uses older HAL before 3.2, but api1 is always supported",
                        __FUNCTION__, cameraId);
                *isSupported = true;
            }
            break;
        case CAMERA_DEVICE_API_VERSION_3_2:
        case CAMERA_DEVICE_API_VERSION_3_3:
        case CAMERA_DEVICE_API_VERSION_3_4:
            ALOGV("%s: Camera id %d uses HAL3.2 or newer, supports api1/api2 directly",
                    __FUNCTION__, cameraId);
            *isSupported = true;
            break;
        case -1: {
            String8 msg = String8::format("Unknown camera ID %d", cameraId);
            ALOGE("%s: %s", __FUNCTION__, msg.string());
            return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, msg.string());
        }
        default: {
            String8 msg = String8::format("Unknown device version %d for device %d",
                    deviceVersion, cameraId);
            ALOGE("%s: %s", __FUNCTION__, msg.string());
            return STATUS_ERROR(ERROR_INVALID_OPERATION, msg.string());
        }
    }

    return Status::ok();
}

void CameraService::removeByClient(const BasicClient* client) {
    Mutex::Autolock lock(mServiceLock);
    for (auto& i : mActiveClientManager.getAll()) {
        auto clientSp = i->getValue();
        if (clientSp.get() == client) {
            mActiveClientManager.remove(i);
        }
    }
}

bool CameraService::evictClientIdByRemote(const wp<IBinder>& remote) {
    const int callingPid = getCallingPid();
    const int servicePid = getpid();
    bool ret = false;
    {
        // Acquire mServiceLock and prevent other clients from connecting
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper);


        std::vector<sp<BasicClient>> evicted;
        for (auto& i : mActiveClientManager.getAll()) {
            auto clientSp = i->getValue();
            if (clientSp.get() == nullptr) {
                ALOGE("%s: Dead client still in mActiveClientManager.", __FUNCTION__);
                mActiveClientManager.remove(i);
                continue;
            }
            if (remote == clientSp->getRemote() && (callingPid == servicePid ||
                    callingPid == clientSp->getClientPid())) {
                mActiveClientManager.remove(i);
                evicted.push_back(clientSp);

                // Notify the client of disconnection
                clientSp->notifyError(
                        hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_DISCONNECTED,
                        CaptureResultExtras());
            }
        }

        // Do not hold mServiceLock while disconnecting clients, but retain the condition blocking
        // other clients from connecting in mServiceLockWrapper if held
        mServiceLock.unlock();

        // Do not clear caller identity, remote caller should be client proccess

        for (auto& i : evicted) {
            if (i.get() != nullptr) {
                i->disconnect();
                ret = true;
            }
        }
        //clear the evicted client list before acquring service lock again.
        evicted.clear();
        // Reacquire mServiceLock
        mServiceLock.lock();

    } // lock is destroyed, allow further connect calls

    return ret;
}


/**
 * Check camera capabilities, such as support for basic color operation
 * Also check that the device HAL version is still in support
 */
int CameraService::checkCameraCapabilities(int id, camera_info info, int *latestStrangeCameraId) {
    // device_version undefined in CAMERA_MODULE_API_VERSION_1_0,
    // All CAMERA_MODULE_API_VERSION_1_0 devices are backward-compatible
    if (mModule->getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_0) {
        // Verify the device version is in the supported range
        switch (info.device_version) {
            case CAMERA_DEVICE_API_VERSION_1_0:
            case CAMERA_DEVICE_API_VERSION_3_0:
            case CAMERA_DEVICE_API_VERSION_3_1:
            case CAMERA_DEVICE_API_VERSION_3_2:
            case CAMERA_DEVICE_API_VERSION_3_3:
            case CAMERA_DEVICE_API_VERSION_3_4:
                // in support
                break;
            case CAMERA_DEVICE_API_VERSION_2_0:
            case CAMERA_DEVICE_API_VERSION_2_1:
                // no longer supported
            default:
                ALOGE("%s: Device %d has HAL version %x, which is not supported",
                        __FUNCTION__, id, info.device_version);
                String8 msg = String8::format(
                        "Unsupported device HAL version %x for device %d",
                        info.device_version, id);
                logServiceError(msg.string(), NO_INIT);
                return NO_INIT;
        }
    }

    // Assume all devices pre-v3.3 are backward-compatible
    bool isBackwardCompatible = true;
    if (mModule->getModuleApiVersion() >= CAMERA_MODULE_API_VERSION_2_0
            && info.device_version >= CAMERA_DEVICE_API_VERSION_3_3) {
        isBackwardCompatible = false;
        status_t res;
        camera_metadata_ro_entry_t caps;
        res = find_camera_metadata_ro_entry(
            info.static_camera_characteristics,
            ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
            &caps);
        if (res != 0) {
            ALOGW("%s: Unable to find camera capabilities for camera device %d",
                    __FUNCTION__, id);
            caps.count = 0;
        }
        for (size_t i = 0; i < caps.count; i++) {
            if (caps.data.u8[i] ==
                    ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE) {
                isBackwardCompatible = true;
                break;
            }
        }
    }

    if (!isBackwardCompatible) {
        mNumberOfNormalCameras--;
        *latestStrangeCameraId = id;
    } else {
        if (id > *latestStrangeCameraId) {
            ALOGE("%s: Normal camera ID %d higher than strange camera ID %d. "
                    "This is not allowed due backward-compatibility requirements",
                    __FUNCTION__, id, *latestStrangeCameraId);
            logServiceError("Invalid order of camera devices", NO_INIT);
            mNumberOfCameras = 0;
            mNumberOfNormalCameras = 0;
            return NO_INIT;
        }
    }
    return OK;
}

std::shared_ptr<CameraService::CameraState> CameraService::getCameraState(
        const String8& cameraId) const {
    std::shared_ptr<CameraState> state;
    {
        Mutex::Autolock lock(mCameraStatesLock);
        auto iter = mCameraStates.find(cameraId);
        if (iter != mCameraStates.end()) {
            state = iter->second;
        }
    }
    return state;
}

sp<CameraService::BasicClient> CameraService::removeClientLocked(const String8& cameraId) {
    // Remove from active clients list
    auto clientDescriptorPtr = mActiveClientManager.remove(cameraId);
    if (clientDescriptorPtr == nullptr) {
        ALOGW("%s: Could not evict client, no client for camera ID %s", __FUNCTION__,
                cameraId.string());
        return sp<BasicClient>{nullptr};
    }

    return clientDescriptorPtr->getValue();
}

void CameraService::doUserSwitch(const std::vector<int32_t>& newUserIds) {
    // Acquire mServiceLock and prevent other clients from connecting
    std::unique_ptr<AutoConditionLock> lock =
            AutoConditionLock::waitAndAcquire(mServiceLockWrapper);

    std::set<userid_t> newAllowedUsers;
    for (size_t i = 0; i < newUserIds.size(); i++) {
        if (newUserIds[i] < 0) {
            ALOGE("%s: Bad user ID %d given during user switch, ignoring.",
                    __FUNCTION__, newUserIds[i]);
            return;
        }
        newAllowedUsers.insert(static_cast<userid_t>(newUserIds[i]));
    }


    if (newAllowedUsers == mAllowedUsers) {
        ALOGW("%s: Received notification of user switch with no updated user IDs.", __FUNCTION__);
        return;
    }

    logUserSwitch(mAllowedUsers, newAllowedUsers);

    mAllowedUsers = std::move(newAllowedUsers);

    // Current user has switched, evict all current clients.
    std::vector<sp<BasicClient>> evicted;
    for (auto& i : mActiveClientManager.getAll()) {
        auto clientSp = i->getValue();

        if (clientSp.get() == nullptr) {
            ALOGE("%s: Dead client still in mActiveClientManager.", __FUNCTION__);
            continue;
        }

        // Don't evict clients that are still allowed.
        uid_t clientUid = clientSp->getClientUid();
        userid_t clientUserId = multiuser_get_user_id(clientUid);
        if (mAllowedUsers.find(clientUserId) != mAllowedUsers.end()) {
            continue;
        }

        evicted.push_back(clientSp);

        String8 curTime = getFormattedCurrentTime();

        ALOGE("Evicting conflicting client for camera ID %s due to user change",
                i->getKey().string());

        // Log the clients evicted
        logEvent(String8::format("EVICT device %s client held by package %s (PID %"
                PRId32 ", priority %" PRId32 ")\n   - Evicted due to user switch.",
                i->getKey().string(), String8{clientSp->getPackageName()}.string(),
                i->getOwnerId(), i->getPriority()));

    }

    // Do not hold mServiceLock while disconnecting clients, but retain the condition
    // blocking other clients from connecting in mServiceLockWrapper if held.
    mServiceLock.unlock();

    // Clear caller identity temporarily so client disconnect PID checks work correctly
    int64_t token = IPCThreadState::self()->clearCallingIdentity();

    for (auto& i : evicted) {
        i->disconnect();
    }

    IPCThreadState::self()->restoreCallingIdentity(token);

    // Reacquire mServiceLock
    mServiceLock.lock();
}

void CameraService::logEvent(const char* event) {
    String8 curTime = getFormattedCurrentTime();
    Mutex::Autolock l(mLogLock);
    mEventLog.add(String8::format("%s : %s", curTime.string(), event));
}

void CameraService::logDisconnected(const char* cameraId, int clientPid,
        const char* clientPackage) {
    // Log the clients evicted
    logEvent(String8::format("DISCONNECT device %s client for package %s (PID %d)", cameraId,
            clientPackage, clientPid));
}

void CameraService::logConnected(const char* cameraId, int clientPid,
        const char* clientPackage) {
    // Log the clients evicted
    logEvent(String8::format("CONNECT device %s client for package %s (PID %d)", cameraId,
            clientPackage, clientPid));
}

void CameraService::logRejected(const char* cameraId, int clientPid,
        const char* clientPackage, const char* reason) {
    // Log the client rejected
    logEvent(String8::format("REJECT device %s client for package %s (PID %d), reason: (%s)",
            cameraId, clientPackage, clientPid, reason));
}

void CameraService::logUserSwitch(const std::set<userid_t>& oldUserIds,
        const std::set<userid_t>& newUserIds) {
    String8 newUsers = toString(newUserIds);
    String8 oldUsers = toString(oldUserIds);
    // Log the new and old users
    logEvent(String8::format("USER_SWITCH previous allowed users: %s , current allowed users: %s",
            oldUsers.string(), newUsers.string()));
}

void CameraService::logDeviceRemoved(const char* cameraId, const char* reason) {
    // Log the device removal
    logEvent(String8::format("REMOVE device %s, reason: (%s)", cameraId, reason));
}

void CameraService::logDeviceAdded(const char* cameraId, const char* reason) {
    // Log the device removal
    logEvent(String8::format("ADD device %s, reason: (%s)", cameraId, reason));
}

void CameraService::logClientDied(int clientPid, const char* reason) {
    // Log the device removal
    logEvent(String8::format("DIED client(s) with PID %d, reason: (%s)", clientPid, reason));
}

void CameraService::logServiceError(const char* msg, int errorCode) {
    String8 curTime = getFormattedCurrentTime();
    logEvent(String8::format("SERVICE ERROR: %s : %d (%s)", msg, errorCode, strerror(-errorCode)));
}

status_t CameraService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags) {

    const int pid = getCallingPid();
    const int selfPid = getpid();

    // Permission checks
    switch (code) {
        case BnCameraService::NOTIFYSYSTEMEVENT: {
            if (pid != selfPid) {
                // Ensure we're being called by system_server, or similar process with
                // permissions to notify the camera service about system events
                if (!checkCallingPermission(
                        String16("android.permission.CAMERA_SEND_SYSTEM_EVENTS"))) {
                    const int uid = getCallingUid();
                    ALOGE("Permission Denial: cannot send updates to camera service about system"
                            " events from pid=%d, uid=%d", pid, uid);
                    return PERMISSION_DENIED;
                }
            }
            break;
        }
    }

    return BnCameraService::onTransact(code, data, reply, flags);
}

// We share the media players for shutter and recording sound for all clients.
// A reference count is kept to determine when we will actually release the
// media players.

MediaPlayer* CameraService::newMediaPlayer(const char *file) {
    MediaPlayer* mp = new MediaPlayer();
    if (mp->setDataSource(NULL /* httpService */, file, NULL) == NO_ERROR) {
        mp->setAudioStreamType(AUDIO_STREAM_ENFORCED_AUDIBLE);
        mp->prepare();
    } else {
        ALOGE("Failed to load CameraService sounds: %s", file);
        return NULL;
    }
    return mp;
}

void CameraService::loadSound() {
    ATRACE_CALL();

    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::loadSound ref=%d", mSoundRef);
    if (mSoundRef++) return;

    mSoundPlayer[SOUND_SHUTTER] = newMediaPlayer("/system/media/audio/ui/camera_click.ogg");
    mSoundPlayer[SOUND_RECORDING_START] = newMediaPlayer("/system/media/audio/ui/VideoRecord.ogg");
    mSoundPlayer[SOUND_RECORDING_STOP] = newMediaPlayer("/system/media/audio/ui/VideoStop.ogg");
}

void CameraService::releaseSound() {
    Mutex::Autolock lock(mSoundLock);
    LOG1("CameraService::releaseSound ref=%d", mSoundRef);
    if (--mSoundRef) return;

    for (int i = 0; i < NUM_SOUNDS; i++) {
        if (mSoundPlayer[i] != 0) {
            mSoundPlayer[i]->disconnect();
            mSoundPlayer[i].clear();
        }
    }
}

void CameraService::playSound(sound_kind kind) {
    ATRACE_CALL();

    LOG1("playSound(%d)", kind);
    Mutex::Autolock lock(mSoundLock);
    sp<MediaPlayer> player = mSoundPlayer[kind];
    if (player != 0) {
        player->seekTo(0);
        player->start();
    }
}

// ----------------------------------------------------------------------------

CameraService::Client::Client(const sp<CameraService>& cameraService,
        const sp<ICameraClient>& cameraClient,
        const String16& clientPackageName,
        int cameraId, int cameraFacing,
        int clientPid, uid_t clientUid,
        int servicePid) :
        CameraService::BasicClient(cameraService,
                IInterface::asBinder(cameraClient),
                clientPackageName,
                cameraId, cameraFacing,
                clientPid, clientUid,
                servicePid)
{
    int callingPid = getCallingPid();
    LOG1("Client::Client E (pid %d, id %d)", callingPid, cameraId);

    mRemoteCallback = cameraClient;

    cameraService->loadSound();

    LOG1("Client::Client X (pid %d, id %d)", callingPid, cameraId);
}

// tear down the client
CameraService::Client::~Client() {
    ALOGV("~Client");
    mDestructionStarted = true;

    mCameraService->releaseSound();
    // unconditionally disconnect. function is idempotent
    Client::disconnect();
}

CameraService::BasicClient::BasicClient(const sp<CameraService>& cameraService,
        const sp<IBinder>& remoteCallback,
        const String16& clientPackageName,
        int cameraId, int cameraFacing,
        int clientPid, uid_t clientUid,
        int servicePid):
        mClientPackageName(clientPackageName), mDisconnected(false)
{
    mCameraService = cameraService;
    mRemoteBinder = remoteCallback;
    mCameraId = cameraId;
    mCameraFacing = cameraFacing;
    mClientPid = clientPid;
    mClientUid = clientUid;
    mServicePid = servicePid;
    mOpsActive = false;
    mDestructionStarted = false;

    // In some cases the calling code has no access to the package it runs under.
    // For example, NDK camera API.
    // In this case we will get the packages for the calling UID and pick the first one
    // for attributing the app op. This will work correctly for runtime permissions
    // as for legacy apps we will toggle the app op for all packages in the UID.
    // The caveat is that the operation may be attributed to the wrong package and
    // stats based on app ops may be slightly off.
    if (mClientPackageName.size() <= 0) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16(kPermissionServiceName));
        if (binder == 0) {
            ALOGE("Cannot get permission service");
            // Leave mClientPackageName unchanged (empty) and the further interaction
            // with camera will fail in BasicClient::startCameraOps
            return;
        }

        sp<IPermissionController> permCtrl = interface_cast<IPermissionController>(binder);
        Vector<String16> packages;

        permCtrl->getPackagesForUid(mClientUid, packages);

        if (packages.isEmpty()) {
            ALOGE("No packages for calling UID");
            // Leave mClientPackageName unchanged (empty) and the further interaction
            // with camera will fail in BasicClient::startCameraOps
            return;
        }
        mClientPackageName = packages[0];
    }
}

CameraService::BasicClient::~BasicClient() {
    ALOGV("~BasicClient");
    mDestructionStarted = true;
}

binder::Status CameraService::BasicClient::disconnect() {
    binder::Status res = Status::ok();
    if (mDisconnected) {
        return res;
    }
    mDisconnected = true;

    mCameraService->removeByClient(this);
    mCameraService->logDisconnected(String8::format("%d", mCameraId), mClientPid,
            String8(mClientPackageName));

    sp<IBinder> remote = getRemote();
    if (remote != nullptr) {
        remote->unlinkToDeath(mCameraService);
    }

    finishCameraOps();
    // Notify flashlight that a camera device is closed.
    mCameraService->mFlashlight->deviceClosed(String8::format("%d", mCameraId));
    ALOGI("%s: Disconnected client for camera %d for PID %d", __FUNCTION__, mCameraId, mClientPid);

    // client shouldn't be able to call into us anymore
    mClientPid = 0;

    return res;
}

status_t CameraService::BasicClient::dump(int, const Vector<String16>&) {
    // No dumping of clients directly over Binder,
    // must go through CameraService::dump
    android_errorWriteWithInfoLog(SN_EVENT_LOG_ID, "26265403",
            IPCThreadState::self()->getCallingUid(), NULL, 0);
    return OK;
}

String16 CameraService::BasicClient::getPackageName() const {
    return mClientPackageName;
}


int CameraService::BasicClient::getClientPid() const {
    return mClientPid;
}

uid_t CameraService::BasicClient::getClientUid() const {
    return mClientUid;
}

bool CameraService::BasicClient::canCastToApiClient(apiLevel level) const {
    // Defaults to API2.
    return level == API_2;
}

status_t CameraService::BasicClient::startCameraOps() {
    ATRACE_CALL();

    int32_t res;
    // Notify app ops that the camera is not available
    mOpsCallback = new OpsCallback(this);

    {
        ALOGV("%s: Start camera ops, package name = %s, client UID = %d",
              __FUNCTION__, String8(mClientPackageName).string(), mClientUid);
    }

    mAppOpsManager.startWatchingMode(AppOpsManager::OP_CAMERA,
            mClientPackageName, mOpsCallback);
    res = mAppOpsManager.startOp(AppOpsManager::OP_CAMERA,
            mClientUid, mClientPackageName);

    if (res == AppOpsManager::MODE_ERRORED) {
        ALOGI("Camera %d: Access for \"%s\" has been revoked",
                mCameraId, String8(mClientPackageName).string());
        return PERMISSION_DENIED;
    }

    if (res == AppOpsManager::MODE_IGNORED) {
        ALOGI("Camera %d: Access for \"%s\" has been restricted",
                mCameraId, String8(mClientPackageName).string());
        // Return the same error as for device policy manager rejection
        return -EACCES;
    }

    mOpsActive = true;

    // Transition device availability listeners from PRESENT -> NOT_AVAILABLE
    mCameraService->updateStatus(ICameraServiceListener::STATUS_NOT_AVAILABLE,
            String8::format("%d", mCameraId));

    // Transition device state to OPEN
    mCameraService->updateProxyDeviceState(ICameraServiceProxy::CAMERA_STATE_OPEN,
            String8::format("%d", mCameraId));

    return OK;
}

status_t CameraService::BasicClient::finishCameraOps() {
    ATRACE_CALL();

    // Check if startCameraOps succeeded, and if so, finish the camera op
    if (mOpsActive) {
        // Notify app ops that the camera is available again
        mAppOpsManager.finishOp(AppOpsManager::OP_CAMERA, mClientUid,
                mClientPackageName);
        mOpsActive = false;

        std::initializer_list<int32_t> rejected = {ICameraServiceListener::STATUS_NOT_PRESENT,
                ICameraServiceListener::STATUS_ENUMERATING};

        // Transition to PRESENT if the camera is not in either of the rejected states
        mCameraService->updateStatus(ICameraServiceListener::STATUS_PRESENT,
                String8::format("%d", mCameraId), rejected);

        // Transition device state to CLOSED
        mCameraService->updateProxyDeviceState(ICameraServiceProxy::CAMERA_STATE_CLOSED,
                String8::format("%d", mCameraId));
    }
    // Always stop watching, even if no camera op is active
    if (mOpsCallback != NULL) {
        mAppOpsManager.stopWatchingMode(mOpsCallback);
    }
    mOpsCallback.clear();

    return OK;
}

void CameraService::BasicClient::opChanged(int32_t op, const String16& packageName) {
    ATRACE_CALL();

    String8 name(packageName);
    String8 myName(mClientPackageName);

    if (op != AppOpsManager::OP_CAMERA) {
        ALOGW("Unexpected app ops notification received: %d", op);
        return;
    }

    int32_t res;
    res = mAppOpsManager.checkOp(AppOpsManager::OP_CAMERA,
            mClientUid, mClientPackageName);
    ALOGV("checkOp returns: %d, %s ", res,
            res == AppOpsManager::MODE_ALLOWED ? "ALLOWED" :
            res == AppOpsManager::MODE_IGNORED ? "IGNORED" :
            res == AppOpsManager::MODE_ERRORED ? "ERRORED" :
            "UNKNOWN");

    if (res != AppOpsManager::MODE_ALLOWED) {
        ALOGI("Camera %d: Access for \"%s\" revoked", mCameraId,
                myName.string());
        // Reset the client PID to allow server-initiated disconnect,
        // and to prevent further calls by client.
        mClientPid = getCallingPid();
        CaptureResultExtras resultExtras; // a dummy result (invalid)
        notifyError(hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_SERVICE, resultExtras);
        disconnect();
    }
}

// ----------------------------------------------------------------------------

// Provide client strong pointer for callbacks.
sp<CameraService::Client> CameraService::Client::getClientFromCookie(void* user) {
    String8 cameraId = String8::format("%d", (int)(intptr_t) user);
    auto clientDescriptor = gCameraService->mActiveClientManager.get(cameraId);
    if (clientDescriptor != nullptr) {
        return sp<Client>{
                static_cast<Client*>(clientDescriptor->getValue().get())};
    }
    return sp<Client>{nullptr};
}

void CameraService::Client::notifyError(int32_t errorCode,
        const CaptureResultExtras& resultExtras) {
    (void) errorCode;
    (void) resultExtras;
    if (mRemoteCallback != NULL) {
        mRemoteCallback->notifyCallback(CAMERA_MSG_ERROR, CAMERA_ERROR_RELEASED, 0);
    } else {
        ALOGE("mRemoteCallback is NULL!!");
    }
}

// NOTE: function is idempotent
binder::Status CameraService::Client::disconnect() {
    ALOGV("Client::disconnect");
    return BasicClient::disconnect();
}

bool CameraService::Client::canCastToApiClient(apiLevel level) const {
    return level == API_1;
}

CameraService::Client::OpsCallback::OpsCallback(wp<BasicClient> client):
        mClient(client) {
}

void CameraService::Client::OpsCallback::opChanged(int32_t op,
        const String16& packageName) {
    sp<BasicClient> client = mClient.promote();
    if (client != NULL) {
        client->opChanged(op, packageName);
    }
}

// ----------------------------------------------------------------------------
//                  CameraState
// ----------------------------------------------------------------------------

CameraService::CameraState::CameraState(const String8& id, int cost,
        const std::set<String8>& conflicting) : mId(id),
        mStatus(ICameraServiceListener::STATUS_PRESENT), mCost(cost), mConflicting(conflicting) {}

CameraService::CameraState::~CameraState() {}

int32_t CameraService::CameraState::getStatus() const {
    Mutex::Autolock lock(mStatusLock);
    return mStatus;
}

CameraParameters CameraService::CameraState::getShimParams() const {
    return mShimParams;
}

void CameraService::CameraState::setShimParams(const CameraParameters& params) {
    mShimParams = params;
}

int CameraService::CameraState::getCost() const {
    return mCost;
}

std::set<String8> CameraService::CameraState::getConflicting() const {
    return mConflicting;
}

String8 CameraService::CameraState::getId() const {
    return mId;
}

// ----------------------------------------------------------------------------
//                  ClientEventListener
// ----------------------------------------------------------------------------

void CameraService::ClientEventListener::onClientAdded(
        const resource_policy::ClientDescriptor<String8,
        sp<CameraService::BasicClient>>& descriptor) {
    auto basicClient = descriptor.getValue();
    if (basicClient.get() != nullptr) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());
        notifier.noteStartCamera(descriptor.getKey(),
                static_cast<int>(basicClient->getClientUid()));
    }
}

void CameraService::ClientEventListener::onClientRemoved(
        const resource_policy::ClientDescriptor<String8,
        sp<CameraService::BasicClient>>& descriptor) {
    auto basicClient = descriptor.getValue();
    if (basicClient.get() != nullptr) {
        BatteryNotifier& notifier(BatteryNotifier::getInstance());
        notifier.noteStopCamera(descriptor.getKey(),
                static_cast<int>(basicClient->getClientUid()));
    }
}


// ----------------------------------------------------------------------------
//                  CameraClientManager
// ----------------------------------------------------------------------------

CameraService::CameraClientManager::CameraClientManager() {
    setListener(std::make_shared<ClientEventListener>());
}

CameraService::CameraClientManager::~CameraClientManager() {}

sp<CameraService::BasicClient> CameraService::CameraClientManager::getCameraClient(
        const String8& id) const {
    auto descriptor = get(id);
    if (descriptor == nullptr) {
        return sp<BasicClient>{nullptr};
    }
    return descriptor->getValue();
}

String8 CameraService::CameraClientManager::toString() const {
    auto all = getAll();
    String8 ret("[");
    bool hasAny = false;
    for (auto& i : all) {
        hasAny = true;
        String8 key = i->getKey();
        int32_t cost = i->getCost();
        int32_t pid = i->getOwnerId();
        int32_t priority = i->getPriority();
        auto conflicting = i->getConflicting();
        auto clientSp = i->getValue();
        String8 packageName;
        userid_t clientUserId = 0;
        if (clientSp.get() != nullptr) {
            packageName = String8{clientSp->getPackageName()};
            uid_t clientUid = clientSp->getClientUid();
            clientUserId = multiuser_get_user_id(clientUid);
        }
        ret.appendFormat("\n(Camera ID: %s, Cost: %" PRId32 ", PID: %" PRId32 ", Priority: %"
                PRId32 ", ", key.string(), cost, pid, priority);

        if (clientSp.get() != nullptr) {
            ret.appendFormat("User Id: %d, ", clientUserId);
        }
        if (packageName.size() != 0) {
            ret.appendFormat("Client Package Name: %s", packageName.string());
        }

        ret.append(", Conflicting Client Devices: {");
        for (auto& j : conflicting) {
            ret.appendFormat("%s, ", j.string());
        }
        ret.append("})");
    }
    if (hasAny) ret.append("\n");
    ret.append("]\n");
    return ret;
}

CameraService::DescriptorPtr CameraService::CameraClientManager::makeClientDescriptor(
        const String8& key, const sp<BasicClient>& value, int32_t cost,
        const std::set<String8>& conflictingKeys, int32_t priority, int32_t ownerId) {

    return std::make_shared<resource_policy::ClientDescriptor<String8, sp<BasicClient>>>(
            key, value, cost, conflictingKeys, priority, ownerId);
}

CameraService::DescriptorPtr CameraService::CameraClientManager::makeClientDescriptor(
        const sp<BasicClient>& value, const CameraService::DescriptorPtr& partial) {
    return makeClientDescriptor(partial->getKey(), value, partial->getCost(),
            partial->getConflicting(), partial->getPriority(), partial->getOwnerId());
}

// ----------------------------------------------------------------------------

static const int kDumpLockRetries = 50;
static const int kDumpLockSleep = 60000;

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleep);
    }
    return locked;
}

status_t CameraService::dump(int fd, const Vector<String16>& args) {
    ATRACE_CALL();

    String8 result("Dump of the Camera Service:\n");
    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        result = result.format("Permission Denial: "
                "can't dump CameraService from pid=%d, uid=%d\n",
                getCallingPid(),
                getCallingUid());
        write(fd, result.string(), result.size());
    } else {
        bool locked = tryLock(mServiceLock);
        // failed to lock - CameraService is probably deadlocked
        if (!locked) {
            result.append("CameraService may be deadlocked\n");
            write(fd, result.string(), result.size());
        }

        bool hasClient = false;
        if (!mModule) {
            result = String8::format("No camera module available!\n");
            write(fd, result.string(), result.size());

            // Dump event log for error information
            dumpEventLog(fd);

            if (locked) mServiceLock.unlock();
            return NO_ERROR;
        }

        result = String8::format("Camera module HAL API version: 0x%x\n", mModule->getHalApiVersion());
        result.appendFormat("Camera module API version: 0x%x\n", mModule->getModuleApiVersion());
        result.appendFormat("Camera module name: %s\n", mModule->getModuleName());
        result.appendFormat("Camera module author: %s\n", mModule->getModuleAuthor());
        result.appendFormat("Number of camera devices: %d\n", mNumberOfCameras);
        result.appendFormat("Number of normal camera devices: %d\n", mNumberOfNormalCameras);
        String8 activeClientString = mActiveClientManager.toString();
        result.appendFormat("Active Camera Clients:\n%s", activeClientString.string());
        result.appendFormat("Allowed users:\n%s\n", toString(mAllowedUsers).string());

        sp<VendorTagDescriptor> desc = VendorTagDescriptor::getGlobalVendorTagDescriptor();
        if (desc == NULL) {
            result.appendFormat("Vendor tags left unimplemented.\n");
        } else {
            result.appendFormat("Vendor tag definitions:\n");
        }

        write(fd, result.string(), result.size());

        if (desc != NULL) {
            desc->dump(fd, /*verbosity*/2, /*indentation*/4);
        }

        dumpEventLog(fd);

        bool stateLocked = tryLock(mCameraStatesLock);
        if (!stateLocked) {
            result = String8::format("CameraStates in use, may be deadlocked\n");
            write(fd, result.string(), result.size());
        }

        for (auto& state : mCameraStates) {
            String8 cameraId = state.first;
            result = String8::format("Camera %s information:\n", cameraId.string());
            camera_info info;

            // TODO: Change getCameraInfo + HAL to use String cameraIds
            status_t rc = mModule->getCameraInfo(cameraIdToInt(cameraId), &info);
            if (rc != OK) {
                result.appendFormat("  Error reading static information!\n");
                write(fd, result.string(), result.size());
            } else {
                result.appendFormat("  Facing: %s\n",
                        info.facing == CAMERA_FACING_BACK ? "BACK" : "FRONT");
                result.appendFormat("  Orientation: %d\n", info.orientation);
                int deviceVersion;
                if (mModule->getModuleApiVersion() < CAMERA_MODULE_API_VERSION_2_0) {
                    deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;
                } else {
                    deviceVersion = info.device_version;
                }

                auto conflicting = state.second->getConflicting();
                result.appendFormat("  Resource Cost: %d\n", state.second->getCost());
                result.appendFormat("  Conflicting Devices:");
                for (auto& id : conflicting) {
                    result.appendFormat(" %s", id.string());
                }
                if (conflicting.size() == 0) {
                    result.appendFormat(" NONE");
                }
                result.appendFormat("\n");

                result.appendFormat("  Device version: %#x\n", deviceVersion);
                if (deviceVersion >= CAMERA_DEVICE_API_VERSION_3_0) {
                    result.appendFormat("  Device static metadata:\n");
                    write(fd, result.string(), result.size());
                    dump_indented_camera_metadata(info.static_camera_characteristics,
                            fd, /*verbosity*/2, /*indentation*/4);
                } else {
                    write(fd, result.string(), result.size());
                }

                CameraParameters p = state.second->getShimParams();
                if (!p.isEmpty()) {
                    result = String8::format("  Camera1 API shim is using parameters:\n        ");
                    write(fd, result.string(), result.size());
                    p.dump(fd, args);
                }
            }

            auto clientDescriptor = mActiveClientManager.get(cameraId);
            if (clientDescriptor == nullptr) {
                result = String8::format("  Device %s is closed, no client instance\n",
                        cameraId.string());
                write(fd, result.string(), result.size());
                continue;
            }
            hasClient = true;
            result = String8::format("  Device %s is open. Client instance dump:\n\n",
                    cameraId.string());
            result.appendFormat("Client priority level: %d\n", clientDescriptor->getPriority());
            result.appendFormat("Client PID: %d\n", clientDescriptor->getOwnerId());

            auto client = clientDescriptor->getValue();
            result.appendFormat("Client package: %s\n",
                    String8(client->getPackageName()).string());
            write(fd, result.string(), result.size());

            client->dumpClient(fd, args);
        }

        if (stateLocked) mCameraStatesLock.unlock();

        if (!hasClient) {
            result = String8::format("\nNo active camera clients yet.\n");
            write(fd, result.string(), result.size());
        }

        if (locked) mServiceLock.unlock();

        // Dump camera traces if there were any
        write(fd, "\n", 1);
        camera3::CameraTraces::dump(fd, args);

        // Process dump arguments, if any
        int n = args.size();
        String16 verboseOption("-v");
        String16 unreachableOption("--unreachable");
        for (int i = 0; i < n; i++) {
            if (args[i] == verboseOption) {
                // change logging level
                if (i + 1 >= n) continue;
                String8 levelStr(args[i+1]);
                int level = atoi(levelStr.string());
                result = String8::format("\nSetting log level to %d.\n", level);
                setLogLevel(level);
                write(fd, result.string(), result.size());
            } else if (args[i] == unreachableOption) {
                // Dump memory analysis
                // TODO - should limit be an argument parameter?
                UnreachableMemoryInfo info;
                bool success = GetUnreachableMemory(info, /*limit*/ 10000);
                if (!success) {
                    dprintf(fd, "\nUnable to dump unreachable memory. "
                            "Try disabling SELinux enforcement.\n");
                } else {
                    dprintf(fd, "\nDumping unreachable memory:\n");
                    std::string s = info.ToString(/*log_contents*/ true);
                    write(fd, s.c_str(), s.size());
                }
            }
        }
    }
    return NO_ERROR;
}

void CameraService::dumpEventLog(int fd) {
    String8 result = String8("\nPrior client events (most recent at top):\n");

    Mutex::Autolock l(mLogLock);
    for (const auto& msg : mEventLog) {
        result.appendFormat("  %s\n", msg.string());
    }

    if (mEventLog.size() == DEFAULT_EVENT_LOG_LENGTH) {
        result.append("  ...\n");
    } else if (mEventLog.size() == 0) {
        result.append("  [no events yet]\n");
    }
    result.append("\n");

    write(fd, result.string(), result.size());
}

void CameraService::handleTorchClientBinderDied(const wp<IBinder> &who) {
    Mutex::Autolock al(mTorchClientMapMutex);
    for (size_t i = 0; i < mTorchClientMap.size(); i++) {
        if (mTorchClientMap[i] == who) {
            // turn off the torch mode that was turned on by dead client
            String8 cameraId = mTorchClientMap.keyAt(i);
            status_t res = mFlashlight->setTorchMode(cameraId, false);
            if (res) {
                ALOGE("%s: torch client died but couldn't turn off torch: "
                    "%s (%d)", __FUNCTION__, strerror(-res), res);
                return;
            }
            mTorchClientMap.removeItemsAt(i);
            break;
        }
    }
}

/*virtual*/void CameraService::binderDied(const wp<IBinder> &who) {

    /**
      * While tempting to promote the wp<IBinder> into a sp, it's actually not supported by the
      * binder driver
      */

    logClientDied(getCallingPid(), String8("Binder died unexpectedly"));

    // check torch client
    handleTorchClientBinderDied(who);

    // check camera device client
    if(!evictClientIdByRemote(who)) {
        ALOGV("%s: Java client's binder death already cleaned up (normal case)", __FUNCTION__);
        return;
    }

    ALOGE("%s: Java client's binder died, removing it from the list of active clients",
            __FUNCTION__);
}

void CameraService::updateStatus(int32_t status, const String8& cameraId) {
    updateStatus(status, cameraId, {});
}

void CameraService::updateStatus(int32_t status, const String8& cameraId,
        std::initializer_list<int32_t> rejectSourceStates) {
    // Do not lock mServiceLock here or can get into a deadlock from
    // connect() -> disconnect -> updateStatus

    auto state = getCameraState(cameraId);

    if (state == nullptr) {
        ALOGW("%s: Could not update the status for %s, no such device exists", __FUNCTION__,
                cameraId.string());
        return;
    }

    // Update the status for this camera state, then send the onStatusChangedCallbacks to each
    // of the listeners with both the mStatusStatus and mStatusListenerLock held
    state->updateStatus(status, cameraId, rejectSourceStates, [this]
            (const String8& cameraId, int32_t status) {

            if (status != ICameraServiceListener::STATUS_ENUMERATING) {
                // Update torch status if it has a flash unit.
                Mutex::Autolock al(mTorchStatusMutex);
                int32_t torchStatus;
                if (getTorchStatusLocked(cameraId, &torchStatus) !=
                        NAME_NOT_FOUND) {
                    int32_t newTorchStatus =
                            status == ICameraServiceListener::STATUS_PRESENT ?
                            ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF :
                            ICameraServiceListener::TORCH_STATUS_NOT_AVAILABLE;
                    if (torchStatus != newTorchStatus) {
                        onTorchStatusChangedLocked(cameraId, newTorchStatus);
                    }
                }
            }

            Mutex::Autolock lock(mStatusListenerLock);

            for (auto& listener : mListenerList) {
                // TODO: Refactor status listeners to use strings for Camera IDs and remove this.
                int id = cameraIdToInt(cameraId);
                if (id != -1) listener->onStatusChanged(status, id);
            }
        });
}

void CameraService::updateProxyDeviceState(ICameraServiceProxy::CameraState newState,
        const String8& cameraId) {
    sp<ICameraServiceProxy> proxyBinder = getCameraServiceProxy();
    if (proxyBinder == nullptr) return;
    String16 id(cameraId);
    proxyBinder->notifyCameraState(id, newState);
}

status_t CameraService::getTorchStatusLocked(
        const String8& cameraId,
        int32_t *status) const {
    if (!status) {
        return BAD_VALUE;
    }
    ssize_t index = mTorchStatusMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        // invalid camera ID or the camera doesn't have a flash unit
        return NAME_NOT_FOUND;
    }

    *status = mTorchStatusMap.valueAt(index);
    return OK;
}

status_t CameraService::setTorchStatusLocked(const String8& cameraId,
        int32_t status) {
    ssize_t index = mTorchStatusMap.indexOfKey(cameraId);
    if (index == NAME_NOT_FOUND) {
        return BAD_VALUE;
    }
    int32_t& item =
            mTorchStatusMap.editValueAt(index);
    item = status;

    return OK;
}

}; // namespace android
