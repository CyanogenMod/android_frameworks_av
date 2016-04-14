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

#define LOG_NDEBUG 0
#define LOG_TAG "CameraBinderTests"

#include <binder/IInterface.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/List.h>
#include <utils/String8.h>
#include <utils/String16.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <system/graphics.h>
#include <hardware/gralloc.h>

#include <camera/CameraMetadata.h>
#include <android/hardware/ICameraService.h>
#include <android/hardware/ICameraServiceListener.h>
#include <android/hardware/BnCameraServiceListener.h>
#include <android/hardware/camera2/ICameraDeviceUser.h>
#include <android/hardware/camera2/ICameraDeviceCallbacks.h>
#include <android/hardware/camera2/BnCameraDeviceCallbacks.h>
#include <camera/camera2/CaptureRequest.h>
#include <camera/camera2/OutputConfiguration.h>
#include <camera/camera2/SubmitInfo.h>

#include <gui/BufferItemConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>

#include <gtest/gtest.h>
#include <unistd.h>
#include <stdint.h>
#include <utility>
#include <vector>
#include <map>
#include <algorithm>

using namespace android;

#define ASSERT_NOT_NULL(x) \
    ASSERT_TRUE((x) != nullptr)

#define SETUP_TIMEOUT 2000000000 // ns
#define IDLE_TIMEOUT 2000000000 // ns

// Stub listener implementation
class TestCameraServiceListener : public hardware::BnCameraServiceListener {
    std::map<String16, int32_t> mCameraTorchStatuses;
    std::map<int32_t, int32_t> mCameraStatuses;
    mutable Mutex mLock;
    mutable Condition mCondition;
    mutable Condition mTorchCondition;
public:
    virtual ~TestCameraServiceListener() {};

    virtual binder::Status onStatusChanged(int32_t status, int32_t cameraId) {
        Mutex::Autolock l(mLock);
        mCameraStatuses[cameraId] = status;
        mCondition.broadcast();
        return binder::Status::ok();
    };

    virtual binder::Status onTorchStatusChanged(int32_t status, const String16& cameraId) {
        Mutex::Autolock l(mLock);
        mCameraTorchStatuses[cameraId] = status;
        mTorchCondition.broadcast();
        return binder::Status::ok();
    };

    bool waitForNumCameras(size_t num) const {
        Mutex::Autolock l(mLock);

        if (mCameraStatuses.size() == num) {
            return true;
        }

        while (mCameraStatuses.size() < num) {
            if (mCondition.waitRelative(mLock, SETUP_TIMEOUT) != OK) {
                return false;
            }
        }
        return true;
    };

    bool waitForTorchState(int32_t status, int32_t cameraId) const {
        Mutex::Autolock l(mLock);

        const auto& iter = mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
        if (iter != mCameraTorchStatuses.end() && iter->second == status) {
            return true;
        }

        bool foundStatus = false;
        while (!foundStatus) {
            if (mTorchCondition.waitRelative(mLock, SETUP_TIMEOUT) != OK) {
                return false;
            }
            const auto& iter =
                    mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
            foundStatus = (iter != mCameraTorchStatuses.end() && iter->second == status);
        }
        return true;
    };

    int32_t getTorchStatus(int32_t cameraId) const {
        Mutex::Autolock l(mLock);
        const auto& iter = mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
        if (iter == mCameraTorchStatuses.end()) {
            return hardware::ICameraServiceListener::TORCH_STATUS_UNKNOWN;
        }
        return iter->second;
    };

    int32_t getStatus(int32_t cameraId) const {
        Mutex::Autolock l(mLock);
        const auto& iter = mCameraStatuses.find(cameraId);
        if (iter == mCameraStatuses.end()) {
            return hardware::ICameraServiceListener::STATUS_UNKNOWN;
        }
        return iter->second;
    };
};

// Callback implementation
class TestCameraDeviceCallbacks : public hardware::camera2::BnCameraDeviceCallbacks {
public:
    enum Status {
        IDLE,
        ERROR,
        PREPARED,
        RUNNING,
        SENT_RESULT,
        UNINITIALIZED,
        REPEATING_REQUEST_ERROR,
    };

protected:
    bool mError;
    int32_t mLastStatus;
    mutable std::vector<int32_t> mStatusesHit;
    mutable Mutex mLock;
    mutable Condition mStatusCondition;
public:
    TestCameraDeviceCallbacks() : mError(false), mLastStatus(UNINITIALIZED) {}

    virtual ~TestCameraDeviceCallbacks() {}

    virtual binder::Status onDeviceError(int errorCode,
            const CaptureResultExtras& resultExtras) {
        (void) resultExtras;
        ALOGE("%s: onDeviceError occurred with: %d", __FUNCTION__, static_cast<int>(errorCode));
        Mutex::Autolock l(mLock);
        mError = true;
        mLastStatus = ERROR;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
        return binder::Status::ok();
    }

    virtual binder::Status onDeviceIdle() {
        Mutex::Autolock l(mLock);
        mLastStatus = IDLE;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
        return binder::Status::ok();
    }

    virtual binder::Status onCaptureStarted(const CaptureResultExtras& resultExtras,
            int64_t timestamp) {
        (void) resultExtras;
        (void) timestamp;
        Mutex::Autolock l(mLock);
        mLastStatus = RUNNING;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
        return binder::Status::ok();
    }


    virtual binder::Status onResultReceived(const CameraMetadata& metadata,
            const CaptureResultExtras& resultExtras) {
        (void) metadata;
        (void) resultExtras;
        Mutex::Autolock l(mLock);
        mLastStatus = SENT_RESULT;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
        return binder::Status::ok();
    }

    virtual binder::Status onPrepared(int streamId) {
        (void) streamId;
        Mutex::Autolock l(mLock);
        mLastStatus = PREPARED;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
        return binder::Status::ok();
    }

    virtual binder::Status onRepeatingRequestError(int64_t lastFrameNumber) {
        (void) lastFrameNumber;
        Mutex::Autolock l(mLock);
        mLastStatus = REPEATING_REQUEST_ERROR;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
        return binder::Status::ok();
    }

    // Test helper functions:

    bool hadError() const {
        Mutex::Autolock l(mLock);
        return mError;
    }

    bool waitForStatus(Status status) const {
        Mutex::Autolock l(mLock);
        if (mLastStatus == status) {
            return true;
        }

        while (std::find(mStatusesHit.begin(), mStatusesHit.end(), status)
                == mStatusesHit.end()) {

            if (mStatusCondition.waitRelative(mLock, IDLE_TIMEOUT) != OK) {
                mStatusesHit.clear();
                return false;
            }
        }
        mStatusesHit.clear();

        return true;

    }

    void clearStatus() const {
        Mutex::Autolock l(mLock);
        mStatusesHit.clear();
    }

    bool waitForIdle() const {
        return waitForStatus(IDLE);
    }

};

namespace {
    Mutex                     gLock;
    class DeathNotifier : public IBinder::DeathRecipient
    {
    public:
        DeathNotifier() {}

        virtual void binderDied(const wp<IBinder>& /*who*/) {
            ALOGV("binderDied");
            Mutex::Autolock _l(gLock);
            ALOGW("Camera service died!");
        }
    };
    sp<DeathNotifier>         gDeathNotifier;
}; // anonymous namespace

// Exercise basic binder calls for the camera service
TEST(CameraServiceBinderTest, CheckBinderCameraService) {
    ProcessState::self()->startThreadPool();
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.camera"));
    ASSERT_NOT_NULL(binder);
    if (gDeathNotifier == NULL) {
        gDeathNotifier = new DeathNotifier();
    }
    binder->linkToDeath(gDeathNotifier);
    sp<hardware::ICameraService> service =
            interface_cast<hardware::ICameraService>(binder);

    binder::Status res;

    int32_t numCameras = 0;
    res = service->getNumberOfCameras(hardware::ICameraService::CAMERA_TYPE_ALL, &numCameras);
    EXPECT_TRUE(res.isOk()) << res;
    EXPECT_LE(0, numCameras);

    // Check listener binder calls
    sp<TestCameraServiceListener> listener(new TestCameraServiceListener());
    res = service->addListener(listener);
    EXPECT_TRUE(res.isOk()) << res;

    EXPECT_TRUE(listener->waitForNumCameras(numCameras));

    for (int32_t i = 0; i < numCameras; i++) {
        bool isSupported = false;
        res = service->supportsCameraApi(i,
                hardware::ICameraService::API_VERSION_2, &isSupported);
        EXPECT_TRUE(res.isOk()) << res;

        // We only care about binder calls for the Camera2 API.  Camera1 is deprecated.
        if (!isSupported) {
            continue;
        }

        // Check metadata binder call
        CameraMetadata metadata;
        res = service->getCameraCharacteristics(i, &metadata);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_FALSE(metadata.isEmpty());

        // Make sure we're available, or skip device tests otherwise
        int32_t s = listener->getStatus(i);
        EXPECT_EQ(::android::hardware::ICameraServiceListener::STATUS_PRESENT, s);
        if (s != ::android::hardware::ICameraServiceListener::STATUS_PRESENT) {
            continue;
        }

        // Check connect binder calls
        sp<TestCameraDeviceCallbacks> callbacks(new TestCameraDeviceCallbacks());
        sp<hardware::camera2::ICameraDeviceUser> device;
        res = service->connectDevice(callbacks, i, String16("meeeeeeeee!"),
                hardware::ICameraService::USE_CALLING_UID, /*out*/&device);
        EXPECT_TRUE(res.isOk()) << res;
        ASSERT_NE(nullptr, device.get());
        device->disconnect();
        EXPECT_FALSE(callbacks->hadError());

        int32_t torchStatus = listener->getTorchStatus(i);
        if (torchStatus == hardware::ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF) {
            // Check torch calls
            res = service->setTorchMode(String16(String8::format("%d", i)),
                    /*enabled*/true, callbacks);
            EXPECT_TRUE(res.isOk()) << res;
            EXPECT_TRUE(listener->waitForTorchState(
                    hardware::ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON, i));
            res = service->setTorchMode(String16(String8::format("%d", i)),
                    /*enabled*/false, callbacks);
            EXPECT_TRUE(res.isOk()) << res;
            EXPECT_TRUE(listener->waitForTorchState(
                    hardware::ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF, i));
        }
    }

    res = service->removeListener(listener);
    EXPECT_TRUE(res.isOk()) << res;
}

// Test fixture for client focused binder tests
class CameraClientBinderTest : public testing::Test {
protected:
    sp<hardware::ICameraService> service;
    int32_t numCameras;
    std::vector<std::pair<sp<TestCameraDeviceCallbacks>, sp<hardware::camera2::ICameraDeviceUser>>>
            openDeviceList;
    sp<TestCameraServiceListener> serviceListener;

    std::pair<sp<TestCameraDeviceCallbacks>, sp<hardware::camera2::ICameraDeviceUser>>
            openNewDevice(int deviceId) {
        sp<TestCameraDeviceCallbacks> callbacks(new TestCameraDeviceCallbacks());
        sp<hardware::camera2::ICameraDeviceUser> device;
        {
            SCOPED_TRACE("openNewDevice");
            binder::Status res = service->connectDevice(callbacks, deviceId, String16("meeeeeeeee!"),
                    hardware::ICameraService::USE_CALLING_UID, /*out*/&device);
            EXPECT_TRUE(res.isOk()) << res;
        }
        auto p = std::make_pair(callbacks, device);
        openDeviceList.push_back(p);
        return p;
    }

    void closeDevice(std::pair<sp<TestCameraDeviceCallbacks>,
            sp<hardware::camera2::ICameraDeviceUser>>& p) {
        if (p.second.get() != nullptr) {
            binder::Status res = p.second->disconnect();
            EXPECT_TRUE(res.isOk()) << res;
            {
                SCOPED_TRACE("closeDevice");
                EXPECT_FALSE(p.first->hadError());
            }
        }
        auto iter = std::find(openDeviceList.begin(), openDeviceList.end(), p);
        if (iter != openDeviceList.end()) {
            openDeviceList.erase(iter);
        }
    }

    virtual void SetUp() {
        ProcessState::self()->startThreadPool();
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16("media.camera"));
        service = interface_cast<hardware::ICameraService>(binder);
        serviceListener = new TestCameraServiceListener();
        service->addListener(serviceListener);
        service->getNumberOfCameras(hardware::ICameraService::CAMERA_TYPE_BACKWARD_COMPATIBLE,
                &numCameras);
    }

    virtual void TearDown() {
        service = nullptr;
        numCameras = 0;
        for (auto& p : openDeviceList) {
            closeDevice(p);
        }
    }

};

TEST_F(CameraClientBinderTest, CheckBinderCameraDeviceUser) {
    ASSERT_NOT_NULL(service);
    EXPECT_TRUE(serviceListener->waitForNumCameras(numCameras));
    for (int32_t i = 0; i < numCameras; i++) {
        // Make sure we're available, or skip device tests otherwise
        int32_t s = serviceListener->getStatus(i);
        EXPECT_EQ(hardware::ICameraServiceListener::STATUS_PRESENT, s);
        if (s != hardware::ICameraServiceListener::STATUS_PRESENT) {
            continue;
        }
        binder::Status res;

        auto p = openNewDevice(i);
        sp<TestCameraDeviceCallbacks> callbacks = p.first;
        sp<hardware::camera2::ICameraDeviceUser> device = p.second;

        // Setup a buffer queue; I'm just using the vendor opaque format here as that is
        // guaranteed to be present
        sp<IGraphicBufferProducer> gbProducer;
        sp<IGraphicBufferConsumer> gbConsumer;
        BufferQueue::createBufferQueue(&gbProducer, &gbConsumer);
        sp<BufferItemConsumer> opaqueConsumer = new BufferItemConsumer(gbConsumer,
                GRALLOC_USAGE_SW_READ_NEVER, /*maxImages*/2, /*controlledByApp*/true);
        EXPECT_TRUE(opaqueConsumer.get() != nullptr);
        opaqueConsumer->setName(String8("nom nom nom"));

        // Set to VGA dimens for default, as that is guaranteed to be present
        EXPECT_EQ(OK, gbConsumer->setDefaultBufferSize(640, 480));
        EXPECT_EQ(OK, gbConsumer->setDefaultBufferFormat(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED));

        sp<Surface> surface(new Surface(gbProducer, /*controlledByApp*/false));

        OutputConfiguration output(gbProducer, /*rotation*/0);

        // Can we configure?
        res = device->beginConfigure();
        EXPECT_TRUE(res.isOk()) << res;
        status_t streamId;
        res = device->createStream(output, &streamId);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_LE(0, streamId);
        res = device->endConfigure(/*isConstrainedHighSpeed*/ false);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_FALSE(callbacks->hadError());

        // Can we make requests?
        CameraMetadata requestTemplate;
        res = device->createDefaultRequest(/*preview template*/1,
                /*out*/&requestTemplate);
        EXPECT_TRUE(res.isOk()) << res;

        hardware::camera2::CaptureRequest request;
        request.mMetadata = requestTemplate;
        request.mSurfaceList.add(surface);
        request.mIsReprocess = false;
        int64_t lastFrameNumber = 0;
        int64_t lastFrameNumberPrev = 0;
        callbacks->clearStatus();

        hardware::camera2::utils::SubmitInfo info;
        res = device->submitRequest(request, /*streaming*/true, /*out*/&info);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_TRUE(callbacks->waitForStatus(TestCameraDeviceCallbacks::SENT_RESULT));
        EXPECT_LE(0, info.mRequestId);

        // Can we stop requests?
        res = device->cancelRequest(info.mRequestId, /*out*/&lastFrameNumber);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_TRUE(callbacks->waitForIdle());
        EXPECT_FALSE(callbacks->hadError());

        // Can we do it again?
        lastFrameNumberPrev = info.mLastFrameNumber;
        lastFrameNumber = 0;
        requestTemplate.clear();
        res = device->createDefaultRequest(hardware::camera2::ICameraDeviceUser::TEMPLATE_PREVIEW,
                /*out*/&requestTemplate);
        EXPECT_TRUE(res.isOk()) << res;
        hardware::camera2::CaptureRequest request2;
        request2.mMetadata = requestTemplate;
        request2.mSurfaceList.add(surface);
        request2.mIsReprocess = false;
        callbacks->clearStatus();
        hardware::camera2::utils::SubmitInfo info2;
        res = device->submitRequest(request2, /*streaming*/true,
                /*out*/&info2);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_EQ(hardware::camera2::ICameraDeviceUser::NO_IN_FLIGHT_REPEATING_FRAMES,
                info2.mLastFrameNumber);
        lastFrameNumber = 0;
        EXPECT_TRUE(callbacks->waitForStatus(TestCameraDeviceCallbacks::SENT_RESULT));
        EXPECT_LE(0, info2.mRequestId);
        res = device->cancelRequest(info2.mRequestId, /*out*/&lastFrameNumber);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_TRUE(callbacks->waitForIdle());
        EXPECT_LE(lastFrameNumberPrev, lastFrameNumber);
        sleep(/*second*/1); // allow some time for errors to show up, if any
        EXPECT_FALSE(callbacks->hadError());

        // Can we do it with a request list?
        lastFrameNumberPrev = lastFrameNumber;
        lastFrameNumber = 0;
        requestTemplate.clear();
        CameraMetadata requestTemplate2;
        res = device->createDefaultRequest(hardware::camera2::ICameraDeviceUser::TEMPLATE_PREVIEW,
                /*out*/&requestTemplate);
        EXPECT_TRUE(res.isOk()) << res;
        res = device->createDefaultRequest(hardware::camera2::ICameraDeviceUser::TEMPLATE_PREVIEW,
                /*out*/&requestTemplate2);
        EXPECT_TRUE(res.isOk()) << res;
        android::hardware::camera2::CaptureRequest request3;
        android::hardware::camera2::CaptureRequest request4;
        request3.mMetadata = requestTemplate;
        request3.mSurfaceList.add(surface);
        request3.mIsReprocess = false;
        request4.mMetadata = requestTemplate2;
        request4.mSurfaceList.add(surface);
        request4.mIsReprocess = false;
        std::vector<hardware::camera2::CaptureRequest> requestList;
        requestList.push_back(request3);
        requestList.push_back(request4);

        callbacks->clearStatus();
        hardware::camera2::utils::SubmitInfo info3;
        res = device->submitRequestList(requestList, /*streaming*/false,
                /*out*/&info3);
        EXPECT_TRUE(res.isOk()) << res;
        EXPECT_LE(0, info3.mRequestId);
        EXPECT_TRUE(callbacks->waitForStatus(TestCameraDeviceCallbacks::SENT_RESULT));
        EXPECT_TRUE(callbacks->waitForIdle());
        EXPECT_LE(lastFrameNumberPrev, info3.mLastFrameNumber);
        sleep(/*second*/1); // allow some time for errors to show up, if any
        EXPECT_FALSE(callbacks->hadError());

        // Can we unconfigure?
        res = device->beginConfigure();
        EXPECT_TRUE(res.isOk()) << res;
        res = device->deleteStream(streamId);
        EXPECT_TRUE(res.isOk()) << res;
        res = device->endConfigure(/*isConstrainedHighSpeed*/ false);
        EXPECT_TRUE(res.isOk()) << res;

        sleep(/*second*/1); // allow some time for errors to show up, if any
        EXPECT_FALSE(callbacks->hadError());

        closeDevice(p);
    }

};
