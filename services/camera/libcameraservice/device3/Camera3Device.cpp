/*
 * Copyright (C) 2013 The Android Open Source Project
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

#define LOG_TAG "Camera3-Device"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0  // Per-frame verbose logging

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

// Convenience macro for transient errors
#define CLOGE(fmt, ...) ALOGE("Camera %d: %s: " fmt, mId, __FUNCTION__, \
            ##__VA_ARGS__)

// Convenience macros for transitioning to the error state
#define SET_ERR(fmt, ...) setErrorState(   \
    "%s: " fmt, __FUNCTION__,              \
    ##__VA_ARGS__)
#define SET_ERR_L(fmt, ...) setErrorStateLocked( \
    "%s: " fmt, __FUNCTION__,                    \
    ##__VA_ARGS__)

#include <inttypes.h>

#include <utils/Log.h>
#include <utils/Trace.h>
#include <utils/Timers.h>

#include "utils/CameraTraces.h"
#include "device3/Camera3Device.h"
#include "device3/Camera3OutputStream.h"
#include "device3/Camera3InputStream.h"
#include "device3/Camera3ZslStream.h"
#include "CameraService.h"

using namespace android::camera3;

namespace android {

Camera3Device::Camera3Device(int id):
        mId(id),
        mHal3Device(NULL),
        mStatus(STATUS_UNINITIALIZED),
        mUsePartialResult(false),
        mNumPartialResults(1),
        mNextResultFrameNumber(0),
        mNextShutterFrameNumber(0),
        mListener(NULL)
{
    ATRACE_CALL();
    camera3_callback_ops::notify = &sNotify;
    camera3_callback_ops::process_capture_result = &sProcessCaptureResult;
    ALOGV("%s: Created device for camera %d", __FUNCTION__, id);
}

Camera3Device::~Camera3Device()
{
    ATRACE_CALL();
    ALOGV("%s: Tearing down for camera id %d", __FUNCTION__, mId);
    disconnect();
}

int Camera3Device::getId() const {
    return mId;
}

/**
 * CameraDeviceBase interface
 */

status_t Camera3Device::initialize(camera_module_t *module)
{
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    ALOGV("%s: Initializing device for camera %d", __FUNCTION__, mId);
    if (mStatus != STATUS_UNINITIALIZED) {
        CLOGE("Already initialized!");
        return INVALID_OPERATION;
    }

    /** Open HAL device */

    status_t res;
    String8 deviceName = String8::format("%d", mId);

    camera3_device_t *device;

    ATRACE_BEGIN("camera3->open");
    res = CameraService::filterOpenErrorCode(module->common.methods->open(
        &module->common, deviceName.string(),
        reinterpret_cast<hw_device_t**>(&device)));
    ATRACE_END();

    if (res != OK) {
        SET_ERR_L("Could not open camera: %s (%d)", strerror(-res), res);
        return res;
    }

    /** Cross-check device version */
    if (device->common.version < CAMERA_DEVICE_API_VERSION_3_0) {
        SET_ERR_L("Could not open camera: "
                "Camera device should be at least %x, reports %x instead",
                CAMERA_DEVICE_API_VERSION_3_0,
                device->common.version);
        device->common.close(&device->common);
        return BAD_VALUE;
    }

    camera_info info;
    res = CameraService::filterGetInfoErrorCode(module->get_camera_info(
        mId, &info));
    if (res != OK) return res;

    if (info.device_version != device->common.version) {
        SET_ERR_L("HAL reporting mismatched camera_info version (%x)"
                " and device version (%x).",
                info.device_version, device->common.version);
        device->common.close(&device->common);
        return BAD_VALUE;
    }

    /** Initialize device with callback functions */

    ATRACE_BEGIN("camera3->initialize");
    res = device->ops->initialize(device, this);
    ATRACE_END();

    if (res != OK) {
        SET_ERR_L("Unable to initialize HAL device: %s (%d)",
                strerror(-res), res);
        device->common.close(&device->common);
        return BAD_VALUE;
    }

    /** Start up status tracker thread */
    mStatusTracker = new StatusTracker(this);
    res = mStatusTracker->run(String8::format("C3Dev-%d-Status", mId).string());
    if (res != OK) {
        SET_ERR_L("Unable to start status tracking thread: %s (%d)",
                strerror(-res), res);
        device->common.close(&device->common);
        mStatusTracker.clear();
        return res;
    }

    /** Start up request queue thread */

    mRequestThread = new RequestThread(this, mStatusTracker, device);
    res = mRequestThread->run(String8::format("C3Dev-%d-ReqQueue", mId).string());
    if (res != OK) {
        SET_ERR_L("Unable to start request queue thread: %s (%d)",
                strerror(-res), res);
        device->common.close(&device->common);
        mRequestThread.clear();
        return res;
    }

    /** Everything is good to go */

    mDeviceVersion = device->common.version;
    mDeviceInfo = info.static_camera_characteristics;
    mHal3Device = device;
    mStatus = STATUS_UNCONFIGURED;
    mNextStreamId = 0;
    mNeedConfig = true;
    mPauseStateNotify = false;

    // Will the HAL be sending in early partial result metadata?
    if (mDeviceVersion >= CAMERA_DEVICE_API_VERSION_3_2) {
        camera_metadata_entry partialResultsCount =
                mDeviceInfo.find(ANDROID_REQUEST_PARTIAL_RESULT_COUNT);
        if (partialResultsCount.count > 0) {
            mNumPartialResults = partialResultsCount.data.i32[0];
            mUsePartialResult = (mNumPartialResults > 1);
        }
    } else {
        camera_metadata_entry partialResultsQuirk =
                mDeviceInfo.find(ANDROID_QUIRKS_USE_PARTIAL_RESULT);
        if (partialResultsQuirk.count > 0 && partialResultsQuirk.data.u8[0] == 1) {
            mUsePartialResult = true;
        }
    }

    return OK;
}

status_t Camera3Device::disconnect() {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);

    ALOGV("%s: E", __FUNCTION__);

    status_t res = OK;

    {
        Mutex::Autolock l(mLock);
        if (mStatus == STATUS_UNINITIALIZED) return res;

        if (mStatus == STATUS_ACTIVE ||
                (mStatus == STATUS_ERROR && mRequestThread != NULL)) {
            res = mRequestThread->clearRepeatingRequests();
            if (res != OK) {
                SET_ERR_L("Can't stop streaming");
                // Continue to close device even in case of error
            } else {
                res = waitUntilStateThenRelock(/*active*/ false, kShutdownTimeout);
                if (res != OK) {
                    SET_ERR_L("Timeout waiting for HAL to drain");
                    // Continue to close device even in case of error
                }
            }
        }

        if (mStatus == STATUS_ERROR) {
            CLOGE("Shutting down in an error state");
        }

        if (mStatusTracker != NULL) {
            mStatusTracker->requestExit();
        }

        if (mRequestThread != NULL) {
            mRequestThread->requestExit();
        }

        mOutputStreams.clear();
        mInputStream.clear();
    }

    // Joining done without holding mLock, otherwise deadlocks may ensue
    // as the threads try to access parent state
    if (mRequestThread != NULL && mStatus != STATUS_ERROR) {
        // HAL may be in a bad state, so waiting for request thread
        // (which may be stuck in the HAL processCaptureRequest call)
        // could be dangerous.
        mRequestThread->join();
    }

    if (mStatusTracker != NULL) {
        mStatusTracker->join();
    }

    {
        Mutex::Autolock l(mLock);

        mRequestThread.clear();
        mStatusTracker.clear();

        if (mHal3Device != NULL) {
            ATRACE_BEGIN("camera3->close");
            mHal3Device->common.close(&mHal3Device->common);
            ATRACE_END();
            mHal3Device = NULL;
        }

        mStatus = STATUS_UNINITIALIZED;
    }

    ALOGV("%s: X", __FUNCTION__);
    return res;
}

// For dumping/debugging only -
// try to acquire a lock a few times, eventually give up to proceed with
// debug/dump operations
bool Camera3Device::tryLockSpinRightRound(Mutex& lock) {
    bool gotLock = false;
    for (size_t i = 0; i < kDumpLockAttempts; ++i) {
        if (lock.tryLock() == NO_ERROR) {
            gotLock = true;
            break;
        } else {
            usleep(kDumpSleepDuration);
        }
    }
    return gotLock;
}

Camera3Device::Size Camera3Device::getMaxJpegResolution() const {
    int32_t maxJpegWidth = 0, maxJpegHeight = 0;
    if (mDeviceVersion >= CAMERA_DEVICE_API_VERSION_3_2) {
        const int STREAM_CONFIGURATION_SIZE = 4;
        const int STREAM_FORMAT_OFFSET = 0;
        const int STREAM_WIDTH_OFFSET = 1;
        const int STREAM_HEIGHT_OFFSET = 2;
        const int STREAM_IS_INPUT_OFFSET = 3;
        camera_metadata_ro_entry_t availableStreamConfigs =
                mDeviceInfo.find(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
        if (availableStreamConfigs.count == 0 ||
                availableStreamConfigs.count % STREAM_CONFIGURATION_SIZE != 0) {
            return Size(0, 0);
        }

        // Get max jpeg size (area-wise).
        for (size_t i=0; i < availableStreamConfigs.count; i+= STREAM_CONFIGURATION_SIZE) {
            int32_t format = availableStreamConfigs.data.i32[i + STREAM_FORMAT_OFFSET];
            int32_t width = availableStreamConfigs.data.i32[i + STREAM_WIDTH_OFFSET];
            int32_t height = availableStreamConfigs.data.i32[i + STREAM_HEIGHT_OFFSET];
            int32_t isInput = availableStreamConfigs.data.i32[i + STREAM_IS_INPUT_OFFSET];
            if (isInput == ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT
                    && format == HAL_PIXEL_FORMAT_BLOB &&
                    (width * height > maxJpegWidth * maxJpegHeight)) {
                maxJpegWidth = width;
                maxJpegHeight = height;
            }
        }
    } else {
        camera_metadata_ro_entry availableJpegSizes =
                mDeviceInfo.find(ANDROID_SCALER_AVAILABLE_JPEG_SIZES);
        if (availableJpegSizes.count == 0 || availableJpegSizes.count % 2 != 0) {
            return Size(0, 0);
        }

        // Get max jpeg size (area-wise).
        for (size_t i = 0; i < availableJpegSizes.count; i += 2) {
            if ((availableJpegSizes.data.i32[i] * availableJpegSizes.data.i32[i + 1])
                    > (maxJpegWidth * maxJpegHeight)) {
                maxJpegWidth = availableJpegSizes.data.i32[i];
                maxJpegHeight = availableJpegSizes.data.i32[i + 1];
            }
        }
    }
    return Size(maxJpegWidth, maxJpegHeight);
}

ssize_t Camera3Device::getJpegBufferSize(uint32_t width, uint32_t height) const {
    // Get max jpeg size (area-wise).
    Size maxJpegResolution = getMaxJpegResolution();
    if (maxJpegResolution.width == 0) {
        ALOGE("%s: Camera %d: Can't find find valid available jpeg sizes in static metadata!",
                __FUNCTION__, mId);
        return BAD_VALUE;
    }

    // Get max jpeg buffer size
    ssize_t maxJpegBufferSize = 0;
    camera_metadata_ro_entry jpegBufMaxSize = mDeviceInfo.find(ANDROID_JPEG_MAX_SIZE);
    if (jpegBufMaxSize.count == 0) {
        ALOGE("%s: Camera %d: Can't find maximum JPEG size in static metadata!", __FUNCTION__, mId);
        return BAD_VALUE;
    }
    maxJpegBufferSize = jpegBufMaxSize.data.i32[0];

    // Calculate final jpeg buffer size for the given resolution.
    float scaleFactor = ((float) (width * height)) /
            (maxJpegResolution.width * maxJpegResolution.height);
    ssize_t jpegBufferSize = scaleFactor * maxJpegBufferSize;
    // Bound the buffer size to [MIN_JPEG_BUFFER_SIZE, maxJpegBufferSize].
    if (jpegBufferSize > maxJpegBufferSize) {
        jpegBufferSize = maxJpegBufferSize;
    } else if (jpegBufferSize < kMinJpegBufferSize) {
        jpegBufferSize = kMinJpegBufferSize;
    }

    return jpegBufferSize;
}

status_t Camera3Device::dump(int fd, const Vector<String16> &args) {
    ATRACE_CALL();
    (void)args;

    // Try to lock, but continue in case of failure (to avoid blocking in
    // deadlocks)
    bool gotInterfaceLock = tryLockSpinRightRound(mInterfaceLock);
    bool gotLock = tryLockSpinRightRound(mLock);

    ALOGW_IF(!gotInterfaceLock,
            "Camera %d: %s: Unable to lock interface lock, proceeding anyway",
            mId, __FUNCTION__);
    ALOGW_IF(!gotLock,
            "Camera %d: %s: Unable to lock main lock, proceeding anyway",
            mId, __FUNCTION__);

    String8 lines;

    const char *status =
            mStatus == STATUS_ERROR         ? "ERROR" :
            mStatus == STATUS_UNINITIALIZED ? "UNINITIALIZED" :
            mStatus == STATUS_UNCONFIGURED  ? "UNCONFIGURED" :
            mStatus == STATUS_CONFIGURED    ? "CONFIGURED" :
            mStatus == STATUS_ACTIVE        ? "ACTIVE" :
            "Unknown";

    lines.appendFormat("    Device status: %s\n", status);
    if (mStatus == STATUS_ERROR) {
        lines.appendFormat("    Error cause: %s\n", mErrorCause.string());
    }
    lines.appendFormat("    Stream configuration:\n");

    if (mInputStream != NULL) {
        write(fd, lines.string(), lines.size());
        mInputStream->dump(fd, args);
    } else {
        lines.appendFormat("      No input stream.\n");
        write(fd, lines.string(), lines.size());
    }
    for (size_t i = 0; i < mOutputStreams.size(); i++) {
        mOutputStreams[i]->dump(fd,args);
    }

    lines = String8("    In-flight requests:\n");
    if (mInFlightMap.size() == 0) {
        lines.append("      None\n");
    } else {
        for (size_t i = 0; i < mInFlightMap.size(); i++) {
            InFlightRequest r = mInFlightMap.valueAt(i);
            lines.appendFormat("      Frame %d |  Timestamp: %" PRId64 ", metadata"
                    " arrived: %s, buffers left: %d\n", mInFlightMap.keyAt(i),
                    r.captureTimestamp, r.haveResultMetadata ? "true" : "false",
                    r.numBuffersLeft);
        }
    }
    write(fd, lines.string(), lines.size());

    {
        lines = String8("    Last request sent:\n");
        write(fd, lines.string(), lines.size());

        CameraMetadata lastRequest = getLatestRequestLocked();
        lastRequest.dump(fd, /*verbosity*/2, /*indentation*/6);
    }

    if (mHal3Device != NULL) {
        lines = String8("    HAL device dump:\n");
        write(fd, lines.string(), lines.size());
        mHal3Device->ops->dump(mHal3Device, fd);
    }

    if (gotLock) mLock.unlock();
    if (gotInterfaceLock) mInterfaceLock.unlock();

    return OK;
}

const CameraMetadata& Camera3Device::info() const {
    ALOGVV("%s: E", __FUNCTION__);
    if (CC_UNLIKELY(mStatus == STATUS_UNINITIALIZED ||
                    mStatus == STATUS_ERROR)) {
        ALOGW("%s: Access to static info %s!", __FUNCTION__,
                mStatus == STATUS_ERROR ?
                "when in error state" : "before init");
    }
    return mDeviceInfo;
}

status_t Camera3Device::checkStatusOkToCaptureLocked() {
    switch (mStatus) {
        case STATUS_ERROR:
            CLOGE("Device has encountered a serious error");
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            CLOGE("Device not initialized");
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }
    return OK;
}

status_t Camera3Device::convertMetadataListToRequestListLocked(
        const List<const CameraMetadata> &metadataList, RequestList *requestList) {
    if (requestList == NULL) {
        CLOGE("requestList cannot be NULL.");
        return BAD_VALUE;
    }

    int32_t burstId = 0;
    for (List<const CameraMetadata>::const_iterator it = metadataList.begin();
            it != metadataList.end(); ++it) {
        sp<CaptureRequest> newRequest = setUpRequestLocked(*it);
        if (newRequest == 0) {
            CLOGE("Can't create capture request");
            return BAD_VALUE;
        }

        // Setup burst Id and request Id
        newRequest->mResultExtras.burstId = burstId++;
        if (it->exists(ANDROID_REQUEST_ID)) {
            if (it->find(ANDROID_REQUEST_ID).count == 0) {
                CLOGE("RequestID entry exists; but must not be empty in metadata");
                return BAD_VALUE;
            }
            newRequest->mResultExtras.requestId = it->find(ANDROID_REQUEST_ID).data.i32[0];
        } else {
            CLOGE("RequestID does not exist in metadata");
            return BAD_VALUE;
        }

        requestList->push_back(newRequest);

        ALOGV("%s: requestId = %" PRId32, __FUNCTION__, newRequest->mResultExtras.requestId);
    }
    return OK;
}

status_t Camera3Device::capture(CameraMetadata &request, int64_t* /*lastFrameNumber*/) {
    ATRACE_CALL();

    List<const CameraMetadata> requests;
    requests.push_back(request);
    return captureList(requests, /*lastFrameNumber*/NULL);
}

status_t Camera3Device::submitRequestsHelper(
        const List<const CameraMetadata> &requests, bool repeating,
        /*out*/
        int64_t *lastFrameNumber) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    status_t res = checkStatusOkToCaptureLocked();
    if (res != OK) {
        // error logged by previous call
        return res;
    }

    RequestList requestList;

    res = convertMetadataListToRequestListLocked(requests, /*out*/&requestList);
    if (res != OK) {
        // error logged by previous call
        return res;
    }

    if (repeating) {
        res = mRequestThread->setRepeatingRequests(requestList, lastFrameNumber);
    } else {
        res = mRequestThread->queueRequestList(requestList, lastFrameNumber);
    }

    if (res == OK) {
        waitUntilStateThenRelock(/*active*/true, kActiveTimeout);
        if (res != OK) {
            SET_ERR_L("Can't transition to active in %f seconds!",
                    kActiveTimeout/1e9);
        }
        ALOGV("Camera %d: Capture request %" PRId32 " enqueued", mId,
              (*(requestList.begin()))->mResultExtras.requestId);
    } else {
        CLOGE("Cannot queue request. Impossible.");
        return BAD_VALUE;
    }

    return res;
}

status_t Camera3Device::captureList(const List<const CameraMetadata> &requests,
                                    int64_t *lastFrameNumber) {
    ATRACE_CALL();

    return submitRequestsHelper(requests, /*repeating*/false, lastFrameNumber);
}

status_t Camera3Device::setStreamingRequest(const CameraMetadata &request,
                                            int64_t* /*lastFrameNumber*/) {
    ATRACE_CALL();

    List<const CameraMetadata> requests;
    requests.push_back(request);
    return setStreamingRequestList(requests, /*lastFrameNumber*/NULL);
}

status_t Camera3Device::setStreamingRequestList(const List<const CameraMetadata> &requests,
                                                int64_t *lastFrameNumber) {
    ATRACE_CALL();

    return submitRequestsHelper(requests, /*repeating*/true, lastFrameNumber);
}

sp<Camera3Device::CaptureRequest> Camera3Device::setUpRequestLocked(
        const CameraMetadata &request) {
    status_t res;

    if (mStatus == STATUS_UNCONFIGURED || mNeedConfig) {
        res = configureStreamsLocked();
        if (res != OK) {
            SET_ERR_L("Can't set up streams: %s (%d)", strerror(-res), res);
            return NULL;
        }
        if (mStatus == STATUS_UNCONFIGURED) {
            CLOGE("No streams configured");
            return NULL;
        }
    }

    sp<CaptureRequest> newRequest = createCaptureRequest(request);
    return newRequest;
}

status_t Camera3Device::clearStreamingRequest(int64_t *lastFrameNumber) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            CLOGE("Device has encountered a serious error");
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            CLOGE("Device not initialized");
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }
    ALOGV("Camera %d: Clearing repeating request", mId);

    return mRequestThread->clearRepeatingRequests(lastFrameNumber);
}

status_t Camera3Device::waitUntilRequestReceived(int32_t requestId, nsecs_t timeout) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);

    return mRequestThread->waitUntilRequestProcessed(requestId, timeout);
}

status_t Camera3Device::createInputStream(
        uint32_t width, uint32_t height, int format, int *id) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);
    ALOGV("Camera %d: Creating new input stream %d: %d x %d, format %d",
            mId, mNextStreamId, width, height, format);

    status_t res;
    bool wasActive = false;

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
            // OK
            break;
        case STATUS_ACTIVE:
            ALOGV("%s: Stopping activity to reconfigure streams", __FUNCTION__);
            res = internalPauseAndWaitLocked();
            if (res != OK) {
                SET_ERR_L("Can't pause captures to reconfigure streams!");
                return res;
            }
            wasActive = true;
            break;
        default:
            SET_ERR_L("%s: Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }
    assert(mStatus != STATUS_ACTIVE);

    if (mInputStream != 0) {
        ALOGE("%s: Cannot create more than 1 input stream", __FUNCTION__);
        return INVALID_OPERATION;
    }

    sp<Camera3InputStream> newStream = new Camera3InputStream(mNextStreamId,
                width, height, format);
    newStream->setStatusTracker(mStatusTracker);

    mInputStream = newStream;

    *id = mNextStreamId++;

    // Continue captures if active at start
    if (wasActive) {
        ALOGV("%s: Restarting activity to reconfigure streams", __FUNCTION__);
        res = configureStreamsLocked();
        if (res != OK) {
            ALOGE("%s: Can't reconfigure device for new stream %d: %s (%d)",
                    __FUNCTION__, mNextStreamId, strerror(-res), res);
            return res;
        }
        internalResumeLocked();
    }

    ALOGV("Camera %d: Created input stream", mId);
    return OK;
}


status_t Camera3Device::createZslStream(
            uint32_t width, uint32_t height,
            int depth,
            /*out*/
            int *id,
            sp<Camera3ZslStream>* zslStream) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);
    ALOGV("Camera %d: Creating ZSL stream %d: %d x %d, depth %d",
            mId, mNextStreamId, width, height, depth);

    status_t res;
    bool wasActive = false;

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
            // OK
            break;
        case STATUS_ACTIVE:
            ALOGV("%s: Stopping activity to reconfigure streams", __FUNCTION__);
            res = internalPauseAndWaitLocked();
            if (res != OK) {
                SET_ERR_L("Can't pause captures to reconfigure streams!");
                return res;
            }
            wasActive = true;
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }
    assert(mStatus != STATUS_ACTIVE);

    if (mInputStream != 0) {
        ALOGE("%s: Cannot create more than 1 input stream", __FUNCTION__);
        return INVALID_OPERATION;
    }

    sp<Camera3ZslStream> newStream = new Camera3ZslStream(mNextStreamId,
                width, height, depth);
    newStream->setStatusTracker(mStatusTracker);

    res = mOutputStreams.add(mNextStreamId, newStream);
    if (res < 0) {
        ALOGE("%s: Can't add new stream to set: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }
    mInputStream = newStream;

    mNeedConfig = true;

    *id = mNextStreamId++;
    *zslStream = newStream;

    // Continue captures if active at start
    if (wasActive) {
        ALOGV("%s: Restarting activity to reconfigure streams", __FUNCTION__);
        res = configureStreamsLocked();
        if (res != OK) {
            ALOGE("%s: Can't reconfigure device for new stream %d: %s (%d)",
                    __FUNCTION__, mNextStreamId, strerror(-res), res);
            return res;
        }
        internalResumeLocked();
    }

    ALOGV("Camera %d: Created ZSL stream", mId);
    return OK;
}

status_t Camera3Device::createStream(sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, int format, int *id) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);
    ALOGV("Camera %d: Creating new stream %d: %d x %d, format %d",
            mId, mNextStreamId, width, height, format);

    status_t res;
    bool wasActive = false;

    switch (mStatus) {
        case STATUS_ERROR:
            CLOGE("Device has encountered a serious error");
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            CLOGE("Device not initialized");
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
            // OK
            break;
        case STATUS_ACTIVE:
            ALOGV("%s: Stopping activity to reconfigure streams", __FUNCTION__);
            res = internalPauseAndWaitLocked();
            if (res != OK) {
                SET_ERR_L("Can't pause captures to reconfigure streams!");
                return res;
            }
            wasActive = true;
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }
    assert(mStatus != STATUS_ACTIVE);

    sp<Camera3OutputStream> newStream;
    if (format == HAL_PIXEL_FORMAT_BLOB) {
        ssize_t jpegBufferSize = getJpegBufferSize(width, height);
        if (jpegBufferSize <= 0) {
            SET_ERR_L("Invalid jpeg buffer size %zd", jpegBufferSize);
            return BAD_VALUE;
        }

        newStream = new Camera3OutputStream(mNextStreamId, consumer,
                width, height, jpegBufferSize, format);
    } else {
        newStream = new Camera3OutputStream(mNextStreamId, consumer,
                width, height, format);
    }
    newStream->setStatusTracker(mStatusTracker);

    res = mOutputStreams.add(mNextStreamId, newStream);
    if (res < 0) {
        SET_ERR_L("Can't add new stream to set: %s (%d)", strerror(-res), res);
        return res;
    }

    *id = mNextStreamId++;
    mNeedConfig = true;

    // Continue captures if active at start
    if (wasActive) {
        ALOGV("%s: Restarting activity to reconfigure streams", __FUNCTION__);
        res = configureStreamsLocked();
        if (res != OK) {
            CLOGE("Can't reconfigure device for new stream %d: %s (%d)",
                    mNextStreamId, strerror(-res), res);
            return res;
        }
        internalResumeLocked();
    }
    ALOGV("Camera %d: Created new stream", mId);
    return OK;
}

status_t Camera3Device::createReprocessStreamFromStream(int outputId, int *id) {
    ATRACE_CALL();
    (void)outputId; (void)id;

    CLOGE("Unimplemented");
    return INVALID_OPERATION;
}


status_t Camera3Device::getStreamInfo(int id,
        uint32_t *width, uint32_t *height, uint32_t *format) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            CLOGE("Device has encountered a serious error");
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            CLOGE("Device not initialized!");
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }

    ssize_t idx = mOutputStreams.indexOfKey(id);
    if (idx == NAME_NOT_FOUND) {
        CLOGE("Stream %d is unknown", id);
        return idx;
    }

    if (width) *width  = mOutputStreams[idx]->getWidth();
    if (height) *height = mOutputStreams[idx]->getHeight();
    if (format) *format = mOutputStreams[idx]->getFormat();

    return OK;
}

status_t Camera3Device::setStreamTransform(int id,
        int transform) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            CLOGE("Device has encountered a serious error");
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            CLOGE("Device not initialized");
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }

    ssize_t idx = mOutputStreams.indexOfKey(id);
    if (idx == NAME_NOT_FOUND) {
        CLOGE("Stream %d does not exist",
                id);
        return BAD_VALUE;
    }

    return mOutputStreams.editValueAt(idx)->setTransform(transform);
}

status_t Camera3Device::deleteStream(int id) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);
    status_t res;

    ALOGV("%s: Camera %d: Deleting stream %d", __FUNCTION__, mId, id);

    // CameraDevice semantics require device to already be idle before
    // deleteStream is called, unlike for createStream.
    if (mStatus == STATUS_ACTIVE) {
        ALOGV("%s: Camera %d: Device not idle", __FUNCTION__, mId);
        return -EBUSY;
    }

    sp<Camera3StreamInterface> deletedStream;
    ssize_t outputStreamIdx = mOutputStreams.indexOfKey(id);
    if (mInputStream != NULL && id == mInputStream->getId()) {
        deletedStream = mInputStream;
        mInputStream.clear();
    } else {
        if (outputStreamIdx == NAME_NOT_FOUND) {
            CLOGE("Stream %d does not exist", id);
            return BAD_VALUE;
        }
    }

    // Delete output stream or the output part of a bi-directional stream.
    if (outputStreamIdx != NAME_NOT_FOUND) {
        deletedStream = mOutputStreams.editValueAt(outputStreamIdx);
        mOutputStreams.removeItem(id);
    }

    // Free up the stream endpoint so that it can be used by some other stream
    res = deletedStream->disconnect();
    if (res != OK) {
        SET_ERR_L("Can't disconnect deleted stream %d", id);
        // fall through since we want to still list the stream as deleted.
    }
    mDeletedStreams.add(deletedStream);
    mNeedConfig = true;

    return res;
}

status_t Camera3Device::deleteReprocessStream(int id) {
    ATRACE_CALL();
    (void)id;

    CLOGE("Unimplemented");
    return INVALID_OPERATION;
}


status_t Camera3Device::createDefaultRequest(int templateId,
        CameraMetadata *request) {
    ATRACE_CALL();
    ALOGV("%s: for template %d", __FUNCTION__, templateId);
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            CLOGE("Device has encountered a serious error");
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            CLOGE("Device is not initialized!");
            return INVALID_OPERATION;
        case STATUS_UNCONFIGURED:
        case STATUS_CONFIGURED:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            SET_ERR_L("Unexpected status: %d", mStatus);
            return INVALID_OPERATION;
    }

    const camera_metadata_t *rawRequest;
    ATRACE_BEGIN("camera3->construct_default_request_settings");
    rawRequest = mHal3Device->ops->construct_default_request_settings(
        mHal3Device, templateId);
    ATRACE_END();
    if (rawRequest == NULL) {
        SET_ERR_L("HAL is unable to construct default settings for template %d",
                templateId);
        return DEAD_OBJECT;
    }
    *request = rawRequest;

    return OK;
}

status_t Camera3Device::waitUntilDrained() {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    Mutex::Autolock l(mLock);

    return waitUntilDrainedLocked();
}

status_t Camera3Device::waitUntilDrainedLocked() {
    switch (mStatus) {
        case STATUS_UNINITIALIZED:
        case STATUS_UNCONFIGURED:
            ALOGV("%s: Already idle", __FUNCTION__);
            return OK;
        case STATUS_CONFIGURED:
            // To avoid race conditions, check with tracker to be sure
        case STATUS_ERROR:
        case STATUS_ACTIVE:
            // Need to verify shut down
            break;
        default:
            SET_ERR_L("Unexpected status: %d",mStatus);
            return INVALID_OPERATION;
    }

    ALOGV("%s: Camera %d: Waiting until idle", __FUNCTION__, mId);
    status_t res = waitUntilStateThenRelock(/*active*/ false, kShutdownTimeout);
    return res;
}

// Pause to reconfigure
status_t Camera3Device::internalPauseAndWaitLocked() {
    mRequestThread->setPaused(true);
    mPauseStateNotify = true;

    ALOGV("%s: Camera %d: Internal wait until idle", __FUNCTION__, mId);
    status_t res = waitUntilStateThenRelock(/*active*/ false, kShutdownTimeout);
    if (res != OK) {
        SET_ERR_L("Can't idle device in %f seconds!",
                kShutdownTimeout/1e9);
    }

    return res;
}

// Resume after internalPauseAndWaitLocked
status_t Camera3Device::internalResumeLocked() {
    status_t res;

    mRequestThread->setPaused(false);

    res = waitUntilStateThenRelock(/*active*/ true, kActiveTimeout);
    if (res != OK) {
        SET_ERR_L("Can't transition to active in %f seconds!",
                kActiveTimeout/1e9);
    }
    mPauseStateNotify = false;
    return OK;
}

status_t Camera3Device::waitUntilStateThenRelock(bool active,
        nsecs_t timeout) {
    status_t res = OK;
    if (active == (mStatus == STATUS_ACTIVE)) {
        // Desired state already reached
        return res;
    }

    bool stateSeen = false;
    do {
        mRecentStatusUpdates.clear();

        res = mStatusChanged.waitRelative(mLock, timeout);
        if (res != OK) break;

        // Check state change history during wait
        for (size_t i = 0; i < mRecentStatusUpdates.size(); i++) {
            if (active == (mRecentStatusUpdates[i] == STATUS_ACTIVE) ) {
                stateSeen = true;
                break;
            }
        }
    } while (!stateSeen);

    return res;
}


status_t Camera3Device::setNotifyCallback(NotificationListener *listener) {
    ATRACE_CALL();
    Mutex::Autolock l(mOutputLock);

    if (listener != NULL && mListener != NULL) {
        ALOGW("%s: Replacing old callback listener", __FUNCTION__);
    }
    mListener = listener;

    return OK;
}

bool Camera3Device::willNotify3A() {
    return false;
}

status_t Camera3Device::waitForNextFrame(nsecs_t timeout) {
    status_t res;
    Mutex::Autolock l(mOutputLock);

    while (mResultQueue.empty()) {
        res = mResultSignal.waitRelative(mOutputLock, timeout);
        if (res == TIMED_OUT) {
            return res;
        } else if (res != OK) {
            ALOGW("%s: Camera %d: No frame in %" PRId64 " ns: %s (%d)",
                    __FUNCTION__, mId, timeout, strerror(-res), res);
            return res;
        }
    }
    return OK;
}

status_t Camera3Device::getNextResult(CaptureResult *frame) {
    ATRACE_CALL();
    Mutex::Autolock l(mOutputLock);

    if (mResultQueue.empty()) {
        return NOT_ENOUGH_DATA;
    }

    if (frame == NULL) {
        ALOGE("%s: argument cannot be NULL", __FUNCTION__);
        return BAD_VALUE;
    }

    CaptureResult &result = *(mResultQueue.begin());
    frame->mResultExtras = result.mResultExtras;
    frame->mMetadata.acquire(result.mMetadata);
    mResultQueue.erase(mResultQueue.begin());

    return OK;
}

status_t Camera3Device::triggerAutofocus(uint32_t id) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);

    ALOGV("%s: Triggering autofocus, id %d", __FUNCTION__, id);
    // Mix-in this trigger into the next request and only the next request.
    RequestTrigger trigger[] = {
        {
            ANDROID_CONTROL_AF_TRIGGER,
            ANDROID_CONTROL_AF_TRIGGER_START
        },
        {
            ANDROID_CONTROL_AF_TRIGGER_ID,
            static_cast<int32_t>(id)
        }
    };

    return mRequestThread->queueTrigger(trigger,
                                        sizeof(trigger)/sizeof(trigger[0]));
}

status_t Camera3Device::triggerCancelAutofocus(uint32_t id) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);

    ALOGV("%s: Triggering cancel autofocus, id %d", __FUNCTION__, id);
    // Mix-in this trigger into the next request and only the next request.
    RequestTrigger trigger[] = {
        {
            ANDROID_CONTROL_AF_TRIGGER,
            ANDROID_CONTROL_AF_TRIGGER_CANCEL
        },
        {
            ANDROID_CONTROL_AF_TRIGGER_ID,
            static_cast<int32_t>(id)
        }
    };

    return mRequestThread->queueTrigger(trigger,
                                        sizeof(trigger)/sizeof(trigger[0]));
}

status_t Camera3Device::triggerPrecaptureMetering(uint32_t id) {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);

    ALOGV("%s: Triggering precapture metering, id %d", __FUNCTION__, id);
    // Mix-in this trigger into the next request and only the next request.
    RequestTrigger trigger[] = {
        {
            ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
            ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START
        },
        {
            ANDROID_CONTROL_AE_PRECAPTURE_ID,
            static_cast<int32_t>(id)
        }
    };

    return mRequestThread->queueTrigger(trigger,
                                        sizeof(trigger)/sizeof(trigger[0]));
}

status_t Camera3Device::pushReprocessBuffer(int reprocessStreamId,
        buffer_handle_t *buffer, wp<BufferReleasedListener> listener) {
    ATRACE_CALL();
    (void)reprocessStreamId; (void)buffer; (void)listener;

    CLOGE("Unimplemented");
    return INVALID_OPERATION;
}

status_t Camera3Device::flush(int64_t *frameNumber) {
    ATRACE_CALL();
    ALOGV("%s: Camera %d: Flushing all requests", __FUNCTION__, mId);
    Mutex::Autolock il(mInterfaceLock);

    {
        Mutex::Autolock l(mLock);
        mRequestThread->clear(/*out*/frameNumber);
    }

    status_t res;
    if (mHal3Device->common.version >= CAMERA_DEVICE_API_VERSION_3_1) {
        res = mHal3Device->ops->flush(mHal3Device);
    } else {
        Mutex::Autolock l(mLock);
        res = waitUntilDrainedLocked();
    }

    return res;
}

uint32_t Camera3Device::getDeviceVersion() {
    ATRACE_CALL();
    Mutex::Autolock il(mInterfaceLock);
    return mDeviceVersion;
}

/**
 * Methods called by subclasses
 */

void Camera3Device::notifyStatus(bool idle) {
    {
        // Need mLock to safely update state and synchronize to current
        // state of methods in flight.
        Mutex::Autolock l(mLock);
        // We can get various system-idle notices from the status tracker
        // while starting up. Only care about them if we've actually sent
        // in some requests recently.
        if (mStatus != STATUS_ACTIVE && mStatus != STATUS_CONFIGURED) {
            return;
        }
        ALOGV("%s: Camera %d: Now %s", __FUNCTION__, mId,
                idle ? "idle" : "active");
        mStatus = idle ? STATUS_CONFIGURED : STATUS_ACTIVE;
        mRecentStatusUpdates.add(mStatus);
        mStatusChanged.signal();

        // Skip notifying listener if we're doing some user-transparent
        // state changes
        if (mPauseStateNotify) return;
    }
    NotificationListener *listener;
    {
        Mutex::Autolock l(mOutputLock);
        listener = mListener;
    }
    if (idle && listener != NULL) {
        listener->notifyIdle();
    }
}

/**
 * Camera3Device private methods
 */

sp<Camera3Device::CaptureRequest> Camera3Device::createCaptureRequest(
        const CameraMetadata &request) {
    ATRACE_CALL();
    status_t res;

    sp<CaptureRequest> newRequest = new CaptureRequest;
    newRequest->mSettings = request;

    camera_metadata_entry_t inputStreams =
            newRequest->mSettings.find(ANDROID_REQUEST_INPUT_STREAMS);
    if (inputStreams.count > 0) {
        if (mInputStream == NULL ||
                mInputStream->getId() != inputStreams.data.i32[0]) {
            CLOGE("Request references unknown input stream %d",
                    inputStreams.data.u8[0]);
            return NULL;
        }
        // Lazy completion of stream configuration (allocation/registration)
        // on first use
        if (mInputStream->isConfiguring()) {
            res = mInputStream->finishConfiguration(mHal3Device);
            if (res != OK) {
                SET_ERR_L("Unable to finish configuring input stream %d:"
                        " %s (%d)",
                        mInputStream->getId(), strerror(-res), res);
                return NULL;
            }
        }

        newRequest->mInputStream = mInputStream;
        newRequest->mSettings.erase(ANDROID_REQUEST_INPUT_STREAMS);
    }

    camera_metadata_entry_t streams =
            newRequest->mSettings.find(ANDROID_REQUEST_OUTPUT_STREAMS);
    if (streams.count == 0) {
        CLOGE("Zero output streams specified!");
        return NULL;
    }

    for (size_t i = 0; i < streams.count; i++) {
        int idx = mOutputStreams.indexOfKey(streams.data.i32[i]);
        if (idx == NAME_NOT_FOUND) {
            CLOGE("Request references unknown stream %d",
                    streams.data.u8[i]);
            return NULL;
        }
        sp<Camera3OutputStreamInterface> stream =
                mOutputStreams.editValueAt(idx);

        // Lazy completion of stream configuration (allocation/registration)
        // on first use
        if (stream->isConfiguring()) {
            res = stream->finishConfiguration(mHal3Device);
            if (res != OK) {
                SET_ERR_L("Unable to finish configuring stream %d: %s (%d)",
                        stream->getId(), strerror(-res), res);
                return NULL;
            }
        }

        newRequest->mOutputStreams.push(stream);
    }
    newRequest->mSettings.erase(ANDROID_REQUEST_OUTPUT_STREAMS);

    return newRequest;
}

status_t Camera3Device::configureStreamsLocked() {
    ATRACE_CALL();
    status_t res;

    if (mStatus != STATUS_UNCONFIGURED && mStatus != STATUS_CONFIGURED) {
        CLOGE("Not idle");
        return INVALID_OPERATION;
    }

    if (!mNeedConfig) {
        ALOGV("%s: Skipping config, no stream changes", __FUNCTION__);
        return OK;
    }

    // Start configuring the streams
    ALOGV("%s: Camera %d: Starting stream configuration", __FUNCTION__, mId);

    camera3_stream_configuration config;

    config.num_streams = (mInputStream != NULL) + mOutputStreams.size();

    Vector<camera3_stream_t*> streams;
    streams.setCapacity(config.num_streams);

    if (mInputStream != NULL) {
        camera3_stream_t *inputStream;
        inputStream = mInputStream->startConfiguration();
        if (inputStream == NULL) {
            SET_ERR_L("Can't start input stream configuration");
            return INVALID_OPERATION;
        }
        streams.add(inputStream);
    }

    for (size_t i = 0; i < mOutputStreams.size(); i++) {

        // Don't configure bidi streams twice, nor add them twice to the list
        if (mOutputStreams[i].get() ==
            static_cast<Camera3StreamInterface*>(mInputStream.get())) {

            config.num_streams--;
            continue;
        }

        camera3_stream_t *outputStream;
        outputStream = mOutputStreams.editValueAt(i)->startConfiguration();
        if (outputStream == NULL) {
            SET_ERR_L("Can't start output stream configuration");
            return INVALID_OPERATION;
        }
        streams.add(outputStream);
    }

    config.streams = streams.editArray();

    // Do the HAL configuration; will potentially touch stream
    // max_buffers, usage, priv fields.
    ATRACE_BEGIN("camera3->configure_streams");
    res = mHal3Device->ops->configure_streams(mHal3Device, &config);
    ATRACE_END();

    if (res != OK) {
        SET_ERR_L("Unable to configure streams with HAL: %s (%d)",
                strerror(-res), res);
        return res;
    }

    // Finish all stream configuration immediately.
    // TODO: Try to relax this later back to lazy completion, which should be
    // faster

    if (mInputStream != NULL && mInputStream->isConfiguring()) {
        res = mInputStream->finishConfiguration(mHal3Device);
        if (res != OK) {
            SET_ERR_L("Can't finish configuring input stream %d: %s (%d)",
                    mInputStream->getId(), strerror(-res), res);
            return res;
        }
    }

    for (size_t i = 0; i < mOutputStreams.size(); i++) {
        sp<Camera3OutputStreamInterface> outputStream =
            mOutputStreams.editValueAt(i);
        if (outputStream->isConfiguring()) {
            res = outputStream->finishConfiguration(mHal3Device);
            if (res != OK) {
                SET_ERR_L("Can't finish configuring output stream %d: %s (%d)",
                        outputStream->getId(), strerror(-res), res);
                return res;
            }
        }
    }

    // Request thread needs to know to avoid using repeat-last-settings protocol
    // across configure_streams() calls
    mRequestThread->configurationComplete();

    // Update device state

    mNeedConfig = false;

    if (config.num_streams > 0) {
        mStatus = STATUS_CONFIGURED;
    } else {
        mStatus = STATUS_UNCONFIGURED;
    }

    ALOGV("%s: Camera %d: Stream configuration complete", __FUNCTION__, mId);

    // tear down the deleted streams after configure streams.
    mDeletedStreams.clear();

    return OK;
}

void Camera3Device::setErrorState(const char *fmt, ...) {
    Mutex::Autolock l(mLock);
    va_list args;
    va_start(args, fmt);

    setErrorStateLockedV(fmt, args);

    va_end(args);
}

void Camera3Device::setErrorStateV(const char *fmt, va_list args) {
    Mutex::Autolock l(mLock);
    setErrorStateLockedV(fmt, args);
}

void Camera3Device::setErrorStateLocked(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    setErrorStateLockedV(fmt, args);

    va_end(args);
}

void Camera3Device::setErrorStateLockedV(const char *fmt, va_list args) {
    // Print out all error messages to log
    String8 errorCause = String8::formatV(fmt, args);
    ALOGE("Camera %d: %s", mId, errorCause.string());

    // But only do error state transition steps for the first error
    if (mStatus == STATUS_ERROR || mStatus == STATUS_UNINITIALIZED) return;

    // Save stack trace. View by dumping it later.
    CameraTraces::saveTrace();
    // TODO: consider adding errorCause and client pid/procname

    mErrorCause = errorCause;

    mRequestThread->setPaused(true);
    mStatus = STATUS_ERROR;
}

/**
 * In-flight request management
 */

status_t Camera3Device::registerInFlight(uint32_t frameNumber,
        int32_t numBuffers, CaptureResultExtras resultExtras, bool hasInput) {
    ATRACE_CALL();
    Mutex::Autolock l(mInFlightLock);

    ssize_t res;
    res = mInFlightMap.add(frameNumber, InFlightRequest(numBuffers, resultExtras, hasInput));
    if (res < 0) return res;

    return OK;
}

/**
 * Check if all 3A fields are ready, and send off a partial 3A-only result
 * to the output frame queue
 */
bool Camera3Device::processPartial3AResult(
        uint32_t frameNumber,
        const CameraMetadata& partial, const CaptureResultExtras& resultExtras) {

    // Check if all 3A states are present
    // The full list of fields is
    //   android.control.afMode
    //   android.control.awbMode
    //   android.control.aeState
    //   android.control.awbState
    //   android.control.afState
    //   android.control.afTriggerID
    //   android.control.aePrecaptureID
    // TODO: Add android.control.aeMode

    bool gotAllStates = true;

    uint8_t afMode;
    uint8_t awbMode;
    uint8_t aeState;
    uint8_t afState;
    uint8_t awbState;

    gotAllStates &= get3AResult(partial, ANDROID_CONTROL_AF_MODE,
        &afMode, frameNumber);

    gotAllStates &= get3AResult(partial, ANDROID_CONTROL_AWB_MODE,
        &awbMode, frameNumber);

    gotAllStates &= get3AResult(partial, ANDROID_CONTROL_AE_STATE,
        &aeState, frameNumber);

    gotAllStates &= get3AResult(partial, ANDROID_CONTROL_AF_STATE,
        &afState, frameNumber);

    gotAllStates &= get3AResult(partial, ANDROID_CONTROL_AWB_STATE,
        &awbState, frameNumber);

    if (!gotAllStates) return false;

    ALOGVV("%s: Camera %d: Frame %d, Request ID %d: AF mode %d, AWB mode %d, "
        "AF state %d, AE state %d, AWB state %d, "
        "AF trigger %d, AE precapture trigger %d",
        __FUNCTION__, mId, frameNumber, resultExtras.requestId,
        afMode, awbMode,
        afState, aeState, awbState,
        resultExtras.afTriggerId, resultExtras.precaptureTriggerId);

    // Got all states, so construct a minimal result to send
    // In addition to the above fields, this means adding in
    //   android.request.frameCount
    //   android.request.requestId
    //   android.quirks.partialResult (for HAL version below HAL3.2)

    const size_t kMinimal3AResultEntries = 10;

    Mutex::Autolock l(mOutputLock);

    CaptureResult captureResult;
    captureResult.mResultExtras = resultExtras;
    captureResult.mMetadata = CameraMetadata(kMinimal3AResultEntries, /*dataCapacity*/ 0);
    // TODO: change this to sp<CaptureResult>. This will need other changes, including,
    // but not limited to CameraDeviceBase::getNextResult
    CaptureResult& min3AResult =
            *mResultQueue.insert(mResultQueue.end(), captureResult);

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_REQUEST_FRAME_COUNT,
            // TODO: This is problematic casting. Need to fix CameraMetadata.
            reinterpret_cast<int32_t*>(&frameNumber), frameNumber)) {
        return false;
    }

    int32_t requestId = resultExtras.requestId;
    if (!insert3AResult(min3AResult.mMetadata, ANDROID_REQUEST_ID,
            &requestId, frameNumber)) {
        return false;
    }

    if (mDeviceVersion < CAMERA_DEVICE_API_VERSION_3_2) {
        static const uint8_t partialResult = ANDROID_QUIRKS_PARTIAL_RESULT_PARTIAL;
        if (!insert3AResult(min3AResult.mMetadata, ANDROID_QUIRKS_PARTIAL_RESULT,
                &partialResult, frameNumber)) {
            return false;
        }
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AF_MODE,
            &afMode, frameNumber)) {
        return false;
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AWB_MODE,
            &awbMode, frameNumber)) {
        return false;
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AE_STATE,
            &aeState, frameNumber)) {
        return false;
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AF_STATE,
            &afState, frameNumber)) {
        return false;
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AWB_STATE,
            &awbState, frameNumber)) {
        return false;
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AF_TRIGGER_ID,
            &resultExtras.afTriggerId, frameNumber)) {
        return false;
    }

    if (!insert3AResult(min3AResult.mMetadata, ANDROID_CONTROL_AE_PRECAPTURE_ID,
            &resultExtras.precaptureTriggerId, frameNumber)) {
        return false;
    }

    // We only send the aggregated partial when all 3A related metadata are available
    // For both API1 and API2.
    // TODO: we probably should pass through all partials to API2 unconditionally.
    mResultSignal.signal();

    return true;
}

template<typename T>
bool Camera3Device::get3AResult(const CameraMetadata& result, int32_t tag,
        T* value, uint32_t frameNumber) {
    (void) frameNumber;

    camera_metadata_ro_entry_t entry;

    entry = result.find(tag);
    if (entry.count == 0) {
        ALOGVV("%s: Camera %d: Frame %d: No %s provided by HAL!", __FUNCTION__,
            mId, frameNumber, get_camera_metadata_tag_name(tag));
        return false;
    }

    if (sizeof(T) == sizeof(uint8_t)) {
        *value = entry.data.u8[0];
    } else if (sizeof(T) == sizeof(int32_t)) {
        *value = entry.data.i32[0];
    } else {
        ALOGE("%s: Unexpected type", __FUNCTION__);
        return false;
    }
    return true;
}

template<typename T>
bool Camera3Device::insert3AResult(CameraMetadata& result, int32_t tag,
        const T* value, uint32_t frameNumber) {
    if (result.update(tag, value, 1) != NO_ERROR) {
        mResultQueue.erase(--mResultQueue.end(), mResultQueue.end());
        SET_ERR("Frame %d: Failed to set %s in partial metadata",
                frameNumber, get_camera_metadata_tag_name(tag));
        return false;
    }
    return true;
}

/**
 * Camera HAL device callback methods
 */

void Camera3Device::processCaptureResult(const camera3_capture_result *result) {
    ATRACE_CALL();

    status_t res;

    uint32_t frameNumber = result->frame_number;
    if (result->result == NULL && result->num_output_buffers == 0 &&
            result->input_buffer == NULL) {
        SET_ERR("No result data provided by HAL for frame %d",
                frameNumber);
        return;
    }

    // For HAL3.2 or above, If HAL doesn't support partial, it must always set
    // partial_result to 1 when metadata is included in this result.
    if (!mUsePartialResult &&
            mDeviceVersion >= CAMERA_DEVICE_API_VERSION_3_2 &&
            result->result != NULL &&
            result->partial_result != 1) {
        SET_ERR("Result is malformed for frame %d: partial_result %u must be 1"
                " if partial result is not supported",
                frameNumber, result->partial_result);
        return;
    }

    bool isPartialResult = false;
    CameraMetadata collectedPartialResult;
    CaptureResultExtras resultExtras;
    bool hasInputBufferInRequest = false;

    // Get capture timestamp and resultExtras from list of in-flight requests,
    // where it was added by the shutter notification for this frame.
    // Then update the in-flight status and remove the in-flight entry if
    // all result data has been received.
    nsecs_t timestamp = 0;
    {
        Mutex::Autolock l(mInFlightLock);
        ssize_t idx = mInFlightMap.indexOfKey(frameNumber);
        if (idx == NAME_NOT_FOUND) {
            SET_ERR("Unknown frame number for capture result: %d",
                    frameNumber);
            return;
        }
        InFlightRequest &request = mInFlightMap.editValueAt(idx);
        ALOGVV("%s: got InFlightRequest requestId = %" PRId32 ", frameNumber = %" PRId64
                ", burstId = %" PRId32,
                __FUNCTION__, request.resultExtras.requestId, request.resultExtras.frameNumber,
                request.resultExtras.burstId);
        // Always update the partial count to the latest one. When framework aggregates adjacent
        // partial results into one, the latest partial count will be used.
        request.resultExtras.partialResultCount = result->partial_result;

        // Check if this result carries only partial metadata
        if (mUsePartialResult && result->result != NULL) {
            if (mDeviceVersion >= CAMERA_DEVICE_API_VERSION_3_2) {
                if (result->partial_result > mNumPartialResults || result->partial_result < 1) {
                    SET_ERR("Result is malformed for frame %d: partial_result %u must be  in"
                            " the range of [1, %d] when metadata is included in the result",
                            frameNumber, result->partial_result, mNumPartialResults);
                    return;
                }
                isPartialResult = (result->partial_result < mNumPartialResults);
                if (isPartialResult) {
                    request.partialResult.collectedResult.append(result->result);
                }
            } else {
                camera_metadata_ro_entry_t partialResultEntry;
                res = find_camera_metadata_ro_entry(result->result,
                        ANDROID_QUIRKS_PARTIAL_RESULT, &partialResultEntry);
                if (res != NAME_NOT_FOUND &&
                        partialResultEntry.count > 0 &&
                        partialResultEntry.data.u8[0] ==
                        ANDROID_QUIRKS_PARTIAL_RESULT_PARTIAL) {
                    // A partial result. Flag this as such, and collect this
                    // set of metadata into the in-flight entry.
                    isPartialResult = true;
                    request.partialResult.collectedResult.append(
                        result->result);
                    request.partialResult.collectedResult.erase(
                        ANDROID_QUIRKS_PARTIAL_RESULT);
                }
            }

            if (isPartialResult) {
                // Fire off a 3A-only result if possible
                if (!request.partialResult.haveSent3A) {
                    request.partialResult.haveSent3A =
                            processPartial3AResult(frameNumber,
                                    request.partialResult.collectedResult,
                                    request.resultExtras);
                }
            }
        }

        timestamp = request.captureTimestamp;
        resultExtras = request.resultExtras;
        hasInputBufferInRequest = request.hasInputBuffer;

        /**
         * One of the following must happen before it's legal to call process_capture_result,
         * unless partial metadata is being provided:
         * - CAMERA3_MSG_SHUTTER (expected during normal operation)
         * - CAMERA3_MSG_ERROR (expected during flush)
         */
        if (request.requestStatus == OK && timestamp == 0 && !isPartialResult) {
            SET_ERR("Called before shutter notify for frame %d",
                    frameNumber);
            return;
        }

        // Did we get the (final) result metadata for this capture?
        if (result->result != NULL && !isPartialResult) {
            if (request.haveResultMetadata) {
                SET_ERR("Called multiple times with metadata for frame %d",
                        frameNumber);
                return;
            }
            if (mUsePartialResult &&
                    !request.partialResult.collectedResult.isEmpty()) {
                collectedPartialResult.acquire(
                    request.partialResult.collectedResult);
            }
            request.haveResultMetadata = true;
        }

        uint32_t numBuffersReturned = result->num_output_buffers;
        if (result->input_buffer != NULL) {
            if (hasInputBufferInRequest) {
                numBuffersReturned += 1;
            } else {
                ALOGW("%s: Input buffer should be NULL if there is no input"
                        " buffer sent in the request",
                        __FUNCTION__);
            }
        }
        request.numBuffersLeft -= numBuffersReturned;
        if (request.numBuffersLeft < 0) {
            SET_ERR("Too many buffers returned for frame %d",
                    frameNumber);
            return;
        }

        // Check if everything has arrived for this result (buffers and metadata), remove it from
        // InFlightMap if both arrived or HAL reports error for this request (i.e. during flush).
        if ((request.requestStatus != OK) ||
                (request.haveResultMetadata && request.numBuffersLeft == 0)) {
            ATRACE_ASYNC_END("frame capture", frameNumber);
            mInFlightMap.removeItemsAt(idx, 1);
        }

        // Sanity check - if we have too many in-flight frames, something has
        // likely gone wrong
        if (mInFlightMap.size() > kInFlightWarnLimit) {
            CLOGE("In-flight list too large: %zu", mInFlightMap.size());
        }

    }

    // Process the result metadata, if provided
    bool gotResult = false;
    if (result->result != NULL && !isPartialResult) {
        Mutex::Autolock l(mOutputLock);

        gotResult = true;

        // TODO: need to track errors for tighter bounds on expected frame number
        if (frameNumber < mNextResultFrameNumber) {
            SET_ERR("Out-of-order capture result metadata submitted! "
                    "(got frame number %d, expecting %d)",
                    frameNumber, mNextResultFrameNumber);
            return;
        }
        mNextResultFrameNumber = frameNumber + 1;

        CaptureResult captureResult;
        captureResult.mResultExtras = resultExtras;
        captureResult.mMetadata = result->result;

        if (captureResult.mMetadata.update(ANDROID_REQUEST_FRAME_COUNT,
                (int32_t*)&frameNumber, 1) != OK) {
            SET_ERR("Failed to set frame# in metadata (%d)",
                    frameNumber);
            gotResult = false;
        } else {
            ALOGVV("%s: Camera %d: Set frame# in metadata (%d)",
                    __FUNCTION__, mId, frameNumber);
        }

        // Append any previous partials to form a complete result
        if (mUsePartialResult && !collectedPartialResult.isEmpty()) {
            captureResult.mMetadata.append(collectedPartialResult);
        }

        captureResult.mMetadata.sort();

        // Check that there's a timestamp in the result metadata

        camera_metadata_entry entry =
                captureResult.mMetadata.find(ANDROID_SENSOR_TIMESTAMP);
        if (entry.count == 0) {
            SET_ERR("No timestamp provided by HAL for frame %d!",
                    frameNumber);
            gotResult = false;
        } else if (timestamp != entry.data.i64[0]) {
            SET_ERR("Timestamp mismatch between shutter notify and result"
                    " metadata for frame %d (%" PRId64 " vs %" PRId64 " respectively)",
                    frameNumber, timestamp, entry.data.i64[0]);
            gotResult = false;
        }

        if (gotResult) {
            // Valid result, insert into queue
            List<CaptureResult>::iterator queuedResult =
                    mResultQueue.insert(mResultQueue.end(), CaptureResult(captureResult));
            ALOGVV("%s: result requestId = %" PRId32 ", frameNumber = %" PRId64
                   ", burstId = %" PRId32, __FUNCTION__,
                   queuedResult->mResultExtras.requestId,
                   queuedResult->mResultExtras.frameNumber,
                   queuedResult->mResultExtras.burstId);
        }
    } // scope for mOutputLock

    // Return completed buffers to their streams with the timestamp

    for (size_t i = 0; i < result->num_output_buffers; i++) {
        Camera3Stream *stream =
                Camera3Stream::cast(result->output_buffers[i].stream);
        res = stream->returnBuffer(result->output_buffers[i], timestamp);
        // Note: stream may be deallocated at this point, if this buffer was the
        // last reference to it.
        if (res != OK) {
            ALOGE("Can't return buffer %zu for frame %d to its stream: "
                    " %s (%d)", i, frameNumber, strerror(-res), res);
        }
    }

    if (result->input_buffer != NULL) {
        if (hasInputBufferInRequest) {
            Camera3Stream *stream =
                Camera3Stream::cast(result->input_buffer->stream);
            res = stream->returnInputBuffer(*(result->input_buffer));
            // Note: stream may be deallocated at this point, if this buffer was the
            // last reference to it.
            if (res != OK) {
                ALOGE("%s: RequestThread: Can't return input buffer for frame %d to"
                      "  its stream:%s (%d)",  __FUNCTION__,
                      frameNumber, strerror(-res), res);
            }
        } else {
            ALOGW("%s: Input buffer should be NULL if there is no input"
                    " buffer sent in the request, skipping input buffer return.",
                    __FUNCTION__);
        }
    }

    // Finally, signal any waiters for new frames

    if (gotResult) {
        mResultSignal.signal();
    }

}

void Camera3Device::notify(const camera3_notify_msg *msg) {
    ATRACE_CALL();
    NotificationListener *listener;
    {
        Mutex::Autolock l(mOutputLock);
        listener = mListener;
    }

    if (msg == NULL) {
        SET_ERR("HAL sent NULL notify message!");
        return;
    }

    switch (msg->type) {
        case CAMERA3_MSG_ERROR: {
            int streamId = 0;
            if (msg->message.error.error_stream != NULL) {
                Camera3Stream *stream =
                        Camera3Stream::cast(
                                  msg->message.error.error_stream);
                streamId = stream->getId();
            }
            ALOGV("Camera %d: %s: HAL error, frame %d, stream %d: %d",
                    mId, __FUNCTION__, msg->message.error.frame_number,
                    streamId, msg->message.error.error_code);

            CaptureResultExtras resultExtras;
            // Set request error status for the request in the in-flight tracking
            {
                Mutex::Autolock l(mInFlightLock);
                ssize_t idx = mInFlightMap.indexOfKey(msg->message.error.frame_number);
                if (idx >= 0) {
                    InFlightRequest &r = mInFlightMap.editValueAt(idx);
                    r.requestStatus = msg->message.error.error_code;
                    resultExtras = r.resultExtras;
                } else {
                    resultExtras.frameNumber = msg->message.error.frame_number;
                    ALOGE("Camera %d: %s: cannot find in-flight request on frame %" PRId64
                          " error", mId, __FUNCTION__, resultExtras.frameNumber);
                }
            }

            if (listener != NULL) {
                if (msg->message.error.error_code == CAMERA3_MSG_ERROR_DEVICE) {
                    listener->notifyError(ICameraDeviceCallbacks::ERROR_CAMERA_DEVICE,
                                          resultExtras);
                }
            } else {
                ALOGE("Camera %d: %s: no listener available", mId, __FUNCTION__);
            }
            break;
        }
        case CAMERA3_MSG_SHUTTER: {
            ssize_t idx;
            uint32_t frameNumber = msg->message.shutter.frame_number;
            nsecs_t timestamp = msg->message.shutter.timestamp;
            // Verify ordering of shutter notifications
            {
                Mutex::Autolock l(mOutputLock);
                // TODO: need to track errors for tighter bounds on expected frame number.
                if (frameNumber < mNextShutterFrameNumber) {
                    SET_ERR("Shutter notification out-of-order. Expected "
                            "notification for frame %d, got frame %d",
                            mNextShutterFrameNumber, frameNumber);
                    break;
                }
                mNextShutterFrameNumber = frameNumber + 1;
            }

            CaptureResultExtras resultExtras;

            // Set timestamp for the request in the in-flight tracking
            // and get the request ID to send upstream
            {
                Mutex::Autolock l(mInFlightLock);
                idx = mInFlightMap.indexOfKey(frameNumber);
                if (idx >= 0) {
                    InFlightRequest &r = mInFlightMap.editValueAt(idx);
                    r.captureTimestamp = timestamp;
                    resultExtras = r.resultExtras;
                }
            }
            if (idx < 0) {
                SET_ERR("Shutter notification for non-existent frame number %d",
                        frameNumber);
                break;
            }
            ALOGVV("Camera %d: %s: Shutter fired for frame %d (id %d) at %" PRId64,
                    mId, __FUNCTION__, frameNumber, resultExtras.requestId, timestamp);
            // Call listener, if any
            if (listener != NULL) {
                listener->notifyShutter(resultExtras, timestamp);
            }
            break;
        }
        default:
            SET_ERR("Unknown notify message from HAL: %d",
                    msg->type);
    }
}

CameraMetadata Camera3Device::getLatestRequestLocked() {
    ALOGV("%s", __FUNCTION__);

    CameraMetadata retVal;

    if (mRequestThread != NULL) {
        retVal = mRequestThread->getLatestRequest();
    }

    return retVal;
}


/**
 * RequestThread inner class methods
 */

Camera3Device::RequestThread::RequestThread(wp<Camera3Device> parent,
        sp<StatusTracker> statusTracker,
        camera3_device_t *hal3Device) :
        Thread(false),
        mParent(parent),
        mStatusTracker(statusTracker),
        mHal3Device(hal3Device),
        mId(getId(parent)),
        mReconfigured(false),
        mDoPause(false),
        mPaused(true),
        mFrameNumber(0),
        mLatestRequestId(NAME_NOT_FOUND),
        mCurrentAfTriggerId(0),
        mCurrentPreCaptureTriggerId(0),
        mRepeatingLastFrameNumber(NO_IN_FLIGHT_REPEATING_FRAMES) {
    mStatusId = statusTracker->addComponent();
}

void Camera3Device::RequestThread::configurationComplete() {
    Mutex::Autolock l(mRequestLock);
    mReconfigured = true;
}

status_t Camera3Device::RequestThread::queueRequestList(
        List<sp<CaptureRequest> > &requests,
        /*out*/
        int64_t *lastFrameNumber) {
    Mutex::Autolock l(mRequestLock);
    for (List<sp<CaptureRequest> >::iterator it = requests.begin(); it != requests.end();
            ++it) {
        mRequestQueue.push_back(*it);
    }

    if (lastFrameNumber != NULL) {
        *lastFrameNumber = mFrameNumber + mRequestQueue.size() - 1;
        ALOGV("%s: requestId %d, mFrameNumber %" PRId32 ", lastFrameNumber %" PRId64 ".",
              __FUNCTION__, (*(requests.begin()))->mResultExtras.requestId, mFrameNumber,
              *lastFrameNumber);
    }

    unpauseForNewRequests();

    return OK;
}


status_t Camera3Device::RequestThread::queueTrigger(
        RequestTrigger trigger[],
        size_t count) {

    Mutex::Autolock l(mTriggerMutex);
    status_t ret;

    for (size_t i = 0; i < count; ++i) {
        ret = queueTriggerLocked(trigger[i]);

        if (ret != OK) {
            return ret;
        }
    }

    return OK;
}

int Camera3Device::RequestThread::getId(const wp<Camera3Device> &device) {
    sp<Camera3Device> d = device.promote();
    if (d != NULL) return d->mId;
    return 0;
}

status_t Camera3Device::RequestThread::queueTriggerLocked(
        RequestTrigger trigger) {

    uint32_t tag = trigger.metadataTag;
    ssize_t index = mTriggerMap.indexOfKey(tag);

    switch (trigger.getTagType()) {
        case TYPE_BYTE:
        // fall-through
        case TYPE_INT32:
            break;
        default:
            ALOGE("%s: Type not supported: 0x%x", __FUNCTION__,
                    trigger.getTagType());
            return INVALID_OPERATION;
    }

    /**
     * Collect only the latest trigger, since we only have 1 field
     * in the request settings per trigger tag, and can't send more than 1
     * trigger per request.
     */
    if (index != NAME_NOT_FOUND) {
        mTriggerMap.editValueAt(index) = trigger;
    } else {
        mTriggerMap.add(tag, trigger);
    }

    return OK;
}

status_t Camera3Device::RequestThread::setRepeatingRequests(
        const RequestList &requests,
        /*out*/
        int64_t *lastFrameNumber) {
    Mutex::Autolock l(mRequestLock);
    if (lastFrameNumber != NULL) {
        *lastFrameNumber = mRepeatingLastFrameNumber;
    }
    mRepeatingRequests.clear();
    mRepeatingRequests.insert(mRepeatingRequests.begin(),
            requests.begin(), requests.end());

    unpauseForNewRequests();

    mRepeatingLastFrameNumber = NO_IN_FLIGHT_REPEATING_FRAMES;
    return OK;
}

bool Camera3Device::RequestThread::isRepeatingRequestLocked(const sp<CaptureRequest> requestIn) {
    if (mRepeatingRequests.empty()) {
        return false;
    }
    int32_t requestId = requestIn->mResultExtras.requestId;
    const RequestList &repeatRequests = mRepeatingRequests;
    // All repeating requests are guaranteed to have same id so only check first quest
    const sp<CaptureRequest> firstRequest = *repeatRequests.begin();
    return (firstRequest->mResultExtras.requestId == requestId);
}

status_t Camera3Device::RequestThread::clearRepeatingRequests(/*out*/int64_t *lastFrameNumber) {
    Mutex::Autolock l(mRequestLock);
    mRepeatingRequests.clear();
    if (lastFrameNumber != NULL) {
        *lastFrameNumber = mRepeatingLastFrameNumber;
    }
    mRepeatingLastFrameNumber = NO_IN_FLIGHT_REPEATING_FRAMES;
    return OK;
}

status_t Camera3Device::RequestThread::clear(/*out*/int64_t *lastFrameNumber) {
    Mutex::Autolock l(mRequestLock);
    ALOGV("RequestThread::%s:", __FUNCTION__);
    mRepeatingRequests.clear();

    // Decrement repeating frame count for those requests never sent to device
    // TODO: Remove this after we have proper error handling so these requests
    // will generate an error callback. This might be the only place calling
    // isRepeatingRequestLocked. If so, isRepeatingRequestLocked should also be removed.
    const RequestList &requests = mRequestQueue;
    for (RequestList::const_iterator it = requests.begin();
            it != requests.end(); ++it) {
        if (isRepeatingRequestLocked(*it)) {
            mRepeatingLastFrameNumber--;
        }
    }
    mRequestQueue.clear();
    mTriggerMap.clear();
    if (lastFrameNumber != NULL) {
        *lastFrameNumber = mRepeatingLastFrameNumber;
    }
    mRepeatingLastFrameNumber = NO_IN_FLIGHT_REPEATING_FRAMES;
    return OK;
}

void Camera3Device::RequestThread::setPaused(bool paused) {
    Mutex::Autolock l(mPauseLock);
    mDoPause = paused;
    mDoPauseSignal.signal();
}

status_t Camera3Device::RequestThread::waitUntilRequestProcessed(
        int32_t requestId, nsecs_t timeout) {
    Mutex::Autolock l(mLatestRequestMutex);
    status_t res;
    while (mLatestRequestId != requestId) {
        nsecs_t startTime = systemTime();

        res = mLatestRequestSignal.waitRelative(mLatestRequestMutex, timeout);
        if (res != OK) return res;

        timeout -= (systemTime() - startTime);
    }

    return OK;
}

void Camera3Device::RequestThread::requestExit() {
    // Call parent to set up shutdown
    Thread::requestExit();
    // The exit from any possible waits
    mDoPauseSignal.signal();
    mRequestSignal.signal();
}

bool Camera3Device::RequestThread::threadLoop() {

    status_t res;

    // Handle paused state.
    if (waitIfPaused()) {
        return true;
    }

    // Get work to do

    sp<CaptureRequest> nextRequest = waitForNextRequest();
    if (nextRequest == NULL) {
        return true;
    }

    // Create request to HAL
    camera3_capture_request_t request = camera3_capture_request_t();
    request.frame_number = nextRequest->mResultExtras.frameNumber;
    Vector<camera3_stream_buffer_t> outputBuffers;

    // Get the request ID, if any
    int requestId;
    camera_metadata_entry_t requestIdEntry =
            nextRequest->mSettings.find(ANDROID_REQUEST_ID);
    if (requestIdEntry.count > 0) {
        requestId = requestIdEntry.data.i32[0];
    } else {
        ALOGW("%s: Did not have android.request.id set in the request",
                __FUNCTION__);
        requestId = NAME_NOT_FOUND;
    }

    // Insert any queued triggers (before metadata is locked)
    int32_t triggerCount;
    res = insertTriggers(nextRequest);
    if (res < 0) {
        SET_ERR("RequestThread: Unable to insert triggers "
                "(capture request %d, HAL device: %s (%d)",
                request.frame_number, strerror(-res), res);
        cleanUpFailedRequest(request, nextRequest, outputBuffers);
        return false;
    }
    triggerCount = res;

    bool triggersMixedIn = (triggerCount > 0 || mPrevTriggers > 0);

    // If the request is the same as last, or we had triggers last time
    if (mPrevRequest != nextRequest || triggersMixedIn) {
        /**
         * HAL workaround:
         * Insert a dummy trigger ID if a trigger is set but no trigger ID is
         */
        res = addDummyTriggerIds(nextRequest);
        if (res != OK) {
            SET_ERR("RequestThread: Unable to insert dummy trigger IDs "
                    "(capture request %d, HAL device: %s (%d)",
                    request.frame_number, strerror(-res), res);
            cleanUpFailedRequest(request, nextRequest, outputBuffers);
            return false;
        }

        /**
         * The request should be presorted so accesses in HAL
         *   are O(logn). Sidenote, sorting a sorted metadata is nop.
         */
        nextRequest->mSettings.sort();
        request.settings = nextRequest->mSettings.getAndLock();
        mPrevRequest = nextRequest;
        ALOGVV("%s: Request settings are NEW", __FUNCTION__);

        IF_ALOGV() {
            camera_metadata_ro_entry_t e = camera_metadata_ro_entry_t();
            find_camera_metadata_ro_entry(
                    request.settings,
                    ANDROID_CONTROL_AF_TRIGGER,
                    &e
            );
            if (e.count > 0) {
                ALOGV("%s: Request (frame num %d) had AF trigger 0x%x",
                      __FUNCTION__,
                      request.frame_number,
                      e.data.u8[0]);
            }
        }
    } else {
        // leave request.settings NULL to indicate 'reuse latest given'
        ALOGVV("%s: Request settings are REUSED",
               __FUNCTION__);
    }

    camera3_stream_buffer_t inputBuffer;
    uint32_t totalNumBuffers = 0;

    // Fill in buffers

    if (nextRequest->mInputStream != NULL) {
        request.input_buffer = &inputBuffer;
        res = nextRequest->mInputStream->getInputBuffer(&inputBuffer);
        if (res != OK) {
            ALOGE("RequestThread: Can't get input buffer, skipping request:"
                    " %s (%d)", strerror(-res), res);
            cleanUpFailedRequest(request, nextRequest, outputBuffers);
            return true;
        }
        totalNumBuffers += 1;
    } else {
        request.input_buffer = NULL;
    }

    outputBuffers.insertAt(camera3_stream_buffer_t(), 0,
            nextRequest->mOutputStreams.size());
    request.output_buffers = outputBuffers.array();
    for (size_t i = 0; i < nextRequest->mOutputStreams.size(); i++) {
        res = nextRequest->mOutputStreams.editItemAt(i)->
                getBuffer(&outputBuffers.editItemAt(i));
        if (res != OK) {
            ALOGE("RequestThread: Can't get output buffer, skipping request:"
                    " %s (%d)", strerror(-res), res);
            cleanUpFailedRequest(request, nextRequest, outputBuffers);
            return true;
        }
        request.num_output_buffers++;
    }
    totalNumBuffers += request.num_output_buffers;

    // Log request in the in-flight queue
    sp<Camera3Device> parent = mParent.promote();
    if (parent == NULL) {
        CLOGE("RequestThread: Parent is gone");
        cleanUpFailedRequest(request, nextRequest, outputBuffers);
        return false;
    }

    res = parent->registerInFlight(request.frame_number,
            totalNumBuffers, nextRequest->mResultExtras,
            /*hasInput*/request.input_buffer != NULL);
    ALOGVV("%s: registered in flight requestId = %" PRId32 ", frameNumber = %" PRId64
           ", burstId = %" PRId32 ".",
            __FUNCTION__,
            nextRequest->mResultExtras.requestId, nextRequest->mResultExtras.frameNumber,
            nextRequest->mResultExtras.burstId);
    if (res != OK) {
        SET_ERR("RequestThread: Unable to register new in-flight request:"
                " %s (%d)", strerror(-res), res);
        cleanUpFailedRequest(request, nextRequest, outputBuffers);
        return false;
    }

    // Inform waitUntilRequestProcessed thread of a new request ID
    {
        Mutex::Autolock al(mLatestRequestMutex);

        mLatestRequestId = requestId;
        mLatestRequestSignal.signal();
    }

    // Submit request and block until ready for next one
    ATRACE_ASYNC_BEGIN("frame capture", request.frame_number);
    ATRACE_BEGIN("camera3->process_capture_request");
    res = mHal3Device->ops->process_capture_request(mHal3Device, &request);
    ATRACE_END();

    if (res != OK) {
        SET_ERR("RequestThread: Unable to submit capture request %d to HAL"
                " device: %s (%d)", request.frame_number, strerror(-res), res);
        cleanUpFailedRequest(request, nextRequest, outputBuffers);
        return false;
    }

    // Update the latest request sent to HAL
    if (request.settings != NULL) { // Don't update them if they were unchanged
        Mutex::Autolock al(mLatestRequestMutex);

        camera_metadata_t* cloned = clone_camera_metadata(request.settings);
        mLatestRequest.acquire(cloned);
    }

    if (request.settings != NULL) {
        nextRequest->mSettings.unlock(request.settings);
    }

    // Remove any previously queued triggers (after unlock)
    res = removeTriggers(mPrevRequest);
    if (res != OK) {
        SET_ERR("RequestThread: Unable to remove triggers "
              "(capture request %d, HAL device: %s (%d)",
              request.frame_number, strerror(-res), res);
        return false;
    }
    mPrevTriggers = triggerCount;

    return true;
}

CameraMetadata Camera3Device::RequestThread::getLatestRequest() const {
    Mutex::Autolock al(mLatestRequestMutex);

    ALOGV("RequestThread::%s", __FUNCTION__);

    return mLatestRequest;
}


void Camera3Device::RequestThread::cleanUpFailedRequest(
        camera3_capture_request_t &request,
        sp<CaptureRequest> &nextRequest,
        Vector<camera3_stream_buffer_t> &outputBuffers) {

    if (request.settings != NULL) {
        nextRequest->mSettings.unlock(request.settings);
    }
    if (request.input_buffer != NULL) {
        request.input_buffer->status = CAMERA3_BUFFER_STATUS_ERROR;
        nextRequest->mInputStream->returnInputBuffer(*(request.input_buffer));
    }
    for (size_t i = 0; i < request.num_output_buffers; i++) {
        outputBuffers.editItemAt(i).status = CAMERA3_BUFFER_STATUS_ERROR;
        nextRequest->mOutputStreams.editItemAt(i)->returnBuffer(
            outputBuffers[i], 0);
    }
}

sp<Camera3Device::CaptureRequest>
        Camera3Device::RequestThread::waitForNextRequest() {
    status_t res;
    sp<CaptureRequest> nextRequest;

    // Optimized a bit for the simple steady-state case (single repeating
    // request), to avoid putting that request in the queue temporarily.
    Mutex::Autolock l(mRequestLock);

    while (mRequestQueue.empty()) {
        if (!mRepeatingRequests.empty()) {
            // Always atomically enqueue all requests in a repeating request
            // list. Guarantees a complete in-sequence set of captures to
            // application.
            const RequestList &requests = mRepeatingRequests;
            RequestList::const_iterator firstRequest =
                    requests.begin();
            nextRequest = *firstRequest;
            mRequestQueue.insert(mRequestQueue.end(),
                    ++firstRequest,
                    requests.end());
            // No need to wait any longer

            mRepeatingLastFrameNumber = mFrameNumber + requests.size() - 1;

            break;
        }

        res = mRequestSignal.waitRelative(mRequestLock, kRequestTimeout);

        if ((mRequestQueue.empty() && mRepeatingRequests.empty()) ||
                exitPending()) {
            Mutex::Autolock pl(mPauseLock);
            if (mPaused == false) {
                ALOGV("%s: RequestThread: Going idle", __FUNCTION__);
                mPaused = true;
                // Let the tracker know
                sp<StatusTracker> statusTracker = mStatusTracker.promote();
                if (statusTracker != 0) {
                    statusTracker->markComponentIdle(mStatusId, Fence::NO_FENCE);
                }
            }
            // Stop waiting for now and let thread management happen
            return NULL;
        }
    }

    if (nextRequest == NULL) {
        // Don't have a repeating request already in hand, so queue
        // must have an entry now.
        RequestList::iterator firstRequest =
                mRequestQueue.begin();
        nextRequest = *firstRequest;
        mRequestQueue.erase(firstRequest);
    }

    // In case we've been unpaused by setPaused clearing mDoPause, need to
    // update internal pause state (capture/setRepeatingRequest unpause
    // directly).
    Mutex::Autolock pl(mPauseLock);
    if (mPaused) {
        ALOGV("%s: RequestThread: Unpaused", __FUNCTION__);
        sp<StatusTracker> statusTracker = mStatusTracker.promote();
        if (statusTracker != 0) {
            statusTracker->markComponentActive(mStatusId);
        }
    }
    mPaused = false;

    // Check if we've reconfigured since last time, and reset the preview
    // request if so. Can't use 'NULL request == repeat' across configure calls.
    if (mReconfigured) {
        mPrevRequest.clear();
        mReconfigured = false;
    }

    if (nextRequest != NULL) {
        nextRequest->mResultExtras.frameNumber = mFrameNumber++;
        nextRequest->mResultExtras.afTriggerId = mCurrentAfTriggerId;
        nextRequest->mResultExtras.precaptureTriggerId = mCurrentPreCaptureTriggerId;
    }
    return nextRequest;
}

bool Camera3Device::RequestThread::waitIfPaused() {
    status_t res;
    Mutex::Autolock l(mPauseLock);
    while (mDoPause) {
        if (mPaused == false) {
            mPaused = true;
            ALOGV("%s: RequestThread: Paused", __FUNCTION__);
            // Let the tracker know
            sp<StatusTracker> statusTracker = mStatusTracker.promote();
            if (statusTracker != 0) {
                statusTracker->markComponentIdle(mStatusId, Fence::NO_FENCE);
            }
        }

        res = mDoPauseSignal.waitRelative(mPauseLock, kRequestTimeout);
        if (res == TIMED_OUT || exitPending()) {
            return true;
        }
    }
    // We don't set mPaused to false here, because waitForNextRequest needs
    // to further manage the paused state in case of starvation.
    return false;
}

void Camera3Device::RequestThread::unpauseForNewRequests() {
    // With work to do, mark thread as unpaused.
    // If paused by request (setPaused), don't resume, to avoid
    // extra signaling/waiting overhead to waitUntilPaused
    mRequestSignal.signal();
    Mutex::Autolock p(mPauseLock);
    if (!mDoPause) {
        ALOGV("%s: RequestThread: Going active", __FUNCTION__);
        if (mPaused) {
            sp<StatusTracker> statusTracker = mStatusTracker.promote();
            if (statusTracker != 0) {
                statusTracker->markComponentActive(mStatusId);
            }
        }
        mPaused = false;
    }
}

void Camera3Device::RequestThread::setErrorState(const char *fmt, ...) {
    sp<Camera3Device> parent = mParent.promote();
    if (parent != NULL) {
        va_list args;
        va_start(args, fmt);

        parent->setErrorStateV(fmt, args);

        va_end(args);
    }
}

status_t Camera3Device::RequestThread::insertTriggers(
        const sp<CaptureRequest> &request) {

    Mutex::Autolock al(mTriggerMutex);

    sp<Camera3Device> parent = mParent.promote();
    if (parent == NULL) {
        CLOGE("RequestThread: Parent is gone");
        return DEAD_OBJECT;
    }

    CameraMetadata &metadata = request->mSettings;
    size_t count = mTriggerMap.size();

    for (size_t i = 0; i < count; ++i) {
        RequestTrigger trigger = mTriggerMap.valueAt(i);
        uint32_t tag = trigger.metadataTag;

        if (tag == ANDROID_CONTROL_AF_TRIGGER_ID || tag == ANDROID_CONTROL_AE_PRECAPTURE_ID) {
            bool isAeTrigger = (trigger.metadataTag == ANDROID_CONTROL_AE_PRECAPTURE_ID);
            uint32_t triggerId = static_cast<uint32_t>(trigger.entryValue);
            if (isAeTrigger) {
                request->mResultExtras.precaptureTriggerId = triggerId;
                mCurrentPreCaptureTriggerId = triggerId;
            } else {
                request->mResultExtras.afTriggerId = triggerId;
                mCurrentAfTriggerId = triggerId;
            }
            if (parent->mDeviceVersion >= CAMERA_DEVICE_API_VERSION_3_2) {
                continue; // Trigger ID tag is deprecated since device HAL 3.2
            }
        }

        camera_metadata_entry entry = metadata.find(tag);

        if (entry.count > 0) {
            /**
             * Already has an entry for this trigger in the request.
             * Rewrite it with our requested trigger value.
             */
            RequestTrigger oldTrigger = trigger;

            oldTrigger.entryValue = entry.data.u8[0];

            mTriggerReplacedMap.add(tag, oldTrigger);
        } else {
            /**
             * More typical, no trigger entry, so we just add it
             */
            mTriggerRemovedMap.add(tag, trigger);
        }

        status_t res;

        switch (trigger.getTagType()) {
            case TYPE_BYTE: {
                uint8_t entryValue = static_cast<uint8_t>(trigger.entryValue);
                res = metadata.update(tag,
                                      &entryValue,
                                      /*count*/1);
                break;
            }
            case TYPE_INT32:
                res = metadata.update(tag,
                                      &trigger.entryValue,
                                      /*count*/1);
                break;
            default:
                ALOGE("%s: Type not supported: 0x%x",
                      __FUNCTION__,
                      trigger.getTagType());
                return INVALID_OPERATION;
        }

        if (res != OK) {
            ALOGE("%s: Failed to update request metadata with trigger tag %s"
                  ", value %d", __FUNCTION__, trigger.getTagName(),
                  trigger.entryValue);
            return res;
        }

        ALOGV("%s: Mixed in trigger %s, value %d", __FUNCTION__,
              trigger.getTagName(),
              trigger.entryValue);
    }

    mTriggerMap.clear();

    return count;
}

status_t Camera3Device::RequestThread::removeTriggers(
        const sp<CaptureRequest> &request) {
    Mutex::Autolock al(mTriggerMutex);

    CameraMetadata &metadata = request->mSettings;

    /**
     * Replace all old entries with their old values.
     */
    for (size_t i = 0; i < mTriggerReplacedMap.size(); ++i) {
        RequestTrigger trigger = mTriggerReplacedMap.valueAt(i);

        status_t res;

        uint32_t tag = trigger.metadataTag;
        switch (trigger.getTagType()) {
            case TYPE_BYTE: {
                uint8_t entryValue = static_cast<uint8_t>(trigger.entryValue);
                res = metadata.update(tag,
                                      &entryValue,
                                      /*count*/1);
                break;
            }
            case TYPE_INT32:
                res = metadata.update(tag,
                                      &trigger.entryValue,
                                      /*count*/1);
                break;
            default:
                ALOGE("%s: Type not supported: 0x%x",
                      __FUNCTION__,
                      trigger.getTagType());
                return INVALID_OPERATION;
        }

        if (res != OK) {
            ALOGE("%s: Failed to restore request metadata with trigger tag %s"
                  ", trigger value %d", __FUNCTION__,
                  trigger.getTagName(), trigger.entryValue);
            return res;
        }
    }
    mTriggerReplacedMap.clear();

    /**
     * Remove all new entries.
     */
    for (size_t i = 0; i < mTriggerRemovedMap.size(); ++i) {
        RequestTrigger trigger = mTriggerRemovedMap.valueAt(i);
        status_t res = metadata.erase(trigger.metadataTag);

        if (res != OK) {
            ALOGE("%s: Failed to erase metadata with trigger tag %s"
                  ", trigger value %d", __FUNCTION__,
                  trigger.getTagName(), trigger.entryValue);
            return res;
        }
    }
    mTriggerRemovedMap.clear();

    return OK;
}

status_t Camera3Device::RequestThread::addDummyTriggerIds(
        const sp<CaptureRequest> &request) {
    // Trigger ID 0 has special meaning in the HAL2 spec, so avoid it here
    static const int32_t dummyTriggerId = 1;
    status_t res;

    CameraMetadata &metadata = request->mSettings;

    // If AF trigger is active, insert a dummy AF trigger ID if none already
    // exists
    camera_metadata_entry afTrigger = metadata.find(ANDROID_CONTROL_AF_TRIGGER);
    camera_metadata_entry afId = metadata.find(ANDROID_CONTROL_AF_TRIGGER_ID);
    if (afTrigger.count > 0 &&
            afTrigger.data.u8[0] != ANDROID_CONTROL_AF_TRIGGER_IDLE &&
            afId.count == 0) {
        res = metadata.update(ANDROID_CONTROL_AF_TRIGGER_ID, &dummyTriggerId, 1);
        if (res != OK) return res;
    }

    // If AE precapture trigger is active, insert a dummy precapture trigger ID
    // if none already exists
    camera_metadata_entry pcTrigger =
            metadata.find(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER);
    camera_metadata_entry pcId = metadata.find(ANDROID_CONTROL_AE_PRECAPTURE_ID);
    if (pcTrigger.count > 0 &&
            pcTrigger.data.u8[0] != ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE &&
            pcId.count == 0) {
        res = metadata.update(ANDROID_CONTROL_AE_PRECAPTURE_ID,
                &dummyTriggerId, 1);
        if (res != OK) return res;
    }

    return OK;
}


/**
 * Static callback forwarding methods from HAL to instance
 */

void Camera3Device::sProcessCaptureResult(const camera3_callback_ops *cb,
        const camera3_capture_result *result) {
    Camera3Device *d =
            const_cast<Camera3Device*>(static_cast<const Camera3Device*>(cb));
    d->processCaptureResult(result);
}

void Camera3Device::sNotify(const camera3_callback_ops *cb,
        const camera3_notify_msg *msg) {
    Camera3Device *d =
            const_cast<Camera3Device*>(static_cast<const Camera3Device*>(cb));
    d->notify(msg);
}

}; // namespace android
