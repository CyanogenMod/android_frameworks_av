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
#define LOG_TAG "ACameraManager"

#include <memory>
#include "ACameraManager.h"
#include "ACameraMetadata.h"
#include "ACameraDevice.h"
#include <utils/Vector.h>
#include <stdlib.h>
#include <camera/VendorTagDescriptor.h>

using namespace android;

//constants shared between ACameraManager and CameraManagerGlobal
namespace {
    const int kMaxCameraIdLen = 32;
}

namespace android {
// Static member definitions
const char* CameraManagerGlobal::kCameraIdKey   = "CameraId";
const char* CameraManagerGlobal::kCallbackFpKey = "CallbackFp";
const char* CameraManagerGlobal::kContextKey    = "CallbackContext";
Mutex                CameraManagerGlobal::sLock;
CameraManagerGlobal* CameraManagerGlobal::sInstance = nullptr;

CameraManagerGlobal&
CameraManagerGlobal::getInstance() {
    Mutex::Autolock _l(sLock);
    CameraManagerGlobal* instance = sInstance;
    if (instance == nullptr) {
        instance = new CameraManagerGlobal();
        sInstance = instance;
    }
    return *instance;
}

CameraManagerGlobal::~CameraManagerGlobal() {
    // clear sInstance so next getInstance call knows to create a new one
    Mutex::Autolock _sl(sLock);
    sInstance = nullptr;
    Mutex::Autolock _l(mLock);
    if (mCameraService != nullptr) {
        IInterface::asBinder(mCameraService)->unlinkToDeath(mDeathNotifier);
        mCameraService->removeListener(mCameraServiceListener);
    }
    mDeathNotifier.clear();
    if (mCbLooper != nullptr) {
        mCbLooper->unregisterHandler(mHandler->id());
        mCbLooper->stop();
    }
    mCbLooper.clear();
    mHandler.clear();
    mCameraServiceListener.clear();
    mCameraService.clear();
}

sp<hardware::ICameraService> CameraManagerGlobal::getCameraService() {
    Mutex::Autolock _l(mLock);
    if (mCameraService.get() == nullptr) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder;
        do {
            binder = sm->getService(String16(kCameraServiceName));
            if (binder != nullptr) {
                break;
            }
            ALOGW("CameraService not published, waiting...");
            usleep(kCameraServicePollDelay);
        } while(true);
        if (mDeathNotifier == nullptr) {
            mDeathNotifier = new DeathNotifier(this);
        }
        binder->linkToDeath(mDeathNotifier);
        mCameraService = interface_cast<hardware::ICameraService>(binder);

        // Setup looper thread to perfrom availiability callbacks
        if (mCbLooper == nullptr) {
            mCbLooper = new ALooper;
            mCbLooper->setName("C2N-mgr-looper");
            status_t ret = mCbLooper->start(
                    /*runOnCallingThread*/false,
                    /*canCallJava*/       true,
                    PRIORITY_DEFAULT);
            if (mHandler == nullptr) {
                mHandler = new CallbackHandler();
            }
            mCbLooper->registerHandler(mHandler);
        }

        // register ICameraServiceListener
        if (mCameraServiceListener == nullptr) {
            mCameraServiceListener = new CameraServiceListener(this);
        }
        mCameraService->addListener(mCameraServiceListener);

        // setup vendor tags
        sp<VendorTagDescriptor> desc = new VendorTagDescriptor();
        binder::Status ret = mCameraService->getCameraVendorTagDescriptor(/*out*/desc.get());

        if (ret.isOk()) {
            status_t err = VendorTagDescriptor::setAsGlobalVendorTagDescriptor(desc);
            if (err != OK) {
                ALOGE("%s: Failed to set vendor tag descriptors, received error %s (%d)",
                        __FUNCTION__, strerror(-err), err);
            }
        } else if (ret.serviceSpecificErrorCode() ==
                hardware::ICameraService::ERROR_DEPRECATED_HAL) {
            ALOGW("%s: Camera HAL too old; does not support vendor tags",
                    __FUNCTION__);
            VendorTagDescriptor::clearGlobalVendorTagDescriptor();
        } else {
            ALOGE("%s: Failed to get vendor tag descriptors: %s",
                    __FUNCTION__, ret.toString8().string());
        }
    }
    ALOGE_IF(mCameraService == nullptr, "no CameraService!?");
    return mCameraService;
}

void CameraManagerGlobal::DeathNotifier::binderDied(const wp<IBinder>&)
{
    ALOGE("Camera service binderDied!");
    sp<CameraManagerGlobal> cm = mCameraManager.promote();
    if (cm != nullptr) {
        AutoMutex lock(cm->mLock);
        for (auto pair : cm->mDeviceStatusMap) {
            int32_t cameraId = pair.first;
            cm->onStatusChangedLocked(
                    CameraServiceListener::STATUS_NOT_PRESENT, cameraId);
        }
        cm->mCameraService.clear();
        // TODO: consider adding re-connect call here?
    }
}

void CameraManagerGlobal::registerAvailabilityCallback(
        const ACameraManager_AvailabilityCallbacks *callback) {
    Mutex::Autolock _l(mLock);
    Callback cb(callback);
    auto pair = mCallbacks.insert(cb);
    // Send initial callbacks if callback is newly registered
    if (pair.second) {
        for (auto pair : mDeviceStatusMap) {
            int32_t cameraId = pair.first;
            int32_t status = pair.second;

            sp<AMessage> msg = new AMessage(kWhatSendSingleCallback, mHandler);
            ACameraManager_AvailabilityCallback cb = isStatusAvailable(status) ?
                    callback->onCameraAvailable : callback->onCameraUnavailable;
            msg->setPointer(kCallbackFpKey, (void *) cb);
            msg->setPointer(kContextKey, callback->context);
            msg->setInt32(kCameraIdKey, cameraId);
            msg->post();
        }
    }
}

void CameraManagerGlobal::unregisterAvailabilityCallback(
        const ACameraManager_AvailabilityCallbacks *callback) {
    Mutex::Autolock _l(mLock);
    Callback cb(callback);
    mCallbacks.erase(cb);
}

bool CameraManagerGlobal::validStatus(int32_t status) {
    switch (status) {
        case hardware::ICameraServiceListener::STATUS_NOT_PRESENT:
        case hardware::ICameraServiceListener::STATUS_PRESENT:
        case hardware::ICameraServiceListener::STATUS_ENUMERATING:
        case hardware::ICameraServiceListener::STATUS_NOT_AVAILABLE:
            return true;
        default:
            return false;
    }
}

bool CameraManagerGlobal::isStatusAvailable(int32_t status) {
    switch (status) {
        case hardware::ICameraServiceListener::STATUS_PRESENT:
            return true;
        default:
            return false;
    }
}

void CameraManagerGlobal::CallbackHandler::sendSingleCallback(
        int32_t cameraId, void* context,
        ACameraManager_AvailabilityCallback cb) const {
    char cameraIdStr[kMaxCameraIdLen];
    snprintf(cameraIdStr, sizeof(cameraIdStr), "%d", cameraId);
    (*cb)(context, cameraIdStr);
}

void CameraManagerGlobal::CallbackHandler::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSendSingleCallback:
        {
            ACameraManager_AvailabilityCallback cb;
            void* context;
            int32_t cameraId;
            bool found = msg->findPointer(kCallbackFpKey, (void**) &cb);
            if (!found) {
                ALOGE("%s: Cannot find camera callback fp!", __FUNCTION__);
                return;
            }
            found = msg->findPointer(kContextKey, &context);
            if (!found) {
                ALOGE("%s: Cannot find callback context!", __FUNCTION__);
                return;
            }
            found = msg->findInt32(kCameraIdKey, &cameraId);
            if (!found) {
                ALOGE("%s: Cannot find camera ID!", __FUNCTION__);
                return;
            }
            sendSingleCallback(cameraId, context, cb);
            break;
        }
        default:
            ALOGE("%s: unknown message type %d", __FUNCTION__, msg->what());
            break;
    }
}

binder::Status CameraManagerGlobal::CameraServiceListener::onStatusChanged(
        int32_t status, int32_t cameraId) {
    sp<CameraManagerGlobal> cm = mCameraManager.promote();
    if (cm != nullptr) {
        cm->onStatusChanged(status, cameraId);
    } else {
        ALOGE("Cannot deliver status change. Global camera manager died");
    }
    return binder::Status::ok();
}

void CameraManagerGlobal::onStatusChanged(
        int32_t status, int32_t cameraId) {
    Mutex::Autolock _l(mLock);
    onStatusChangedLocked(status, cameraId);
}

void CameraManagerGlobal::onStatusChangedLocked(
        int32_t status, int32_t cameraId) {
        if (!validStatus(status)) {
            ALOGE("%s: Invalid status %d", __FUNCTION__, status);
            return;
        }

        bool firstStatus = (mDeviceStatusMap.count(cameraId) == 0);
        int32_t oldStatus = firstStatus ?
                status : // first status
                mDeviceStatusMap[cameraId];

        if (!firstStatus &&
                isStatusAvailable(status) == isStatusAvailable(oldStatus)) {
            // No status update. No need to send callback
            return;
        }

        // Iterate through all registered callbacks
        mDeviceStatusMap[cameraId] = status;
        for (auto cb : mCallbacks) {
            sp<AMessage> msg = new AMessage(kWhatSendSingleCallback, mHandler);
            ACameraManager_AvailabilityCallback cbFp = isStatusAvailable(status) ?
                    cb.mAvailable : cb.mUnavailable;
            msg->setPointer(kCallbackFpKey, (void *) cbFp);
            msg->setPointer(kContextKey, cb.mContext);
            msg->setInt32(kCameraIdKey, cameraId);
            msg->post();
        }
}

} // namespace android

/**
 * ACameraManger Implementation
 */
camera_status_t
ACameraManager::getOrCreateCameraIdListLocked(ACameraIdList** cameraIdList) {
    if (mCachedCameraIdList.numCameras == kCameraIdListNotInit) {
        int numCameras = 0;
        Vector<char *> cameraIds;
        sp<hardware::ICameraService> cs = CameraManagerGlobal::getInstance().getCameraService();
        if (cs == nullptr) {
            ALOGE("%s: Cannot reach camera service!", __FUNCTION__);
            return ACAMERA_ERROR_CAMERA_DISCONNECTED;
        }
        // Get number of cameras
        int numAllCameras = 0;
        binder::Status serviceRet = cs->getNumberOfCameras(hardware::ICameraService::CAMERA_TYPE_ALL,
                &numAllCameras);
        if (!serviceRet.isOk()) {
            ALOGE("%s: Error getting camera count: %s", __FUNCTION__,
                    serviceRet.toString8().string());
            numAllCameras = 0;
        }
        // Filter API2 compatible cameras and push to cameraIds
        for (int i = 0; i < numAllCameras; i++) {
            // TODO: Only suppot HALs that supports API2 directly now
            bool camera2Support = false;
            serviceRet = cs->supportsCameraApi(i, hardware::ICameraService::API_VERSION_2,
                    &camera2Support);
            char buf[kMaxCameraIdLen];
            if (camera2Support) {
                numCameras++;
                mCameraIds.insert(i);
                snprintf(buf, sizeof(buf), "%d", i);
                size_t cameraIdSize = strlen(buf) + 1;
                char *cameraId = new char[cameraIdSize];
                if (!cameraId) {
                    ALOGE("Allocate memory for ACameraIdList failed!");
                    return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
                }
                strlcpy(cameraId, buf, cameraIdSize);
                cameraIds.push(cameraId);
            }
        }
        mCachedCameraIdList.numCameras = numCameras;
        mCachedCameraIdList.cameraIds = new const char*[numCameras];
        if (!mCachedCameraIdList.cameraIds) {
            ALOGE("Allocate memory for ACameraIdList failed!");
            return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
        }
        for (int i = 0; i < numCameras; i++) {
            mCachedCameraIdList.cameraIds[i] = cameraIds[i];
        }
    }
    *cameraIdList = &mCachedCameraIdList;
    return ACAMERA_OK;
}

camera_status_t
ACameraManager::getCameraIdList(ACameraIdList** cameraIdList) {
    Mutex::Autolock _l(mLock);
    ACameraIdList* cachedList;
    camera_status_t ret = getOrCreateCameraIdListLocked(&cachedList);
    if (ret != ACAMERA_OK) {
        ALOGE("Get camera ID list failed! err: %d", ret);
        return ret;
    }

    int numCameras = cachedList->numCameras;
    ACameraIdList *out = new ACameraIdList;
    if (!out) {
        ALOGE("Allocate memory for ACameraIdList failed!");
        return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
    }
    out->numCameras = numCameras;
    out->cameraIds = new const char*[numCameras];
    if (!out->cameraIds) {
        ALOGE("Allocate memory for ACameraIdList failed!");
        return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
    }
    for (int i = 0; i < numCameras; i++) {
        const char* src = cachedList->cameraIds[i];
        size_t dstSize = strlen(src) + 1;
        char* dst = new char[dstSize];
        if (!dst) {
            ALOGE("Allocate memory for ACameraIdList failed!");
            return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
        }
        strlcpy(dst, src, dstSize);
        out->cameraIds[i] = dst;
    }
    *cameraIdList = out;
    return ACAMERA_OK;
}

void
ACameraManager::deleteCameraIdList(ACameraIdList* cameraIdList) {
    if (cameraIdList != nullptr) {
        if (cameraIdList->cameraIds != nullptr) {
            for (int i = 0; i < cameraIdList->numCameras; i ++) {
                delete[] cameraIdList->cameraIds[i];
            }
            delete[] cameraIdList->cameraIds;
        }
        delete cameraIdList;
    }
}

camera_status_t ACameraManager::getCameraCharacteristics(
        const char *cameraIdStr, ACameraMetadata **characteristics) {
    Mutex::Autolock _l(mLock);
    ACameraIdList* cachedList;
    // Make sure mCameraIds is initialized
    camera_status_t ret = getOrCreateCameraIdListLocked(&cachedList);
    if (ret != ACAMERA_OK) {
        ALOGE("%s: Get camera ID list failed! err: %d", __FUNCTION__, ret);
        return ret;
    }
    int cameraId = atoi(cameraIdStr);
    if (mCameraIds.count(cameraId) == 0) {
        ALOGE("%s: Camera ID %s does not exist!", __FUNCTION__, cameraIdStr);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    sp<hardware::ICameraService> cs = CameraManagerGlobal::getInstance().getCameraService();
    if (cs == nullptr) {
        ALOGE("%s: Cannot reach camera service!", __FUNCTION__);
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    CameraMetadata rawMetadata;
    binder::Status serviceRet = cs->getCameraCharacteristics(cameraId, &rawMetadata);
    if (!serviceRet.isOk()) {
        ALOGE("Get camera characteristics from camera service failed: %s",
                serviceRet.toString8().string());
        return ACAMERA_ERROR_UNKNOWN; // should not reach here
    }

    *characteristics = new ACameraMetadata(
            rawMetadata.release(), ACameraMetadata::ACM_CHARACTERISTICS);
    return ACAMERA_OK;
}

camera_status_t
ACameraManager::openCamera(
        const char* cameraId,
        ACameraDevice_StateCallbacks* callback,
        /*out*/ACameraDevice** outDevice) {
    ACameraMetadata* rawChars;
    camera_status_t ret = getCameraCharacteristics(cameraId, &rawChars);
    Mutex::Autolock _l(mLock);
    if (ret != ACAMERA_OK) {
        ALOGE("%s: cannot get camera characteristics for camera %s. err %d",
                __FUNCTION__, cameraId, ret);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    std::unique_ptr<ACameraMetadata> chars(rawChars);
    rawChars = nullptr;

    ACameraDevice* device = new ACameraDevice(cameraId, callback, std::move(chars));

    sp<hardware::ICameraService> cs = CameraManagerGlobal::getInstance().getCameraService();
    if (cs == nullptr) {
        ALOGE("%s: Cannot reach camera service!", __FUNCTION__);
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }

    int id = atoi(cameraId);
    sp<hardware::camera2::ICameraDeviceCallbacks> callbacks = device->getServiceCallback();
    sp<hardware::camera2::ICameraDeviceUser> deviceRemote;
    // No way to get package name from native.
    // Send a zero length package name and let camera service figure it out from UID
    binder::Status serviceRet = cs->connectDevice(
            callbacks, id, String16(""),
            hardware::ICameraService::USE_CALLING_UID, /*out*/&deviceRemote);

    if (!serviceRet.isOk()) {
        ALOGE("%s: connect camera device failed: %s", __FUNCTION__, serviceRet.toString8().string());
        delete device;
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    if (deviceRemote == nullptr) {
        ALOGE("%s: connect camera device failed! remote device is null", __FUNCTION__);
        delete device;
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    device->setRemoteDevice(deviceRemote);
    *outDevice = device;
    return ACAMERA_OK;
}

ACameraManager::~ACameraManager() {
    Mutex::Autolock _l(mLock);
    if (mCachedCameraIdList.numCameras != kCameraIdListNotInit) {
        for (int i = 0; i < mCachedCameraIdList.numCameras; i++) {
            delete[] mCachedCameraIdList.cameraIds[i];
        }
        delete[] mCachedCameraIdList.cameraIds;
    }
}
