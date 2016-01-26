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
#include <camera/ICameraService.h>
#include <camera/ICameraServiceListener.h>
#include <camera/camera2/CaptureRequest.h>
#include <camera/camera2/ICameraDeviceUser.h>
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/OutputConfiguration.h>

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
class TestCameraServiceListener : public BnCameraServiceListener {
    std::map<String16, TorchStatus> mCameraTorchStatuses;
    std::map<int32_t, Status> mCameraStatuses;
    mutable Mutex mLock;
    mutable Condition mCondition;
    mutable Condition mTorchCondition;
public:
    virtual ~TestCameraServiceListener() {};

    virtual void onStatusChanged(Status status, int32_t cameraId) {
        Mutex::Autolock l(mLock);
        mCameraStatuses[cameraId] = status;
        mCondition.broadcast();
    };

    virtual void onTorchStatusChanged(TorchStatus status, const String16& cameraId) {
        Mutex::Autolock l(mLock);
        mCameraTorchStatuses[cameraId] = status;
        mTorchCondition.broadcast();
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

    bool waitForTorchState(TorchStatus status, int32_t cameraId) const {
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

    TorchStatus getTorchStatus(int32_t cameraId) const {
        Mutex::Autolock l(mLock);
        const auto& iter = mCameraTorchStatuses.find(String16(String8::format("%d", cameraId)));
        if (iter == mCameraTorchStatuses.end()) {
            return ICameraServiceListener::TORCH_STATUS_UNKNOWN;
        }
        return iter->second;
    };

    Status getStatus(int32_t cameraId) const {
        Mutex::Autolock l(mLock);
        const auto& iter = mCameraStatuses.find(cameraId);
        if (iter == mCameraStatuses.end()) {
            return ICameraServiceListener::STATUS_UNKNOWN;
        }
        return iter->second;
    };
};

// Callback implementation
class TestCameraDeviceCallbacks : public BnCameraDeviceCallbacks {
public:
    enum Status {
        IDLE,
        ERROR,
        PREPARED,
        RUNNING,
        SENT_RESULT,
        UNINITIALIZED
    };

protected:
    bool mError;
    Status mLastStatus;
    mutable std::vector<Status> mStatusesHit;
    mutable Mutex mLock;
    mutable Condition mStatusCondition;
public:
    TestCameraDeviceCallbacks() : mError(false), mLastStatus(UNINITIALIZED) {}

    virtual ~TestCameraDeviceCallbacks() {}

    virtual void onDeviceError(CameraErrorCode errorCode,
            const CaptureResultExtras& resultExtras) {
        (void) resultExtras;
        ALOGE("%s: onDeviceError occurred with: %d", __FUNCTION__, static_cast<int>(errorCode));
        Mutex::Autolock l(mLock);
        mError = true;
        mLastStatus = ERROR;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    virtual void onDeviceIdle() {
        Mutex::Autolock l(mLock);
        mLastStatus = IDLE;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    virtual void onCaptureStarted(const CaptureResultExtras& resultExtras,
            int64_t timestamp) {
        (void) resultExtras;
        (void) timestamp;
        Mutex::Autolock l(mLock);
        mLastStatus = RUNNING;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }


    virtual void onResultReceived(const CameraMetadata& metadata,
            const CaptureResultExtras& resultExtras) {
        (void) metadata;
        (void) resultExtras;
        Mutex::Autolock l(mLock);
        mLastStatus = SENT_RESULT;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
    }

    virtual void onPrepared(int streamId) {
        (void) streamId;
        Mutex::Autolock l(mLock);
        mLastStatus = PREPARED;
        mStatusesHit.push_back(mLastStatus);
        mStatusCondition.broadcast();
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
    sp<ICameraService> service = interface_cast<ICameraService>(binder);


    int32_t numCameras = service->getNumberOfCameras(ICameraService::CAMERA_TYPE_ALL);
    EXPECT_LE(0, numCameras);

    // Check listener binder calls
    sp<TestCameraServiceListener> listener(new TestCameraServiceListener());
    EXPECT_EQ(OK, service->addListener(listener));

    EXPECT_TRUE(listener->waitForNumCameras(numCameras));

    for (int32_t i = 0; i < numCameras; i++) {
        // We only care about binder calls for the Camera2 API.  Camera1 is deprecated.
        status_t camera2Support = service->supportsCameraApi(i, ICameraService::API_VERSION_2);
        if (camera2Support != OK) {
            EXPECT_EQ(-EOPNOTSUPP, camera2Support);
            continue;
        }

        // Check metadata binder call
        CameraMetadata metadata;
        EXPECT_EQ(OK, service->getCameraCharacteristics(i, &metadata));
        EXPECT_FALSE(metadata.isEmpty());

        // Make sure we're available, or skip device tests otherwise
        ICameraServiceListener::Status s = listener->getStatus(i);
        EXPECT_EQ(ICameraServiceListener::STATUS_AVAILABLE, s);
        if (s != ICameraServiceListener::STATUS_AVAILABLE) {
            continue;
        }

        // Check connect binder calls
        sp<TestCameraDeviceCallbacks> callbacks(new TestCameraDeviceCallbacks());
        sp<ICameraDeviceUser> device;
        EXPECT_EQ(OK, service->connectDevice(callbacks, i, String16("meeeeeeeee!"),
                ICameraService::USE_CALLING_UID, /*out*/device));
        ASSERT_NE(nullptr, device.get());
        device->disconnect();
        EXPECT_FALSE(callbacks->hadError());

        ICameraServiceListener::TorchStatus torchStatus = listener->getTorchStatus(i);
        if (torchStatus == ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF) {
            // Check torch calls
            EXPECT_EQ(OK, service->setTorchMode(String16(String8::format("%d", i)),
                    /*enabled*/true, callbacks));
            EXPECT_TRUE(listener->waitForTorchState(
                    ICameraServiceListener::TORCH_STATUS_AVAILABLE_ON, i));
            EXPECT_EQ(OK, service->setTorchMode(String16(String8::format("%d", i)),
                    /*enabled*/false, callbacks));
            EXPECT_TRUE(listener->waitForTorchState(
                    ICameraServiceListener::TORCH_STATUS_AVAILABLE_OFF, i));
        }
    }

    EXPECT_EQ(OK, service->removeListener(listener));
}

// Test fixture for client focused binder tests
class CameraClientBinderTest : public testing::Test {
protected:
    sp<ICameraService> service;
    int32_t numCameras;
    std::vector<std::pair<sp<TestCameraDeviceCallbacks>, sp<ICameraDeviceUser>>> openDeviceList;
    sp<TestCameraServiceListener> serviceListener;

    std::pair<sp<TestCameraDeviceCallbacks>, sp<ICameraDeviceUser>> openNewDevice(int deviceId) {

        sp<TestCameraDeviceCallbacks> callbacks(new TestCameraDeviceCallbacks());
        sp<ICameraDeviceUser> device;
        {
            SCOPED_TRACE("openNewDevice");
            EXPECT_EQ(OK, service->connectDevice(callbacks, deviceId, String16("meeeeeeeee!"),
                    ICameraService::USE_CALLING_UID, /*out*/device));
        }
        auto p = std::make_pair(callbacks, device);
        openDeviceList.push_back(p);
        return p;
    }

    void closeDevice(std::pair<sp<TestCameraDeviceCallbacks>, sp<ICameraDeviceUser>>& p) {
        if (p.second.get() != nullptr) {
            p.second->disconnect();
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
        service = interface_cast<ICameraService>(binder);
        serviceListener = new TestCameraServiceListener();
        service->addListener(serviceListener);
        numCameras = service->getNumberOfCameras();
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
        ICameraServiceListener::Status s = serviceListener->getStatus(i);
        EXPECT_EQ(ICameraServiceListener::STATUS_AVAILABLE, s);
        if (s != ICameraServiceListener::STATUS_AVAILABLE) {
            continue;
        }

        auto p = openNewDevice(i);
        sp<TestCameraDeviceCallbacks> callbacks = p.first;
        sp<ICameraDeviceUser> device = p.second;

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
        EXPECT_EQ(OK, device->beginConfigure());
        status_t streamId = device->createStream(output);
        EXPECT_LE(0, streamId);
        EXPECT_EQ(OK, device->endConfigure());
        EXPECT_FALSE(callbacks->hadError());

        // Can we make requests?
        CameraMetadata requestTemplate;
        EXPECT_EQ(OK, device->createDefaultRequest(/*preview template*/1,
                /*out*/&requestTemplate));
        sp<CaptureRequest> request(new CaptureRequest());
        request->mMetadata = requestTemplate;
        request->mSurfaceList.add(surface);
        request->mIsReprocess = false;
        int64_t lastFrameNumber = 0;
        int64_t lastFrameNumberPrev = 0;
        callbacks->clearStatus();
        int requestId = device->submitRequest(request, /*streaming*/true, /*out*/&lastFrameNumber);
        EXPECT_TRUE(callbacks->waitForStatus(TestCameraDeviceCallbacks::SENT_RESULT));
        EXPECT_LE(0, requestId);

        // Can we stop requests?
        EXPECT_EQ(OK, device->cancelRequest(requestId, /*out*/&lastFrameNumber));
        EXPECT_TRUE(callbacks->waitForIdle());
        EXPECT_FALSE(callbacks->hadError());

        // Can we do it again?
        lastFrameNumberPrev = lastFrameNumber;
        lastFrameNumber = 0;
        requestTemplate.clear();
        EXPECT_EQ(OK, device->createDefaultRequest(/*preview template*/1,
                /*out*/&requestTemplate));
        sp<CaptureRequest> request2(new CaptureRequest());
        request2->mMetadata = requestTemplate;
        request2->mSurfaceList.add(surface);
        request2->mIsReprocess = false;
        callbacks->clearStatus();
        int requestId2 = device->submitRequest(request2, /*streaming*/true,
                /*out*/&lastFrameNumber);
        EXPECT_EQ(-1, lastFrameNumber);
        lastFrameNumber = 0;
        EXPECT_TRUE(callbacks->waitForStatus(TestCameraDeviceCallbacks::SENT_RESULT));
        EXPECT_LE(0, requestId2);
        EXPECT_EQ(OK, device->cancelRequest(requestId2, /*out*/&lastFrameNumber));
        EXPECT_TRUE(callbacks->waitForIdle());
        EXPECT_LE(lastFrameNumberPrev, lastFrameNumber);
        sleep(/*second*/1); // allow some time for errors to show up, if any
        EXPECT_FALSE(callbacks->hadError());

        // Can we do it with a request list?
        lastFrameNumberPrev = lastFrameNumber;
        lastFrameNumber = 0;
        requestTemplate.clear();
        CameraMetadata requestTemplate2;
        EXPECT_EQ(OK, device->createDefaultRequest(/*preview template*/1,
                /*out*/&requestTemplate));
        EXPECT_EQ(OK, device->createDefaultRequest(/*preview template*/1,
                /*out*/&requestTemplate2));
        sp<CaptureRequest> request3(new CaptureRequest());
        sp<CaptureRequest> request4(new CaptureRequest());
        request3->mMetadata = requestTemplate;
        request3->mSurfaceList.add(surface);
        request3->mIsReprocess = false;
        request4->mMetadata = requestTemplate2;
        request4->mSurfaceList.add(surface);
        request4->mIsReprocess = false;
        List<sp<CaptureRequest>> requestList;
        requestList.push_back(request3);
        requestList.push_back(request4);

        callbacks->clearStatus();
        int requestId3 = device->submitRequestList(requestList, /*streaming*/false,
                /*out*/&lastFrameNumber);
        EXPECT_LE(0, requestId3);
        EXPECT_TRUE(callbacks->waitForStatus(TestCameraDeviceCallbacks::SENT_RESULT));
        EXPECT_TRUE(callbacks->waitForIdle());
        EXPECT_LE(lastFrameNumberPrev, lastFrameNumber);
        sleep(/*second*/1); // allow some time for errors to show up, if any
        EXPECT_FALSE(callbacks->hadError());

        // Can we unconfigure?
        EXPECT_EQ(OK, device->beginConfigure());
        EXPECT_EQ(OK, device->deleteStream(streamId));
        EXPECT_EQ(OK, device->endConfigure());
        sleep(/*second*/1); // allow some time for errors to show up, if any
        EXPECT_FALSE(callbacks->hadError());

        closeDevice(p);
    }

};
