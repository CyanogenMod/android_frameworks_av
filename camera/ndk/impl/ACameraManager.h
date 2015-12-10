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

#ifndef _ACAMERA_MANAGER_H
#define _ACAMERA_MANAGER_H

#include "NdkCameraManager.h"

#include <camera/CameraMetadata.h>
#include <camera/ICameraService.h>
#include <camera/ICameraServiceListener.h>
#include <binder/IServiceManager.h>
#include <utils/StrongPointer.h>
#include <utils/Mutex.h>

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/AMessage.h>

#include <set>
#include <map>

using namespace android;

namespace android {

/**
 * Per-process singleton instance of CameraManger. Shared by all ACameraManager
 * instances. Created when first ACameraManager is created and destroyed when
 * all ACameraManager instances are deleted.
 *
 * TODO: maybe CameraManagerGlobal is better sutied in libcameraclient?
 */
class CameraManagerGlobal final : public RefBase {
  public:
    static CameraManagerGlobal& getInstance();
    sp<ICameraService> getCameraService();

    void registerAvailabilityCallback(
            const ACameraManager_AvailabilityCallbacks *callback);
    void unregisterAvailabilityCallback(
            const ACameraManager_AvailabilityCallbacks *callback);

  private:
    sp<ICameraService> mCameraService;
    const int          kCameraServicePollDelay = 500000; // 0.5s
    const char*        kCameraServiceName      = "media.camera";
    Mutex              mLock;

    class DeathNotifier : public IBinder::DeathRecipient {
      public:
        DeathNotifier(CameraManagerGlobal* cm) : mCameraManager(cm) {}
      protected:
        // IBinder::DeathRecipient implementation
        virtual void binderDied(const wp<IBinder>& who);
      private:
        const wp<CameraManagerGlobal> mCameraManager;
    };
    sp<DeathNotifier> mDeathNotifier;

    class CameraServiceListener final : public BnCameraServiceListener {
      public:
        CameraServiceListener(CameraManagerGlobal* cm) : mCameraManager(cm) {}
        virtual void onStatusChanged(Status status, int32_t cameraId);

        // Torch API not implemented yet
        virtual void onTorchStatusChanged(TorchStatus, const String16&) {};
      private:
        const wp<CameraManagerGlobal> mCameraManager;
    };
    sp<CameraServiceListener> mCameraServiceListener;

    // Wrapper of ACameraManager_AvailabilityCallbacks so we can store it in std::set
    struct Callback {
        Callback(const ACameraManager_AvailabilityCallbacks *callback) :
            mAvailable(callback->onCameraAvailable),
            mUnavailable(callback->onCameraUnavailable),
            mContext(callback->context) {}

        bool operator == (const Callback& other) const {
            return (mAvailable == other.mAvailable &&
                    mUnavailable == other.mUnavailable &&
                    mContext == other.mContext);
        }
        bool operator != (const Callback& other) const {
            return !(*this == other);
        }
        bool operator < (const Callback& other) const {
            if (*this == other) return false;
            if (mContext != other.mContext) return mContext < other.mContext;
            if (mAvailable != other.mAvailable) return mAvailable < other.mAvailable;
            return mUnavailable < other.mUnavailable;
        }
        bool operator > (const Callback& other) const {
            return (*this != other && !(*this < other));
        }
        ACameraManager_AvailabilityCallback mAvailable;
        ACameraManager_AvailabilityCallback mUnavailable;
        void*                               mContext;
    };
    std::set<Callback> mCallbacks;

    // definition of handler and message
    enum {
        kWhatSendSingleCallback
    };
    static const char* kCameraIdKey;
    static const char* kCallbackFpKey;
    static const char* kContextKey;
    class CallbackHandler : public AHandler {
      public:
        CallbackHandler() {}
        void onMessageReceived(const sp<AMessage> &msg) override;
      private:
        inline void sendSingleCallback(
                int32_t cameraId, void* context,
                ACameraManager_AvailabilityCallback cb) const;
    };
    sp<CallbackHandler> mHandler;
    sp<ALooper>         mCbLooper; // Looper thread where callbacks actually happen on

    typedef ICameraServiceListener::Status Status;
    void onStatusChanged(Status status, int32_t cameraId);
    void onStatusChangedLocked(Status status, int32_t cameraId);
    // Utils for status
    static bool validStatus(Status status);
    static bool isStatusAvailable(Status status);

    // Map camera_id -> status
    std::map<int32_t, Status> mDeviceStatusMap;

    // For the singleton instance
    static Mutex sLock;
    static CameraManagerGlobal* sInstance;
    CameraManagerGlobal() {};
    ~CameraManagerGlobal();
};

} // namespace android;

/**
 * ACameraManager opaque struct definition
 * Leave outside of android namespace because it's NDK struct
 */
struct ACameraManager {
    ACameraManager() :
            mCachedCameraIdList({kCameraIdListNotInit, nullptr}),
            mGlobalManager(&(CameraManagerGlobal::getInstance())) {}
    ~ACameraManager();
    camera_status_t getCameraIdList(ACameraIdList** cameraIdList);
    static void     deleteCameraIdList(ACameraIdList* cameraIdList);

    camera_status_t getCameraCharacteristics(
            const char *cameraId, ACameraMetadata **characteristics);
    camera_status_t openCamera(const char* cameraId,
                               ACameraDevice_StateCallbacks* callback,
                               /*out*/ACameraDevice** device);

  private:
    camera_status_t getOrCreateCameraIdListLocked(ACameraIdList** cameraIdList);

    enum {
        kCameraIdListNotInit = -1
    };
    Mutex         mLock;
    std::set<int> mCameraIds;          // Init by getOrCreateCameraIdListLocked
    ACameraIdList mCachedCameraIdList; // Init by getOrCreateCameraIdListLocked
    sp<CameraManagerGlobal> mGlobalManager;
};

#endif //_ACAMERA_MANAGER_H
