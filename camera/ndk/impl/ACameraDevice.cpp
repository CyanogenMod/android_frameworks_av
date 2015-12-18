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
#define LOG_TAG "ACameraDevice"

#include <vector>
#include <utility>
#include <inttypes.h>
#include <android/hardware/ICameraService.h>
#include <camera2/SubmitInfo.h>
#include <gui/Surface.h>
#include "ACameraDevice.h"
#include "ACameraMetadata.h"
#include "ACaptureRequest.h"
#include "ACameraCaptureSession.h"

using namespace android;

namespace android {
// Static member definitions
const char* CameraDevice::kContextKey        = "Context";
const char* CameraDevice::kDeviceKey         = "Device";
const char* CameraDevice::kErrorCodeKey      = "ErrorCode";
const char* CameraDevice::kCallbackFpKey     = "Callback";
const char* CameraDevice::kSessionSpKey      = "SessionSp";
const char* CameraDevice::kCaptureRequestKey = "CaptureRequest";
const char* CameraDevice::kTimeStampKey      = "TimeStamp";
const char* CameraDevice::kCaptureResultKey  = "CaptureResult";
const char* CameraDevice::kCaptureFailureKey = "CaptureFailure";
const char* CameraDevice::kSequenceIdKey     = "SequenceId";
const char* CameraDevice::kFrameNumberKey    = "FrameNumber";

/**
 * CameraDevice Implementation
 */
CameraDevice::CameraDevice(
        const char* id,
        ACameraDevice_StateCallbacks* cb,
        std::unique_ptr<ACameraMetadata> chars,
        ACameraDevice* wrapper) :
        mCameraId(id),
        mAppCallbacks(*cb),
        mChars(std::move(chars)),
        mServiceCallback(new ServiceCallback(this)),
        mWrapper(wrapper),
        mInError(false),
        mError(ACAMERA_OK),
        mIdle(true) {
    mClosing = false;
    // Setup looper thread to perfrom device callbacks to app
    mCbLooper = new ALooper;
    mCbLooper->setName("C2N-dev-looper");
    status_t ret = mCbLooper->start(
            /*runOnCallingThread*/false,
            /*canCallJava*/       true,
            PRIORITY_DEFAULT);
    mHandler = new CallbackHandler();
    mCbLooper->registerHandler(mHandler);

    CameraMetadata metadata = mChars->mData;
    camera_metadata_entry entry = metadata.find(ANDROID_REQUEST_PARTIAL_RESULT_COUNT);
    if (entry.count != 1) {
        ALOGW("%s: bad count %zu for partial result count", __FUNCTION__, entry.count);
        mPartialResultCount = 1;
    } else {
        mPartialResultCount = entry.data.i32[0];
    }

    entry = metadata.find(ANDROID_LENS_INFO_SHADING_MAP_SIZE);
    if (entry.count != 2) {
        ALOGW("%s: bad count %zu for shading map size", __FUNCTION__, entry.count);
        mShadingMapSize[0] = 0;
        mShadingMapSize[1] = 0;
    } else {
        mShadingMapSize[0] = entry.data.i32[0];
        mShadingMapSize[1] = entry.data.i32[1];
    }
}

// Device close implementaiton
CameraDevice::~CameraDevice() {
    Mutex::Autolock _l(mDeviceLock);
    if (!isClosed()) {
        disconnectLocked();
    }
    if (mCbLooper != nullptr) {
        mCbLooper->unregisterHandler(mHandler->id());
        mCbLooper->stop();
    }
    mCbLooper.clear();
    mHandler.clear();
}

// TODO: cached created request?
camera_status_t
CameraDevice::createCaptureRequest(
        ACameraDevice_request_template templateId,
        ACaptureRequest** request) const {
    Mutex::Autolock _l(mDeviceLock);
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }
    if (mRemote == nullptr) {
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    CameraMetadata rawRequest;
    binder::Status remoteRet = mRemote->createDefaultRequest(templateId, &rawRequest);
    if (remoteRet.serviceSpecificErrorCode() ==
            hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT) {
        ALOGW("Create capture request failed! template %d is not supported on this device",
            templateId);
        return ACAMERA_ERROR_UNSUPPORTED;
    } else if (!remoteRet.isOk()) {
        ALOGE("Create capture request failed: %s", remoteRet.toString8().string());
        return ACAMERA_ERROR_UNKNOWN;
    }
    ACaptureRequest* outReq = new ACaptureRequest();
    outReq->settings = new ACameraMetadata(rawRequest.release(), ACameraMetadata::ACM_REQUEST);
    outReq->targets  = new ACameraOutputTargets();
    *request = outReq;
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::createCaptureSession(
        const ACaptureSessionOutputContainer*       outputs,
        const ACameraCaptureSession_stateCallbacks* callbacks,
        /*out*/ACameraCaptureSession** session) {
    Mutex::Autolock _l(mDeviceLock);
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    if (mCurrentSession != nullptr) {
        mCurrentSession->closeByDevice();
        stopRepeatingLocked();
    }

    // Create new session
    ret = configureStreamsLocked(outputs);
    if (ret != ACAMERA_OK) {
        ALOGE("Fail to create new session. cannot configure streams");
        return ret;
    }

    ACameraCaptureSession* newSession = new ACameraCaptureSession(
            mNextSessionId++, outputs, callbacks, this);

    bool configureSucceeded = (ret == ACAMERA_OK);

    // set new session as current session
    newSession->incStrong((void *) ACameraDevice_createCaptureSession);
    mCurrentSession = newSession;
    *session = newSession;
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::captureLocked(
        sp<ACameraCaptureSession> session,
        /*optional*/ACameraCaptureSession_captureCallbacks* cbs,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) {
    return submitRequestsLocked(
            session, cbs, numRequests, requests, captureSequenceId, /*isRepeating*/false);
}

camera_status_t
CameraDevice::setRepeatingRequestsLocked(
        sp<ACameraCaptureSession> session,
        /*optional*/ACameraCaptureSession_captureCallbacks* cbs,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId) {
    return submitRequestsLocked(
            session, cbs, numRequests, requests, captureSequenceId, /*isRepeating*/true);
}

camera_status_t
CameraDevice::submitRequestsLocked(
        sp<ACameraCaptureSession> session,
        /*optional*/ACameraCaptureSession_captureCallbacks* cbs,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId,
        bool isRepeating) {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera %s submit capture request failed! ret %d", getId(), ret);
        return ret;
    }

    // Form two vectors of capture request, one for internal tracking
    std::vector<hardware::camera2::CaptureRequest> requestList;
    Vector<sp<CaptureRequest> > requestsV;
    requestsV.setCapacity(numRequests);
    for (int i = 0; i < numRequests; i++) {
        sp<CaptureRequest> req;
        ret = allocateCaptureRequest(requests[i], req);
        if (ret != ACAMERA_OK) {
            ALOGE("Convert capture request to internal format failure! ret %d", ret);
            return ret;
        }
        if (req->mSurfaceList.empty()) {
            ALOGE("Capture request without output target cannot be submitted!");
            return ACAMERA_ERROR_INVALID_PARAMETER;
        }
        requestList.push_back(*(req.get()));
        requestsV.push_back(req);
    }

    if (isRepeating) {
        ret = stopRepeatingLocked();
        if (ret != ACAMERA_OK) {
            ALOGE("Camera %s stop repeating failed! ret %d", getId(), ret);
            return ret;
        }
    }

    binder::Status remoteRet;
    hardware::camera2::utils::SubmitInfo info;
    remoteRet = mRemote->submitRequestList(requestList, isRepeating, &info);
    int sequenceId = info.mRequestId;
    int64_t lastFrameNumber = info.mLastFrameNumber;
    if (sequenceId < 0) {
        ALOGE("Camera %s submit request remote failure: ret %d", getId(), sequenceId);
        return ACAMERA_ERROR_UNKNOWN;
    }

    CallbackHolder cbHolder(session, requestsV, isRepeating, cbs);
    mSequenceCallbackMap.insert(std::make_pair(sequenceId, cbHolder));

    if (isRepeating) {
        // stopRepeating above should have cleanup repeating sequence id
        if (mRepeatingSequenceId != REQUEST_ID_NONE) {
            setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
            return ACAMERA_ERROR_CAMERA_DEVICE;
        }
        mRepeatingSequenceId = sequenceId;
    } else {
        mSequenceLastFrameNumberMap.insert(std::make_pair(sequenceId, lastFrameNumber));
    }

    if (mIdle) {
        sp<AMessage> msg = new AMessage(kWhatSessionStateCb, mHandler);
        msg->setPointer(kContextKey, session->mUserSessionCallback.context);
        msg->setObject(kSessionSpKey, session);
        msg->setPointer(kCallbackFpKey, (void*) session->mUserSessionCallback.onActive);
        msg->post();
    }
    mIdle = false;
    mBusySession = session;

    if (captureSequenceId) {
        *captureSequenceId = sequenceId;
    }
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::allocateCaptureRequest(
        const ACaptureRequest* request, /*out*/sp<CaptureRequest>& outReq) {
    camera_status_t ret;
    sp<CaptureRequest> req(new CaptureRequest());
    req->mMetadata = request->settings->mData;
    req->mIsReprocess = false; // NDK does not support reprocessing yet

    for (auto outputTarget : request->targets->mOutputs) {
        ANativeWindow* anw = outputTarget.mWindow;
        sp<Surface> surface;
        ret = getSurfaceFromANativeWindow(anw, surface);
        if (ret != ACAMERA_OK) {
            ALOGE("Bad output target in capture request! ret %d", ret);
            return ret;
        }
        req->mSurfaceList.push_back(surface);
    }
    outReq = req;
    return ACAMERA_OK;
}

ACaptureRequest*
CameraDevice::allocateACaptureRequest(sp<CaptureRequest>& req) {
    ACaptureRequest* pRequest = new ACaptureRequest();
    CameraMetadata clone = req->mMetadata;
    pRequest->settings = new ACameraMetadata(clone.release(), ACameraMetadata::ACM_REQUEST);
    pRequest->targets  = new ACameraOutputTargets();
    for (size_t i = 0; i < req->mSurfaceList.size(); i++) {
        ANativeWindow* anw = static_cast<ANativeWindow*>(req->mSurfaceList[i].get());
        ACameraOutputTarget outputTarget(anw);
        pRequest->targets->mOutputs.insert(outputTarget);
    }
    return pRequest;
}

void
CameraDevice::freeACaptureRequest(ACaptureRequest* req) {
    if (req == nullptr) {
        return;
    }
    delete req->settings;
    delete req->targets;
    delete req;
}

void
CameraDevice::notifySessionEndOfLifeLocked(ACameraCaptureSession* session) {
    if (isClosed()) {
        // Device is closing already. do nothing
        return;
    }

    if (session != mCurrentSession) {
        // Session has been replaced by other seesion or device is closed
        return;
    }
    mCurrentSession = nullptr;

    // Should not happen
    if (!session->mIsClosed) {
        ALOGE("Error: unclosed session %p reaches end of life!", session);
        setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
        return;
    }

    // No new session, unconfigure now
    camera_status_t ret = configureStreamsLocked(nullptr);
    if (ret != ACAMERA_OK) {
        ALOGE("Unconfigure stream failed. Device might still be configured! ret %d", ret);
    }
}

void
CameraDevice::disconnectLocked() {
    if (mClosing.exchange(true)) {
        // Already closing, just return
        ALOGW("Camera device %s is already closing.", getId());
        return;
    }

    if (mRemote != nullptr) {
        mRemote->disconnect();
    }
    mRemote = nullptr;

    if (mCurrentSession != nullptr) {
        mCurrentSession->closeByDevice();
        mCurrentSession = nullptr;
    }
}

camera_status_t
CameraDevice::stopRepeatingLocked() {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera %s stop repeating failed! ret %d", getId(), ret);
        return ret;
    }
    if (mRepeatingSequenceId != REQUEST_ID_NONE) {
        int repeatingSequenceId = mRepeatingSequenceId;
        mRepeatingSequenceId = REQUEST_ID_NONE;

        int64_t lastFrameNumber;
        binder::Status remoteRet = mRemote->cancelRequest(repeatingSequenceId, &lastFrameNumber);
        if (!remoteRet.isOk()) {
            ALOGE("Stop repeating request fails in remote: %s", remoteRet.toString8().string());
            return ACAMERA_ERROR_UNKNOWN;
        }
        checkRepeatingSequenceCompleteLocked(repeatingSequenceId, lastFrameNumber);
    }
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::waitUntilIdleLocked() {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Wait until camera %s idle failed! ret %d", getId(), ret);
        return ret;
    }

    if (mRepeatingSequenceId != REQUEST_ID_NONE) {
        ALOGE("Camera device %s won't go to idle when there is repeating request!", getId());
        return ACAMERA_ERROR_INVALID_OPERATION;
    }

    binder::Status remoteRet = mRemote->waitUntilIdle();
    if (!remoteRet.isOk()) {
        ALOGE("Camera device %s waitUntilIdle failed: %s", getId(), remoteRet.toString8().string());
        // TODO: define a function to convert status_t -> camera_status_t
        return ACAMERA_ERROR_UNKNOWN;
    }

    return ACAMERA_OK;
}

camera_status_t
CameraDevice::getIGBPfromSessionOutput(
        const ACaptureSessionOutput& config,
        sp<IGraphicBufferProducer>& out) {
    ANativeWindow* anw = config.mWindow;
    if (anw == nullptr) {
        ALOGE("Error: output ANativeWindow is null");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    int value;
    int err = (*anw->query)(anw, NATIVE_WINDOW_CONCRETE_TYPE, &value);
    if (value != NATIVE_WINDOW_SURFACE) {
        ALOGE("Error: ANativeWindow is not backed by Surface!");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    const sp<Surface> surface(static_cast<Surface*>(anw));
    out = surface->getIGraphicBufferProducer();
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::getSurfaceFromANativeWindow(
        ANativeWindow* anw, sp<Surface>& out) {
    if (anw == nullptr) {
        ALOGE("Error: output ANativeWindow is null");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    int value;
    int err = (*anw->query)(anw, NATIVE_WINDOW_CONCRETE_TYPE, &value);
    if (value != NATIVE_WINDOW_SURFACE) {
        ALOGE("Error: ANativeWindow is not backed by Surface!");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    sp<Surface> surface(static_cast<Surface*>(anw));
    out = surface;
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::configureStreamsLocked(const ACaptureSessionOutputContainer* outputs) {
    ACaptureSessionOutputContainer emptyOutput;
    if (outputs == nullptr) {
        outputs = &emptyOutput;
    }

    bool success = false;
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    std::set<OutputConfiguration> outputSet;
    for (auto outConfig : outputs->mOutputs) {
        sp<IGraphicBufferProducer> iGBP(nullptr);
        ret = getIGBPfromSessionOutput(outConfig, iGBP);
        if (ret != ACAMERA_OK) {
            return ret;
        }
        outputSet.insert(OutputConfiguration(iGBP, outConfig.mRotation));
    }
    std::set<OutputConfiguration> addSet = outputSet;
    std::vector<int> deleteList;

    // Determine which streams need to be created, which to be deleted
    for (auto& kvPair : mConfiguredOutputs) {
        int streamId = kvPair.first;
        OutputConfiguration& outConfig = kvPair.second;
        if (outputSet.count(outConfig) == 0) {
            deleteList.push_back(streamId); // Need to delete a no longer needed stream
        } else {
            addSet.erase(outConfig);        // No need to add already existing stream
        }
    }

    ret = stopRepeatingLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera device %s stop repeating failed, ret %d", getId(), ret);
        return ret;
    }

    ret = waitUntilIdleLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera device %s wait until idle failed, ret %d", getId(), ret);
        return ret;
    }

    // Send onReady to previous session
    // CurrentSession will be updated after configureStreamLocked, so here
    // mCurrentSession is the session to be replaced by a new session
    if (!mIdle && mCurrentSession != nullptr) {
        if (mBusySession != mCurrentSession) {
            ALOGE("Current session != busy session");
            setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
            return ACAMERA_ERROR_CAMERA_DEVICE;
        }
        sp<AMessage> msg = new AMessage(kWhatSessionStateCb, mHandler);
        msg->setPointer(kContextKey, mBusySession->mUserSessionCallback.context);
        msg->setObject(kSessionSpKey, mBusySession);
        msg->setPointer(kCallbackFpKey, (void*) mBusySession->mUserSessionCallback.onReady);
        mBusySession.clear();
        msg->post();
    }
    mIdle = true;

    binder::Status remoteRet = mRemote->beginConfigure();
    if (!remoteRet.isOk()) {
        ALOGE("Camera device %s begin configure failed: %s", getId(), remoteRet.toString8().string());
        return ACAMERA_ERROR_UNKNOWN;
    }

    // delete to-be-deleted streams
    for (auto streamId : deleteList) {
        remoteRet = mRemote->deleteStream(streamId);
        if (!remoteRet.isOk()) {
            ALOGE("Camera device %s failed to remove stream %d: %s", getId(), streamId,
                    remoteRet.toString8().string());
            return ACAMERA_ERROR_UNKNOWN;
        }
        mConfiguredOutputs.erase(streamId);
    }

    // add new streams
    for (auto outConfig : addSet) {
        int streamId;
        remoteRet = mRemote->createStream(outConfig, &streamId);
        if (!remoteRet.isOk()) {
            ALOGE("Camera device %s failed to create stream: %s", getId(),
                    remoteRet.toString8().string());
            return ACAMERA_ERROR_UNKNOWN;
        }
        mConfiguredOutputs.insert(std::make_pair(streamId, outConfig));
    }

    remoteRet = mRemote->endConfigure(/*isConstrainedHighSpeed*/ false);
    if (remoteRet.serviceSpecificErrorCode() == hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT) {
        ALOGE("Camera device %s cannnot support app output configuration: %s", getId(),
                remoteRet.toString8().string());
        return ACAMERA_ERROR_STREAM_CONFIGURE_FAIL;
    } else if (!remoteRet.isOk()) {
        ALOGE("Camera device %s end configure failed: %s", getId(), remoteRet.toString8().string());
        return ACAMERA_ERROR_UNKNOWN;
    }

    return ACAMERA_OK;
}

void
CameraDevice::setRemoteDevice(sp<hardware::camera2::ICameraDeviceUser> remote) {
    Mutex::Autolock _l(mDeviceLock);
    mRemote = remote;
}

camera_status_t
CameraDevice::checkCameraClosedOrErrorLocked() const {
    if (mRemote == nullptr) {
        ALOGE("%s: camera device already closed", __FUNCTION__);
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    if (mInError) {// triggered by onDeviceError
        ALOGE("%s: camera device has encountered a serious error", __FUNCTION__);
        return mError;
    }
    return ACAMERA_OK;
}

void
CameraDevice::setCameraDeviceErrorLocked(camera_status_t error) {
    mInError = true;
    mError = error;
    return;
}

void
CameraDevice::FrameNumberTracker::updateTracker(int64_t frameNumber, bool isError) {
    ALOGV("updateTracker frame %" PRId64 " isError %d", frameNumber, isError);
    if (isError) {
        mFutureErrorSet.insert(frameNumber);
    } else if (frameNumber <= mCompletedFrameNumber) {
        ALOGE("Frame number %" PRId64 " decreased! current fn %" PRId64,
                frameNumber, mCompletedFrameNumber);
        return;
    } else {
        if (frameNumber != mCompletedFrameNumber + 1) {
            ALOGE("Frame number out of order. Expect %" PRId64 " but get %" PRId64,
                    mCompletedFrameNumber + 1, frameNumber);
            // Do not assert as in java implementation
        }
        mCompletedFrameNumber = frameNumber;
    }
    update();
}

void
CameraDevice::FrameNumberTracker::update() {
    for (auto it = mFutureErrorSet.begin(); it != mFutureErrorSet.end();) {
        int64_t errorFrameNumber = *it;
        if (errorFrameNumber == mCompletedFrameNumber + 1) {
            mCompletedFrameNumber++;
            it = mFutureErrorSet.erase(it);
        } else if (errorFrameNumber <= mCompletedFrameNumber) {
            // This should not happen, but deal with it anyway
            ALOGE("Completd frame number passed through current frame number!");
            // erase the old error since it's no longer useful
            it = mFutureErrorSet.erase(it);
        } else {
            // Normal requests hasn't catched up error frames, just break
            break;
        }
    }
    ALOGV("Update complete frame %" PRId64, mCompletedFrameNumber);
}

void
CameraDevice::onCaptureErrorLocked(
        int32_t errorCode,
        const CaptureResultExtras& resultExtras) {
    int sequenceId = resultExtras.requestId;
    int64_t frameNumber = resultExtras.frameNumber;
    int32_t burstId = resultExtras.burstId;

    // No way to report buffer error now
    if (errorCode == hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_BUFFER) {
        ALOGE("Camera %s Lost output buffer for frame %" PRId64,
                getId(), frameNumber);
        return;
    }
    // Fire capture failure callback if there is one registered
    auto it = mSequenceCallbackMap.find(sequenceId);
    if (it != mSequenceCallbackMap.end()) {
        CallbackHolder cbh = (*it).second;
        ACameraCaptureSession_captureCallback_failed onError = cbh.mCallbacks.onCaptureFailed;
        sp<ACameraCaptureSession> session = cbh.mSession;
        if ((size_t) burstId >= cbh.mRequests.size()) {
            ALOGE("%s: Error: request index %d out of bound (size %zu)",
                    __FUNCTION__, burstId, cbh.mRequests.size());
            setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
            return;
        }
        sp<CaptureRequest> request = cbh.mRequests[burstId];
        sp<CameraCaptureFailure> failure(new CameraCaptureFailure());
        failure->frameNumber = frameNumber;
        // TODO: refine this when implementing flush
        failure->reason      = CAPTURE_FAILURE_REASON_ERROR;
        failure->sequenceId  = sequenceId;
        failure->wasImageCaptured = (errorCode ==
                hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_RESULT);

        sp<AMessage> msg = new AMessage(kWhatCaptureFail, mHandler);
        msg->setPointer(kContextKey, cbh.mCallbacks.context);
        msg->setObject(kSessionSpKey, session);
        msg->setPointer(kCallbackFpKey, (void*) onError);
        msg->setObject(kCaptureRequestKey, request);
        msg->setObject(kCaptureFailureKey, failure);
        msg->post();
    }

    // Update tracker
    mFrameNumberTracker.updateTracker(frameNumber, /*isError*/true);
    checkAndFireSequenceCompleteLocked();
}

void CameraDevice::CallbackHandler::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatOnDisconnected:
        case kWhatOnError:
        case kWhatSessionStateCb:
        case kWhatCaptureStart:
        case kWhatCaptureResult:
        case kWhatCaptureFail:
        case kWhatCaptureSeqEnd:
        case kWhatCaptureSeqAbort:
            ALOGV("%s: Received msg %d", __FUNCTION__, msg->what());
            break;
        default:
            ALOGE("%s:Error: unknown device callback %d", __FUNCTION__, msg->what());
            return;
    }
    // Check the common part of all message
    void* context;
    bool found = msg->findPointer(kContextKey, &context);
    if (!found) {
        ALOGE("%s: Cannot find callback context!", __FUNCTION__);
        return;
    }
    switch (msg->what()) {
        case kWhatOnDisconnected:
        {
            ACameraDevice* dev;
            found = msg->findPointer(kDeviceKey, (void**) &dev);
            if (!found || dev == nullptr) {
                ALOGE("%s: Cannot find device pointer!", __FUNCTION__);
                return;
            }
            ACameraDevice_StateCallback onDisconnected;
            found = msg->findPointer(kCallbackFpKey, (void**) &onDisconnected);
            if (!found) {
                ALOGE("%s: Cannot find onDisconnected!", __FUNCTION__);
                return;
            }
            if (onDisconnected == nullptr) {
                return;
            }
            (*onDisconnected)(context, dev);
            break;
        }
        case kWhatOnError:
        {
            ACameraDevice* dev;
            found = msg->findPointer(kDeviceKey, (void**) &dev);
            if (!found || dev == nullptr) {
                ALOGE("%s: Cannot find device pointer!", __FUNCTION__);
                return;
            }
            ACameraDevice_ErrorStateCallback onError;
            found = msg->findPointer(kCallbackFpKey, (void**) &onError);
            if (!found) {
                ALOGE("%s: Cannot find onError!", __FUNCTION__);
                return;
            }
            int errorCode;
            found = msg->findInt32(kErrorCodeKey, &errorCode);
            if (!found) {
                ALOGE("%s: Cannot find error code!", __FUNCTION__);
                return;
            }
            if (onError == nullptr) {
                return;
            }
            (*onError)(context, dev, errorCode);
            break;
        }
        case kWhatSessionStateCb:
        case kWhatCaptureStart:
        case kWhatCaptureResult:
        case kWhatCaptureFail:
        case kWhatCaptureSeqEnd:
        case kWhatCaptureSeqAbort:
        {
            sp<RefBase> obj;
            found = msg->findObject(kSessionSpKey, &obj);
            if (!found || obj == nullptr) {
                ALOGE("%s: Cannot find session pointer!", __FUNCTION__);
                return;
            }
            sp<ACameraCaptureSession> session(static_cast<ACameraCaptureSession*>(obj.get()));
            sp<CaptureRequest> requestSp = nullptr;
            switch (msg->what()) {
                case kWhatCaptureStart:
                case kWhatCaptureResult:
                case kWhatCaptureFail:
                    found = msg->findObject(kCaptureRequestKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture request!", __FUNCTION__);
                        return;
                    }
                    requestSp = static_cast<CaptureRequest*>(obj.get());
                    break;
            }

            switch (msg->what()) {
                case kWhatSessionStateCb:
                {
                    ACameraCaptureSession_stateCallback onState;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onState);
                    if (!found) {
                        ALOGE("%s: Cannot find state callback!", __FUNCTION__);
                        return;
                    }
                    if (onState == nullptr) {
                        return;
                    }
                    (*onState)(context, session.get());
                    break;
                }
                case kWhatCaptureStart:
                {
                    ACameraCaptureSession_captureCallback_start onStart;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onStart);
                    if (!found) {
                        ALOGE("%s: Cannot find capture start callback!", __FUNCTION__);
                        return;
                    }
                    if (onStart == nullptr) {
                        return;
                    }
                    int64_t timestamp;
                    found = msg->findInt64(kTimeStampKey, &timestamp);
                    if (!found) {
                        ALOGE("%s: Cannot find timestamp!", __FUNCTION__);
                        return;
                    }
                    ACaptureRequest* request = allocateACaptureRequest(requestSp);
                    (*onStart)(context, session.get(), request, timestamp);
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatCaptureResult:
                {
                    ACameraCaptureSession_captureCallback_result onResult;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onResult);
                    if (!found) {
                        ALOGE("%s: Cannot find capture result callback!", __FUNCTION__);
                        return;
                    }
                    if (onResult == nullptr) {
                        return;
                    }

                    found = msg->findObject(kCaptureResultKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture result!", __FUNCTION__);
                        return;
                    }
                    sp<ACameraMetadata> result(static_cast<ACameraMetadata*>(obj.get()));
                    ACaptureRequest* request = allocateACaptureRequest(requestSp);
                    (*onResult)(context, session.get(), request, result.get());
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatCaptureFail:
                {
                    ACameraCaptureSession_captureCallback_failed onFail;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onFail);
                    if (!found) {
                        ALOGE("%s: Cannot find capture fail callback!", __FUNCTION__);
                        return;
                    }
                    if (onFail == nullptr) {
                        return;
                    }

                    found = msg->findObject(kCaptureFailureKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture failure!", __FUNCTION__);
                        return;
                    }
                    sp<CameraCaptureFailure> failureSp(
                            static_cast<CameraCaptureFailure*>(obj.get()));
                    ACameraCaptureFailure* failure =
                            static_cast<ACameraCaptureFailure*>(failureSp.get());
                    ACaptureRequest* request = allocateACaptureRequest(requestSp);
                    (*onFail)(context, session.get(), request, failure);
                    freeACaptureRequest(request);
                    delete failure;
                    break;
                }
                case kWhatCaptureSeqEnd:
                {
                    ACameraCaptureSession_captureCallback_sequenceEnd onSeqEnd;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onSeqEnd);
                    if (!found) {
                        ALOGE("%s: Cannot find sequence end callback!", __FUNCTION__);
                        return;
                    }
                    if (onSeqEnd == nullptr) {
                        return;
                    }
                    int seqId;
                    found = msg->findInt32(kSequenceIdKey, &seqId);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }
                    int64_t frameNumber;
                    found = msg->findInt64(kFrameNumberKey, &frameNumber);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }
                    (*onSeqEnd)(context, session.get(), seqId, frameNumber);
                    break;
                }
                case kWhatCaptureSeqAbort:
                {
                    ACameraCaptureSession_captureCallback_sequenceAbort onSeqAbort;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onSeqAbort);
                    if (!found) {
                        ALOGE("%s: Cannot find sequence end callback!", __FUNCTION__);
                        return;
                    }
                    if (onSeqAbort == nullptr) {
                        return;
                    }
                    int seqId;
                    found = msg->findInt32(kSequenceIdKey, &seqId);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }
                    (*onSeqAbort)(context, session.get(), seqId);
                    break;
                }
            }
            break;
        }
    }
}

CameraDevice::CallbackHolder::CallbackHolder(
    sp<ACameraCaptureSession>          session,
    const Vector<sp<CaptureRequest> >& requests,
    bool                               isRepeating,
    ACameraCaptureSession_captureCallbacks* cbs) :
    mSession(session), mRequests(requests),
    mIsRepeating(isRepeating), mCallbacks(fillCb(cbs)) {}

void
CameraDevice::checkRepeatingSequenceCompleteLocked(
    const int sequenceId, const int64_t lastFrameNumber) {
    ALOGV("Repeating seqId %d lastFrameNumer %" PRId64, sequenceId, lastFrameNumber);
    if (lastFrameNumber == NO_FRAMES_CAPTURED) {
        if (mSequenceCallbackMap.count(sequenceId) == 0) {
            ALOGW("No callback found for sequenceId %d", sequenceId);
            return;
        }
        // remove callback holder from callback map
        auto cbIt = mSequenceCallbackMap.find(sequenceId);
        CallbackHolder cbh = cbIt->second;
        mSequenceCallbackMap.erase(cbIt);
        // send seq aborted callback
        sp<AMessage> msg = new AMessage(kWhatCaptureSeqAbort, mHandler);
        msg->setPointer(kContextKey, cbh.mCallbacks.context);
        msg->setObject(kSessionSpKey, cbh.mSession);
        msg->setPointer(kCallbackFpKey, (void*) cbh.mCallbacks.onCaptureSequenceAborted);
        msg->setInt32(kSequenceIdKey, sequenceId);
        msg->post();
    } else {
        // Use mSequenceLastFrameNumberMap to track
        mSequenceLastFrameNumberMap.insert(std::make_pair(sequenceId, lastFrameNumber));

        // Last frame might have arrived. Check now
        checkAndFireSequenceCompleteLocked();
    }
}

void
CameraDevice::checkAndFireSequenceCompleteLocked() {
    int64_t completedFrameNumber = mFrameNumberTracker.getCompletedFrameNumber();
    //std::map<int, int64_t> mSequenceLastFrameNumberMap;
    auto it = mSequenceLastFrameNumberMap.begin();
    while (it != mSequenceLastFrameNumberMap.end()) {
        int sequenceId = it->first;
        int64_t lastFrameNumber = it->second;
        bool seqCompleted = false;
        bool hasCallback  = true;

        if (mRemote == nullptr) {
            ALOGW("Camera %s closed while checking sequence complete", getId());
            return;
        }

        // Check if there is callback for this sequence
        // This should not happen because we always register callback (with nullptr inside)
        if (mSequenceCallbackMap.count(sequenceId) == 0) {
            ALOGW("No callback found for sequenceId %d", sequenceId);
            hasCallback = false;
        }

        if (lastFrameNumber <= completedFrameNumber) {
            ALOGV("seq %d reached last frame %" PRId64 ", completed %" PRId64,
                    sequenceId, lastFrameNumber, completedFrameNumber);
            seqCompleted = true;
        }

        if (seqCompleted && hasCallback) {
            // remove callback holder from callback map
            auto cbIt = mSequenceCallbackMap.find(sequenceId);
            CallbackHolder cbh = cbIt->second;
            mSequenceCallbackMap.erase(cbIt);
            // send seq complete callback
            sp<AMessage> msg = new AMessage(kWhatCaptureSeqEnd, mHandler);
            msg->setPointer(kContextKey, cbh.mCallbacks.context);
            msg->setObject(kSessionSpKey, cbh.mSession);
            msg->setPointer(kCallbackFpKey, (void*) cbh.mCallbacks.onCaptureSequenceCompleted);
            msg->setInt32(kSequenceIdKey, sequenceId);
            msg->setInt64(kFrameNumberKey, lastFrameNumber);

            // Clear the session sp before we send out the message
            // This will guarantee the rare case where the message is processed
            // before cbh goes out of scope and causing we call the session
            // destructor while holding device lock
            cbh.mSession.clear();
            msg->post();
        }

        // No need to track sequence complete if there is no callback registered
        if (seqCompleted || !hasCallback) {
            it = mSequenceLastFrameNumberMap.erase(it);
        } else {
            ++it;
        }
    }
}

/**
  * Camera service callback implementation
  */
binder::Status
CameraDevice::ServiceCallback::onDeviceError(
        int32_t errorCode,
        const CaptureResultExtras& resultExtras) {
    ALOGD("Device error received, code %d, frame number %" PRId64 ", request ID %d, subseq ID %d",
            errorCode, resultExtras.frameNumber, resultExtras.requestId, resultExtras.burstId);
    binder::Status ret = binder::Status::ok();
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->mRemote == nullptr) {
        return ret; // device has been closed
    }
    switch (errorCode) {
        case ERROR_CAMERA_DISCONNECTED:
        {
            // Camera is disconnected, close the session and expect no more callbacks
            if (dev->mCurrentSession != nullptr) {
                dev->mCurrentSession->closeByDevice();
                dev->mCurrentSession = nullptr;
            }
            sp<AMessage> msg = new AMessage(kWhatOnDisconnected, dev->mHandler);
            msg->setPointer(kContextKey, dev->mAppCallbacks.context);
            msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
            msg->setPointer(kCallbackFpKey, (void*) dev->mAppCallbacks.onDisconnected);
            msg->post();
            break;
        }
        default:
            ALOGE("Unknown error from camera device: %d", errorCode);
            // no break
        case ERROR_CAMERA_DEVICE:
        case ERROR_CAMERA_SERVICE:
        {
            switch (errorCode) {
                case ERROR_CAMERA_DEVICE:
                    dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
                    break;
                case ERROR_CAMERA_SERVICE:
                    dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
                    break;
                default:
                    dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_UNKNOWN);
                    break;
            }
            sp<AMessage> msg = new AMessage(kWhatOnError, dev->mHandler);
            msg->setPointer(kContextKey, dev->mAppCallbacks.context);
            msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
            msg->setPointer(kCallbackFpKey, (void*) dev->mAppCallbacks.onError);
            msg->setInt32(kErrorCodeKey, errorCode);
            msg->post();
            break;
        }
        case ERROR_CAMERA_REQUEST:
        case ERROR_CAMERA_RESULT:
        case ERROR_CAMERA_BUFFER:
            dev->onCaptureErrorLocked(errorCode, resultExtras);
            break;
    }
    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onDeviceIdle() {
    ALOGV("Camera is now idle");
    binder::Status ret = binder::Status::ok();
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->isClosed() || dev->mRemote == nullptr) {
        return ret;
    }

    if (dev->mIdle) {
        // Already in idle state. Possibly other thread did waitUntilIdle
        return ret;
    }

    if (dev->mCurrentSession != nullptr) {
        ALOGE("onDeviceIdle sending state cb");
        if (dev->mBusySession != dev->mCurrentSession) {
            ALOGE("Current session != busy session");
            dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
            return ret;
        }
        sp<AMessage> msg = new AMessage(kWhatSessionStateCb, dev->mHandler);
        msg->setPointer(kContextKey, dev->mBusySession->mUserSessionCallback.context);
        msg->setObject(kSessionSpKey, dev->mBusySession);
        msg->setPointer(kCallbackFpKey, (void*) dev->mBusySession->mUserSessionCallback.onReady);
        // Make sure we clear the sp first so the session destructor can
        // only happen on handler thread (where we don't hold device/session lock)
        dev->mBusySession.clear();
        msg->post();
    }
    dev->mIdle = true;
    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onCaptureStarted(
        const CaptureResultExtras& resultExtras,
        int64_t timestamp) {
    binder::Status ret = binder::Status::ok();

    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }
    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->isClosed() || dev->mRemote == nullptr) {
        return ret;
    }

    int sequenceId = resultExtras.requestId;
    int64_t frameNumber = resultExtras.frameNumber;
    int32_t burstId = resultExtras.burstId;

    auto it = dev->mSequenceCallbackMap.find(sequenceId);
    if (it != dev->mSequenceCallbackMap.end()) {
        CallbackHolder cbh = (*it).second;
        ACameraCaptureSession_captureCallback_start onStart = cbh.mCallbacks.onCaptureStarted;
        sp<ACameraCaptureSession> session = cbh.mSession;
        if ((size_t) burstId >= cbh.mRequests.size()) {
            ALOGE("%s: Error: request index %d out of bound (size %zu)",
                    __FUNCTION__, burstId, cbh.mRequests.size());
            dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
        }
        sp<CaptureRequest> request = cbh.mRequests[burstId];
        sp<AMessage> msg = new AMessage(kWhatCaptureStart, dev->mHandler);
        msg->setPointer(kContextKey, cbh.mCallbacks.context);
        msg->setObject(kSessionSpKey, session);
        msg->setPointer(kCallbackFpKey, (void*) onStart);
        msg->setObject(kCaptureRequestKey, request);
        msg->setInt64(kTimeStampKey, timestamp);
        msg->post();
    }
    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onResultReceived(
        const CameraMetadata& metadata,
        const CaptureResultExtras& resultExtras) {
    binder::Status ret = binder::Status::ok();

    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }
    int sequenceId = resultExtras.requestId;
    int64_t frameNumber = resultExtras.frameNumber;
    int32_t burstId = resultExtras.burstId;
    bool    isPartialResult = (resultExtras.partialResultCount < dev->mPartialResultCount);

    if (!isPartialResult) {
        ALOGV("SeqId %d frame %" PRId64 " result arrive.", sequenceId, frameNumber);
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->mRemote == nullptr) {
        return ret; // device has been disconnected
    }

    if (dev->isClosed()) {
        if (!isPartialResult) {
            dev->mFrameNumberTracker.updateTracker(frameNumber, /*isError*/false);
        }
        // early return to avoid callback sent to closed devices
        return ret;
    }

    CameraMetadata metadataCopy = metadata;
    // Copied from java implmentation. Why do we need this?
    metadataCopy.update(ANDROID_LENS_INFO_SHADING_MAP_SIZE, dev->mShadingMapSize, /*data_count*/2);

    auto it = dev->mSequenceCallbackMap.find(sequenceId);
    if (it != dev->mSequenceCallbackMap.end()) {
        CallbackHolder cbh = (*it).second;
        ACameraCaptureSession_captureCallback_result onResult = isPartialResult ?
                cbh.mCallbacks.onCaptureProgressed :
                cbh.mCallbacks.onCaptureCompleted;
        sp<ACameraCaptureSession> session = cbh.mSession;
        if ((size_t) burstId >= cbh.mRequests.size()) {
            ALOGE("%s: Error: request index %d out of bound (size %zu)",
                    __FUNCTION__, burstId, cbh.mRequests.size());
            dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
        }
        sp<CaptureRequest> request = cbh.mRequests[burstId];
        sp<ACameraMetadata> result(new ACameraMetadata(
                metadataCopy.release(), ACameraMetadata::ACM_RESULT));

        sp<AMessage> msg = new AMessage(kWhatCaptureResult, dev->mHandler);
        msg->setPointer(kContextKey, cbh.mCallbacks.context);
        msg->setObject(kSessionSpKey, session);
        msg->setPointer(kCallbackFpKey, (void*) onResult);
        msg->setObject(kCaptureRequestKey, request);
        msg->setObject(kCaptureResultKey, result);
        msg->post();
    }

    if (!isPartialResult) {
        dev->mFrameNumberTracker.updateTracker(frameNumber, /*isError*/false);
        dev->checkAndFireSequenceCompleteLocked();
    }

    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onPrepared(int) {
    // Prepare not yet implemented in NDK
    return binder::Status::ok();
}

} // namespace android
